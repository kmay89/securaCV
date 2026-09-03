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
// Not compiled on the Nightlight (CD_NIGHTLIGHT): that build shares this
// flavor's HAL and modal surfaces but swaps the standing face for
// nightlight_ui.cpp — the same arrangement dash_ui has with CD_NIGHTSTAND7.
#include "flavor_config.h"
#if defined(CD_FLAVOR_NIGHTSTAND) && !defined(CD_NIGHTLIGHT)

#include <lvgl.h>
#include <stdio.h>

#include "portrait_ui.h"
#include "theme.h"
#include "canary_mark.h"
#include "character.h"
#include "look_state.h"
#include "settings_ui.h"
#if defined(FEATURE_LANTERN) && FEATURE_LANTERN
#include "lantern.h"
#include "hallway.h"
#endif
#include "color/look_engine.h"
#include "color/plumage.h"

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

// The flagship AMOLED (450x600) shows this same face at ~2x the pixel pitch
// (~310 ppi), so the row metrics scale UP on any glass taller than the
// designed 320 — the witness column keeps its physical size and the bigger
// type ladder (character.cpp) has room to breathe. Every shipped small
// glass takes the literals it always had (BIG is false there, bit-for-bit).
constexpr bool BIG = SCR_H > 320;
constexpr int ROW_H    = BIG ? (22 * SCR_H) / 320 : 22;
constexpr int ROW_GAP  = BIG ? (2 * SCR_H) / 320 : 2;
constexpr int ROW_W    = BIG ? SCR_W - (20 * SCR_W) / 172 : SCR_W - 20;
constexpr int ROW_RAD  = BIG ? 12 : 6;
constexpr int CHIP_SZ  = BIG ? (10 * SCR_H) / 320 : 10;
constexpr int CHIP_X   = BIG ? (8 * SCR_W) / 172 : 8;
constexpr int NAME_X   = BIG ? (26 * SCR_W) / 172 : 26;

// Witness rows that fit between the column top (V(172)) and the glance
// line's reserve. The small glasses keep the designed cap of 5; a BIG glass
// may grow to 8 — but only as far as its scaled rows actually FIT, which on
// the 450x600 AMOLED is 5 full-size rows (the proportional face keeps its
// physical row height rather than shrinking type to force a count). The
// formula reproduces exactly 5 on the original 172x320 glass, degrades
// gracefully on shorter ones, and lets a future even-taller glass use its
// room without another edit here.
constexpr int ROWS_FIT = (SCR_H - V(172) - (BIG ? V(26) : 26)) / (ROW_H + ROW_GAP);
constexpr int ROWS_CAP = BIG ? 8 : 5;
constexpr int ROWS = ROWS_FIT < ROWS_CAP ? (ROWS_FIT < 1 ? 1 : ROWS_FIT) : ROWS_CAP;

lv_obj_t* s_wash = nullptr;        // breathing severity field behind the bird
lv_obj_t* s_bird = nullptr;        // the living canary
lv_obj_t* s_state = nullptr;       // the state word, in the state's hue
lv_obj_t* s_row[ROWS] = {nullptr};
lv_obj_t* s_chip[ROWS] = {nullptr};
lv_obj_t* s_name[ROWS] = {nullptr};
lv_obj_t* s_more = nullptr;        // "+N more"
lv_obj_t* s_glance = nullptr;      // count + link honesty
#if defined(FEATURE_TOUCH) && FEATURE_TOUCH
// The settings doorway, top-right by day (touch glass only). This face used
// to have NO touch path to the settings panel at all — the Touch-1.69 and
// the AMOLED carried the whole panel and no way to open it.
lv_obj_t* s_gear = nullptr;
constexpr int GEAR_W = BIG ? 150 : 60;   // the hit zone: a thumb, not a cursor
constexpr int GEAR_H = BIG ? 56 : 40;
#endif

