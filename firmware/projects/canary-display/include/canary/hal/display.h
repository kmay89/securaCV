#pragma once
#include <stdint.h>

class Arduino_GFX;  // moononournation GFX — the one graphics type the UI sees

// Panel + touch HAL. Exactly one of src/hal/display_watch.cpp (GC9A01 SPI +
// CST816S, gated CD_FLAVOR_WATCH) or src/hal/display_dash.cpp (S3 RGB LCD +
// GT911 + CH422G, gated CD_FLAVOR_DASH) provides these. The UI layer draws
// through Arduino_GFX primitives and never touches panel specifics — that
// containment is what lets one app ship on two very different pieces of
// glass.

namespace canary::hal {

// Bring up panel (+ touch controller). False = glass unusable; the app
// keeps running headless (MQTT still works) so the fault is diagnosable.
bool display_init();

// Draw target: the bare panel on both flavors. LVGL owns buffering and
// dirty-region rendering (ui/lvgl_port.cpp); flushes arrive through
// draw16bitRGBBitmap.
Arduino_GFX* gfx();

// Kept for interface stability; a no-op on both flavors now that LVGL
// flushes dirty regions itself.
void display_flush();

// 0..255. Watch has real PWM dimming; dash hardware is on/off only (CH422G
// expander), so any level > 0 = on. Callers treat levels as intent — the
// HAL maps them onto what the glass can actually do.
void backlight_set(uint8_t level);

// Night-profile backlight: 1 kHz / 13-bit PWM whose bottom steps subdivide
// what the day profile calls "duty 1" — the resolution the calibrated night
// floor needs (settings wave). Watch swaps the LEDC profile on the fly and
// swaps back on the next backlight_set(); dash renders any duty > 0 as on.
// Duty range 0..8191 (canary::glass::NIGHT_DUTY_MAX).
void backlight_night_set(uint16_t duty13);

struct TouchSample {
  bool touched = false;
  int16_t x = 0;
  int16_t y = 0;
};

// Poll the touch controller (cheap; called every loop pass).
TouchSample touch_read();

}  // namespace canary::hal
