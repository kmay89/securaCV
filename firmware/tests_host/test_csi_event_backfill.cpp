// Host tests for common/csi/src/csi_event_backfill.h — replaying committed
// csi_events from the SD event log once the MQTT broker returns (backlog
// F37). Whole outages are replayed against a model world: a card holding
// the log in the shared line format (csi_event_log_line.h), the canary's
// MQTT layer with its 12-slot offline queue (tamper alerts outrank events,
// mqtt_offline_queue.h), NVS, the event-id allocator with its floor
// (csi_event_id_floor.h), and Home Assistant's replay gate
// (custom_components/securacv/sensor.py `_replay_gate`: a verified events
// body whose event_id is below the last verified one is refused).
//
// The properties: the backfill never sends an id HA's gate would refuse,
// across reboots too; an outage longer than the offline queue still
// reaches HA whole and in id order; queued tamper alerts go first and a
// live tamper alert never waits on a backlog; every loop pass does bounded
// work.
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "csi_event_backfill.h"

using namespace csi_event_backfill;

static int g_checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                         \
    }                                                                   \
    ++g_checks;                                                         \
  } while (0)

// ── Home Assistant's replay gate, for one device ─────────────────────────
struct Ha {
  bool has_mark = false;
  uint32_t mark = 0;
  std::vector<uint32_t> accepted;      // event ids, in arrival order
  std::vector<bool> accepted_replay;   // the body's `replay` flag
  std::vector<uint32_t> refused;       // any origin
  std::vector<uint32_t> refused_backfill;
  void receive(uint32_t id, bool replay, bool backfill) {
    if (has_mark && id < mark) {
      refused.push_back(id);
      if (backfill) refused_backfill.push_back(id);
      return;
    }
    mark = id;
    has_mark = true;
    accepted.push_back(id);
    accepted_replay.push_back(replay);
  }
};

// One thing the broker saw, in order (events and tamper alerts).
struct Wire {
  bool tamper;
  uint32_t id;       // event id, or the tamper alert's sequence number
  bool backfill;
};

// ── The world the planner runs in ────────────────────────────────────────
struct World : Port {
  // Card: the log file, and the adapter behaviors csi_event_log.cpp has
  // (retention cut at a cap, a torn line sealed before the next append).
  bool card_in = true;
  std::string log;
  uint32_t cap_bytes = 0;          // 0 = no retention cut
  bool append_fails = false;
  size_t tear_next_append = 0;     // write only this many bytes of the next line
  bool needs_seal = false;
  int read_fail_budget = 0;        // fail the next N reads
  int reads = 0;                   // this pass
  // MQTT
  bool configured = true;
  bool connected = true;
  struct Queued { bool tamper; uint32_t id; bool deferred; };
  std::deque<Queued> offline;      // the 12-slot offline queue
  size_t offline_cap = 12;
  std::vector<Wire> wire;
  int sends = 0;                   // publishes this pass (live + backfill)
  uint32_t unbuildable_id = 0;
  // NVS
  uint32_t nvs_ceiling = 0;
  bool nvs_ok = true;
  int nvs_writes = 0;
  // HA
  Ha ha;

