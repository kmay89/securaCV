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
 *     says a marked row's body never builds, and otherwise hands the row to
 *     Home Assistant's replay gate (custom_components/securacv/sensor.py
 *     `_replay_gate`: a body whose event_id is below the last verified one
 *     is refused); destination_epoch() moves when the broker changes;
 *   - csi_integration.cpp's glue: the commit hook (privacy gate, then
 *     csi_event_egress::on_committed), the id floor's NVS persistence
 *     (csi_event_on_id_advance) and its boot restore
 *     (apply_event_id_floor_from_nvs, then csi_event_egress::begin, both
 *     before the modules register), and csi_integration::loop's
 *     flush_dismissals() before csi_mqtt::loop's pump on each main-loop
 *     pass;
 *   - csi_mqtt's tamper bridge: the real csi_event_wire body builder decides
 *     whether a row has one;
 *   - the esp_mqtt task: connect() flips the link up.
 * firmware/scripts/check_wap_event_egress.py holds the firmware's hook and
 * call sites to that glue (the hook only enqueues, after the privacy gate;
 * begin once, after the floor restore and before the modules register; the
 * pump once, on the loop task).
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
 * The rest pin the planner's rules on this device and the RAM hold's: no
 * broker or a changed broker means not owed, a reboot mid-backlog
 * republishes nothing and skips at most a stride, a dismissal line is never
 * replayed, a send failure keeps its row (on the card and in RAM), rows the
 * card does not keep wait in RAM in order (also while the card is not open
 * yet or briefly closed, for at most kCardWaitMs), ambient rows are never
 * held, the hook never blocks, and the card adapter (torn tails, short
 * writes, dismissal lines at the tail, a failed rewrite) keeps every row.
 * Built with -DEGRESS_BEFORE_REVIEW against the egress and card sources the
 * review fixes replaced, the late-card, closed-card, broker-change, ambient
 * and dismissal-wait scenarios fail.
 *
 * Both proofs build this file, with this tests_host's Makefile and stubs,
 * against older sketch sources. From the repo root, with <rev> the
 * extraction commit (13c862c: the pre-fix logic moved unchanged into
 * csi_event_egress.cpp, except that it now sends the tamper bridge when
 * the events body does not build) and <flag> EGRESS_PRE_FIX, or the first
 * fix (3fc1f93) and EGRESS_BEFORE_REVIEW:
 *   d=$(mktemp -d); git archive <rev> firmware | tar -x -C "$d"
 *   rm -rf "$d/firmware/projects/canary-wap/tests_host"
 *   git archive HEAD firmware/projects/canary-wap/tests_host | tar -x -C "$d"
 *   make -C "$d/firmware/projects/canary-wap/tests_host" test_wap_event_egress \
 *     CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Wpedantic -D<flag>"
 *   "$d/firmware/projects/canary-wap/tests_host/test_wap_event_egress"
 *
 * What it does not pin: the real SD driver, esp_mqtt, FreeRTOS scheduling
 * (the host build compiles the chokepoint's locks out; the interleaving is
 * modeled by committing from inside a publish). Bench territory. */

#include "csi_event_egress.h"
#include "csi_event_id_floor.h"
#include "csi_event_log.h"
#include "csi_event_log_line.h"
#include "csi_event_wire.h"
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
#include <set>
#include <string>
#include <vector>

#if defined(EGRESS_BEFORE_REVIEW) || defined(EGRESS_PRE_FIX)
/* The older sources have no card wait; the scenarios drain as long as they
 * do against the fixed egress. */
namespace csi_event_egress { constexpr uint32_t kCardWaitMs = 45000; }
#endif

bool sd_mount_in_flight() { return false; }

static int g_fail = 0;
/* Ids handed over with NVS not past them, over every scenario (F47); main()
 * checks it is zero at the end. */
