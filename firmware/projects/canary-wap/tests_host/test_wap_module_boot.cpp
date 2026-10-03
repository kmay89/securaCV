// Host tests for the CSI modules' boot init on the canary-wap (sweep F93).
//
// csi_module_register() only records a module. Before F93 the canary-wap
// ran a module's init() only from reinit_module(), after /api/settings,
// /api/csi/calibrate/apply or a Tuning Lab change, so a preset, threshold,
// pet mode or anomaly cooldown saved in an earlier boot did nothing until
// the owner changed a setting again. csi_integration::init() now calls
// csi_settings_nvs_init_modules() right after register_v1_modules(): every
// registered module's init(), once, through one read-only NVS handle.
//
// What runs here is REAL: the canary-wap's settings readers and boot init
// (csi_settings_nvs.cpp, by the shared csi_module_settings_nvs.h rule), the
// staged CSI library (csi_event, csi_bundler, csi_module) and the staged
// modules, over a fake NVS that outlives a modeled reboot
// (stubs/module_boot/Preferences.h).
//
// What is modeled: csi_integration.cpp, which no host suite compiles. A boot
// here is its order: the event-id floor (apply_event_id_floor_from_nvs),
// register_v1_modules(), then csi_settings_nvs_init_modules(); a window is
// on_csi_window()'s csi_module_tick_all() and the loop's
// csi_bundler_tick(). check_wap_event_egress.py's rule 3 holds init() to
// that order. The model registers the modules register_v1_modules()
// registers, and this test reads that function to keep it so; the three
// feature-gated ones it leaves out (acoustic.events, vault.events,
// ble.scout) have empty init() bodies, which it also reads.
//
// Each test of a stored setting fails with csi_module_init_all() made a
// no-op (the canary-wap before F93, which ran no init at boot); the
// NVS-fault test, the commit test and the source pin pass there too, and
// guard the new boot path. The once-per-boot and NVS-fault tests fail with
// the boot init handing csi_module_init_all() no session (a handle per
// setting).
//
// Sweep F125: the boot's read-only open asks IDF's nvs_open() first
// (csi_module_settings_nvs.h, shared with the canary), so a board whose NVS
// holds no "csi" namespace (a canary-wap's first boot after an erase) opens
// nothing for the modules and logs no Preferences error for it. The
// never-written test fails with begin() going straight to Preferences (the
// code before F125).
//
// Sweep F150: the canary-wap's boot reads "csi" read-only twice before
// anything writes it: csi_integration::init() restores the event-id floor,
// then the events egress reads its delivery ceiling. Each went to
// Preferences::begin(), which logs "nvs_open failed: NOT_FOUND" at error
// level for a namespace that is not there: two lines on the first boot
// after an erase. (The egress then stores its first ceiling record, which
// creates the namespace, so the reads after it find it; on a boot where that
// record is not written, each later read-only open logged one more line.)
// Every read-only open of the namespace in the sketch now goes through
// csi_module_settings_nvs::begin_read_only(), the F125 probe in front of
// Preferences::begin(): the floor's read (read_event_id_floor_rows(),
// csi_settings_nvs.cpp), Quiet Hours and the privacy ceiling run here in
// the boot's order on such NVS; test_wap_event_egress.cpp runs the egress's
// read and test_mqtt_reinit.cpp the MQTT bridge's; the source pin at the end
// holds every other read-only open of the namespace (csi_integration.cpp's,
// canary_wap.ino's) to the helper.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "Preferences.h"
#include "csi_bundler.h"
#include "csi_event.h"
#include "csi_event_id_floor.h"
#include "csi_module.h"
#include "csi_module_settings_nvs.h"
#include "csi_settings_nvs.h"
#include "csi_tune_lab.h"

#include "anomaly_baseline.h"
#include "ble_events_module.h"
#include "core_activity_ribbon.h"
#include "core_breathing.h"
#include "core_multilink_fusion.h"
#include "core_presence.h"
#include "meta_daily_summary.h"
#include "meta_empty_room_baseline.h"
#include "meta_quiet_hours.h"
#include "tamper_events_module.h"
#include "wifi_channel_activity.h"

#ifndef WAP_SKETCH_DIR
#error "build with -DWAP_SKETCH_DIR=\"<the canary_wap sketch directory>\""
#endif

static int g_checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                         \
    }                                                                   \
    ++g_checks;                                                         \
  } while (0)

