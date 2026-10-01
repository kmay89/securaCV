/* test_wap_event_egress.cpp — the canary-wap's committed-event egress
 * (csi_event_egress.cpp): what reaches Home Assistant, in what order, after
 * an outage, a reconnect and a reboot (backlog F78).
 *
 * Compiles the REAL csi_event_egress.cpp, the REAL csi_event_log.cpp over a
 * RAM card (stubs/sd_fake) and the REAL staged CSI chokepoint
 * (csi_event.cpp / csi_module.cpp / csi_bundler.cpp, byte-identical to
 * firmware/common/csi/src by check_csi_sync.sh), with the staged
 * csi_event_backfill.h. Rows are committed through csi_event_emit, so ids,
 * the ring and the bundler are the firmware's.
 *
 * The model around it (this file):
 *   - csi_mqtt's wire: publish_event_row() refuses while the link is down,
 *     and otherwise hands the row to Home Assistant's replay gate
 *     (custom_components/securacv/sensor.py `_replay_gate`: a body whose
 *     event_id is below the last verified one is refused);
 *   - csi_integration.cpp's glue: the commit hook (privacy gate, then
 *     csi_event_egress::on_committed), the id floor's NVS persistence
 *     (csi_event_on_id_advance) and its boot restore
 *     (apply_event_id_floor_from_nvs, then csi_event_egress::begin);
 *   - the esp_mqtt task: connect() flips the link up and raises the backfill
 *     request MQTT_EVENT_CONNECTED raises.
 * firmware/scripts/check_wap_event_egress.py holds the firmware's hook and
 * call sites to that glue (the hook only enqueues; begin after the floor
 * restore; the pump on the loop task).
 *
 * The three F78 properties, each failing on the code before the fix (built
 * with -DEGRESS_PRE_FIX against the sources the fix replaced):
 *   - a reconnect with a backlog on the card, where a row commits before the
 *     main loop drains: every row reaches HA once, in id order, none skipped
 *     (a direct row, and a closed bundle, which is not on the card: F77);
 *   - the commit hook and the backfill cannot both advance the watermark: a
 *     commit that lands in the middle of the backfill walk (as the NimBLE
 *     host task's would) publishes nothing and moves nothing; the rows still
 *     arrive once, in order.
 * The rest pin the planner's rules on this device: no broker means not
 * owed, a reboot mid-backlog republishes nothing, a dismissal line is never
 * replayed, a send failure keeps its row, rows the card cannot keep wait in
 * RAM in order, the hook never blocks.
 *
 * What it does not pin: the real SD driver, esp_mqtt, FreeRTOS scheduling
 * (the host build compiles the chokepoint's locks out; the interleaving is
 * modeled by committing from inside a publish). Bench territory. */

#include "csi_event_egress.h"
#include "csi_event_id_floor.h"
#include "csi_event_log.h"
#include "csi_event_log_line.h"
#include "csi_integration.h"
#include "csi_module.h"
#include "csi_mqtt.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#ifndef EGRESS_PRE_FIX
#include <freertos/queue.h>
#endif

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

bool sd_mount_in_flight() { return false; }

static int g_fail = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); g_fail++; } \
  else { printf("ok   %s\n", msg); } } while (0)

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

/* ── The world: link, wire, NVS checks ─────────────────────────────────── */
struct World {
  bool accepting = true;
  bool connected = false;
  bool backfill_request = false;   /* MQTT_EVENT_CONNECTED's flag (pre-fix API) */
  int  fail_next_publishes = 0;
  int  publishes_in_hook = 0;      /* a publish made from inside the commit hook */
  int  watermark_moves_in_hook = 0;
  int  ceiling_violations = 0;     /* an id handed over with NVS not past it */
  int  tamper_bridges = 0;
  bool in_hook = false;
  std::function<void(uint32_t)> after_publish;   /* fires once */
  Ha ha;
  std::vector<uint32_t> wire;      /* every events publish the broker took */
};
static World W;

static uint32_t nvs_get(const char* key) {
  Preferences p;
  if (!p.begin("csi", true)) return 0;
  const uint32_t v = p.getULong(key, 0);
  p.end();
  return v;
}