static int g_ceiling_violations_total = 0;
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
  uint32_t dest_epoch = 0;         /* csi_mqtt::destination_epoch(): a broker change moves it */
  bool dismiss_in_hook = false;    /* the owner dismisses each row before the hook copies it */
  int  fail_next_publishes = 0;
  std::set<uint32_t> unbuildable;  /* rows whose events body never builds */
  int  unbuildable_tries = 0;
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
uint32_t destination_epoch() { return W.dest_epoch; }
bool take_backfill_request() {
  const bool r = W.backfill_request;
  W.backfill_request = false;
  return r;
}
EventSend publish_event_row(const csi_event_record_t& rec, uint16_t, bool replay) {
  if (W.in_hook) W.publishes_in_hook++;
  /* The body is built before the link is looked at (csi_mqtt.cpp). */
  if (W.unbuildable.count(rec.event_id)) { W.unbuildable_tries++; return EventSend::kUnbuildable; }
  if (!W.connected) return EventSend::kNotNow;
  if (W.fail_next_publishes > 0) { W.fail_next_publishes--; return EventSend::kNotNow; }
  /* NVS must already hold a delivery ceiling above the id (F47), unless
   * NVS refuses writes. */
  if (!host_nvs().fail_puts && nvs_get(NVS_KEY_DELIVERED) <= rec.event_id) {
    W.ceiling_violations++;
    g_ceiling_violations_total++;
  }
  W.wire.push_back(rec.event_id);
  W.ha.receive(rec.event_id, replay);
  if (W.after_publish) {
    auto f = W.after_publish;
    W.after_publish = nullptr;
    f(rec.event_id);
  }
  return EventSend::kSent;
}
bool publish_tamper_bridge(const char* module_id, const char* type_name,
                           const csi_event_values_t* values) {
  /* csi_mqtt.cpp: no bridge body, no publish. */
  char body[128];
  if (!csi_event_wire::build_tamper_bridge_body(body, sizeof(body), module_id, type_name, values)) {
    return false;
  }
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
  /* The HTTP task's dismiss handler, landing between the ring write and
   * the egress's copy of the row. */
  if (W.dismiss_in_hook && csi_event_dismiss(event_id)) (void)csi_event_log::queue_dismissal(event_id);
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
/* As tamper_events_module.cpp declares it: system.integrity's "tamper". */
static const csi_event_decl_t TAMPER_EVENTS[] = {
  { "tamper", CSI_FIELD_STATE_NAME | CSI_FIELD_TIME_BUCKET, CSI_PRIVACY_P0, 0 },
};
static const csi_module_t TAMPER_MODULE = {
  "system.integrity", CSI_PRIVACY_P0, TAMPER_EVENTS, 1, noop_init, noop_tick, nullptr, nullptr,
};
/* An ambient module, as wifi.channel_activity: CSI_CATEGORY_AMBIENT rows
 * bypass the bundler and commit straight into the ring. */
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
  strcpy(v.note, "p");
  return csi_event_emit("test.egress", "ping", &v);
}

[[maybe_unused]] static uint32_t emit_ambient() {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_AMBIENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_TIME_BUCKET | CSI_FIELD_MOTION_SCORE;
  strcpy(v.state_name, "channel_active");
  v.motion_score = 40;
  return csi_event_emit("test.ambient", "channel_active", &v);
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
  SD.short_write_next = 0;
  SD.fail_renames = false;
  if (card) SD.dirs.insert("/EVENTS");
  W = World();
  boot();
}

static void connect() {
  W.connected = true;
  W.backfill_request = true;   /* MQTT_EVENT_CONNECTED */
}

/* One main-loop pass: csi_integration::loop writes queued dismissals, then
 * csi_mqtt::loop pumps the egress (canary_wap.ino's order). */
static constexpr uint32_t kPassMs = 50;
static void loop_pass() {
  stub_millis() += kPassMs;
  (void)csi_event_log::flush_dismissals();
  csi_event_egress::pump();
}

/* Enough passes to outlast the egress's card wait (kCardWaitMs). */
static constexpr int kDrainPasses = (int)(csi_event_egress::kCardWaitMs / kPassMs) + 100;