// ── Time: the library's and the ribbon's CLOCK_MONOTONIC ────────────────
static uint32_t g_ms = 9000000;
extern "C" int clock_gettime(clockid_t, struct timespec* ts) noexcept {
  ts->tv_sec = (time_t)(g_ms / 1000);
  ts->tv_nsec = (long)((g_ms % 1000) * 1000000);
  return 0;
}

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

// ── NVS rows, as /api/settings and the Tuning Lab write them ────────────
static void store_int(const char* nvs_key, int32_t v) {
  host_prefs().i32[std::string("csi/") + nvs_key] = v;
}
static void store_bool(const char* nvs_key, bool v) {
  host_prefs().flag[std::string("csi/") + nvs_key] = v;
}

// ── The boot (csi_integration::init's order) ────────────────────────────
constexpr uint32_t kFloor = csi_event_id_floor::kIdSpaceBase + 5000;

// register_v1_modules(), less the feature-gated modules with empty inits
// (test_the_model_is_register_v1_modules pins both halves of that).
static void register_v1_modules_model() {
  csi_module_register(core_presence_module());
  csi_module_register(core_breathing_module());
  csi_module_register(core_activity_ribbon_module());
  csi_module_register(meta_daily_summary_module());
  csi_module_register(meta_quiet_hours_module());
  csi_module_register(anomaly_baseline_module());
  csi_module_register(wifi_channel_activity_module());
  csi_module_register(core_multilink_fusion_module());
  csi_module_register(meta_empty_room_baseline_module());
  csi_module_register(ble_events_module());
  csi_module_register(tamper_events_module());
}
static const char* const kModeled[] = {
  "core_presence_module", "core_breathing_module", "core_activity_ribbon_module",
  "meta_daily_summary_module", "meta_quiet_hours_module", "anomaly_baseline_module",
  "wifi_channel_activity_module", "core_multilink_fusion_module",
  "meta_empty_room_baseline_module", "ble_events_module", "tamper_events_module",
};

// A reboot: RAM is gone, NVS stays. Then init()'s order.
static void reboot_and_boot() {
  csi_event_test_reset();
  csi_module_test_reset();
  csi_event_set_event_id_floor(kFloor);       // apply_event_id_floor_from_nvs()
  g_commits = 0;
  g_id_advances = 0;
  g_first_committed_id = 0;
  host_prefs().begins = 0;
  host_prefs().opens = 0;
  host_prefs().error_logs = 0;
  host_prefs().probes = 0;
  host_prefs().gets.clear();
  register_v1_modules_model();                // register_v1_modules()
  (void)csi_settings_nvs_init_modules();      // F93
}

// ── Windows (on_csi_window, then the loop's bundler tick) ───────────────
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
static void window(const csi_features_t& f) {
  g_ms += 1000;
  csi_module_tick_all(&f);
  csi_bundler_tick();
}
static void hold(const csi_features_t& f) {
  for (int i = 0; i < 6; ++i) window(f);
}
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

// The dashboard's preset, saved in an earlier boot: "sensitive" (motion
// threshold 25) reads a motion of 30 as "subtle" from the boot's first
// windows. Before F93 the boot ran on the balanced default (35) and read
// "empty" until the owner changed a setting.
static int test_a_saved_preset_applies_at_boot() {
  host_prefs().clear();
  store_int("cp.preset", 0);
  reboot_and_boot();
  hold(window_of(30));
  CHECK(presence_open("subtle"));
  CHECK(!presence_open("empty"));

  host_prefs().clear();
  reboot_and_boot();
  hold(window_of(30));
  CHECK(presence_open("empty"));
  CHECK(!presence_open("subtle"));
  return 0;
}

// The calibration's thresholds (stored directly, cp.mt / cp.at / cp.bt)
// and pet mode (a bool row) apply at boot too.
static int test_saved_thresholds_and_pet_mode_apply_at_boot() {
  host_prefs().clear();
  store_int("cp.mt", 12);
  reboot_and_boot();
  hold(window_of(15));
  CHECK(presence_open("subtle"));

  host_prefs().clear();
  store_bool("cp.pet_mode", true);
  reboot_and_boot();
  hold(window_of(0, 40));
  CHECK(presence_open("subtle"));
  CHECK(!presence_open("quiet"));
  return 0;
}