#if defined(FEATURE_LANTERN) && FEATURE_LANTERN
// ── The lamp's glass ──
// The night light used to be one object with a two-stop vertical gradient,
// which is a fade, not a pattern. It is now a STACK of bands, each carrying
// its own two-stop gradient to the next band's color: piecewise-linear across
// the stack, so it still reads as one smooth field, but with enough
// independent stops that the plumage engine (color/plumage.h) can move a
// glow through it. That traveling glow is the whole point — a phrase of
// light climbing the pane.
//
// LANTERN_BANDS is a budget, not a resolution: 14 bands on a 320-tall glass
// is ~23 px each, past the point where a band edge is visible through a
// gradient, and it costs 14 style writes a frame. The C6 renders this
// single-buffered and still keeps up because nothing here reflows — only
// colors change.
constexpr int LANTERN_BANDS = 14;
lv_obj_t* s_lantern = nullptr;                  // the overlay container
lv_obj_t* s_band[LANTERN_BANDS] = {nullptr};    // its bands, top to bottom
#endif

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
  for (int i = 0; i < ROWS; i++) {
    lv_obj_t* row = lv_obj_create(scr);
    lv_obj_set_size(row, ROW_W, ROW_H);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, col_top + i * (ROW_H + ROW_GAP));
    lv_obj_set_style_radius(row, ROW_RAD, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_bg_color(row, col_surface(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);   // shown only when in use
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* chip = lv_obj_create(row);
    lv_obj_set_size(chip, CHIP_SZ, CHIP_SZ);
    lv_obj_align(chip, LV_ALIGN_LEFT_MID, CHIP_X, 0);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_pad_all(chip, 0, 0);
    lv_obj_set_style_bg_color(chip, sev_color(Sev::Ok, false), 0);

    lv_obj_t* name = mk_label(row, font_body(), col_text());
    lv_obj_align(name, LV_ALIGN_LEFT_MID, NAME_X, 0);

    s_row[i] = row;
    s_chip[i] = chip;
    s_name[i] = name;
  }

  s_more = mk_label(scr, font_caption(), col_faint());
  lv_obj_set_style_text_align(s_more, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_more, LV_ALIGN_TOP_MID, 0, col_top + ROWS * (ROW_H + ROW_GAP) + 2);
  lv_label_set_text(s_more, "");

  // ── The glance line ──
  s_glance = mk_label(scr, font_body(), col_muted());
  lv_obj_set_style_text_align(s_glance, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_glance, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_label_set_text(s_glance, "");

#if defined(FEATURE_TOUCH) && FEATURE_TOUCH
  // ── The settings doorway ──
  // A quiet gear in the top-right corner, clear of the bird (108 px wide,
  // centered). The big glass spells it out; the 240 stick keeps the glyph.
  // Day only — update() hides it after dark, when the glass is a lamp.
  s_gear = mk_label(scr, BIG ? font_caption() : font_label(), col_faint());
  lv_label_set_text(s_gear, BIG ? LV_SYMBOL_SETTINGS " settings"
                                : LV_SYMBOL_SETTINGS);
  lv_obj_align(s_gear, LV_ALIGN_TOP_RIGHT, BIG ? -16 : -12, BIG ? 14 : 12);
#endif

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
  lv_obj_clear_flag(s_lantern, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_lantern, LV_OBJ_FLAG_HIDDEN);

  // The bands. Heights are apportioned so they tile the glass exactly with no
  // seam and no overhang, whatever SCR_H and LANTERN_BANDS are: each band
  // starts where the previous one ended, computed from the SAME rounding, so
  // a remainder can never leave a dark line across the lamp.
  for (int i = 0; i < LANTERN_BANDS; i++) {
    const int y0 = (SCR_H * i) / LANTERN_BANDS;
    const int y1 = (SCR_H * (i + 1)) / LANTERN_BANDS;
    lv_obj_t* b = lv_obj_create(s_lantern);
    lv_obj_set_size(b, SCR_W, y1 - y0);
    lv_obj_set_pos(b, 0, y0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    s_band[i] = b;
  }
#endif
}

void portrait_ui_update(const Fleet& fleet, uint32_t now,
                        const PortraitState& st) {
  if (!s_wash) return;
  const bool night = st.night;
  const Sev worst = fleet.worst(now);
  const int count = fleet.count();

#if defined(FEATURE_TOUCH) && FEATURE_TOUCH
  // The doorway keeps daylight hours: after dark the column is a clock and
  // a lamp, and a stray tap must stay the peek it already was.
  if (s_gear) {
    if (night) lv_obj_add_flag(s_gear, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_clear_flag(s_gear, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(s_gear, col_faint(), 0);
  }
#endif

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
    lv_label_set_text_fmt(s_glance, "%d %s \xE2\x80\xA2 online", count,
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
    const bool lit = lamp.active(now, night, attention);

    // The song only runs while the lamp is genuinely lit. `gated` HOLDS the
    // phrase clock rather than banking it, so a lamp that has been off all day
    // does not come on and immediately start talking.
    //
    // This is the ONLY tick in the firmware: the beacon reads the same shared
    // song without advancing it, so the point of light and the pane are always
    // mid-way through the same syllable.
    auto& sng = canary::ui::song();
    sng.tick(now, night, /*gated=*/!lit);

    if (lit) {
      canary::color::LookParams lampp = look_params();
      lampp.scene_idx = lamp.scene();
      // Night dimming belongs to the STATE channel, not to a light somebody
      // asked for — so the lamp is rendered at day brightness and its level is
      // set by Hallway mode's own dwell instead.
      lampp.night = false;

      // Hallway mode's rise/hold/ebb envelope. When it is off (the default,
      // i.e. an ordinary summoned lantern) the preset does not apply and the
      // lamp burns at the look's own brightness for its timed window.
      uint8_t depth = 0;
      auto& hall = canary::care::hallway();
      if (hall.enabled() && st.night_elapsed_min + st.night_remaining_min > 0) {
        const uint8_t dwell =
            hall.level(st.night_elapsed_min, st.night_remaining_min, attention);
        lampp.brightness =
            (uint8_t)((uint32_t)hall.preset().brightness * dwell / 255u);
        lampp.warmth = hall.preset().warmth;
        // The song is scaled by the dwell too, so it fades in with the lamp
        // rather than speaking at full volume into a room that is still dark.
        depth = (uint8_t)((uint32_t)hall.preset().depth * dwell / 255u);
      }

      // The field: the resting scene wash, with the phrase's glow riding up
      // through it. plumage_bands hands Warn+/safe_dark straight back to the
      // look engine, so the honest paths are the same code they always were.
      canary::color::Rgb ws[LANTERN_BANDS + 1];
      canary::color::plumage_bands(now, lampp, canary::color::Sev::Ok,
                                   /*safe_dark=*/false, sng, depth, ws,
                                   LANTERN_BANDS + 1);
      for (int i = 0; i < LANTERN_BANDS; i++) {
        if (!s_band[i]) continue;
        // Each band runs from its own stop to the next one's, so the seams
        // between bands are continuous and the stack reads as one field.
        lv_obj_set_style_bg_color(
            s_band[i], lv_color_make(ws[i].r, ws[i].g, ws[i].b), 0);
        lv_obj_set_style_bg_grad_color(
            s_band[i], lv_color_make(ws[i + 1].r, ws[i + 1].g, ws[i + 1].b), 0);
      }
      lv_obj_clear_flag(s_lantern, LV_OBJ_FLAG_HIDDEN);

      // Publish this frame for the beacon. The LED joins the lamp only under
      // Hallway mode's explicit opt-in (see care/hallway.h); a plain summoned
      // lantern leaves the beacon a pure attention channel.
      auto& lf = canary::ui::lamp_frame();
      lf.lit = true;
      lf.beacon = hall.beacon();
      lf.depth = depth;
      lf.look = lampp;
    } else {
      lv_obj_add_flag(s_lantern, LV_OBJ_FLAG_HIDDEN);
      canary::ui::lamp_frame().lit = false;
    }
  }
#endif
}

bool portrait_ui_handle_tap(int16_t x, int16_t y) {
#if defined(FEATURE_TOUCH) && FEATURE_TOUCH
  // The gear corner, day only. The hit zone is deliberately larger than the
  // glyph — a thumb, not a cursor.
  if (s_gear && !character_night() && x >= SCR_W - GEAR_W && y <= GEAR_H) {
    settings_ui_open();
    return true;
  }
#else
  (void)x;
  (void)y;
#endif
  return false;
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
