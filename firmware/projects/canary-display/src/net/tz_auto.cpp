// src/net/tz_auto.cpp — learn the house's timezone from the web. See
// tz_auto.h for the contract.
//
// Secrets are included FIRST (same __has_include chain as
// runtime_config.cpp), before the flavor config's #ifndef CD_TZ default —
// so a hand-set CD_TZ / CD_TZ_EXPLICIT / CD_TZ_WEB_LOOKUP is visible in
// THIS translation unit (review catch: documented overrides that only
// secrets.h defines must be included where they're honored).
#if __has_include("secrets.h")
  #include "secrets.h"
#elif __has_include("secrets/secrets.h")
  #include "secrets/secrets.h"
#endif
#include <config.h>
#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <string.h>
#include <time.h>

#include "canary/net/tz_auto.h"
#include "canary/net/wifi_mgr.h"
#include "canary/log.h"

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

// final_rule=false marks a fixed-offset fallback: applied and persisted
// (right today), but NOT treated as learned — the learner keeps retrying
// on a long cadence so a zone that observes DST self-corrects after a
// later successful table match (review catch: marking the fallback as
// learned froze those devices an hour wrong after the next transition).
void apply_and_persist(const char* posix, const char* iana, bool final_rule) {
  configTzTime(posix, "pool.ntp.org", "time.nist.gov");
  Preferences p;
  if (p.begin("scv-tz", false)) {
    p.putString("posix", posix);
    if (iana) p.putString("iana", iana);
    p.putUChar("fixed", final_rule ? 0 : 1);
    p.end();
  }
  s_learned = final_rule;
  canary::log_line("TZ", final_rule ? "Timezone learned and applied."
                                    : "Timezone offset applied (fixed).");
}

}  // namespace

void tz_boot_string(const char* seed, char* out, unsigned cap) {
  Preferences p;
  out[0] = '\0';
  uint8_t fixed = 0;
  if (p.begin("scv-tz", true)) {
    p.getString("posix", out, cap);
    fixed = p.getUChar("fixed", 0);
    p.end();
  }
  if (out[0]) {
    // A fixed-offset fallback boots with the stored value but leaves the
    // learner armed; only a full rule ends the search.
    s_learned = fixed == 0;
    return;
  }
  // No NVS value yet: the seed as THIS unit sees it. The secrets-first
  // include above means a hand-set CD_TZ wins over the flavor's seed
  // (America/New_York) even though the caller's copy of CD_TZ may be the
  // flavor default.
  (void)seed;
  strlcpy(out, CD_TZ, cap);
}

bool tz_learned() { return s_learned; }

bool tz_set_manual(const char* posix) {
  if (!posix || !posix[0]) return false;
  // 48 is the buffer every reader of this value uses (tz_boot_string's
  // callers, the ZONES table's widest rule); refuse rather than store a
  // string that would come back truncated into an invalid TZ.
  if (strlen(posix) >= 48) return false;
  // final_rule=true: a human said so. That marks the zone learned, which
  // also stops tz_auto_tick from ever looking it up again — an explicit
  // choice outranks the network, the same way CD_TZ_EXPLICIT does.
  apply_and_persist(posix, nullptr, /*final_rule=*/true);
  s_gave_up = true;
  return true;
}

void tz_current(char* out, unsigned cap) {
  if (!out || cap == 0) return;
  tz_boot_string(CD_TZ, out, cap);
}

void tz_auto_tick(uint32_t now_ms) {
#if !defined(CD_TZ_WEB_LOOKUP) || !CD_TZ_WEB_LOOKUP
  // PRIVACY DEFAULT (review catch): the lookup sends the household's
  // public IP to a third-party geolocation service, and this project's
  // invariant is that nothing leaves the home unasked. The learner is
  // therefore compiled OUT unless secrets.h explicitly opts in with
  // CD_TZ_WEB_LOOKUP 1 — until then, wall time comes from CD_TZ.
  (void)now_ms;
  return;
#else
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
      apply_and_persist(rule, iana, /*final_rule=*/true);
      return;
    }
  }
  long off = 0;
  if (json_int(body, "offset", &off) && off >= -14L * 3600 &&
      off <= 14L * 3600) {
    // Unknown zone: fixed offset — right today, not final. Re-arm the
    // learner on a slow cadence (and stop charging the give-up budget)
    // so a DST-observing zone self-corrects after a later lookup.
    offset_to_posix(off, posix, sizeof(posix));
    apply_and_persist(posix, iana[0] ? iana : nullptr, /*final_rule=*/false);
    s_attempts = 0;
    s_next_try_ms = now_ms + 6UL * 3600UL * 1000UL;
  }
#endif  // CD_TZ_WEB_LOOKUP
}

}  // namespace canary::net
