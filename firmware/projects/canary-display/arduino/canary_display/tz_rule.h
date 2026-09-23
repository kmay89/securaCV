// firmware/common/time/tz_rule.h — the fleet's one IANA -> POSIX TZ table,
// the POSIX-rule validator, and the local minute-of-day every day-aligned
// feature keys on (repo sweep F28).
//
// Why a table and not a lookup service: the canary-wap has no SNTP and no web
// egress at all, and nothing leaves the home unasked. A phone already knows
// its IANA zone (Intl.DateTimeFormat().resolvedOptions().timeZone) and it is
// standing on the device's own setup network, so the zone travels one hop
// and is mapped here, on the device, to a full POSIX rule with its DST
// transitions (a bare UTC offset would go wrong twice a year).
//
// Consumers:
//   - canary-display src/net/tz_auto.cpp — its geo-IP learner (compiled out by
//     default) maps the zone it hears through posix_for_iana(). The table
//     used to live there; it moved here unchanged, plus the IANA aliases the
//     display's own portal already recognizes (its TZS list) and UTC, so a
//     phone-reported zone means the same thing on every Canary.
//   - canary (PIO) and canary-wap — the household time zone setting: seeded
//     at provisioning from the phone's zone, editable on the settings
//     surfaces, stored in NVS as the POSIX rule, applied with
//     setenv("TZ") + tzset() (NOT configTzTime, which also starts SNTP — the
//     WAP deliberately has none). local_minute_of_day() then drives the CSI
//     chokepoint's time_bucket / quiet-hours clock offset.
//
// An UNSET zone keeps today's behavior byte-for-byte: with no TZ in the
// environment newlib's localtime_r is UTC, so local_minute_of_day() equals the
// old (t % 86400) / 60.
//
// Pure: no Arduino, no NVS. Host test: firmware/tests_host/test_tz_rule.cpp
// (glibc honors the same POSIX rules via setenv/tzset/localtime_r). Staged
// flat into the canary-wap sketch (check_csi_sync.sh) and the display sketch
// (setup.sh regen).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