namespace csi_mqtt {
bool connected() { return W.connected; }
bool accepting() { return W.accepting; }
bool take_backfill_request() {
  const bool r = W.backfill_request;
  W.backfill_request = false;
  return r;
}
EventSend publish_event_row(const csi_event_record_t& rec, uint16_t, bool replay) {
  if (W.in_hook) W.publishes_in_hook++;
  if (!W.connected) return EventSend::kNotNow;
  if (W.fail_next_publishes > 0) { W.fail_next_publishes--; return EventSend::kNotNow; }
  /* NVS must already hold a delivery ceiling above the id (F47), unless
   * NVS refuses writes. */
  if (!host_nvs().fail_puts && nvs_get(NVS_KEY_DELIVERED) <= rec.event_id) W.ceiling_violations++;
  W.wire.push_back(rec.event_id);
  W.ha.receive(rec.event_id, replay);
  if (W.after_publish) {
    auto f = W.after_publish;
    W.after_publish = nullptr;
    f(rec.event_id);
  }
  return EventSend::kSent;
}
bool publish_tamper_bridge(const char* module_id, const char*, const csi_event_values_t*) {
  if (strcmp(module_id, "system.integrity") != 0) return false;
  W.tamper_bridges++;
  return W.connected;
}
}  // namespace csi_mqtt

/* ── csi_integration.cpp's glue ────────────────────────────────────────── */
static uint32_t g_floor_stored = 0;
namespace csi_integration {
uint32_t event_id_floor_stored() { return g_floor_stored; }
}

extern "C" {
bool csi_event_commit_witness(uint32_t, const char*, const char*, csi_event_category_t,
                              const csi_event_values_t*) {
  return true;
}
/* csi_integration.cpp's strong override: the privacy gate, the stream
 * snapshot (not modeled), then the egress. */
void csi_event_on_committed(uint32_t event_id, const char* module_id, const char* type_name,
                            csi_event_category_t category, csi_privacy_class_t privacy,
                            const csi_event_values_t* values) {
  if (!values) return;
  if (privacy > csi_event_get_privacy_ceiling()) return;
  W.in_hook = true;
  const uint32_t before = csi_event_egress::watermark();
  csi_event_egress::on_committed(event_id, module_id, type_name, category, privacy, values);
  if (csi_event_egress::watermark() != before) W.watermark_moves_in_hook++;
  W.in_hook = false;
}
/* csi_integration.cpp's floor persistence (csi_event_id_floor.h's cadence). */
void csi_event_on_id_advance(uint32_t id) {
  if (!csi_event_id_floor::must_persist(g_floor_stored, id)) return;
  Preferences p;
  if (!p.begin("csi", false)) return;
  const uint32_t f = csi_event_id_floor::floor_for(id);
  if (p.putULong("ev.next", f) > 0) g_floor_stored = f;
  p.end();
}
}

/* ── Rows ──────────────────────────────────────────────────────────────── */
static void noop_init(const csi_module_settings_t*) {}
static void noop_tick(const csi_features_t*) {}
static const csi_event_decl_t EVENTS[] = {
  /* stateless: commits at once, into the ring, onto the card */
  { "ping", CSI_FIELD_NOTE | CSI_FIELD_TIME_BUCKET, CSI_PRIVACY_P0, 0 },
  /* state-bearing: bundled; a closed bundle is not in the ring, so not on
   * the card (F77) */
  { "state", CSI_FIELD_STATE_NAME | CSI_FIELD_TIME_BUCKET, CSI_PRIVACY_P0, 0 },
};
static const csi_module_t MODULE = {
  "test.egress", CSI_PRIVACY_P0, EVENTS, sizeof(EVENTS) / sizeof(EVENTS[0]),
  noop_init, noop_tick, nullptr, nullptr,
};
static const csi_event_decl_t TAMPER_EVENTS[] = {
  { "integrity_event", CSI_FIELD_STATE_NAME | CSI_FIELD_TIME_BUCKET, CSI_PRIVACY_P0, 0 },
};
static const csi_module_t TAMPER_MODULE = {
  "system.integrity", CSI_PRIVACY_P0, TAMPER_EVENTS, 1, noop_init, noop_tick, nullptr, nullptr,
};

static uint32_t emit_ping() {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_NOTE | CSI_FIELD_TIME_BUCKET;
  strcpy(v.note, "p");
  return csi_event_emit("test.egress", "ping", &v);
}