static void drain(int passes = kDrainPasses) {
  for (int i = 0; i < passes; ++i) loop_pass();
}

[[maybe_unused]] static bool rising(const std::vector<uint32_t>& ids) {
  for (size_t i = 1; i < ids.size(); ++i) if (ids[i] <= ids[i - 1]) return false;
  return true;
}

static bool exactly(const std::vector<uint32_t>& got, const std::vector<uint32_t>& want) {
  return got == want;
}

[[maybe_unused]] static bool has(const std::vector<uint32_t>& v, uint32_t id) {
  return std::find(v.begin(), v.end(), id) != v.end();
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
  CHECK(!W.ha.replay.empty() && W.ha.replay.back() == false,
        "the window row, committed with the link up, is news: replay:false");
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
  const uint32_t t = commit_closed_bundle("system.integrity", "tamper", "sd_removed");
  loop_pass();
  CHECK(W.tamper_bridges == 1, "the bridge went on the first pass");
  CHECK(!has(W.ha.accepted, t), "its events row waits behind the backlog");
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

static void test_a_burst_drains_in_two_passes() {
  printf("-- a full queue's burst (a bundler tick closing every slot, a stall) drains in two passes\n");
  fresh_device();
  connect();
  loop_pass();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 16; ++i) ids.push_back(emit_ping());   // the loop task stalls
  loop_pass();
  CHECK(W.ha.accepted.size() == 8, "one pass takes eight rows off the queue (kPumpBudget)");
  loop_pass();
  CHECK(exactly(W.ha.accepted, ids), "the next takes the rest, in order");
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
  (void)commit_closed_bundle(); loop_pass();   // link down: waits in RAM behind the card rows
  boot();                                      // power cycle: the RAM row is gone
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids), "the card's rows are still owed after the reboot, and arrive");
}

/* With the link up the walk's own hand-over writes the ceiling up to kStride
 * ids ahead of the row it sends (F47's trade: a reboot mid-backfill can
 * skip up to kStride rows). A row held in RAM must not add to that: handed
 * to the planner while card rows wait, it would write the ceiling past the
 * whole backlog. */
static size_t card_rows_delivered(const std::vector<uint32_t>& ids) {
  size_t n = 0;
  for (uint32_t id : ids) if (has(W.ha.accepted, id)) n++;
  return n;
}

static void test_held_row_with_the_link_up_skips_at_most_a_stride() {
  printf("-- a row held behind a long backlog with the link up, then a reboot: at most a stride skipped\n");
  for (int early = 0; early < 2; ++early) {
    fresh_device();
    std::vector<uint32_t> ids;
    const int backlog = early ? 30 : 20;
    for (int i = 0; i < backlog; ++i) { ids.push_back(emit_ping()); loop_pass(); }
    connect();
    if (early) loop_pass();                      // the walk sends its first rows
    (void)commit_closed_bundle(); loop_pass();   // waits in RAM behind the card
    boot();
    connect();
    drain();
    CHECK(card_rows_delivered(ids) + csi_event_id_floor::kStride >= ids.size(),
          early ? "a bundle early in a 30-row walk: at most kStride card rows skipped"
                : "a bundle before the first pass over 20 rows: at most kStride card rows skipped");
    CHECK(W.ha.refused.empty() && rising(W.wire), "and HA refused none");
  }
}

static void test_ram_row_then_card_rows_then_reboot() {
  printf("-- a row in RAM, then card rows, link down, then a reboot: the card rows are owed\n");
  fresh_device();
  std::vector<uint32_t> ids;
  (void)commit_closed_bundle(); loop_pass();   // nothing on the card yet, link down: RAM
  for (int i = 0; i < 5; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  boot();
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids), "all five card rows arrive after the reboot");
}

static void test_failed_append_waits_behind_a_ram_row() {
  printf("-- a row whose append fails does not overtake a row waiting in RAM\n");
  fresh_device();
  std::vector<uint32_t> ids;
  ids.push_back(commit_closed_bundle()); loop_pass();   // link down: RAM
  connect();
  SD.fail_writes = true;
  ids.push_back(emit_ping());                           // in the reconnect window; its append fails
  loop_pass();
  SD.fail_writes = false;
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "the RAM row first, then the failed-append row, each once");
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

