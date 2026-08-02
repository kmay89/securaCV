#pragma once
#include <config.h>   // CD_FLAVOR_* selectors
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

// Poll the touch controller (cheap; called every loop pass). On the RGB dash
// glass the returned x/y are already un-rotated into the current logical
// frame (see touch_set_rotation), so callers never orientation-correct taps.
TouchSample touch_read();

#ifdef CD_FLAVOR_DASH
// Tell the touch layer the active display rotation (canary::glass::Rotation)
// and the panel's native landscape size, so touch_read() maps raw GT911
// coordinates back into the rotated logical frame the UI drew in. Called by
// lvgl_port_set_rotation whenever orientation changes. Landscape (0) is the
// identity, so this only matters once a user turns the glass.
void touch_set_rotation(uint8_t rot, int16_t native_w, int16_t native_h);
#endif

#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
// ── Isolated IO (Waveshare 4.3B terminal block, via the CH422G) ─────────
// Provided by display_dash.cpp only when the board pin map declares
// HAS_ISOLATED_IO (waveshare-esp32s3-lcd43b). The dev playground is the
// only consumer today (docs/hardware/dev_playground_43b.md).

// Read EXIO0..7 as a raw bit snapshot into *value (mask with
// ISO_IN_BIT_*). Returns false — leaving *value untouched — on any I2C
// failure or short read, so a bus glitch can never masquerade as an
// active-low input edge (review catch). The CH422G's EXIO direction is
// global, so this briefly flips the expander to input mode and restores
// the output latch afterwards — call at a bounded rate (the playground
// polls 50 Hz) and VERIFY no backlight flicker at the bench.
bool expander_read_inputs(uint8_t* value);

// Drive an isolated open-drain output (mask with ISO_OUT_BIT_*).
// sink=true conducts (pulls the DO terminal low through the optocoupler),
// sink=false releases it. Latch polarity carries a VERIFY note in pins.h.
bool expander_od_set(uint8_t mask, bool sink);

// Last written OD latch (bit HIGH = released) — playground UI truth.
uint8_t expander_od_state();
#endif  // HAS_ISOLATED_IO

}  // namespace canary::hal
