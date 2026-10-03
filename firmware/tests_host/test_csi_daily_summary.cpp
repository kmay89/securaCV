// Host tests for meta.daily_summary on the canary (sweep F121): the canary's
// REAL clock feed, main.cpp's updateCsiClockOffset() cut verbatim out of
// main.cpp (cut_functions.awk) with the WALL_CLOCK_FLOOR it guards on, over
// the REAL module bridge (canary/src/csi_modules_integration.cpp), the common
// CSI library and the modules it registers.
//
// Before F121 nothing called meta_daily_summary_set_clock(), so the module's
// clock stayed at its 0xffff sentinel, its tick() returned before the 23:55
// check, and no daily_summary row was ever committed. Now the loop pass that
// keeps the chokepoint's clock offset hands the module the same household
// minute of day, through securacv_csi_modules_set_clock(), and only with a
// synced wall clock.
//
// A "loop pass" here is what main.cpp's loop() does for this: the clock sync
// calls updateCsiClockOffset(time(nullptr)) on every pass with a set clock
// (syncClockFromGps), a CSI window reaches the bridge's feed, and the bundler
// ticks. The test calls the cut updater on every pass, synced or not; the
// updater's own guard is what turns an unsynced clock away (the device's
// caller never offers one, as main.cpp reads).
//
// Against the code before F121 (no feed) every test fails but
// test_an_unsynced_clock_commits_none, which guards the new feed: it fails
// with the updater's floor guard taken out. With the module's first-clock
// rule taken out, the reboot and late-sync tests fail (a second row).
//
// The latch is keyed on the household's local date, which the updater
// passes beside the minute: one row per date however the clock reaches it.
// The DST, zone and clock-step tests change TZ the way the device's
// set_timezone path does (setenv + tzset) and fail on a latch that let go on
// the minute alone (a minute before 00:30): a spring-forward at midnight, a
// zone moved east after the row and a step from 23:58 to 00:45 lost the next
// date's row, and a zone moved west after midnight committed a second row
// for the date it returned to. test_the_household_minute_is_local runs a day
// in New York, where a feed of the UTC minute commits at 19:55 local. The
// source pin holds the per-pass call these tests stand in for: the updater
// inside syncClockFromGps()'s `if (clock_set)`, ahead of its resync return,
// and loop() calling syncClockFromGps().
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

#include "Arduino.h"
#include "Preferences.h"
#include "csi_bundler.h"
#include "csi_event.h"
#include "csi_event_id_floor.h"
#include "csi_event_wire.h"
#include "csi_hal.h"
#include "csi_module.h"
#include "csi_modules_integration.h"
#include "time/tz_rule.h"

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

// ── main.cpp's clock feed, cut verbatim (see the Makefile) ──────────────
#include "canary_clock_feed.inc"

// ── The HAL the bridge talks to (its watchdog hook only) ────────────────
namespace csi_hal {
bool start() { return true; }
void stop() {}
void set_watchdog(uint32_t, WatchdogCallback) {}
}  // namespace csi_hal

