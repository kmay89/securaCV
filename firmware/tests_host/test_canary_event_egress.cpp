/* test_canary_event_egress.cpp — the canary's committed-event egress
 * (canary/src/csi_event_egress.cpp): what reaches Home Assistant, in what
 * order, across outages, card faults and reboots (backlog F37, F103, F104).
 *
 * Compiles the REAL canary/src/csi_event_egress.cpp, the REAL
 * canary/src/csi_event_log.cpp (the SD event log adapter) over a RAM card
 * (stubs/canary_egress/SD.h), and the REAL CSI chokepoint
 * (common/csi/src/csi_event.cpp, csi_module.cpp, csi_bundler.cpp), with the
 * real csi_event_backfill.h planner, csi_event_wire.h body builder and
 * mqtt_offline_queue.h. Rows are committed through csi_event_emit, so ids,
 * the floor's NVS writes and the commit hook are the firmware's.
 *
 * The model around it (this file):
 *   - securacv_mqtt.cpp's event surfaces: mqtt_publish_event() and
 *     mqtt_publish_tamper() run the real publish-or-queue order
 *     (mqtt_offline_queue::publish_or_queue, backlog F107: live when the link
 *     is up and the offline queue is empty, else into the queue, behind the
 *     records already there), mqtt_publish_event_live() refuses while the
 *     link is down or the queue holds records, mqtt_loop() drains up to four
 *     queued records a pass, mqtt_destination_epoch() moves when the broker
 *     changes. firmware/scripts/check_event_egress_order.py holds
 *     securacv_mqtt.cpp to the shared order, the live refusal and the epoch;
 *   - Home Assistant's replay gate (custom_components/securacv/sensor.py
 *     `_replay_gate`): an events body whose event_id is below the last one it
 *     verified is refused. Bodies are parsed for event_id and replay;
 *   - the storage manager (securacv_storage.cpp): a card mounted or not, a
 *     remount as a new mount generation, two failed writes in a row marking
 *     the card lost;
 *   - main.cpp's loop: mqtt_loop(), then csi_event_egress_pump(); a reboot
 *     is a RAM reset (the allocator, the egress, the offline queue) that
 *     keeps NVS, the card and Home Assistant.
 *
 * Every hand-over (a live send, a backfill send, a buffered record) is
 * checked against the NVS delivery ceiling: it must already be above the id
 * (F47), so a reboot never republishes one. main() fails on any violation.
 *
 * F103, failing on the egress before its fix (prove it by building this
 * file against the canary/src/csi_event_egress.cpp it replaced): a row
 * whose card append fails during an outage, then a reboot: every card row
 * still arrives; without a reboot the failed row waits behind them (during
 * an outage, mid-backfill, behind a held row) instead of overtaking them;
 * a failed append that opens an outage, or one behind a held row with the
 * link down, writes no ceiling over the card rows after it.
 *
 * F104, failing on the egress before its fix (the same proof): a card
 * closed mid-backlog, a row committed while it is out, the card back:
 * nothing is skipped; a card that mounts late: a row committed first waits
 * for its rows; the wait is bounded (csi_event_backfill::kCardWaitMs); a
 * held row writes no ceiling over the card rows after it; ambient rows are
 * never held; a broker change before the card opens drops what it holds.
 *
 * F103's review added a line that lands without its newline: its append
 * fails, so the row waits in the hold, and the next append seals the line,
 * which the walk then sends. It reaches HA once, not from the card and again
 * from the hold, by either path out of the hold (failing on the egress
 * before that fix: the same proof, with <rev> the parent of "fix(csi): a
 * held row the walk already sent from the card is not sent again"). And the
 * card wait's timing: a close long after boot is timed from the close, a
 * 30 s remount is inside the wait, and the wait ends exactly kCardWaitMs
 * after it began.
 *
 * The rest pin the hold's own rules: a close with nothing owed does not
 * wait; a broker change drops the hold and ends the wait, and one made with
 * the card open is not applied again at a remount; held rows keep their
 * replay flag; a held row's failed publish is retried, ahead of the card
 * row; a refused flush keeps its row; past kHeldMax the oldest goes; a row
 * routed in the pass the wait ends, or whose append fails in the pass the
 * card opens, waits behind the hold. Each fails on at least one mutation of
 * the fix that nothing else here catches. Two mutations change nothing a
 * host can see and have no scenario: a failed append that takes the hold
 * when nothing waits (the same pass's flush hands it over, ceiling first),
 * and the flush run before the backfill pass instead of after it (a held
 * row goes one pass later, in the same order).
 *
 * F107: a row routed, and a held row flushed, in the pass the offline queue
 * still drains an outage join the queue's back instead of going live past
 * it (each fails with the branch removed from, or moved after the live send
 * in, mqtt_offline_queue.h's publish_or_queue()). F109: the egress's
 * counters (csi_event_egress_stats(), the health publish's
 * csi_event_egress): live, held, replayed and queued rows, a full egress
 * queue, the hold's overflow and the ambient rows it refuses, all from
 * boot. F82: csi_event_egress_id_space_low() is up once the real
 * allocator reaches kHoldLimit, after a reboot past it, and after a wrap.
 * Each fails with its line of the fix reverted.
 *
 * The proofs build this file, with this directory's Makefile and stubs,
 * against an older egress source: for F104 the one from the commit that
 * added this test ("test(canary): host-test the real canary event egress"),
 * for F103 the one from the F104 fix, for the review's line without its
 * newline the one named above. From the repo root, with <rev> that commit:
 *   d=$(mktemp -d); git archive HEAD firmware | tar -x -C "$d"
 *   git show <rev>:firmware/canary/src/csi_event_egress.cpp \
 *     > "$d/firmware/canary/src/csi_event_egress.cpp"
 *   make -C "$d/firmware/tests_host" build/test_canary_event_egress
 *   "$d/firmware/tests_host/build/test_canary_event_egress"
 *
 * What it does not pin: the real SD driver and storage manager, PubSubClient,
 * FreeRTOS scheduling (the commit hook runs on the committing task in the
 * firmware; here everything is one thread), and main.cpp itself
 * (check_event_egress_order.py and check_csi_bundle_tick.py hold its calls).
 * Bench territory.
 *
 * Build/run: make -C firmware/tests_host (the CI "host tests" job). */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Arduino.h"
