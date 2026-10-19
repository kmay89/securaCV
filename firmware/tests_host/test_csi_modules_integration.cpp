// Host tests for the canary's CSI bundling (sweep F81), on the canary's
// REAL module bridge: canary/src/csi_modules_integration.cpp, compiled as
// the PlatformIO envs compile it (no BLE Scout, no mesh), over the real
// common CSI library and the modules it registers, under a fake clock.
//
// The canary used to call csi_event_flush_bundles() after every CSI window.
// That closes EVERY open bundle, so no bundle ever saw a second observation:
// core.presence's refreshes (5, 20 and every 60 windows into a state) each
// committed a row of their own and each spent the module's 6/hour ceiling,
// and after about three minutes in one state its next real transition was
// dropped. Now the feed closes nothing, and securacv_csi_modules_tick(),
// called once per main loop, closes only the bundles that are due (their
// 10-minute window or their 2-minute quiet gap), as the canary-wap does.
//
// The test plays a stand-in for main.cpp's loop: one CSI window a second
// through securacv_csi_modules_feed(), then securacv_csi_modules_tick().
// Rows are what csi_event_on_committed sees (the egress queue's input on a
// device). It pins the bridge's split (the feed closes nothing, the tick
// closes what is due); main.cpp itself is compiled by CI, not here, and
// firmware/scripts/check_csi_bundle_tick.py holds its loop() to calling the
// tick once, outside the CSI power and degrade gates, before the egress pump.
//
// Each test fails on the bridge before F81 (built with a no-op
// securacv_csi_modules_tick, which is what main.cpp ran there: nothing).
// test_a_closed_bundle_reaches_the_ring fails on the library before sweep
// F77, where a closed bundle committed without a ring row.
// test_a_dismissed_presence_row_nudges_its_module_once fails on the library
// before F77's review, which dispatched every dismiss of a row to its module.
//
// Backlog F90, on the same bridge and clock: a state held one, two or three
// hours (plus 0, 7, 133 or 301 s of phase) reopens its
// bundle every ten minutes, six an hour, core.presence's whole ceiling. The
// library checked the ceiling before the bundler, so after about an hour
// the held state's own refreshes were refused and the walk-out waited for a
// slot to age out. Now a refresh of the open bundle is never refused and
// the ceiling's last slot is kept for a change of state:
// test_a_held_state_keeps_its_refreshes_and_the_way_out fails on the
// library before F90 and passes on it.
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "Arduino.h"
#include "csi_bundler.h"
#include "csi_event.h"
#include "csi_hal.h"
#include "csi_modules_integration.h"
#include "core_presence.h"

static int g_checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                         \
    }                                                                   \
    ++g_checks;                                                         \
  } while (0)

// ── Time: one clock for millis() and the library's CLOCK_MONOTONIC ──────
uint32_t g_host_millis = 5000000;
HostSerial Serial;
extern "C" int clock_gettime(clockid_t, struct timespec* ts) noexcept {
  ts->tv_sec = (time_t)(g_host_millis / 1000);
  ts->tv_nsec = (long)((g_host_millis % 1000) * 1000000);
  return 0;
}

// ── The HAL the bridge talks to (its watchdog hook only) ────────────────
namespace csi_hal {
bool start() { return true; }
void stop() {}
void set_watchdog(uint32_t, WatchdogCallback) {}
}  // namespace csi_hal

// ── Rows: what the canary's egress override would queue ─────────────────
struct Row {
  uint32_t id;
  uint32_t at_ms;
  char state[CSI_EVENT_NAME_MAX];
  uint16_t bundled;
  uint16_t duration_sec;
};
static std::vector<Row> g_presence_rows;

extern "C" void csi_event_on_committed(uint32_t event_id, const char* module_id, const char*,
                                       csi_event_category_t, csi_privacy_class_t,
                                       const csi_event_values_t* v) {
  if (!module_id || std::strcmp(module_id, "core.presence") != 0 || !v) return;
  Row r;
  r.id = event_id;
  r.at_ms = g_host_millis;
  std::memset(r.state, 0, sizeof(r.state));
  std::strncpy(r.state, v->state_name, sizeof(r.state) - 1);
  r.bundled = v->bundled_count;
  r.duration_sec = v->duration_sec;
  g_presence_rows.push_back(r);
}

static size_t rows_of(const char* state) {
  size_t n = 0;
  for (const Row& r : g_presence_rows) n += std::strcmp(r.state, state) == 0;
  return n;
}
static const Row* only_row_of(const char* state) {
  const Row* found = nullptr;
  for (const Row& r : g_presence_rows) {
    if (std::strcmp(r.state, state) != 0) continue;
    if (found) return nullptr;
    found = &r;
  }
  return found;
}

