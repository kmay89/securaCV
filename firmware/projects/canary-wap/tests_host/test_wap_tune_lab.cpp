// Host tests for the canary-wap's Tuning Lab and its Quiet Hours (sweeps
// F123, F128).
//
// F123: the Tuning Lab's table declared core.quiet_hours.start_min 0 and
// end_min 480 (00:00 to 08:00) while the chokepoint's boot apply and
// GET /api/settings defaulted to 1380 and 420 (23:00 to 07:00). On a device
// that never stored them, /api/tune/coefficients and the exported bundle
// reported 00:00 to 08:00, and the Lab's reset buttons (and a bundle loaded
// back) stored those values. Now csi_settings_nvs.h holds the one default
// (kQuietHoursDefault*), the table declares it, and the boot apply and
// GET /api/settings read it through one reader (read_quiet_hours()).
//
// F128: a Quiet Hours knob changed in the Tuning Lab (or by a bundle import,
// the same handler) was stored but applied only at the next boot or the next
// Quiet Hours POST to /api/settings. tune_post() (csi_tune_lab.cpp) now
// re-applies the stored window to the chokepoint when a core.quiet_hours.*
// knob is stored, as /api/settings does, on the same (HTTP server) task.
//
// The dashboard's Quiet Hours (POST /api/settings) wrote "qh.en" /
// "qh.start" / "qh.end" by hand in csi_integration.cpp while the reader went
// through the shared key map, and no suite ran the writes. The store moved
// to csi_settings_nvs.cpp (store_quiet_hours_from_settings()), by the key
// map; it runs here against the reader and the Lab, the rows' stored names
// are pinned, and the handler is pinned to the store and its one apply.
//
// What runs here is REAL: the Tuning Lab's table and POST (csi_tune_lab.cpp),
// the Quiet Hours reader, store and apply (csi_settings_nvs.cpp), the staged CSI
// library (chokepoint, bundler, module registry) and the staged modules, over
// a fake NVS (stubs/module_boot). What is modeled: csi_integration.cpp, which
// no host suite compiles. Its handlers are thin around these functions, and
// the source pins at the end hold them to that (each pin is checked against
// in-memory mutations of the source, so it is known to bite): GET
// /api/settings reads Quiet Hours through read_quiet_hours(), and its POST
// stores them through store_quiet_hours_from_settings() and applies them
// once after closing NVS; the Tuning Lab POST hands
// its body to tune_post() with reinit_module(), whose body is the model's;
// the bundle import is that POST; the boot applies Quiet Hours; and the
// table and the apply are defined nowhere else.
//
// Against the code before F123 (the table's 0 / 480), the default tests
// fail; with tune_post() not re-applying Quiet Hours (the code before F128),
// the apply-at-once and bundle tests fail.
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
#include "csi_event.h"
#include "csi_module.h"
#include "csi_module_settings_nvs.h"
#include "csi_settings_nvs.h"
#include "csi_tune_lab.h"

#include "anomaly_baseline.h"
#include "core_breathing.h"
#include "core_presence.h"
#include "meta_quiet_hours.h"

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

// ── Time: the library's CLOCK_MONOTONIC ─────────────────────────────────
static uint32_t g_ms = 50000000;
extern "C" int clock_gettime(clockid_t, struct timespec* ts) noexcept {
  ts->tv_sec = (time_t)(g_ms / 1000);
  ts->tv_nsec = (long)((g_ms % 1000) * 1000000);
  return 0;
}

// The household minute of day the chokepoint compares Quiet Hours against
// (the loop keeps this offset on the device).
static void set_clock(int minute_of_day) {
  csi_event_set_clock_offset_minutes(minute_of_day - (int32_t)(g_ms / 60000));
}

// ── What commits ────────────────────────────────────────────────────────
static std::vector<std::string> g_commits;   // "<module>/<type>"
static uint16_t g_last_summary_count = 0;
extern "C" void csi_event_on_committed(uint32_t, const char* module_id, const char* type_name,
                                       csi_event_category_t, csi_privacy_class_t,
                                       const csi_event_values_t* values) {
  g_commits.push_back(std::string(module_id) + "/" + type_name);
  if (std::strcmp(type_name, "held_summary") == 0 && values) {
    g_last_summary_count = values->bundled_count;
  }
}
extern "C" void csi_event_on_id_advance(uint32_t) {}

// A stateless row (no state name, so the bundler commits it at once) from
// a module with no hourly ceiling. Quiet Hours holds it while the window is
// active; returns true when it committed.
static const csi_event_decl_t kPingEvents[] = {
  {"ping", CSI_FIELD_TIME_BUCKET, CSI_PRIVACY_P0, 0},
};
static void ping_tick(const csi_features_t*) {}
static const csi_module_t kPing = {
  "test.ping", CSI_PRIVACY_P0, kPingEvents, 1, nullptr, ping_tick, nullptr, nullptr,
};
static bool ping_commits() {
  const size_t before = g_commits.size();
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_TIME_BUCKET;
  (void)csi_event_emit("test.ping", "ping", &v);
  for (size_t i = before; i < g_commits.size(); ++i) {
    if (g_commits[i] == "test.ping/ping") return true;
  }
  return false;
}

// ── reinit_module(), as csi_integration.cpp defines it (pinned below) ───
static std::vector<std::string> g_reinits;
static void reinit_module_model(const char* module_id) {
  g_reinits.push_back(module_id);
  const csi_module_t* m = csi_module_find(module_id);
  if (!m) return;
  if (m->deinit) m->deinit();
  if (m->init)   m->init(nullptr);
}

// ── A boot (csi_integration::init's order, the parts that matter here) ──
// RAM gone, NVS kept: the modules register, their boot init runs, and
// register_v1_modules() applies the stored Quiet Hours. g_registered counts
// the registrations the registry accepted (each test checks all five).
static int g_registered = 0;
static void reboot_and_boot() {
  csi_event_test_reset();
  csi_module_test_reset();
  g_registered = 0;
  g_registered += csi_module_register(core_presence_module());
  g_registered += csi_module_register(core_breathing_module());
  g_registered += csi_module_register(meta_quiet_hours_module());
  g_registered += csi_module_register(anomaly_baseline_module());
  g_registered += csi_module_register(&kPing);
  (void)csi_settings_nvs_init_modules();
  apply_quiet_hours_from_nvs();
  g_commits.clear();
  g_reinits.clear();
  g_last_summary_count = 0;
}

// The stored rows, read as GET /api/tune/coefficients and
// GET /api/tune/preset read them (tune_read_value over a read-only handle).
static int32_t lab_value(const char* full_key) {
  const TuneCoeff* c = tune_coeff_for(full_key);
  if (c == nullptr) return -1;
  Preferences prefs;
  if (!prefs.begin(csi_module_settings_nvs::kNamespace, /*readOnly=*/true)) return c->default_v;
  const int32_t v = tune_read_value(prefs, *c);
  prefs.end();
  return v;
}

// ── F123: one default ───────────────────────────────────────────────────

// The Lab declares the device's own Quiet Hours default: off, 23:00 to
// 07:00, as read_quiet_hours() (the boot apply and GET /api/settings) reads
// a store that never held them. What a never-configured device reports to
// the Lab, and exports in its bundle, is that window.
static int test_the_lab_declares_the_device_quiet_hours_default() {
  CHECK(kQuietHoursDefaultEnabled == false);
  CHECK(kQuietHoursDefaultStartMin == 23 * 60);
  CHECK(kQuietHoursDefaultEndMin == 7 * 60);

  const TuneCoeff* en = tune_coeff_for("core.quiet_hours.enabled");
  const TuneCoeff* start = tune_coeff_for("core.quiet_hours.start_min");
  const TuneCoeff* end = tune_coeff_for("core.quiet_hours.end_min");
  CHECK(en != nullptr && start != nullptr && end != nullptr);
  CHECK(start->default_v == kQuietHoursDefaultStartMin);
  CHECK(end->default_v == kQuietHoursDefaultEndMin);
  CHECK(en->default_v == (kQuietHoursDefaultEnabled ? 1 : 0));

  host_prefs().clear();
  host_prefs().created.insert("csi");          // the namespace, no Quiet Hours rows
  Preferences prefs;
  CHECK(prefs.begin("csi", /*readOnly=*/true));
  const QuietHours qh = read_quiet_hours(prefs);
  prefs.end();
  CHECK(qh.enabled == kQuietHoursDefaultEnabled);
  CHECK(qh.start_min == kQuietHoursDefaultStartMin);
  CHECK(qh.end_min == kQuietHoursDefaultEndMin);

  CHECK(lab_value("core.quiet_hours.start_min") == qh.start_min);
  CHECK(lab_value("core.quiet_hours.end_min") == qh.end_min);
  CHECK(lab_value("core.quiet_hours.enabled") == (qh.enabled ? 1 : 0));
  host_prefs().clear();
  return 0;
}