/* A state-bearing emit, then its bundle closes: one committed row that is
 * not on the card. Returns its event id (the newest id). */
static uint32_t commit_closed_bundle(const char* module = "test.egress",
                                     const char* type = "state",
                                     const char* state = "active") {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_TIME_BUCKET;
  strncpy(v.state_name, state, sizeof(v.state_name) - 1);
  (void)csi_event_emit(module, type, &v);
  csi_event_flush_bundles();
  return csi_event_get_next_event_id() - 1;
}

/* ── Boot, link, loop ──────────────────────────────────────────────────── */
static void restore_floor() {
  /* apply_event_id_floor_from_nvs */
  const uint32_t persisted = nvs_get("ev.next");
  const uint32_t delivered = nvs_get(csi_mqtt::NVS_KEY_DELIVERED);
  csi_event_set_event_id_floor(csi_event_id_floor::boot_floor(persisted, delivered));
  if (persisted > 0) g_floor_stored = persisted;
}

/* A power cycle: RAM starts over, NVS and the card stay. */
static void boot() {
  csi_event_test_reset();
  csi_event_log::test_rearm_load();
  csi_event_egress::test_reset();
  g_floor_stored = 0;
  W.connected = false;
  W.backfill_request = false;
  restore_floor();
  csi_event_egress::begin();
  csi_event_log::arm_load();   /* csi_integration::init, the floor restored */
}

/* A fresh device: empty NVS, a card with an empty /EVENTS, no broker link. */
static void fresh_device(bool card = true) {
  host_nvs().u32.clear();
  host_nvs().fail_puts = false;
  SD.files.clear();
  SD.dirs.clear();
  SD.present = card;
  SD.fail_writes = false;
  if (card) SD.dirs.insert("/EVENTS");
  W = World();
  boot();
}

static void connect() {
  W.connected = true;
  W.backfill_request = true;   /* MQTT_EVENT_CONNECTED */
}

/* One main-loop pass. */
static void loop_pass() {
  stub_millis() += 50;
  csi_event_egress::pump();
}

static void drain(int passes = 400) {
  for (int i = 0; i < passes; ++i) loop_pass();
}

[[maybe_unused]] static bool rising(const std::vector<uint32_t>& ids) {
  for (size_t i = 1; i < ids.size(); ++i) if (ids[i] <= ids[i - 1]) return false;
  return true;
}

static bool exactly(const std::vector<uint32_t>& got, const std::vector<uint32_t>& want) {
  return got == want;
}

/* ── F78: the live publish waits behind the backlog ────────────────────── */

