// src/ui/look_state.cpp — the shared Nightstand look controls. See look_state.h.
// Nightstand flavors only (the 1.47"/1.69" portrait boards and the 7" bedside
// glass); empty elsewhere so the watch/dash builds never pull in the color
// engine (and their size guard is untouched).
#include "flavor_config.h"
#if defined(CD_FLAVOR_NIGHTSTAND) || defined(CD_NIGHTSTAND7)

#include "look_state.h"

namespace canary::ui {

namespace {
canary::color::LookParams s_look;
canary::color::Plumage s_song;
LampFrame s_lamp;
}  // namespace

canary::color::LookParams& look_params() { return s_look; }

canary::color::Plumage& song() { return s_song; }

void song_seed(uint32_t seed, uint32_t now_ms) { s_song.begin(seed, now_ms); }

LampFrame& lamp_frame() { return s_lamp; }

void look_set_scene(uint8_t idx) {
  if (canary::color::kSceneCount)
    s_look.scene_idx = (uint8_t)(idx % canary::color::kSceneCount);
  // Choosing a catalog scene puts the wheel away — one look is on at a time,
  // and the settings screen should never show two answers to "what color?".
  s_look.custom_hue = -1;
}

void look_set_custom_hue(int16_t hue) {
  s_look.custom_hue = hue < 0 ? -1 : (int16_t)(hue % 360);
}

void look_cycle_scene() {
  if (canary::color::kSceneCount)
    s_look.scene_idx = (uint8_t)((s_look.scene_idx + 1) % canary::color::kSceneCount);
}

}  // namespace canary::ui

#endif  // CD_FLAVOR_NIGHTSTAND