// The Lab's reset buttons store each row's declared default. An owner who
// turns Quiet Hours on in the Lab and resets its start and end gets the
// window the dashboard shows, 23:00 to 07:00: from the next boot on, a row
// at 23:30 is held and one at 07:30 is not. (Through a reboot, so this
// holds whether or not the Lab applies at once; F128 is tested below.)
// Before F123 the reset stored 00:00 to 08:00: the 23:30 row went out and
// the 07:30 one was held.
static int test_resetting_the_lab_window_keeps_the_device_window() {
  host_prefs().clear();
  reboot_and_boot();
  CHECK(g_registered == 5);
  set_clock(12 * 60);
  CHECK(ping_commits());                       // Quiet Hours off: a row goes out
  char body[192];
  std::snprintf(body, sizeof(body),
                "{\"core.quiet_hours.enabled\":true,\"core.quiet_hours.start_min\":%ld,"
                "\"core.quiet_hours.end_min\":%ld}",
                (long)tune_coeff_for("core.quiet_hours.start_min")->default_v,
                (long)tune_coeff_for("core.quiet_hours.end_min")->default_v);
  const TunePost post = tune_post(body, reinit_module_model);
  CHECK(post.nvs_ok);
  CHECK(post.changed == 3);

  reboot_and_boot();
  set_clock(23 * 60 + 30);
  CHECK(!ping_commits());                      // inside 23:00-07:00
  reboot_and_boot();
  set_clock(7 * 60 + 30);
  CHECK(ping_commits());                       // outside it
  host_prefs().clear();
  return 0;
}

// The dashboard's own first values (before GET /api/settings answers, and
// when it cannot) are the same window.
static std::string read_source(const char* name);
static int test_the_dashboard_starts_from_the_same_window() {
  const std::string dash = read_source("csi_dashboard_html.h");
  CHECK(!dash.empty());
  std::smatch m;
  CHECK(std::regex_search(dash, m, std::regex(R"(window\.QH_ENABLED\s*=\s*(true|false)\s*;)")));
  CHECK((m[1].str() == "true") == kQuietHoursDefaultEnabled);
  CHECK(std::regex_search(dash, m, std::regex(R"(window\.QH_START_MIN\s*=\s*(\d+)\s*\*\s*60\s*;)")));
  CHECK(std::stol(m[1].str()) * 60 == kQuietHoursDefaultStartMin);
  CHECK(std::regex_search(dash, m, std::regex(R"(window\.QH_END_MIN\s*=\s*(\d+)\s*\*\s*60\s*;)")));
  CHECK(std::stol(m[1].str()) * 60 == kQuietHoursDefaultEndMin);
  CHECK(std::regex_search(dash, m, std::regex(R"re(id="qhStart"\s+value="(\d\d):(\d\d)")re")));
  CHECK(std::stol(m[1].str()) * 60 + std::stol(m[2].str()) == kQuietHoursDefaultStartMin);
  CHECK(std::regex_search(dash, m, std::regex(R"re(id="qhEnd"\s+value="(\d\d):(\d\d)")re")));
  CHECK(std::stol(m[1].str()) * 60 + std::stol(m[2].str()) == kQuietHoursDefaultEndMin);
  return 0;
}

// ── F128: a Lab Quiet Hours change applies at once ──────────────────────

// Quiet Hours off; at 12:00 the owner turns it on in the Lab for 11:30 to
// 12:30. The next row is held, with no reboot and no /api/settings POST,
// and no module re-ran its init (no module reads these knobs). Moving the
// start past now applies at once too, and turning it off closes the window:
// the next emit commits the held summary, then the row.
static int test_a_lab_quiet_hours_change_applies_at_once() {
  host_prefs().clear();
  reboot_and_boot();
  set_clock(12 * 60);
  CHECK(ping_commits());

  TunePost post = tune_post("{\"core.quiet_hours.enabled\":true,"
                            "\"core.quiet_hours.start_min\":690,"
                            "\"core.quiet_hours.end_min\":750}",
                            reinit_module_model);
  CHECK(post.nvs_ok);
  CHECK(post.changed == 3);
  CHECK(post.quiet_hours);
  CHECK(!post.reinit_presence && !post.reinit_breathing && !post.reinit_anomaly);
  CHECK(g_reinits.empty());
  CHECK(!ping_commits());                      // held, at once
  CHECK(!ping_commits());

  // One knob alone: the start moves to 12:05, so 12:00 is outside.
  post = tune_post("{\"core.quiet_hours.start_min\":725}", reinit_module_model);
  CHECK(post.changed == 1);
  CHECK(post.quiet_hours);
  CHECK(ping_commits());
  CHECK(g_last_summary_count == 2);            // the two held rows, summarized
  set_clock(12 * 60 + 10);
  CHECK(!ping_commits());                      // 12:10 is inside 12:05-12:30

  // Off, from the Lab: the window closes at once.
  g_commits.clear();
  post = tune_post("{\"core.quiet_hours.enabled\":false}", reinit_module_model);
  CHECK(post.quiet_hours);
  CHECK(ping_commits());
  CHECK(g_commits.size() == 2);
  CHECK(g_commits[0] == "meta.quiet_hours/held_summary");
  CHECK(g_last_summary_count == 1);
  host_prefs().clear();
  return 0;
}

// A bundle import (POST /api/tune/preset is the same handler) carries every
// knob, in the flat shape GET /api/tune/preset streams. It stores them all,
// re-runs each module's init() once, and applies its Quiet Hours at once.
static int test_a_bundle_import_applies_every_knob() {
  host_prefs().clear();
  reboot_and_boot();
  set_clock(3 * 60);                           // 03:00
  CHECK(ping_commits());

  std::string bundle = "{";
  for (size_t i = 0; i < TUNE_COEFF_COUNT; ++i) {
    const TuneCoeff& c = TUNE_COEFFS[i];
    int32_t v = c.default_v;
    if (std::strcmp(c.full_key, "core.quiet_hours.enabled") == 0) v = 1;
    if (std::strcmp(c.full_key, "core.presence.preset") == 0) v = 0;
    if (i) bundle += ",";
    bundle += "\"" + std::string(c.full_key) + "\":" + std::to_string(v);
  }
  bundle += "}";
  const TunePost post = tune_post(bundle.c_str(), reinit_module_model);
  CHECK(post.nvs_ok);
  CHECK(post.changed == (int)TUNE_COEFF_COUNT);
  CHECK(post.reinit_presence && post.reinit_breathing && post.reinit_anomaly);
  CHECK(post.quiet_hours);
  CHECK((g_reinits == std::vector<std::string>{"core.presence", "core.breathing",
                                               "anomaly.baseline"}));
  CHECK(!ping_commits());                      // 03:00, inside the default window
  CHECK(lab_value("core.presence.preset") == 0);
  CHECK(lab_value("core.quiet_hours.end_min") == kQuietHoursDefaultEndMin);
  host_prefs().clear();
  return 0;
}

// ── The dashboard's Quiet Hours and the Lab's are the same rows ─────────

// POST /api/settings stores its "quiet_hours" object through
// store_quiet_hours_from_settings(), then (when it stored a row) applies
// with apply_quiet_hours_from_nvs(), as the pinned handler does. What it
// stores is what GET /api/settings (read_quiet_hours()), the boot apply and
// the Tuning Lab read, and what the Lab stores is what the dashboard reads.
// Only the object's own fields count, minutes clamp, a string value reads as
// the bare one, and the body comes back unchanged. Before, the handler
// wrote "qh.en" / "qh.start" / "qh.end" by hand while the reader went
// through the key map, and nothing ran the writes: a key spelled
// differently on one side saved a window nothing read.
static bool dashboard_post(const char* json) {
  char body[384];
  std::snprintf(body, sizeof(body), "%s", json);
  Preferences prefs;
  if (!prefs.begin(csi_module_settings_nvs::kNamespace, /*readOnly=*/false)) return false;
  const bool qh_changed = store_quiet_hours_from_settings(prefs, body);
  prefs.end();
  if (std::strcmp(body, json) != 0) return false;   // the body comes back as it was
  if (qh_changed) apply_quiet_hours_from_nvs();
  return qh_changed;
}

