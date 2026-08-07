// include/canary/ui/look_state.h — the Nightstand's current "look" controls.
//
// One shared LookParams (scene, brightness, warmth, gamma, motion) read by
// BOTH ambient channels — the WS2812 beacon (hal/ambient_led.cpp) and the
// glass wash (ui/portrait_ui.cpp) — so the point of light and the pane always
// agree on the look. The settings surface and an MQTT topic will own these at
// runtime; today they default to the Canary Dawn scene. Nightstand flavor
// only (the color engine lives in firmware/common/color).
#pragma once
#include "color/look_engine.h"
#include "color/plumage.h"

namespace canary::ui {

// The live, mutable look. Callers set `.night` from the render tick's quiet-
// hours flag before reading a color.
canary::color::LookParams& look_params();

// The lamp's VOICE (color/plumage.h) — shared for exactly the same reason
// LookParams is: the glass and the beacon must be saying the same thing at
// the same instant, and two Plumage instances ticking independently would
// drift into two different birds in one device.
//
// Only ever ticked while the lamp is genuinely lit; see the gating in
// portrait_ui.cpp. The beacon reads it without ticking it.
canary::color::Plumage& song();

// Seed the voice from the device identity: distinct across devices (two
// Canaries in one hallway must not speak in lockstep), reproducible within
// one (which is what makes the layer host-testable). main.cpp passes the same
// FNV-1a hash of the NVS device id it seeds the ambient-life cadence with.
void song_seed(uint32_t seed, uint32_t now_ms);

// The lamp's live render state for this frame, published by the glass
// (portrait_ui.cpp, which owns the lantern/Hallway decision) and read by the
// beacon (hal/ambient_led.cpp).
//
// The beacon cannot work this out for itself — whether the lamp is lit
// depends on the lantern's timeout, the auto schedule and the attention veto,
// all of which the glass has already resolved. Publishing the answer rather
// than recomputing it is what stops the two channels from disagreeing about
// whether the light is on.
struct LampFrame {
  bool lit = false;      // the lamp is showing on the glass right now
  bool beacon = false;   // ...and Hallway mode says the LED may join it
  uint8_t depth = 0;     // plumage swell depth, already dwell-scaled
  canary::color::LookParams look;
};

LampFrame& lamp_frame();

// Select / advance the scene (settings, MQTT, or a future BOOT-button demo).
void look_set_scene(uint8_t idx);   // clamped to the scene count
void look_cycle_scene();            // next scene, wrapping

// The owner turned the color wheel. `hue` is 0..359; anything negative hands
// the glass back to `scene_idx`. Picking a catalog scene clears it, so the
// two controls can never both claim to be the current look.
void look_set_custom_hue(int16_t hue);

}  // namespace canary::ui