static void test_reconnect_window_direct_row() {
  printf("-- a direct row committed between CONNECTED and the drain\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 5; ++i) { ids.push_back(emit_ping()); loop_pass(); }   // the outage
  CHECK(W.ha.accepted.empty(), "nothing reaches HA while the link is down");
  connect();
  ids.push_back(emit_ping());   // commits before the main loop drains the backfill
  drain();
  CHECK(exactly(W.ha.accepted, ids), "HA has every row once, in id order");
  CHECK(W.ha.refused.empty(), "and refused none");
  CHECK(W.ceiling_violations == 0, "every id was under the NVS ceiling before it went");
}

static void test_reconnect_window_closed_bundle() {
  printf("-- a closed bundle (not on the card) committed in the same window\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 5; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  connect();
  ids.push_back(commit_closed_bundle());
  drain();
  CHECK(exactly(W.ha.accepted, ids), "the bundle arrives after the backlog, every row once, in order");
  CHECK(W.ha.refused.empty(), "and HA refused none");
}

static void test_commit_inside_the_backfill_walk() {
  printf("-- a commit lands in the middle of the backfill walk (the NimBLE task's)\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 4; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  connect();
  uint32_t nested = 0;
  W.after_publish = [&nested](uint32_t) { nested = emit_ping(); };   // right after the 1st backfill send
  drain();
  ids.push_back(nested);
  CHECK(nested != 0, "the nested commit happened");
  CHECK(W.publishes_in_hook == 0, "the commit hook published nothing (no MQTT under the commit lock)");
  CHECK(W.watermark_moves_in_hook == 0, "the commit hook never moved the watermark");
  CHECK(exactly(W.ha.accepted, ids), "HA has every row once, in id order");
  CHECK(W.ha.refused.empty(), "and refused none");
  /* the same with a closed bundle committed from inside the walk */
  fresh_device();
  ids.clear();
  for (int i = 0; i < 4; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  connect();
  nested = 0;
  W.after_publish = [&nested](uint32_t) { nested = commit_closed_bundle(); };
  drain();
  ids.push_back(nested);
  CHECK(W.publishes_in_hook == 0 && W.watermark_moves_in_hook == 0,
        "a bundle closing inside the walk publishes nothing from the hook either");
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "and it arrives after the rows before it, once, in order");
}

#ifndef EGRESS_PRE_FIX
/* ── The planner's rules on this device ────────────────────────────────── */

static void test_steady_state_is_live() {
  printf("-- link up, nothing waiting: each row goes out on the next pass\n");
  fresh_device();
  connect();
  loop_pass();
  const uint32_t a = emit_ping();
  CHECK(W.ha.accepted.empty(), "the hook only enqueues");
  loop_pass();
  CHECK(exactly(W.ha.accepted, {a}) && W.ha.replay.back() == false,
        "the next pass publishes it, not a replay");
  const uint32_t b = commit_closed_bundle();
  loop_pass();
  CHECK(exactly(W.ha.accepted, {a, b}) && W.ha.replay.back() == false,
        "a closed bundle too");
  CHECK(csi_event_egress::watermark() == b, "the watermark is the last id handed over");
}

static void test_reboot_mid_backlog() {
  printf("-- a reboot with rows waiting: they are still owed, nothing republished\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 6; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  boot();                       // power cycle during the outage
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids), "the earlier boot's rows arrive once, in order");
  CHECK(std::all_of(W.ha.replay.begin(), W.ha.replay.end(), [](bool r) { return r; }),
        "marked as replays");
  const size_t sent = W.wire.size();
  boot();
  connect();
  drain();
  CHECK(W.wire.size() == sent, "a second reboot republishes nothing");
  const uint32_t c = emit_ping();
  drain(4);
  CHECK(W.ha.accepted.back() == c && W.ha.refused.empty(), "and the new boot's row goes out");
}

static void test_no_broker_is_not_owed() {
  printf("-- no broker configured: rows are logged, owed to nobody\n");
  fresh_device();
  W.accepting = false;
  for (int i = 0; i < 3; ++i) { emit_ping(); loop_pass(); }
  CHECK(W.wire.empty(), "nothing goes out");
  W.accepting = true;
  connect();
  drain();
  CHECK(W.wire.empty(), "a broker configured later is not sent the earlier rows");
  const uint32_t d = emit_ping();
  drain(4);
  CHECK(exactly(W.ha.accepted, {d}), "the next row goes out live");
}

static void test_dismissal_is_not_replayed() {
  printf("-- a dismissal line is never replayed\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 3; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  CHECK(csi_event_dismiss(ids[1]) && csi_event_log::queue_dismissal(ids[1]), "the owner dismisses one");
  CHECK(csi_event_log::flush_dismissals() == 1, "its dismissal line is written");
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.wire.size() == 3,
        "the three rows go out once each; the dismissal line does not");
}

static void test_send_failure_keeps_the_row() {
  printf("-- a send that fails mid-walk is retried, not skipped\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 5; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  connect();
  loop_pass();                   // the walk starts
  W.fail_next_publishes = 2;
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "every row arrives once, in order, after the failures");
}

static void test_rows_without_a_card_wait_in_ram() {
  printf("-- no card: rows wait in RAM through an outage, in order, the oldest dropped first\n");
  fresh_device(/*card=*/false);
  std::vector<uint32_t> ids;
  for (int i = 0; i < 3; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids), "three rows held through the outage arrive in order");
  CHECK(std::all_of(W.ha.replay.begin(), W.ha.replay.end(), [](bool r) { return r; }),
        "marked as replays (built while the link was down)");
  W.connected = false;
  ids.clear();
  for (int i = 0; i < 12; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  const csi_event_egress::Stats st = csi_event_egress::stats();
  CHECK(st.held_dropped == 4, "past the RAM hold's 8 rows the oldest are dropped, counted");
  W.ha.accepted.clear();
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, std::vector<uint32_t>(ids.begin() + 4, ids.end())),
        "the newest 8 arrive, in order");
  CHECK(W.ha.refused.empty(), "and HA refused none");
}

