// include/canary/ui/canary_mark.h — the canary itself, on the glass.
//
// A small geometric canary in brand yellow: round body, round head, orange
// beak, dark eye that blinks, a wing a shade deeper. Pure LVGL objects (no
// image assets, no canvas), so it renders identically on both LVGL majors
// and costs a few hundred bytes.
//
// Life, rationed (Quiet Glass motion budget): the bird exists only in the
// sanctioned delight moments — onboarding and the no-canaries-yet faces.
// It blinks every few seconds, bobs gently while idle, and hops once when
// something good happens. It never appears during alerts or at night.
#pragma once
#include <lvgl.h>
#include <stdint.h>

namespace canary::ui {

enum class CanaryMood : uint8_t { Hidden, Idle, Happy };

// Builds the bird inside `parent` (size_px square). Recreating (e.g. on a
// new screen) is safe: the module tracks one live bird and cleans up its
// timers when the bird's screen is deleted.
lv_obj_t* canary_mark_create(lv_obj_t* parent, int size_px);

void canary_mark_mood(CanaryMood m);

}  // namespace canary::ui