#include "Preferences.h"
#include "SD.h"
#include "csi_event.h"
#include "csi_event_backfill.h"
#include "csi_event_egress.h"
#include "csi_event_id_floor.h"
#include "csi_module.h"
#include "identity/device_signature.h"
#include "mqtt/mqtt_offline_queue.h"
#include "securacv_mqtt.h"
#include "securacv_storage.h"
#include "securacv_witness.h"

static int g_fail = 0;
static int g_checks = 0;
static int g_ceiling_violations_total = 0;
#define CHECK(cond, msg) do { \
  ++g_checks; \
  if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); g_fail++; } \
  else { std::printf("ok   %s\n", msg); } } while (0)

/* ── Home Assistant's replay gate, for this device ─────────────────────── */
struct Ha {
  bool has_mark = false;
  uint32_t mark = 0;
  std::vector<uint32_t> accepted;
  std::vector<bool> replay;   /* the body's `replay`, per accepted row */
  std::vector<uint32_t> refused;
  void receive(uint32_t id, bool is_replay) {
    if (has_mark && id < mark) { refused.push_back(id); return; }
    mark = id;
    has_mark = true;
    accepted.push_back(id);
    replay.push_back(is_replay);
  }
};

/* ── The world: broker, storage manager, what went out ─────────────────── */
struct World {
  bool accepting = true;        /* a broker is configured */
  bool connected = false;       /* the link is up */
  uint32_t epoch = 0;           /* mqtt_destination_epoch() */
  int fail_next_sends = 0;      /* the link is up but the next N publishes fail */
  bool mounted = true;          /* storage_is_mounted() */
  bool mount_in_flight = false;
  uint32_t write_errors = 0;    /* consecutive; two mark the card lost */
  int tampers = 0;              /* tamper-topic records that reached the broker */
  int log_health_calls = 0;
  Ha ha;
  std::vector<uint32_t> wire;   /* every events id the broker took, in order */
};
static World W;
/* storage_mount_generation(): monotonic over the whole run, since the
 * adapter's RAM (the generation it last evaluated) is not reset between
 * scenarios; a reboot or a remount is a new one. */
static uint32_t g_generation = 1;

/* securacv_mqtt.cpp's offline queue: MQTT_OFFLINE_SLOTS x
 * MQTT_OFFLINE_SLOT_BYTES (12 x 512), the real policy. */
static constexpr size_t kOfflineSlots = 12;
static constexpr size_t kOfflineSlotBytes = 512;
static uint8_t g_offline_storage[kOfflineSlots *
                                 mqtt_offline_queue::slot_stride(kOfflineSlotBytes)];
static mqtt_offline_queue::Queue g_offline;

static uint32_t nvs_get(const char* key) {
  Preferences p;
  if (!p.begin("securacv", true)) return 0;
  const uint32_t v = p.getULong(key, 0);
  p.end();
  return v;
}
static uint32_t nvs_ceiling() { return nvs_get("csi.evsent"); }

static uint32_t body_id(const char* body) {
  const char* p = std::strstr(body, "\"event_id\":");
  return p ? (uint32_t)std::strtoul(p + 11, nullptr, 10) : 0;
}
static bool body_replay(const char* body) {
  return std::strstr(body, "\"replay\":true") != nullptr;
}

/* An events id leaves the device (sent, or buffered for later): NVS must
 * already hold a ceiling above it, unless NVS refuses writes. */
static void handed_over(uint32_t id) {
  if (!host_nvs().fail_puts && nvs_ceiling() <= id) {
    g_ceiling_violations_total++;
    std::printf("INVARIANT: id %u handed over with the NVS ceiling at %u\n", id, nvs_ceiling());
  }
}

static bool link_send(mqtt_offline_queue::Kind kind, const char* payload) {
  if (!W.connected) return false;
  if (W.fail_next_sends > 0) {
    W.fail_next_sends--;
    return false;
  }
  if (kind == mqtt_offline_queue::KIND_TAMPER) {
    W.tampers++;
    return true;
  }
  const uint32_t id = body_id(payload);
  handed_over(id);
  W.wire.push_back(id);
  W.ha.receive(id, body_replay(payload));
  return true;
}

/* securacv_mqtt.cpp's publish_or_queue(): the real order
 * (mqtt_offline_queue::publish_or_queue, backlog F107) over the real queue,
 * with this file's link. A record the queue took is handed over too: its
 * events id must already be under the NVS ceiling (nothing writes NVS
 * between the push and this check). */
static bool publish_or_queue(mqtt_offline_queue::Kind kind, const char* payload, bool retained) {
  const uint32_t queued_before = g_offline.stats().queued;
  const bool ok = mqtt_offline_queue::publish_or_queue(
      g_offline, W.connected, kind, retained, payload,
      [&]() { return link_send(kind, payload); }, []() {});
  if (kind == mqtt_offline_queue::KIND_EVENT && g_offline.stats().queued != queued_before) {
    handed_over(body_id(payload));
  }
  return ok;
}

bool mqtt_accepting() { return W.accepting; }
bool mqtt_connected() { return W.accepting && W.connected; }
uint32_t mqtt_destination_epoch() { return W.epoch; }
bool mqtt_publish_event(const char* json_payload) {
  if (!W.accepting) return false;
  return publish_or_queue(mqtt_offline_queue::KIND_EVENT, json_payload, false);
}
bool mqtt_publish_event_live(const char* json_payload) {
  if (json_payload == nullptr || !W.accepting) return false;
  if (!W.connected || !g_offline.empty()) return false;
  return link_send(mqtt_offline_queue::KIND_EVENT, json_payload);
}
bool mqtt_publish_tamper(const char* json_payload, bool retained) {
  if (!W.accepting) return false;
  return publish_or_queue(mqtt_offline_queue::KIND_TAMPER, json_payload, retained);
}
/* securacv_mqtt.cpp's offline_queue_drain(), from mqtt_loop(). */
void mqtt_loop() {
  mqtt_offline_queue::Kind kind;
  bool retained;
  const char* payload;
  for (int budget = 4; budget > 0 && W.connected && g_offline.front(&kind, &retained, &payload);
       --budget) {
    if (!link_send(kind, payload)) return;
    g_offline.pop_front();
  }
}

/* ── The storage manager (securacv_storage.cpp) ────────────────────────── */
bool storage_is_mounted() { return W.mounted; }
bool storage_mount_in_flight() { return W.mount_in_flight; }
uint32_t storage_mount_generation() { return g_generation; }
void storage_note_write_failure() {
  if (++W.write_errors >= 2) W.mounted = false;   /* kConsecutiveErrorLimit: marked lost */
}
void storage_note_write_success() { W.write_errors = 0; }

