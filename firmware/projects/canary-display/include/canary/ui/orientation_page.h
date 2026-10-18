// include/canary/ui/orientation_page.h — what the dash glass says about its
// turn (F245): Settings > Orientation, the root row, and the settings GET.
//
// Two rotations, and they can differ. The SAVED one is the owner's choice
// (Settings.rotation, kept in NVS; the on-glass editor and the phone's
// /api/set write it). The WORN one is what lvgl_port_set_rotation() did
// (lvgl_port_rotation()): on LVGL 9 a glass with no turn buffer refuses a
// turn and stays landscape (F205, canary/ui/lvgl_port.h), and the face
// follows the worn turn (F223, canary/ui/dash_face.h). The turn buffer is
// asked for once, at bring-up, so a refusal holds until the next start.
//
// The saved choice is kept, not overwritten with the turn worn: a request
// refused at one start (PSRAM that said no once) does not lose the owner's
// setting, and a later start with the memory wears it. What the glass SHOWS
// follows the turn worn: the Orientation page checks the row the glass is
// wearing, the root row names it, and when the saved choice could not be
// worn the page's footer says so and names both. The settings GET keeps
// `orientation` (the saved choice, so the app's contract is unchanged) and
// adds `orientation_worn`.
//
// Pure: no LVGL, no Arduino. Callers hand in settings().rotation and
// lvgl_port_rotation(). Host-tested in tests_host/test_display_settings.cpp,
// and against the real lvgl_port.cpp with its turn buffer refused in
// tests_host/test_lvgl_port_turn.cpp. The source pins in
// canary-local/tests/dash_orientation.test.js hold settings_ui.cpp and
// glass_web.cpp to it.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "canary/glass_settings.h"  // Rotation, rotation_name

namespace canary::ui {

struct OrientationState {
  uint8_t saved;  // the owner's choice (Settings.rotation), 0..3
  uint8_t worn;   // the turn the glass wears (lvgl_port_rotation()), 0..3
  // The saved choice could not be worn: the port refused the turn.
  bool refused() const { return saved != worn; }
};

inline OrientationState orientation_state(uint8_t saved, uint8_t worn) {
  OrientationState s;
  s.saved = (uint8_t)(saved & 3);
  s.worn = (uint8_t)(worn & 3);
  return s;
}

// The page's four rows, top to bottom.
constexpr uint8_t kOrientationRows[4] = {
    canary::glass::ROT_LANDSCAPE, canary::glass::ROT_PORTRAIT,
    canary::glass::ROT_LANDSCAPE_INV, canary::glass::ROT_PORTRAIT_INV};

// A row's label, as the page draws it.
inline const char* orientation_label(uint8_t rot) {
  switch (rot & 3) {
    case canary::glass::ROT_PORTRAIT:      return "Portrait";
    case canary::glass::ROT_LANDSCAPE_INV: return "Landscape, Flipped";
    case canary::glass::ROT_PORTRAIT_INV:  return "Portrait, Flipped";
    default:                               return "Landscape";
  }
}

// The check sits on the turn the glass wears, never on a saved turn it could
// not wear.
inline bool orientation_row_checked(const OrientationState& s, uint8_t rot) {
  return s.worn == (uint8_t)(rot & 3);
}

// The root row's value: the turn worn, named as the root row always named it
// (rotation_name, its first letter capitalized). Returns buf.
inline const char* orientation_row_value(const OrientationState& s, char* buf,
                                         size_t n) {
  if (!buf || n == 0) return "";
  snprintf(buf, n, "%s", canary::glass::rotation_name(s.worn));
  if (buf[0] >= 'a' && buf[0] <= 'z') buf[0] = (char)(buf[0] - 'a' + 'A');
  return buf;
}

// Room for the longest footer (both labels "Landscape, Flipped").
constexpr size_t kOrientationFooterCap = 160;

// The footer under the four rows. Worn as saved: what a turn is for. Refused:
// that the glass could not turn, the turn it wears, the choice it kept, and
// what tries again (a restart: the turn buffer is asked for at bring-up
// only, so tapping the row again cannot). Returns buf.
inline const char* orientation_footer(const OrientationState& s, char* buf,
                                      size_t n) {
  if (!buf || n == 0) return "";
  if (!s.refused()) {
    snprintf(buf, n, "%s",
             "Portrait suits a wall column or a tall bedside face. "
             "The layout follows the turn.");
  } else {
    snprintf(buf, n,
             "Not enough memory to turn this glass, so it stays %s. "
             "%s is saved, and a restart tries the turn again.",
             orientation_label(s.worn), orientation_label(s.saved));
  }
  return buf;
}

}  // namespace canary::ui
