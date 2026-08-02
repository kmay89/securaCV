// src/ui/portrait_ui.cpp — the Nightstand face (172x320 portrait), LVGL.
//
// Design contract (display_nightstand_line.md §2-§5): color is the primary
// language, the bird breathes and lives, a glance reads the room from bed.
// The layout, top to bottom:
//   - a severity color WASH, breathing, behind
//   - the living CANARY (canary_mark — the same host-tested engine the round
//     and dash faces use), full portrait pose
//   - the STATE WORD in the state's own hue
//   - the WITNESS COLUMN: one row per Canary, a color chip + room name, the
//     worst floating to the top (the narrow glass wants a vertical stack, not
//     a round halo)
//   - the GLANCE LINE: count + honest link state
//
// Motion is rationed the same way the rest of the family rations it: one slow
// wash breath, plus whatever the bird's own engine does. Night is handled at
// the theme choke point (character_set_night) + the backlight floor in
// main.cpp — this face just draws through col_*/sev_color, which already tell
// the truth after dark.
#include <config.h>
#ifdef CD_FLAVOR_NIGHTSTAND

#include <lvgl.h>
#include <stdio.h>

#include "canary/ui/portrait_ui.h"
#include "canary/ui/theme.h"
#include "canary/ui/canary_mark.h"
#include "canary/ui/character.h"
#include "canary/ui/look_state.h"
#if defined(FEATURE_LANTERN) && FEATURE_LANTERN
#include "canary/care/lantern.h"
#endif
#include "color/look_engine.h"

namespace canary::ui {

using canary::fleet::Fleet;
using canary::fleet::Sev;
using canary::fleet::Witness;

namespace {

// The narrow glass reads at most ~5 witness rows from across a room before
// they get too short; past that a muted "+N more" stands in (the worst are
// always in the visible set because the column is severity-ordered).
// (ROWS is derived below, after the glass geometry — the shorter Touch-1.69
// glass fits 4 rows where the 172x320 original fits its designed 5.)

// Geometry comes from the board's pin header, so every portrait board lays
// out correctly: the 1.47" boards are 172x320, the Touch-1.69 is 240x280.
// Vertical positions below are authored against the original 320-tall glass
// and scaled through V(), which is the identity on 172x320 — the shipped
// 1.47 layout is bit-for-bit unchanged.
#include "pins.h"
constexpr int SCR_W = TFT_WIDTH;
constexpr int SCR_H = TFT_HEIGHT;
constexpr int V(int y320) { return (y320 * SCR_H) / 320; }

// Witness rows that fit between the column top (V(172)) and the glance
// line's reserve, capped at the designed 5 — the formula reproduces exactly
// 5 on the original 172x320 glass and degrades gracefully on shorter ones.
constexpr int ROWS_FIT = (SCR_H - V(172) - 26) / 24;
constexpr int ROWS = ROWS_FIT < 5 ? (ROWS_FIT < 1 ? 1 : ROWS_FIT) : 5;

lv_obj_t* s_wash = nullptr;        // breathing severity field behind the bird
lv_obj_t* s_bird = nullptr;        // the living canary
lv_obj_t* s_state = nullptr;       // the state word, in the state's hue
lv_obj_t* s_row[ROWS] = {nullptr};
lv_obj_t* s_chip[ROWS] = {nullptr};
lv_obj_t* s_name[ROWS] = {nullptr};
lv_obj_t* s_more = nullptr;        // "+N more"
lv_obj_t* s_glance = nullptr;      // count + link honesty
lv_obj_t* s_lantern = nullptr;     // the night-light overlay (topmost)

lv_anim_t s_wash_anim;
uint32_t s_glance_bright_until = 0;   // ambient-life check-in window

lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* f, lv_color_t c) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, "");
  return l;
}

// The wash inhales and exhales forever — a slow field of the fleet's color
// that makes the whole pane feel alive. Only its opacity animates (cheap on
// the PSRAM-less C6); its color is set per-frame from the worst severity.
void wash_opa_cb(void* var, int32_t v) {
  lv_obj_set_style_bg_opa((lv_obj_t*)var, (lv_opa_t)v, LV_PART_MAIN);
}

void start_wash_breath() {
  if (!s_wash) return;
  lv_anim_init(&s_wash_anim);
  lv_anim_set_var(&s_wash_anim, s_wash);
  lv_anim_set_exec_cb(&s_wash_anim, wash_opa_cb);
  lv_anim_set_values(&s_wash_anim, LV_OPA_10, LV_OPA_40);
  lv_anim_set_time(&s_wash_anim, MOTION_BREATH_MS / 2);
  lv_anim_set_playback_time(&s_wash_anim, MOTION_BREATH_MS / 2);
  lv_anim_set_repeat_count(&s_wash_anim, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&s_wash_anim, lv_anim_path_ease_in_out);
  lv_anim_start(&s_wash_anim);
}