  AppendResult card_append(const char* line, size_t len) override {
    AppendResult r = {false, (uint32_t)log.size(), 0};
    if (!card_in || append_fails) return r;
    if (cap_bytes && log.size() >= cap_bytes) {
      size_t cut = log.size() / 4;
      while (cut < log.size() && log[cut] != '\n') ++cut;
      if (cut < log.size()) ++cut;
      log.erase(0, cut);
      r.cut = (uint32_t)cut;
    }
    if (needs_seal) {
      log.push_back('\n');
      needs_seal = false;
    }
    if (tear_next_append) {
      log.append(line, tear_next_append);
      tear_next_append = 0;
      needs_seal = true;
      r.size = (uint32_t)log.size();
      return r;
    }
    log.append(line, len);
    r.ok = true;
    r.size = (uint32_t)log.size();
    return r;
  }
  size_t card_read(uint32_t off, char* buf, size_t cap) override {
    ++reads;
    if (!card_in) return 0;
    if (read_fail_budget > 0) {
      --read_fail_budget;
      return 0;
    }
    if (off >= log.size()) return 0;
    const size_t n = std::min(cap, log.size() - off);
    std::memcpy(buf, log.data() + off, n);
    return n;
  }
  Sent publish(const csi_event_record_t& rec, bool replay, bool backfill) {
    if (rec.event_id == unbuildable_id) return Sent::kNever;
    if (!configured || !connected || !offline.empty()) return Sent::kNotNow;
    ++sends;
    wire.push_back({false, rec.event_id, backfill});
    ha.receive(rec.event_id, replay, backfill);
    return Sent::kYes;
  }
  Sent send_live(const csi_event_record_t& rec) override {
    return publish(rec, false, false);
  }
  Sent send_backfill(const csi_event_record_t& rec, bool fresh) override {
    return publish(rec, !fresh, true);
  }
  bool hand_to_queue(const csi_event_record_t& rec, bool deferred) override {
    return publish_or_queue(false, rec.event_id, deferred);
  }
  bool persist_ceiling(uint32_t c) override {
    if (!nvs_ok) return false;
    nvs_ceiling = c;
    ++nvs_writes;
    return true;
  }

  // The MQTT layer's publish_or_queue (securacv_mqtt.cpp) + its queue's
  // drop policy (mqtt_offline_queue.h): live when up and nothing queued,
  // else buffered; when full the oldest EVENT goes, a tamper alert never
  // gives way to an event.
  bool publish_or_queue(bool tamper, uint32_t id, bool deferred) {
    if (!configured) return false;
    if (connected && offline.empty()) {
      wire.push_back({tamper, id, false});
      if (!tamper) ha.receive(id, deferred, false);
      return true;
    }
    if (offline.size() == offline_cap) {
      auto ev = offline.begin();
      while (ev != offline.end() && ev->tamper) ++ev;
      if (ev == offline.end() && !tamper) return false;
      offline.erase(ev == offline.end() ? offline.begin() : ev);
    }
    offline.push_back({tamper, id, deferred});
    return true;
  }
  // mqtt_loop(): drain up to 4 queued records while the link is up.
  void mqtt_loop() {
    for (int b = 0; b < 4 && connected && !offline.empty(); ++b) {
      const Queued q = offline.front();
      offline.pop_front();
      wire.push_back({q.tamper, q.id, false});
      if (!q.tamper) ha.receive(q.id, q.deferred, false);
    }
  }
};

// The event-id allocator (csi_event.cpp) with the floor both trees persist.
struct Allocator {
  uint32_t nvs_floor = 0;
  uint32_t stored = 0;
  uint32_t next = 1;
  // A reboot: RAM starts over, csi_event_set_event_id_floor() restores.
  void boot() {
    stored = nvs_floor;
    next = 1;
    if (nvs_floor > next) next = nvs_floor;
  }
  uint32_t allocate() {
    const uint32_t id = next++;
    if (csi_event_id_floor::must_persist(stored, id)) {
      nvs_floor = csi_event_id_floor::floor_for(id);
      stored = nvs_floor;
    }
    return id;
  }
};

static csi_event_record_t row(uint32_t id, uint32_t ms) {
  csi_event_record_t r;
  std::memset(&r, 0, sizeof(r));
  r.event_id = id;
  r.first_seen_ms = ms;
  r.last_seen_ms = ms;
  r.category = CSI_CATEGORY_EVENT;
  r.privacy = CSI_PRIVACY_P1;
  r.bundled_count = 1;
  std::strcpy(r.module_id, "core.presence");
  std::strcpy(r.type_name, "presence_changed");
  std::strcpy(r.values.state_name, "active");
  r.values.motion_score = (uint8_t)(id % 100);
  return r;
}