// ── Windows ─────────────────────────────────────────────────────────────
// core.presence reads motion as the mean of the four phase-Doppler bands
// v[8..11] (80 is past the default "active" threshold, 75, and below the
// "together" gate, 95) and rejects a window whose RSSI max/min (v[22]/v[23])
// swing without Doppler. Equal RSSI stats: no swing.
static csi_features_t window_of(int8_t doppler) {
  csi_features_t f;
  std::memset(&f, 0, sizeof(f));
  for (int i = 8; i < 12; ++i) f.v[i] = doppler;
  f.v[20] = -50;
  f.v[22] = -50;
  f.v[23] = -50;
  f.frames_in_window = 20;
  return f;
}
static const csi_features_t kActive = window_of(80);
static const csi_features_t kEmpty = window_of(0);

// A stand-in for main.cpp's loop, one pass per CSI window (1 Hz).
static void loop_pass(const csi_features_t* f) {
  g_host_millis += 1000;
  if (f) securacv_csi_modules_feed(f);   // nullptr: CSI shed, no window
  securacv_csi_modules_tick();
}

// The open core.presence bundle of `state`, from the bundler's own view.
static bool open_bundle(const char* state, csi_event_record_t* out) {
  csi_event_record_t rows[8];
  const size_t n = csi_bundler_snapshot_open(rows, 8);
  for (size_t i = 0; i < n; ++i) {
    if (std::strcmp(rows[i].module_id, "core.presence") == 0 &&
        std::strcmp(rows[i].values.state_name, state) == 0) {
      *out = rows[i];
      return true;
    }
  }
  return false;
}

// A device fresh out of boot: chokepoint, bundler and presence state reset,
// modules registered by the bridge (a re-register keeps the first).
static void fresh() {
  csi_event_test_reset();
  core_presence_module()->deinit();   // state EMPTY, streak 0
  (void)securacv_csi_modules_init();
  g_presence_rows.clear();
}

// Walk into the room: active windows until core.presence opens its
// "active" bundle (two windows of hysteresis). Returns the opening time:
// the open bundle's, or, where the window that opened it also closed it
// (the bridge before F81), its row's.
static uint32_t walk_in() {
  csi_event_record_t open;
  for (int i = 0; i < 10; ++i) {
    loop_pass(&kActive);
    if (open_bundle("active", &open)) return open.first_seen_ms;
    if (rows_of("active") != 0) return g_host_millis;
  }
  return 0;
}

static uint32_t emit_probe(const char* state) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME;
  std::snprintf(v.state_name, sizeof(v.state_name), "%s", state);
  return csi_event_emit("core.presence", "presence_changed", &v);
}

// Nine minutes and fifty seconds in one state, inside its 10-minute
// window: core.presence refreshes it eleven times (at 5, 20, 60, 120, ...,
// 540 windows). None of that commits a row, and none of it spends the
// 6/hour ceiling: the opening spent one slot, so five new states can still
// open. Before F81 the canary committed a row per refresh and its ceiling
// was spent by the sixth.
static int test_a_refresh_inside_the_window_commits_nothing_and_spends_nothing() {
  fresh();
  const uint32_t opened = walk_in();
  CHECK(opened != 0);
  while (g_host_millis - opened < 590 * 1000) loop_pass(&kActive);

  CHECK(g_presence_rows.empty());
  csi_event_record_t open;
  CHECK(open_bundle("active", &open));
  CHECK(open.first_seen_ms == opened);       // still the first bundle
  CHECK(open.bundled_count == 1 + 11);       // the opening and eleven refreshes

  int admitted = 0;
  for (int i = 0; i < 5; ++i) {
    char s[16];
    std::snprintf(s, sizeof(s), "probe%d", i);
    if (emit_probe(s) != 0) ++admitted;
  }
  CHECK(admitted == 5);
  CHECK(emit_probe("probe5") == 0);          // the seventh opening is over
  return 0;
}

// Leave the room: the "active" refreshes stop, and the loop's tick closes
// its bundle once the 2-minute quiet gap has passed, as ONE row carrying
// every observation and the span from the opening to the last refresh.
// Before F81 the same five minutes were eight one-observation rows.
static int test_a_closed_bundle_commits_once() {
  fresh();
  const uint32_t opened = walk_in();
  CHECK(opened != 0);
  while (g_host_millis - opened < 300 * 1000) loop_pass(&kActive);
  csi_event_record_t open;
  CHECK(open_bundle("active", &open));
  const uint32_t last_seen = open.last_seen_ms;
  CHECK(last_seen - opened == 300 * 1000);   // the refresh at 300 windows
  CHECK(g_presence_rows.empty());

  for (int i = 0; i < 200; ++i) loop_pass(&kEmpty);

  CHECK(rows_of("active") == 1);
  const Row* row = only_row_of("active");
  CHECK(row != nullptr);
  CHECK(row->bundled == 1 + 7);              // opening + 5, 20, 60, ..., 300
  CHECK(row->duration_sec == 300);
  CHECK(row->at_ms >= last_seen + CSI_BUNDLER_MAX_GAP_MS);
  CHECK(row->at_ms < last_seen + CSI_BUNDLER_MAX_GAP_MS + 1000);
  CHECK(!open_bundle("active", &open));
  return 0;
}

