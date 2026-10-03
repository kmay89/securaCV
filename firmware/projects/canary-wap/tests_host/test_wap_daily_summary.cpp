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
// The latch is keyed on the household's local date, which the updater
// passes beside the minute: one row per date however the clock reaches it.
// The DST, zone and clock-step tests change TZ the way the sketch's
// set_timezone path does (setenv + tzset) and fail on a latch that let go on
// the minute alone (a minute before 00:30): a spring-forward at midnight, a
// zone moved east after the row and a step from 23:58 to 00:45 lost the next
// date's row, and a zone moved west after midnight committed a second row
// for the date it returned to. test_the_household_minute_is_local runs a day
// in New York, where a feed of the UTC minute commits at 19:55 local. The
// source pin holds the per-pass call these tests stand in for: the updater
// inside sync_clock_from_gps()'s `if (clock_set)`, ahead of its resync
// return, and loop() calling sync_clock_from_gps(). The Quiet Hours test
// holds that the summary is among the rows the window held: the same night
// without meta.daily_summary registered holds one row fewer.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
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
  time_t               wall;     // the wall clock it committed at
  char                 date[11]; // the household date it committed on
  csi_event_category_t category;
  csi_privacy_class_t  privacy;
  csi_event_values_t   values;
};
static Summary g_summaries[16];
static int g_summary_count = 0;
static int g_held_summaries = 0;
static uint16_t g_held_count = 0;
static uint16_t g_wall_minute = 0xffff;
static time_t g_wall = 0;
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
    s.wall = g_wall;
    struct tm lt = {};
    (void)localtime_r(&g_wall, &lt);
    std::strftime(s.date, sizeof(s.date), "%Y-%m-%d", &lt);
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

// The Quiet Hours test's control boots without meta.daily_summary.
static bool g_register_daily_summary = true;

