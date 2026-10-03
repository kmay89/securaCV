#pragma once
// Which face the 7"/dash glass wears: the portrait column (portrait7_ui) or
// the landscape poster (dash_ui / nightstand7_ui). main.cpp asks this to build
// the face, to route touch and updates to the live one, and to notice that a
// turn swapped them.
//
// It reads the turn the port WORE, not the saved setting. The two differ
// when lvgl_port_set_rotation() refuses a turn it cannot draw: on LVGL 9 a
// glass with no turn buffer (a PSRAM glass whose 128,000 B PSRAM request was
// refused, or a fallback glass whose 25,600 B request PSRAM and internal RAM
// both refused) keeps the canvas, LVGL and touch landscape (F205). The saved
// setting still says portrait then, and a face picked from it was the
// 480-wide column laid out on the 800x480 canvas (F223). Read from the port,
// the face is always the one the canvas is shaped for.
#include "canary/glass_settings.h"
#include "canary/ui/lvgl_port.h"

namespace canary::ui {

inline bool dash_face_portrait() {
  return canary::glass::rotation_is_portrait(lvgl_port_rotation());
}

}  // namespace canary::ui
