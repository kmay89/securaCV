// Host tests for the CSI modules' boot init on the canary-wap (sweep F93).
//
// csi_module_register() only records a module. Before F93 the canary-wap
// ran a module's init() only from reinit_module(), after /api/settings,
// /api/csi/calibrate/apply or a Tuning Lab change, so a preset, threshold,
// pet mode or anomaly cooldown saved in an earlier boot did nothing until
// the owner changed a setting again. csi_integration::init() now calls
// csi_module_init_all() right after register_v1_modules().
//
// What runs here is REAL: the canary-wap's settings readers
// (csi_settings_nvs.cpp, by the shared csi_module_settings_nvs.h rule), the
// staged CSI library (csi_event, csi_bundler, csi_module) and the staged
// modules, over a fake NVS that outlives a modeled reboot
// (stubs/module_boot/Preferences.h).
//
// What is modeled: csi_integration.cpp, which no host suite compiles. A boot
// here is its order: the event-id floor (apply_event_id_floor_from_nvs),
// register_v1_modules(), then csi_module_init_all(nullptr); a window is
// on_csi_window()'s csi_module_tick_all() and the loop's
// csi_bundler_tick(). check_wap_event_egress.py's rule 3 holds init() to
// that order. The model registers the modules register_v1_modules()
// registers, and this test reads that function to keep it so; the three
// feature-gated ones it leaves out (acoustic.events, vault.events,
// ble.scout) have empty init() bodies, which it also reads.
//
// Each test of a stored setting fails with csi_module_init_all() made a
// no-op (the canary-wap before F93, which ran no init at boot); the commit
// test passes there too, and guards the new boot path.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
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
  host_prefs().opens = 0;
  host_prefs().gets.clear();
  register_v1_modules_model();                // register_v1_modules()
  (void)csi_module_init_all(nullptr);         // F93
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
// each once. A second csi_module_init_all() reads nothing. A settings
// change still applies at once, through reinit_module()'s direct
// deinit() + init() (modeled), which the boot's latch does not stop.
static int test_init_runs_once_per_boot_and_a_change_still_applies() {
  host_prefs().clear();
  store_int("cp.preset", 0);
  reboot_and_boot();
  CHECK(host_prefs().opens == 16);
  CHECK(host_prefs().gets.size() == 16);
  for (const char* k : {"cp.pet_mode", "cp.preset", "cp.sens", "cp.mt", "cp.at", "cp.bt",
                        "cp.ps", "cp.srs", "cp.sdf", "cp.se", "cb.lt", "cb.cs",
                        "ab.sr", "ab.mm", "ab.mb", "ab.cd"}) {
    CHECK(host_prefs().gets_of(std::string("csi/") + k) == 1);
  }
  CHECK(csi_module_init_all(nullptr) == 0);
  CHECK(host_prefs().opens == 16);

  // /api/settings stores "quiet" (motion threshold 50) and reinits.
  store_int("cp.preset", 2);
  const csi_module_t* m = csi_module_find("core.presence");
  CHECK(m != nullptr);
  m->deinit();
  m->init(nullptr);
  hold(window_of(30));
  CHECK(!presence_open("subtle"));
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

int main() {
  if (test_a_saved_preset_applies_at_boot()) return 1;
  if (test_saved_thresholds_and_pet_mode_apply_at_boot()) return 1;
  if (test_a_saved_anomaly_cooldown_applies_at_boot()) return 1;
  if (test_init_runs_once_per_boot_and_a_change_still_applies()) return 1;
  if (test_nothing_commits_during_the_boot_init()) return 1;
  if (test_the_model_is_register_v1_modules()) return 1;
  std::printf("ALL wap_module_boot TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