static std::string card_line(uint32_t id, bool dismissed = false) {
  csi_event_record_t r;
  memset(&r, 0, sizeof(r));
  r.event_id = id;
  r.category = CSI_CATEGORY_EVENT;
  r.privacy = CSI_PRIVACY_P0;
  r.bundled_count = 1;
  strcpy(r.module_id, "test.egress");
  strcpy(r.type_name, "ping");
  strcpy(r.values.note, "p");
  r.values.dismissed = dismissed ? 1 : 0;
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
  /* Just below the allocator's next id: a line this device could have
   * written, so it is sent like any other. */
  SD.files["/EVENTS/today.ndjson"] += card_line(next - 1);
  csi_event_log::test_rearm_load();   // the card is opened again on this boot
  connect();
  drain();
  ids.push_back(next - 1);
  CHECK(exactly(W.ha.accepted, ids),
        "the real rows and the line below the allocator's next id go out; the forged ids do not");
  CHECK(csi_event_egress::watermark() < next, "and the watermark never takes them");
  CHECK(csi_event_get_next_event_id() == next, "and the walk allocated no id");
}

static void test_upgrade_floor_without_a_ceiling() {
  printf("-- an upgrade's first boot (an id floor in NVS, no delivery ceiling): the card's older rows count as delivered\n");
  fresh_device();
  const uint32_t floor = csi_event_id_floor::kIdSpaceBase + 0x10000u;
  /* An earlier image persisted the floor and may have published the rows
   * below it; it wrote no ceiling. */
  host_nvs().u32.clear();
  host_nvs().u32["csi/ev.next"] = floor;
  SD.files["/EVENTS/today.ndjson"] = card_line(floor - 0x200u) + card_line(floor - 0x100u);
  boot();
  connect();
  drain();
  CHECK(W.wire.empty(), "rows below the restored floor are not replayed into HA's gate");
  const uint32_t d = emit_ping();
  drain(4);
  CHECK(d >= floor && exactly(W.ha.accepted, {d}), "and the boot's first row goes out live");
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

/* The parsed lines of the card's log for `id`: their dismissed flags. */
static std::vector<int> card_flags_for(uint32_t id) {
  std::vector<int> out;
  const std::string& log = SD.files["/EVENTS/today.ndjson"];
  size_t p = 0;
  while (p < log.size()) {
    const size_t e = log.find('\n', p);
    if (e == std::string::npos) break;
    csi_event_record_t r;
    if (csi_event_log_line::parse(log.substr(p, e - p).c_str(), &r) && r.event_id == id) {
      out.push_back(r.values.dismissed);
    }
    p = e + 1;
  }
  return out;
}

static void test_dismissed_inside_the_hook_is_logged_as_the_original() {
  printf("-- a row dismissed before the hook copies it is logged, and replayed, as the original\n");
  fresh_device();
  loop_pass();                      // the card is open
  W.dismiss_in_hook = true;
  const uint32_t x = emit_ping();   // link down: the ring row is dismissed before the copy
  W.dismiss_in_hook = false;
  loop_pass();
  const uint32_t y = emit_ping(); loop_pass();
  const std::vector<int> flags = card_flags_for(x);
  CHECK(std::count(flags.begin(), flags.end(), 0) == 1 && std::count(flags.begin(), flags.end(), 1) == 1,
        "the card holds x's original (dismissed:0) and its dismissal line (dismissed:1)");
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, {x, y}) && W.wire.size() == 2,
        "x is replayed once, as the original, then y");
  boot();
  csi_event_record_t r;
  CHECK(csi_event_log::load_into_ring() >= 1 && csi_event_find(x, &r) && r.values.dismissed == 1,
        "after a reboot x comes back dismissed");
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

/* ── The RAM hold's failure paths ──────────────────────────────────────── */