static void register_v1_modules_model() {
  csi_module_register(core_presence_module());
  csi_module_register(core_breathing_module());
  csi_module_register(core_activity_ribbon_module());
  if (g_register_daily_summary) csi_module_register(meta_daily_summary_module());
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
  g_wall = wall;
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
// daily_summary row commits, and the morning's held_summary counts it: the
// same night booted without meta.daily_summary holds exactly one row fewer.
// Pinned as it stands; whether the summary should pass the gate is a NEW
// decision handed up with F121.
static int quiet_night(bool with_summary_module, uint16_t* held) {
  host_prefs().clear();
  host_prefs().flag["csi/qh.en"] = true;
  reset_counts();
  g_register_daily_summary = with_summary_module;
  reboot_and_boot();
  g_register_daily_summary = true;
  run(kOct3 + 22 * 3600, kOct3 + kDay + 7 * 3600 + 60);  // 22:00 to 07:01
  CHECK(g_summary_count == 0);
  CHECK(g_held_summaries == 0);
  run(kOct3 + kDay + 7 * 3600 + 60, kOct3 + kDay + 7 * 3600 + 300, 30, 60);  // movement
  CHECK(g_held_summaries == 1);
  CHECK(g_summary_count == 0);
  *held = g_held_count;
  return 0;
}

static int test_quiet_hours_over_2355_hold_the_summary() {
  uint16_t held_with = 0;
  uint16_t held_without = 0;
  CHECK(quiet_night(true, &held_with) == 0);
  CHECK(quiet_night(false, &held_without) == 0);
  CHECK(held_without >= 1);
  CHECK(held_with == held_without + 1);                 // the summary, held
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

// The household's zone, as the sketch's set_timezone path applies it.
static void set_zone(const char* posix) {
  setenv("TZ", posix, 1);
  tzset();
}

static bool dates_differ(int count) {
  for (int i = 0; i < count && i < 16; ++i)
    for (int j = i + 1; j < count && j < 16; ++j)
      if (std::strcmp(g_summaries[i].date, g_summaries[j].date) == 0) return false;
  return true;
}

// DST at midnight. Havana springs forward at 00:00 (23:59:59 CST is followed
// by 01:00 CDT, so no minute before 00:30 happens that night), and the date
// after it still owes its row: one per date, Mar 6 to Mar 9. Santiago falls
// back at 24:00 (23:59:59 -03 is followed by 23:00 -04), so 23:55 happens
// twice on Apr 4 and one row commits for it. Before the latch was keyed on
// the date, Mar 8 had no row.
static int test_dst_at_midnight_keeps_one_row_per_date() {
  constexpr time_t kMar6 = 1772755200;   // 2026-03-06 00:00:00 UTC
  constexpr time_t kApr3 = 1775174400;   // 2026-04-03 00:00:00 UTC
  host_prefs().clear();
  reset_counts();
  set_zone("CST5CDT,M3.2.0/0,M11.1.0/1");
  reboot_and_boot();
  run(kMar6 + 17 * 3600, kMar6 + 4 * kDay + 4 * 3600 + 30 * 60);   // Mar 6 12:00 to Mar 10 00:30
  CHECK(g_summary_count == 4);
  CHECK(std::strcmp(g_summaries[0].date, "2026-03-06") == 0);
  CHECK(std::strcmp(g_summaries[1].date, "2026-03-07") == 0);
  CHECK(std::strcmp(g_summaries[2].date, "2026-03-08") == 0);
  CHECK(std::strcmp(g_summaries[3].date, "2026-03-09") == 0);
  for (int i = 0; i < 4; ++i) CHECK(g_summaries[i].minute == 1435);

  reset_counts();
  set_zone("<-04>4<-03>,M9.1.6/24,M4.1.6/24");
  reboot_and_boot();
  run(kApr3 + kDay + 15 * 3600, kApr3 + 2 * kDay + 4 * 3600 + 30 * 60);   // Apr 4 12:00 to Apr 5 00:30
  CHECK(g_summary_count == 1);
  CHECK(std::strcmp(g_summaries[0].date, "2026-04-04") == 0);
  CHECK(g_summaries[0].wall == kApr3 + 2 * kDay + 2 * 3600 + 55 * 60);   // the first 23:55 (-03)
  run(kApr3 + 2 * kDay + 4 * 3600 + 30 * 60, kApr3 + 3 * kDay + 4 * 3600 + 30 * 60);
  CHECK(g_summary_count == 2);
  CHECK(std::strcmp(g_summaries[1].date, "2026-04-05") == 0);
  set_zone("UTC0");
  return 0;
}

// The zone moved east by an hour at 23:57, after the row: the clock reads
// 00:57 on the next date, and that date still owes its row. A clock stepped
// from 23:58 to 00:45 (a GPS correction) is the same: the next date's row
// commits. Before the latch was keyed on the date, neither did.
static int test_a_new_date_reached_past_0030_owes_its_row() {
  host_prefs().clear();
  reset_counts();
  set_zone("UTC0");
  reboot_and_boot();
  run(kOct3 + 23 * 3600, kOct3 + 23 * 3600 + 57 * 60);
  CHECK(g_summary_count == 1);
  set_zone("<+01>-1");                                  // 23:57 UTC is 00:57 Oct 4
  run(kOct3 + 23 * 3600 + 57 * 60, kOct3 + kDay + 23 * 3600 + 30 * 60);   // to 00:30 Oct 5
  CHECK(g_summary_count == 2);
  CHECK(std::strcmp(g_summaries[1].date, "2026-10-04") == 0);
  CHECK(g_summaries[1].minute == 1435);
  CHECK(g_summaries[1].wall == kOct3 + kDay + 22 * 3600 + 55 * 60);

  reset_counts();
  set_zone("UTC0");
  reboot_and_boot();
  run(kOct3 + 23 * 3600, kOct3 + 23 * 3600 + 58 * 60);
  CHECK(g_summary_count == 1);
  run(kOct3 + kDay + 45 * 60, kOct3 + 2 * kDay + 10 * 60);   // stepped to 00:45
  CHECK(g_summary_count == 2);
  CHECK(std::strcmp(g_summaries[1].date, "2026-10-04") == 0);
  return 0;
}

// The zone moved west by an hour at 00:10, after Oct 3's row: the clock reads
// 23:10 on Oct 3 again and passes 23:55 a second time, and Oct 3 commits no
// second row; Oct 4 commits its own. Before the latch was keyed on the date,
// the minute 00:10 released it and Oct 3 had two.
static int test_a_zone_moved_west_after_midnight_commits_no_second() {
  host_prefs().clear();
  reset_counts();
  set_zone("UTC0");
  reboot_and_boot();
  run(kOct3 + 23 * 3600, kOct3 + kDay + 10 * 60);       // through 00:10 Oct 4
  CHECK(g_summary_count == 1);
  set_zone("<-01>1");                                   // 00:10 UTC is 23:10 Oct 3
  run(kOct3 + kDay + 10 * 60, kOct3 + 2 * kDay + 3600 + 30 * 60);   // to 00:30 Oct 5
  CHECK(g_summary_count == 2);
  CHECK(std::strcmp(g_summaries[0].date, "2026-10-03") == 0);
  CHECK(std::strcmp(g_summaries[1].date, "2026-10-04") == 0);
  CHECK(dates_differ(g_summary_count));
  set_zone("UTC0");
  return 0;
}

// The minute fed is the household's, not UTC's: a day in New York (EDT,
// UTC-4) commits its row at 23:55 local, which is 03:55 UTC on the next UTC
// date, and dates it to the local day. A feed of the UTC minute commits at
// 23:55 UTC, 19:55 local.
static int test_the_household_minute_is_local() {
  host_prefs().clear();
  reset_counts();
  set_zone("EST5EDT,M3.2.0,M11.1.0");
  reboot_and_boot();
  run(kOct3 + 16 * 3600, kOct3 + kDay + 4 * 3600 + 30 * 60);   // 12:00 to 00:30 EDT
  CHECK(g_summary_count == 1);
  CHECK(g_summaries[0].minute == 1435);
  CHECK(g_summaries[0].wall == kOct3 + kDay + 3 * 3600 + 55 * 60);   // 03:55 UTC Oct 4
  CHECK(std::strcmp(g_summaries[0].date, "2026-10-03") == 0);
  set_zone("UTC0");
  return 0;
}

// ── Source pin: the loop pass these tests stand in for ──────────────────
static std::string read_file(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
static std::string body_of(const std::string& src, const std::string& sig) {
  const size_t at = src.find("\n" + sig);
  if (at == std::string::npos) return "";
  const size_t end = src.find("\n}\n", at);
  if (end == std::string::npos) return "";
  return src.substr(at, end - at);
}

// The tests call the cut updater on every pass. In the sketch that is
// sync_clock_from_gps(), which loop() calls on every pass: it feeds the
// updater inside `if (clock_set)`, before the early return that skips the
// rest between GPS resync checks. The call after a GPS step alone would feed
// the module only when GPS moves the clock, almost never inside 23:55..23:59.
static int test_the_loop_feeds_the_clock_on_every_pass() {
  const std::string ino = read_file(std::string(WAP_SKETCH_DIR) + "/canary_wap.ino");
  CHECK(!ino.empty());
  const std::string sync = body_of(ino, "static void sync_clock_from_gps() {");
  CHECK(!sync.empty());
  const size_t guard = sync.find("\n  if (clock_set) {\n");
  CHECK(guard != std::string::npos);
  const size_t close = sync.find("\n  }\n", guard);
  const size_t feed = sync.find("\n    update_csi_clock_offset(sys_now);\n", guard);
  const size_t resync = sync.find("GPS_CLOCK_RESYNC_INTERVAL_MS");
  CHECK(close != std::string::npos && feed != std::string::npos && resync != std::string::npos);
  CHECK(guard < feed && feed < close && close < resync);
  CHECK(sync.find("return;", resync) != std::string::npos);
  const std::string loop = body_of(ino, "void loop() {");
  CHECK(loop.find("\n  sync_clock_from_gps();\n") != std::string::npos);
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
  rc |= test_dst_at_midnight_keeps_one_row_per_date();
  rc |= test_a_new_date_reached_past_0030_owes_its_row();
  rc |= test_a_zone_moved_west_after_midnight_commits_no_second();
  rc |= test_the_household_minute_is_local();
  rc |= test_the_loop_feeds_the_clock_on_every_pass();
  if (rc != 0) {
    std::fprintf(stderr, "test_wap_daily_summary: FAILED\n");
    return 1;
  }
  std::printf("test_wap_daily_summary: ALL %d CHECKS PASSED\n", g_checks);
  return 0;
}