// ── The chokepoint's hooks: every committed row, and the summaries ──────
struct Summary {
  uint32_t           id;
  uint16_t           minute;     // household minute of day it committed at
  time_t             wall;       // the wall clock it committed at
  char               date[11];   // the household date it committed on
  csi_event_category_t category;
  csi_privacy_class_t  privacy;
  csi_event_values_t values;
};
static Summary g_summaries[16];
static int g_summary_count = 0;
static uint16_t g_wall_minute = 0xffff;   // the minute the current pass runs at
static time_t g_wall = 0;                  // the wall clock the current pass runs at
extern "C" void csi_event_on_committed(uint32_t event_id, const char* module_id,
                                       const char* type_name, csi_event_category_t category,
                                       csi_privacy_class_t privacy,
                                       const csi_event_values_t* values) {
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

// ── A boot ──────────────────────────────────────────────────────────────
constexpr uint32_t kFloor = csi_event_id_floor::kIdSpaceBase + 1000;
constexpr time_t kDay = 86400;
constexpr time_t kOct3 = 1790985600;   // 2026-10-03 00:00:00 UTC (TZ is UTC here)

// A reboot and the canary's CSI boot, as test_csi_module_boot.cpp models it:
// RAM gone (chokepoint, bundler, registry, every boot init, the module's
// clock and latch), NVS kept; the floor, then the modules.
static void reboot_and_boot() {
  csi_event_test_reset();
  csi_module_test_reset();
  securacv_csi_modules_deinit();
  csi_event_set_event_id_floor(kFloor);       // csi_event_egress_begin()
  (void)securacv_csi_modules_init();
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
// `csi` false no window reaches the feed (CSI shed by the power or degrade
// gates, or no traffic): the clock is still fed, no module ticks.
static void loop_pass(time_t wall, uint32_t step_s, int8_t doppler = 0, bool csi = true) {
  g_host_millis += step_s * 1000u;
  g_wall_minute = (uint16_t)tz_rule::local_minute_of_day(wall);
  g_wall = wall;
  updateCsiClockOffset(wall);                 // syncClockFromGps(), every pass
  if (csi) {
    const csi_features_t f = window_of(doppler);
    securacv_csi_modules_feed(&f);
  }
  securacv_csi_modules_tick();
}

// Passes every `step_s` seconds from `from` (inclusive) to `to` (exclusive).
static void run(time_t from, time_t to, uint32_t step_s = 30, int8_t doppler = 0,
                bool csi = true) {
  for (time_t t = from; t < to; t += step_s) loop_pass(t, step_s, doppler, csi);
}

static void reset_counts() {
  g_summary_count = 0;
  std::memset(g_summaries, 0, sizeof(g_summaries));
}

// A synced day that crosses 23:55 commits exactly one row, in the window;
// the next day's crossing commits the next one, and nothing else does.
static int test_a_day_crossing_2355_commits_one_summary() {
  host_prefs().clear();
  reset_counts();
  reboot_and_boot();
  run(kOct3 + 12 * 3600, kOct3 + kDay + 30 * 60);       // 12:00 to 00:30
  CHECK(g_summary_count == 1);
  CHECK(g_summaries[0].minute >= 1435 && g_summaries[0].minute <= 1439);
  CHECK(g_summaries[0].minute == 1435);                 // the first pass at 23:55

  run(kOct3 + kDay + 30 * 60, kOct3 + 2 * kDay + 30 * 60);   // the next day
  CHECK(g_summary_count == 2);
  CHECK(g_summaries[1].minute == 1435);
  CHECK(g_summaries[1].id > g_summaries[0].id);
  return 0;
}

// A reboot after the 23:55 row, still inside the window, commits no second
// one; a boot that starts before the window still owes the day's row; and
// the day after a reboot inside the window gets its row as usual.
static int test_a_reboot_after_the_summary_commits_no_second() {
  host_prefs().clear();
  reset_counts();
  reboot_and_boot();
  run(kOct3 + 23 * 3600, kOct3 + 23 * 3600 + 56 * 60);  // 23:00 to 23:56
  CHECK(g_summary_count == 1);

  reboot_and_boot();                                    // a reboot at 23:57
  run(kOct3 + 23 * 3600 + 57 * 60, kOct3 + kDay + 10 * 60);
  CHECK(g_summary_count == 1);                          // no second row today

  run(kOct3 + kDay + 10 * 60, kOct3 + 2 * kDay + 10 * 60);
  CHECK(g_summary_count == 2);                          // tomorrow's, as usual

  // A reboot at 23:50, before the window: that day's row is still owed.
  reset_counts();
  reboot_and_boot();
  run(kOct3 + 23 * 3600 + 50 * 60, kOct3 + kDay + 5 * 60);
  CHECK(g_summary_count == 1);
  CHECK(g_summaries[0].minute == 1435);
  return 0;
}

// A canary whose clock never synced reads an epoch near zero (the ESP32's
// post-boot clock). Fifty simulated hours of loop passes, whose uptime-based
// minute of day crosses 23:55 twice, commit no summary: the updater feeds
// nothing below the floor. Once the clock syncs, that day's 23:55 commits.
static int test_an_unsynced_clock_commits_none() {
  host_prefs().clear();
  reset_counts();
  reboot_and_boot();
  run(0, 50 * 3600, 60);
  CHECK(g_summary_count == 0);

  // GPS sets the clock at 22:00; the summary follows at 23:55.
  run(kOct3 + 22 * 3600, kOct3 + kDay + 60, 60);
  CHECK(g_summary_count == 1);
  CHECK(g_summaries[0].minute == 1435);
  return 0;
}

// The clock's first sync, or a boot, inside 23:55..23:59 commits nothing that
// day (the boot cannot know whether an earlier one already did); the next
// day's crossing commits as usual. A clock stepped back inside the window
// after the row does not commit a second.
static int test_a_first_clock_inside_the_window_waits_for_the_next_day() {
  host_prefs().clear();
  reset_counts();
  reboot_and_boot();
  run(0, 600, 30);                                      // unsynced at boot
  run(kOct3 + 23 * 3600 + 57 * 60, kOct3 + kDay + 30 * 60);   // synced at 23:57
  CHECK(g_summary_count == 0);
  run(kOct3 + kDay + 30 * 60, kOct3 + 2 * kDay);        // to the next midnight
  CHECK(g_summary_count == 1);

  // Back-stepped to 23:54, then through the window again: still one.
  run(kOct3 + 2 * kDay - 6 * 60, kOct3 + 2 * kDay);
  CHECK(g_summary_count == 1);
  return 0;
}

// The summary fires on a module tick, so a window-less stretch commits none
// (the 23:55 row waits for a window inside 23:55..23:59). But the latch lets
// go on the clock, not on a tick: a night with no windows across midnight
// still owes the next day's row. Before F121 the latch let go only on a tick
// before 00:30, so such a night lost every later summary.
static int test_a_night_without_windows_still_owes_the_next_row() {
  host_prefs().clear();
  reset_counts();
  reboot_and_boot();
  run(kOct3 + 23 * 3600, kOct3 + 23 * 3600 + 59 * 60);
  CHECK(g_summary_count == 1);
  run(kOct3 + 23 * 3600 + 59 * 60, kOct3 + kDay + 3600, 30, 0, false);   // 23:59 to 01:00
  run(kOct3 + kDay + 3600, kOct3 + 2 * kDay);
  CHECK(g_summary_count == 2);

  // No window inside the next window at all: that day commits none.
  run(kOct3 + 2 * kDay, kOct3 + 2 * kDay + 23 * 3600 + 50 * 60);
  run(kOct3 + 2 * kDay + 23 * 3600 + 50 * 60, kOct3 + 3 * kDay + 3600, 30, 0, false);
  CHECK(g_summary_count == 2);
  return 0;
}

// What the row carries, held to docs/csi_modules.md's row: a P0 event row of
// type daily_summary whose note is "a<active> q<empty> x<anomaly>", counted
// over the committed ring rows it walked (bundled = how many), with the
// 23:50 bucket. A day of presence changes still reads "a0 q0 x0": a closed
// presence bundle never reaches the ring (sweep F77, a decision this does not
// take). The MQTT events body carries type and bundled but no note, and its
// event_type is the empty state's "unknown".
static int test_what_the_summary_row_carries() {
  host_prefs().clear();
  reset_counts();
  reboot_and_boot();
  run(kOct3 + 20 * 3600, kOct3 + 21 * 3600, 30, 60);    // an active hour
  run(kOct3 + 21 * 3600, kOct3 + 23 * 3600, 30, 0);     // then quiet
  run(kOct3 + 23 * 3600, kOct3 + 23 * 3600 + 56 * 60);
  CHECK(g_summary_count == 1);
  const Summary& s = g_summaries[0];
  CHECK(s.category == CSI_CATEGORY_EVENT);
  CHECK(s.privacy == CSI_PRIVACY_P0);
  CHECK(s.values.state_name[0] == '\0');
  CHECK(std::strcmp(s.values.note, "a0 q0 x0") == 0);
  CHECK(s.values.time_bucket == 143);                   // 23:50..23:59
  CHECK(s.values.present_fields & CSI_FIELD_BUNDLED_COUNT);

  csi_event_record_t ring[64];
  const size_t n = csi_event_recent(ring, 64);
  // The ring now holds the summary itself on top of the rows it walked.
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
  CHECK(std::strstr(body, "a0 q0 x0") == nullptr);      // the note rides nowhere
  CHECK(std::strstr(body, "\"note\"") == nullptr);
  return 0;
}

// The household's zone, as the device's set_timezone path applies it.
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
// 00:57 on the next date, and that date still owes its row. Before the latch
// was keyed on the date, it had none.
static int test_a_zone_moved_east_after_the_row_owes_the_next_row() {
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
  set_zone("UTC0");
  return 0;
}

// A clock stepped from 23:58 to 00:45 (a GPS correction after a drift) skips
// every minute before 00:30, and the next date still owes its row. Before the
// latch was keyed on the date, it had none.
static int test_a_clock_stepped_past_0030_owes_the_next_row() {
  host_prefs().clear();
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

// The tests call the cut updater on every pass. On the device that is
// syncClockFromGps(), which loop() calls on every pass: it feeds the updater
// inside `if (clock_set)`, before the early return that skips the rest
// between GPS resync checks. The call after a GPS step alone would feed the
// module only when GPS moves the clock, almost never inside 23:55..23:59.
static int test_the_loop_feeds_the_clock_on_every_pass() {
  const std::string main_cpp = read_file(CANARY_MAIN_CPP);
  CHECK(!main_cpp.empty());
  const std::string sync = body_of(main_cpp, "static void syncClockFromGps() {");
  CHECK(!sync.empty());
  const size_t guard = sync.find("\n  if (clock_set) {\n");
  CHECK(guard != std::string::npos);
  const size_t close = sync.find("\n  }\n", guard);
  const size_t feed = sync.find("\n    updateCsiClockOffset(sys_now);\n", guard);
  const size_t resync = sync.find("CLOCK_RESYNC_INTERVAL_MS");
  CHECK(close != std::string::npos && feed != std::string::npos && resync != std::string::npos);
  CHECK(guard < feed && feed < close && close < resync);
  CHECK(sync.find("return;", resync) != std::string::npos);
  const std::string loop = body_of(main_cpp, "void loop() {");
  CHECK(loop.find("\n  syncClockFromGps();\n") != std::string::npos);
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
  rc |= test_what_the_summary_row_carries();
  rc |= test_dst_at_midnight_keeps_one_row_per_date();
  rc |= test_a_zone_moved_east_after_the_row_owes_the_next_row();
  rc |= test_a_clock_stepped_past_0030_owes_the_next_row();
  rc |= test_a_zone_moved_west_after_midnight_commits_no_second();
  rc |= test_the_household_minute_is_local();
  rc |= test_the_loop_feeds_the_clock_on_every_pass();
  if (rc != 0) {
    std::fprintf(stderr, "test_csi_daily_summary: FAILED\n");
    return 1;
  }
  std::printf("test_csi_daily_summary: ALL %d CHECKS PASSED\n", g_checks);
  return 0;
}
