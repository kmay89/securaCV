// src/ui/canary_mark.cpp — the brand canary widget. See canary_mark.h.
//
// Living-canary craft notes (display_living_canary.md): valence lives in
// the eye, arousal lives in the body. Idle is SLOW (breathing pace, one
// flourish a minute at most); moods change the silhouette, not the noise.
// Every pose here maps to a system state the mood engine can name.
#include <config.h>
#include <Arduino.h>
#include <lvgl.h>
#include <esp_random.h>

#include "canary/ui/canary_mark.h"
#include "canary/ui/theme.h"

namespace canary::ui {

namespace {

lv_obj_t* s_bird = nullptr;    // container (nullptr when no live bird)
lv_obj_t* s_eye = nullptr;
lv_obj_t* s_wing = nullptr;
lv_obj_t* s_beak = nullptr;
lv_timer_t* s_blink = nullptr;
lv_timer_t* s_flourish = nullptr;   // idle scheduler-lite (Flipper cadence)
lv_timer_t* s_look_timer = nullptr;  // one-shot glance-aside return
lv_anim_t s_bob;
CanaryMood s_mood = CanaryMood::Hidden;
int s_size = 0;
int s_base_y = 0;
int s_base_x = 0;
bool s_eye_shut = false;
uint16_t s_trust_days = 0;

// Rest-pose geometry the poses offset from (fractions of s_size).
int s_eye_x = 0, s_eye_y = 0, s_eye_d = 0;
int s_wing_x = 0, s_wing_y = 0;
int s_beak_x = 0, s_beak_y = 0;

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
    // displays never blinks in lockstep. A worried bird blinks faster.
    const uint32_t base = s_mood == CanaryMood::Worried ||
                                  s_mood == CanaryMood::Distressed
                              ? 1500
                              : 2400;
    lv_timer_set_period(t, base + (lv_tick_get() % 1600));
  } else {
    lv_obj_add_flag(s_eye, LV_OBJ_FLAG_HIDDEN);
    s_eye_shut = true;
    lv_timer_set_period(t, 130);
  }
}

void bob_cb(void* var, int32_t v) {
  lv_obj_set_y((lv_obj_t*)var, s_base_y + v);
}

// Breath: the one always-on motion. Rate is the arousal baseline —
// alert 1.4 s, calm 1.4 s, asleep 2.8 s half-period.
void start_bob(uint32_t half_ms, int amp) {
  lv_anim_init(&s_bob);
  lv_anim_set_var(&s_bob, s_bird);
  lv_anim_set_exec_cb(&s_bob, bob_cb);
  lv_anim_set_values(&s_bob, -amp, amp);
  lv_anim_set_time(&s_bob, half_ms);
  lv_anim_set_playback_time(&s_bob, half_ms);
  lv_anim_set_repeat_count(&s_bob, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&s_bob, lv_anim_path_ease_in_out);
  lv_anim_start(&s_bob);
}

void hop_cb(void* var, int32_t v) {
  lv_obj_set_y((lv_obj_t*)var, s_base_y - v);
}

void hop_done(lv_anim_t*) { start_bob(1400, 2); }

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

// ── Pose vector (subset): eye openness, wing lift, head tuck ─────────────

void pose_rest() {
  if (!s_bird) return;
  lv_obj_set_size(s_eye, s_eye_d, s_eye_d);
  lv_obj_set_pos(s_eye, s_eye_x, s_eye_y);
  lv_obj_set_pos(s_wing, s_wing_x, s_wing_y);
  lv_obj_set_pos(s_beak, s_beak_x, s_beak_y);
  lv_obj_set_x(s_bird, s_base_x);
}

void pose_worried() {
  // Eye narrowed a step (fear reads in the eye), wing half-raised
  // (arousal reads in the body).
  pose_rest();
  const int d = s_eye_d * 3 / 4 < 2 ? 2 : s_eye_d * 3 / 4;
  lv_obj_set_size(s_eye, d, d);
  lv_obj_set_pos(s_eye, s_eye_x, s_eye_y + (s_eye_d - d));
  lv_obj_set_y(s_wing, s_wing_y - s_size * 4 / 100);
}

