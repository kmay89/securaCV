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
void bedside_on_weather(const char* payload, unsigned len);

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
