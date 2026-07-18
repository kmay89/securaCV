// src/ui/splash.cpp — boot splash. See splash.h.
#include "flavor_config.h"
#include <Arduino.h>
#include <lvgl.h>

#include "splash.h"
#include "canary_mark.h"
#include "theme.h"
#include "display.h"

namespace canary::ui {

void splash_play(uint32_t hold_ms) {
  lv_obj_t* prev = lv_scr_act();

  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, col_bg(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

#ifdef CD_FLAVOR_WATCH
  constexpr int BIRD = 64;
  constexpr int BIRD_Y = -42, WORD_Y = 26, TAG_Y = 56;
#else
  constexpr int BIRD = 96;
  constexpr int BIRD_Y = -66, WORD_Y = 34, TAG_Y = 78;
#endif

  lv_obj_t* bird = canary_mark_create(scr, BIRD);
  lv_obj_align(bird, LV_ALIGN_CENTER, 0, BIRD_Y);

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

  lv_scr_load(scr);
  canary::hal::backlight_set(CD_BRIGHT_DAY);
  canary_mark_mood(CanaryMood::Happy);  // the hop is the hello

  uint32_t t0 = millis();
  while ((int32_t)(millis() - t0) < (int32_t)hold_ms) {
    lv_timer_handler();
    delay(5);
  }
  // Cross-fade home; auto-delete tears the splash (and, via its DELETE
  // hook, the bird's timers) down so the next face starts clean.
  lv_scr_load_anim(prev, LV_SCR_LOAD_ANIM_FADE_ON, 380, 0, true);
  t0 = millis();
  while ((int32_t)(millis() - t0) < 420) {
    lv_timer_handler();
    delay(5);
  }
}

}  // namespace canary::ui