void pose_asleep() {
  // Eye becomes a line; beak tucks down a notch; everything else still.
  pose_rest();
  lv_obj_set_size(s_eye, s_eye_d * 3 / 2, 2);
  lv_obj_set_pos(s_eye, s_eye_x - s_eye_d / 4, s_eye_y + s_eye_d / 2);
  lv_obj_set_y(s_beak, s_beak_y + s_size * 5 / 100);
}

// ── Idle flourish scheduler-lite (the Flipper manifest, miniaturized) ───
//
// Every 25–60 s (jittered) in Idle, pick one small flourish by weight;
// the rare ones need trust days. Worried/Distressed swap the pool for
// fidgets. One flourish at a time, never queued.

void wing_settle_cb(lv_anim_t*) {
  if (!s_wing) return;
  // Settle back into the CURRENT pose, not blindly into rest — a worried
  // bird keeps its half-raised wing after a fidget.
  const bool lifted = s_mood == CanaryMood::Worried ||
                      s_mood == CanaryMood::Distressed;
  lv_obj_set_y(s_wing, lifted ? s_wing_y - s_size * 4 / 100 : s_wing_y);
}

void wing_anim_cb(void* var, int32_t v) {
  lv_obj_set_y((lv_obj_t*)var, v);
}

// Preen / fidget: the wing lifts and resettles a few times.
void flourish_wing(int reps, uint32_t half_ms) {
  if (!s_wing) return;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_wing);
  lv_anim_set_exec_cb(&a, wing_anim_cb);
  const int y = lv_obj_get_y(s_wing);
  lv_anim_set_values(&a, y, y - s_size * 6 / 100);
  lv_anim_set_time(&a, half_ms);
  lv_anim_set_playback_time(&a, half_ms);
  lv_anim_set_repeat_count(&a, (uint16_t)reps);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_ready_cb(&a, wing_settle_cb);
  lv_anim_start(&a);
}

void look_back_cb(lv_timer_t* t) {
  if (s_eye && s_mood != CanaryMood::Asleep)
    lv_obj_set_x(s_eye, s_eye_x);
  lv_timer_del(t);  // one-shot: self-delete, no repeat count (a repeat
                    // count of 1 would auto-delete AND we'd delete —
                    // double free)
  s_look_timer = nullptr;
}

// Glance aside: the eye saccades toward a side and returns (birds move
// in steps, not tweens). The timer is TRACKED (review catch): an
// untracked one-shot could outlive a bird deleted and recreated within
// its 900 ms and poke the new bird's eye.
void flourish_look() {
  if (!s_eye) return;
  if (s_look_timer) {
    lv_timer_del(s_look_timer);
    s_look_timer = nullptr;
  }
  const int dir = (esp_random() & 1) ? 1 : -1;
  lv_obj_set_x(s_eye, s_eye_x + dir * (s_eye_d / 2 + 1));
  s_look_timer = lv_timer_create(look_back_cb, 900, nullptr);
}

void flourish_cb(lv_timer_t* t) {
  if (!s_bird || lv_obj_has_flag(s_bird, LV_OBJ_FLAG_HIDDEN)) return;
  // Re-jitter the cadence every fire so the bird never feels metronomic.
  lv_timer_set_period(t, 25000 + (esp_random() % 35000));

  const uint32_t roll = esp_random() % 100;
  switch (s_mood) {
    case CanaryMood::Idle:
      // Weighted pool: glance 45, preen 30, hop 15; the joy-hop-and-
      // double-preen "song" needs a week of clean days (rarity is the
      // reward for security hygiene, not for grinding).
      if (roll < 45) flourish_look();
      else if (roll < 75) flourish_wing(2, 260);
      else if (roll < 90) start_hop();
      else if (s_trust_days >= 7) {
        start_hop();
        flourish_wing(4, 180);
      } else {
        flourish_look();
      }
      break;
    case CanaryMood::Worried:
      // Scanning saccades only — no play while something is late.
      flourish_look();
      break;
    case CanaryMood::Distressed:
      // Fidgety wing, restless glances.
      if (roll < 50) flourish_wing(3, 160);
      else flourish_look();
      break;
    default:
      break;  // Asleep/Happy/Hidden: no flourishes
  }
}

