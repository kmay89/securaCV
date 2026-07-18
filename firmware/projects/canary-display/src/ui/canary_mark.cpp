// src/ui/canary_mark.cpp — the brand canary widget. See canary_mark.h.
#include <config.h>
#include <Arduino.h>
#include <lvgl.h>

#include "canary/ui/canary_mark.h"
#include "canary/ui/theme.h"

namespace canary::ui {

namespace {

lv_obj_t* s_bird = nullptr;    // container (nullptr when no live bird)
lv_obj_t* s_eye = nullptr;
lv_timer_t* s_blink = nullptr;
lv_anim_t s_bob;
CanaryMood s_mood = CanaryMood::Hidden;
int s_base_y = 0;
bool s_eye_shut = false;

// Brand palette. Canary yellow with a deeper wing shade and an orange
// beak — the only place these hues exist, so the bird IS the brand mark.
lv_color_t col_feather() { return lv_color_hex(0xFFD44F); }
lv_color_t col_wing()    { return lv_color_hex(0xE3B33C); }
lv_color_t col_beak()    { return lv_color_hex(0xF08C2E); }

lv_obj_t* dot(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t c) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_set_size(o, w, h);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

void blink_cb(lv_timer_t* t) {
  if (!s_eye) return;
  if (s_eye_shut) {
    lv_obj_clear_flag(s_eye, LV_OBJ_FLAG_HIDDEN);
    s_eye_shut = false;
    // Next blink lands 2.4–4 s out — dithered off the tick so a room of
    // displays never blinks in lockstep.
    lv_timer_set_period(t, 2400 + (lv_tick_get() % 1600));
  } else {
    lv_obj_add_flag(s_eye, LV_OBJ_FLAG_HIDDEN);
    s_eye_shut = true;
    lv_timer_set_period(t, 130);
  }
}

void bob_cb(void* var, int32_t v) {
  lv_obj_set_y((lv_obj_t*)var, s_base_y + v);
}

void start_bob() {
  lv_anim_init(&s_bob);
  lv_anim_set_var(&s_bob, s_bird);
  lv_anim_set_exec_cb(&s_bob, bob_cb);
  lv_anim_set_values(&s_bob, -2, 2);
  lv_anim_set_time(&s_bob, 1400);
  lv_anim_set_playback_time(&s_bob, 1400);
  lv_anim_set_repeat_count(&s_bob, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&s_bob, lv_anim_path_ease_in_out);
  lv_anim_start(&s_bob);
}

void hop_cb(void* var, int32_t v) {
  lv_obj_set_y((lv_obj_t*)var, s_base_y - v);
}

void hop_done(lv_anim_t*) { start_bob(); }

void start_hop() {
  lv_anim_init(&s_bob);
  lv_anim_set_var(&s_bob, s_bird);
  lv_anim_set_exec_cb(&s_bob, hop_cb);
  lv_anim_set_values(&s_bob, 0, 12);
  lv_anim_set_time(&s_bob, 240);
  lv_anim_set_playback_time(&s_bob, 320);
  lv_anim_set_path_cb(&s_bob, lv_anim_path_overshoot);
  lv_anim_set_ready_cb(&s_bob, hop_done);
  lv_anim_start(&s_bob);
}

void on_delete(lv_event_t*) {
  // The screen owning the bird is going away: drop every reference so a
  // later create starts clean and the blink timer never touches freed
  // memory.
  if (s_blink) {
    lv_timer_del(s_blink);
    s_blink = nullptr;
  }
  if (s_bird) lv_anim_del(s_bird, nullptr);
  s_bird = nullptr;
  s_eye = nullptr;
  s_mood = CanaryMood::Hidden;
}

}  // namespace

lv_obj_t* canary_mark_create(lv_obj_t* parent, int s) {
  if (s_bird) on_delete(nullptr);  // one live bird at a time

  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_set_size(c, s, s);
  lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_pad_all(c, 0, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);

  // Faces right: tail, body, wing, head, beak, eye — layered in that order.
  dot(c, 0, s * 42 / 100, s * 18 / 100, s * 12 / 100, col_wing());       // tail
  dot(c, s * 10 / 100, s * 32 / 100, s * 62 / 100, s * 54 / 100,
      col_feather());                                                    // body
  dot(c, s * 20 / 100, s * 46 / 100, s * 30 / 100, s * 22 / 100,
      col_wing());                                                       // wing
  dot(c, s * 46 / 100, s * 10 / 100, s * 42 / 100, s * 42 / 100,
      col_feather());                                                    // head
  dot(c, s * 84 / 100, s * 24 / 100, s * 14 / 100, s * 10 / 100,
      col_beak());                                                       // beak
  const int eye = s * 8 / 100 < 3 ? 3 : s * 8 / 100;
  s_eye = dot(c, s * 66 / 100, s * 20 / 100, eye, eye,
              lv_color_hex(0x1A1A1A));                                   // eye

  s_bird = c;
  s_base_y = lv_obj_get_y(c);
  lv_obj_add_event_cb(c, on_delete, LV_EVENT_DELETE, nullptr);
  s_blink = lv_timer_create(blink_cb, 2900, nullptr);
  s_mood = CanaryMood::Hidden;
  lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
  return c;
}

void canary_mark_mood(CanaryMood m) {
  if (!s_bird || m == s_mood) return;
  s_mood = m;
  if (m == CanaryMood::Hidden) {
    lv_anim_del(s_bird, nullptr);
    lv_obj_add_flag(s_bird, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(s_blink);
    return;
  }
  s_base_y = lv_obj_get_y(s_bird);
  lv_obj_clear_flag(s_bird, LV_OBJ_FLAG_HIDDEN);
  lv_timer_resume(s_blink);
  lv_anim_del(s_bird, nullptr);
  if (m == CanaryMood::Happy) start_hop();
  else start_bob();
}

}  // namespace canary::ui