static void test_held_flush_publish_failure_is_retried() {
  printf("-- a row flushed from the RAM hold whose publish fails stays held and goes next pass\n");
  fresh_device(/*card=*/false);
  std::vector<uint32_t> ids;
  for (int i = 0; i < 3; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  drain();                       // no card ever comes: the card wait ends, the link is still down
  connect();
  W.fail_next_publishes = 1;     // the first flush's publish fails
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "every held row arrives once, in id order");
}

static void test_held_merge_publish_failure_is_retried() {
  printf("-- a RAM row merged into the card walk whose publish fails is retried before the card row\n");
  fresh_device();
  std::vector<uint32_t> ids;
  ids.push_back(commit_closed_bundle()); loop_pass();   // link down: RAM
  ids.push_back(emit_ping()); loop_pass();              // link down: the card
  connect();
  W.fail_next_publishes = 1;                            // the merged RAM row's publish fails
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "the RAM row, then the card row, each once");
}

static void test_unbuildable_rows_are_skipped_not_stalled() {
  printf("-- a row whose body never builds is passed over, wherever it waits, and nothing stalls\n");
  fresh_device();
  std::vector<uint32_t> ids;
  ids.push_back(emit_ping()); loop_pass();
  const uint32_t bad_card = emit_ping(); loop_pass();          // on the card
  ids.push_back(emit_ping()); loop_pass();
  const uint32_t bad_held = commit_closed_bundle(); loop_pass();   // in RAM, merged into the walk
  ids.push_back(emit_ping()); loop_pass();
  const uint32_t bad_flushed = commit_closed_bundle(); loop_pass();   // in RAM, flushed after the walk
  W.unbuildable = {bad_card, bad_held, bad_flushed};
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids), "every other row arrives once, in id order");
  const uint32_t bad_live = commit_closed_bundle();             // nothing waits: the live route
  W.unbuildable.insert(bad_live);
  const uint32_t after = emit_ping();
  drain(4);
  CHECK(W.ha.accepted.back() == after && !has(W.wire, bad_live),
        "a live row whose body never builds does not hold the next one");
  CHECK(W.unbuildable_tries == 4, "each was tried once, never retried");
}

/* ── A card that is not open (yet, or for a moment) ────────────────────── */

static void test_late_card_mount_holds_new_rows() {
  printf("-- a reboot with rows on a card that mounts late: a row committed first waits for them\n");
  for (int bundle = 0; bundle < 2; ++bundle) {
    fresh_device();
    std::vector<uint32_t> ids;
    for (int i = 0; i < 5; ++i) { ids.push_back(emit_ping()); loop_pass(); }
    SD.present = false;           // power cycle; the card has not mounted yet
    boot();
    connect();
    loop_pass();
    const uint32_t r = bundle ? commit_closed_bundle() : emit_ping();
    ids.push_back(r);
    drain(4);
    CHECK(W.ha.accepted.empty(), bundle ? "the bundle waits in RAM for the card"
                                        : "the direct row waits in RAM for the card");
    SD.present = true;            // the mount worker's result is adopted
    drain();
    CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
          bundle ? "the card's rows, then the bundle, each once"
                 : "the card's rows, then the direct row, each once");
  }
}

static void test_card_closed_mid_backfill_holds_new_rows() {
  printf("-- the card closes for a moment mid-backfill: rows committed meanwhile wait for it\n");
  for (int bundle = 0; bundle < 2; ++bundle) {
    fresh_device();
    std::vector<uint32_t> ids;
    for (int i = 0; i < 10; ++i) { ids.push_back(emit_ping()); loop_pass(); }
    connect();
    for (int i = 0; i < 2; ++i) loop_pass();   // the walk starts
    SD.present = false;                        // SD_ERROR -> SD.end(), or a remount in flight
    loop_pass();
    ids.push_back(bundle ? commit_closed_bundle() : emit_ping());
    drain(4);
    SD.present = true;
    drain();
    CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
          bundle ? "every card row, then the bundle, each once"
                 : "every card row, then the row committed while it was out, each once");
  }
}

