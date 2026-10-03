// Host tests for meta.daily_summary on the canary-wap (sweep F121): the
// sketch's REAL clock feed, canary_wap.ino's update_csi_clock_offset() cut
// verbatim out of the sketch (firmware/tests_host/cut_functions.awk) with
// the GPS_CLOCK_FLOOR it guards on, over the staged CSI library and modules,
// booted in csi_integration::init()'s order with the sketch's real settings
// readers (csi_settings_nvs.cpp: the Quiet Hours apply and the modules' boot
// init) over the fake NVS of stubs/module_boot.
//
// Before F121 nothing called meta_daily_summary_set_clock(), so the module's
// clock stayed at its 0xffff sentinel, its tick() returned before the 23:55
// check, and no daily_summary row was ever committed. Now the loop pass that
// keeps the chokepoint's clock offset hands the module the same household
// minute of day, and only with a synced wall clock.
//
// A "loop pass" is what the sketch's loop() does for this: the clock sync
// calls update_csi_clock_offset() on every pass with a set clock
// (sync_clock_from_gps), on_csi_window() ticks the modules for a window of
// two frames or more, and csi_integration::loop() ticks the bundler. The test
// calls the cut updater on every pass, synced or not; the updater's own guard
// turns an unsynced clock away (the sketch's caller never offers one).
//
// Against the code before F121 (no feed) every test fails but
// test_an_unsynced_clock_commits_none, which guards the new feed: it fails
// with the updater's floor guard taken out. With the module's first-clock
// rule taken out, the reboot and late-sync tests fail (a second row).
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "Preferences.h"
#include "csi_bundler.h"
#include "csi_event.h"
#include "csi_event_id_floor.h"
#include "csi_event_wire.h"
#include "csi_module.h"
#include "csi_settings_nvs.h"
#include "tz_rule.h"

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
static uint32_t g_ms = 9000000;
extern "C" int clock_gettime(clockid_t, struct timespec* ts) noexcept {
  ts->tv_sec = (time_t)(g_ms / 1000);
  ts->tv_nsec = (long)((g_ms % 1000) * 1000000);
  return 0;
}
static uint32_t millis() { return g_ms; }

// ── canary_wap.ino's clock feed, cut verbatim (see the Makefile) ────────
#include "test_wap_daily_summary_clock.inc"

// ── The chokepoint's hooks: the summaries, and the Quiet Hours digest ───
struct Summary {
  uint32_t             id;
  uint16_t             minute;   // household minute of day it committed at
  csi_event_category_t category;
  csi_privacy_class_t  privacy;
  csi_event_values_t   values;
};
static Summary g_summaries[16];
static int g_summary_count = 0;
static int g_held_summaries = 0;
static uint16_t g_held_count = 0;
static uint16_t g_wall_minute = 0xffff;
extern "C" void csi_event_on_committed(uint32_t event_id, const char* module_id,
                                       const char* type_name, csi_event_category_t category,
                                       csi_privacy_class_t privacy,
                                       const csi_event_values_t* values) {
  if (std::strcmp(module_id, "meta.quiet_hours") == 0 &&
      std::strcmp(type_name, "held_summary") == 0) {
    ++g_held_summaries;
    g_held_count = values->bundled_count;
    return;
  }
  if (std::strcmp(module_id, "meta.daily_summary") != 0) return;
  if (std::strcmp(type_name, "daily_summary") != 0) return;
  if (g_summary_count < 16) {
    Summary& s = g_summaries[g_summary_count];
    s.id = event_id;
    s.minute = g_wall_minute;
    s.category = category;
    s.privacy = privacy;
    s.values = *values;
  }
  ++g_summary_count;
}
extern "C" void csi_event_on_id_advance(uint32_t) {}

// ── A boot, in csi_integration::init()'s order ──────────────────────────
constexpr uint32_t kFloor = csi_event_id_floor::kIdSpaceBase + 5000;
constexpr time_t kDay = 86400;
constexpr time_t kOct3 = 1790985600;   // 2026-10-03 00:00:00 UTC (TZ is UTC here)

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
  apply_quiet_hours_from_nvs();               // register_v1_modules()'s last step
}

// A reboot: RAM gone (chokepoint, bundler, registry, the module's clock and
// latch), NVS kept. Then init()'s order: the floor, the modules and their
// Quiet Hours, the modules' boot init (F93).
static void reboot_and_boot() {
  csi_event_test_reset();
  csi_module_test_reset();
  csi_event_set_event_id_floor(kFloor);       // apply_event_id_floor_from_nvs()
  register_v1_modules_model();                // register_v1_modules()
  (void)csi_settings_nvs_init_modules();      // F93
}

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

