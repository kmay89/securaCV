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
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
  csi_event_category_t category;
  csi_privacy_class_t  privacy;
  csi_event_values_t values;
};
static Summary g_summaries[16];
static int g_summary_count = 0;
static uint16_t g_wall_minute = 0xffff;   // the minute the current pass runs at
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
  if (rc != 0) {
    std::fprintf(stderr, "test_csi_daily_summary: FAILED\n");
    return 1;
  }
  std::printf("test_csi_daily_summary: ALL %d CHECKS PASSED\n", g_checks);
  return 0;
}