static void test_card_wait_is_bounded() {
  printf("-- a card that does not come back: rows in RAM wait kCardWaitMs, then go\n");
  fresh_device(/*card=*/false);  // a device with no card at all
  connect();
  loop_pass();
  const uint32_t a = emit_ping();
  drain(4);
  CHECK(W.ha.accepted.empty(), "a row committed just after boot waits for a card that may mount");
  drain();
  CHECK(exactly(W.ha.accepted, {a}), "and goes once the wait is over");
  const uint32_t b = emit_ping();
  drain(4);
  CHECK(W.ha.accepted.back() == b, "after that, rows go live");
  /* a card closed mid-backfill that stays out */
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 10; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  connect();
  for (int i = 0; i < 2; ++i) loop_pass();
  SD.present = false;
  loop_pass();
  const uint32_t r = commit_closed_bundle();
  drain(4);
  CHECK(!has(W.ha.accepted, r), "the bundle waits while the card may come back");
  drain();
  CHECK(has(W.ha.accepted, r) && W.ha.refused.empty(),
        "past kCardWaitMs it goes; the rows still on the card are given up");
}

/* ── A changed broker ──────────────────────────────────────────────────── */

static void test_broker_change_drops_the_backlog() {
  printf("-- a broker changed during an outage is not sent what waited for the old one\n");
  fresh_device();
  for (int i = 0; i < 3; ++i) { emit_ping(); loop_pass(); }
  (void)commit_closed_bundle(); loop_pass();
  W.dest_epoch++;                // a config POST names another broker
  loop_pass();
  connect();
  drain();
  CHECK(W.wire.empty(), "neither the card's nor RAM's rows go to the new broker");
  const uint32_t d = emit_ping();
  drain(4);
  CHECK(exactly(W.ha.accepted, {d}), "the next row goes out live");
  /* the card had not opened yet this boot when the broker changed */
  fresh_device();
  for (int i = 0; i < 3; ++i) { emit_ping(); loop_pass(); }
  SD.present = false;
  boot();
  loop_pass();
  W.dest_epoch++;
  loop_pass();
  SD.present = true;
  connect();
  drain();
  CHECK(W.wire.empty(), "a card that opens after the change is not replayed to the new broker either");
}

#ifndef EGRESS_BEFORE_REVIEW
static void test_destination_digest() {
  printf("-- the destination digest: host, port, user and prefix move it; a password or TLS change does not\n");
  csi_mqtt::Config a;
  memset(&a, 0, sizeof(a));
  a.enabled = true;
  strcpy(a.host, "broker.local");
  a.port = 1883;
  strcpy(a.user, "canary");
  strcpy(a.prefix, "securacv");
  const uint32_t d = csi_mqtt::destination_digest(a);
  csi_mqtt::Config b = a;
  strcpy(b.pass, "rotated");
  b.tls = true;
  b.tls_mode = 1;
  CHECK(csi_mqtt::destination_digest(b) == d, "a password or TLS change is the same destination");
  b = a; strcpy(b.host, "broker2.local");
  CHECK(csi_mqtt::destination_digest(b) != d, "another host is another destination");
  b = a; b.port = 8883;
  CHECK(csi_mqtt::destination_digest(b) != d, "another port");
  b = a; strcpy(b.user, "other");
  CHECK(csi_mqtt::destination_digest(b) != d, "another user");
  b = a; strcpy(b.prefix, "home");
  CHECK(csi_mqtt::destination_digest(b) != d, "another topic prefix");
  b = a; strcpy(b.host, "broker.localc"); strcpy(b.user, "anary");
  CHECK(csi_mqtt::destination_digest(b) != d, "fields do not run into each other");
}
#endif

/* ── Ambient rows ──────────────────────────────────────────────────────── */

