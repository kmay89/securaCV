// src/ui/splash.cpp — boot splash. See splash.h.
//
// Two splashes live here. The FIRST boot ever is a meeting, not a logo:
// the bird hops in, notices you, and introduces itself in a speech
// bubble, one short typed line at a time. The craft is in the timing —
// presence before speech (the bird arrives, settles, and tilts its head
// BEFORE the first word), a typewriter that breathes at punctuation, and
// a tap that always advances (respect beats spectacle). Every later boot
// plays the short familiar splash instead: you have already met.
//
// The SCRIPT itself is no longer here. Both meetings are scenes in
// firmware/common/story/ and are played by the shared performance engine, so
// the same bird — the same arrival, the same joke, the same promise — turns up
// on the watch, the dash, the nightstand stick and (as the engine is adopted)
// the phone and the wrist. This file is now a renderer for a character it does
// not own, which is the point: a personality that is re-typed per surface is
// four personalities.
#include <config.h>
// The nightstand (172x320 portrait, no touch) borrows the watch's
// small-portrait rendering for the shared modal/support surfaces; only its
// standing fleet face (portrait_ui.cpp) is bespoke. Aliasing the flavor here,
// per-TU, keeps those surfaces one-renderer without threading a third
// geometry through every branch. The hero faces stay separate: glance_ui.cpp
// is guarded CD_FLAVOR_WATCH and never compiles for the nightstand.
#if defined(CD_FLAVOR_NIGHTSTAND) && !defined(CD_FLAVOR_WATCH)
#define CD_FLAVOR_WATCH 1
#endif
#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>
#include <string.h>

#include "canary/ui/splash.h"
#include "canary/ui/canary_mark.h"
#include "canary/ui/round_frame_core.h"
#include "story/story.h"
#include "story/story_scripts.h"
#include "identity/device_pseudonym.h"
#include "canary/ui/theme.h"
#include "canary/ui/character.h"
#include "canary/hal/display.h"