// The host's pump, as csi_event_egress_pump() runs it on the loop task:
// mqtt_loop() first (the offline queue drains), then the rows, then one
// backfill pass. Returns what the pass sent; checks the per-pass bounds.
struct Host {
  World& w;
  Planner& p;
  Allocator& a;
  uint32_t now = 1000;
  uint32_t last_send_now = 0;
  bool sent_before = false;
  int bound_violations = 0;
  long total_reads = 0;

  Link link() const {
    return Link{w.configured, w.connected, a.stored, now};
  }
  size_t tick(int commits = 0, bool tamper_alert = false) {
    w.reads = 0;
    w.sends = 0;
    w.mqtt_loop();
    if (!w.configured) p.not_owed(link(), w);
    for (int i = 0; i < commits; ++i) {
      if (tamper_alert) (void)w.publish_or_queue(true, a.next, false);
      (void)p.commit(row(a.allocate(), now), link(), w);
    }
    const int live_sends = w.sends;
    const size_t sent = p.pass(link(), w);
    total_reads += w.reads;
    if (w.reads > (int)kChunksPerPass) ++bound_violations;
    if (w.sends - live_sends > (int)kSendsPerPass) ++bound_violations;
    if (sent > 0) {
      if (sent_before && now - last_send_now < kSendIntervalMs) ++bound_violations;
      sent_before = true;
      last_send_now = now;
    }
    now += 10;
    return sent;
  }
  // Run passes until the backlog is gone (or a pass limit).
  int drain(int max_passes = 100000) {
    int passes = 0;
    while ((p.pending() || !w.offline.empty()) && passes < max_passes) {
      tick();
      ++passes;
    }
    return passes;
  }
};

static void open_card(World& w, Planner& p) {
  const size_t tail = std::min(w.log.size(), kTailRead);
  const uint32_t id = last_line_id(w.log.data() + (w.log.size() - tail), tail);
  if (w.log.size() && w.log.back() != '\n') w.needs_seal = true;
  p.card_open((uint32_t)w.log.size(), id);
}

static bool strictly_rising(const std::vector<uint32_t>& ids) {
  for (size_t i = 1; i < ids.size(); ++i) {
    if (ids[i] <= ids[i - 1]) return false;
  }
  return true;
}

// ── Pure helpers ─────────────────────────────────────────────────────────

static int test_ceiling_for() {
  // Plain stride when no floor bounds it (or the id is from the bundler's
  // space, above the floor).
  CHECK(ceiling_for(50, 0) == 60);
  CHECK(ceiling_for(0x80000005u, 60) == 0x8000000Fu);
  // Capped at the allocator's floor when the id came from below it, so a
  // reboot's first ids (which start at the floor) stay above the watermark.
  CHECK(ceiling_for(55, 60) == 60);
  CHECK(ceiling_for(59, 60) == 60);
  CHECK(ceiling_for(50, 70) == 60);
  // Always above the id, saturating.
  CHECK(ceiling_for(0xFFFFFFFFu, 0) == 0xFFFFFFFFu);
  for (uint32_t id = 1; id < 200; ++id) {
    for (uint32_t floor = 0; floor < 220; floor += 7) {
      if (ceiling_for(id, floor) <= id) return 1;
    }
  }
  ++g_checks;
  return 0;
}

static int test_last_line_id() {
  char a[256], b[256];
  csi_event_record_t r1 = row(41, 1), r2 = row(42, 2);
  const size_t n1 = csi_event_log_line::marshal(&r1, a, sizeof(a));
  const size_t n2 = csi_event_log_line::marshal(&r2, b, sizeof(b));
  std::string log = std::string(a, n1) + std::string(b, n2);
  CHECK(last_line_id(log.data(), log.size()) == 42);
  // A torn last line does not count; the whole line before it does.
  std::string torn = log + "{\"id\":43,\"fir";
  CHECK(last_line_id(torn.data(), torn.size()) == 42);
  // A tail read that starts mid-line: that partial line is not a record.
  CHECK(last_line_id(log.data() + 5, log.size() - 5) == 42);
  CHECK(last_line_id(log.data() + 5, n1 - 5) == 0);
  // A garbage line (a sealed torn fragment) is walked past.
  std::string sealed = log + "{\"id\":43,\"fir\n";
  CHECK(last_line_id(sealed.data(), sealed.size()) == 42);
  CHECK(last_line_id("", 0) == 0);
  CHECK(last_line_id(nullptr, 10) == 0);
  return 0;
}

