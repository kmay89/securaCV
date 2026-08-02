// src/care/bedside.cpp — nightstand data lines: hub weather cache, bedroom
// comfort words, on-device sun times. See bedside.h for the contract.
#include <config.h>

// LDF lesson: bundled-library includes stay ABOVE feature gates.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>
#include <ctype.h>
#include <string.h>

#include "canary/care/bedside.h"
#include "canary/care/comfort.h"
#include "canary/care/suncalc.h"
#include "canary/log.h"

namespace canary::care {

namespace {

// ── Hub weather cache ─────────────────────────────────────────────────────
constexpr int WX_ABSENT_C10 = -10000;   // "field not in the blob" sentinel
struct WeatherCache {
  bool have = false;
  int t_now_c10 = WX_ABSENT_C10;
  int hi_c10 = 0, lo_c10 = 0;
  int rain_pct = -1;
  char cond[20] = {0};
  // Tomorrow (optional fields; absent on older hub automations).
  int hi2_c10 = WX_ABSENT_C10, lo2_c10 = WX_ABSENT_C10;
  int rain2_pct = -1;
  char cond2[20] = {0};
  // Active advisory/warning (optional; visual-only, never a chime).
  bool alert_have = false;
  char alert_text[48] = {0};
  uint8_t alert_sev = 0;
  int64_t alert_until = 0;     // wall-clock end (epoch seconds)
  int64_t ts = 0;              // hub's wall-clock stamp (staleness gate)
};
WeatherCache s_wx;

// HA's fixed 15-state condition enum -> plain words (glass vocabulary).
const char* cond_word(const char* c) {
  struct Row { const char* key; const char* word; };
  static const Row MAP[] = {
      {"clear-night", "clear"},        {"cloudy", "cloudy"},
      {"exceptional", "wild out"},     {"fog", "foggy"},
      {"hail", "hail"},                {"lightning", "storms"},
      {"lightning-rainy", "storms"},   {"partlycloudy", "some clouds"},
      {"pouring", "heavy rain"},       {"rainy", "rain"},
      {"snowy", "snow"},               {"snowy-rainy", "sleet"},
      {"sunny", "sunny"},              {"windy", "windy"},
      {"windy-variant", "windy"},
  };
  for (const Row& r : MAP) {
    if (strcmp(c, r.key) == 0) return r.word;
  }
  return "";  // unknown condition string: show numbers, skip the word
}

bool weather_fresh() {
  if (!s_wx.have) return false;
  const time_t now = time(nullptr);
  // A forecast is only honest for a few hours; and without valid wall
  // clock we can't judge staleness, so we don't show it at all. The skew
  // window rejects future-stamped blobs too (review catch: a hub with a
  // wrong clock would otherwise be "fresh" forever).
  const int64_t age = (int64_t)now - s_wx.ts;
  return now > 1700000000 && age > -300 && age < 3 * 3600;
}

// ── Sun cache (recomputed when the civil day changes) ────────────────────
struct SunCache {
  int yday = -1;
  bool valid = false;
  int rise_min = 0, set_min = 0;
};
SunCache s_sun;

bool sun_today(int* rise_min, int* set_min) {
  if (CD_LAT > 90.0 || CD_LAT < -90.0) return false;  // unset sentinel
  const time_t now = time(nullptr);
  if (now < 1700000000) return false;
  struct tm lt;
  localtime_r(&now, &lt);
  if (s_sun.yday != lt.tm_yday) {
    struct tm gt;
    gmtime_r(&now, &gt);
    // Local-vs-UTC offset in minutes, DST included, from the same instant.
    int off = (lt.tm_hour - gt.tm_hour) * 60 + (lt.tm_min - gt.tm_min);
    if (lt.tm_yday != gt.tm_yday) {
      // Crossed a date line between the two views of "now".
      const int dd = lt.tm_yday - gt.tm_yday;
      off += (dd == 1 || dd < -1) ? 1440 : -1440;
    }
    s_sun.valid = sun_times(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                            CD_LAT, CD_LON, off, &s_sun.rise_min,
                            &s_sun.set_min);
    s_sun.yday = lt.tm_yday;
  }
  if (!s_sun.valid) return false;
  *rise_min = s_sun.rise_min;
  *set_min = s_sun.set_min;
  return true;
}

}  // namespace

void bedside_on_weather(const char* payload, unsigned len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
    canary::log_line("WX", "Weather update didn't parse - ignored.");
    return;
  }
  if (!doc["hi"].is<float>() || !doc["lo"].is<float>()) return;
  s_wx.hi_c10 = (int)(doc["hi"].as<float>() * 10.0f);
  s_wx.lo_c10 = (int)(doc["lo"].as<float>() * 10.0f);
  s_wx.rain_pct = doc["rain"] | -1;
  snprintf(s_wx.cond, sizeof(s_wx.cond), "%s", (const char*)(doc["cond"] | ""));
  s_wx.ts = doc["ts"] | (int64_t)0;
  // Optional current + tomorrow fields (older automations simply omit them;
  // every absent field re-arms its sentinel so a hub DROPPING a field never
  // leaves yesterday's value rendering as today's truth).
  s_wx.t_now_c10 =
      doc["t_now"].is<float>() ? (int)(doc["t_now"].as<float>() * 10.0f)
                               : WX_ABSENT_C10;
  const bool have2 = doc["hi2"].is<float>() && doc["lo2"].is<float>();
  s_wx.hi2_c10 = have2 ? (int)(doc["hi2"].as<float>() * 10.0f) : WX_ABSENT_C10;
  s_wx.lo2_c10 = have2 ? (int)(doc["lo2"].as<float>() * 10.0f) : WX_ABSENT_C10;
  s_wx.rain2_pct = have2 ? (doc["rain2"] | -1) : -1;
  snprintf(s_wx.cond2, sizeof(s_wx.cond2), "%s",
           have2 ? (const char*)(doc["cond2"] | "") : "");
  // Optional weather advisory/warning. Read through the same `| default`
  // idiom the rest of this parser uses: a missing "alert", a null one, and
  // one with no "event" all land on the empty string and simply don't
  // render — no nested type test needed.
  s_wx.alert_have = false;
  {
    const char* ev = doc["alert"]["event"] | "";
    if (ev[0]) {
      snprintf(s_wx.alert_text, sizeof(s_wx.alert_text), "%s", ev);
      s_wx.alert_sev = (uint8_t)((doc["alert"]["sev"] | 0) != 0 ? 1 : 0);
      s_wx.alert_until = doc["alert"]["until"] | (int64_t)0;
      s_wx.alert_have = true;
    }
  }
  s_wx.have = true;
}

