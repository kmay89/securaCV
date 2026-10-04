// Host tests for the CSI modules' boot init on the canary (sweep F93), on
// the canary's REAL module bridge (canary/src/csi_modules_integration.cpp,
// as the PlatformIO envs compile it: no BLE Scout, no mesh) over the real
// common CSI library and the modules it registers, with a fake NVS
// (stubs/csi_modules/Preferences.h) that outlives a modeled reboot.
//
// csi_module_register() only records a module. Before F93 nothing on the
// canary ever called a module's init(), so its csi_module_settings_*
// overrides were never read: a stored preset, threshold, pet mode or
// anomaly cooldown changed nothing. Now securacv_csi_modules_init() runs
// init_modules_from_nvs() after it registers the modules: every module's
// init(), once, through csi_module_init_all() and one read-only NVS handle
// for the whole boot. The library ticks no module before its init.
//
// A "boot" here is what main.cpp's setup() does for the CSI pipeline, in
// its order: RAM state gone, NVS kept; csi_event_egress_begin() restores the
// event-id floor (modeled: csi_event_set_event_id_floor); then
// securacv_csi_modules_init(). main.cpp itself is CI's to compile;
// check_event_egress_order.py (rule 8) holds it to that order.
//
// Against the canary before F93 (no boot init, a tick for every registered
// module) every test fails but two: the commit test (the old bridge ran no
// init, so nothing could commit there) and the key-map test, which guard
// the new boot path. The once-per-boot and NVS-fault tests also fail with
// the boot init handing csi_module_init_all() no session (a handle per
// setting).
//
// Sweep F125: a canary's NVS has no "csi" namespace unless a canary-wap
// image made one, and Arduino-ESP32's Preferences::begin() logs an
// error-level "nvs_open failed: NOT_FOUND" for the boot's read-only open of
// it. The shared session's begin() asks IDF's nvs_open() first, which says
// NOT_FOUND without that log, and opens nothing. The clean-boot test fails
// with begin() going straight to Preferences (the code before F125): one
// Preferences open, one error line.
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "Arduino.h"
#include "Preferences.h"
#include "csi_bundler.h"
#include "csi_event.h"
#include "csi_event_id_floor.h"
#include "csi_hal.h"
#include "csi_module.h"
#include "csi_module_settings_nvs.h"
#include "csi_modules_integration.h"

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
uint32_t g_host_millis = 7000000;
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

// ── The chokepoint's hooks: what a commit would hand the egress ─────────
static int g_commits = 0;
static int g_id_advances = 0;
static uint32_t g_first_committed_id = 0;
extern "C" void csi_event_on_committed(uint32_t event_id, const char*, const char*,
                                       csi_event_category_t, csi_privacy_class_t,
                                       const csi_event_values_t*) {
  if (g_commits++ == 0) g_first_committed_id = event_id;
}
extern "C" void csi_event_on_id_advance(uint32_t) { ++g_id_advances; }

// ── NVS rows, as the canary-wap's settings surfaces write them ──────────
static void store_int(const char* nvs_key, int32_t v) {
  host_prefs().i32[std::string("csi/") + nvs_key] = v;
}
static void store_bool(const char* nvs_key, bool v) {
  host_prefs().flag[std::string("csi/") + nvs_key] = v;
}

// ── A boot ──────────────────────────────────────────────────────────────
constexpr uint32_t kFloor = csi_event_id_floor::kIdSpaceBase + 1000;

// A reboot and the canary's CSI boot: RAM is gone (the chokepoint, the
// bundler, the registry and every boot init, and the bridge's own flag),
// NVS stays. Then main.cpp's order: the floor, then the modules.
static void reboot_and_boot() {
  csi_event_test_reset();
  csi_module_test_reset();
  securacv_csi_modules_deinit();
  csi_event_set_event_id_floor(kFloor);       // csi_event_egress_begin()
  g_commits = 0;
  g_id_advances = 0;
  g_first_committed_id = 0;
  host_prefs().begins = 0;
  host_prefs().opens = 0;
  host_prefs().error_logs = 0;
  host_prefs().probes = 0;
  host_prefs().gets.clear();
  (void)securacv_csi_modules_init();
}