static int test_owner_verdict() {
  CHECK(owner_verdict(true, "0123456789abcdef\n", "0123456789abcdef") == Owner::kOurs);
  CHECK(owner_verdict(true, "0123456789abcdef", "0123456789abcdef") == Owner::kOurs);
  CHECK(owner_verdict(false, "0123456789abcdef\r\n", "0123456789abcdef") == Owner::kOurs);
  // Another device's log, or a log with no owner file (a canary-wap card,
  // a card from before this firmware): never used.
  CHECK(owner_verdict(true, "fedcba9876543210\n", "0123456789abcdef") == Owner::kForeign);
  CHECK(owner_verdict(true, "", "0123456789abcdef") == Owner::kForeign);
  CHECK(owner_verdict(true, "0123456789abcdef0\n", "0123456789abcdef") == Owner::kForeign);
  CHECK(owner_verdict(true, "0123456789abcde", "0123456789abcdef") == Owner::kForeign);
  // No log yet: claim it.
  CHECK(owner_verdict(false, "", "0123456789abcdef") == Owner::kClaim);
  CHECK(owner_verdict(false, "fedcba9876543210\n", "0123456789abcdef") == Owner::kClaim);
  // Nothing to bind to: never use a card.
  CHECK(owner_verdict(false, "", "") == Owner::kForeign);
  CHECK(owner_verdict(true, "x", nullptr) == Owner::kForeign);
  return 0;
}

// ── Whole-outage scenarios ───────────────────────────────────────────────

static int test_steady_state_is_live() {
  World w; Planner p; Allocator a;
  a.boot();
  p.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, p);
  Host h{w, p, a};
  for (int i = 0; i < 30; ++i) h.tick(1);
  CHECK(w.ha.accepted.size() == 30);
  CHECK(strictly_rising(w.ha.accepted));
  CHECK(w.ha.refused.empty());
  for (bool r : w.ha.accepted_replay) CHECK(!r);  // live rows are news
  CHECK(!p.pending());
  CHECK(p.stats().live == 30 && p.stats().replayed == 0);
  // Every row is on the card, and the scan cursor is caught up with it.
  CHECK(p.scan_offset() == w.log.size());
  // NVS: the first-boot record, then one write per stride, not per row.
  CHECK(w.nvs_writes <= 1 + 30 / (int)csi_event_id_floor::kStride + 1);
  CHECK(w.nvs_ceiling > p.watermark());
  CHECK(h.bound_violations == 0);
  return 0;
}

static int test_outage_longer_than_the_queue() {
  World w; Planner p; Allocator a;
  a.boot();
  p.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, p);
  Host h{w, p, a};
  for (int i = 0; i < 5; ++i) h.tick(1);           // live
  w.connected = false;                              // the broker goes away
  for (int i = 0; i < 40; ++i) h.tick(1);          // 40 rows: far past 12 slots
  for (int i = 0; i < 3; ++i) (void)w.publish_or_queue(true, 900 + i, false);  // tamper alerts
  CHECK(w.offline.size() == 3);                     // the rows waited on the card, not in the queue
  CHECK(p.pending());
  w.connected = true;
  const size_t first_backfill = w.wire.size();
  h.drain();
  // Queued tamper alerts reached the broker before any backfilled row.
  size_t tampers_seen = 0;
  for (size_t i = first_backfill; i < w.wire.size(); ++i) {
    if (w.wire[i].tamper) {
      ++tampers_seen;
    } else if (w.wire[i].backfill) {
      CHECK(tampers_seen == 3);
    }
  }
  CHECK(tampers_seen == 3);
  // Every committed row reached HA once, in id order, none refused; the
  // outage's rows say replay.
  CHECK(w.ha.accepted.size() == 45);
  CHECK(strictly_rising(w.ha.accepted));
  CHECK(w.ha.refused.empty());
  for (size_t i = 5; i < 45; ++i) CHECK(w.ha.accepted_replay[i]);
  CHECK(p.stats().replayed == 40);
  CHECK(!p.pending());
  CHECK(h.bound_violations == 0);
  return 0;
}

