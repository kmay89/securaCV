// src/net/tz_auto.cpp — learn the house's timezone from the web. See
// tz_auto.h for the contract.
#include "flavor_config.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <string.h>
#include <time.h>

#include "tz_auto.h"
#include "wifi_mgr.h"
#include "log.h"

namespace canary::net {

namespace {

// IANA zone -> full POSIX rule, DST transitions included. Covers the
// zones a home display realistically lands in; anything else falls back
// to the service's fixed offset (right today, no DST flips). Flash-
// resident string pairs — ~1.5 KB.
struct ZoneRule { const char* iana; const char* posix; };
const ZoneRule ZONES[] = {
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
};

bool s_learned = false;
bool s_gave_up = false;
uint32_t s_next_try_ms = 15000;  // let WiFi/NTP settle first
uint8_t s_attempts = 0;

// Extract "key":"value" or "key":number from a small JSON body without a
// JSON library. Good enough for a two-field response we requested.
bool json_str(const String& body, const char* key, char* out, size_t cap) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  int a = body.indexOf(pat);
  if (a < 0) return false;
  a += strlen(pat);
  int b = body.indexOf('"', a);
  if (b < 0 || b - a <= 0 || (size_t)(b - a) >= cap) return false;
  memcpy(out, body.c_str() + a, b - a);
  out[b - a] = '\0';
  return true;
}

bool json_int(const String& body, const char* key, long* out) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  int a = body.indexOf(pat);
  if (a < 0) return false;
  *out = strtol(body.c_str() + a + strlen(pat), nullptr, 10);
  return true;
}

const char* zone_to_posix(const char* iana) {
  for (const auto& z : ZONES)
    if (strcmp(z.iana, iana) == 0) return z.posix;
  return nullptr;
}

// POSIX fixed-offset string from seconds east of UTC. POSIX inverts the
// sign: UTC-4 (offset -14400) is "LT4".
void offset_to_posix(long sec_east, char* out, size_t cap) {
  long west = -sec_east;
  const char sign = west < 0 ? '-' : '+';
  long a = west < 0 ? -west : west;
  const long hh = a / 3600, mm = (a % 3600) / 60;
  if (mm)
    snprintf(out, cap, "LT%c%ld:%02ld", sign, hh, mm);
  else
    snprintf(out, cap, "LT%c%ld", sign, hh);
}

void apply_and_persist(const char* posix, const char* iana) {
  // Re-arm SNTP with the learned rule; the servers stay the cross-checking
  // pair from boot.
  configTzTime(posix, "pool.ntp.org", "time.nist.gov");
  Preferences p;
  if (p.begin("scv-tz", false)) {
    p.putString("posix", posix);
    if (iana) p.putString("iana", iana);
    p.end();
  }
  s_learned = true;
  canary::log_line("TZ", "Timezone learned and applied.");
}

}  // namespace

void tz_boot_string(const char* seed, char* out, unsigned cap) {
  Preferences p;
  out[0] = '\0';
  if (p.begin("scv-tz", true)) {
    p.getString("posix", out, cap);
    p.end();
  }
  if (out[0]) {
    s_learned = true;
    return;
  }
  strlcpy(out, seed, cap);
}

bool tz_learned() { return s_learned; }

void tz_auto_tick(uint32_t now_ms) {
#ifdef CD_TZ_EXPLICIT
  // A hand-set secrets.h timezone always wins; never auto-learn over it.
  return;
#endif
  if (s_learned || s_gave_up) return;
  if (!wifi_connected()) return;
  if ((int32_t)(now_ms - s_next_try_ms) < 0) return;
  if (s_attempts >= 6) {  // ~1h of backoff spent; stop burning radio
    s_gave_up = true;
    canary::log_line("TZ", "Timezone lookup gave up (offline service?).");
    return;
  }
  s_attempts++;
  s_next_try_ms = now_ms + 60000UL * s_attempts * 2;  // 2,4,6,8,10 min

  HTTPClient http;
  http.setTimeout(4000);
  // Free, keyless, HTTP-friendly IP geolocation; two fields only.
  if (!http.begin("http://ip-api.com/json/?fields=status,timezone,offset"))
    return;
  const int code = http.GET();
  if (code != 200) {
    http.end();
    return;
  }
  String body = http.getString();
  http.end();
  if (body.indexOf("\"success\"") < 0) return;

  char iana[48] = {0};
  char posix[48] = {0};
  if (json_str(body, "timezone", iana, sizeof(iana))) {
    const char* rule = zone_to_posix(iana);
    if (rule) {
      apply_and_persist(rule, iana);
      return;
    }
  }
  long off = 0;
  if (json_int(body, "offset", &off) && off >= -14L * 3600 &&
      off <= 14L * 3600) {
    // Unknown zone: fixed offset — right today; DST flips will be a two-
    // day wait for the next successful lookup after the change.
    offset_to_posix(off, posix, sizeof(posix));
    apply_and_persist(posix, iana[0] ? iana : nullptr);
  }
}

}  // namespace canary::net
