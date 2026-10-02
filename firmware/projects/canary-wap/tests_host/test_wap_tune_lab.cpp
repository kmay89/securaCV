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