/* ── Identity and health (securacv_witness, device_signature) ──────────── */
DeviceIdentity& witness_get_device() {
  static DeviceIdentity d = [] {
    DeviceIdentity x;
    std::memset(&x, 0, sizeof(x));
    for (int i = 0; i < 8; ++i) x.pubkey_fp[i] = (uint8_t)(0x10 + i);
    std::strcpy(x.device_id, "canary-test");
    return x;
  }();
  return d;
}
void log_health(LogLevel, LogCategory, const char*, const char*) { W.log_health_calls++; }

namespace device_signature {
void init(const uint8_t*, const uint8_t*, const char*, const char*) {}
bool sign_event(uint32_t, const char*, const char*, const char*, int, int, int,
                char* sig_b64url_out, size_t sig_cap) {
  if (sig_cap < SIG_B64URL_CAP) return false;
  std::memset(sig_b64url_out, 'A', SIG_B64URL_LEN);
  sig_b64url_out[SIG_B64URL_LEN] = '\0';
  return true;
}
const char* fingerprint_hex() { return "1011121314151617"; }
}  // namespace device_signature

/* ── Rows ──────────────────────────────────────────────────────────────── */
static void noop_init(const csi_module_settings_t*) {}
static void noop_tick(const csi_features_t*) {}
static const csi_event_decl_t EVENTS[] = {
  /* stateless: commits at once */
  { "ping", CSI_FIELD_NOTE | CSI_FIELD_TIME_BUCKET, CSI_PRIVACY_P0, 0 },
};
static const csi_module_t MODULE = {
  "test.egress", CSI_PRIVACY_P0, EVENTS, sizeof(EVENTS) / sizeof(EVENTS[0]),
  noop_init, noop_tick, nullptr, nullptr,
};
/* An ambient module, as wifi.channel_activity (the canary registers it):
 * CSI_CATEGORY_AMBIENT rows bypass the bundler and commit at once. */
static const csi_event_decl_t AMBIENT_EVENTS[] = {
  { "channel_active", CSI_FIELD_STATE_NAME | CSI_FIELD_TIME_BUCKET | CSI_FIELD_MOTION_SCORE,
    CSI_PRIVACY_P0, 0 },
};
static const csi_module_t AMBIENT_MODULE = {
  "test.ambient", CSI_PRIVACY_P0, AMBIENT_EVENTS, 1, noop_init, noop_tick, nullptr, nullptr,
};

static uint32_t emit_ping() {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_NOTE | CSI_FIELD_TIME_BUCKET;
  std::strcpy(v.note, "p");
  return csi_event_emit("test.egress", "ping", &v);
}

[[maybe_unused]] static uint32_t emit_ambient() {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_AMBIENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_TIME_BUCKET | CSI_FIELD_MOTION_SCORE;
  std::strcpy(v.state_name, "channel_active");
  v.motion_score = 40;
  return csi_event_emit("test.ambient", "channel_active", &v);
}

/* ── Boot, card, link, loop ────────────────────────────────────────────── */

/* A power cycle: RAM starts over (the allocator, the egress, the MQTT
 * layer's offline queue); NVS, the card and Home Assistant stay. A card that
 * is in mounts afresh (a new generation, as the adapter sees it). */
static void boot() {
  csi_event_test_reset();
  csi_event_egress_test_reset();
  g_offline.init(g_offline_storage, sizeof(g_offline_storage), kOfflineSlotBytes);
  W.connected = false;
  W.write_errors = 0;
  g_generation++;
  csi_event_egress_begin();
}

/* A fresh device: empty NVS, a card with nothing on it (or none), no link. */
static void fresh_device(bool card = true) {
  host_nvs().u32.clear();
  host_nvs().fail_puts = false;
  SD.files.clear();
  SD.dirs.clear();
  SD.present = card;
  SD.fail_writes = false;
  SD.short_write_next = 0;
  SD.short_by_next = 0;
  SD.fail_renames = false;
  W = World();
  W.mounted = card;
  boot();
}

static void connect() { W.connected = true; }

/* The card leaves (a pull, an SD error's lost mark, a remount in flight). */
static void card_out() { W.mounted = false; }
/* The storage manager's periodic check mounts it again: a new generation. */
static void card_back() {
  SD.present = true;
  W.mounted = true;
  W.write_errors = 0;
  g_generation++;
}

/* One main-loop pass: mqtt_loop(), then the egress pump (main.cpp's order). */
static constexpr uint32_t kPassMs = 50;
static void loop_pass() {
  stub_millis() += kPassMs;
  mqtt_loop();
  csi_event_egress_pump();
}

/* Enough passes to outlast a card wait (45 s) and drain any backlog. */
static constexpr int kDrainPasses = (int)(46000 / kPassMs) + 200;
static void drain(int passes = kDrainPasses) {
  for (int i = 0; i < passes; ++i) loop_pass();
}

static bool exactly(const std::vector<uint32_t>& got, const std::vector<uint32_t>& want) {
  return got == want;
}
[[maybe_unused]] static bool has(const std::vector<uint32_t>& v, uint32_t id) {
  return std::find(v.begin(), v.end(), id) != v.end();
}
[[maybe_unused]] static bool rising(const std::vector<uint32_t>& ids) {
  for (size_t i = 1; i < ids.size(); ++i) if (ids[i] <= ids[i - 1]) return false;
  return true;
}

/* ── F37: the egress's rules on the real source ────────────────────────── */

static void test_steady_state_is_live() {
  std::printf("-- card in, link up: every row goes live, once, in order\n");
  fresh_device();
  connect();
  loop_pass();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 6; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(), "six rows, live, in order");
  bool any_replay = false;
  for (bool r : W.ha.replay) any_replay |= r;
  CHECK(!any_replay, "live rows are news, not replays");
  CHECK(SD.files["/EVENTS/today.ndjson"].size() > 0, "and each is on the card");
}

static void test_outage_with_a_card_backfills_in_order() {
  std::printf("-- an outage with a card: the rows wait on it and arrive once the broker is back\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 20; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  CHECK(W.ha.accepted.empty() && g_offline.empty(), "nothing went out, and nothing was queued");
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(), "all twenty, once, in id order");
  bool all_replay = !W.ha.replay.empty();
  for (bool r : W.ha.replay) all_replay &= r;
  CHECK(all_replay, "each one marked as a replay");
}

