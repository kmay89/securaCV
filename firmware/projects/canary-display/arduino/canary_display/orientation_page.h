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
// They also differ, for up to a second, with nothing refused: /api/set only
// stores the choice, and main.cpp's render hands it to the port on its next
// pass. So a refusal is what the PORT says (lvgl_port_turn_refused(): the
// last turn asked had no turn buffer), never the difference alone; a turn
// not yet asked for keeps the page's own footer.
//
// The saved choice is kept, not overwritten with the turn worn: a request
// refused at one start (PSRAM that said no once) does not lose the owner's
// setting, and a later start with the memory wears it. What the glass SHOWS
// follows the turn worn: the Orientation page checks the row the glass is
// wearing, the root row names it, and when the port refused the saved
// choice the page's footer says so and names both. The settings GET keeps
// `orientation` (the saved choice, so the app's contract is unchanged) and
// adds `orientation_worn` and `orientation_refused`.
//
// Pure: no LVGL, no Arduino. Callers hand in settings().rotation,
// lvgl_port_rotation() and lvgl_port_turn_refused(). Host-tested in
// tests_host/test_display_settings.cpp, and against the real lvgl_port.cpp
// with its turn buffer granted and refused in
// tests_host/test_lvgl_port_turn.cpp. The source pins in
// canary-local/tests/dash_orientation.test.js hold settings_ui.cpp and
// glass_web.cpp to it. The page's words live here, so
// firmware/scripts/microcopy_lint.sh reads this file with the glass sources.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "glass_settings.h"  // Rotation, rotation_name

namespace canary::ui {

struct OrientationState {
  uint8_t saved;      // the owner's choice (Settings.rotation), 0..3
  uint8_t worn;       // the turn the glass wears (lvgl_port_rotation()), 0..3
  bool port_refused;  // the port refused the last turn (lvgl_port_turn_refused())
  // The saved choice could not be worn: the port refused it. A saved turn
  // the port has not been asked for yet differs from the turn worn too, and
  // is not a refusal.
  bool refused() const { return port_refused && saved != worn; }
};

inline OrientationState orientation_state(uint8_t saved, uint8_t worn,
                                          bool port_refused) {
  OrientationState s;
  s.saved = (uint8_t)(saved & 3);
  s.worn = (uint8_t)(worn & 3);
  s.port_refused = port_refused;
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

// Room for the root row's value: the longest rotation_name ("landscape
// flipped", 17 characters) and its NUL, with room to spare.
constexpr size_t kOrientationValueCap = 24;

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

// The footer under the four rows. Not refused: what a turn is for. Refused:
// that the glass could not turn, the turn it wears, and that a restart tries
// the saved choice again (the turn buffer is asked for at bring-up only, so
// tapping the row again cannot). The saved label ends the sentence, after a
// colon, so the comma in "Portrait, Flipped" reads as the label's own.
// Returns buf.
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
             "A restart tries the saved turn again: %s.",
             orientation_label(s.worn), orientation_label(s.saved));
  }
  return buf;
}

}  // namespace canary::ui
