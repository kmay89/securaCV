// include/canary/care/nightlight_glue.h — device glue for the nightlight:
// the shared visits instance plus the two prefs the pure model deliberately
// doesn't own (they are storage, not logic). Model: care/nightlight.h.
// Nightlight flavor only (CD_NIGHTLIGHT).
#pragma once
#include <stdint.h>
#include "canary/care/nightlight.h"

namespace canary::care {

// Load prefs from NVS (seeds below on first boot) and seed the visits
// cadence. `seed` is the same FNV-1a device-id hash main.cpp feeds the
// ambient-life layer: distinct across devices, reproducible within one.
void nightlight_begin(uint32_t seed);

NightlightVisits& nightlight_visits();

// 12-hour clock (the shipped default — a bedside kid's clock says 8:15,
// not 20:15). The app and the settings surface own it at runtime.
bool nightlight_clock_12h();
void nightlight_set_clock_12h(bool on);

// The lamp's drawn strength (LookParams.brightness, 0..255). This scales
// the COLORS the field renders; the backlight's 50% duty ceiling
// (CD_BL_MAX_PCT, enforced in the HAL) is a separate, harder wall that no
// value here can climb over.
uint8_t nightlight_lamp_bri();
void nightlight_set_lamp_bri(uint8_t bri);

}  // namespace canary::care
