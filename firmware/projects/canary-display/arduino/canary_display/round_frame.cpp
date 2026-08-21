// src/ui/round_frame.cpp — the circle's LVGL glue (see round_frame_core.h
// for the geometry and the why).
#include "flavor_config.h"
#include <lvgl.h>

#include "round_frame.h"

namespace canary::ui {

namespace {

// Side padding a rectangular panel keeps when these helpers run there (the
// nightstand renders the shared modal surfaces through the watch branch).
constexpr int kRectSidePad = 8;

int panel_w() { return (int)lv_disp_get_hor_res(NULL); }
int panel_h() { return (int)lv_disp_get_ver_res(NULL); }

int label_line_h(lv_obj_t* label) {
  const lv_font_t* f = lv_obj_get_style_text_font(label, LV_PART_MAIN);
  return f ? (int)lv_font_get_line_height(f) : 16;
}

// Size + ellipsis + center, shared by every anchor. Width never goes below
// one glyph — a zero-width label at an impossible latitude would vanish
// silently, and a sliver with dots at least admits something is there.
void fit(lv_obj_t* label, int y_top, int h) {
  int w;
#if RF_GLASS_ROUND
  w = roundframe::chord(y_top, h);
#else
  (void)y_top;
  w = panel_w() - 2 * kRectSidePad;
#endif
  const int min_w = h;  // ~one glyph
  if (w < min_w) w = min_w;
  lv_obj_set_size(label, w, h);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

}  // namespace

int rf_row_width(int y_top, int h) {
#if RF_GLASS_ROUND
  return roundframe::chord(y_top, h);
#else
  (void)y_top;
  (void)h;
  return panel_w() - 2 * kRectSidePad;
#endif
}

void rf_fit_top(lv_obj_t* label, int y_top) {
  const int h = label_line_h(label);
  fit(label, y_top, h);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y_top);
}

void rf_fit_center(lv_obj_t* label, int y_center_off) {
  const int h = label_line_h(label);
  const int y_top = panel_h() / 2 + y_center_off - h / 2;
  fit(label, y_top, h);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, y_center_off);
}

void rf_fit_bottom(lv_obj_t* label, int y_bottom_off) {
  const int h = label_line_h(label);
  const int y_top = panel_h() + y_bottom_off - h;
  fit(label, y_top, h);
  lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, y_bottom_off);
}

}  // namespace canary::ui