static int test_rows_during_the_backlog_wait_their_turn() {
  World w; Planner p; Allocator a;
  a.boot();
  p.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, p);
  Host h{w, p, a};
  w.connected = false;
  for (int i = 0; i < 20; ++i) h.tick(1);
  w.connected = true;
  // New rows commit while the backlog is still going out: they are held
  // behind it (sending one live would raise HA's mark past the backlog).
  for (int i = 0; i < 6; ++i) h.tick(1);
  CHECK(p.stats().live == 0);
  h.drain();
  CHECK(w.ha.accepted.size() == 26);
  CHECK(strictly_rising(w.ha.accepted));
  CHECK(w.ha.refused.empty());
  // The outage rows are replays; the rows committed while the link was up
  // are still news (HA device triggers fire on them).
  for (size_t i = 0; i < 20; ++i) CHECK(w.ha.accepted_replay[i]);
  for (size_t i = 20; i < 26; ++i) CHECK(!w.ha.accepted_replay[i]);
  // Once caught up, rows go live again.
  h.tick(1);
  CHECK(p.stats().live == 1);
  CHECK(h.bound_violations == 0);
  return 0;
}

static int test_live_tamper_alert_never_waits_on_the_backlog() {
  World w; Planner p; Allocator a;
  a.boot();
  p.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, p);
  Host h{w, p, a};
  w.connected = false;
  for (int i = 0; i < 30; ++i) h.tick(1);
  w.connected = true;
  h.tick();  // backfill under way
  CHECK(p.pending());
  // A tamper alert raised now goes straight out on the tamper topic, while
  // its events row waits its turn behind the backlog.
  const size_t before = w.wire.size();
  h.tick(1, /*tamper_alert=*/true);
  bool tamper_out = false;
  for (size_t i = before; i < w.wire.size(); ++i) tamper_out |= w.wire[i].tamper;
  CHECK(tamper_out);
  CHECK(p.pending());
  h.drain();
  CHECK(w.ha.refused.empty());
  CHECK(w.ha.accepted.size() == 31);
  return 0;
}

static int test_reboot_mid_outage_never_republishes() {
  World w; Allocator a;
  Planner p;
  a.boot();
  p.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, p);
  {
    Host h{w, p, a};
    for (int i = 0; i < 23; ++i) h.tick(1);   // live: ids 1..23
    w.connected = false;
    for (int i = 0; i < 30; ++i) h.tick(1);   // held: 24..53
  }
  // Power cut. RAM is gone; the card, NVS and HA's marks survive.
  const uint32_t delivered_before = w.ha.mark;
  Planner q;
  a.boot();
  q.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, q);
  // Nothing at or below what HA has is ever replayed: the ceiling is above
  // every id handed over.
  CHECK(q.watermark() >= delivered_before);
  Host h{w, q, a};
  w.connected = false;
  for (int i = 0; i < 5; ++i) h.tick(1);      // the new boot's rows, still offline
  w.connected = true;
  h.drain();
  CHECK(w.ha.refused.empty());
  CHECK(strictly_rising(w.ha.accepted));
  // At most kStride - 1 of the held rows fall under the ceiling; the rest
  // and every row of the new boot arrive.
  const size_t total = 23 + 30 + 5;
  CHECK(w.ha.accepted.size() + (csi_event_id_floor::kStride - 1) >= total);
  CHECK(w.ha.accepted.back() == a.next - 1);
  size_t new_boot = 0;
  for (uint32_t id : w.ha.accepted) new_boot += (id >= a.next - 5) ? 1 : 0;
  CHECK(new_boot == 5);
  return 0;
}

