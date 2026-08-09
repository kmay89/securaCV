// Arcade mode runtime (see include/canary/mode/arcade_mode.h). The whole TU
// is empty without FEATURE_ARCADE (and it is dash-only — the pure logic is
// geometry-blind, but this face is laid out for the 800x480 panel).

#include "canary/config.h"

#if defined(FEATURE_ARCADE) && FEATURE_ARCADE && defined(CD_FLAVOR_DASH)

#include <Arduino.h>
#include <lvgl.h>

#include "canary/mode/arcade_mode.h"
#include "canary/mode/arcade_logic.h"
#include "canary/mode/mode_glue.h"
#include "canary/hal/display.h"
#include "canary/ui/lvgl_port.h"
#include "canary/version.h"

#include "boot/boot_banner.h"

namespace canary {
namespace mode {

namespace {

using namespace canary::mode::arcade;

// The factory geometry the host test pins: 8x5 zones on 800x480.
constexpr uint8_t  COLS = 8, ROWS = 5, ZONES = COLS * ROWS;
constexpr int16_t  SCR_W = 800, SCR_H = 480;
constexpr int16_t  TARGET_PX = 96;
// Latency bar for the verdict: generous, because a human reaction rides on
// top of the panel's — the bar exists to catch a zone that needed several
// stabs or a controller that lags, not to time athletes.
constexpr uint16_t WORST_ALLOWED_MS = 2000;

lv_color_t c_bg()     { return lv_color_hex(0x000000); }
lv_color_t c_text()   { return lv_color_hex(0xEDEDED); }
lv_color_t c_muted()  { return lv_color_hex(0x9A9A9A); }
lv_color_t c_accent() { return lv_color_hex(0x3AA3FF); }
lv_color_t c_ok()     { return lv_color_hex(0x4CAF50); }
lv_color_t c_warn()   { return lv_color_hex(0xFFB300); }
lv_color_t c_alert()  { return lv_color_hex(0xE53935); }

enum class Phase : uint8_t { Idle, Playing, Report };

bool      s_display_ok = false;
Phase     s_phase = Phase::Idle;
RoundPlan s_plan;
RoundStats s_stats;
uint32_t  s_seed = 0;
uint32_t  s_jitter_seed = 0;
uint8_t   s_step = 0;           // index into s_plan.order
Target    s_target;
uint32_t  s_target_shown_ms = 0;

lv_obj_t* s_title = nullptr;
lv_obj_t* s_body = nullptr;     // idle/report text
lv_obj_t* s_bird = nullptr;     // the live target
lv_obj_t* s_progress = nullptr; // "12/40" during play

bool     s_touch_down = false;
uint32_t s_touch_down_ms = 0;
int16_t  s_tx = 0, s_ty = 0;

void build_face() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, c_bg(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clean(scr);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  s_title = lv_label_create(scr);
  lv_obj_set_style_text_font(s_title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(s_title, c_warn(), 0);
  lv_label_set_text(s_title, "ARCADE  •  Canary Catch  •  hold 3s to exit");
  lv_obj_set_pos(s_title, 14, 10);

  s_body = lv_label_create(scr);
  lv_obj_set_style_text_font(s_body, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_body, c_text(), 0);
  lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_body, LV_ALIGN_CENTER, 0, 0);

  s_progress = lv_label_create(scr);
  lv_obj_set_style_text_font(s_progress, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_progress, c_muted(), 0);
  lv_obj_align(s_progress, LV_ALIGN_TOP_RIGHT, -14, 14);
  lv_label_set_text(s_progress, "");

  // The bird: a round target the whole game is about. Hidden until play.
  s_bird = lv_obj_create(scr);
  lv_obj_set_style_radius(s_bird, TARGET_PX / 2, 0);
  lv_obj_set_style_bg_color(s_bird, c_accent(), 0);
  lv_obj_set_style_bg_opa(s_bird, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_bird, c_text(), 0);
  lv_obj_set_style_border_width(s_bird, 2, 0);
  lv_obj_add_flag(s_bird, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t* eye = lv_label_create(s_bird);
  lv_obj_set_style_text_font(eye, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(eye, lv_color_hex(0x111111), 0);
  lv_label_set_text(eye, "o o");
  lv_obj_center(eye);
}

void show_idle() {
  s_phase = Phase::Idle;
  lv_obj_add_flag(s_bird, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(s_progress, "");
  lv_obj_set_style_text_color(s_body, c_text(), 0);
  lv_label_set_text(s_body,
      "Catch the canary!\n\n"
      "One round lands a bird in every touch zone\n"
      "of the panel - the score screen is the QA report.\n\n"
      "tap to start");
  lv_obj_align(s_body, LV_ALIGN_CENTER, 0, 0);
}

void place_target(uint32_t now) {
  const uint8_t zone = s_plan.order[s_step];
  s_target = target_for_zone(zone, COLS, ROWS, SCR_W, SCR_H, TARGET_PX,
                             s_jitter_seed);
  lv_obj_set_size(s_bird, s_target.size, s_target.size);
  lv_obj_set_style_radius(s_bird, s_target.size / 2, 0);
  lv_obj_set_pos(s_bird, s_target.x, s_target.y);
  lv_obj_clear_flag(s_bird, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text_fmt(s_progress, "%u/%u", (unsigned)(s_step + 1),
                        (unsigned)ZONES);
  s_target_shown_ms = now;
}

void start_round(uint32_t now) {
  s_phase = Phase::Playing;
  s_seed = now;  // shown on the report; a factory failure replays exactly
  s_jitter_seed = s_seed ^ 0xA5A5A5A5u;
  s_plan.build(ZONES, s_seed);
  s_stats = RoundStats();
  s_step = 0;
  lv_label_set_text(s_body, "");
  place_target(now);
  Serial.printf("ARC1 %lu EVT round start seed=%lu zones=%u\r\n",
                (unsigned long)now, (unsigned long)s_seed, (unsigned)ZONES);
}

void show_report(uint32_t now) {
  s_phase = Phase::Report;
  lv_obj_add_flag(s_bird, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(s_progress, "");
  const bool pass = qa_pass(s_stats, ZONES, WORST_ALLOWED_MS);
  lv_obj_set_style_text_color(s_body, pass ? c_ok() : c_alert(), 0);
  char rep[256];
  snprintf(rep, sizeof(rep),
           "%s\n\n"
           "zones  %u/%u\n"
           "stray taps  %u\n"
           "latency  avg %u ms • worst %u ms (bar %u)\n"
           "seed  %lu\n\n"
           "tap to play again",
           pass ? "PASS" : "FAIL", (unsigned)s_stats.zones_hit(ZONES),
           (unsigned)ZONES, (unsigned)s_stats.misses,
           (unsigned)s_stats.avg_ms(), (unsigned)s_stats.worst_ms,
           (unsigned)WORST_ALLOWED_MS, (unsigned long)s_seed);
  lv_label_set_text(s_body, rep);
  lv_obj_align(s_body, LV_ALIGN_CENTER, 0, 0);
  Serial.printf(
      "ARC1 %lu EVT report pass=%d zones=%u/%u miss=%u avg_ms=%u worst_ms=%u "
      "seed=%lu\r\n",
      (unsigned long)now, pass ? 1 : 0, (unsigned)s_stats.zones_hit(ZONES),
      (unsigned)ZONES, (unsigned)s_stats.misses, (unsigned)s_stats.avg_ms(),
      (unsigned)s_stats.worst_ms, (unsigned long)s_seed);
}

void on_tap(int16_t x, int16_t y, uint32_t now) {
  switch (s_phase) {
    case Phase::Idle:
      start_round(now);
      return;
    case Phase::Playing: {
      if (target_contains(s_target, x, y)) {
        const uint32_t lat = now - s_target_shown_ms;
        s_stats.on_hit(s_target.zone, (uint16_t)(lat > 65535 ? 65535 : lat));
        Serial.printf("ARC1 %lu EVT hit zone=%u ms=%lu\r\n",
                      (unsigned long)now, (unsigned)s_target.zone,
                      (unsigned long)lat);
        s_step++;
        if (s_step >= s_plan.count) show_report(now);
        else                        place_target(now);
      } else {
        s_stats.on_miss();
        Serial.printf("ARC1 %lu EVT miss x=%d y=%d\r\n", (unsigned long)now,
                      (int)x, (int)y);
      }
      return;
    }
    case Phase::Report:
      show_idle();
      return;
  }
}

}  // namespace

void arcade_mode_setup() {
  boot_line("              .--------.");
  boot_line("              |  ^  ^  |        ARCADE (Canary Catch)");
  boot_line("              '--------'        the touch-QA suite in a costume");
  boot_separator();
  boot_kvf("Round", "%u zones (%ux%u grid), latency bar %u ms",
           (unsigned)ZONES, (unsigned)COLS, (unsigned)ROWS,
           (unsigned)WORST_ALLOWED_MS);
  boot_kv("Exit",  "hold the glass 3 s");
  boot_blank();

  s_display_ok = canary::hal::display_init();
  if (s_display_ok) s_display_ok = canary::ui::lvgl_port_init();
  if (s_display_ok) {
    build_face();
    show_idle();
    lv_timer_handler();
    canary::hal::backlight_set(255);
  }

  Serial.printf("ARC1 HELLO fw=%s zones=%u\r\n", CANARY_FW_VERSION,
                (unsigned)ZONES);
  boot_scene_ready(
      "Arcade up. One full round exercises every touch zone;",
      "the score screen is the factory report (ARC1 on this console).",
      NULL);
}

void arcade_mode_loop() {
  const uint32_t now = millis();

  const auto ts = canary::hal::touch_read();
  if (ts.touched && !s_touch_down) {
    s_touch_down = true;
    s_touch_down_ms = now;
    s_tx = ts.x;
    s_ty = ts.y;
  } else if (!ts.touched && s_touch_down) {
    s_touch_down = false;
    const uint32_t held = now - s_touch_down_ms;
    if (held >= 3000) {
      mode_exit_to_fleet();  // does not return
    }
    if (held < 600 && s_display_ok) on_tap(s_tx, s_ty, now);
  }

  if (s_display_ok) lv_timer_handler();
  delay(5);
}

}  // namespace mode
}  // namespace canary

#endif  // FEATURE_ARCADE && CD_FLAVOR_DASH