static int test_the_dashboard_writes_the_rows_the_device_reads() {
  host_prefs().clear();
  reboot_and_boot();
  set_clock(12 * 60);
  CHECK(ping_commits());

  // The dashboard turns Quiet Hours on for 11:30 to 12:30.
  CHECK(dashboard_post("{\"pet_mode\":true,\"quiet_hours\":{\"enabled\":true,"
                       "\"start_min\":690,\"end_min\":750},\"privacy_ceiling\":\"p0\"}"));
  CHECK(!ping_commits());                      // applied at once
  {
    Preferences prefs;
    CHECK(prefs.begin("csi", /*readOnly=*/true));
    const QuietHours qh = read_quiet_hours(prefs);   // GET /api/settings
    prefs.end();
    CHECK(qh.enabled && qh.start_min == 690 && qh.end_min == 750);
  }
  CHECK(lab_value("core.quiet_hours.enabled") == 1);   // the Lab reads them
  CHECK(lab_value("core.quiet_hours.start_min") == 690);
  CHECK(lab_value("core.quiet_hours.end_min") == 750);
  reboot_and_boot();                           // and so does the boot
  set_clock(12 * 60);
  CHECK(!ping_commits());

  // The Lab moves the start; the dashboard's GET reads the Lab's row.
  CHECK(tune_post("{\"core.quiet_hours.start_min\":725}", reinit_module_model).changed == 1);
  {
    Preferences prefs;
    CHECK(prefs.begin("csi", /*readOnly=*/true));
    CHECK(read_quiet_hours(prefs).start_min == 725);
    prefs.end();
  }

  // Only the object's own fields: a top-level "enabled" is not Quiet Hours.
  // Minutes clamp; a string value reads as the bare one.
  CHECK(dashboard_post("{\"enabled\":false,\"quiet_hours\":{\"start_min\":-5,\"end_min\":\"5000\"}}"));
  {
    Preferences prefs;
    CHECK(prefs.begin("csi", /*readOnly=*/true));
    const QuietHours qh = read_quiet_hours(prefs);
    prefs.end();
    CHECK(qh.enabled);
    CHECK(qh.start_min == 0);
    CHECK(qh.end_min == 1439);
  }

  // Nothing to store: no object, or no field it can read.
  const size_t rows = host_prefs().i32.size() + host_prefs().flag.size();
  CHECK(!dashboard_post("{\"pet_mode\":false}"));
  CHECK(!dashboard_post("{\"quiet_hours\":{\"enabled\":\"maybe\",\"start_min\":\"soon\"}}"));
  CHECK(!dashboard_post("{\"quiet_hours\":true,\"enabled\":true}"));
  CHECK(host_prefs().i32.size() + host_prefs().flag.size() == rows);

  // Off, from the dashboard: the window closes at once.
  set_clock(12 * 60 + 10);
  CHECK(!ping_commits());
  CHECK(dashboard_post("{\"quiet_hours\":{\"enabled\":false}}"));
  CHECK(ping_commits());
  host_prefs().clear();
  return 0;
}

// Devices hold their Quiet Hours under these names: the dashboard wrote them
// by hand before the shared key map spelled them, and both writers and the
// reader now take them from the map. Renaming a row (in the map, so on
// every side at once) would agree with itself and lose every saved window
// at the upgrade.
static int test_the_quiet_hours_rows_keep_their_stored_names() {
  using csi_module_settings_nvs::nvs_key_for;
  CHECK(std::strcmp(csi_module_settings_nvs::kNamespace, "csi") == 0);
  CHECK(nvs_key_for("core.quiet_hours.enabled") != nullptr);
  CHECK(std::strcmp(nvs_key_for("core.quiet_hours.enabled"), "qh.en") == 0);
  CHECK(nvs_key_for("core.quiet_hours.start_min") != nullptr);
  CHECK(std::strcmp(nvs_key_for("core.quiet_hours.start_min"), "qh.start") == 0);
  CHECK(nvs_key_for("core.quiet_hours.end_min") != nullptr);
  CHECK(std::strcmp(nvs_key_for("core.quiet_hours.end_min"), "qh.end") == 0);

  // A window an older image saved under those names is the one read.
  host_prefs().clear();
  host_prefs().flag["csi/qh.en"] = true;
  host_prefs().i32["csi/qh.start"] = 600;
  host_prefs().i32["csi/qh.end"] = 660;
  Preferences prefs;
  CHECK(prefs.begin("csi", /*readOnly=*/true));
  const QuietHours qh = read_quiet_hours(prefs);
  prefs.end();
  CHECK(qh.enabled && qh.start_min == 600 && qh.end_min == 660);
  CHECK(lab_value("core.quiet_hours.start_min") == 600);
  host_prefs().clear();
  return 0;
}

// ── F151: the dashboard's and the calibration's presence rows ───────────
//
// POST /api/settings stored the dashboard's pet mode, preset and sensitivity
// as "cp.pet_mode", "cp.preset" and "cp.sens", and the calibration's apply
// its thresholds as "cp.mt", "cp.at" and "cp.bt", by literal key in
// csi_integration.cpp, while core.presence reads them through the shared
// key map. No suite compiled the handlers: renaming the POST's "cp.sens" to
// "cp.sen" left every canary-wap host suite, check_csi_sync.sh and
// regression_check.sh green (probed on #1762's head), and on a device the
// slider would have saved a value no module read. The stores and readers
// moved to csi_settings_nvs.cpp, by the key map, and run here against the
// module's own read (csi_module_settings_int / _bool with no session, as
// reinit_module()'s init(nullptr) reads) and the Tuning Lab's; the source
// pins below hold the handlers to them and keep any module setting's NVS
// key out of every other sketch source. The privacy ceiling's "cp.pc" was
// spelled by hand in three places too, and goes the same way.

// A dashboard POST body through the presence store, as the handler runs it.
static bool presence_post(const char* json) {
  Preferences prefs;
  if (!prefs.begin(csi_module_settings_nvs::kNamespace, /*readOnly=*/false)) return false;
  const bool stored = store_presence_from_settings(prefs, json);
  prefs.end();
  return stored;
}

static PresenceSettings get_presence() {
  Preferences prefs;
  PresenceSettings s = {false, -1, -1};
  if (prefs.begin(csi_module_settings_nvs::kNamespace, /*readOnly=*/true)) {
    s = read_presence_settings(prefs);
    prefs.end();
  }
  return s;
}