bool bedside_weather(BedsideWeather* out) {
  if (!out || !weather_fresh()) return false;
  out->t_now_c10 = s_wx.t_now_c10;
  out->hi_c10 = s_wx.hi_c10;
  out->lo_c10 = s_wx.lo_c10;
  out->rain_pct = s_wx.rain_pct;
  out->cond = cond_word(s_wx.cond);
  out->hi2_c10 = s_wx.hi2_c10;
  out->lo2_c10 = s_wx.lo2_c10;
  out->rain2_pct = s_wx.rain2_pct;
  out->cond2 = cond_word(s_wx.cond2);
  return true;
}

bool bedside_tomorrow_line(char* out, size_t cap) {
  if (!weather_fresh() || s_wx.hi2_c10 == WX_ABSENT_C10) return false;
  const char* w = cond_word(s_wx.cond2);
  size_t o = (size_t)snprintf(
      out, cap, "tomorrow %d\xC2\xB0/%d\xC2\xB0",
      (s_wx.hi2_c10 + (s_wx.hi2_c10 >= 0 ? 5 : -5)) / 10,
      (s_wx.lo2_c10 + (s_wx.lo2_c10 >= 0 ? 5 : -5)) / 10);
  if (s_wx.rain2_pct > 0 && o < cap) {
    o += (size_t)snprintf(out + o, cap - o, " • rain %d%%", s_wx.rain2_pct);
  }
  if (w[0] && o < cap) {
    snprintf(out + o, cap - o, " • %s", w);
  }
  return true;
}