// The Tuning Lab's default for a coefficient, from the real TUNE_COEFFS
// (csi_tune_lab.cpp): the value its per-row "reset" and "Reset all" POST
// (tune_ui.h), which tune_write_value() stores as a row like any other. -1
// if absent.
static std::string read_source(const char* name);
static int32_t tune_default_of(const char* full_key) {
  const TuneCoeff* c = tune_coeff_for(full_key);
  return c != nullptr ? c->default_v : -1;
}

// A threshold stored directly wins over the saved preset and sensitivity,
// at boot as after a change (core_presence.cpp's on_init() passes the
// preset's baseline only as the default of the direct reads). The
// calibration's apply stores all three thresholds, and so do the Tuning
// Lab's per-row reset and "Reset all" (each POSTs its TUNE_COEFFS default
// as a row) and a bundle import (every coefficient). On such a board a
// saved preset changes nothing, at boot or at once. The docs and the
// CHANGELOG say so; this pins it. It is not an F93 behavior: the precedence
// predates it. The first case fails on the canary-wap before F93 (no boot
// init: the static 35 reads a motion of 40 as "subtle"); every case fails
// with on_init() made to let the preset win.
static int test_a_stored_threshold_wins_over_the_saved_preset() {
  namespace nvs = csi_module_settings_nvs;
  // "sensitive" (motion threshold 25) beside a motion threshold of 45.
  host_prefs().clear();
  store_int("cp.preset", 0);
  store_int("cp.mt", 45);
  reboot_and_boot();
  hold(window_of(40));
  CHECK(presence_open("empty"));
  CHECK(!presence_open("subtle"));

  // "sensitive" beside the rows the Tuning Lab's reset buttons store: the
  // device's own TUNE_COEFFS defaults, the balanced thresholds.
  const char* const kThresholds[] = {"core.presence.motion_threshold",
                                     "core.presence.active_threshold",
                                     "core.presence.breathing_threshold"};
  host_prefs().clear();
  store_int("cp.preset", 0);
  for (const char* full : kThresholds) {
    const int32_t d = tune_default_of(full);
    CHECK(d > 0);
    CHECK(nvs::nvs_key_for(full) != nullptr);
    store_int(nvs::nvs_key_for(full), d);
  }
  CHECK(tune_default_of("core.presence.motion_threshold") == 35);
  reboot_and_boot();
  hold(window_of(30));
  CHECK(presence_open("empty"));              // sensitive alone reads "subtle"
  CHECK(!presence_open("subtle"));

  // The dashboard saves "sensitive" again and slides sensitivity to 100
  // (/api/settings, then reinit_module(), modeled): still the stored rows.
  store_int("cp.preset", 0);
  store_int("cp.sens", 100);
  const csi_module_t* m = csi_module_find("core.presence");
  CHECK(m != nullptr);
  m->deinit();
  m->init(nullptr);
  hold(window_of(30));
  CHECK(!presence_open("subtle"));
  host_prefs().clear();
  return 0;
}

// F166: the calibration's status reports the thresholds core.presence
// runs. For each way the rows can stand, the motion threshold the status's
// reader reports (read_presence_thresholds_in_use(), the handler's) is the
// one the booted module classifies at: a motion one below it reads
// "empty", the motion itself "subtle". Before F166 the status reported the
// balanced 35 for every preset with no row stored, and "sensitive" ran 25.
static int32_t status_motion_threshold(const char** source) {
  Preferences prefs;
  PresenceThresholdsInUse t = presence_thresholds_in_use_unread();
  if (csi_module_settings_nvs::begin_read_only(prefs)) {
    t = read_presence_thresholds_in_use(prefs);
    prefs.end();
  }
  *source = presence_thresholds_source(t);
  return t.thresholds.motion;
}

static bool module_runs_motion_threshold(int32_t threshold) {
  reboot_and_boot();
  hold(window_of((int8_t)(threshold - 1)));
  const bool below_is_empty = presence_open("empty") && !presence_open("subtle");
  reboot_and_boot();
  hold(window_of((int8_t)threshold));
  const bool at_is_subtle = presence_open("subtle");
  return below_is_empty && at_is_subtle;
}

