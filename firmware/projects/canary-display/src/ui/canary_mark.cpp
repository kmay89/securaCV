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
lv_obj_t* s_shine = nullptr;   // eye catchlight (child of the eye)
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

// Temperament (Character wave): bounded scalars layered UNDER the mood
// engine — they rescale timings the mood already chose, never the pose.
// The raw breath request is remembered so a live re-apply rescales the
// CURRENT mood's rate instead of guessing.
float s_temp_breath = 1.0f;
float s_temp_flourish = 1.0f;
float s_temp_hop = 1.0f;
uint32_t s_breath_raw_ms = 1400;
int s_breath_amp = 2;

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
    const bool uneasy = s_mood == CanaryMood::Worried ||
                        s_mood == CanaryMood::Distressed ||
                        s_mood == CanaryMood::Searching ||
                        s_mood == CanaryMood::Calling;
    const uint32_t base = uneasy ? 1500 : 2400;
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
// alert 1.4 s, calm 1.4 s, asleep 2.8 s half-period. The Character's
// breath temperament rescales the request (clamped to a sane band).
void start_bob(uint32_t half_ms, int amp) {
  s_breath_raw_ms = half_ms;
  s_breath_amp = amp;
  uint32_t p = (uint32_t)((float)half_ms * s_temp_breath);
  if (p < 500) p = 500;
  if (p > 6000) p = 6000;
  lv_anim_init(&s_bob);
  lv_anim_set_var(&s_bob, s_bird);
  lv_anim_set_exec_cb(&s_bob, bob_cb);
  lv_anim_set_values(&s_bob, -amp, amp);
  lv_anim_set_time(&s_bob, p);
  lv_anim_set_playback_time(&s_bob, p);
  lv_anim_set_repeat_count(&s_bob, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&s_bob, lv_anim_path_ease_in_out);
  lv_anim_start(&s_bob);
}

void hop_cb(void* var, int32_t v) {
  lv_obj_set_y((lv_obj_t*)var, s_base_y - v);
}

void hop_done(lv_anim_t*) { start_bob(1400, 2); }

