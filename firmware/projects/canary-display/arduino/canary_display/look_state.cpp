// src/ui/look_state.cpp — the shared Nightstand look controls. See look_state.h.
// Nightstand flavor only; empty elsewhere so the watch/dash builds never pull
// in the color engine (and their size guard is untouched).
#include "flavor_config.h"
#ifdef CD_FLAVOR_NIGHTSTAND

#include "look_state.h"

namespace canary::ui {

namespace { canary::color::LookParams s_look; }

canary::color::LookParams& look_params() { return s_look; }

void look_set_scene(uint8_t idx) {
  if (canary::color::kSceneCount)
    s_look.scene_idx = (uint8_t)(idx % canary::color::kSceneCount);
}

void look_cycle_scene() {
  if (canary::color::kSceneCount)
    s_look.scene_idx = (uint8_t)((s_look.scene_idx + 1) % canary::color::kSceneCount);
}

}  // namespace canary::ui

#endif  // CD_FLAVOR_NIGHTSTAND