static void test_ambient_rows_are_not_held() {
  printf("-- no card, an outage: ambient rows are not held, so they never evict a real event\n");
  fresh_device(/*card=*/false);
  const uint32_t real = commit_closed_bundle(); loop_pass();
  int ambient = 0;
  for (int i = 0; i < 8; ++i) {
    if (emit_ambient()) ambient++;
    for (int k = 0; k < 100; ++k) loop_pass();   // one spike per cooldown
  }
  CHECK(ambient == 8, "eight ambient rows committed during the outage");
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, {real}), "the presence row arrives; no ambient row is replayed");
  CHECK(csi_event_egress::stats().held_dropped == 0, "the hold dropped nothing");
#ifndef EGRESS_BEFORE_REVIEW
  CHECK(csi_event_egress::stats().ambient_dropped == 8, "the ambient rows were dropped, counted");
#endif
  const uint32_t live = emit_ambient();
  drain(4);
  CHECK(live != 0 && W.ha.accepted.back() == live, "with the link up an ambient row goes live");
}

/* ── The card adapter (csi_event_log.cpp) ──────────────────────────────── */

static void test_torn_tail_at_open_is_sealed() {
  printf("-- a log that ends in a torn line: the next row is sealed onto a line of its own\n");
  fresh_device();
  std::vector<uint32_t> ids;
  for (int i = 0; i < 2; ++i) { ids.push_back(emit_ping()); loop_pass(); }
  SD.files["/EVENTS/today.ndjson"] += "{\"id\":32212";   // a power cut mid-line
  boot();
  ids.push_back(emit_ping()); loop_pass();           // offline: onto the card
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "the rows before the fragment and the row after it each arrive once");
}

static void test_short_write_is_sealed() {
  printf("-- a short write leaves a torn line: the row waits in RAM, the next row seals it\n");
  fresh_device();
  std::vector<uint32_t> ids;
  ids.push_back(emit_ping()); loop_pass();
  SD.short_write_next = 20;
  ids.push_back(emit_ping()); loop_pass();           // its line lands torn
  ids.push_back(emit_ping()); loop_pass();
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(),
        "every row arrives once, in id order, the torn one from RAM");
}

static void test_tail_dismissal_does_not_hide_unsent_rows() {
  printf("-- dismissal lines at the end of the log do not hide the unsent rows before them\n");
  for (int many = 0; many < 2; ++many) {
    fresh_device();
    connect();
    loop_pass();
    std::vector<uint32_t> sent;
    for (int i = 0; i < 6; ++i) { sent.push_back(emit_ping()); loop_pass(); }   // delivered live
    W.connected = false;
    std::vector<uint32_t> unsent;
    for (int i = 0; i < 16; ++i) { unsent.push_back(emit_ping()); loop_pass(); }
    /* The owner dismisses delivered rows: their dismissal lines end the log,
     * one of them, or more than one tail-scan window's worth (768 bytes). */
    const int dismissals = many ? 6 : 1;
    size_t tail_bytes = 0;
    for (int i = 0; i < dismissals; ++i) {
      CHECK(csi_event_dismiss(sent[i]) && csi_event_log::queue_dismissal(sent[i]), "dismissed");
      const size_t before = SD.files["/EVENTS/today.ndjson"].size();
      loop_pass();
      tail_bytes += SD.files["/EVENTS/today.ndjson"].size() - before;
    }
    if (many) CHECK(tail_bytes > 768, "the dismissal lines fill more than one tail-scan window");
    boot();
    const uint32_t restored = csi_event_egress::watermark();
    std::vector<uint32_t> owed;
    for (uint32_t id : unsent) if (id > restored) owed.push_back(id);
    CHECK(!owed.empty(), "rows above the restored watermark are owed");
    W.ha = Ha();
    W.ha.has_mark = true;
    W.ha.mark = sent.back();
    connect();
    drain();
    CHECK(exactly(W.ha.accepted, owed) && W.ha.refused.empty(),
          many ? "past a window of dismissal lines, every owed row arrives once, in order"
               : "past one dismissal line, every owed row arrives once, in order");
  }
}

