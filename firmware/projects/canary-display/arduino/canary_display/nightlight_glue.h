// include/canary/care/nightlight_glue.h — device glue for the nightlight:
// the shared visits instance plus the two prefs the pure model deliberately
// doesn't own (they are storage, not logic). Model: care/nightlight.h.
// Nightlight flavor only (CD_NIGHTLIGHT).
#pragma once
#include <stdint.h>
#include "nightlight.h"

namespace canary::care {

// Load prefs from NVS (seeds below on first boot). Called EARLY in setup —
// the persisted orientation must be known before the first face builds.
// The visits cadence is seeded separately (nightlight_visits().seed) from
// the same FNV-1a device-id hash the ambient-life layer uses.
void nightlight_begin();

NightlightVisits& nightlight_visits();

// ── Orientation (the auto-orient wave) ───────────────────────────────────
// The RENDERED rotation, 0..3 quarter turns CW (io/orientation.h Orient
// numbering = Arduino_GFX's). Persisted; main.cpp owns applying it to the
// panel + LVGL + face.
uint8_t nightlight_rotation();
void nightlight_set_rotation(uint8_t rot);

// Auto-orient: the IMU follows real movement (gravity-settled — see
// io/orientation.h). A MANUAL orientation choice — triple-press or the
// app — turns this off, the way touching a climate dial leaves AUTO;
// the app's toggle turns it back on.
bool nightlight_auto_rotate();
void nightlight_set_auto_rotate(bool on);

// Cross-context orientation request (the web settings handler runs inside
// glass_web_tick and must not rebuild the face itself): stash a rotation,
// main.cpp's loop takes it and walks the single apply path. Returns -1
// when nothing is pending.
void nightlight_request_rotation(uint8_t rot);
int nightlight_take_rotation_request();

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