static int test_the_dashboard_writes_the_presence_rows_the_module_reads() {
  host_prefs().clear();
  reboot_and_boot();
  // Nothing stored: GET reports core.presence's own defaults.
  host_prefs().created.insert("csi");
  PresenceSettings s = get_presence();
  CHECK(!s.pet_mode && s.preset == 1 && s.sensitivity == 50);
  CHECK(s.preset == csi_module_settings_int(nullptr, "core.presence.preset", 1));
  CHECK(s.sensitivity == csi_module_settings_int(nullptr, "core.presence.sensitivity", 50));

  // The dashboard sends each key alone (csi_dashboard_html.h's
  // persistPetMode, persistPreset and persistSensitivity): each body by
  // itself is a stored write, or the handler answers 400 "no recognized
  // keys" and does not re-run core.presence (F151's review: three of the
  // four could drop their flag with every suite green).
  CHECK(presence_post("{\"pet_mode\":true}"));
  CHECK(csi_module_settings_bool(nullptr, "core.presence.pet_mode", false));
  CHECK(presence_post("{\"pet_mode\":false}"));
  CHECK(!csi_module_settings_bool(nullptr, "core.presence.pet_mode", true));
  CHECK(presence_post("{\"preset\":\"quiet\"}"));
  CHECK(csi_module_settings_int(nullptr, "core.presence.preset", -1) == 2);
  CHECK(presence_post("{\"preset\":\"balanced\"}"));
  CHECK(csi_module_settings_int(nullptr, "core.presence.preset", -1) == 1);
  CHECK(presence_post("{\"preset\":\"sensitive\"}"));
  CHECK(csi_module_settings_int(nullptr, "core.presence.preset", -1) == 0);
  CHECK(presence_post("{\"sensitivity\":40}"));
  CHECK(csi_module_settings_int(nullptr, "core.presence.sensitivity", -1) == 40);
  host_prefs().clear();
  host_prefs().created.insert("csi");

  // The dashboard saves all three; a value sent as a string reads as the
  // bare one.
  CHECK(presence_post("{\"pet_mode\":true,\"preset\":\"sensitive\",\"sensitivity\":\"75\"}"));
  // What core.presence's init() reads (the key map, no session: the
  // re-init after the POST), what the Lab reads, and what GET reports.
  CHECK(csi_module_settings_bool(nullptr, "core.presence.pet_mode", false));
  CHECK(csi_module_settings_int(nullptr, "core.presence.preset", -1) == 0);
  CHECK(csi_module_settings_int(nullptr, "core.presence.sensitivity", -1) == 75);
  CHECK(lab_value("core.presence.pet_mode") == 1);
  CHECK(lab_value("core.presence.preset") == 0);
  CHECK(lab_value("core.presence.sensitivity") == 75);
  s = get_presence();
  CHECK(s.pet_mode && s.preset == 0 && s.sensitivity == 75);
  // Under the names devices hold them under.
  CHECK(host_prefs().flag.count("csi/cp.pet_mode") == 1 && host_prefs().flag["csi/cp.pet_mode"]);
  CHECK(host_prefs().i32.count("csi/cp.preset") == 1 && host_prefs().i32["csi/cp.preset"] == 0);
  CHECK(host_prefs().i32.count("csi/cp.sens") == 1 && host_prefs().i32["csi/cp.sens"] == 75);

  // One key at a time, clamped; the other rows stay.
  CHECK(presence_post("{\"sensitivity\":250}"));
  CHECK(csi_module_settings_int(nullptr, "core.presence.sensitivity", -1) == 100);
  CHECK(presence_post("{\"sensitivity\":-4}"));
  CHECK(csi_module_settings_int(nullptr, "core.presence.sensitivity", -1) == 0);
  CHECK(presence_post("{\"preset\":\"quiet\",\"pet_mode\":false}"));
  CHECK(csi_module_settings_int(nullptr, "core.presence.preset", -1) == 2);
  CHECK(!csi_module_settings_bool(nullptr, "core.presence.pet_mode", true));

  // Nothing it can read stores nothing: an unknown preset, a key inside
  // another, a value that is not one.
  const size_t rows = host_prefs().i32.size() + host_prefs().flag.size();
  CHECK(!presence_post("{\"preset\":\"loud\"}"));
  CHECK(!presence_post("{\"not_pet_mode\":true,\"sensitivity\":\"high\"}"));
  CHECK(!presence_post("{\"quiet_hours\":{\"enabled\":true}}"));
  CHECK(host_prefs().i32.size() + host_prefs().flag.size() == rows);
  CHECK(csi_module_settings_int(nullptr, "core.presence.preset", -1) == 2);
  host_prefs().clear();
  return 0;
}

static int test_the_calibration_writes_the_thresholds_the_module_reads() {
  host_prefs().clear();
  reboot_and_boot();
  host_prefs().created.insert("csi");
  {
    Preferences prefs;
    CHECK(prefs.begin("csi", /*readOnly=*/true));
    const PresenceThresholds t = read_presence_thresholds(prefs);   // the status's "current"
    prefs.end();
    CHECK(t.motion == 35 && t.active == 75 && t.breathing == 30);
    // ...which is what the status answers when NVS does not open too.
    const PresenceThresholds d = presence_threshold_defaults();
    CHECK(d.motion == t.motion && d.active == t.active && d.breathing == t.breathing);
  }
  {
    Preferences prefs;
    CHECK(prefs.begin("csi", /*readOnly=*/false));
    const PresenceThresholds proposed = {41, 83, 27};
    CHECK(store_presence_thresholds(prefs, proposed));
    prefs.end();
  }
  CHECK(csi_module_settings_int(nullptr, "core.presence.motion_threshold", -1) == 41);
  CHECK(csi_module_settings_int(nullptr, "core.presence.active_threshold", -1) == 83);
  CHECK(csi_module_settings_int(nullptr, "core.presence.breathing_threshold", -1) == 27);
  CHECK(lab_value("core.presence.motion_threshold") == 41);
  CHECK(lab_value("core.presence.active_threshold") == 83);
  CHECK(lab_value("core.presence.breathing_threshold") == 27);
  {
    Preferences prefs;
    CHECK(prefs.begin("csi", /*readOnly=*/true));
    const PresenceThresholds t = read_presence_thresholds(prefs);
    prefs.end();
    CHECK(t.motion == 41 && t.active == 83 && t.breathing == 27);
  }
  CHECK(host_prefs().i32["csi/cp.mt"] == 41);
  CHECK(host_prefs().i32["csi/cp.at"] == 83);
  CHECK(host_prefs().i32["csi/cp.bt"] == 27);
  // NVS that takes no row reports it.
  {
    Preferences prefs;
    CHECK(prefs.begin("csi", /*readOnly=*/true));
    const PresenceThresholds proposed = {50, 90, 40};
    CHECK(!store_presence_thresholds(prefs, proposed));   // a read-only handle stores nothing
    prefs.end();
  }
  CHECK(csi_module_settings_int(nullptr, "core.presence.motion_threshold", -1) == 41);
  host_prefs().clear();
  return 0;
}

static int test_the_privacy_ceiling_is_one_row() {
  host_prefs().clear();
  reboot_and_boot();
  csi_event_set_privacy_ceiling(CSI_PRIVACY_P0);
  auto post = [](const char* json) {
    Preferences prefs;
    if (!prefs.begin("csi", /*readOnly=*/false)) return false;
    const bool stored = store_privacy_ceiling_from_settings(prefs, json);
    prefs.end();
    return stored;
  };
  auto get = []() {
    Preferences prefs;
    if (!prefs.begin("csi", /*readOnly=*/true)) return (int32_t)-1;
    const int32_t v = read_privacy_ceiling(prefs);
    prefs.end();
    return v;
  };
  host_prefs().created.insert("csi");
  CHECK(get() == (int32_t)CSI_PRIVACY_P0);                 // nothing stored: P0
  CHECK(post("{\"pet_mode\":true,\"privacy_ceiling\":\"p2\"}"));
  CHECK(get() == (int32_t)CSI_PRIVACY_P2);                 // GET /api/settings
  CHECK(host_prefs().i32["csi/cp.pc"] == (int32_t)CSI_PRIVACY_P2);
  apply_privacy_ceiling_from_nvs();                        // the boot, and the POST's apply
  CHECK(csi_event_get_privacy_ceiling() == CSI_PRIVACY_P2);
  CHECK(!post("{\"privacy_ceiling\":\"p9\"}"));            // ignored: the row survives
  CHECK(!post("{\"privacy\":\"p1\"}"));
  CHECK(get() == (int32_t)CSI_PRIVACY_P2);
  CHECK(post("{\"privacy_ceiling\": \"p1\"}"));
  apply_privacy_ceiling_from_nvs();
  CHECK(csi_event_get_privacy_ceiling() == CSI_PRIVACY_P1);
  host_prefs().i32["csi/cp.pc"] = 7;                       // out of range: P0, never more
  apply_privacy_ceiling_from_nvs();
  CHECK(csi_event_get_privacy_ceiling() == CSI_PRIVACY_P0);
  csi_event_set_privacy_ceiling(CSI_PRIVACY_P0);
  host_prefs().clear();
  return 0;
}

