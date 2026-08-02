// include/canary/care/bedside.h — nightstand data lines (nightstand wave).
//
// The bedside display's three enrichments beyond the fleet itself:
//   - hub weather: HA republishes its forecast onto ONE retained topic
//     (securacv/env/weather) — the glass never touches the internet.
//   - bedroom comfort: the temp/humidity the fleet already publishes,
//     rendered as sleep-science words (comfort.h bands).
//   - sun times: computed on-device (suncalc.h) from CD_LAT/CD_LON.
// Each writer returns false when it has nothing honest to say — a stale
// forecast (>3 h) or an unset latitude simply doesn't render.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "canary/fleet/fleet_model.h"
#include "canary/fleet/fleet_instance.h"

namespace canary::care {

// Feed from the MQTT dispatcher (retained JSON from the hub):
//   {"cond":"partlycloudy","t_now":21.4,"hi":24.0,"lo":13.0,"rain":20,
//    "ts":1752854400}
// Optional fields the nightstand faces render when present (all degrade to
// absent silently — an older hub automation keeps working unchanged):
//   "hi2","lo2","rain2","cond2"  tomorrow's forecast (the night face's
//                                "what am I waking into" line)
//   "alert":{"event":"Wind Advisory","sev":1,"until":1752900000}
//                                an active weather advisory/warning; sev
//                                0 = advisory/watch, 1 = warning; `until`
//                                is the wall-clock end (epoch seconds)
void bedside_on_weather(const char* payload, unsigned len);

// Structured read of the fresh forecast (same 3-hour honesty window as the
// composed lines). Fields are absent as -10000 (temps ×10), -1 (rain), ""
// (cond word). Returns false when the cache is stale/empty.
struct BedsideWeather {
  int t_now_c10;                 // current outdoor temp ×10, -10000 if absent
  int hi_c10, lo_c10;            // today
  int rain_pct;                  // today, -1 if absent
  const char* cond;              // today's plain word ("some clouds"), "" unknown
  int hi2_c10, lo2_c10;          // tomorrow, -10000 if absent
  int rain2_pct;                 // tomorrow, -1 if absent
  const char* cond2;             // tomorrow's plain word, "" unknown
};
bool bedside_weather(BedsideWeather* out);

// "tomorrow 26°/14° · rain 10% · sunny" — false without tomorrow fields.
bool bedside_tomorrow_line(char* out, size_t cap);

// An active weather advisory/warning: true while `until` is ahead of the
// wall clock (and the blob is fresh). Visual-only by design — weather never
// chimes; the glass shows it, quiet hours stay quiet.
struct BedsideWxAlert {
  char text[48];                 // the hub's short event name, humanized case
  uint8_t sev;                   // 0 advisory/watch · 1 warning
};
bool bedside_weather_alert(BedsideWxAlert* out);

// "24°/13° · rain 20% · some clouds · sun up 5:37" (fresh forecast only;
// sun fragment only when CD_LAT/CD_LON are configured).
bool bedside_morning_line(char* out, size_t cap);

// "sun down 20:27" for the evening badge.
bool bedside_evening_line(char* out, size_t cap);

// "bedroom 18.5° just right" (+" · dry air" when humidity has a word).
// Picks the witness whose room matches CD_BEDSIDE_ROOM, else the first
// one publishing temperature.
bool bedside_comfort_line(const canary::fleet::Fleet& fleet, char* out,
                          size_t cap);

}  // namespace canary::care
