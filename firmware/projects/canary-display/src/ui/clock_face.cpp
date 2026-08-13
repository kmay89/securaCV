// src/ui/clock_face.cpp — the drawn Analog dial. See the header.
#include <config.h>
#ifdef CD_FLAVOR_DASH

#include <lvgl.h>
#include <math.h>

#include "canary/ui/clock_face.h"

namespace canary::ui {

namespace {

constexpr float kPi = 3.14159265f;

// Clock angle (0 = twelve, clockwise) -> parent coordinates on a circle.
void on_circle(int cx, int cy, float deg, float radius, int* x, int* y) {
  const float a = deg * kPi / 180.0f;
  *x = cx + (int)lroundf(sinf(a) * radius);
  *y = cy - (int)lroundf(cosf(a) * radius);
}

lv_obj_t* mk_dot(lv_obj_t* parent, int x, int y, int d) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_set_size(o, d, d);
  lv_obj_set_pos(o, x - d / 2, y - d / 2);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

lv_obj_t* mk_hand(lv_obj_t* parent, int width) {
  lv_obj_t* l = lv_line_create(parent);
  lv_obj_set_style_line_width(l, width, 0);
  lv_obj_set_style_line_rounded(l, true, 0);
  return l;
}

void set_hand(lv_obj_t* hand, ClockLinePt* pts, int cx, int cy, float deg,
              float len, float tail) {
  int tip_x, tip_y, tail_x, tail_y;
  on_circle(cx, cy, deg, len, &tip_x, &tip_y);
  on_circle(cx, cy, deg + 180.0f, tail, &tail_x, &tail_y);
  pts[0].x = (decltype(pts[0].x))tail_x;
  pts[0].y = (decltype(pts[0].y))tail_y;
  pts[1].x = (decltype(pts[1].x))tip_x;
  pts[1].y = (decltype(pts[1].y))tip_y;
  lv_line_set_points(hand, pts, 2);
}

}  // namespace

void analog_clock_build(AnalogClock* c, lv_obj_t* parent, int cx, int cy,
                        int r) {
  c->cx = cx;
  c->cy = cy;
  c->r = r;

  c->dial = lv_obj_create(parent);
  lv_obj_set_size(c->dial, 2 * r, 2 * r);
  lv_obj_set_pos(c->dial, cx - r, cy - r);
  lv_obj_set_style_radius(c->dial, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(c->dial, LV_OPA_0, 0);
  lv_obj_set_style_border_width(c->dial, 2, 0);
  lv_obj_set_style_pad_all(c->dial, 0, 0);
  lv_obj_clear_flag(c->dial, LV_OBJ_FLAG_SCROLLABLE);

  for (int i = 0; i < 12; i++) {
    const bool cardinal = (i % 3) == 0;
    int x, y;
    on_circle(cx, cy, (float)(i * 30), (float)r - (cardinal ? 12 : 10), &x, &y);
    c->marks[i] = mk_dot(parent, x, y, cardinal ? 10 : 6);
  }

  // Minute under hour? No — hour under minute, hub on top: the shorter,
  // thicker hour hand reads first, the minute sweeps over it.
  c->hand_h = mk_hand(parent, r / 9 > 6 ? r / 9 : 6);
  c->hand_m = mk_hand(parent, r / 14 > 4 ? r / 14 : 4);
  c->hub = mk_dot(parent, cx, cy, r / 8 > 10 ? r / 8 : 10);
}

void analog_clock_update(AnalogClock* c, int hh, int mm, lv_color_t col,
                         lv_color_t muted, bool valid) {
  if (!c->dial) return;
  lv_obj_set_style_border_color(c->dial, muted, 0);
  for (int i = 0; i < 12; i++) {
    if (!c->marks[i]) continue;
    lv_obj_set_style_bg_color(c->marks[i], (i % 3) == 0 ? col : muted, 0);
    lv_obj_set_style_bg_opa(c->marks[i], (i % 3) == 0 ? LV_OPA_COVER
                                                      : LV_OPA_60, 0);
  }
  if (c->hub) {
    lv_obj_set_style_bg_color(c->hub, col, 0);
    lv_obj_set_style_bg_opa(c->hub, LV_OPA_COVER, 0);
  }
  if (!c->hand_h || !c->hand_m) return;
  if (!valid) {
    lv_obj_add_flag(c->hand_h, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(c->hand_m, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(c->hand_h, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(c->hand_m, LV_OBJ_FLAG_HIDDEN);
  const float r = (float)c->r;
  const float hour_deg = (float)((hh % 12) * 30) + (float)mm * 0.5f;
  const float min_deg = (float)(mm * 6);
  set_hand(c->hand_h, c->hpts, c->cx, c->cy, hour_deg, r * 0.52f, r * 0.12f);
  set_hand(c->hand_m, c->mpts, c->cx, c->cy, min_deg, r * 0.78f, r * 0.14f);
  lv_obj_set_style_line_color(c->hand_h, col, 0);
  lv_obj_set_style_line_color(c->hand_m, col, 0);
}

}  // namespace canary::ui

#endif  // CD_FLAVOR_DASH