namespace canary::ui {

namespace {

// Rising touch edge, tracked locally: the intro runs before the main
// loop's gesture machine exists, so it keeps its own tiny one.
bool tap_edge() {
  static bool was = false;
  const auto t = canary::hal::touch_read();
  const bool edge = t.touched && !was;
  was = t.touched;
  return edge;
}

// Pump LVGL for ms. When skippable, a tap returns true immediately —
// the user outranks the storyboard.
bool pump(uint32_t ms, bool skippable) {
  const uint32_t t0 = millis();
  while ((int32_t)(millis() - t0) < (int32_t)ms) {
    lv_timer_handler();
    if (skippable && tap_edge()) return true;
    delay(5);
  }
  return false;
}

// (The typewriter moved into the story engine — `StoryTeller::frame()` reports
// how many characters have landed, punctuation breaths included, so the timing
// is shared with every other surface that plays a scene instead of being one
// board's local loop.)

bool met_before() {
  Preferences p;
  if (!p.begin("scv-hello", /*readOnly=*/true)) return false;
  const bool met = p.getUChar("met", 0) != 0;
  p.end();
  return met;
}

void remember_meeting() {
  Preferences p;
  if (!p.begin("scv-hello", /*readOnly=*/false)) return;
  p.putUChar("met", 1);
  p.end();
}

}  // namespace

// The exit curtain: an opaque layer-top overlay dropped by splash_play and
// lifted by splash_reveal once the real face has painted beneath it.
lv_obj_t* s_curtain = nullptr;

void splash_play(uint32_t hold_ms) {
  lv_obj_t* prev = lv_scr_act();
  // The default boot screen is theme-styled (bright) — paint it our black
  // so no frame of it can ever read as a flash.
  lv_obj_set_style_bg_color(prev, col_bg(), 0);
  lv_obj_set_style_bg_opa(prev, LV_OPA_COVER, 0);

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, col_bg(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  const bool first_meeting = !met_before();

#ifdef CD_FLAVOR_WATCH
  constexpr int BIRD = 64;
  constexpr int BIRD_Y = -42, WORD_Y = 26, TAG_Y = 56;
  // Intro: bird high, bubble CENTERED — the round glass is widest at its
  // middle, and a wrapped line low on the disc would clip its corners.
  // The Round Frame engine pins that claim: the bubble's band (its tallest
  // three-line case, roughly ±40 px around BUB_Y) must keep its chord.
  constexpr int INTRO_BIRD_Y = -70;
  constexpr int BUB_W = 196, BUB_Y = 5;
#if !defined(CD_FLAVOR_NIGHTSTAND)
  static_assert(BUB_W <= canary::ui::roundframe::chord(
                             canary::ui::roundframe::kDiscDiameter / 2 +
                                 BUB_Y - 40,
                             80),
                "speech bubble outgrew the disc's mid-band chord");
#endif
#else
  constexpr int BIRD = 96;
  constexpr int BIRD_Y = -66, WORD_Y = 34, TAG_Y = 78;
  constexpr int INTRO_BIRD_Y = -80;
  constexpr int BUB_W = 420, BUB_Y = 24;
#endif

  lv_obj_t* bird = canary_mark_create(scr, BIRD);
  lv_obj_align(bird, LV_ALIGN_CENTER, 0,
               first_meeting ? INTRO_BIRD_Y : BIRD_Y);

  lv_obj_t* word = lv_label_create(scr);
  lv_obj_set_style_text_font(word, font_title(), 0);
  lv_obj_set_style_text_color(word, col_text(), 0);
  lv_obj_set_style_text_letter_space(word, 2, 0);
  lv_label_set_text(word, "SecuraCV");
  lv_obj_align(word, LV_ALIGN_CENTER, 0, WORD_Y);

  lv_obj_t* tag = lv_label_create(scr);
  lv_obj_set_style_text_font(tag, font_caption(), 0);
  lv_obj_set_style_text_color(tag, col_muted(), 0);
  lv_label_set_text(tag, "a canary that shows");
  lv_obj_align(tag, LV_ALIGN_CENTER, 0, TAG_Y);

  // Speech bubble: card surface, soft corners, a small tail rotated out
  // of a square, pointing up at the speaker.
  lv_obj_t* bub = lv_obj_create(scr);
  lv_obj_set_width(bub, BUB_W);
  lv_obj_set_height(bub, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(bub, col_surface(), 0);
  lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(bub, col_edge(), 0);
  lv_obj_set_style_border_width(bub, 1, 0);
  lv_obj_set_style_radius(bub, 12, 0);
  lv_obj_set_style_pad_all(bub, 10, 0);
  lv_obj_clear_flag(bub, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(bub, LV_ALIGN_CENTER, 0, BUB_Y);

  lv_obj_t* tail = lv_obj_create(scr);
  lv_obj_set_size(tail, 12, 12);
  lv_obj_set_style_bg_color(tail, col_surface(), 0);
  lv_obj_set_style_bg_opa(tail, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(tail, col_edge(), 0);
  lv_obj_set_style_border_width(tail, 1, 0);
  lv_obj_set_style_radius(tail, 2, 0);
#if LVGL_VERSION_MAJOR >= 9
  lv_obj_set_style_transform_rotation(tail, 450, 0);
#else
  lv_obj_set_style_transform_angle(tail, 450, 0);
#endif
  lv_obj_clear_flag(tail, LV_OBJ_FLAG_SCROLLABLE);

  // Fixed label width, centered text. A percent-sized child inside a
  // content-sized parent is circular in LVGL and inflated the bubble into
  // a giant empty pill on the bench; the fixed width also lets short lines
  // sit centered instead of lopsided against the left pad.
  lv_obj_t* line = lv_label_create(bub);
  lv_obj_set_width(line, BUB_W - 24);
  lv_label_set_long_mode(line, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(line, font_label(), 0);
  lv_obj_set_style_text_color(line, col_text(), 0);
  lv_label_set_text(line, "");

  lv_scr_load(scr);
  canary::hal::backlight_set(CD_BRIGHT_DAY);

  if (first_meeting) {
    // ── The first meeting ────────────────────────────────────────────
    // Wordmark waits in the wings; the bird speaks first.
    lv_obj_add_flag(word, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tag, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tail, LV_OBJ_FLAG_HIDDEN);

    // Presence before speech is the script's now, not ours: kHello opens on
    // three wordless beats (arrive, settle, look at you) before a character
    // is typed, so the timing lives with the writing instead of being
    // re-tuned here every time somebody edits the arc.

    // The tail hangs off the bubble's top edge, under the bird.
    lv_obj_align_to(tail, bub, LV_ALIGN_OUT_TOP_MID, 0, 5);
    lv_obj_clear_flag(bub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(tail, LV_OBJ_FLAG_HIDDEN);

    // ── The script is the story engine's, not ours ────────────────────
    // `story::kHello` (firmware/common/story/story_scripts.h) is the whole
    // arc: presence, the introduction, the setup, the "Nothing." punchline,
    // the walk-back, the color language, the fleet, and the promise.
    //
    // This renderer consumes TWO of a beat's four channels today: `pose` goes
    // to the mark, and `line` types into the bubble. `gesture` and `tone` are
    // authored in the script but NOT dispatched here yet — the scripted
    // Narrow held-breath and the Pulse punchline do not reach the glass wash,
    // the WS2812 or the type weight. They are the next slice; until then the
    // beats carry the direction and no surface acts on it.
    //
    // The pseudonym is the one runtime substitution the script may carry. It
    // is derived from a per-device random salt and reads NO hardware MAC — see
    // device_pseudonym.h — which is precisely why the bird is allowed to make
    // a joke about not having yours.
    {
      canary::story::StoryTeller teller;
      char pseudo[device_pseudonym::HEX_LEN + 1] = {0};
      if (device_pseudonym::device_id_hex(pseudo, sizeof(pseudo)))
        teller.set_subject(pseudo);

      teller.play(&canary::story::kHello, millis(), /*night=*/false);

      char buf[192];
      canary::story::Pose last = canary::story::Pose::Hold;
      bool bubble_up = true;

      while (teller.active()) {
        const canary::story::Frame f = teller.frame(millis());
        if (f.beat == nullptr) break;

        // Pose -> the mark's own vocabulary. Anything this board has no pose
        // for simply does not move, which is a legitimate reading of a beat.
        if (f.beat->pose != last) {
          last = f.beat->pose;
          switch (f.beat->pose) {
            case canary::story::Pose::Enter:
            case canary::story::Pose::Hop:
            case canary::story::Pose::Ruffle:
              canary_mark_mood(CanaryMood::Happy);
              break;
            case canary::story::Pose::Settle:
            case canary::story::Pose::Lean:
            case canary::story::Pose::Look:
              canary_mark_mood(CanaryMood::Idle);
              break;
            case canary::story::Pose::Tilt:
              canary_mark_react(CanaryReact::Tilt);
              break;
            case canary::story::Pose::Preen:
              canary_mark_react(CanaryReact::Startle);
              break;
            case canary::story::Pose::Stretch:
              canary_mark_react(CanaryReact::Greeting);
              break;
            default:
              break;
          }
        }

        // Wordless beats hide the bubble entirely — the silence before the
        // punchline has to LOOK like silence, not like an empty box.
        if (f.beat->line != nullptr) {
          teller.expand(f.beat, buf, sizeof(buf));
          const size_t full = strlen(buf);
          const size_t shown = (size_t)f.chars < full ? (size_t)f.chars : full;
          buf[shown] = '\0';
          lv_label_set_text(line, buf);
          if (!bubble_up) {
            lv_obj_clear_flag(bub, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(tail, LV_OBJ_FLAG_HIDDEN);
            bubble_up = true;
          }
        } else if (bubble_up) {
          lv_obj_add_flag(bub, LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(tail, LV_OBJ_FLAG_HIDDEN);
          bubble_up = false;
        }

        // The user outranks the storyboard: a tap completes the line, the
        // next tap moves on. The engine owns that rule; we just report taps.
        if (tap_edge()) teller.tap(millis());
        pump(16, false);
      }
      lv_obj_add_flag(bub, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(tail, LV_OBJ_FLAG_HIDDEN);
    }

    // The bubble bows out; the wordmark takes the stage for one beat, so
    // the name lands AFTER the friend does.
    lv_obj_add_flag(bub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(word, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(tag, LV_OBJ_FLAG_HIDDEN);
    pump(900, true);

    remember_meeting();
  } else {
    // ── Hello again ──────────────────────────────────────────────────
    // The short familiar splash: hop, wordmark — and the tagline becomes
    // a quiet greeting, because you two have already met. No bubble, no
    // extra time; a returning boot must never feel slower than home.
    // The greeting is the Character's (character_apply runs before the
    // splash); the first-meeting script above stays canonical — you meet
    // the same bird no matter which look the glass later wears.
    lv_obj_add_flag(bub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tail, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(tag, active_voice().hello_again);
    canary_mark_mood(CanaryMood::Happy);
    pump(hold_ms, false);
  }

  // Drop the curtain (opaque, on the global top layer), then cut to the
  // black base screen and tear the splash down — its DELETE hook cleans
  // the bird's timers so the face can build its own. The face paints
  // UNDER the curtain; splash_reveal() lifts it.
  const int W = lv_disp_get_hor_res(NULL);
  const int H = lv_disp_get_ver_res(NULL);
  s_curtain = lv_obj_create(lv_layer_top());
  lv_obj_set_size(s_curtain, W, H + 4);
  lv_obj_set_pos(s_curtain, 0, 0);
  // A clean, solid curtain — the yellow scanline below carries the arcade
  // feel. (A full-screen vertical gradient here banded on the RGB565 panel.)
  lv_obj_set_style_bg_color(s_curtain, col_bg(), 0);
  lv_obj_set_style_bg_opa(s_curtain, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_curtain, 0, 0);
  lv_obj_set_style_radius(s_curtain, 0, 0);
  lv_obj_set_style_pad_all(s_curtain, 0, 0);
  lv_obj_clear_flag(s_curtain, LV_OBJ_FLAG_SCROLLABLE);
  // The arcade edge: a thin canary-yellow scanline leading the wipe.
  lv_obj_t* edge = lv_obj_create(s_curtain);
  lv_obj_set_size(edge, W, 3);
  lv_obj_align(edge, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(edge, lv_color_hex(0xFFD44F), 0);
  lv_obj_set_style_bg_opa(edge, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(edge, 0, 0);
  lv_obj_set_style_radius(edge, 0, 0);

  lv_scr_load(prev);
  lv_obj_del(scr);
  lv_timer_handler();
}

void splash_reveal() {
  if (!s_curtain) return;
  // Wipe the curtain up off the painted face: ease-in so it gathers speed,
  // the yellow scanline leading. Blocking pump, same as the splash beats.
  const int H = lv_disp_get_ver_res(NULL);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_curtain);
  lv_anim_set_exec_cb(&a, [](void* var, int32_t v) {
    lv_obj_set_y((lv_obj_t*)var, v);
  });
  lv_anim_set_values(&a, 0, -(H + 8));
  lv_anim_set_time(&a, 460);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_start(&a);
  const uint32_t t0 = millis();
  while ((int32_t)(millis() - t0) < 520) {
    lv_timer_handler();
    delay(5);
  }
  lv_obj_del(s_curtain);
  s_curtain = nullptr;
}

}  // namespace canary::ui