// One loop pass at wall time `wall`, `step_s` seconds after the last. With
// `csi` false no window ticks the modules (on_csi_window's honesty gate
// skips windows of fewer than two frames): the clock is still fed.
static void loop_pass(time_t wall, uint32_t step_s, int8_t doppler = 0, bool csi = true) {
  g_ms += step_s * 1000u;
  g_wall_minute = (uint16_t)tz_rule::local_minute_of_day(wall);
  update_csi_clock_offset(wall);              // sync_clock_from_gps(), every pass
  if (csi) {
    const csi_features_t f = window_of(doppler);
    csi_module_tick_all(&f);                  // on_csi_window()
  }
  csi_bundler_tick();                         // csi_integration::loop()
}

static void run(time_t from, time_t to, uint32_t step_s = 30, int8_t doppler = 0,
                bool csi = true) {
  for (time_t t = from; t < to; t += step_s) loop_pass(t, step_s, doppler, csi);
}

static void reset_counts() {
  g_summary_count = 0;
  g_held_summaries = 0;
  g_held_count = 0;
  std::memset(g_summaries, 0, sizeof(g_summaries));
}

static int test_a_day_crossing_2355_commits_one_summary() {
  host_prefs().clear();
  reset_counts();
  reboot_and_boot();
  run(kOct3 + 12 * 3600, kOct3 + kDay + 30 * 60);       // 12:00 to 00:30
  CHECK(g_summary_count == 1);
  CHECK(g_summaries[0].minute == 1435);                 // the first pass at 23:55

  run(kOct3 + kDay + 30 * 60, kOct3 + 2 * kDay + 30 * 60);
  CHECK(g_summary_count == 2);
  CHECK(g_summaries[1].minute == 1435);
  CHECK(g_summaries[1].id > g_summaries[0].id);
  return 0;
}

static int test_a_reboot_after_the_summary_commits_no_second() {
  host_prefs().clear();
  reset_counts();
  reboot_and_boot();
  run(kOct3 + 23 * 3600, kOct3 + 23 * 3600 + 56 * 60);  // 23:00 to 23:56
  CHECK(g_summary_count == 1);

  reboot_and_boot();                                    // a reboot at 23:57
  run(kOct3 + 23 * 3600 + 57 * 60, kOct3 + kDay + 10 * 60);
  CHECK(g_summary_count == 1);

  run(kOct3 + kDay + 10 * 60, kOct3 + 2 * kDay + 10 * 60);
  CHECK(g_summary_count == 2);

  reset_counts();
  reboot_and_boot();                                    // a reboot at 23:50
  run(kOct3 + 23 * 3600 + 50 * 60, kOct3 + kDay + 5 * 60);
  CHECK(g_summary_count == 1);
  CHECK(g_summaries[0].minute == 1435);
  return 0;
}

// The canary-wap has no SNTP: until GPS sets it, its clock reads an epoch
// near zero. Fifty hours of passes commit no summary; GPS at 22:00 and the
// day's 23:55 does.
static int test_an_unsynced_clock_commits_none() {
  host_prefs().clear();
  reset_counts();
  reboot_and_boot();
  run(0, 50 * 3600, 60);
  CHECK(g_summary_count == 0);

  run(kOct3 + 22 * 3600, kOct3 + kDay + 60, 60);
  CHECK(g_summary_count == 1);
  CHECK(g_summaries[0].minute == 1435);
  return 0;
}

static int test_a_first_clock_inside_the_window_waits_for_the_next_day() {
  host_prefs().clear();
  reset_counts();
  reboot_and_boot();
  run(0, 600, 30);
  run(kOct3 + 23 * 3600 + 57 * 60, kOct3 + kDay + 30 * 60);   // GPS at 23:57
  CHECK(g_summary_count == 0);
  run(kOct3 + kDay + 30 * 60, kOct3 + 2 * kDay);
  CHECK(g_summary_count == 1);

  run(kOct3 + 2 * kDay - 6 * 60, kOct3 + 2 * kDay);     // stepped back to 23:54
  CHECK(g_summary_count == 1);
  return 0;
}