static void test_reboot_in_an_outage_keeps_the_backlog_owed() {
  std::printf("-- a reboot during an outage: the card's rows are still owed\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 5; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  boot();
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids), "all five arrive after the reboot");
}

static void test_no_card_outage_uses_the_offline_queue() {
  std::printf("-- no card, an outage: rows ride the MQTT layer's offline queue (F29)\n");
  fresh_device(/*card=*/false);
  drain();   /* past any wait for a card that is not there */
  std::vector<uint32_t> ids;
  for (int i = 0; i < 5; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  CHECK(g_offline.size() == 5, "five rows queued");
  connect();
  drain(20);
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(), "and they arrive, in order");
}

static uint32_t commit_with_a_failed_append();

/* ── F107: a row handed over off the card never overtakes the queue ───── */

/* The offline queue drains four records a pass (mqtt_loop()), so after an
 * outage longer than that, rows from the outage are still queued when the
 * pump hands the next row over. It must join the back of the queue
 * (mqtt_offline_queue::publish_or_queue): sent live, it would raise Home
 * Assistant's mark past the queued rows, and the replay gate would refuse
 * every one of them. */
static void test_row_after_an_outage_waits_behind_the_queue() {
  std::printf("-- F107: no card, an outage longer than one drain: a row committed as the link returns waits its turn\n");
  fresh_device(/*card=*/false);
  drain();   /* past the wait for a card that is not there */
  std::vector<uint32_t> ids;
  for (int i = 0; i < 6; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  CHECK(g_offline.size() == 6, "six rows queued in the outage");
  connect();
  ids.push_back(emit_ping());   /* routed in the pass the queue drains four */
  loop_pass();
  CHECK(g_offline.size() == 3, "the new row joined the back of the queue, behind the two left");
  drain(20);
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "all seven arrive, in id order, none refused");
}

/* The RAM hold's flush hands its rows to the same order (backlog F104). A
 * row whose card append failed while the link was down waits in the hold;
 * when the link returns, the hold flushes in the pass the queue still drains
 * the outage's older rows. */
static void test_held_row_flush_waits_behind_the_queue() {
  std::printf("-- F107: a held row flushed as the link returns waits behind the outage's queued rows\n");
  fresh_device(/*card=*/false);
  drain();   /* past the wait for a card that is not there */
  std::vector<uint32_t> ids;
  for (int i = 0; i < 6; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  CHECK(g_offline.size() == 6, "six rows queued while no card was in");
  card_back();               /* a card goes in: nothing on it is owed */
  loop_pass();
  ids.push_back(commit_with_a_failed_append());   /* the link is down: it waits in the hold */
  CHECK(g_offline.size() == 6 && W.ha.accepted.empty(), "the failed row was neither queued nor sent");
  connect();
  loop_pass();               /* four queued rows go; the hold flushes */
  CHECK(g_offline.size() == 3, "the held row joined the back of the queue, behind the two left");
  drain(20);
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "the six queued rows, then the held row, none refused");
}

/* ── F103: a failed card append never overtakes, nor covers, the card ──── */

/* The append fails once (one write error: the storage manager keeps the card
 * mounted), then the card takes writes again. */
static uint32_t commit_with_a_failed_append() {
  SD.fail_writes = true;
  const uint32_t id = emit_ping();
  loop_pass();
  SD.fail_writes = false;
  return id;
}

