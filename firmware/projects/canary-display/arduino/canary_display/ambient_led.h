// include/canary/hal/ambient_led.h — the ambient state beacon.
//
// The Nightstand boards carry ONE onboard WS2812-class addressable RGB LED
// (RGBLED_PIN, driven via RMT). It is the device's PRIMARY across-the-room
// state channel: a single point of color you can read from bed without the
// screen lighting the room. It breathes with the living canary and is
// colored by the WORST active severity in the fleet.
//
// The honesty invariant holds on the LED exactly as on the glass: silence is
// never rendered as safety. At night, an all-quiet fleet with healthy links
// goes DARK — the beacon is a pure attention signal, so dark = sleep easy and
// any glow = look. The separate user-summoned warm night-light (a follow-up,
// BOOT-button driven) is what a nightlight-seeker turns on; it never routes
// through this channel and so never says "safe" by glowing.
//
// Compiled only where FEATURE_AMBIENT_LED is set (the nightstand flavor); a
// no-op everywhere else, and on the wasm emulator (no RMT peripheral).
#pragma once
#include <stdint.h>
#include "fleet_model.h"

namespace canary::hal {

// Bring up the RMT-driven single-pixel strand and light the boot beacon:
// bright canary yellow until the first tick knows a real state. A wake-up
// liveness signal, not a severity claim — the honesty invariant governs the
// steady state, and the first ambient_led_tick() takes over. Safe to call
// even where the feature/board is absent (compiles to nothing).
void ambient_led_init();

// Drive the beacon for this frame. Called every loop pass (cheap); it owns
// the breath waveform internally so the point of light inhales and exhales in
// step with the bird.
//   worst     — the fleet's worst active severity (fleet.worst(now))
//   night     — quiet hours in effect
//   safe_dark — night AND all-quiet AND links healthy: the LED goes fully
//               dark (the honest "safe is darkness, never a green glow")
void ambient_led_tick(uint32_t now_ms, canary::fleet::Sev worst,
                      bool night, bool safe_dark);

// Force the beacon dark (boot, headless, shutdown).
void ambient_led_off();

}  // namespace canary::hal