static int test_new_boot_ids_are_not_mistaken_for_delivered() {
  // The ceiling is capped at the allocator's floor, so a reboot whose
  // ceiling write came late in a stride does not swallow the next boot's
  // first ids (they start at that floor).
  World w; Allocator a;
  Planner p;
  a.boot();
  p.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, p);
  {
    Host h{w, p, a};
    // Ids that never reach the pump (a P2 row above the privacy ceiling, a
    // row the egress queue dropped) put hand-overs and floor writes out of
    // step: the floor was written at id 1 (-> 11), the first hand-over is
    // id 5, and a plain stride would write 15 — above the floor the next
    // boot starts at.
    for (int i = 0; i < 4; ++i) (void)a.allocate();
    for (int i = 0; i < 6; ++i) h.tick(1);   // ids 5..10, live
  }
  CHECK(a.nvs_floor == 11);
  CHECK(w.nvs_ceiling <= a.nvs_floor);
  Planner q;
  a.boot();
  q.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, q);
  CHECK(q.watermark() < a.next);  // the new boot's first id is not "delivered"
  Host h{w, q, a};
  w.connected = false;
  h.tick(1);
  w.connected = true;
  h.drain();
  CHECK(w.ha.accepted.back() == a.next - 1);
  CHECK(w.ha.refused.empty());
  return 0;
}

static int test_first_boot_of_this_firmware() {
  // Upgrading from a firmware that published live only: no ceiling in NVS,
  // an event-id floor from before. Everything below the floor counts as
  // delivered (it may have been), and the record starts at once, so rows
  // held this boot survive a reboot.
  World w; Allocator a;
  a.nvs_floor = 500;
  a.boot();
  Planner p;
  p.begin(0, a.stored, w);
  CHECK(p.watermark() == 499);
  CHECK(w.nvs_ceiling == 500);
  open_card(w, p);
  {
    Host h{w, p, a};
    w.connected = false;
    for (int i = 0; i < 4; ++i) h.tick(1);  // held: 500..503
  }
  Planner q;
  a.boot();
  q.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, q);
  Host h{w, q, a};
  w.connected = true;
  h.drain();
  CHECK(w.ha.accepted.size() == 4);
  CHECK(w.ha.accepted.front() == 500);
  return 0;
}

static int test_interleaved_id_spaces_are_never_refused() {
  // csi_bundler.cpp hands out ids from 0x80000000 in a space no floor
  // covers, and its rows commit between the chokepoint's. The backfill
  // walks forward and never sends at or below its watermark, so whatever
  // the mix, nothing it sends is refused.
  World w; Planner p; Allocator a;
  a.boot();
  p.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, p);
  Host h{w, p, a};
  w.connected = false;
  uint32_t bundle = 0x80000000u;
  for (int i = 0; i < 20; ++i) {
    const uint32_t id = (i % 3 == 1) ? bundle++ : a.allocate();
    (void)p.commit(row(id, h.now), h.link(), w);
    h.tick();
  }
  w.connected = true;
  h.drain();
  CHECK(w.ha.refused_backfill.empty());
  CHECK(strictly_rising(w.ha.accepted));
  CHECK(!p.pending());
  return 0;
}

static int test_retention_cut_moves_the_cursor() {
  World w; Planner p; Allocator a;
  w.cap_bytes = 4096;
  a.boot();
  p.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, p);
  Host h{w, p, a};
  for (int i = 0; i < 25; ++i) h.tick(1);  // ~ 4 KB live: the cut runs
  CHECK(w.log.size() < 4096 + 400);
  CHECK(p.scan_offset() == w.log.size());
  w.connected = false;
  for (int i = 0; i < 12; ++i) h.tick(1);  // held; a cut may drop old lines only
  w.connected = true;
  h.drain();
  CHECK(w.ha.refused.empty());
  CHECK(w.ha.accepted.size() == 37);
  CHECK(strictly_rising(w.ha.accepted));
  // A backlog bigger than retention: the cut drops the oldest waiting
  // rows (counted); what survives still goes out in order.
  w.connected = false;
  for (int i = 0; i < 60; ++i) h.tick(1);
  w.connected = true;
  h.drain();
  CHECK(p.stats().truncated_unsent > 0);
  CHECK(w.ha.refused.empty());
  CHECK(strictly_rising(w.ha.accepted));
  CHECK(w.ha.accepted.back() == a.next - 1);
  return 0;
}