static void test_failed_append_in_an_outage_then_a_reboot() {
  std::printf("-- F103: a row whose append fails during an outage, then a reboot: every card row arrives\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 5; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  (void)commit_with_a_failed_append();   /* not on the card; rows wait there */
  CHECK(W.mounted, "one failed write: the card stays mounted");
  for (int i = 0; i < 2; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  boot();                                /* power cycle before the broker returns */
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "the failed row moved no NVS ceiling past the card: all seven card rows arrive");
}

static void test_failed_append_in_an_outage_waits_its_turn() {
  std::printf("-- F103: a row whose append fails during an outage waits behind the card's rows\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 5; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  ids.push_back(commit_with_a_failed_append());
  for (int i = 0; i < 2; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "the five card rows, then the failed row, then the two after it, each once");
}

static void test_failed_append_mid_backfill_waits_its_turn() {
  std::printf("-- F103: a row whose append fails while the backfill runs waits behind it\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 10; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  connect();
  loop_pass();                           /* the walk sends its first rows */
  CHECK(!W.ha.accepted.empty() && W.ha.accepted.size() < 10, "the backfill is under way");
  ids.push_back(commit_with_a_failed_append());
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "every card row, then the failed row, each once");
}

static void test_failed_append_that_opens_an_outage_covers_nothing() {
  std::printf("-- F103: a failed append that opens an outage, card rows after it, a reboot: they arrive\n");
  fresh_device();
  connect();
  loop_pass();
  W.connected = false;                   /* the broker goes away */
  (void)commit_with_a_failed_append();   /* nothing waits yet: it is the first */
  std::vector<uint32_t> ids;
  for (int i = 0; i < 3; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  boot();
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "its ceiling (a stride past it) did not cover the three card rows after it");
}

static void test_failed_append_with_nothing_waiting_goes_live() {
  std::printf("-- F103: a failed append with the link up and nothing waiting goes live at once\n");
  fresh_device();
  connect();
  loop_pass();
  const uint32_t a = emit_ping(); loop_pass();
  const uint32_t f = commit_with_a_failed_append();
  CHECK(exactly(W.ha.accepted, {a, f}), "it goes live in its own pass");
  const uint32_t b = emit_ping(); loop_pass();
  CHECK(exactly(W.ha.accepted, {a, f, b}) && W.ha.refused.empty(), "and the next row after it");
}

static void test_failed_append_waits_behind_a_held_row() {
  std::printf("-- F103: a row whose append fails does not overtake a row waiting in the hold\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 3; ++i) { ids.push_back(emit_ping()); loop_pass(); }   /* an outage */
  W.mounted = false;                          /* power cycle; the boot mount is still running */
  boot();
  connect();
  loop_pass();
  ids.push_back(emit_ping()); loop_pass();    /* held for the card */
  card_back();                                /* the log opens with its three rows owed */
  loop_pass();
  ids.push_back(commit_with_a_failed_append());
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "the card's rows, the held row, then the failed-append row, each once");
}

static void test_failed_append_behind_a_held_row_covers_nothing() {
  std::printf("-- F103: a failed append behind a held row, the link down, card rows after, a reboot\n");
  fresh_device();
  W.mounted = false;
  boot();
  loop_pass();
  (void)emit_ping(); loop_pass();             /* held for the card */
  card_back();                                /* an empty log; the link is down: still held */
  loop_pass();
  (void)commit_with_a_failed_append();        /* waits behind it */
  std::vector<uint32_t> ids;
  for (int i = 0; i < 3; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  CHECK(g_offline.empty(), "neither waiting row was handed to the offline queue");
  boot();                                     /* power cycle: the two RAM rows are gone */
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "no ceiling was written for them: the three card rows after them arrive");
}

/* An append that lands every byte of its line but the newline reports a
 * failure (csi_event_log::append: a short write), so the row waits in the
 * hold; the next append writes the newline first, and the line it left is
 * then a whole one the walk sends. The hold's copy must not go as well: HA's
 * replay gate passes an equal id, so its triggers would fire twice. */
static void test_torn_but_whole_line_is_sent_once() {
  std::printf("-- F103: an append that lands all but its newline, a row after it: each arrives once\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 3; ++i) { ids.push_back(emit_ping()); loop_pass(); }   /* an outage */
  SD.short_by_next = 1;
  ids.push_back(emit_ping()); loop_pass();    /* its append fails: held, rows wait on the card */
  ids.push_back(emit_ping()); loop_pass();    /* sealed: that row's line is now whole on the card */
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "the walk sends the card's copy; the held copy is not sent again");
}

static void test_torn_but_whole_line_then_a_close_is_sent_once() {
  std::printf("-- F103: the same row's card copy sent, then the card closes: the held copy stays unsent\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 3; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  SD.short_by_next = 1;
  const uint32_t r = emit_ping(); loop_pass();
  const uint32_t s = emit_ping(); loop_pass();
  ids.push_back(r);
  ids.push_back(s);
  connect();
  for (int i = 0; i < 100 && !has(W.ha.accepted, r); ++i) loop_pass();
  CHECK(has(W.ha.accepted, r) && !has(W.ha.accepted, s),
        "the walk sent the card's copy of the row and parks before the next");
  card_out();
  drain();                                    /* the card wait ends; the hold flushes */
  card_back();
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "nothing went twice: the flush passed over the delivered copy, then the card's last row");
}

/* ── F104: a card that is not open holds new rows, for a bounded time ──── */

static void test_card_closed_mid_backlog_holds_new_rows() {
  std::printf("-- F104: the card closes mid-backlog, a row commits, the card comes back: nothing skipped\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 10; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  connect();
  for (int i = 0; i < 2; ++i) loop_pass();   /* the walk starts */
  CHECK(W.ha.accepted.size() < 10, "rows still wait on the card");
  card_out();                                 /* an SD error's lost mark, or a pull */
  loop_pass();
  ids.push_back(emit_ping());
  drain(4);
  CHECK(W.ha.accepted.size() < 10, "the new row waits while the card may come back");
  card_back();                                /* the storage manager remounts it */
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "every card row, then the row committed while it was out, each once");
}

static void test_late_card_mount_holds_new_rows() {
  std::printf("-- F104: a reboot with rows on a card that mounts late: a row committed first waits\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 5; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  W.mounted = false;                          /* power cycle; the boot mount is still running */
  boot();
  connect();
  loop_pass();
  ids.push_back(emit_ping());
  drain(4);
  CHECK(W.ha.accepted.empty(), "the row waits in RAM for the card");
  card_back();                                /* the mount worker's result is adopted */
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "the card's rows, then the row, each once");
  ids.push_back(emit_ping());
  loop_pass();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "the hold is empty: the next row goes live in its own pass, nothing sent twice");
}

static void test_card_wait_is_bounded() {
  std::printf("-- F104: a card that never opens: rows wait kCardWaitMs, then go\n");
  fresh_device(/*card=*/false);               /* a canary with no card at all */
  connect();
  loop_pass();
  const uint32_t a = emit_ping();
  drain(4);
  CHECK(W.ha.accepted.empty(), "a row committed just after boot waits for a card that may mount");
  drain();
  CHECK(exactly(W.ha.accepted, {a}), "and goes once the wait is over");
  const uint32_t b = emit_ping();
  loop_pass();
  CHECK(exactly(W.ha.accepted, {a, b}), "after that, rows go live at once");
  /* a card closed mid-backlog that stays out */
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 10; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  connect();
  for (int i = 0; i < 2; ++i) loop_pass();
  card_out();
  loop_pass();
  const uint32_t r = emit_ping();
  drain(4);
  CHECK(!has(W.ha.accepted, r), "the row waits while the card may come back");
  drain();
  CHECK(has(W.ha.accepted, r) && W.ha.refused.empty(),
        "past kCardWaitMs it goes; the rows still on the card are given up");
}

static void test_rows_held_for_a_card_ride_the_queue_once_it_is_given_up() {
  std::printf("-- F104: no card and no link: rows held for the card go to the offline queue after the wait\n");
  fresh_device(/*card=*/false);
  std::vector<uint32_t> ids;
  for (int i = 0; i < 3; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  drain();                                    /* the wait ends with the link still down */
  for (int i = 0; i < 2; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  CHECK(g_offline.size() == 5, "all five in the MQTT layer's offline queue, in order");
  connect();
  drain(20);
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(), "and they arrive, in order");
}

static void test_held_row_goes_before_a_newer_card_row() {
  std::printf("-- F104: a row held for a late card goes before a row committed once it opens\n");
  fresh_device();
  W.mounted = false;
  boot();
  connect();
  loop_pass();
  const uint32_t r = emit_ping();
  drain(4);
  card_back();                                /* an empty log: nothing older on it */
  const uint32_t g = emit_ping();             /* committed in the pass the card opens */
  drain();
  CHECK(exactly(W.ha.accepted, {r, g}) && W.ha.refused.empty(), "the held row first, then the card row");
}

static void test_ambient_rows_are_not_held() {
  std::printf("-- F104: ambient rows are never held, so they never evict a real row from the hold\n");
  fresh_device(/*card=*/false);
  connect();
  loop_pass();
  const uint32_t real = emit_ping(); loop_pass();
  int ambient = 0;
  for (int i = 0; i < 9; ++i) {
    if (emit_ambient()) ambient++;
    loop_pass();
  }
  CHECK(ambient == 9, "nine ambient rows committed during the card wait");
  drain();
  CHECK(exactly(W.ha.accepted, {real}), "the real row arrives; no ambient row was held");
  CHECK(csi_event_egress_stats().ambient_dropped == 9,
        "each ambient row that had to wait is counted (F109): health's ambient_dropped");
  const uint32_t live = emit_ambient();
  loop_pass();
  CHECK(live != 0 && !W.ha.accepted.empty() && W.ha.accepted.back() == live, "after the wait an ambient row goes live");
  CHECK(csi_event_egress_stats().ambient_dropped == 9, "one that went live is not counted");
}

static void test_broker_change_before_the_card_opens() {
  std::printf("-- F104: the broker changes before a late card opens: its rows are not the new broker's\n");
  fresh_device();
  for (int i = 0; i < 4; ++i) { (void)emit_ping(); loop_pass(); }   /* owed to broker A */
  W.mounted = false;
  boot();
  loop_pass();
  W.epoch++;                                  /* reprovisioned to broker B */
  loop_pass();
  card_back();
  loop_pass();
  connect();
  const uint32_t b = emit_ping();
  drain();
  CHECK(exactly(W.ha.accepted, {b}), "only the row committed for broker B goes to it");
}

static void test_held_rows_cover_no_card_rows_after_them() {
  std::printf("-- F104: rows held for a late card, the link down: the card rows after them stay owed\n");
  fresh_device();
  W.mounted = false;                          /* a card in, its boot mount still running */
  boot();
  loop_pass();
  (void)emit_ping(); loop_pass();             /* held: the card may hold older rows */
  card_back();                                /* an empty log: nothing older on it after all */
  loop_pass();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 3; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  boot();                                     /* power cycle before the broker returns */
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "the held row wrote no ceiling over the three card rows committed after it");
}

/* ── F104: how long the card wait lasts, and from when ─────────────────── */

static void test_card_closes_long_after_boot() {
  std::printf("-- F104: the card closes mid-backlog a minute after boot, a row, the card back: nothing skipped\n");
  fresh_device();
  stub_millis() += 60000;                     /* uptime well past kCardWaitMs */
  std::vector<uint32_t> ids;
  for (int i = 0; i < 10; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  connect();
  for (int i = 0; i < 2; ++i) loop_pass();
  card_out();
  loop_pass();
  ids.push_back(emit_ping());
  drain(4);
  CHECK(!has(W.ha.accepted, ids.back()), "the row waits: the wait is timed from the close, not from boot");
  card_back();
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "every card row, then the row committed while it was out, each once");
}

static void test_card_out_for_a_remount() {
  std::printf("-- F104: the card is out 30 s mid-backlog (the storage manager's recheck): nothing skipped\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 10; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  connect();
  for (int i = 0; i < 2; ++i) loop_pass();
  card_out();
  loop_pass();
  ids.push_back(emit_ping());
  drain((int)(30000 / kPassMs));
  CHECK(!has(W.ha.accepted, ids.back()), "after 30 s the row still waits for the card");
  card_back();
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "the card back after one recheck: every card row, then the row, each once");
}

/* Passes until the next one is the first at or past kCardWaitMs since `t0`. */
static void run_to_the_last_waiting_pass(uint32_t t0) {
  while ((uint32_t)(stub_millis() + kPassMs - t0) < csi_event_backfill::kCardWaitMs) loop_pass();
}

static void test_card_wait_ends_at_kCardWaitMs() {
  std::printf("-- F104: the wait ends exactly kCardWaitMs after it began; a row committed then waits its turn\n");
  fresh_device(/*card=*/false);               /* the wait began in begin(), at boot */
  const uint32_t t0 = stub_millis();
  connect();
  loop_pass();
  const uint32_t a = emit_ping(); loop_pass();
  run_to_the_last_waiting_pass(t0);
  CHECK(W.ha.accepted.empty(), "one pass short of kCardWaitMs after boot the row still waits");
  const uint32_t b = emit_ping();             /* routed in the pass the wait ends */
  loop_pass();
  drain(4);
  CHECK(exactly(W.ha.accepted, {a, b}) && W.ha.refused.empty(),
        "the next pass ends it: the held row goes, then the row routed in that pass behind it");
  /* a close long after boot: timed from the pass that saw the close */
  fresh_device();
  stub_millis() += 60000;
  std::vector<uint32_t> ids;
  for (int i = 0; i < 10; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  connect();
  for (int i = 0; i < 2; ++i) loop_pass();
  card_out();
  loop_pass();
  const uint32_t t_close = stub_millis();
  const uint32_t r = emit_ping(); loop_pass();
  run_to_the_last_waiting_pass(t_close);
  CHECK(!has(W.ha.accepted, r), "one pass short of kCardWaitMs after the close the row still waits");
  loop_pass();
  CHECK(has(W.ha.accepted, r) && W.ha.refused.empty(), "the next pass gives the card up and the row goes");
}

/* ── F104: the hold's own rules ────────────────────────────────────────── */

static void test_close_with_nothing_waiting_does_not_wait() {
  std::printf("-- F104: the card closes with nothing waiting on it: the next row goes at once\n");
  fresh_device();
  connect();
  loop_pass();
  const uint32_t a = emit_ping(); loop_pass();
  card_out();
  loop_pass();
  const uint32_t b = emit_ping(); loop_pass();
  CHECK(exactly(W.ha.accepted, {a, b}), "no card wait after a close that left nothing owed");
}

static void test_broker_change_drops_the_hold() {
  std::printf("-- F104: the broker changes while a row waits in the hold: it is not the new broker's\n");
  fresh_device();
  W.mounted = false;                          /* a card in, its boot mount still running */
  boot();
  connect();
  loop_pass();
  const uint32_t r = emit_ping(); loop_pass();   /* held for the card */
  W.epoch++;                                  /* reprovisioned to broker B */
  loop_pass();
  card_back();
  const uint32_t g = emit_ping();
  drain();
  CHECK(!has(W.ha.accepted, r) && has(W.ha.accepted, g) && W.ha.refused.empty(),
        "the held row owed to broker A is dropped; broker B gets its own row");
}

static void test_broker_change_ends_the_card_wait() {
  std::printf("-- F104: a broker change ends the card wait: rows for the new broker go at once\n");
  fresh_device(/*card=*/false);
  connect();
  loop_pass();
  W.epoch++;                                  /* nothing on any card is owed to broker B */
  loop_pass();
  const uint32_t g = emit_ping(); loop_pass();
  CHECK(exactly(W.ha.accepted, {g}), "broker B's row goes in its own pass");
}

static void test_broker_change_with_the_card_open_survives_a_remount() {
  std::printf("-- F104: the broker changes with the card open; rows for it wait on the card, which remounts\n");
  fresh_device();
  connect();
  loop_pass();
  W.epoch++;                                  /* applied at once: the card's log is open */
  loop_pass();
  W.connected = false;
  std::vector<uint32_t> ids;
  for (int i = 0; i < 3; ++i) { ids.push_back(emit_ping()); loop_pass(); }   /* owed to broker B */
  card_out();
  loop_pass();
  card_back();
  loop_pass();
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "the change is not applied again when the log reopens: broker B gets its three rows");
}

static void test_held_rows_keep_their_replay_flag() {
  std::printf("-- F104: a row held with the link up arrives as news; one held with it down as a replay\n");
  fresh_device();
  W.mounted = false;
  boot();
  connect();
  loop_pass();
  const uint32_t r = emit_ping(); loop_pass();   /* the link up: news */
  card_back();                                /* an empty log: the hold flushes */
  drain();
  CHECK(exactly(W.ha.accepted, {r}) && W.ha.replay.size() == 1 && !W.ha.replay[0],
        "flushed from the hold: not a replay");
  fresh_device();
  W.mounted = false;
  boot();
  connect();
  loop_pass();
  const uint32_t r2 = emit_ping(); drain(4);
  card_back();
  const uint32_t g2 = emit_ping();            /* on the card; the held row goes ahead of it */
  drain();
  CHECK(exactly(W.ha.accepted, {r2, g2}) && W.ha.replay.size() == 2 && !W.ha.replay[0] &&
        !W.ha.replay[1], "sent ahead of a card row: not a replay, nor is the card row");
  fresh_device();
  W.mounted = false;
  boot();
  loop_pass();
  const uint32_t r3 = emit_ping(); loop_pass();  /* the link down: a replay when it goes */
  card_back();
  loop_pass();
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, {r3}) && W.ha.replay.size() == 1 && W.ha.replay[0],
        "held while the link was down: a replay");
}

