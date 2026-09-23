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

// Is this a string worth handing to tzset()? 1..MAX_POSIX_LEN printable
// ASCII, starts with a letter or '<' (a POSIX std name), no spaces, quotes or
// backslashes. Plausibility, not a full parser: newlib and glibc fall back to
// UTC on a rule they cannot parse, so this gate's job is to keep control
// bytes, JSON-breaking characters and oversize values out of NVS (a stored
// value is re-applied on every boot), refused rather than "cleaned".
inline bool posix_plausible(const char* s) {
  if (s == nullptr) return false;
  const size_t n = strlen(s);
  if (n == 0 || n > MAX_POSIX_LEN) return false;
  const char c0 = s[0];
  const bool letter = (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z');
  if (!letter && c0 != '<') return false;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = (unsigned char)s[i];
    if (c <= 0x20 || c > 0x7E || c == '"' || c == '\'' || c == '\\') return false;
  }
  return true;
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
  BAD_RULE     = 2,  // a "tz" that fails posix_plausible
  UNKNOWN_ZONE = 3,  // a "tz_iana" the table does not know
};

// An explicit POSIX rule wins over an IANA name when both are present (a
// person typed it). Empty strings count as absent. out must hold
// MAX_POSIX_LEN + 1 bytes; it is written only on OK.
inline Resolve resolve(const char* posix, const char* iana, char* out) {
  if (posix != nullptr && posix[0] != '\0') {
    if (!posix_plausible(posix)) return Resolve::BAD_RULE;
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