static int test_the_calibration_status_reports_what_the_module_runs() {
  struct Case { int32_t preset, sens, mt; const char* source; int32_t want; };
  const Case kCases[] = {
    {-1, -1, -1, "preset", 35},   // nothing stored: balanced
    { 0, -1, -1, "preset", 25},   // sensitive
    { 2, -1, -1, "preset", 50},   // quiet
    { 0, 80, -1, "preset", 13},   // sensitive, slider at 80: 25 - 12
    { 2, 10, -1, "preset", 66},   // quiet, slider at 10: 50 + 16
    { 0, -1, 45, "mixed",  45},   // a stored motion row wins over the preset
  };
  for (const Case& c : kCases) {
    host_prefs().clear();
    if (c.preset >= 0) store_int("cp.preset", c.preset);
    if (c.sens >= 0) store_int("cp.sens", c.sens);
    if (c.mt >= 0) store_int("cp.mt", c.mt);
    const char* source = "";
    const int32_t reported = status_motion_threshold(&source);
    CHECK(reported == c.want);
    CHECK(std::strcmp(source, c.source) == 0);
    CHECK(module_runs_motion_threshold(reported));
  }
  host_prefs().clear();
  return 0;
}

// anomaly.baseline's cooldown from the Tuning Lab (30 s; 600 s default): a
// second motion spike 36 s after the first reports again, into the open
// bundle, only under the stored cooldown.
static int spike_twice_and_count() {
  for (int i = 0; i < 61; ++i) window(window_of(0));
  window(window_of(80));
  for (int i = 0; i < 35; ++i) window(window_of(0));
  window(window_of(80));
  csi_event_record_t row;
  if (!open_bundle("anomaly.baseline", "unusual_motion", &row)) return -1;
  return row.bundled_count;
}
static int test_a_saved_anomaly_cooldown_applies_at_boot() {
  host_prefs().clear();
  store_int("ab.cd", 30);
  reboot_and_boot();
  CHECK(spike_twice_and_count() == 2);

  host_prefs().clear();
  reboot_and_boot();
  CHECK(spike_twice_and_count() == 1);
  return 0;
}

// Once per boot: the WAP's modules read the same 16 mapped rows as the
// canary's (meta.quiet_hours, ble.events and system.integrity read none),
// each once, all through the boot's one read-only handle. A second
// csi_module_init_all() reads nothing. A settings change still applies at
// once, through reinit_module()'s direct deinit() + init(nullptr)
// (modeled), which the boot's latch does not stop and which reads with a
// handle per row, as before F93.
static int test_init_runs_once_per_boot_and_a_change_still_applies() {
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
  CHECK(csi_module_init_all(nullptr) == 0);
  CHECK(host_prefs().begins == 1);

  // /api/settings stores "quiet" (motion threshold 50) and reinits.
  store_int("cp.preset", 2);
  const csi_module_t* m = csi_module_find("core.presence");
  CHECK(m != nullptr);
  m->deinit();
  m->init(nullptr);
  CHECK(host_prefs().opens == 1 + 10);        // core.presence's ten rows
  hold(window_of(30));
  CHECK(!presence_open("subtle"));
  return 0;
}

// A board whose NVS never held the namespace (the first boot after an
// erase): the boot's probe finds it absent through IDF's nvs_open(), which
// logs nothing, and the modules' boot init opens nothing and logs no
// Preferences error (F125); every module runs on its defaults. Once a
// settings surface has written a row, the boot opens once, as before.
static int test_a_never_written_namespace_opens_nothing_at_boot() {
  host_prefs().clear();
  reboot_and_boot();
  CHECK(host_prefs().error_logs == 0);
  CHECK(host_prefs().begins == 0);
  CHECK(host_prefs().probes == 1);
  CHECK(host_prefs().gets.empty());
  hold(window_of(30));
  CHECK(presence_open("empty"));

  store_int("cp.preset", 0);
  reboot_and_boot();
  CHECK(host_prefs().begins == 1);
  CHECK(host_prefs().opens == 1);
  CHECK(host_prefs().error_logs == 0);
  host_prefs().clear();
  return 0;
}

// NVS refusing every open (a fault): the boot tries the Preferences open
// once, not once per setting, keeps that fault's one error line, and every
// module runs on its defaults.
static int test_an_nvs_fault_costs_the_boot_one_open() {
  host_prefs().clear();
  host_prefs().fail_begin = true;
  reboot_and_boot();
  CHECK(host_prefs().begins == 1);
  CHECK(host_prefs().opens == 0);
  CHECK(host_prefs().error_logs == 1);
  CHECK(host_prefs().gets.empty());
  hold(window_of(30));
  CHECK(presence_open("empty"));
  host_prefs().clear();
  return 0;
}