static void test_held_row_publish_failure_is_retried() {
  std::printf("-- F104: a held row's publish fails ahead of a card row: it is retried, then the card row\n");
  fresh_device();
  W.mounted = false;
  boot();
  connect();
  loop_pass();
  const uint32_t r = emit_ping();
  drain(4);
  card_back();
  W.fail_next_sends = 1;                      /* the held row's live publish fails once */
  const uint32_t g = emit_ping();
  drain();
  CHECK(exactly(W.ha.accepted, {r, g}) && W.ha.refused.empty(),
        "the held row is kept and goes first; the card row waits for it");
}

/* As tamper_events_module.cpp declares it: system.integrity's "tamper",
 * whose rows also ride the tamper topic (the bridge), and a bundle. */
static const csi_event_decl_t TAMPER_EVENTS[] = {
  { "tamper", CSI_FIELD_STATE_NAME | CSI_FIELD_TIME_BUCKET, CSI_PRIVACY_P0, 0 },
};
static const csi_module_t TAMPER_MODULE = {
  "system.integrity", CSI_PRIVACY_P0, TAMPER_EVENTS, 1, noop_init, noop_tick, nullptr, nullptr,
};
static uint32_t commit_tamper() {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_TIME_BUCKET;
  std::strcpy(v.state_name, "sd_removed");
  (void)csi_event_emit("system.integrity", "tamper", &v);
  csi_event_flush_bundles();
  return csi_event_get_next_event_id() - 1;
}