// ── The table: every knob applies at once, to its own group ─────────────
static int test_every_knob_applies_at_once_to_its_group() {
  for (size_t i = 0; i < TUNE_COEFF_COUNT; ++i) {
    const TuneCoeff& c = TUNE_COEFFS[i];
    const std::string group = c.group;
    CHECK(std::string(c.full_key).compare(0, group.size() + 1, group + ".") == 0);
    CHECK(csi_module_settings_nvs::nvs_key_for(c.full_key) != nullptr);
    CHECK(c.min_v <= c.default_v && c.default_v <= c.max_v);
    CHECK(tune_coeff_for(c.full_key) == &c);
    TuneApply want = TA_QUIET_HOURS;
    if (group == "core.presence") want = TA_REINIT_PRESENCE;
    else if (group == "core.breathing") want = TA_REINIT_BREATHING;
    else if (group == "anomaly.baseline") want = TA_REINIT_ANOMALY;
    else CHECK(group == "core.quiet_hours");
    CHECK(c.apply == want);

    // One knob, POSTed as the Lab's slider does: exactly its group applies.
    host_prefs().clear();
    reboot_and_boot();
    const std::string body = "{\"" + std::string(c.full_key) + "\":" + std::to_string(c.default_v) + "}";
    const TunePost post = tune_post(body.c_str(), reinit_module_model);
    CHECK(post.changed == 1);
    CHECK(post.reinit_presence == (want == TA_REINIT_PRESENCE));
    CHECK(post.reinit_breathing == (want == TA_REINIT_BREATHING));
    CHECK(post.reinit_anomaly == (want == TA_REINIT_ANOMALY));
    CHECK(post.quiet_hours == (want == TA_QUIET_HOURS));
    if (want == TA_QUIET_HOURS) {
      CHECK(g_reinits.empty());
    } else {
      CHECK((g_reinits == std::vector<std::string>{group}));
    }
  }
  host_prefs().clear();
  return 0;
}

// What the POST refuses or ignores, as before: NVS that will not open
// stores and applies nothing; an unknown key (or a known key's name inside
// another) changes nothing; a value past a knob's range is clamped.
static int test_the_post_stores_only_known_knobs() {
  host_prefs().clear();
  reboot_and_boot();
  host_prefs().fail_begin = true;
  TunePost post = tune_post("{\"core.quiet_hours.enabled\":true}", reinit_module_model);
  CHECK(!post.nvs_ok);
  CHECK(post.changed == 0);
  CHECK(!post.quiet_hours);
  host_prefs().fail_begin = false;

  post = tune_post("{\"core.presence.bogus\":1,\"not_core.presence.pet_mode\":true}",
                   reinit_module_model);
  CHECK(post.nvs_ok);
  CHECK(post.changed == 0);
  CHECK(g_reinits.empty());
  CHECK(!post.quiet_hours);

  post = tune_post("{\"core.quiet_hours.start_min\":5000}", reinit_module_model);
  CHECK(post.changed == 1);
  CHECK(lab_value("core.quiet_hours.start_min") == 1439);
  host_prefs().clear();
  return 0;
}