// ── Windows ─────────────────────────────────────────────────────────────
// core.presence and anomaly.baseline read motion as the mean of the four
// phase-Doppler bands v[8..11]; breathing is the peak of the FFT bins
// v[12..19]. Equal RSSI stats (v[20], v[22], v[23]): no shimmer.
static csi_features_t window_of(int8_t doppler, int8_t breath_bin = 0) {
  csi_features_t f;
  std::memset(&f, 0, sizeof(f));
  for (int i = 8; i < 12; ++i) f.v[i] = doppler;
  f.v[12] = breath_bin;
  f.v[20] = -50;
  f.v[22] = -50;
  f.v[23] = -50;
  f.frames_in_window = 20;
  return f;
}

// One pass of main.cpp's loop with a CSI window.
static void loop_pass(const csi_features_t& f) {
  g_host_millis += 1000;
  securacv_csi_modules_feed(&f);
  securacv_csi_modules_tick();
}

// The open bundle of `module`'s `state`, from the bundler's own view.
static bool open_bundle(const char* module, const char* state, csi_event_record_t* out) {
  csi_event_record_t rows[8];
  const size_t n = csi_bundler_snapshot_open(rows, 8);
  for (size_t i = 0; i < n; ++i) {
    if (std::strcmp(rows[i].module_id, module) == 0 &&
        std::strcmp(rows[i].values.state_name, state) == 0) {
      if (out) *out = rows[i];
      return true;
    }
  }
  return false;
}
static bool presence_open(const char* state) { return open_bundle("core.presence", state, nullptr); }

// Six windows of one kind: past core.presence's two-window hysteresis.
static void hold(const csi_features_t& f) {
  for (int i = 0; i < 6; ++i) loop_pass(f);
}

// A motion of 30 is past the sensitive preset's motion threshold (25) and
// short of the balanced default's (35). With "sensitive" stored, the room
// reads "subtle" from the first windows of the boot. Before F93 the stored
// preset was never read and the room stayed "empty".
static int test_a_stored_preset_applies_at_boot() {
  host_prefs().clear();
  store_int("cp.preset", 0);
  reboot_and_boot();
  hold(window_of(30));
  CHECK(presence_open("subtle"));
  CHECK(!presence_open("empty"));

  // Nothing stored: the balanced default, as before F93.
  host_prefs().clear();
  reboot_and_boot();
  hold(window_of(30));
  CHECK(!presence_open("subtle"));
  CHECK(presence_open("empty"));
  return 0;
}

// The sensitivity slider moves every threshold (100: 20 points lower), and
// a threshold stored directly (the calibration, the Tuning Lab) wins over
// the preset it was stored beside.
static int test_stored_sensitivity_and_thresholds_apply_at_boot() {
  host_prefs().clear();
  store_int("cp.sens", 100);                  // balanced 35 - 20 = 15
  reboot_and_boot();
  hold(window_of(20));
  CHECK(presence_open("subtle"));

  host_prefs().clear();
  store_int("cp.preset", 2);                  // quiet: motion threshold 50
  store_int("cp.mt", 10);                     // ...but 10, stored directly
  reboot_and_boot();
  hold(window_of(12));
  CHECK(presence_open("subtle"));
  return 0;
}

// The same precedence the other way round: a stored threshold beside the
// "sensitive" preset (25) keeps the preset from lowering it. A canary-wap
// image stores all three thresholds on a calibration apply, a Tuning Lab
// reset (its TUNE_COEFFS defaults, 35 / 75 / 30, as rows; the canary-wap's
// test_wap_module_boot.cpp reads them from its real table) or a bundle import,
// and a board with those rows reads like the balanced default whatever its
// preset row says. The precedence predates F93; this pins what the docs and
// the CHANGELOG say about it. The first case fails on the canary before F93
// (the static 35 reads 40 as "subtle"); both fail with on_init() made to
// let the preset win.
static int test_a_stored_threshold_wins_over_the_stored_preset() {
  host_prefs().clear();
  store_int("cp.preset", 0);
  store_int("cp.mt", 45);
  reboot_and_boot();
  hold(window_of(40));
  CHECK(presence_open("empty"));
  CHECK(!presence_open("subtle"));

  host_prefs().clear();
  store_int("cp.preset", 0);
  store_int("cp.mt", 35);
  store_int("cp.at", 75);
  store_int("cp.bt", 30);
  reboot_and_boot();
  hold(window_of(30));
  CHECK(presence_open("empty"));              // "sensitive" alone reads "subtle"
  CHECK(!presence_open("subtle"));
  host_prefs().clear();
  return 0;
}