bool bedside_weather_alert(BedsideWxAlert* out) {
  if (!out || !weather_fresh() || !s_wx.alert_have) return false;
  // An alert is live only while `until` is ahead of a valid wall clock —
  // the same honesty rule as the forecast itself: no clock, no claim.
  const time_t now = time(nullptr);
  if (now < 1700000000) return false;
  if (s_wx.alert_until != 0 && (int64_t)now >= s_wx.alert_until) return false;
  snprintf(out->text, sizeof(out->text), "%s", s_wx.alert_text);
  out->sev = s_wx.alert_sev;
  return true;
}

bool bedside_morning_line(char* out, size_t cap) {
  if (!weather_fresh()) return false;
  const char* w = cond_word(s_wx.cond);
  size_t o = (size_t)snprintf(out, cap, "%d\xC2\xB0/%d\xC2\xB0",
                              (s_wx.hi_c10 + (s_wx.hi_c10 >= 0 ? 5 : -5)) / 10,
                              (s_wx.lo_c10 + (s_wx.lo_c10 >= 0 ? 5 : -5)) / 10);
  if (s_wx.rain_pct > 0 && o < cap) {
    o += (size_t)snprintf(out + o, cap - o, " • rain %d%%", s_wx.rain_pct);
  }
  if (w[0] && o < cap) {
    o += (size_t)snprintf(out + o, cap - o, " • %s", w);
  }
  int rise, set;
  if (sun_today(&rise, &set) && o < cap) {
    snprintf(out + o, cap - o, " • sun up %d:%02d", rise / 60, rise % 60);
  }
  return true;
}

bool bedside_evening_line(char* out, size_t cap) {
  int rise, set;
  if (!sun_today(&rise, &set)) return false;
  snprintf(out, cap, "sun down %d:%02d", set / 60, set % 60);
  return true;
}

bool bedside_comfort_line(const canary::fleet::Fleet& fleet, char* out,
                          size_t cap) {
  using canary::fleet::Witness;
  // Case-insensitive substring (strcasestr is a GNU extension — spell it
  // out so both toolchain rows compile it identically).
  auto contains_ci = [](const char* hay, const char* needle) {
    const size_t nl = strlen(needle);
    if (nl == 0) return true;
    for (const char* h = hay; *h; h++) {
      size_t i = 0;
      while (i < nl && h[i] &&
             tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i]))
        i++;
      if (i == nl) return true;
    }
    return false;
  };
  const Witness* pick = nullptr;
  for (int i = 0; i < fleet.count(); i++) {
    const Witness* w = fleet.at(i);
    if (!w || !w->temp_present) continue;
    if (!pick) pick = w;
    if (w->room[0] && contains_ci(w->room, CD_BEDSIDE_ROOM)) {
      pick = w;
      break;
    }
  }
  if (!pick) return false;

  // Band state persists across calls so the hysteresis actually engages.
  static TempBand s_tb = TempBand::None;
  static RhBand s_rb = RhBand::None;
  s_tb = temp_band(pick->temp_c10, s_tb);
  const char* room = pick->room[0] ? pick->room : "bedroom";
  // Explicit sign: integer division loses the minus between -0.9 and
  // -0.1 °C (the same trap the care wave hit — review catch, again).
  const int at10 = pick->temp_c10 < 0 ? -pick->temp_c10 : pick->temp_c10;
  size_t o = (size_t)snprintf(out, cap, "%.9s %s%d.%d\xC2\xB0 %s", room,
                              pick->temp_c10 < 0 ? "-" : "", at10 / 10,
                              at10 % 10, temp_word(s_tb));
  if (pick->humidity_pct >= 0) {
    s_rb = rh_band(pick->humidity_pct, s_rb);
    const char* hw = rh_word(s_rb);
    if (hw[0] && o < cap) snprintf(out + o, cap - o, " • %s", hw);
  }
  return true;
}

}  // namespace canary::care
