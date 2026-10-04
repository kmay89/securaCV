#pragma once
#include <lvgl.h>

#include "round_frame_core.h"

// Round Frame, LVGL layer — fit-to-the-circle helpers for labels.
//
// One rule: a label near the rim gets the width its latitude honestly
// offers, and text that cannot fit ellipsizes (LV_LABEL_LONG_DOT) instead
// of running off the glass. On a rectangular panel the same calls degrade
// to "panel width minus side padding", so the shared modal surfaces
// (settings / onboarding / commissioning, which the nightstand renders
// through the watch branch) can call them unconditionally.
//
// Round is specifically the watch: in the shared TUs the nightstand aliases
// CD_FLAVOR_WATCH for the small-portrait renderer, so "watch and not
// nightstand" is the honest test for circular glass.
#if defined(CD_FLAVOR_WATCH) && !defined(CD_FLAVOR_NIGHTSTAND)
#define RF_GLASS_ROUND 1
#else
#define RF_GLASS_ROUND 0
#endif

namespace canary::ui {

// Widest honest width for a single-line label whose top lands at y_top with
// line height h: the band chord on round glass, panel width minus padding on
// rectangular glass.
int rf_row_width(int y_top, int h);

// Fit a single-line label into its latitude's band and align it. Each helper
// mirrors the lv_obj_align call it replaces — same anchor, same offset — so
// a face converts one line at a time:
//   lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y)    -> rf_fit_top(l, y)
//   lv_obj_align(l, LV_ALIGN_CENTER, 0, off)   -> rf_fit_center(l, off)
//   lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, o) -> rf_fit_bottom(l, o)
// The label is sized to (band width x line height), text-centered, and set
// to LV_LABEL_LONG_DOT: honest ellipsis, never a spill past the rim.
void rf_fit_top(lv_obj_t* label, int y_top);
void rf_fit_center(lv_obj_t* label, int y_center_off);
void rf_fit_bottom(lv_obj_t* label, int y_bottom_off);

}  // namespace canary::ui