// Sweep F77: the closed bundle enters the ring, so csi_event_recent() (the
// daily summary's walk; the canary-wap's /api/events/today) and
// csi_event_find() (what the canary-wap's egress logs on its card) see it.
// Its ring row ends at the close and starts duration_sec before it: here the
// gap close two minutes after the last refresh, so it starts two minutes
// after the bundle opened. Fails on the library before F77, where a closed
// bundle never reached the ring.
static int test_a_closed_bundle_reaches_the_ring() {
  fresh();
  const uint32_t opened = walk_in();
  CHECK(opened != 0);
  while (g_host_millis - opened < 300 * 1000) loop_pass(&kActive);
  for (int i = 0; i < 200; ++i) loop_pass(&kEmpty);
  const Row* row = only_row_of("active");
  CHECK(row != nullptr);

  csi_event_record_t rec;
  CHECK(csi_event_find(row->id, &rec));
  CHECK(std::strcmp(rec.values.state_name, "active") == 0);
  CHECK(rec.bundled_count == row->bundled && rec.values.duration_sec == 300);
  CHECK(rec.last_seen_ms == row->at_ms);                  // the close
  CHECK(rec.first_seen_ms == row->at_ms - 300u * 1000u);  // the close less the span
  CHECK(rec.first_seen_ms > opened);                      // by the quiet gap: not the opening

  csi_event_record_t recent[16];
  const size_t n = csi_event_recent(recent, 16);
  bool listed = false;
  for (size_t i = 0; i < n; ++i) listed = listed || recent[i].event_id == row->id;
  CHECK(listed);
  return 0;
}

// Stay in the room: the bundle closes on its 10-minute window, as one row,
// and the refresh that found it due opens the next. Before F81 the first
// ten minutes were six rows (and the ceiling refused the rest).
static int test_a_window_closes_into_one_row_and_reopens() {
  fresh();
  const uint32_t opened = walk_in();
  CHECK(opened != 0);
  while (g_host_millis - opened < 610 * 1000) loop_pass(&kActive);

  CHECK(rows_of("active") == 1);
  const Row* row = only_row_of("active");
  CHECK(row != nullptr);
  CHECK(row->bundled == 1 + 11);
  CHECK(row->duration_sec == 540);           // opening to the refresh at 540
  csi_event_record_t open;
  CHECK(open_bundle("active", &open));       // the next one is open
  CHECK(open.first_seen_ms >= opened + CSI_BUNDLER_WINDOW_MS);
  return 0;
}

// CSI shed (the power policy or a heap degrade stops csi::process(), so no
// window and no feed): the loop's tick still closes the open bundle on its
// quiet gap. A tick that ran only inside the feed would hold it open until
// CSI came back.
static int test_a_bundle_commits_while_csi_is_shed() {
  fresh();
  const uint32_t opened = walk_in();
  CHECK(opened != 0);
  while (g_host_millis - opened < 30 * 1000) loop_pass(&kActive);
  csi_event_record_t open;
  CHECK(open_bundle("active", &open));
  const uint32_t last_seen = open.last_seen_ms;   // the refresh at 20

  for (int i = 0; i < 200; ++i) loop_pass(nullptr);

  CHECK(rows_of("active") == 1);
  const Row* row = only_row_of("active");
  CHECK(row != nullptr);
  CHECK(row->bundled == 1 + 2);
  CHECK(row->at_ms >= last_seen + CSI_BUNDLER_MAX_GAP_MS);
  CHECK(row->at_ms < last_seen + CSI_BUNDLER_MAX_GAP_MS + 1000);
  return 0;
}