static int test_a_night_without_windows_still_owes_the_next_row() {
  host_prefs().clear();
  reset_counts();
  reboot_and_boot();
  run(kOct3 + 23 * 3600, kOct3 + 23 * 3600 + 59 * 60);
  CHECK(g_summary_count == 1);
  run(kOct3 + 23 * 3600 + 59 * 60, kOct3 + kDay + 3600, 30, 0, false);   // starved night
  run(kOct3 + kDay + 3600, kOct3 + 2 * kDay);
  CHECK(g_summary_count == 2);

  run(kOct3 + 2 * kDay, kOct3 + 2 * kDay + 23 * 3600 + 50 * 60);
  run(kOct3 + 2 * kDay + 23 * 3600 + 50 * 60, kOct3 + 3 * kDay + 3600, 30, 0, false);
  CHECK(g_summary_count == 2);                          // no window, no row that day
  return 0;
}

// Quiet Hours on over 23:55 (the device default window, 23:00 to 07:00, saved
// on): the chokepoint holds the summary like any non-anomaly row, so no
// daily_summary row commits, and the morning's held_summary counts it.
// Pinned as it stands; whether the summary should pass the gate is a NEW
// decision handed up with F121.
static int test_quiet_hours_over_2355_hold_the_summary() {
  host_prefs().clear();
  host_prefs().flag["csi/qh.en"] = true;
  reset_counts();
  reboot_and_boot();
  run(kOct3 + 22 * 3600, kOct3 + kDay + 7 * 3600 + 60);  // 22:00 to 07:01
  CHECK(g_summary_count == 0);
  CHECK(g_held_summaries == 0);
  run(kOct3 + kDay + 7 * 3600 + 60, kOct3 + kDay + 7 * 3600 + 300, 30, 60);  // movement
  CHECK(g_held_summaries == 1);
  CHECK(g_held_count >= 1);
  CHECK(g_summary_count == 0);
  return 0;
}

// What the row carries (docs/csi_modules.md's row): a P0 event row whose
// note is "a<active> q<empty> x<anomaly>" over the committed ring rows it
// walked, bundled = how many, the 23:50 bucket. A day of presence changes
// reads "a0 q0 x0" (closed bundles never reach the ring, sweep F77). The
// MQTT events body carries type and bundled, no note, and the empty state's
// event_type "unknown".
static int test_what_the_summary_row_carries() {
  host_prefs().clear();
  reset_counts();
  reboot_and_boot();
  run(kOct3 + 20 * 3600, kOct3 + 21 * 3600, 30, 60);
  run(kOct3 + 21 * 3600, kOct3 + 23 * 3600, 30, 0);
  run(kOct3 + 23 * 3600, kOct3 + 23 * 3600 + 56 * 60);
  CHECK(g_summary_count == 1);
  const Summary& s = g_summaries[0];
  CHECK(s.category == CSI_CATEGORY_EVENT);
  CHECK(s.privacy == CSI_PRIVACY_P0);
  CHECK(s.values.state_name[0] == '\0');
  CHECK(std::strcmp(s.values.note, "a0 q0 x0") == 0);
  CHECK(s.values.time_bucket == 143);

  csi_event_record_t ring[64];
  const size_t n = csi_event_recent(ring, 64);
  CHECK(n >= 1);
  CHECK(s.values.bundled_count == (uint16_t)(n - 1));

  char body[768];
  const csi_event_wire::Signer unsigned_signer = {nullptr, 1, nullptr, nullptr};
  const size_t len = csi_event_wire::build_event_body(
      body, sizeof(body), s.id, "meta.daily_summary", "daily_summary",
      s.category, s.privacy, &s.values, 0, 1, false, unsigned_signer);
  CHECK(len > 0);
  CHECK(std::strstr(body, "\"type\":\"daily_summary\"") != nullptr);
  CHECK(std::strstr(body, "\"event_type\":\"unknown\"") != nullptr);
  CHECK(std::strstr(body, "a0 q0 x0") == nullptr);
  return 0;
}

int main() {
  setenv("TZ", "UTC0", 1);
  tzset();
  int rc = 0;
  rc |= test_a_day_crossing_2355_commits_one_summary();
  rc |= test_a_reboot_after_the_summary_commits_no_second();
  rc |= test_an_unsynced_clock_commits_none();
  rc |= test_a_first_clock_inside_the_window_waits_for_the_next_day();
  rc |= test_a_night_without_windows_still_owes_the_next_row();
  rc |= test_quiet_hours_over_2355_hold_the_summary();
  rc |= test_what_the_summary_row_carries();
  if (rc != 0) {
    std::fprintf(stderr, "test_wap_daily_summary: FAILED\n");
    return 1;
  }
  std::printf("test_wap_daily_summary: ALL %d CHECKS PASSED\n", g_checks);
  return 0;
}