namespace tz_rule {

// Longest POSIX rule any Canary stores: the display's existing 48-byte buffer
// (tz_set_manual refuses >= 48) is the fleet-wide ceiling.
constexpr size_t MAX_POSIX_LEN = 47;
// Longest IANA name accepted from a client ("America/Argentina/Buenos_Aires"
// is 30).
constexpr size_t MAX_IANA_LEN = 47;

struct ZoneRule {
  const char* iana;
  const char* posix;
};

// IANA zone -> full POSIX rule, DST transitions included. The first block is
// the display's tz_auto.cpp table, moved verbatim; the second is the aliases
// its onboarding portal already maps (provision.cpp TZS) plus UTC — the host
// test proves each alias keeps the same wall clock as the portal's rule.
// Flash-resident string pairs.
inline const ZoneRule* zones(size_t* count) {
  static const ZoneRule kZones[] = {
      {"America/New_York",    "EST5EDT,M3.2.0,M11.1.0"},
      {"America/Toronto",     "EST5EDT,M3.2.0,M11.1.0"},
      {"America/Detroit",     "EST5EDT,M3.2.0,M11.1.0"},
      {"America/Chicago",     "CST6CDT,M3.2.0,M11.1.0"},
      {"America/Winnipeg",    "CST6CDT,M3.2.0,M11.1.0"},
      {"America/Mexico_City", "CST6"},
      {"America/Denver",      "MST7MDT,M3.2.0,M11.1.0"},
      {"America/Edmonton",    "MST7MDT,M3.2.0,M11.1.0"},
      {"America/Phoenix",     "MST7"},
      {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
      {"America/Vancouver",   "PST8PDT,M3.2.0,M11.1.0"},
      {"America/Anchorage",   "AKST9AKDT,M3.2.0,M11.1.0"},
      {"Pacific/Honolulu",    "HST10"},
      {"America/Sao_Paulo",   "<-03>3"},
      {"America/Argentina/Buenos_Aires", "<-03>3"},
      {"America/Bogota",      "<-05>5"},
      {"Europe/London",       "GMT0BST,M3.5.0/1,M10.5.0"},
      {"Europe/Dublin",       "GMT0IST,M3.5.0/1,M10.5.0"},
      {"Europe/Lisbon",       "WET0WEST,M3.5.0/1,M10.5.0"},
      {"Europe/Paris",        "CET-1CEST,M3.5.0,M10.5.0/3"},
      {"Europe/Berlin",       "CET-1CEST,M3.5.0,M10.5.0/3"},
      {"Europe/Madrid",       "CET-1CEST,M3.5.0,M10.5.0/3"},
      {"Europe/Rome",         "CET-1CEST,M3.5.0,M10.5.0/3"},
      {"Europe/Amsterdam",    "CET-1CEST,M3.5.0,M10.5.0/3"},
      {"Europe/Brussels",     "CET-1CEST,M3.5.0,M10.5.0/3"},
      {"Europe/Stockholm",    "CET-1CEST,M3.5.0,M10.5.0/3"},
      {"Europe/Warsaw",       "CET-1CEST,M3.5.0,M10.5.0/3"},
      {"Europe/Athens",       "EET-2EEST,M3.5.0/3,M10.5.0/4"},
      {"Europe/Helsinki",     "EET-2EEST,M3.5.0/3,M10.5.0/4"},
      {"Europe/Kyiv",         "EET-2EEST,M3.5.0/3,M10.5.0/4"},
      {"Europe/Moscow",       "MSK-3"},
      {"Asia/Dubai",          "<+04>-4"},
      {"Asia/Kolkata",        "IST-5:30"},
      {"Asia/Shanghai",       "CST-8"},
      {"Asia/Hong_Kong",      "HKT-8"},
      {"Asia/Singapore",      "<+08>-8"},
      {"Asia/Tokyo",          "JST-9"},
      {"Asia/Seoul",          "KST-9"},
      {"Australia/Perth",     "AWST-8"},
      {"Australia/Brisbane",  "AEST-10"},
      {"Australia/Sydney",    "AEST-10AEDT,M10.1.0,M4.1.0/3"},
      {"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
      {"Pacific/Auckland",    "NZST-12NZDT,M9.5.0,M4.1.0/3"},
      // The display portal's aliases (provision.cpp TZS), its exact rules.
      {"America/Montreal",    "EST5EDT,M3.2.0,M11.1.0"},
      {"America/Nassau",      "EST5EDT,M3.2.0,M11.1.0"},
      {"America/Boise",       "MST7MDT,M3.2.0,M11.1.0"},
      {"America/Tijuana",     "PST8PDT,M3.2.0,M11.1.0"},
      {"America/Juneau",      "AKST9AKDT,M3.2.0,M11.1.0"},
      {"America/Lima",        "<-05>5"},
      {"Europe/Vienna",       "CET-1CEST,M3.5.0,M10.5.0/3"},
      {"Europe/Oslo",         "CET-1CEST,M3.5.0,M10.5.0/3"},
      {"Europe/Copenhagen",   "CET-1CEST,M3.5.0,M10.5.0/3"},
      {"Europe/Prague",       "CET-1CEST,M3.5.0,M10.5.0/3"},
      {"Europe/Zurich",       "CET-1CEST,M3.5.0,M10.5.0/3"},
      {"Europe/Budapest",     "CET-1CEST,M3.5.0,M10.5.0/3"},
      {"Europe/Bucharest",    "EET-2EEST,M3.5.0/3,M10.5.0/4"},
      {"Europe/Kiev",         "EET-2EEST,M3.5.0/3,M10.5.0/4"},
      {"Europe/Riga",         "EET-2EEST,M3.5.0/3,M10.5.0/4"},
      {"Europe/Sofia",        "EET-2EEST,M3.5.0/3,M10.5.0/4"},
      {"Europe/Istanbul",     "MSK-3"},
      {"Asia/Calcutta",       "IST-5:30"},
      {"Asia/Colombo",        "IST-5:30"},
      {"Asia/Chongqing",      "CST-8"},
      {"Asia/Macau",          "HKT-8"},
      {"Asia/Taipei",         "HKT-8"},
      {"Asia/Kuala_Lumpur",   "<+08>-8"},
      {"Asia/Manila",         "<+08>-8"},
      {"Australia/Hobart",    "AEST-10AEDT,M10.1.0,M4.1.0/3"},
      {"Australia/Canberra",  "AEST-10AEDT,M10.1.0,M4.1.0/3"},
      {"UTC",                 "UTC0"},
      {"Etc/UTC",             "UTC0"},
  };
  if (count) *count = sizeof(kZones) / sizeof(kZones[0]);
  return kZones;
}

// The POSIX rule for an IANA zone, or nullptr when the table does not know it
// (the caller keeps the current setting — an unknown zone never sets UTC).
inline const char* posix_for_iana(const char* iana) {
  if (iana == nullptr || iana[0] == '\0') return nullptr;
  size_t n = 0;
  const ZoneRule* z = zones(&n);
  for (size_t i = 0; i < n; ++i) {
    if (strcmp(z[i].iana, iana) == 0) return z[i].posix;
  }
  return nullptr;
}

// The POSIX TZ grammar, strictly: the only rules this gate passes are ones
// every libc a Canary runs reads to the end. That matters because a rule a
// libc cannot read is NOT safely ignored: the ESP newlib 4.1 base under
// Arduino core 2.0.x (the canary-wap's CI build) returns from tzset() part
// way through a parse without resetting anything (tzset_r.c: a failed offset
// or date is a bare `return`), so the previous zone's offset, or a mix of the
// old and new rule, stays in force until the next reboot, and after it
// whatever the half-read rule left. Only the esp-4.3 base falls back to UTC.
// So the settings surfaces must refuse every rule outside the subset newlib
// 4.1, newlib 4.3 and glibc (the host test) all parse completely:
//
//   std offset [dst [offset] ,date[/time] ,date[/time]]
//
//   std, dst  [A-Za-z]{3,10}  or  <[-+0-9A-Za-z]{3,10}>   (newlib's TZNAME_MAX)
//   offset    [+-]?hh[:mm[:ss]]   hh 0..24 (1-2 digits), mm and ss 00..59
//   date      Mm.w.d (m 1..12, w 1..5, d 0..6) | Jn (1..365) | n (0..365)
//   time      hh[:mm[:ss]]        hh 0..167 (POSIX.1-2024; unsigned, since
//                                  newlib 4.1 reads a '-' as a huge hour)
//
// Narrower than POSIX on purpose, each by what one libc would do otherwise:
// a DST name needs BOTH change dates (with none, each libc supplies its own
// default, the US rules on newlib, so "CET-1CEST" would quietly change clocks
// on US dates); dates without a DST name are refused (every libc ignores
// them); nothing may trail the rule (newlib stops reading at the first thing
// it does not expect and keeps the rest of what it had); no leading ':'.
// Also keeps control bytes, quotes, backslashes and oversize values out of
// NVS (a stored rule is re-applied on every boot): refused, never "cleaned".
namespace detail {

inline bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

// 1..max_digits decimal digits (no more), value in lo..hi.
inline bool take_uint(const char*& p, unsigned max_digits, unsigned lo, unsigned hi) {
  unsigned v = 0, k = 0;
  while (k < max_digits && is_digit(p[k])) v = v * 10 + (unsigned)(p[k++] - '0');
  if (k == 0 || is_digit(p[k]) || v < lo || v > hi) return false;
  p += k;
  return true;
}

// [A-Za-z]{3,10} or <[-+0-9A-Za-z]{3,10}>
inline bool take_name(const char*& p) {
  size_t k = 0;
  if (*p == '<') {
    const char* q = p + 1;
    while (is_alpha(q[k]) || is_digit(q[k]) || q[k] == '+' || q[k] == '-') k++;
    if (k < 3 || k > 10 || q[k] != '>') return false;
    p = q + k + 1;
    return true;
  }
  while (is_alpha(p[k])) k++;
  if (k < 3 || k > 10) return false;
  p += k;
  return true;
}

// hh[:mm[:ss]] with hh <= max_hour; mm and ss exactly two digits, <= 59.
inline bool take_hms(const char*& p, unsigned max_hour_digits, unsigned max_hour) {
  if (!take_uint(p, max_hour_digits, 0, max_hour)) return false;
  for (int part = 0; part < 2 && *p == ':'; ++part) {
    const char* q = p + 1;
    if (!is_digit(q[0]) || !is_digit(q[1]) || is_digit(q[2])) return false;
    if ((q[0] - '0') * 10 + (q[1] - '0') > 59) return false;
    p = q + 2;
  }
  return true;
}

inline bool take_offset(const char*& p) {
  if (*p == '+' || *p == '-') p++;
  return take_hms(p, 2, 24);
}

// ,date[/time]
inline bool take_change(const char*& p) {
  if (*p++ != ',') return false;
  if (*p == 'M') {
    p++;
    if (!take_uint(p, 2, 1, 12)) return false;               // month
    if (*p++ != '.' || !take_uint(p, 1, 1, 5)) return false; // week
    if (*p++ != '.' || !take_uint(p, 1, 0, 6)) return false; // weekday
  } else if (*p == 'J') {
    p++;
    if (!take_uint(p, 3, 1, 365)) return false;              // Julian, no Feb 29
  } else if (!take_uint(p, 3, 0, 365)) {                     // zero-based day
    return false;
  }
  if (*p == '/') {
    p++;
    if (!take_hms(p, 3, 167)) return false;
  }
  return true;
}

}  // namespace detail

inline bool posix_valid(const char* s) {
  if (s == nullptr) return false;
  const size_t n = strlen(s);
  if (n == 0 || n > MAX_POSIX_LEN) return false;
  const char* p = s;
  if (!detail::take_name(p) || !detail::take_offset(p)) return false;
  if (*p == '\0') return true;                                // standard time only
  if (!detail::take_name(p)) return false;                    // the DST name
  if (*p != ',' && !detail::take_offset(p)) return false;     // its optional offset
  if (!detail::take_change(p) || !detail::take_change(p)) return false;
  return *p == '\0';
}

// Minutes since LOCAL midnight (0..1439) for a wall-clock epoch, under
// whatever TZ the process has applied. With no TZ set this is UTC — the exact
// value the pre-F28 (t % 86400) / 60 produced. A localtime_r failure also
// falls back to that UTC value rather than inventing one.
inline int32_t local_minute_of_day(time_t t) {
  struct tm lt;
  if (localtime_r(&t, &lt) != nullptr) {
    return (int32_t)(lt.tm_hour * 60 + lt.tm_min);
  }
  int64_t s = (int64_t)t % 86400;
  if (s < 0) s += 86400;
  return (int32_t)(s / 60);
}

// What a client sent, resolved to the POSIX rule to store.
enum class Resolve : uint8_t {
  OK           = 0,  // out holds the rule
  NONE         = 1,  // neither field present: leave the setting alone
  BAD_RULE     = 2,  // a "tz" that fails posix_valid
  UNKNOWN_ZONE = 3,  // a "tz_iana" the table does not know
};

// An explicit POSIX rule wins over an IANA name when both are present (a
// person typed it). Empty strings count as absent. out must hold
// MAX_POSIX_LEN + 1 bytes; it is written only on OK.
inline Resolve resolve(const char* posix, const char* iana, char* out) {
  if (posix != nullptr && posix[0] != '\0') {
    if (!posix_valid(posix)) return Resolve::BAD_RULE;
    memcpy(out, posix, strlen(posix) + 1);
    return Resolve::OK;
  }
  if (iana != nullptr && iana[0] != '\0') {
    const char* rule = posix_for_iana(iana);
    if (rule == nullptr) return Resolve::UNKNOWN_ZONE;
    memcpy(out, rule, strlen(rule) + 1);
    return Resolve::OK;
  }
  return Resolve::NONE;
}

// The canary-wap's settings handler parses its small JSON bodies by hand
// (no ArduinoJson in that TU). Copy the string value of `quoted_key` (the key
// WITH its quotes, e.g. "\"tz\"", so "tz" never matches "tz_iana") into
// out[cap]. Returns false when the key is absent, the value is not a string,
// it holds an escape (no zone name or rule needs one), or it does not fit.
inline bool json_string_field(const char* body, const char* quoted_key,
                              char* out, size_t cap) {
  if (body == nullptr || quoted_key == nullptr || out == nullptr || cap == 0) return false;
  const char* k = strstr(body, quoted_key);
  if (k == nullptr) return false;
  const char* v = k + strlen(quoted_key);
  while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
  if (*v != ':') return false;
  v++;
  while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
  if (*v != '"') return false;
  v++;
  size_t n = 0;
  while (v[n] != '"') {
    if (v[n] == '\0' || v[n] == '\\') return false;
    n++;
  }
  if (n + 1 > cap) return false;
  memcpy(out, v, n);
  out[n] = '\0';
  return true;
}

}  // namespace tz_rule