// Pet mode (stored as a bool): a breathing peak reads "subtle" until it has
// held for pet_mode_seconds; without it the same windows read "quiet".
static int test_stored_pet_mode_applies_at_boot() {
  host_prefs().clear();
  store_bool("cp.pet_mode", true);
  reboot_and_boot();
  hold(window_of(0, 40));
  CHECK(presence_open("subtle"));
  CHECK(!presence_open("quiet"));

  host_prefs().clear();
  reboot_and_boot();
  hold(window_of(0, 40));
  CHECK(presence_open("quiet"));
  return 0;
}

// anomaly.baseline's cooldown (30 s stored, 600 s default): after its
// 60-window warm-up, a motion spike reports unusual_motion; a second spike
// 36 s later reports again under the stored cooldown and merges into the
// open bundle (two observations). Under the default it is still cooling
// down, and the bundle holds one.
static int spike_twice_and_count() {
  for (int i = 0; i < 61; ++i) loop_pass(window_of(0));
  loop_pass(window_of(80));
  for (int i = 0; i < 35; ++i) loop_pass(window_of(0));
  loop_pass(window_of(80));
  csi_event_record_t row;
  if (!open_bundle("anomaly.baseline", "unusual_motion", &row)) return -1;
  return row.bundled_count;
}
static int test_a_stored_anomaly_cooldown_applies_at_boot() {
  host_prefs().clear();
  store_int("ab.cd", 30);
  reboot_and_boot();
  CHECK(spike_twice_and_count() == 2);

  host_prefs().clear();
  reboot_and_boot();
  CHECK(spike_twice_and_count() == 1);
  return 0;
}

// Each module's init() runs once per boot. The canary's modules read 16
// mapped rows between them (core.presence 10, core.breathing 2,
// anomaly.baseline 4; wifi.channel_activity's three keys are not in the
// map, so they read as defaults), all through the boot's one read-only
// handle. A second securacv_csi_modules_init() reads nothing: a row
// changed between the two calls does not apply until the next boot.
static int test_init_runs_once_per_boot() {
  host_prefs().clear();
  store_int("cp.preset", 0);
  reboot_and_boot();
  CHECK(host_prefs().begins == 1);
  CHECK(host_prefs().opens == 1);
  CHECK(host_prefs().gets.size() == 16);
  for (const char* k : {"cp.pet_mode", "cp.preset", "cp.sens", "cp.mt", "cp.at", "cp.bt",
                        "cp.ps", "cp.srs", "cp.sdf", "cp.se", "cb.lt", "cb.cs",
                        "ab.sr", "ab.mm", "ab.mb", "ab.cd"}) {
    CHECK(host_prefs().gets_of(std::string("csi/") + k) == 1);
  }

  store_int("cp.preset", 2);
  (void)securacv_csi_modules_init();
  CHECK(host_prefs().gets.size() == 16);      // no row read again
  hold(window_of(30));
  CHECK(presence_open("subtle"));             // still the boot's "sensitive"
  return 0;
}

// Nothing on the canary creates the "csi" namespace, and NVS refuses a
// read-only open of a namespace never created; Arduino's Preferences logs
// that refusal at error level, which the release envs keep (sweep F125).
// A clean canary's boot asks IDF's nvs_open() once, which answers NOT_FOUND
// without a log, and then opens nothing: no Preferences open, no error
// line, every module on its defaults. A board a canary-wap image wrote
// opens once, still without an error line.
static int test_a_clean_canary_boot_logs_no_nvs_error() {
  host_prefs().clear();
  reboot_and_boot();
  CHECK(host_prefs().error_logs == 0);
  CHECK(host_prefs().begins == 0);
  CHECK(host_prefs().probes == 1);
  CHECK(host_prefs().gets.empty());
  hold(window_of(30));
  CHECK(presence_open("empty"));              // the balanced default
  CHECK(!presence_open("subtle"));

  store_int("cp.preset", 0);                  // a canary-wap image's row
  reboot_and_boot();
  CHECK(host_prefs().probes == 1);
  CHECK(host_prefs().begins == 1);
  CHECK(host_prefs().opens == 1);
  CHECK(host_prefs().error_logs == 0);
  hold(window_of(30));
  CHECK(presence_open("subtle"));
  host_prefs().clear();
  return 0;
}

