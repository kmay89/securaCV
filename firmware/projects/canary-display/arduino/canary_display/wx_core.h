// include/canary/net/wx_core.h — the standalone-weather fetcher's pure half.
//
// FEATURE_STANDALONE_WEATHER (docs/security/SECURITY_MODEL.md — disclosed):
// a hub-less home may OPT IN to the display fetching its own forecast. The
// shape of that request is a privacy promise, so it is built here — pure,
// host-tested (firmware/tests_host/test_wx_core.cpp) — and the test pins
// what the query may contain: a coarse coordinate and field names. No
// identifiers, no API key, no location finer than the 0.1° grid the app
// coarsens to (~11 km). The same anonymous-commons shape as SNTP.
//
// The fetched values leave through ONE door: wx_blob_build renders them as
// the exact retained-blob JSON a hub would have published, and the fetcher
// hands that to bedside_on_weather — so every face, line and advisory
// behaves identically whether the forecast came from a hub or from here.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace canary::net {

inline const char* WX_HOST = "api.open-meteo.com";

// The full request path for a coarse (x10) coordinate. Values are printed
// with one decimal — exactly the grid the store holds, never finer.
inline int wx_url_path(int lat10, int lon10, char* out, size_t cap) {
  const int la = lat10 < 0 ? -lat10 : lat10;
  const int lo = lon10 < 0 ? -lon10 : lon10;
  return snprintf(out, cap,
                  "/v1/forecast?latitude=%s%d.%d&longitude=%s%d.%d"
                  "&current=temperature_2m,weather_code"
                  "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
                  "precipitation_probability_max"
                  "&forecast_days=2&timezone=auto",
                  lat10 < 0 ? "-" : "", la / 10, la % 10,
                  lon10 < 0 ? "-" : "", lo / 10, lo % 10);
}

// WMO weather-interpretation code -> the same plain words the hub blob
// carries ("some clouds", not an icon id). Unknown codes degrade to "" —
// the faces already render an absent condition as nothing.
inline const char* wx_code_word(int code) {
  if (code == 0) return "clear";
  if (code == 1) return "mostly clear";
  if (code == 2) return "some clouds";
  if (code == 3) return "overcast";
  if (code == 45 || code == 48) return "fog";
  if (code >= 51 && code <= 57) return "drizzle";
  if (code >= 61 && code <= 67) return "rain";
  if (code >= 71 && code <= 77) return "snow";
  if (code >= 80 && code <= 82) return "showers";
  if (code == 85 || code == 86) return "snow showers";
  if (code >= 95 && code <= 99) return "thunderstorm";
  return "";
}

// Render the values as the retained hub-blob JSON bedside_on_weather
// parses. Temps are x10 ints (-10000 = absent), rain -1 = absent, conds ""
// = absent; ts is the wall clock the freshness window is judged against.
inline int wx_blob_build(char* out, size_t cap, int t_now_c10, int hi_c10,
                         int lo_c10, int rain_pct, const char* cond,
                         int hi2_c10, int lo2_c10, int rain2_pct,
                         const char* cond2, uint32_t ts) {
  size_t o = 0;
  // Clamped append: a truncating snprintf must never let `o` run past cap,
  // or the next call's `cap - o` underflows into a huge size.
  const auto app = [&](auto... args) {
    if (o + 1 >= cap) return;
    const int n = snprintf(out + o, cap - o, args...);
    if (n > 0) o = (o + (size_t)n < cap) ? o + (size_t)n : cap - 1;
  };
  const auto put10 = [&](const char* fmt, int v10) {
    if (v10 <= -10000) return;
    const int a = v10 < 0 ? -v10 : v10;
    app(fmt, v10 < 0 ? "-" : "", a / 10, a % 10);
  };
  app("%s", "{");
  put10("\"t_now\":%s%d.%d,", t_now_c10);
  put10("\"hi\":%s%d.%d,", hi_c10);
  put10("\"lo\":%s%d.%d,", lo_c10);
  if (rain_pct >= 0) app("\"rain\":%d,", rain_pct);
  if (cond && cond[0]) app("\"cond\":\"%s\",", cond);
  put10("\"hi2\":%s%d.%d,", hi2_c10);
  put10("\"lo2\":%s%d.%d,", lo2_c10);
  if (rain2_pct >= 0) app("\"rain2\":%d,", rain2_pct);
  if (cond2 && cond2[0]) app("\"cond2\":\"%s\",", cond2);
  app("\"ts\":%lu}", (unsigned long)ts);
  return (int)o;
}

}  // namespace canary::net