// F90: hold "active" for `hold_s` windows past walk_in(), then walk out.
// core.presence refreshes its held state at 5, 20 and every 60 windows; a
// refresh the loop pass makes while the bundle is still open (inside its
// window and its quiet gap, read from the bundler's own snapshot before the
// pass) must land in that bundle. The walk-out must open its "empty" bundle
// within core.presence's two windows of hysteresis.
static int hold_then_walk_out(uint32_t hold_s, uint32_t* refreshes, uint32_t* refused,
                              bool* walked_out) {
  fresh();
  *refreshes = 0;
  *refused = 0;
  const uint32_t opened = walk_in();
  CHECK(opened != 0);
  for (uint32_t w = 1; w <= hold_s; ++w) {
    csi_event_record_t before;
    const bool was_open = open_bundle("active", &before);
    loop_pass(&kActive);
    const bool refresh_window = (w == 5 || w == 20 || w % 60 == 0);
    if (!refresh_window || !was_open) continue;
    const bool mergeable = g_host_millis - before.first_seen_ms < CSI_BUNDLER_WINDOW_MS &&
                           g_host_millis - before.last_seen_ms < CSI_BUNDLER_MAX_GAP_MS;
    if (!mergeable) continue;
    ++*refreshes;
    csi_event_record_t after;
    if (!open_bundle("active", &after) || after.first_seen_ms != before.first_seen_ms ||
        after.bundled_count != before.bundled_count + 1) {
      ++*refused;
    }
  }
  loop_pass(&kEmpty);
  loop_pass(&kEmpty);                          // two windows of hysteresis
  csi_event_record_t out;
  *walked_out = open_bundle("empty", &out);
  return 0;
}

static int test_a_held_state_keeps_its_refreshes_and_the_way_out() {
  static const uint32_t kPhaseS[] = {0, 7, 133, 301};
  for (uint32_t hours = 1; hours <= 3; ++hours) {
    for (uint32_t phase : kPhaseS) {
      uint32_t refreshes = 0, refused = 0;
      bool walked_out = false;
      if (hold_then_walk_out(hours * 3600 + phase, &refreshes, &refused, &walked_out)) return 1;
      if (refused != 0 || !walked_out) {
        std::fprintf(stderr, "  hold %u h + %u s: %u of %u open-bundle refreshes refused, "
                     "walk-out %s\n", (unsigned)hours, (unsigned)phase, (unsigned)refused,
                     (unsigned)refreshes, walked_out ? "admitted" : "refused");
      }
      CHECK(refreshes > 0);
      CHECK(refused == 0);
      CHECK(walked_out);
    }
  }
  return 0;
}

// Sweep F77's review: a closed presence row is a ring row now, so a dismiss
// of it ("That was nothing" on the canary-wap's Today sheet) reaches
// core.presence's handler, which raises its motion and breathing thresholds
// a point. Only the first dismiss of a row does: a second press, or a
// client's retry, finds the row dismissed and nudges nothing. Read off the
// module's own classification: the motion threshold starts at 35, so a
// window of 35 opens a "subtle" bundle; one nudge makes it 36, so 35 no
// longer does and 36 still does. Fails on the library before the review
// (three dismisses, three nudges, 38: a window of 36 opens nothing). Runs
// last, since it leaves the thresholds nudged.
static int test_a_dismissed_presence_row_nudges_its_module_once() {
  fresh();
  csi_event_record_t open;
  const csi_features_t at35 = window_of(35);
  const csi_features_t at36 = window_of(36);
  for (int i = 0; i < 5; ++i) loop_pass(&at35);
  CHECK(open_bundle("subtle", &open));          // the threshold is 35
  const uint32_t opened = walk_in();
  CHECK(opened != 0);
  while (g_host_millis - opened < 300 * 1000) loop_pass(&kActive);
  for (int i = 0; i < 200; ++i) loop_pass(&kEmpty);
  CHECK(!open_bundle("subtle", &open) && !open_bundle("active", &open));
  const Row* row = only_row_of("active");
  CHECK(row != nullptr);
  const uint32_t id = row->id;

  CHECK(csi_event_dismiss(id));
  CHECK(csi_event_dismiss(id));                 // the button pressed again
  CHECK(csi_event_dismiss(id));                 // a client's retry
  csi_event_record_t rec;
  CHECK(csi_event_find(id, &rec) && rec.values.dismissed == 1);

  for (int i = 0; i < 5; ++i) loop_pass(&at35);
  CHECK(!open_bundle("subtle", &open));         // nudged: 35 is below the threshold
  for (int i = 0; i < 5; ++i) loop_pass(&at36);
  CHECK(open_bundle("subtle", &open));          // once: 36 still reaches it
  return 0;
}

int main() {
  if (test_a_refresh_inside_the_window_commits_nothing_and_spends_nothing()) return 1;
  if (test_a_closed_bundle_commits_once()) return 1;
  if (test_a_closed_bundle_reaches_the_ring()) return 1;
  if (test_a_window_closes_into_one_row_and_reopens()) return 1;
  if (test_a_bundle_commits_while_csi_is_shed()) return 1;
  if (test_a_held_state_keeps_its_refreshes_and_the_way_out()) return 1;
  if (test_a_dismissed_presence_row_nudges_its_module_once()) return 1;   // last: it nudges
  std::printf("test_csi_modules_integration: %d checks passed\n", g_checks);
  return 0;
}