static void test_idle_passes_read_nothing() {
  printf("-- an open card with nothing waiting is not re-read every pass\n");
  fresh_device();
  for (int i = 0; i < 5; ++i) { emit_ping(); loop_pass(); }
  connect();
  drain();
  const size_t before = fake_sd_bytes_read();
  drain(50);
  CHECK(fake_sd_bytes_read() == before, "fifty idle passes read no byte of the card");
}

static void test_broken_rewrite_closes_the_log() {
  printf("-- a retention rewrite whose rename fails closes the log until the card is pulled\n");
  fresh_device();
  /* A log at MAX_BYTES of dismissal lines (never replayed, never a tail row). */
  std::string big;
  uint32_t id = 0xC0000000u - 100000u;
  while (big.size() < csi_event_log::MAX_BYTES) big += card_line(id++, /*dismissed=*/true);
  SD.files["/EVENTS/today.ndjson"] = big;
  csi_event_log::test_rearm_load();
  loop_pass();                                       // the card opens
  SD.fail_renames = true;
  std::vector<uint32_t> ids;
  ids.push_back(emit_ping()); loop_pass();           // the cut's rename fails
  ids.push_back(emit_ping()); loop_pass();
  CHECK(SD.files.count("/EVENTS/today.ndjson") == 0 && SD.files.count("/EVENTS/today.ndjson.tmp") == 1,
        "the survivors wait in the .tmp file, and no fresh log is started over them");
  SD.fail_renames = false;
  SD.present = false; loop_pass();                   // pulled
  SD.present = true;  loop_pass();                   // reinserted: the mount's reconcile
  CHECK(SD.files.count("/EVENTS/today.ndjson") == 1 &&
        SD.files["/EVENTS/today.ndjson"].size() >= csi_event_log::MAX_BYTES / 2,
        "the reinserted card's reconcile brings the survivors back as the log");
  connect();
  drain();
  CHECK(exactly(W.ha.accepted, ids) && W.ha.refused.empty(), "the two rows arrive from RAM, in order");
}
#endif  // !EGRESS_PRE_FIX

int main() {
  Serial.quiet = true;
  csi_event_test_reset();
  csi_module_register(&MODULE);
  csi_module_register(&TAMPER_MODULE);
  csi_module_register(&AMBIENT_MODULE);

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
  test_a_burst_drains_in_two_passes();
  test_ram_row_goes_before_a_newer_card_row();
  test_held_row_does_not_raise_the_ceiling_past_the_card();
  test_held_row_with_the_link_up_skips_at_most_a_stride();
  test_ram_row_then_card_rows_then_reboot();
  test_failed_append_waits_behind_a_ram_row();
  test_dismissal_line_ahead_of_unsent_rows();
  test_failed_append_waits_behind_the_backlog();
  test_unconfigured_broker_drops_the_backlog();
  test_forged_card_line_is_never_sent();
  test_upgrade_floor_without_a_ceiling();
  test_dismissal_written_before_its_original();
  test_dismissed_inside_the_hook_is_logged_as_the_original();
  test_nvs_failure_still_delivers();
  test_held_flush_publish_failure_is_retried();
  test_held_merge_publish_failure_is_retried();
  test_unbuildable_rows_are_skipped_not_stalled();
  test_late_card_mount_holds_new_rows();
  test_card_closed_mid_backfill_holds_new_rows();
  test_card_wait_is_bounded();
  test_broker_change_drops_the_backlog();
#ifndef EGRESS_BEFORE_REVIEW
  test_destination_digest();
#endif
  test_ambient_rows_are_not_held();
  test_torn_tail_at_open_is_sealed();
  test_short_write_is_sealed();
  test_tail_dismissal_does_not_hide_unsent_rows();
  test_idle_passes_read_nothing();
  test_broken_rewrite_closes_the_log();
#endif

  CHECK(g_ceiling_violations_total == 0,
        "in every scenario, each id was under the NVS ceiling before it went (F47)");

  if (g_fail == 0) {
    printf("ALL wap event egress tests PASSED\n");
    return 0;
  }
  printf("%d FAILED\n", g_fail);
  return 1;
}