static void test_bundles_and_card_rows_interleave_in_order() {
  printf("-- card rows and RAM rows waiting together go out in id order\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 3; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  ids.push_back(commit_closed_bundle()); loop_pass();
  ids.push_back(emit_ping()); loop_pass();
  ids.push_back(commit_closed_bundle()); loop_pass();
  ids.push_back(emit_ping()); loop_pass();
  connect();
  ids.push_back(commit_closed_bundle());   // and one in the reconnect window
  ids.push_back(emit_ping());
  drain();
  CHECK(exactly(W.ha.accepted, ids), "every row once, in id order");
  CHECK(rising(W.wire) && W.ha.refused.empty(), "the wire never goes backwards");
}

static void test_tamper_bridge_does_not_wait() {
  printf("-- a tamper row's bridge publishes at once, while its events row waits its turn\n");
  fresh_device();
  for (int i = 0; i < 4; ++i) { emit_ping(); loop_pass(); }
  connect();
  const uint32_t t = commit_closed_bundle("system.integrity", "integrity_event", "sd_removed");
  loop_pass();
  CHECK(W.tamper_bridges == 1, "the bridge went on the first pass");
  CHECK(std::find(W.ha.accepted.begin(), W.ha.accepted.end(), t) == W.ha.accepted.end(),
        "its events row waits behind the backlog");
  drain();
  CHECK(W.ha.accepted.back() == t && W.ha.refused.empty(), "and arrives last, in order");
}

static void test_the_hook_never_blocks() {
  printf("-- a full commit queue drops (counted) instead of blocking the committing task\n");
  fresh_device();
  connect();
  loop_pass();
  const unsigned full_before = stub_queue_sends_while_full();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 20; ++i) ids.push_back(emit_ping());   // the loop task stalls
  CHECK(stub_queue_sends_while_full() - full_before == 4, "16 queued, 4 refused at once");
  CHECK(csi_event_egress::stats().dropped == 4, "and counted");
  drain();
  CHECK(exactly(W.ha.accepted, std::vector<uint32_t>(ids.begin(), ids.begin() + 16)),
        "the queued 16 go out in order");
}

static void test_ram_row_goes_before_a_newer_card_row() {
  printf("-- a row waiting in RAM goes before a card row committed in the reconnect window\n");
  fresh_device();
  std::vector<uint32_t> ids;
  ids.push_back(commit_closed_bundle()); loop_pass();   // no card backlog, link down: RAM
  connect();
  ids.push_back(emit_ping());                           // before the main loop's next pass
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "the older RAM row first, then the card row, each once");
}

static void test_held_row_does_not_raise_the_ceiling_past_the_card() {
  printf("-- a row waiting in RAM does not move the NVS ceiling past rows waiting on the card\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 5; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  (void)commit_closed_bundle(); loop_pass();   // waits in RAM behind the card rows
  boot();                                      // power cycle: the RAM row is gone
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids), "the card's rows are still owed after the reboot, and arrive");
}

static void test_dismissal_line_ahead_of_unsent_rows() {
  printf("-- a dismissal line the walk reaches before its row is sent is not replayed\n");
  fresh_device();
  const uint32_t x = emit_ping(); loop_pass();   // link down: on the card, unsent
  CHECK(csi_event_dismiss(x) && csi_event_log::queue_dismissal(x) &&
        csi_event_log::flush_dismissals() == 1, "the owner dismisses it");
  const uint32_t y = emit_ping(); loop_pass();
  /* A retention cut drops the head of the log to a line start: here it
   * takes x's original and leaves its dismissal line ahead of y. */
  std::string& log = SD.files["/EVENTS/today.ndjson"];
  log.erase(0, log.find('\n') + 1);
  boot();
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, {y}), "y goes out; the dismissal line does not");
}

static void test_failed_append_waits_behind_the_backlog() {
  printf("-- a row whose card append fails waits in RAM behind the rows on the card\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 3; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  connect();
  SD.fail_writes = true;
  ids.push_back(emit_ping());
  loop_pass();                 // its append fails while the backlog waits
  SD.fail_writes = false;
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "it goes out after the backlog, every row once, in order");
}

