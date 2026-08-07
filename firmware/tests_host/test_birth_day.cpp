/* Host tests for the birth-day decision (firmware/common/identity/birth_day.h).
 *
 * Every test here is about a way the certificate could lie. The module has one
 * job — decide whether to write a birth day and whether it may be called one —
 * and each rule it enforces exists because the alternative produces a device
 * that misstates its own age. So the suite is organized by the lie prevented,
 * not by the function called.
 *
 * Build & run (via firmware/tests_host/Makefile, mirrors the CI contract):
 *   g++ -std=c++17 -Wall -Wextra -Werror -I ../common test_birth_day.cpp
 */
#include <cstdio>
#include <cstdint>

#include "identity/birth_day.h"

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

/* 2026-08-07T12:00:00Z — a believable clock, mid-day so the day arithmetic
 * has something to truncate. */
static constexpr uint32_t kNoon = 1786190400u;

// ── the lie: "born today", every day ────────────────────────────────────────
//
// Rule 1. A device that restamps launders its age: the certificate would show
// the date of the most recent boot with a good clock, and an old Canary would
// look newborn after any outage. Nothing may overwrite a recorded day.
static void test_a_recorded_day_is_never_restated() {
  birth::Stamp stored;
  stored.day = birth::day_of(kNoon) - 400;   // born well over a year ago
  stored.exact = true;

  birth::Observation now;
  now.unix_s = kNoon;
  now.key_age_known = true;
  now.key_age_s = 5;

  birth::Stamp out;
  CHECK(!birth::consider(stored, now, &out));

  // Not even a *worse* stamp may replace it — an inexact one especially.
  birth::Stamp inexact_stored;
  inexact_stored.day = birth::day_of(kNoon) - 400;
  inexact_stored.exact = false;
  CHECK(!birth::consider(inexact_stored, now, &out));
}

// ── the lie: "born 1 January 1970" ──────────────────────────────────────────
//
// Rule 2. An ESP32 boots at the epoch. Stamping that reading would both brand
// the device with a nonsense date and, because of rule 1, make it permanent.
static void test_the_boot_epoch_is_not_a_date() {
  birth::Stamp none;
  birth::Observation now;
  now.key_age_known = true;
  now.key_age_s = 1;
  birth::Stamp out;

  now.unix_s = 0;
  CHECK(!birth::consider(none, now, &out));
  now.unix_s = 42;                             // a few seconds of uptime
  CHECK(!birth::consider(none, now, &out));
  now.unix_s = birth::kClockFloor - 1;         // plausible-looking but too old
  CHECK(!birth::consider(none, now, &out));

  now.unix_s = birth::kClockFloor;             // …and the floor itself passes
  CHECK(birth::consider(none, now, &out));
  CHECK(out.day == birth::day_of(birth::kClockFloor));
}

// ── the lie: a shelf becomes a birthday ─────────────────────────────────────
//
// Rule 3. A Canary flashed in a workshop and plugged in a week later first
// learns the date a week after it was born. Recording that day is right; calling
// it a birth day is not, and the flag is what lets the app say "first dated".
static void test_exactness_tracks_how_fresh_the_key_was() {
  birth::Stamp none;
  birth::Stamp out;
  birth::Observation now;
  now.unix_s = kNoon;

  // Provisioned and online in a minute: this is a birthday.
  now.key_age_known = true;
  now.key_age_s = 60;
  CHECK(birth::consider(none, now, &out));
  CHECK(out.exact);

  // Right on the grace boundary — still a birthday.
  now.key_age_s = birth::kGraceSeconds;
  CHECK(birth::consider(none, now, &out));
  CHECK(out.exact);

  // One second past it is not.
  now.key_age_s = birth::kGraceSeconds + 1;
  CHECK(birth::consider(none, now, &out));
  CHECK(out.day == birth::day_of(kNoon));
  CHECK(!out.exact);

  // A key that predates this boot has an age nobody kept. The day is still
  // worth recording — it bounds the device's age — but it is not a birthday.
  now.key_age_known = false;
  now.key_age_s = 0;              // would look freshest of all if believed
  CHECK(birth::consider(none, now, &out));
  CHECK(out.day == birth::day_of(kNoon));
  CHECK(!out.exact);
}

// ── the arithmetic the date rendering rests on ──────────────────────────────
static void test_days_carry_no_time_of_day() {
  const uint32_t day = birth::day_of(kNoon);
  CHECK(birth::unix_of_day(day) <= kNoon);
  CHECK(kNoon - birth::unix_of_day(day) < birth::kSecondsPerDay);
  CHECK(birth::unix_of_day(day) % birth::kSecondsPerDay == 0);

  // Every second of a day maps to that day, and the next second rolls over.
  const uint32_t midnight = birth::unix_of_day(day);
  CHECK(birth::day_of(midnight) == day);
  CHECK(birth::day_of(midnight + birth::kSecondsPerDay - 1) == day);
  CHECK(birth::day_of(midnight + birth::kSecondsPerDay) == day + 1);
}

// ── the caller's contract ───────────────────────────────────────────────────
static void test_a_null_destination_writes_nothing() {
  birth::Stamp none;
  birth::Observation now;
  now.unix_s = kNoon;
  CHECK(!birth::consider(none, now, nullptr));
}

// A stamp that has never been written must not read as one — the certificate
// card decides between "Born" and "Paired" on exactly this.
static void test_zero_is_not_a_recorded_day() {
  birth::Stamp none;
  CHECK(!none.recorded());
  CHECK(!none.exact);
  birth::Stamp some;
  some.day = 1;
  CHECK(some.recorded());
}

int main() {
  test_a_recorded_day_is_never_restated();
  test_the_boot_epoch_is_not_a_date();
  test_exactness_tracks_how_fresh_the_key_was();
  test_days_carry_no_time_of_day();
  test_a_null_destination_writes_nothing();
  test_zero_is_not_a_recorded_day();

  if (g_failures) {
    std::printf("test_birth_day: %d FAILURE(S)\n", g_failures);
    return 1;
  }
  std::printf("test_birth_day: all checks passed\n");
  return 0;
}
