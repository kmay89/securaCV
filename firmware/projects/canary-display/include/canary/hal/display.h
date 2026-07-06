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

// Draw target. Watch: an offscreen 240x240 canvas (flushed per frame, no
// tearing). Dash: the RGB framebuffer directly.
Arduino_GFX* gfx();

// Push the frame (canvas flush on watch; no-op on dash where the RGB
// peripheral scans the framebuffer continuously).
void display_flush();

// 0..255. Watch has real PWM dimming; dash hardware is on/off only (CH422G
// expander), so any level > 0 = on. Callers treat levels as intent — the
// HAL maps them onto what the glass can actually do.
void backlight_set(uint8_t level);

struct TouchSample {
  bool touched = false;
  int16_t x = 0;
  int16_t y = 0;
};

// Poll the touch controller (cheap; called every loop pass).
TouchSample touch_read();

}  // namespace canary::hal
