#pragma once
//
// gnss_time — turns a parsed NMEA UTC date/time into a validated Unix epoch.
//
// GNSS time is one of the few genuinely trustworthy clocks a battery witness
// device has: it comes from the satellite constellation, not from a network
// peer, so it's available with no WiFi/internet uplink at all (the canary and
// canary-wap firmware have no SNTP path today). But a checksum-valid NMEA
// sentence only proves the bytes weren't corrupted in transit — it says
// nothing about whether the receiver itself glitched, or whether the field a
// caller handed us actually came from an RMC sentence with status 'A'
// (active/valid) rather than 'V' (void). Both checks belong here, at the one
// choke point every caller converts through, rather than duplicated (and
// possibly forgotten) at each parse site.
//
// Pure and header-only, like gps_privacy.h next to it, so the exact
// conversion the firmware runs is also exercised by the host test harness.
//
#include <cstdint>
#include <ctime>

namespace securacv {
namespace gnss {

// Rejects obviously-wrong calendar fields — a corrupt-but-checksum-valid
// sentence, a receiver that hasn't loaded ephemeris yet and emits zeros, or a
// field we misparsed. Bounds the year to a century-wide window around when
// this code could plausibly run; a GNSS receiver reporting 1980 or 2200 is
// not a clock worth trusting.
inline bool gnss_calendar_valid(int year, int month, int day,
                                 int hour, int minute, int second) {
  if (year < 2016 || year > 2116) return false;
  if (month < 1 || month > 12) return false;
  if (hour < 0 || hour > 23) return false;
  if (minute < 0 || minute > 59) return false;
  if (second < 0 || second > 59) return false;  // leap seconds not modeled

  static const int8_t days_in_month[] = {31, 28, 31, 30, 31, 30,
                                          31, 31, 30, 31, 30, 31};
  bool leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
  int dim = days_in_month[month - 1] + ((month == 2 && leap) ? 1 : 0);
  if (day < 1 || day > dim) return false;
  return true;
}

// Days since the 1970-01-01 civil epoch. Howard Hinnant's days_from_civil —
// exact for all proleptic-Gregorian y/m/d, no libc TZ/mktime dependency (the
// ESP32 Arduino core's timegm() is unreliable across boards, and mktime()
// interprets its input in local time unless TZ is forced to UTC first).
inline int64_t days_from_civil(int64_t y, int m, int d) {
  y -= (m <= 2);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);          // [0, 399]
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// Converts a validated NMEA UTC date/time to a Unix epoch second. Returns
// false (and leaves *out untouched) if the calendar fields fail
// gnss_calendar_valid() — callers must check the return value rather than
// trust whatever was in *out.
inline bool gnss_utc_to_unix(int year, int month, int day,
                              int hour, int minute, int second,
                              time_t* out) {
  if (!out) return false;
  if (!gnss_calendar_valid(year, month, day, hour, minute, second)) return false;

  const int64_t days = days_from_civil(year, month, day);
  const int64_t secs = days * 86400 + hour * 3600 + minute * 60 + second;
  *out = static_cast<time_t>(secs);
  return true;
}

}  // namespace gnss
}  // namespace securacv