static void test_unconfigured_broker_drops_the_backlog() {
  printf("-- a broker unconfigured during an outage: the backlog is owed to nobody\n");
  fresh_device();
  for (int i = 0; i < 3; ++i) { emit_ping(); loop_pass(); }
  (void)commit_closed_bundle(); loop_pass();
  W.accepting = false;
  loop_pass();
  W.accepting = true;
  connect();
  drain();
  CHECK(W.wire.empty(), "a broker configured again is not sent the card's or RAM's rows");
  const uint32_t d = emit_ping();
  drain(4);
  CHECK(exactly(W.ha.accepted, {d}), "the next row goes out live");
}

static std::string card_line(uint32_t id) {
  csi_event_record_t r;
  memset(&r, 0, sizeof(r));
  r.event_id = id;
  r.category = CSI_CATEGORY_EVENT;
  r.privacy = CSI_PRIVACY_P0;
  r.bundled_count = 1;
  strcpy(r.module_id, "test.egress");
  strcpy(r.type_name, "ping");
  strcpy(r.values.note, "p");
  char buf[csi_event_log_line::kLineMax];
  return std::string(buf, csi_event_log_line::marshal(&r, buf, sizeof(buf)));
}

static void test_forged_card_line_is_never_sent() {
  printf("-- a card line this device never handed out is never sent (F46)\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 2; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  boot();   // the next id is now above every id the card holds
  const uint32_t next = csi_event_get_next_event_id();
  SD.files["/EVENTS/today.ndjson"] += card_line(0xFFFFFFF0u) + card_line(next);
  csi_event_log::test_rearm_load();   // the card is opened again on this boot
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids), "the real rows go out; the forged ids do not");
  CHECK(csi_event_egress::watermark() < next, "and the watermark never takes them");
}

static void test_dismissal_written_before_its_original() {
  printf("-- the owner dismisses a row before the egress logs it: it still goes out, once\n");
  fresh_device();
  loop_pass();                   // the card is open
  const uint32_t x = emit_ping();   // the hook queued its copy
  CHECK(csi_event_dismiss(x) && csi_event_log::queue_dismissal(x), "dismissed at once");
  CHECK(csi_event_log::flush_dismissals() == 1,
        "csi_integration::loop writes the dismissal before the pump logs the original");
  loop_pass();
  const uint32_t y = emit_ping(); loop_pass();
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, {x, y}), "both rows go out once, in order");
  boot();
  CHECK(csi_event_log::load_into_ring() >= 1, "after a reboot the log reloads");
  csi_event_record_t r;
  CHECK(csi_event_find(x, &r) && r.values.dismissed == 1, "with the row dismissed");
}

static void test_nvs_failure_still_delivers() {
  printf("-- NVS refusing writes does not stop delivery\n");
  fresh_device();
  host_nvs().fail_puts = true;
  std::vector<uint32_t> ids;
  for (int i = 0; i < 3; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids), "the rows go out");
  host_nvs().fail_puts = false;
}
#endif  // !EGRESS_PRE_FIX

int main() {
  Serial.quiet = true;
  csi_event_test_reset();
  csi_module_register(&MODULE);
  csi_module_register(&TAMPER_MODULE);

  test_reconnect_window_direct_row();
  test_reconnect_window_closed_bundle();
  test_commit_inside_the_backfill_walk();
#ifndef EGRESS_PRE_FIX
  test_steady_state_is_live();
  test_reboot_mid_backlog();
  test_no_broker_is_not_owed();
  test_dismissal_is_not_replayed();
  test_send_failure_keeps_the_row();
  test_rows_without_a_card_wait_in_ram();
  test_bundles_and_card_rows_interleave_in_order();
  test_tamper_bridge_does_not_wait();
  test_the_hook_never_blocks();
  test_ram_row_goes_before_a_newer_card_row();
  test_held_row_does_not_raise_the_ceiling_past_the_card();
  test_dismissal_line_ahead_of_unsent_rows();
  test_failed_append_waits_behind_the_backlog();
  test_unconfigured_broker_drops_the_backlog();
  test_forged_card_line_is_never_sent();
  test_dismissal_written_before_its_original();
  test_nvs_failure_still_delivers();
#endif

  if (g_fail == 0) {
    printf("ALL wap event egress tests PASSED\n");
    return 0;
  }
  printf("%d FAILED\n", g_fail);
  return 1;
}