// ── Source pins: csi_integration.cpp is thin around the above ───────────
static std::string read_source(const char* name) {
  std::ifstream in(std::string(WAP_SKETCH_DIR) + "/" + name);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The body of the first definition whose header matches `head` (a regex
// ending at its opening brace), comments and whitespace out; "" if none.
static std::string code_body(const std::string& src, const std::string& head) {
  std::smatch m;
  if (!std::regex_search(src, m, std::regex(head))) return "";
  size_t at = (size_t)(m.position(0) + m.length(0)) - 1;
  int depth = 0;
  for (size_t j = at; j < src.size(); ++j) {
    if (src[j] == '{') ++depth;
    if (src[j] == '}' && --depth == 0) {
      std::string body = src.substr(at + 1, j - at - 1);
      body = std::regex_replace(body, std::regex(R"(/\*[\s\S]*?\*/)"), "");
      body = std::regex_replace(body, std::regex(R"(//[^\n]*)"), "");
      return std::regex_replace(body, std::regex(R"(\s+)"), "");
    }
  }
  return "";
}

static size_t count_of(const std::string& s, const std::string& what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
  return n;
}

// The string literals of a C++ source, comments skipped (a raw string's
// body is one literal). Hand-rolled rather than std::regex: some sketch
// sources are large, and libstdc++'s regex recurses per character.
static std::vector<std::string> string_literals(const std::string& s) {
  std::vector<std::string> out;
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    const char c = s[i];
    if (c == '/' && i + 1 < n && s[i + 1] == '/') {
      i = s.find('\n', i);
      if (i == std::string::npos) break;
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      i = s.find("*/", i + 2);
      if (i == std::string::npos) break;
      i += 2;
      continue;
    }
    if (c == 'R' && i + 1 < n && s[i + 1] == '"' &&
        (i == 0 || !(std::isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_'))) {
      const size_t open = s.find('(', i + 2);
      if (open == std::string::npos) break;
      const std::string close = ")" + s.substr(i + 2, open - (i + 2)) + "\"";
      const size_t end = s.find(close, open + 1);
      if (end == std::string::npos) break;
      out.push_back(s.substr(open + 1, end - open - 1));
      i = end + close.size();
      continue;
    }
    if (c == '\'' && !(i > 0 && std::isxdigit((unsigned char)s[i - 1]) && i + 1 < n &&
                       std::isxdigit((unsigned char)s[i + 1]))) {   // not a digit separator
      for (++i; i < n && s[i] != '\''; ++i) {
        if (s[i] == '\\') ++i;
      }
      ++i;
      continue;
    }
    if (c == '"') {
      std::string lit;
      for (++i; i < n && s[i] != '"'; ++i) {
        if (s[i] == '\\' && i + 1 < n) lit += s[i++];
        lit += s[i];
      }
      ++i;
      out.push_back(lit);
      continue;
    }
    ++i;
  }
  return out;
}

// The NVS keys of the shared key map's rows start with a module's short
// prefix ("cp." core.presence, "cb." core.breathing, "qh." Quiet Hours,
// "ab." anomaly.baseline). A literal in a sketch source that starts with
// one spells a module setting's key by hand (sweep F151): it must come from
// the map (nvs_key_for()), which the module's own reads use, or a rename on
// one side saves a value no module reads. Returns each such literal.
static std::vector<std::string> hand_spelled_setting_keys(const std::string& src) {
  std::vector<std::string> prefixes;
  for (size_t i = 0; i < csi_module_settings_nvs::kKeyCount; ++i) {
    const std::string nvs = csi_module_settings_nvs::kKeys[i].nvs;
    const size_t dot = nvs.find('.');
    if (dot == std::string::npos) continue;
    const std::string prefix = nvs.substr(0, dot + 1);
    if (std::find(prefixes.begin(), prefixes.end(), prefix) == prefixes.end()) prefixes.push_back(prefix);
  }
  std::vector<std::string> out;
  for (const std::string& lit : string_literals(src)) {
    for (const std::string& prefix : prefixes) {
      if (lit.compare(0, prefix.size(), prefix) == 0 && lit.size() > prefix.size() &&
          lit.size() <= 15 && lit.find(' ') == std::string::npos) {
        out.push_back(lit);
      }
    }
  }
  return out;
}

static const char* const kCalibApply = R"(\besp_err_t\s+handle_calibrate_apply\s*\(\s*httpd_req_t\s*\*\s*req\s*\)\s*\{)";
static const char* const kCalibStatus = R"(\besp_err_t\s+handle_calibrate_status\s*\(\s*httpd_req_t\s*\*\s*req\s*\)\s*\{)";
static const char* const kSettingsGet = R"(\besp_err_t\s+handle_settings_get\s*\(\s*httpd_req_t\s*\*\s*req\s*\)\s*\{)";
static const char* const kSettingsPost = R"(\besp_err_t\s+handle_settings_post\s*\(\s*httpd_req_t\s*\*\s*req\s*\)\s*\{)";
static const char* const kTunePost = R"(\besp_err_t\s+handle_tune_post_coefficients\s*\(\s*httpd_req_t\s*\*\s*req\s*\)\s*\{)";
static const char* const kTunePreset = R"(\besp_err_t\s+handle_tune_post_preset\s*\(\s*httpd_req_t\s*\*\s*req\s*\)\s*\{)";
static const char* const kRegister = R"(\bvoid\s+register_v1_modules\s*\(\s*\)\s*\{)";
static const char* const kReinit = R"(\bvoid\s+reinit_module\s*\(\s*const\s+char\s*\*\s*module_id\s*\)\s*\{)";

// Every way csi_integration.cpp could stop being thin around the tested
// code; empty when it is.
static std::vector<std::string> pin_problems(const std::string& integ) {
  std::vector<std::string> out;
  const std::string get = code_body(integ, kSettingsGet);
  if (get.empty() || count_of(get, "read_quiet_hours(prefs)") != 1 || get.find("\"qh.") != std::string::npos) {
    out.push_back("GET /api/settings reads Quiet Hours other than through read_quiet_hours(prefs)");
  }
  // POST /api/settings: the dashboard's Quiet Hours go through the tested
  // store (no row spelled by hand, no parse of its own), count as a write
  // (or a Quiet-Hours-only POST answers 400), and are applied once, after
  // the handle closes, when a row was stored.
  const std::string set = code_body(integ, kSettingsPost);
  const size_t store_at = set.find("constboolqh_changed=store_quiet_hours_from_settings(prefs,body);"
                                   "if(qh_changed)wrote_anything=true;");
  const size_t end_at = set.find("prefs.end();");
  const size_t apply_at = set.find("if(qh_changed)apply_quiet_hours_from_nvs();");
  if (set.empty() || store_at == std::string::npos ||
      count_of(set, "store_quiet_hours_from_settings(") != 1) {
    out.push_back("POST /api/settings does not store Quiet Hours through store_quiet_hours_from_settings()");
  }
  if (set.find("\"qh.") != std::string::npos || set.find("\\\"quiet_hours\\\"") != std::string::npos ||
      set.find("\\\"start_min\\\"") != std::string::npos || set.find("\\\"end_min\\\"") != std::string::npos) {
    out.push_back("POST /api/settings parses or stores Quiet Hours itself");
  }
  if (apply_at == std::string::npos || count_of(set, "apply_quiet_hours_from_nvs(") != 1 ||
      end_at == std::string::npos || !(store_at < end_at && end_at < apply_at)) {
    out.push_back("POST /api/settings does not apply a stored Quiet Hours change once, after closing NVS");
  }
  const std::string post = code_body(integ, kTunePost);
  if (post.empty() || count_of(post, "tune_post(") != 1 ||
      count_of(post, "=tune_post(body,reinit_module);") != 1) {
    out.push_back("the Tuning Lab POST does not hand its body to tune_post(body, reinit_module)");
  }
  for (const char* own : {"Preferences", "tune_write_value(", "reinit_module(\"", "apply_quiet_hours_from_nvs("}) {
    if (post.find(own) != std::string::npos) {
      out.push_back(std::string("the Tuning Lab POST does tune_post()'s work itself: ") + own);
    }
  }
  const std::string preset = code_body(integ, kTunePreset);
  if (preset.find("returnhandle_tune_post_coefficients(req);") == std::string::npos) {
    out.push_back("the bundle import is not the Tuning Lab POST");
  }
  const std::string reg = code_body(integ, kRegister);
  if (count_of(reg, "apply_quiet_hours_from_nvs();") != 1) {
    out.push_back("register_v1_modules() does not apply the stored Quiet Hours once at boot");
  }
  if (code_body(integ, kReinit) !=
      "constcsi_module_t*m=csi_module_find(module_id);if(!m)return;"
      "if(m->deinit)m->deinit();if(m->init)m->init(nullptr);") {
    out.push_back("reinit_module() is not the test's model of it");
  }
  if (std::regex_search(integ, std::regex(R"(\bTUNE_COEFFS\s*\[\s*\]\s*=)")) ||
      std::regex_search(integ, std::regex(R"(\bapply_quiet_hours_from_nvs\s*\(\s*(void)?\s*\)\s*\{)")) ||
      std::regex_search(integ, std::regex(R"(\bread_quiet_hours\s*\([^)]*\)\s*\{)")) ||
      std::regex_search(integ, std::regex(R"(\btune_post\s*\([^)]*\)\s*\{)"))) {
    out.push_back("csi_integration.cpp defines its own table, reader, apply or POST body");
  }
  // F151: core.presence's rows and the privacy ceiling, through the tested
  // stores and readers, by the key map.
  if (count_of(get, "constPresenceSettingspresence=read_presence_settings(prefs);") != 1 ||
      count_of(get, "read_privacy_ceiling(prefs)") != 1) {
    out.push_back("GET /api/settings reads the presence rows or the ceiling other than through the tested readers");
  }
  // What it reads goes out under the right names (F151's review: a swap
  // here reports one row as another with every behavior test green).
  if (count_of(get, "constPresenceSettingspresence=read_presence_settings(prefs);"
                    "constboolpet_mode=presence.pet_mode;"
                    "constint32_tpreset_idx=presence.preset;"
                    "constint32_tsensitivity=presence.sensitivity;") != 1 ||
      count_of(get, "constchar*preset_str=(preset_idx==0)?\"sensitive\":(preset_idx==2)?\"quiet\":\"balanced\";") != 1 ||
      count_of(get, "pet_mode?\"true\":\"false\",preset_str,(long)sensitivity,") != 1) {
    out.push_back("GET /api/settings reports a presence row under another's name");
  }
  const size_t presence_at = set.find("if(store_presence_from_settings(prefs,body))wrote_anything=true;");
  if (presence_at == std::string::npos || count_of(set, "store_presence_from_settings(") != 1 ||
      !(presence_at < end_at)) {
    out.push_back("POST /api/settings does not store the presence rows through store_presence_from_settings(), as a write");
  }
  const size_t ceiling_at = set.find("constboolceiling_changed=store_privacy_ceiling_from_settings(prefs,body);"
                                     "if(ceiling_changed)wrote_anything=true;");
  const size_t ceiling_apply_at = set.find("if(ceiling_changed)apply_privacy_ceiling_from_nvs();");
  if (ceiling_at == std::string::npos || count_of(set, "store_privacy_ceiling_from_settings(") != 1 ||
      ceiling_apply_at == std::string::npos || count_of(set, "apply_privacy_ceiling_from_nvs(") != 1 ||
      !(ceiling_at < end_at && end_at < ceiling_apply_at)) {
    out.push_back("POST /api/settings does not store the ceiling through its store and apply it once, after closing NVS");
  }
  for (const char* own : {"\\\"pet_mode\\\"", "\\\"preset\\\"", "\\\"sensitivity\\\"",
                          "\\\"privacy_ceiling\\\""}) {
    if (set.find(own) != std::string::npos) {
      out.push_back(std::string("POST /api/settings parses a presence or ceiling key itself: ") + own);
    }
  }
  const std::string apply = code_body(integ, kCalibApply);
  const size_t thresholds_at = apply.find("(void)store_presence_thresholds(prefs,proposed);prefs.end();");
  const size_t reinit_at = apply.find("reinit_module(\"core.presence\");");
  if (apply.empty() || thresholds_at == std::string::npos ||
      count_of(apply, "store_presence_thresholds(") != 1 || apply.find("put") != std::string::npos ||
      reinit_at == std::string::npos || !(thresholds_at < reinit_at)) {
    out.push_back("the calibration's apply does not store its thresholds through store_presence_thresholds(), then re-init");
  }
  // Each proposal on its own row (a swap stored the motion proposal as the
  // active threshold, F151's own error class, with every suite green).
  if (count_of(apply, "PresenceThresholdsproposed;"
                      "proposed.motion=(int32_t)g_calibration.proposed_motion;"
                      "proposed.active=(int32_t)g_calibration.proposed_active;"
                      "proposed.breathing=(int32_t)g_calibration.proposed_breathing;"
                      "(void)store_presence_thresholds(prefs,proposed);") != 1 ||
      count_of(apply, "proposed.") != 3) {
    out.push_back("the calibration's apply stores a proposal on another threshold's row");
  }
  const std::string status = code_body(integ, kCalibStatus);
  if (count_of(status, "current=read_presence_thresholds(prefs);") != 1 ||
      status.find("getInt") != std::string::npos) {
    out.push_back("the calibration's status reads the thresholds other than through read_presence_thresholds()");
  }
  // With NVS not open it answers the reader's own defaults, and each
  // threshold goes out under its own name.
  if (count_of(status, "PresenceThresholdscurrent=presence_threshold_defaults();"
                       "if(prefs_ok){current=read_presence_thresholds(prefs);prefs.end();}"
                       "constint32_tcur_motion=current.motion;"
                       "constint32_tcur_active=current.active;"
                       "constint32_tcur_breath=current.breathing;") != 1 ||
      count_of(status, "(long)cur_motion,(long)cur_active,(long)cur_breath);") != 1) {
    out.push_back("the calibration's status answers other defaults, or one threshold under another's name");
  }
  if (std::regex_search(integ, std::regex(R"(\b(store_presence_from_settings|read_presence_settings|store_presence_thresholds|read_presence_thresholds|presence_threshold_defaults|read_privacy_ceiling|store_privacy_ceiling_from_settings|apply_privacy_ceiling_from_nvs)\s*\([^)]*\)\s*\{)"))) {
    out.push_back("csi_integration.cpp defines its own presence or ceiling store, reader or apply");
  }
  for (const std::string& lit : hand_spelled_setting_keys(integ)) {
    out.push_back("csi_integration.cpp spells a module setting's NVS key by hand: \"" + lit + "\"");
  }
  return out;
}

static bool mutate(std::string& s, const std::string& from, const std::string& to) {
  const size_t at = s.find(from);
  if (at == std::string::npos) return false;
  s.replace(at, from.size(), to);
  return true;
}

static int test_csi_integration_is_thin_around_the_tested_code() {
  const std::string integ = read_source("csi_integration.cpp");
  CHECK(!integ.empty());
  const std::vector<std::string> problems = pin_problems(integ);
  for (const std::string& p : problems) std::fprintf(stderr, "pin: %s\n", p.c_str());
  CHECK(problems.empty());

  struct Mutation { const char* name; const char* from; const char* to; };
  const Mutation kMutations[] = {
    {"GET reads its own Quiet Hours defaults", "read_quiet_hours(prefs)",
     "QuietHours{prefs.getBool(\"qh.en\", false), prefs.getInt(\"qh.start\", 0), prefs.getInt(\"qh.end\", 480)}"},
    {"the POST drops the module reinit", "tune_post(body, reinit_module)", "tune_post(body, nullptr)"},
    {"the POST stores without tune_post()", "const TunePost post = tune_post(body, reinit_module);",
     "TunePost post = {}; { Preferences prefs; (void)prefs; }"},
    {"the POST re-applies by hand", "free(body);\n\n  if (!post.nvs_ok)",
     "free(body);\n  apply_quiet_hours_from_nvs();\n\n  if (!post.nvs_ok)"},
    {"the bundle import is its own handler", "return handle_tune_post_coefficients(req);",
     "return ESP_OK;"},
    {"the boot does not apply Quiet Hours", "  apply_quiet_hours_from_nvs();\n\n  csi_hal::set_watchdog",
     "\n  csi_hal::set_watchdog"},
    {"reinit_module() skips deinit", "if (m->deinit) m->deinit();", ""},
    {"a local apply comes back", "void reinit_module(const char* module_id) {",
     "void apply_quiet_hours_from_nvs(void) {}\nvoid reinit_module(const char* module_id) {"},
    // The settings POST (the reviewer's probes X8 and X10, and their kin).
    {"the settings POST writes a Quiet Hours row by hand",
     "const bool qh_changed = store_quiet_hours_from_settings(prefs, body);",
     "const bool qh_changed = store_quiet_hours_from_settings(prefs, body);\n"
     "  if (strstr(body, \"\\\"start_min\\\"\")) prefs.putInt(\"qh.st\", 0);"},
    {"the settings POST parses Quiet Hours itself",
     "const bool qh_changed = store_quiet_hours_from_settings(prefs, body);",
     "bool qh_changed = false;\n  if (strstr(body, \"\\\"quiet_hours\\\"\")) qh_changed = true;"},
    {"the settings POST does not apply Quiet Hours", "  if (qh_changed) apply_quiet_hours_from_nvs();\n", ""},
    {"the settings POST applies before closing NVS",
     "  const bool qh_changed = store_quiet_hours_from_settings(prefs, body);\n",
     "  const bool qh_changed = store_quiet_hours_from_settings(prefs, body);\n"
     "  if (qh_changed) apply_quiet_hours_from_nvs();\n"},
    {"a Quiet-Hours-only settings POST answers 400", "  if (qh_changed) wrote_anything = true;\n", ""},
    // F151: the item's own probe, in the form it takes now (the POST's
    // sensitivity stored by hand under a renamed key), and its kin.
    {"the settings POST stores the sensitivity by hand as cp.sen",
     "if (store_presence_from_settings(prefs, body)) wrote_anything = true;",
     "if (const char* k = strstr(body, \"\\\"sensitivity\\\"\")) {\n"
     "    prefs.putInt(\"cp.sen\", (int32_t)strtol(strchr(k, ':') + 1, nullptr, 10));\n"
     "    wrote_anything = true;\n  }"},
    {"the settings POST adds a presence row by hand",
     "if (store_presence_from_settings(prefs, body)) wrote_anything = true;",
     "if (store_presence_from_settings(prefs, body)) wrote_anything = true;\n  prefs.putInt(\"cp.sen\", 50);"},
    {"the settings POST drops the presence store",
     "if (store_presence_from_settings(prefs, body)) wrote_anything = true;", ""},
    {"a presence-only settings POST answers 400",
     "if (store_presence_from_settings(prefs, body)) wrote_anything = true;",
     "(void)store_presence_from_settings(prefs, body);"},
    {"GET reads the sensitivity by hand",
     "const PresenceSettings presence = read_presence_settings(prefs);",
     "PresenceSettings presence = read_presence_settings(prefs);\n  presence.sensitivity = prefs.getInt(\"cp.sen\", 50);"},
    {"GET reads the ceiling by hand", "const int32_t privacy_raw = read_privacy_ceiling(prefs);",
     "const int32_t privacy_raw = prefs.getInt(\"cp.pc\", 0);"},
    {"the calibration stores a threshold by hand", "(void)store_presence_thresholds(prefs, proposed);",
     "(void)proposed;\n  prefs.putInt(\"cp.mt\", (int32_t)g_calibration.proposed_motion);"},
    {"the calibration drops its store", "(void)store_presence_thresholds(prefs, proposed);", "(void)proposed;"},
    {"the calibration re-inits before it stores", "  reinit_module(\"core.presence\");\n\n  /* Mark the calibration",
     "\n  /* Mark the calibration"},
    {"the calibration's status reads by hand", "current = read_presence_thresholds(prefs);",
     "current.motion = prefs.getInt(\"cp.mt\", 35);"},
    {"the settings POST stores the ceiling by hand",
     "const bool ceiling_changed = store_privacy_ceiling_from_settings(prefs, body);",
     "const bool ceiling_changed = strstr(body, \"\\\"privacy_ceiling\\\"\") != nullptr && prefs.putInt(\"cp.pc\", 1) > 0;"},
    {"the settings POST does not apply the ceiling", "  if (ceiling_changed) apply_privacy_ceiling_from_nvs();\n", ""},
    // F151's review: the mappings to and from the tested structs.
    {"GET reports the sensitivity as the preset (g01)", "const int32_t preset_idx  = presence.preset;",
     "const int32_t preset_idx  = presence.sensitivity;"},
    {"GET reports the preset as the sensitivity", "const int32_t sensitivity = presence.sensitivity;",
     "const int32_t sensitivity = presence.preset;"},
    {"GET reports pet mode off", "const bool    pet_mode    = presence.pet_mode;",
     "const bool    pet_mode    = false;"},
    {"GET maps preset 0 to quiet", "(preset_idx == 0) ? \"sensitive\"", "(preset_idx == 0) ? \"quiet\""},
    {"GET swaps two values in the body", "pet_mode ? \"true\" : \"false\", preset_str, (long)sensitivity,",
     "pet_mode ? \"true\" : \"false\", preset_str, (long)preset_idx,"},
    {"the status reports active as motion (g02)", "const int32_t cur_motion = current.motion;",
     "const int32_t cur_motion = current.active;"},
    {"the status reports breathing as active", "const int32_t cur_active = current.active;",
     "const int32_t cur_active = current.breathing;"},
    {"the status swaps two values in the body", "(long)cur_motion, (long)cur_active, (long)cur_breath);",
     "(long)cur_active, (long)cur_motion, (long)cur_breath);"},
    {"the status's NVS-closed fallback is its own literal (g04)",
     "PresenceThresholds current = presence_threshold_defaults();", "PresenceThresholds current = {35, 75, 31};"},
    {"the status keeps the defaults with NVS open", "current = read_presence_thresholds(prefs);",
     "(void)read_presence_thresholds(prefs);"},
    {"the calibration stores the motion proposal as active (g03)",
     "proposed.active    = (int32_t)g_calibration.proposed_active;",
     "proposed.active    = (int32_t)g_calibration.proposed_motion;"},
    {"the calibration swaps motion and active",
     "  proposed.motion    = (int32_t)g_calibration.proposed_motion;\n"
     "  proposed.active    = (int32_t)g_calibration.proposed_active;\n",
     "  proposed.motion    = (int32_t)g_calibration.proposed_active;\n"
     "  proposed.active    = (int32_t)g_calibration.proposed_motion;\n"},
    {"the calibration overwrites a proposal after the three",
     "  (void)store_presence_thresholds(prefs, proposed);",
     "  proposed.breathing = (int32_t)g_calibration.proposed_motion;\n  (void)store_presence_thresholds(prefs, proposed);"},
    {"the calibration stores a positional proposal",
     "  PresenceThresholds proposed;\n"
     "  proposed.motion    = (int32_t)g_calibration.proposed_motion;\n"
     "  proposed.active    = (int32_t)g_calibration.proposed_active;\n"
     "  proposed.breathing = (int32_t)g_calibration.proposed_breathing;\n",
     "  const PresenceThresholds proposed = {(int32_t)g_calibration.proposed_active,\n"
     "    (int32_t)g_calibration.proposed_motion, (int32_t)g_calibration.proposed_breathing};\n"},
    {"a local threshold default comes back", "void reinit_module(const char* module_id) {",
     "PresenceThresholds presence_threshold_defaults(void) { return {35, 75, 30}; }\nvoid reinit_module(const char* module_id) {"},
    {"a local ceiling apply comes back", "void reinit_module(const char* module_id) {",
     "void apply_privacy_ceiling_from_nvs() {}\nvoid reinit_module(const char* module_id) {"},
  };
  for (const Mutation& mu : kMutations) {
    std::string src = integ;
    if (!mutate(src, mu.from, mu.to)) {
      std::fprintf(stderr, "mutation '%s' no longer applies: update it with the source\n", mu.name);
      CHECK(false);
    }
    if (pin_problems(src).empty()) {
      std::fprintf(stderr, "mutation '%s' was not caught\n", mu.name);
      CHECK(false);
    }
  }
  return 0;
}

// F151: no sketch source but the key map spells a module setting's NVS
// key. Every source that touches NVS (it names Preferences) is read; the
// map's own header is the one place the short keys live. Checked against
// in-memory mutations of the sources that write those rows.
static int test_no_sketch_source_spells_a_module_settings_key() {
  namespace fs = std::filesystem;
  std::vector<std::string> scanned;
  for (const fs::directory_entry& e : fs::directory_iterator(WAP_SKETCH_DIR)) {
    const std::string name = e.path().filename().string();
    const std::string ext = e.path().extension().string();
    if (ext != ".cpp" && ext != ".h" && ext != ".ino") continue;
    if (name == "csi_module_settings_nvs.h") continue;   // the map
    const std::string src = read_source(name.c_str());
    if (src.find("Preferences") == std::string::npos) continue;
    scanned.push_back(name);
    for (const std::string& lit : hand_spelled_setting_keys(src)) {
      std::fprintf(stderr, "%s spells a module setting's NVS key by hand: \"%s\"\n", name.c_str(), lit.c_str());
      CHECK(false);
    }
  }
  for (const char* must : {"csi_integration.cpp", "csi_settings_nvs.cpp", "csi_tune_lab.cpp",
                           "csi_mqtt.cpp", "csi_event_egress.cpp", "canary_wap.ino"}) {
    CHECK(std::find(scanned.begin(), scanned.end(), must) != scanned.end());
  }

  struct Mutation { const char* name; const char* file; const char* from; const char* to; };
  const Mutation kMutations[] = {
    {"the dashboard's store spells the sensitivity", "csi_settings_nvs.cpp",
     "prefs.putInt(nvs_key_for(\"core.presence.sensitivity\"), (int32_t)n);",
     "prefs.putInt(\"cp.sen\", (int32_t)n);"},
    {"the calibration's store spells a threshold", "csi_settings_nvs.cpp",
     "prefs.putInt(nvs_key_for(\"core.presence.motion_threshold\"), thresholds.motion) > 0;",
     "prefs.putInt(\"cp.mt\", thresholds.motion) > 0;"},
    {"the Quiet Hours reader spells a row", "csi_settings_nvs.cpp",
     "prefs.getBool(nvs_key_for(\"core.quiet_hours.enabled\"), kQuietHoursDefaultEnabled);",
     "prefs.getBool(\"qh.en\", kQuietHoursDefaultEnabled);"},
    {"the sketch reads a presence row", "canary_wap.ino", "static const char* csi_zone_id() {",
     "static int32_t csi_sens_peek(Preferences& p) { return p.getInt(\"cp.sens\", 50); }\n"
     "static const char* csi_zone_id() {"},
    {"the MQTT bridge reads an anomaly row", "csi_mqtt.cpp", "bool config_load(Config* out) {",
     "static int32_t cd_peek(Preferences& p) { return p.getInt(\"ab.cd\", 600); }\n"
     "bool config_load(Config* out) {"},
  };
  for (const Mutation& mu : kMutations) {
    std::string src = read_source(mu.file);
    if (!mutate(src, mu.from, mu.to)) {
      std::fprintf(stderr, "mutation '%s' no longer applies: update it with the source\n", mu.name);
      CHECK(false);
    }
    if (hand_spelled_setting_keys(src).empty()) {
      std::fprintf(stderr, "mutation '%s' was not caught\n", mu.name);
      CHECK(false);
    }
  }
  // Comments and other namespaces' keys are not module settings.
  CHECK(hand_spelled_setting_keys("// \"cp.sens\"\n/* \"qh.en\" */ x = \"csi.ff\"; y = \"tz.iana\";").empty());
  CHECK((hand_spelled_setting_keys("p.putInt(\"cp.sen\", 1); q = R\"(cp.raw)\";") ==
         std::vector<std::string>{"cp.sen", "cp.raw"}));
  return 0;
}

int main(int argc, char** argv) {
  // One test by name (argv[1]), as the revert proofs run them; all of them
  // otherwise.
  struct Test { const char* name; int (*fn)(); };
  const Test kTests[] = {
    {"the_lab_declares_the_device_quiet_hours_default", test_the_lab_declares_the_device_quiet_hours_default},
    {"resetting_the_lab_window_keeps_the_device_window", test_resetting_the_lab_window_keeps_the_device_window},
    {"the_dashboard_starts_from_the_same_window", test_the_dashboard_starts_from_the_same_window},
    {"a_lab_quiet_hours_change_applies_at_once", test_a_lab_quiet_hours_change_applies_at_once},
    {"a_bundle_import_applies_every_knob", test_a_bundle_import_applies_every_knob},
    {"the_dashboard_writes_the_rows_the_device_reads", test_the_dashboard_writes_the_rows_the_device_reads},
    {"the_quiet_hours_rows_keep_their_stored_names", test_the_quiet_hours_rows_keep_their_stored_names},
    {"every_knob_applies_at_once_to_its_group", test_every_knob_applies_at_once_to_its_group},
    {"the_post_stores_only_known_knobs", test_the_post_stores_only_known_knobs},
    {"the_dashboard_writes_the_presence_rows_the_module_reads",
     test_the_dashboard_writes_the_presence_rows_the_module_reads},
    {"the_calibration_writes_the_thresholds_the_module_reads",
     test_the_calibration_writes_the_thresholds_the_module_reads},
    {"the_privacy_ceiling_is_one_row", test_the_privacy_ceiling_is_one_row},
    {"no_sketch_source_spells_a_module_settings_key", test_no_sketch_source_spells_a_module_settings_key},
    {"csi_integration_is_thin_around_the_tested_code", test_csi_integration_is_thin_around_the_tested_code},
  };
  int ran = 0;
  for (const Test& t : kTests) {
    if (argc > 1 && std::strcmp(argv[1], t.name) != 0) continue;
    ++ran;
    if (t.fn()) return 1;
  }
  if (ran == 0) {
    std::fprintf(stderr, "no test named %s\n", argv[1]);
    return 2;
  }
  std::printf("ALL wap_tune_lab TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
