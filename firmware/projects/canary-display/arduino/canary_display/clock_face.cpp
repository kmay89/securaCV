// src/ui/clock_face.cpp — the drawn Analog dial. See the header.
#include "flavor_config.h"
#ifdef CD_FLAVOR_DASH

#include <lvgl.h>
#include <math.h>

#include "clock_face.h"
#include "motion.h"

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

// Paint both hands at the given x10 angles and remember what the dial
// shows. Angles normalize into one turn here — a sweep may aim past 360°
// to take the short way through twelve, but nothing stores or draws the
// unwrapped value (floats lose arc-precision as angles grow).
void paint_hands(AnalogClock* c, int32_t h10, int32_t m10) {
  h10 = ((h10 % 3600) + 3600) % 3600;
  m10 = ((m10 % 3600) + 3600) % 3600;
  const float r = (float)c->r;
  set_hand(c->hand_h, c->hpts, c->cx, c->cy, (float)h10 / 10.0f, r * 0.52f,
           r * 0.12f);
  set_hand(c->hand_m, c->mpts, c->cx, c->cy, (float)m10 / 10.0f, r * 0.78f,
           r * 0.14f);
  c->shown_h10 = h10;
  c->shown_m10 = m10;
}

// ── The minute sweep ─────────────────────────────────────────────────────
// One dial per face, so one sweep: the anim's var is the MINUTE HAND (LVGL
// reaps it with the object when a rebuild cleans the screen — the engine's
// var rule), and this pointer names the dial it belongs to. The exec guard
// checks both, so a stale tick against a rebuilt face is a no-op.
AnalogClock* s_sweep_c = nullptr;
int32_t s_sweep_from_h10 = 0, s_sweep_to_h10 = 0;
int32_t s_sweep_from_m10 = 0, s_sweep_to_m10 = 0;

void sweep_exec(void* var, int32_t v) {
  AnalogClock* c = s_sweep_c;
  if (!c || !c->hand_h || !c->hand_m || (lv_obj_t*)var != c->hand_m) return;
  paint_hands(c, motion::lerp1024(s_sweep_from_h10, s_sweep_to_h10, v),
              motion::lerp1024(s_sweep_from_m10, s_sweep_to_m10, v));
}

// Shortest signed x10-degree path a..b (354° -> 0° sweeps +6°, not -354°).
int32_t short_delta10(int32_t from10, int32_t to10) {
  int32_t d = (to10 - from10) % 3600;
  if (d < -1800) d += 3600;
  if (d > 1800) d -= 3600;
  return d;
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
    c->shown_h10 = c->shown_m10 = -1;   // time's return lands, not sweeps
    return;
  }
  lv_obj_clear_flag(c->hand_h, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(c->hand_m, LV_OBJ_FLAG_HIDDEN);
  const int32_t to_h10 = (int32_t)((hh % 12) * 300) + mm * 5;
  const int32_t to_m10 = (int32_t)mm * 60;
  const uint32_t dur = motion::ms(motion::Dur::Medium);
  if (c->shown_m10 < 0 || dur == 0 || !motion::allowed(motion::Fx::Micro)) {
    // First paint, a returning clock, or a tier/state that says snap.
    paint_hands(c, to_h10, to_m10);
  } else if (to_m10 != c->shown_m10 || to_h10 != c->shown_h10) {
    // A render tick lands mid-sweep on purpose (the model refreshes faster
    // than a minute) — if the flight is already aimed at this time, let it
    // fly rather than restarting from wherever the hands are.
    if (s_sweep_c == c && lv_anim_get(c->hand_m, sweep_exec) &&
        ((s_sweep_to_m10 % 3600) + 3600) % 3600 == to_m10 &&
        ((s_sweep_to_h10 % 3600) + 3600) % 3600 == to_h10) {
      lv_obj_set_style_line_color(c->hand_h, col, 0);
      lv_obj_set_style_line_color(c->hand_m, col, 0);
      return;
    }
    // The minute tick, eased: both hands glide the short way from what the
    // dial shows to the new time. Re-aim any sweep already in flight.
    s_sweep_c = c;
    s_sweep_from_h10 = c->shown_h10;
    s_sweep_to_h10 = c->shown_h10 + short_delta10(c->shown_h10, to_h10);
    s_sweep_from_m10 = c->shown_m10;
    s_sweep_to_m10 = c->shown_m10 + short_delta10(c->shown_m10, to_m10);
    lv_anim_del(c->hand_m, sweep_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, c->hand_m);
    lv_anim_set_exec_cb(&a, sweep_exec);
    lv_anim_set_values(&a, 0, motion::kMotionOne);
    lv_anim_set_time(&a, dur);
    lv_anim_set_path_cb(&a, motion::path_out_cubic);
    lv_anim_start(&a);
  }
  lv_obj_set_style_line_color(c->hand_h, col, 0);
  lv_obj_set_style_line_color(c->hand_m, col, 0);
}

}  // namespace canary::ui

#endif  // CD_FLAVOR_DASH
