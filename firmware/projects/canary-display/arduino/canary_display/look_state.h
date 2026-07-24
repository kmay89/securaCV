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

namespace canary::ui {

// The live, mutable look. Callers set `.night` from the render tick's quiet-
// hours flag before reading a color.
canary::color::LookParams& look_params();

// Select / advance the scene (settings, MQTT, or a future BOOT-button demo).
void look_set_scene(uint8_t idx);   // clamped to the scene count
void look_cycle_scene();            // next scene, wrapping

}  // namespace canary::ui
