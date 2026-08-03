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
#include "pins.h"  // MUST precede the gate: HAS_RGBLED lives here, not in
                   // config.h. With the include below the gate (as first
                   // shipped) HAS_RGBLED was undefined at gate time and this
                   // TU compiled EMPTY on the nightstand boards — the beacon
                   // was dead code. Same contract as settings_ui.cpp's
                   // HAS_ISOLATED_IO: board header first, then the gate.

#if defined(FEATURE_AMBIENT_LED) && FEATURE_AMBIENT_LED && \
    defined(HAS_RGBLED) && HAS_RGBLED

#include <Arduino.h>
#include "ambient_led.h"
#include "look_state.h"
#include "color/look_engine.h"
#include "color/plumage.h"

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
  // Boot beacon: bright canary yellow (0xFFD44F — the splash scanline's
  // brand yellow) from the first instant of power, so the bird visibly wakes
  // before the glass does. This is a liveness signal, not a state claim —
  // the first ambient_led_tick() repaints with the real fleet state (or
  // honest darkness) as soon as one is known, so the yellow lives exactly
  // as long as boot does.
  write_rgb(0xFF, 0xD4, 0x4F);
}

void ambient_led_off() {
  if (!s_ready) return;
  write_rgb(0, 0, 0);
}

void ambient_led_tick(uint32_t now_ms, Sev worst, bool night, bool safe_dark) {
  if (!s_ready) return;

  // ── The lamp, when Hallway mode has invited the LED to join it ──
  // The glass publishes the lamp's frame (ui/look_state.h LampFrame) because
  // only the glass knows whether the lantern is actually lit — the timeout,
  // the auto schedule and the attention veto all resolve there. Reading that
  // answer rather than recomputing it is what keeps the point of light and
  // the pane from disagreeing about whether the light is on.
  //
  // This is the ONE path on which the beacon is not a pure attention channel,
  // and it is reachable only when: Hallway mode is on, its beacon opt-in is
  // set, and the lamp is currently lit — which itself already means the fleet
  // is calm and the links are healthy, because attention extinguishes the
  // lamp before this can be true. The reasoning is written out in full in
  // care/hallway.h; the short version is that the dark-means-safe inference
  // was already spent by turning lantern hours on, and a glow here cannot add
  // a claim the lit glass beside it is not already making.
  const auto& lamp = canary::ui::lamp_frame();
  if (lamp.lit && lamp.beacon && (uint8_t)worst < (uint8_t)Sev::Warn &&
      !safe_dark) {
    // Read the shared song WITHOUT ticking it — the glass owns the clock, so
    // the beacon is always mid-way through the same syllable the pane shows.
    const canary::color::Rgb c = canary::color::plumage_led(
        now_ms, lamp.look, canary::color::Sev::Ok, /*safe_dark=*/false,
        canary::ui::song(), lamp.depth);
    write_rgb(c.r, c.g, c.b);
    return;
  }

  // ── The honest state channel, unchanged ──
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