// Selection-sort the used witnesses by severity (worst first), keeping arrival
// order within a tier. count is small (<= CD_FLEET_MAX_DEVICES), so O(n^2) is
// free and avoids any allocation.
int order_by_severity(const Fleet& fleet, uint32_t now, int* out, int cap) {
  const int n = fleet.count();
  int m = 0;
  for (int i = 0; i < n && m < cap; i++) out[m++] = i;
  for (int a = 0; a < m; a++) {
    int best = a;
    Sev best_s = fleet.witness_sev(*fleet.at(out[a]), now);
    for (int b = a + 1; b < m; b++) {
      Sev s = fleet.witness_sev(*fleet.at(out[b]), now);
      if ((uint8_t)s > (uint8_t)best_s) { best = b; best_s = s; }
    }
    if (best != a) { int t = out[a]; out[a] = out[best]; out[best] = t; }
  }
  return m;
}

const char* state_word(Sev s) {
  switch (s) {
    case Sev::Warn:   return "wants a look";
    case Sev::Alert:  return "needs you";
    case Sev::Tamper: return "tamper";
    default:          return "all quiet";
  }
}

}  // namespace

void portrait_ui_create() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, col_bg(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // ── The wash (created first so everything draws on top of it) ──
  // A soft rounded field in the top two-thirds: the severity color at the
  // top fading into the ground, so the bird sits inside a halo of the
  // fleet's mood rather than in front of a hard band.
  s_wash = lv_obj_create(scr);
  lv_obj_set_size(s_wash, SCR_W + 40, V(200));
  lv_obj_align(s_wash, LV_ALIGN_TOP_MID, 0, V(-24));
  lv_obj_set_style_radius(s_wash, 120, 0);
  lv_obj_set_style_border_width(s_wash, 0, 0);
  lv_obj_set_style_pad_all(s_wash, 0, 0);
  lv_obj_clear_flag(s_wash, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_wash, sev_color(Sev::Ok, false), 0);
  lv_obj_set_style_bg_grad_color(s_wash, col_bg(), 0);
  lv_obj_set_style_bg_grad_dir(s_wash, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(s_wash, LV_OPA_30, 0);
  start_wash_breath();

  // ── The living canary ──
  // 108 px of bird in the top band — the vertical room the round face never
  // had. The mark's own engine breathes, blinks, and reacts.
  s_bird = canary_mark_create(scr, V(108));
  if (s_bird) lv_obj_align(s_bird, LV_ALIGN_TOP_MID, 0, V(22));

  // ── The state word, in the state's own hue ──
  s_state = mk_label(scr, font_title(), sev_color(Sev::Ok, false));
  lv_obj_set_style_text_align(s_state, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_state, LV_ALIGN_TOP_MID, 0, V(138));
  lv_label_set_text(s_state, "all quiet");

  // ── The witness column ──
  const int col_top = V(172);
  const int row_h = 22;
  const int row_gap = 2;
  for (int i = 0; i < ROWS; i++) {
    lv_obj_t* row = lv_obj_create(scr);
    lv_obj_set_size(row, SCR_W - 20, row_h);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, col_top + i * (row_h + row_gap));
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_bg_color(row, col_surface(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);   // shown only when in use
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* chip = lv_obj_create(row);
    lv_obj_set_size(chip, 10, 10);
    lv_obj_align(chip, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_pad_all(chip, 0, 0);
    lv_obj_set_style_bg_color(chip, sev_color(Sev::Ok, false), 0);

    lv_obj_t* name = mk_label(row, font_body(), col_text());
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 26, 0);

    s_row[i] = row;
    s_chip[i] = chip;
    s_name[i] = name;
  }

  s_more = mk_label(scr, font_caption(), col_faint());
  lv_obj_set_style_text_align(s_more, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_more, LV_ALIGN_TOP_MID, 0, col_top + ROWS * (row_h + row_gap) + 2);
  lv_label_set_text(s_more, "");

  // ── The glance line ──
  s_glance = mk_label(scr, font_body(), col_muted());
  lv_obj_set_style_text_align(s_glance, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_glance, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_label_set_text(s_glance, "");

#if defined(FEATURE_LANTERN) && FEATURE_LANTERN
  // ── The lantern overlay ──
  // Created last so it covers the face: on these small boards the screen IS
  // the night light. Hidden until summoned; the honest veto in the model
  // takes the glass back the instant anything wants attention.
  s_lantern = lv_obj_create(scr);
  lv_obj_set_size(s_lantern, SCR_W, SCR_H);
  lv_obj_set_pos(s_lantern, 0, 0);
  lv_obj_set_style_radius(s_lantern, 0, 0);
  lv_obj_set_style_border_width(s_lantern, 0, 0);
  lv_obj_set_style_pad_all(s_lantern, 0, 0);
  lv_obj_set_style_bg_opa(s_lantern, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_grad_dir(s_lantern, LV_GRAD_DIR_VER, 0);
  lv_obj_clear_flag(s_lantern, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_lantern, LV_OBJ_FLAG_HIDDEN);
#endif
}

void portrait_ui_update(const Fleet& fleet, uint32_t now,
                        const PortraitState& st) {
  if (!s_wash) return;
  const bool night = st.night;
  const Sev worst = fleet.worst(now);
  const int count = fleet.count();

  // The bird wears the mood the engine chose this pass.
  canary_mark_mood(st.bird);

  // The wash is the glass half of the look engine: the chosen scene's gradient
  // when the fleet is calm, the true semantic color the instant anything needs
  // attention (worst >= Warn, handled inside wash_stops). Top and bottom stops
  // drive LVGL's vertical gradient; the breathing opacity animation (started in
  // create) gives it life. The beacon runs the same engine, so pane and point
  // of light always agree.
  {
    auto& lp = canary::ui::look_params();
    lp.night = night;
    canary::color::Rgb ws[2];
    canary::color::wash_stops(now, lp, (canary::color::Sev)(uint8_t)worst,
                              /*safe_dark=*/false, ws, 2);
    lv_obj_set_style_bg_color(s_wash, lv_color_make(ws[0].r, ws[0].g, ws[0].b), 0);
    lv_obj_set_style_bg_grad_color(s_wash, lv_color_make(ws[1].r, ws[1].g, ws[1].b), 0);
  }

  // State word + hue.
  lv_label_set_text(s_state, state_word(worst));
  lv_obj_set_style_text_color(s_state, sev_color(worst, night), 0);

  // Witness column, severity-ordered.
  int order[CD_FLEET_MAX_DEVICES];
  const int m = order_by_severity(fleet, now, order,
                                  (int)(sizeof(order) / sizeof(order[0])));
  const int shown = m < ROWS ? m : ROWS;
  for (int i = 0; i < ROWS; i++) {
    if (i < shown) {
      const Witness* w = fleet.at(order[i]);
      const Sev s = w ? fleet.witness_sev(*w, now) : Sev::Ok;
      const bool attention = (uint8_t)s >= (uint8_t)Sev::Warn;
      lv_obj_set_style_bg_opa(s_row[i], attention ? LV_OPA_10 : LV_OPA_0, 0);
      lv_obj_set_style_bg_color(s_chip[i], sev_color(s, night), 0);
      lv_label_set_text(s_name[i], w ? Fleet::display_name(*w) : "-");
      lv_obj_set_style_text_color(s_name[i],
                                  attention ? col_text() : col_muted(), 0);
      lv_obj_clear_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (m > ROWS) {
    lv_label_set_text_fmt(s_more, "+%d more", m - ROWS);
    lv_obj_clear_flag(s_more, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_more, LV_OBJ_FLAG_HIDDEN);
  }

  // Glance line: count, and — honesty first — a dead link is never hidden.
  const bool link_down = !st.wifi_ok || !st.mqtt_ok;
  if (link_down) {
    lv_label_set_text(s_glance,
                      !st.wifi_ok
                          ? (st.wifi_reason ? st.wifi_reason : "wifi down")
                          : "broker down");
    lv_obj_set_style_text_color(s_glance, sev_color(Sev::Warn, night), 0);
  } else if (count == 0) {
    lv_label_set_text(s_glance, "no canaries yet");
    lv_obj_set_style_text_color(s_glance, col_muted(), 0);
  } else {
    // An ambient-life check-in brightens this line for a few seconds, then
    // it settles back to muted — the whole visible half of a "moment".
    const bool bright = (int32_t)(now - s_glance_bright_until) < 0;
    lv_label_set_text_fmt(s_glance, "%d %s \xC2\xB7 online", count,
                          count == 1 ? "canary" : "canaries");
    lv_obj_set_style_text_color(s_glance, bright ? col_text() : col_muted(), 0);
  }

#if defined(FEATURE_LANTERN) && FEATURE_LANTERN
  // ── The lantern ──
  // A lamp, not a state signal: its light is the chosen look-engine scene at
  // day brightness (night dimming belongs to the state channel, not to a
  // light somebody just asked for). The model has already yielded on any
  // Warn+ or dead link, so reaching here means the room is genuinely calm.
  if (s_lantern) {
    auto& lamp = canary::care::lantern();
    const bool attention = (uint8_t)worst >= (uint8_t)Sev::Warn || link_down;
    if (lamp.active(now, night, attention)) {
      canary::color::LookParams lampp = look_params();
      lampp.scene_idx = lamp.scene();
      lampp.night = false;
      canary::color::Rgb ws[2];
      canary::color::wash_stops(now, lampp, canary::color::Sev::Ok,
                                /*safe_dark=*/false, ws, 2);
      lv_obj_set_style_bg_color(s_lantern,
                                lv_color_make(ws[0].r, ws[0].g, ws[0].b), 0);
      lv_obj_set_style_bg_grad_color(
          s_lantern, lv_color_make(ws[1].r, ws[1].g, ws[1].b), 0);
      lv_obj_clear_flag(s_lantern, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_lantern, LV_OBJ_FLAG_HIDDEN);
    }
  }
#endif
}

void portrait_ui_ack_hold(bool /*active*/) {
  // The BOOT-button acknowledge (main.cpp) is deliberate rather than swept:
  // there is no finger on the glass to track, so a sweep ring would animate
  // against nothing. Kept for main.cpp call symmetry.
}

void portrait_ui_life_glance(uint32_t now) {
  s_glance_bright_until = now + 6000;
}

}  // namespace canary::ui

#endif  // CD_FLAVOR_NIGHTSTAND