/* The MQTT layer refuses an event only when its offline queue is full of
 * tamper alerts (it never displaces one; an event pushed into a full queue
 * otherwise displaces the oldest event). Twelve tamper rows committed in the
 * card wait with the link down: their twelve bridge alerts fill the queue,
 * and the hold keeps the newest eight rows. The wait ends with the link
 * still down, so every flush is refused, pass after pass, and must keep its
 * row. Once the link is back, mqtt_loop() sends four alerts a pass and the
 * flush puts the eight rows behind the eight alerts left: four fit, and the
 * queue's own overflow rule keeps the newest four. */
static void test_hold_flush_refused_by_a_full_queue_is_kept() {
  std::printf("-- F104: the offline queue is full of tamper alerts when the hold flushes: the rows are kept\n");
  fresh_device(/*card=*/false);
  std::vector<uint32_t> ids;
  for (int i = 0; i < (int)kOfflineSlots; ++i) { ids.push_back(commit_tamper()); loop_pass(); }
  CHECK(g_offline.size() == kOfflineSlots, "twelve tamper alerts fill the offline queue");
  drain();                                    /* the wait ends with the link down: the flush is refused */
  CHECK(W.ha.accepted.empty() && g_offline.size() == kOfflineSlots,
        "no event displaced a tamper alert");
  connect();
  drain(40);
  CHECK(W.tampers == (int)kOfflineSlots, "the tamper alerts go first");
  const std::vector<uint32_t> want(ids.end() - 4, ids.end());
  CHECK(exactly(W.ha.accepted, want) && W.ha.refused.empty(),
        "then the held rows the queue had room for, in order: the refused flushes kept them");
}

static void test_hold_overflow_drops_the_oldest() {
  std::printf("-- F104: ten rows wait for a late card: the oldest two are dropped\n");
  fresh_device();
  W.mounted = false;
  boot();
  connect();
  loop_pass();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 10; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  card_back();
  drain();
  const std::vector<uint32_t> want(ids.begin() + 2, ids.end());
  CHECK(exactly(W.ha.accepted, want) && W.ha.refused.empty(), "the newest eight, in order");
  CHECK(csi_event_egress_stats().held_dropped == 2,
        "and the two it dropped are counted (F109): health's held_dropped");
}

/* ── F82: a warning before the event-id space runs out ──────────────────── */

