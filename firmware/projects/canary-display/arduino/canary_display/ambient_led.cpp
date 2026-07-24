// src/hal/ambient_led.cpp — the ambient state beacon (single WS2812 via RMT).
//
// See ambient_led.h for the honesty contract. The color itself now comes from
// the shared look engine (firmware/common/color): a gamma-true, gradient-
// capable scene when the fleet is calm, the true semantic color the instant
// anything needs attention, and black when safe at night. This TU is empty
// unless the flavor turns the feature on AND the board carries the LED, so it
// costs nothing on the watch/dash. On the wasm emulator the hardware write is
// skipped (no RMT peripheral in the browser).
#include "flavor_config.h"

#if defined(FEATURE_AMBIENT_LED) && FEATURE_AMBIENT_LED && \
    defined(HAS_RGBLED) && HAS_RGBLED

#include <Arduino.h>
#include "pins.h"
#include "ambient_led.h"
#include "look_state.h"
#include "color/look_engine.h"

namespace canary::hal {

using canary::fleet::Sev;

namespace {

bool s_ready = false;

void write_rgb(uint8_t r, uint8_t g, uint8_t b) {
#if !defined(__EMSCRIPTEN__)
  // Core built-in single-pixel RMT driver (esp32-hal-rgb-led): correct WS2812
  // timing on the single-core C6 and the dual-core S3 without a bit-banged
  // strobe (which would glitch under Wi-Fi on the C6).
  neopixelWrite(RGBLED_PIN, r, g, b);
#else
  (void)r; (void)g; (void)b;
#endif
}

}  // namespace

void ambient_led_init() {
  s_ready = true;
  write_rgb(0, 0, 0);  // dark until the first state is known
}

void ambient_led_off() {
  if (!s_ready) return;
  write_rgb(0, 0, 0);
}

void ambient_led_tick(uint32_t now_ms, Sev worst, bool night, bool safe_dark) {
  if (!s_ready) return;
  // The engine owns the whole look: scene palette + motion when calm, the
  // honest semantic override at Warn+, and black when safe_dark. Feed it the
  // live night flag; everything else is the user's chosen look.
  auto& lp = canary::ui::look_params();
  lp.night = night;
  const canary::color::Rgb c = canary::color::led_color(
      now_ms, lp, (canary::color::Sev)(uint8_t)worst, safe_dark);
  write_rgb(c.r, c.g, c.b);
}

}  // namespace canary::hal

#else  // feature/board absent — keep the symbols so callers link everywhere

#include "ambient_led.h"
namespace canary::hal {
void ambient_led_init() {}
void ambient_led_off() {}
void ambient_led_tick(uint32_t, canary::fleet::Sev, bool, bool) {}
}  // namespace canary::hal

#endif