static int test_no_broker_then_a_broker_is_not_flooded() {
  World w; Planner p; Allocator a;
  w.configured = false;
  a.boot();
  p.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, p);
  Host h{w, p, a};
  for (int i = 0; i < 30; ++i) h.tick(1);   // logged, owed to nobody
  CHECK(!w.log.empty());
  CHECK(!p.pending());
  // Configured later (and across a reboot): only new rows go out.
  Planner q;
  a.boot();
  q.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, q);
  w.configured = true;
  Host h2{w, q, a};
  for (int i = 0; i < 3; ++i) h2.tick(1);
  h2.drain();
  CHECK(w.ha.accepted.size() == 3);
  CHECK(q.stats().replayed == 0);
  return 0;
}

static int test_broker_change_drops_the_backlog() {
  World w; Planner p; Allocator a;
  a.boot();
  p.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, p);
  Host h{w, p, a};
  w.connected = false;
  for (int i = 0; i < 10; ++i) h.tick(1);
  CHECK(p.pending());
  p.not_owed(h.link(), w);   // mqtt_destination_epoch() moved
  CHECK(!p.pending());
  // A remount does not bring it back...
  p.card_close();
  open_card(w, p);
  CHECK(!p.pending());
  // ...and the dropped backlog stays dropped across a reboot too: the watermark
  // moved past it and was persisted before anything new went out.
  Planner q;
  a.boot();
  q.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, q);
  Host h2{w, q, a};
  w.connected = true;
  h2.drain();
  CHECK(q.stats().replayed == 0);
  h2.tick(1);
  CHECK(w.ha.accepted.size() == 1);   // only the row after the change
  CHECK(w.ha.refused.empty());
  return 0;
}

static int test_card_lost_while_rows_wait() {
  World w; Planner p; Allocator a;
  a.boot();
  p.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, p);
  Host h{w, p, a};
  w.connected = false;
  for (int i = 0; i < 8; ++i) h.tick(1);      // held on the card
  w.card_in = false;
  p.card_close();
  for (int i = 0; i < 4; ++i) h.tick(1);      // no card: the offline queue
  w.connected = true;
  h.drain();
  // The queued rows arrived; the held ones stayed on the card, and once the
  // queue raised HA's mark past them they are not sent (HA would refuse).
  CHECK(w.ha.accepted.size() == 4);
  w.card_in = true;
  open_card(w, p);
  h.drain();
  h.tick(1);
  CHECK(w.ha.refused.empty());
  CHECK(w.ha.accepted.size() == 5);
  return 0;
}

static int test_failed_reads_and_damage_do_not_stall_live_rows() {
  World w; Planner p; Allocator a;
  a.boot();
  p.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, p);
  Host h{w, p, a};
  for (int i = 0; i < 3; ++i) h.tick(1);    // live
  // A damaged run (a cluster of zeros, longer than one read) and a torn
  // append: that row is not on the card, so it goes the offline-queue way.
  w.log.append(std::string(1500, '\0'));
  w.log.push_back('\n');
  w.tear_next_append = 20;
  h.tick(1);
  w.connected = false;
  for (int i = 0; i < 5; ++i) h.tick(1);    // sealed first, then whole lines
  w.connected = true;
  h.drain();
  // The walk stepped over the damage and the sealed torn line.
  CHECK(w.ha.refused.empty());
  CHECK(strictly_rising(w.ha.accepted));
  CHECK(w.ha.accepted.size() == 9);
  CHECK(p.stats().replayed == 5);
  // Reads that keep failing: the walk gives up on this card, live resumes.
  w.connected = false;
  for (int i = 0; i < 3; ++i) h.tick(1);
  w.connected = true;
  w.read_fail_budget = 1000;
  for (int i = 0; i < 10; ++i) h.tick();
  CHECK(!p.pending());
  CHECK(p.stats().read_giveups == 1);
  const size_t before = w.ha.accepted.size();
  h.tick(1);
  CHECK(w.ha.accepted.size() == before + 1);  // live again
  CHECK(w.ha.refused.empty());
  return 0;
}

