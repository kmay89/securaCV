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
  uint32_t at_ms;
  char state[CSI_EVENT_NAME_MAX];
  uint16_t bundled;
  uint16_t duration_sec;
};
static std::vector<Row> g_presence_rows;

extern "C" void csi_event_on_committed(uint32_t, const char* module_id, const char*,
                                       csi_event_category_t, csi_privacy_class_t,
                                       const csi_event_values_t* v) {
  if (!module_id || std::strcmp(module_id, "core.presence") != 0 || !v) return;
  Row r;
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

int main() {
  if (test_a_refresh_inside_the_window_commits_nothing_and_spends_nothing()) return 1;
  if (test_a_closed_bundle_commits_once()) return 1;
  if (test_a_window_closes_into_one_row_and_reopens()) return 1;
  if (test_a_bundle_commits_while_csi_is_shed()) return 1;
  std::printf("test_csi_modules_integration: %d checks passed\n", g_checks);
  return 0;
}
