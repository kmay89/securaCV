// Host-side test for gnss_time.h — the NMEA UTC date/time -> Unix epoch
// conversion GPS-derived system-clock sync relies on. Pure header, no Arduino
// glue: the test includes it directly and checks known epoch values plus the
// calendar-validity guardrails (rejects corrupt/void-looking fields before
// they'd otherwise reach settimeofday()).

#include "../../../common/gnss/gnss_time.h"

#include <cstdio>

using securacv::gnss::days_from_civil;
using securacv::gnss::gnss_calendar_valid;
using securacv::gnss::gnss_utc_to_unix;

static int g_failures = 0;
#define CHECK(cond, msg)                                          \
  do {                                                            \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; }   \
    else         { printf("ok:   %s\n", (msg)); }                 \
  } while (0)

int main() {
  time_t t = 0;

  // 2026-01-01 00:00:00 UTC is a known epoch value (1767225600).
  CHECK(gnss_utc_to_unix(2026, 1, 1, 0, 0, 0, &t) && t == 1767225600,
        "2026-01-01T00:00:00Z matches known epoch");

  // The calendar-math itself (days_from_civil) is exercised directly here,
  // independent of the trust-window check, against known reference epochs:
  // the Unix epoch itself, and a date taken from an actual NMEA RMC example
  // ($GPRMC,123519,A,...,230394,... -> 1994-03-23T12:35:19Z).
  CHECK(days_from_civil(1970, 1, 1) * 86400 == 0, "1970-01-01T00:00:00Z is epoch 0");
  CHECK(days_from_civil(1994, 3, 23) * 86400 + 12 * 3600 + 35 * 60 + 19 == 764426119,
        "sample NMEA RMC time matches its known epoch");

  // Leap day is valid; the day after is not.
  CHECK(gnss_calendar_valid(2024, 2, 29, 0, 0, 0), "2024-02-29 is valid (leap year)");
  CHECK(!gnss_calendar_valid(2023, 2, 29, 0, 0, 0), "2023-02-29 is invalid (not a leap year)");
  CHECK(!gnss_calendar_valid(2100, 2, 29, 0, 0, 0),
        "2100-02-29 is invalid (divisible by 100, not 400)");
  CHECK(gnss_calendar_valid(2028, 2, 29, 0, 0, 0),
        "2028-02-29 is valid (divisible by 4, in-window)");
  // 2000 is divisible by 400 (leap) but predates the trust window — checked
  // via days_from_civil directly so the two rules aren't conflated.
  CHECK(days_from_civil(2000, 2, 29) == days_from_civil(2000, 2, 28) + 1,
        "2000-02-29 exists in the civil calendar (divisible by 400)");

  // Out-of-range fields a corrupt-but-checksum-valid sentence, or a receiver
  // that hasn't loaded ephemeris yet (all-zero fields), could produce.
  CHECK(!gnss_calendar_valid(2026, 0, 1, 0, 0, 0), "month 0 is invalid");
  CHECK(!gnss_calendar_valid(2026, 13, 1, 0, 0, 0), "month 13 is invalid");
  CHECK(!gnss_calendar_valid(2026, 1, 32, 0, 0, 0), "day 32 is invalid");
  CHECK(!gnss_calendar_valid(2026, 4, 31, 0, 0, 0), "April 31 is invalid (30-day month)");
  CHECK(!gnss_calendar_valid(2026, 1, 1, 24, 0, 0), "hour 24 is invalid");
  CHECK(!gnss_calendar_valid(2026, 1, 1, 0, 60, 0), "minute 60 is invalid");
  CHECK(!gnss_calendar_valid(2026, 1, 1, 0, 0, 60), "second 60 is invalid (no leap-second modeling)");
  CHECK(!gnss_calendar_valid(1980, 1, 1, 0, 0, 0), "year 1980 is outside the trust window");
  CHECK(!gnss_calendar_valid(2200, 1, 1, 0, 0, 0), "year 2200 is outside the trust window");

  // gnss_utc_to_unix() refuses to write *out when validation fails.
  t = 42;
  CHECK(!gnss_utc_to_unix(2026, 13, 1, 0, 0, 0, &t) && t == 42,
        "invalid input leaves *out untouched");

  printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