void start_hop() {
  // Hop apex rides the Character's hop energy, clamped — even the
  // springiest look stays inside the calm-tech ration.
  int apex = (int)(12.0f * s_temp_hop);
  if (apex < 8) apex = 8;
  if (apex > 18) apex = 18;
  lv_anim_init(&s_bob);
  lv_anim_set_var(&s_bob, s_bird);
  lv_anim_set_exec_cb(&s_bob, hop_cb);
  lv_anim_set_values(&s_bob, 0, apex);
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
  if (s_shine) lv_obj_clear_flag(s_shine, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_pos(s_wing, s_wing_x, s_wing_y);
  // Size AND position: the calling pose opens the beak taller, and a rest
  // that only moved it would leave the beak stuck open (review catch).
  lv_obj_set_size(s_beak, s_size * 14 / 100, s_size * 10 / 100);
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
  // The catchlight sleeps too — a closed eye doesn't sparkle.
  pose_rest();
  if (s_shine) lv_obj_add_flag(s_shine, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_size(s_eye, s_eye_d * 3 / 2, 2);
  lv_obj_set_pos(s_eye, s_eye_x - s_eye_d / 4, s_eye_y + s_eye_d / 2);
  lv_obj_set_y(s_beak, s_beak_y + s_size * 5 / 100);
}

void pose_searching() {
  // Someone is late: the bird leans toward the edge it faces, eye held
  // outward and fully open — looking FOR them, not afraid of them.
  pose_rest();
  lv_obj_set_x(s_bird, s_base_x + s_size * 6 / 100);
  lv_obj_set_x(s_eye, s_eye_x + s_eye_d / 2 + 1);
}

void pose_calling() {
  // Someone is lost: beak open (taller, opening downward), wing half
  // raised — the body says it is calling out for them.
  pose_rest();
  lv_obj_set_height(s_beak, s_size * 16 / 100);
  lv_obj_set_y(s_wing, s_wing_y - s_size * 4 / 100);
}

// Restore whatever pose the CURRENT mood owns (used by reactions and any
// settle path — never blindly rest).
void pose_current() {
  switch (s_mood) {
    case CanaryMood::Worried:
    case CanaryMood::Distressed: pose_worried(); break;
    case CanaryMood::Searching:  pose_searching(); break;
    case CanaryMood::Calling:    pose_calling(); break;
    case CanaryMood::Asleep:     pose_asleep(); break;
    default:                     pose_rest(); break;
  }
}

// ── Idle flourish scheduler-lite (the Flipper manifest, miniaturized) ───
//
// Every 25–60 s (jittered) in Idle, pick one small flourish by weight;
// the rare ones need trust days. Worried/Distressed swap the pool for
// fidgets. One flourish at a time, never queued.

void wing_settle_cb(lv_anim_t*) {
  if (!s_wing) return;
  // Settle back into the CURRENT pose, not blindly into rest — a worried
  // (or calling) bird keeps its half-raised wing after a fidget.
  const bool lifted = s_mood == CanaryMood::Worried ||
                      s_mood == CanaryMood::Distressed ||
                      s_mood == CanaryMood::Calling;
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

// Searching scan: glance back toward the room, then return to the held
// outward gaze (look_back_cb restores rest-x; the searching pose is
// re-asserted so the outward hold survives the scan).
void search_scan_back_cb(lv_timer_t* t) {
  if (s_bird && s_mood == CanaryMood::Searching) pose_searching();
  lv_timer_del(t);
  s_look_timer = nullptr;
}

void flourish_search_scan() {
  if (!s_eye) return;
  if (s_look_timer) {
    lv_timer_del(s_look_timer);
    s_look_timer = nullptr;
  }
  lv_obj_set_x(s_eye, s_eye_x - (s_eye_d / 2 + 1));  // check over the shoulder
  s_look_timer = lv_timer_create(search_scan_back_cb, 700, nullptr);
}

void flourish_cb(lv_timer_t* t) {
  if (!s_bird || lv_obj_has_flag(s_bird, LV_OBJ_FLAG_HIDDEN)) return;
  // Re-jitter the cadence every fire so the bird never feels metronomic;
  // the Character's flourish temperament widens or tightens the window.
  const uint32_t base = 25000 + (esp_random() % 35000);
  lv_timer_set_period(t, (uint32_t)((float)base * s_temp_flourish));

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
    case CanaryMood::Searching:
      // Mostly the held outward watch; sometimes a quick scan back over
      // the shoulder, sometimes a small hop toward the edge.
      if (roll < 60) flourish_search_scan();
      else start_hop();
      break;
    case CanaryMood::Calling:
      // Restless body while calling — wing fidget or a glance around.
      if (roll < 50) flourish_wing(2, 220);
      else flourish_look();
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

// ── One-shot reactions (event-driven, layered over the mood) ─────────────

lv_timer_t* s_react_timer = nullptr;  // tracked pose-restore one-shot
uint32_t s_last_small_react = 0;      // Tilt/Startle ration clock
bool s_base_recorded = false;         // base captured once, post-placement

void react_restore_cb(lv_timer_t* t) {
  if (s_bird) pose_current();
  lv_timer_del(t);
  s_react_timer = nullptr;
}

void arm_react_restore(uint32_t ms) {
  if (s_react_timer) lv_timer_del(s_react_timer);
  s_react_timer = lv_timer_create(react_restore_cb, ms, nullptr);
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
  if (s_react_timer) {
    lv_timer_del(s_react_timer);
    s_react_timer = nullptr;
  }
  if (s_bird) lv_anim_del(s_bird, nullptr);
  if (s_wing) lv_anim_del(s_wing, nullptr);
  s_bird = nullptr;
  s_eye = nullptr;
  s_shine = nullptr;
  s_wing = nullptr;
  s_beak = nullptr;
  s_mood = CanaryMood::Hidden;
  s_base_recorded = false;  // the next bird records its own base
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
  // Cheek blush: a soft warm dot under the eye — reads as "alive and
  // sweet" at any size, costs one object. Sits on the head, behind the
  // eye in z-order (added first).
  dot(c, s * 58 / 100, s * 33 / 100, s * 13 / 100, s * 8 / 100,
      lv_color_hex(0xF2A38F));
  const int eye = s * 10 / 100 < 3 ? 3 : s * 10 / 100;  // a touch bigger:
  s_eye_x = s * 66 / 100;                               // big eyes read as
  s_eye_y = s * 19 / 100;                               // young and kind
  s_eye_d = eye;
  s_eye = dot(c, s_eye_x, s_eye_y, eye, eye,
              lv_color_hex(0x1A1A1A));                                   // eye
  // Catchlight: the single white spark that turns a dot into a LOOK. A
  // child of the eye, so blinks and saccades carry it automatically.
  const int shine = eye / 3 < 1 ? 1 : eye / 3;
  s_shine = dot(s_eye, eye / 5, eye / 6, shine, shine,
                lv_color_hex(0xFFFFFF));

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
  const CanaryMood prev = s_mood;
  s_mood = m;
  // A mood change invalidates any in-flight one-shot: a pending glance
  // return or reaction restore firing later would clobber the new pose
  // (review catch).
  if (s_look_timer) {
    lv_timer_del(s_look_timer);
    s_look_timer = nullptr;
  }
  if (s_react_timer) {
    lv_timer_del(s_react_timer);
    s_react_timer = nullptr;
  }
  if (m == CanaryMood::Hidden) {
    lv_anim_del(s_bird, nullptr);
    lv_anim_del(s_wing, nullptr);
    // Normalize the searching lean before going off stage so the hidden
    // bird parks at its true base (review catch).
    if (prev == CanaryMood::Searching && s_base_recorded)
      lv_obj_set_x(s_bird, s_base_x);
    lv_obj_add_flag(s_bird, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(s_blink);
    lv_timer_pause(s_flourish);
    return;
  }
  // Capture the base ONCE, at the first on-stage mood after the host has
  // placed the bird. Re-reading on every transition would bake live
  // bob/hop offsets (up to the 12 px hop apex) into the base and the
  // bird would drift (review catch). The poses restore from this base.
  if (!s_base_recorded) {
    s_base_y = lv_obj_get_y(s_bird);
    s_base_x = lv_obj_get_x(s_bird);
    s_base_recorded = true;
  }
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
    case CanaryMood::Searching:
      pose_searching();
      lv_timer_resume(s_blink);
      lv_timer_resume(s_flourish);
      start_bob(1100, 2);
      break;
    case CanaryMood::Calling:
      pose_calling();
      lv_timer_resume(s_blink);
      lv_timer_resume(s_flourish);
      start_bob(1100, 2);
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

void canary_mark_temperament(float breath, float flourish, float hop) {
  // Bounds are clamped in code (display_character.md §5): even the
  // liveliest Character stays inside the calm-tech ration, and the mood
  // engine still owns truth — these only rescale timings it already chose.
  if (breath < 0.85f) breath = 0.85f;
  if (breath > 1.25f) breath = 1.25f;
  if (flourish < 0.7f) flourish = 0.7f;
  if (flourish > 1.4f) flourish = 1.4f;
  if (hop < 0.85f) hop = 0.85f;
  if (hop > 1.25f) hop = 1.25f;
  s_temp_breath = breath;
  s_temp_flourish = flourish;
  s_temp_hop = hop;
  // Re-apply the breath live so the new rate lands immediately: the
  // picker persists to the hidden face's bird, and the raw request keeps
  // the CURRENT mood's pace — the new scalar just rescales it. No bird
  // (or an off-stage one) means no timers to touch.
  if (s_bird && s_mood != CanaryMood::Hidden)
    start_bob(s_breath_raw_ms, s_breath_amp);
}

void canary_mark_react(CanaryReact r) {
  // Never while off stage, and never startle a sleeping bird (night is
  // sacred — the greeting fires after the wake transition, not during).
  if (!s_bird || s_mood == CanaryMood::Hidden || s_mood == CanaryMood::Asleep)
    return;
  const uint32_t now = lv_tick_get();
  const bool small = r == CanaryReact::Tilt || r == CanaryReact::Startle;
  if (small && now - s_last_small_react < 8000) return;  // rationed
  if (small) s_last_small_react = now;
  // A pending glance return firing mid-reaction would clobber the
  // reaction's eye placement (review catch).
  if (s_look_timer) {
    lv_timer_del(s_look_timer);
    s_look_timer = nullptr;
  }

  switch (r) {
    case CanaryReact::Tilt:
      // Curious head-cock: beak dips while the eye rises a step.
      lv_obj_set_y(s_beak, s_beak_y + s_size * 3 / 100);
      lv_obj_set_y(s_eye, lv_obj_get_y(s_eye) - s_size * 2 / 100);
      arm_react_restore(700);
      break;
    case CanaryReact::Startle: {
      // Quick hop with a momentarily wide eye.
      const int d = s_eye_d + 2;
      lv_obj_set_size(s_eye, d, d);
      lv_obj_set_pos(s_eye, s_eye_x - 1, s_eye_y - 1);
      start_hop();
      arm_react_restore(500);
      break;
    }
    case CanaryReact::Greeting:
      // Morning stretch: one slow, high wing lift.
      flourish_wing(1, 700);
      arm_react_restore(1600);
      break;
    case CanaryReact::Joyful:
      // The song: a hop with a long double ruffle. Rare by construction —
      // the glue only sends it at a trust milestone.
      start_hop();
      flourish_wing(6, 150);
      arm_react_restore(2000);
      break;
    case CanaryReact::Reassure:
      // Acknowledgement is friendship done right: the bird settles, but it
      // does not pretend the condition vanished before the facts do.
      lv_obj_set_y(s_eye, lv_obj_get_y(s_eye) + 1);
      flourish_wing(1, 420);
      arm_react_restore(900);
      break;
    case CanaryReact::Nestle:
      // Quiet-hours handoff: tuck the beak and soften the eye. No sound, no
      // flash, no attention grab — trust sometimes means becoming smaller.
      pose_asleep();
      arm_react_restore(1200);
      break;
  }
}

}  // namespace canary::ui