// The boot init commits nothing and opens no bundle, so the boot's first
// row takes the floor apply_event_id_floor_from_nvs() restored (F46, F83).
static int test_nothing_commits_during_the_boot_init() {
  host_prefs().clear();
  store_int("cp.preset", 0);
  store_bool("cp.pet_mode", true);
  reboot_and_boot();
  CHECK(g_commits == 0);
  CHECK(g_id_advances == 0);
  CHECK(csi_bundler_open_count() == 0);
  CHECK(csi_event_get_next_event_id() == kFloor);

  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_TIME_BUCKET;
  (void)csi_event_emit("core.activity_ribbon", "ribbon_bucket_advanced", &v);
  CHECK(g_commits == 1);
  CHECK(g_first_committed_id == kFloor);
  return 0;
}

// ── The model is register_v1_modules() ──────────────────────────────────
static std::string read_source(const char* name) {
  std::ifstream in(std::string(WAP_SKETCH_DIR) + "/" + name);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The body of the first definition whose header matches `head` (a regex
// ending at its opening brace), or "".
static std::string body_of(const std::string& src, const std::string& head) {
  std::smatch m;
  if (!std::regex_search(src, m, std::regex(head))) return "";
  size_t at = (size_t)(m.position(0) + m.length(0)) - 1;
  int depth = 0;
  for (size_t j = at; j < src.size(); ++j) {
    if (src[j] == '{') ++depth;
    if (src[j] == '}' && --depth == 0) return src.substr(at + 1, j - at - 1);
  }
  return "";
}

// Comments out, whitespace out: what an init() body does.
static std::string code_of(std::string body) {
  body = std::regex_replace(body, std::regex(R"(/\*[\s\S]*?\*/)"), "");
  body = std::regex_replace(body, std::regex(R"(//[^\n]*)"), "");
  return std::regex_replace(body, std::regex(R"(\s+)"), "");
}

static int test_the_model_is_register_v1_modules() {
  const std::string integ = read_source("csi_integration.cpp");
  CHECK(!integ.empty());
  const std::string reg = body_of(integ, R"(\bvoid\s+register_v1_modules\s*\(\s*\)\s*\{)");
  CHECK(!reg.empty());
  std::vector<std::string> registered;
  const std::regex call(R"(csi_module_register\(\s*([\w:]+)\s*\(\s*\)\s*\))");
  for (std::sregex_iterator it(reg.begin(), reg.end(), call), end; it != end; ++it) {
    registered.push_back((*it)[1].str());
  }
  // The device's list is the model's plus the three feature-gated modules,
  // in register_v1_modules()'s order.
  const std::vector<std::string> want = {
    "core_presence_module", "core_breathing_module", "core_activity_ribbon_module",
    "meta_daily_summary_module", "meta_quiet_hours_module", "anomaly_baseline_module",
    "wifi_channel_activity_module", "core_multilink_fusion_module",
    "meta_empty_room_baseline_module", "ble_events_module", "acoustic_events_module",
    "vault_events_module", "tamper_events_module", "ble_scout::ble_scout_module",
  };
  CHECK(registered == want);
  size_t modeled = 0;
  for (const std::string& r : registered) {
    for (const char* k : kModeled) modeled += (r == k);
  }
  CHECK(modeled == sizeof(kModeled) / sizeof(kModeled[0]));

  // ...whose init() bodies do nothing, so leaving them out of the boot
  // model leaves out no read, emit or allocation.
  // (Each manifest names that function as its init, and its body is empty
  // once comments are gone.)
  struct Gated { const char* file; const char* fn; };
  for (const Gated& g : {Gated{"acoustic_events_module.cpp", "on_init"},
                         Gated{"vault_events_module.cpp", "on_init"},
                         Gated{"ble_scout.cpp", "module_init"}}) {
    const std::string src = read_source(g.file);
    CHECK(!src.empty());
    CHECK(std::regex_search(src, std::regex(std::string(R"(/\*\s*init\s*\*/\s*)") + g.fn + R"(\s*,)")));
    const std::string head = std::string(R"(\bvoid\s+)") + g.fn +
                             R"(\s*\(\s*const\s+csi_module_settings_t\s*\*[^)]*\)\s*\{)";
    CHECK(std::regex_search(src, std::regex(head)));
    CHECK(code_of(body_of(src, head)).empty());
  }
  return 0;
}

// ── Sweep F150: the first boot after an NVS erase ───────────────────────

// begin_read_only() answers as Preferences::begin() would, without the
// error line an absent namespace costs: false and no open for one never
// created; one open for one that is there; and a fault still goes to
// Preferences, which logs it.
static int test_begin_read_only_answers_as_the_open_would() {
  host_prefs().clear();
  {
    Preferences p;
    CHECK(!csi_module_settings_nvs::begin_read_only(p));
    CHECK(!csi_module_settings_nvs::begin_read_only(p, "another"));
  }
  CHECK(host_prefs().begins == 0 && host_prefs().error_logs == 0 && host_prefs().probes == 2);
  CHECK(!host_prefs().has_namespace("csi"));             // a read creates nothing
  host_prefs().created.insert("csi");
  {
    Preferences p;
    CHECK(csi_module_settings_nvs::begin_read_only(p));
    p.end();
  }
  CHECK(host_prefs().opens == 1 && host_prefs().error_logs == 0);
  host_prefs().clear();
  host_prefs().fail_begin = true;
  {
    Preferences p;
    CHECK(!csi_module_settings_nvs::begin_read_only(p));
  }
  CHECK(host_prefs().begins == 1 && host_prefs().error_logs == 1);
  host_prefs().clear();
  return 0;
}

static void reset_counts() {
  host_prefs().begins = 0;
  host_prefs().opens = 0;
  host_prefs().error_logs = 0;
  host_prefs().probes = 0;
  host_prefs().gets.clear();
}

// csi_integration::init()'s reads of "csi", in its order, on NVS that holds
// nothing. The floor's read finds no namespace and says NVS was not read
// (the boot does not reload the card's log, as before), without a line.
// Then the events egress (test_wap_event_egress.cpp) reads its ceiling, as
// quietly, and stores its first record, which creates the namespace; the
// reads after it open it. On a boot where that record is not written (the
// egress could not allocate its state), they find no namespace either, and
// say nothing either.
static int test_a_first_boot_after_an_erase_logs_no_nvs_error() {
  constexpr const char* kFloorKey = "ev.next";       // csi_integration.cpp's NVS_KEY_EVENT_ID
  constexpr const char* kCeilingKey = "csi.evsent";  // csi_mqtt::NVS_KEY_DELIVERED
  for (int egress_record = 0; egress_record < 2; ++egress_record) {
    host_prefs().clear();
    csi_event_test_reset();
    csi_module_test_reset();
    uint32_t floor = 1, ceiling = 1;
    CHECK(!read_event_id_floor_rows(kFloorKey, kCeilingKey, &floor, &ceiling));
    CHECK(floor == 0 && ceiling == 0);
    CHECK(host_prefs().begins == 0 && host_prefs().error_logs == 0);
    if (egress_record) {
      Preferences w;                                   // the egress's persist_ceiling()
      CHECK(w.begin("csi", /*readOnly=*/false));
      CHECK(w.putULong(kCeilingKey, 1) == 4);
      w.end();
    }
    reset_counts();
    register_v1_modules_model();                       // register_v1_modules(),
    apply_quiet_hours_from_nvs();                      //   which applies Quiet Hours
    (void)csi_settings_nvs_init_modules();             // the modules' boot init (F93, F125)
    apply_privacy_ceiling_from_nvs();
    CHECK(host_prefs().error_logs == 0);
    CHECK(host_prefs().begins == (egress_record ? 3 : 0));
    CHECK(host_prefs().opens == host_prefs().begins);
    CHECK(csi_event_get_privacy_ceiling() == CSI_PRIVACY_P0);
  }

  // A later boot reads what was stored, through one handle, with no line.
  host_prefs().clear();
  host_prefs().u32[std::string("csi/") + kFloorKey] = kFloor;
  host_prefs().u32[std::string("csi/") + kCeilingKey] = kFloor - 3;
  uint32_t floor = 0, ceiling = 0;
  CHECK(read_event_id_floor_rows(kFloorKey, kCeilingKey, &floor, &ceiling));
  CHECK(floor == kFloor && ceiling == kFloor - 3);
  CHECK(host_prefs().opens == 1 && host_prefs().error_logs == 0);

  // NVS refusing every open (a fault) keeps each read's one line.
  host_prefs().fail_begin = true;
  reset_counts();
  CHECK(!read_event_id_floor_rows(kFloorKey, kCeilingKey, &floor, &ceiling));
  apply_quiet_hours_from_nvs();
  apply_privacy_ceiling_from_nvs();
  CHECK(host_prefs().begins == 3 && host_prefs().error_logs == 3);
  host_prefs().clear();
  return 0;
}

// ── Source pin: every read-only open of "csi" is the quiet one ──────────

// `src` with its comments blanked (string literals kept: "csi" is one).
static std::string without_comments(const std::string& s) {
  std::string out = s;
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
      while (i < n && s[i] != '\n') out[i++] = ' ';
      continue;
    }
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
      const size_t end = s.find("*/", i + 2);
      const size_t stop = end == std::string::npos ? n : end + 2;
      for (; i < stop; ++i) {
        if (out[i] != '\n') out[i] = ' ';
      }
      continue;
    }
    if (s[i] == 'R' && i + 1 < n && s[i + 1] == '"' &&
        (i == 0 || !(std::isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_'))) {
      const size_t open = s.find('(', i + 2);
      if (open == std::string::npos) break;
      const std::string close = ")" + s.substr(i + 2, open - (i + 2)) + "\"";
      const size_t end = s.find(close, open + 1);
      i = end == std::string::npos ? n : end + close.size();
      continue;
    }
    if (s[i] == '"' || (s[i] == '\'' && !(i > 0 && std::isxdigit((unsigned char)s[i - 1])))) {
      const char q = s[i];
      for (++i; i < n && s[i] != q; ++i) {
        if (s[i] == '\\') ++i;
      }
      ++i;
      continue;
    }
    ++i;
  }
  return out;
}

static std::string trimmed(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) ++a;
  while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
  return s.substr(a, b - a);
}