// NVS itself refusing every open (a fault, not a missing namespace): the
// probe cannot tell, so the boot tries the Preferences open once, not once
// per setting, and that fault keeps its one error line; every module runs on
// its defaults.
static int test_an_nvs_fault_costs_the_boot_one_open() {
  host_prefs().clear();
  host_prefs().fail_begin = true;
  reboot_and_boot();
  CHECK(host_prefs().begins == 1);
  CHECK(host_prefs().opens == 0);
  CHECK(host_prefs().error_logs == 1);
  CHECK(host_prefs().gets.empty());
  hold(window_of(30));
  CHECK(presence_open("empty"));              // the balanced default
  host_prefs().clear();
  return 0;
}

// The boot init commits nothing and opens no bundle: no init() emits. So
// the first row of the boot takes the floor csi_event_egress_begin()
// restored, whatever a module stored (sweep F46, F83).
static int test_nothing_commits_during_the_boot_init() {
  host_prefs().clear();
  store_int("cp.preset", 0);
  store_int("ab.cd", 30);
  reboot_and_boot();
  CHECK(g_commits == 0);
  CHECK(g_id_advances == 0);
  CHECK(csi_bundler_open_count() == 0);
  CHECK(csi_event_get_next_event_id() == kFloor);

  // The first row of the boot: a stateless emit commits at once.
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_TIME_BUCKET;
  (void)csi_event_emit("core.activity_ribbon", "ribbon_bucket_advanced", &v);
  CHECK(g_commits == 1);
  CHECK(g_first_committed_id == kFloor);
  return 0;
}

// ── The library: init once, never a tick before it ─────────────────────
static int g_inits = 0;
static int g_ticks = 0;
static int g_inits_b = 0;
static const csi_event_decl_t kCountEvents[] = {
  {"counted", CSI_FIELD_TIME_BUCKET, CSI_PRIVACY_P0, 0},
};
static void count_init(const csi_module_settings_t*) { ++g_inits; }
static void count_tick(const csi_features_t*) { ++g_ticks; }
static void count_init_b(const csi_module_settings_t*) { ++g_inits_b; }
static const csi_module_t kCounter = {
  "test.counter", CSI_PRIVACY_P0, kCountEvents, 1, count_init, count_tick, nullptr, nullptr,
};
static const csi_module_t kLate = {
  "test.late", CSI_PRIVACY_P0, kCountEvents, 1, count_init_b, count_tick, nullptr, nullptr,
};
static const csi_module_t kNoInit = {
  "test.no_init", CSI_PRIVACY_P0, kCountEvents, 1, nullptr, count_tick, nullptr, nullptr,
};

static int test_no_module_ticks_before_its_boot_init() {
  csi_module_test_reset();
  g_inits = g_ticks = g_inits_b = 0;
  const csi_features_t f = window_of(0);
  CHECK(csi_module_register(&kCounter));
  CHECK(csi_module_register(&kNoInit));
  csi_module_tick_all(&f);
  CHECK(g_ticks == 0);                        // registered, not initialized
  CHECK(csi_module_init_all(nullptr) == 1);   // kNoInit has no init: counts as done
  CHECK(g_inits == 1);
  csi_module_tick_all(&f);
  CHECK(g_ticks == 2);

  CHECK(csi_module_init_all(nullptr) == 0);   // never twice
  CHECK(g_inits == 1);
  CHECK(csi_module_register(&kLate));         // registered after the boot init
  csi_module_tick_all(&f);
  CHECK(g_ticks == 4);                        // the late one does not tick yet
  CHECK(csi_module_init_all(nullptr) == 1);
  CHECK(g_inits == 1);
  CHECK(g_inits_b == 1);
  csi_module_tick_all(&f);
  CHECK(g_ticks == 7);
  csi_module_test_reset();
  return 0;
}