static void test_id_space_low_is_flagged() {
  std::printf("-- F82: the egress says the id space is low from kHoldLimit on, and after a wrap\n");
  using csi_event_id_floor::kHoldLimit;
  fresh_device();
  connect();
  loop_pass();
  (void)emit_ping(); loop_pass();
  CHECK(!csi_event_egress_id_space_low(), "a fresh device's ids are far from the end");
  csi_event_set_event_id_floor(kHoldLimit - 1);
  CHECK(!csi_event_egress_id_space_low(), "one id short of kHoldLimit: not yet");
  const uint32_t last_before = emit_ping(); loop_pass();
  CHECK(last_before == kHoldLimit - 1 && csi_event_egress_id_space_low(),
        "the allocator reaches kHoldLimit: the health flag is up");
  /* A reboot restores the floor from NVS, past kHoldLimit: up before any row. */
  boot();
  CHECK(csi_event_get_next_event_id() > kHoldLimit && csi_event_egress_id_space_low(),
        "after a reboot it is up before the boot's first row");
  /* The wrap: ids restart at 1, below the id space. */
  csi_event_set_event_id_floor(0xFFFFFFFEu);
  uint32_t id = 0;
  for (int i = 0; i < 3; ++i) { id = emit_ping(); loop_pass(); }
  CHECK(id == 1 && csi_event_egress_id_space_low(), "after the wrap (ids from 1 again) it stays up");
  /* Each later boot first reissues 0xFFFFFFFF (the floor saturates). */
  boot();
  CHECK(csi_event_get_next_event_id() == 0xFFFFFFFFu && csi_event_egress_id_space_low(),
        "and after the next reboot too");
}

/* ── F109: what the egress did, where a host can read it ───────────────── */

static void test_egress_counts_what_it_did() {
  std::printf("-- F109: the egress counts its live, held, replayed and queued rows and its drops\n");
  fresh_device();
  connect();
  loop_pass();
  for (int i = 0; i < 3; ++i) { (void)emit_ping(); loop_pass(); }
  W.connected = false;
  for (int i = 0; i < 4; ++i) { (void)emit_ping(); loop_pass(); }
  CsiEventEgressStats st = csi_event_egress_stats();
  CHECK(st.planner.live == 3 && st.planner.held == 4 && st.planner.replayed == 0,
        "three rows went live, four wait on the card");
  connect();
  drain(40);
  st = csi_event_egress_stats();
  CHECK(st.planner.replayed == 4 && W.ha.accepted.size() == 7, "the backfill sent the four");
  /* The loop task stalls: ten commits, no pump. The egress queue holds eight. */
  for (int i = 0; i < 10; ++i) (void)emit_ping();
  drain(10);
  st = csi_event_egress_stats();
  CHECK(st.dropped == 2, "the two commits the full egress queue refused are counted");
  CHECK(st.planner.live == 11 && W.ha.accepted.size() == 15, "the eight it kept went live");
  CHECK(st.held_dropped == 0 && st.ambient_dropped == 0, "nothing else was dropped");

  /* No card: rows the planner hands to the MQTT layer are counted queued. */
  fresh_device(/*card=*/false);
  drain();   /* past the wait for a card that is not there */
  for (int i = 0; i < 3; ++i) { (void)emit_ping(); loop_pass(); }
  st = csi_event_egress_stats();
  CHECK(st.planner.queued == 3 && st.planner.live == 0, "with no card, three rows queued");
  /* A power cycle starts every counter over (they are this boot's). */
  boot();
  st = csi_event_egress_stats();
  CHECK(st.dropped == 0 && st.held_dropped == 0 && st.ambient_dropped == 0 &&
        st.planner.queued == 0 && st.planner.live == 0, "a reboot starts the counters over");
}

static void test_failed_append_as_the_card_opens_waits_behind_the_hold() {
  std::printf("-- F103: a row whose append fails in the pass a late card opens waits behind the held row\n");
  fresh_device();
  connect();
  loop_pass();
  const uint32_t a = emit_ping(); loop_pass();   /* live: the card is claimed, nothing owed on it */
  W.mounted = false;                          /* power cycle; the boot mount is still running */
  boot();
  connect();
  loop_pass();
  const uint32_t r = emit_ping(); drain(4);   /* held for the card */
  card_back();                                /* opening it writes nothing: it is ours already */
  SD.fail_writes = true;
  const uint32_t f = emit_ping();
  loop_pass();                                /* the log opens, nothing waits on it, then f's append fails */
  SD.fail_writes = false;
  drain();
  CHECK(exactly(W.ha.accepted, {a, r, f}) && W.ha.refused.empty(),
        "the held row first, then the failed-append row, each once");
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main() {
  Serial.quiet = true;
  csi_event_test_reset();
  csi_module_register(&MODULE);
  csi_module_register(&AMBIENT_MODULE);
  csi_module_register(&TAMPER_MODULE);

  test_steady_state_is_live();
  test_outage_with_a_card_backfills_in_order();
  test_reboot_in_an_outage_keeps_the_backlog_owed();
  test_no_card_outage_uses_the_offline_queue();
  test_row_after_an_outage_waits_behind_the_queue();
  test_held_row_flush_waits_behind_the_queue();
  test_failed_append_in_an_outage_then_a_reboot();
  test_failed_append_in_an_outage_waits_its_turn();
  test_failed_append_mid_backfill_waits_its_turn();
  test_failed_append_that_opens_an_outage_covers_nothing();
  test_failed_append_with_nothing_waiting_goes_live();
  test_failed_append_waits_behind_a_held_row();
  test_failed_append_behind_a_held_row_covers_nothing();
  test_torn_but_whole_line_is_sent_once();
  test_torn_but_whole_line_then_a_close_is_sent_once();
  test_card_closed_mid_backlog_holds_new_rows();
  test_late_card_mount_holds_new_rows();
  test_card_wait_is_bounded();
  test_rows_held_for_a_card_ride_the_queue_once_it_is_given_up();
  test_held_row_goes_before_a_newer_card_row();
  test_held_rows_cover_no_card_rows_after_them();
  test_ambient_rows_are_not_held();
  test_broker_change_before_the_card_opens();
  test_card_closes_long_after_boot();
  test_card_out_for_a_remount();
  test_card_wait_ends_at_kCardWaitMs();
  test_close_with_nothing_waiting_does_not_wait();
  test_broker_change_drops_the_hold();
  test_broker_change_ends_the_card_wait();
  test_broker_change_with_the_card_open_survives_a_remount();
  test_held_rows_keep_their_replay_flag();
  test_held_row_publish_failure_is_retried();
  test_hold_flush_refused_by_a_full_queue_is_kept();
  test_hold_overflow_drops_the_oldest();
  test_failed_append_as_the_card_opens_waits_behind_the_hold();
  test_egress_counts_what_it_did();
  test_id_space_low_is_flagged();

  CHECK(g_ceiling_violations_total == 0,
        "in every scenario, each id was under the NVS ceiling before it was handed over (F47)");
  if (g_fail) {
    std::printf("test_canary_event_egress: %d of %d checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  std::printf("test_canary_event_egress: %d checks passed\n", g_checks);
  return 0;
}