void on_delete(lv_event_t*) {
  // The screen owning the bird is going away: drop every reference so a
  // later create starts clean and the timers never touch freed memory.
  if (s_blink) {
    lv_timer_del(s_blink);
    s_blink = nullptr;
  }
  if (s_flourish) {
    lv_timer_del(s_flourish);
    s_flourish = nullptr;
  }
  if (s_look_timer) {
    lv_timer_del(s_look_timer);
    s_look_timer = nullptr;
  }
  if (s_bird) lv_anim_del(s_bird, nullptr);
  if (s_wing) lv_anim_del(s_wing, nullptr);
  s_bird = nullptr;
  s_eye = nullptr;
  s_wing = nullptr;
  s_beak = nullptr;
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
  s_wing_x = s * 20 / 100;
  s_wing_y = s * 46 / 100;
  s_wing = dot(c, s_wing_x, s_wing_y, s * 30 / 100, s * 22 / 100,
               col_wing());                                              // wing
  dot(c, s * 46 / 100, s * 10 / 100, s * 42 / 100, s * 42 / 100,
      col_feather());                                                    // head
  s_beak_x = s * 84 / 100;
  s_beak_y = s * 24 / 100;
  s_beak = dot(c, s_beak_x, s_beak_y, s * 14 / 100, s * 10 / 100,
               col_beak());                                              // beak
  const int eye = s * 8 / 100 < 3 ? 3 : s * 8 / 100;
  s_eye_x = s * 66 / 100;
  s_eye_y = s * 20 / 100;
  s_eye_d = eye;
  s_eye = dot(c, s_eye_x, s_eye_y, eye, eye,
              lv_color_hex(0x1A1A1A));                                   // eye

  s_bird = c;
  s_size = s;
  s_base_y = lv_obj_get_y(c);
  s_base_x = lv_obj_get_x(c);
  lv_obj_add_event_cb(c, on_delete, LV_EVENT_DELETE, nullptr);
  s_blink = lv_timer_create(blink_cb, 2900, nullptr);
  s_flourish = lv_timer_create(flourish_cb, 30000, nullptr);
  s_mood = CanaryMood::Hidden;
  lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
  return c;
}

void canary_mark_mood(CanaryMood m) {
  if (!s_bird || m == s_mood) return;
  s_mood = m;
  if (m == CanaryMood::Hidden) {
    lv_anim_del(s_bird, nullptr);
    lv_anim_del(s_wing, nullptr);
    lv_obj_add_flag(s_bird, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(s_blink);
    lv_timer_pause(s_flourish);
    return;
  }
  s_base_y = lv_obj_get_y(s_bird);
  s_base_x = lv_obj_get_x(s_bird);
  lv_obj_clear_flag(s_bird, LV_OBJ_FLAG_HIDDEN);
  lv_anim_del(s_bird, nullptr);
  lv_anim_del(s_wing, nullptr);

  switch (m) {
    case CanaryMood::Asleep:
      // Stillness is the information: line eye, tucked beak, slow breath,
      // no blinks, no flourishes.
      pose_asleep();
      lv_timer_pause(s_blink);
      lv_timer_pause(s_flourish);
      s_eye_shut = false;
      lv_obj_clear_flag(s_eye, LV_OBJ_FLAG_HIDDEN);
      start_bob(2800, 1);
      break;
    case CanaryMood::Worried:
    case CanaryMood::Distressed:
      pose_worried();
      lv_timer_resume(s_blink);
      lv_timer_resume(s_flourish);
      start_bob(1100, 2);  // breath quickens a touch
      break;
    case CanaryMood::Happy:
      pose_rest();
      lv_timer_resume(s_blink);
      lv_timer_resume(s_flourish);
      start_hop();
      break;
    default:  // Idle
      pose_rest();
      lv_timer_resume(s_blink);
      lv_timer_resume(s_flourish);
      start_bob(1400, 2);
      break;
  }
}

void canary_mark_trust(uint16_t days) { s_trust_days = days; }

}  // namespace canary::ui