// ── The one rule both trees read settings by ────────────────────────────
static int test_the_shared_settings_rule() {
  namespace nvs = csi_module_settings_nvs;
  for (size_t i = 0; i < nvs::kKeyCount; ++i) {
    CHECK(std::strlen(nvs::kKeys[i].nvs) <= 15);        // Preferences' key limit
    CHECK(nvs::nvs_key_for(nvs::kKeys[i].full) == nvs::kKeys[i].nvs);
    for (size_t j = i + 1; j < nvs::kKeyCount; ++j) {
      CHECK(std::strcmp(nvs::kKeys[i].full, nvs::kKeys[j].full) != 0);
      CHECK(std::strcmp(nvs::kKeys[i].nvs, nvs::kKeys[j].nvs) != 0);
    }
  }
  CHECK(std::strcmp(nvs::kNamespace, "csi") == 0);

  host_prefs().clear();
  store_int("cp.preset", 0);
  store_bool("cp.pet_mode", true);
  // The bridge's overrides read by the rule: the stored row, typed.
  CHECK(csi_module_settings_int(nullptr, "core.presence.preset", 1) == 0);
  CHECK(csi_module_settings_bool(nullptr, "core.presence.pet_mode", false));
  // A row of another type reads as the default (NVS rows are typed).
  CHECK(csi_module_settings_int(nullptr, "core.presence.pet_mode", 7) == 7);
  // A key not in the map reads as the default and opens nothing.
  const int opens = host_prefs().opens;
  CHECK(csi_module_settings_int(nullptr, "wifi.channel_activity.cooldown_sec", 5) == 5);
  CHECK(csi_module_settings_int(nullptr, nullptr, 9) == 9);
  CHECK(host_prefs().opens == opens);
  // A namespace that will not open reads as the default.
  host_prefs().fail_begin = true;
  CHECK(csi_module_settings_int(nullptr, "core.presence.preset", 1) == 1);
  CHECK(csi_module_settings_float(nullptr, "core.presence.preset", 2.5f) == 2.5f);
  host_prefs().fail_begin = false;

  // Through a session: one handle for every read, the same answers.
  {
    csi_module_settings_nvs::Session<Preferences> session;
    const int begins = host_prefs().begins;
    nvs::begin(session);
    CHECK(session.open);
    CHECK(nvs::read_int<Preferences>(&session, "core.presence.preset", 1) == 0);
    CHECK(nvs::read_bool<Preferences>(&session, "core.presence.pet_mode", false));
    CHECK(nvs::read_int<Preferences>(&session, "core.presence.pet_mode", 7) == 7);
    CHECK(nvs::read_int<Preferences>(&session, "wifi.channel_activity.cooldown_sec", 5) == 5);
    CHECK(host_prefs().begins == begins + 1);
    nvs::end(session);
    CHECK(!session.open);
  }
  // A session over a namespace NVS does not hold opens nothing and logs
  // nothing (F125), and answers every read with its default.
  {
    host_prefs().clear();
    csi_module_settings_nvs::Session<Preferences> session;
    nvs::begin(session);
    CHECK(!session.open);
    CHECK(host_prefs().probes == 1);
    CHECK(host_prefs().begins == 0);
    CHECK(host_prefs().error_logs == 0);
    CHECK(nvs::read_int<Preferences>(&session, "core.presence.preset", 1) == 1);
    nvs::end(session);
  }
  // A session whose open was refused answers every read with its default,
  // and opens nothing more.
  {
    host_prefs().fail_begin = true;
    csi_module_settings_nvs::Session<Preferences> session;
    nvs::begin(session);
    CHECK(!session.open);
    const int begins = host_prefs().begins;
    CHECK(nvs::read_int<Preferences>(&session, "core.presence.preset", 1) == 1);
    CHECK(!nvs::read_bool<Preferences>(&session, "core.presence.pet_mode", false));
    CHECK(host_prefs().begins == begins);
    nvs::end(session);
  }
  host_prefs().clear();
  return 0;
}

int main() {
  if (test_a_stored_preset_applies_at_boot()) return 1;
  if (test_stored_sensitivity_and_thresholds_apply_at_boot()) return 1;
  if (test_a_stored_threshold_wins_over_the_stored_preset()) return 1;
  if (test_stored_pet_mode_applies_at_boot()) return 1;
  if (test_a_stored_anomaly_cooldown_applies_at_boot()) return 1;
  if (test_init_runs_once_per_boot()) return 1;
  if (test_a_clean_canary_boot_logs_no_nvs_error()) return 1;
  if (test_an_nvs_fault_costs_the_boot_one_open()) return 1;
  if (test_nothing_commits_during_the_boot_init()) return 1;
  if (test_no_module_ticks_before_its_boot_init()) return 1;
  if (test_the_shared_settings_rule()) return 1;
  std::printf("test_csi_module_boot: %d checks passed\n", g_checks);
  return 0;
}