static int test_unbuildable_row_is_skipped_not_a_stall() {
  World w; Planner p; Allocator a;
  a.boot();
  p.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, p);
  Host h{w, p, a};
  w.connected = false;
  for (int i = 0; i < 6; ++i) h.tick(1);
  w.unbuildable_id = 3;
  w.connected = true;
  h.drain();
  CHECK(w.ha.accepted.size() == 5);
  CHECK(p.stats().unsendable == 1);
  CHECK(!p.pending());
  return 0;
}

static int test_nvs_failure_still_delivers() {
  World w; Planner p; Allocator a;
  w.nvs_ok = false;
  a.boot();
  p.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, p);
  Host h{w, p, a};
  for (int i = 0; i < 5; ++i) h.tick(1);
  w.connected = false;
  for (int i = 0; i < 5; ++i) h.tick(1);
  w.connected = true;
  h.drain();
  CHECK(w.ha.accepted.size() == 10);   // delivery never waits on flash
  CHECK(p.stored_ceiling() == 0);      // and each hand-over tried again
  return 0;
}

static int test_pass_bounds_under_a_big_backlog() {
  World w; Planner p; Allocator a;
  a.boot();
  p.begin(w.nvs_ceiling, a.stored, w);
  open_card(w, p);
  Host h{w, p, a};
  w.connected = false;
  for (int i = 0; i < 300; ++i) h.tick(1);
  w.connected = true;
  const int passes = h.drain();
  CHECK(h.bound_violations == 0);
  CHECK(w.ha.accepted.size() == 300);
  // Paced: kSendsPerPass per kSendIntervalMs at most (10 ms per test pass).
  CHECK(passes >= (int)(300 / kSendsPerPass) * (int)(kSendIntervalMs / 10) - 20);
  // And a pass waiting out the interval reads nothing: the card is read
  // about once per sending pass, not once per loop pass.
  CHECK(h.total_reads < passes / 4);
  return 0;
}

int main() {
  if (test_ceiling_for()) return 1;
  if (test_last_line_id()) return 1;
  if (test_owner_verdict()) return 1;
  if (test_steady_state_is_live()) return 1;
  if (test_outage_longer_than_the_queue()) return 1;
  if (test_rows_during_the_backlog_wait_their_turn()) return 1;
  if (test_live_tamper_alert_never_waits_on_the_backlog()) return 1;
  if (test_reboot_mid_outage_never_republishes()) return 1;
  if (test_new_boot_ids_are_not_mistaken_for_delivered()) return 1;
  if (test_first_boot_of_this_firmware()) return 1;
  if (test_interleaved_id_spaces_are_never_refused()) return 1;
  if (test_retention_cut_moves_the_cursor()) return 1;
  if (test_no_broker_then_a_broker_is_not_flooded()) return 1;
  if (test_broker_change_drops_the_backlog()) return 1;
  if (test_card_lost_while_rows_wait()) return 1;
  if (test_failed_reads_and_damage_do_not_stall_live_rows()) return 1;
  if (test_unbuildable_row_is_skipped_not_a_stall()) return 1;
  if (test_nvs_failure_still_delivers()) return 1;
  if (test_pass_bounds_under_a_big_backlog()) return 1;
  std::printf("test_csi_event_backfill: %d checks passed\n", g_checks);
  return 0;
}
