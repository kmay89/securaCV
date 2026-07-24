// src/hal/ambient_led.cpp — the ambient state beacon (single WS2812 via RMT).
//
// See ambient_led.h for the honesty contract. This TU is empty unless the
// flavor turns the feature on AND the board actually carries the LED, so it
// costs nothing on the watch/dash. On the wasm emulator (__EMSCRIPTEN__) the
// waveform math still runs but the hardware write is skipped — there is no
// RMT peripheral in the browser.
#include <config.h>

#if defined(FEATURE_AMBIENT_LED) && FEATURE_AMBIENT_LED && \
    defined(HAS_RGBLED) && HAS_RGBLED

#include <Arduino.h>
#include <math.h>
#include "pins.h"
#include "canary/hal/ambient_led.h"

namespace canary::hal {

using canary::fleet::Sev;

namespace {

bool s_ready = false;

// Peak brightness scalars (the WS2812 is fierce; a nightstand wants a calm
// point of light, not a flashlight). Day reads across a lit room; night is a
// faint attention ember that never fights dark-adapted eyes.
constexpr float DAY_PEAK   = 0.55f;
constexpr float NIGHT_PEAK = 0.14f;

struct Rgb { uint8_t r, g, b; };

// Severity -> beacon hue. Day uses the canonical timeline-card bytes (a state
// is the same color on the LED as on the glass as in the app). Night uses the
// red-shifted ember so the beacon never reintroduces a melatonin-band glow.
Rgb hue_for(Sev worst, bool night) {
  if (night) {
    // Only ever reached when NOT safe_dark (warn/alert/late/lost). One warm
    // ember; brightness (below) still separates a nudge from an alarm.
    return worst >= Sev::Alert ? Rgb{0x99, 0x22, 0x19}   // ncol_alert ember
                               : Rgb{0x5A, 0x1C, 0x12};   // dim red-shift
  }
  switch (worst) {
    case Sev::Warn:            return Rgb{0xFB, 0x8C, 0x00};  // amber — a look
    case Sev::Alert:
    case Sev::Tamper:          return Rgb{0xE5, 0x39, 0x35};  // red — needs you
    case Sev::Ok:
    case Sev::Notice:
    default:                   return Rgb{0x43, 0xA0, 0x47};  // green — all quiet
  }
}

// Breath half-period: arousal sets the pace, matching canary_mark's bob
// (calm 1.4 s, alert 1.1 s). A day-safe green breathes slow and easy.
uint32_t half_period_ms(Sev worst) {
  if (worst >= Sev::Alert) return 1100;
  if (worst >= Sev::Warn)  return 1250;
  return 1400;
}

// Triangle phase 0..1..0 with a cosine ease — the same shape LVGL's
// ease_in_out path draws, so the LED and the on-glass breath stay in sync.
float breath(uint32_t now, uint32_t half_ms) {
  const uint32_t period = half_ms * 2;
  const float p = (float)(now % period) / (float)period;   // 0..1
  const float tri = p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f;  // 0..1..0
  return 0.5f - 0.5f * cosf(tri * (float)M_PI);
}

void write_rgb(uint8_t r, uint8_t g, uint8_t b) {
#if !defined(__EMSCRIPTEN__)
  // Core built-in single-pixel RMT driver (esp32-hal-rgb-led). Handles the
  // WS2812 timing on both the single-core C6 and the dual-core S3 without a
  // bit-banged strobe (which would glitch under Wi-Fi on the C6).
  neopixelWrite(RGBLED_PIN, r, g, b);
#else
  (void)r; (void)g; (void)b;  // no RMT peripheral in the browser
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

  // The honest core: safe at night is DARKNESS, never a glow.
  if (safe_dark) { write_rgb(0, 0, 0); return; }

  const Rgb hue = hue_for(worst, night);
  const float peak = night ? NIGHT_PEAK : DAY_PEAK;

  // Breathe: the point of light inhales and exhales. Day-safe green sits at a
  // gentle floor (never fully off) so the beacon reads as "alive and well";
  // any attention state breathes deeper and faster.
  const float phase = breath(now_ms, half_period_ms(worst));
  const bool attention = worst >= Sev::Warn;
  const float floor = attention ? 0.35f : 0.55f;  // depth of the breath
  const float level = peak * (floor + (1.0f - floor) * phase);

  write_rgb((uint8_t)(hue.r * level),
            (uint8_t)(hue.g * level),
            (uint8_t)(hue.b * level));
}

}  // namespace canary::hal

#else  // feature/board absent — keep the symbols so callers link everywhere

#include "canary/hal/ambient_led.h"
namespace canary::hal {
void ambient_led_init() {}
void ambient_led_off() {}
void ambient_led_tick(uint32_t, canary::fleet::Sev, bool, bool) {}
}  // namespace canary::hal

#endif