// The read-only Preferences opens of the "csi" namespace in `src` that do
// not go through begin_read_only(): each `.begin(<ns>, true)` whose <ns> is
// one of the spellings the sketch gives that namespace.
static std::vector<std::string> plain_read_only_opens(const std::string& src) {
  static const char* const kCsi[] = {"SETTINGS_NS", "\"csi\"", "kNamespace",
                                     "csi_module_settings_nvs::kNamespace"};
  const std::string code = without_comments(src);
  std::vector<std::string> out;
  for (size_t at = code.find(".begin("); at != std::string::npos; at = code.find(".begin(", at + 1)) {
    size_t i = at + 7;
    int depth = 1;
    std::vector<std::string> args(1);
    for (; i < code.size() && depth > 0; ++i) {
      const char c = code[i];
      if (c == '(') ++depth;
      if (c == ')' && --depth == 0) break;
      if (c == ',' && depth == 1) {
        args.emplace_back();
        continue;
      }
      args.back() += c;
    }
    if (args.size() != 2 || trimmed(args[1]) != "true") continue;
    const std::string ns = trimmed(args[0]);
    for (const char* k : kCsi) {
      if (ns == k) out.push_back(code.substr(at, i - at + 1));
    }
  }
  return out;
}

static int test_every_read_only_open_of_csi_is_the_quiet_one() {
  namespace fs = std::filesystem;
  std::vector<std::string> scanned;
  size_t quiet_calls = 0;
  const std::regex settings_ns(R"(\bSETTINGS_NS\s*=\s*([^;]+);)");
  for (const fs::directory_entry& e : fs::directory_iterator(WAP_SKETCH_DIR)) {
    const std::string name = e.path().filename().string();
    const std::string ext = e.path().extension().string();
    if (ext != ".cpp" && ext != ".h" && ext != ".ino") continue;
    const std::string src = read_source(name.c_str());
    if (src.find("Preferences") == std::string::npos && src.find("begin_read_only") == std::string::npos) continue;
    scanned.push_back(name);
    // A file's SETTINGS_NS is the "csi" namespace, or the rule below would
    // not know what it opens.
    for (std::sregex_iterator it(src.begin(), src.end(), settings_ns), end; it != end; ++it) {
      const std::string v = trimmed((*it)[1].str());
      if (v != "\"csi\"" && v != "csi_module_settings_nvs::kNamespace") {
        std::fprintf(stderr, "%s: SETTINGS_NS is %s, not the csi namespace\n", name.c_str(), v.c_str());
        CHECK(false);
      }
    }
    for (const std::string& open : plain_read_only_opens(src)) {
      std::fprintf(stderr, "%s: a read-only open of \"csi\" not through begin_read_only(): %s\n",
                   name.c_str(), open.c_str());
      CHECK(false);
    }
    const std::string code = without_comments(src);
    for (size_t at = code.find("begin_read_only("); at != std::string::npos;
         at = code.find("begin_read_only(", at + 1)) {
      if (name == "csi_module_settings_nvs.h") continue;
      // The quiet opens of another namespace (mesh_network.cpp's of "mesh",
      // sweep F164; test_mesh_liveness_wap.cpp holds those) are not this
      // rule's: count a call only when it opens "csi" (no namespace
      // argument, or one of the sketch's spellings of it).
      const size_t close = code.find(')', at);
      const std::string call = code.substr(at, close == std::string::npos ? 0 : close - at);
      const size_t comma = call.find(',');
      const std::string ns = comma == std::string::npos ? "" : trimmed(call.substr(comma + 1));
      if (ns.empty() || ns == "SETTINGS_NS" || ns == "\"csi\"" || ns == "kNamespace" ||
          ns == "csi_module_settings_nvs::kNamespace") {
        ++quiet_calls;
      }
    }
  }
  for (const char* must : {"csi_integration.cpp", "csi_settings_nvs.cpp", "csi_event_egress.cpp",
                           "csi_mqtt.cpp", "canary_wap.ino", "csi_module_settings_nvs.h"}) {
    CHECK(std::find(scanned.begin(), scanned.end(), must) != scanned.end());
  }
  CHECK(quiet_calls >= 13);

  // Every quiet open, put back as a plain one, is caught; and so are the
  // other spellings of the namespace.
  size_t mutations = 0;
  for (const char* file : {"csi_integration.cpp", "csi_settings_nvs.cpp", "csi_event_egress.cpp",
                           "csi_mqtt.cpp", "canary_wap.ino"}) {
    const std::string src = read_source(file);
    const std::regex quiet(R"(csi_module_settings_nvs::begin_read_only\((\w+)(?:,\s*([^)]+))?\))");
    for (std::sregex_iterator it(src.begin(), src.end(), quiet), end; it != end; ++it) {
      const std::string ns = (*it)[2].matched ? (*it)[2].str() : std::string("csi_module_settings_nvs::kNamespace");
      std::string mutated = src;
      mutated.replace((size_t)it->position(0), (size_t)it->length(0),
                      (*it)[1].str() + ".begin(" + ns + ", /*readOnly=*/true)");
      ++mutations;
      if (plain_read_only_opens(mutated).empty()) {
        std::fprintf(stderr, "%s: putting back the plain open at offset %zu was not caught\n", file,
                     (size_t)it->position(0));
        CHECK(false);
      }
    }
  }
  CHECK(mutations == quiet_calls);
  CHECK(plain_read_only_opens("p.begin(\"csi\", true);").size() == 1);
  CHECK(plain_read_only_opens("p.begin(kNamespace,/*ro*/true);").size() == 1);
  CHECK(plain_read_only_opens("p.begin(\"csi\", false); p.begin(\"mesh\", true); "
                              "// p.begin(\"csi\", true);\n/* p.begin(SETTINGS_NS, true) */").empty());
  return 0;
}

int main() {
  if (test_a_saved_preset_applies_at_boot()) return 1;
  if (test_saved_thresholds_and_pet_mode_apply_at_boot()) return 1;
  if (test_a_stored_threshold_wins_over_the_saved_preset()) return 1;
  if (test_the_calibration_status_reports_what_the_module_runs()) return 1;
  if (test_a_saved_anomaly_cooldown_applies_at_boot()) return 1;
  if (test_init_runs_once_per_boot_and_a_change_still_applies()) return 1;
  if (test_a_never_written_namespace_opens_nothing_at_boot()) return 1;
  if (test_an_nvs_fault_costs_the_boot_one_open()) return 1;
  if (test_nothing_commits_during_the_boot_init()) return 1;
  if (test_the_model_is_register_v1_modules()) return 1;
  if (test_begin_read_only_answers_as_the_open_would()) return 1;
  if (test_a_first_boot_after_an_erase_logs_no_nvs_error()) return 1;
  if (test_every_read_only_open_of_csi_is_the_quiet_one()) return 1;
  std::printf("ALL wap_module_boot TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
