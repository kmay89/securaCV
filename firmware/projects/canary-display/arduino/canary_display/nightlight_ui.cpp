// src/ui/nightlight_ui.cpp — the Canary Nightlight face (C3 1.47" portrait),
// LVGL. See include/canary/ui/nightlight_ui.h for the layer contract.
//
// Layout, top to bottom on the 180x320 glass (geometry-parametric via
// pins.h, like every portrait face):
//   - the GLANCE LINE: the honest channel — link trouble, an unsynced
//     clock, a fleet condition. Hidden only when there is nothing to say.
//   - the CLOCK: HH:MM in drawn seven-segment digits (the same rounded
//     mk_seg grammar the Nightstand 7 uses, scaled to a pocket glass).
//     Ghost segments by day, digits-only at night, a breathing colon.
//   - the CAPTION: the companion's few words during a visit.
//   - the COMPANION: the living canary (canary_mark — its face belongs to
//     the mood engine), perched low; a visit lifts it to center stage for
//     a few seconds, then hands the clock back.
//   - the LAMP, behind everything: a banded look-engine field (the same
//     piecewise-gradient stack the nightstand's lantern draws), carrying
//     the chosen scene — warm Lantern orange, Rainbow, Moonbeam white —
//     at lamp strength when lit, as a soft ambient wash otherwise.
#include "flavor_config.h"
#ifdef CD_NIGHTLIGHT

#include <lvgl.h>
#include <stdio.h>

#include "nightlight_ui.h"
#include "theme.h"
#include "canary_mark.h"
#include "character.h"
#include "look_state.h"
#include "lantern.h"
#include "nightlight_glue.h"
#include "color/look_engine.h"
#include "color/plumage.h"

namespace canary::ui {

using canary::care::Visit;
using canary::care::VisitKind;
using canary::fleet::Fleet;
using canary::fleet::Sev;

namespace {

#include "pins.h"
constexpr int SCR_W = TFT_WIDTH;
constexpr int SCR_H = TFT_HEIGHT;
constexpr int V(int y320) { return (y320 * SCR_H) / 320; }

// ── The lamp field ──
// Same band budget as the nightstand's lantern: 14 bands tile a 320-tall
// glass at ~23 px each — past the point where a band edge shows through a
// gradient, and cheap enough for the PSRAM-less C3 (colors change, nothing
// reflows).
constexpr int NL_BANDS = 14;
lv_obj_t* s_band[NL_BANDS] = {nullptr};

// ── The clock ──
struct SegDigit {
  lv_obj_t* seg[7] = {nullptr};   // A B C D E F G
};
constexpr uint8_t DIGIT_MAP[10] = {
    0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
    0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111,
};
// Digit geometry, solved from the glass width: [m][D][g][D][colon][D][g][D][m].
constexpr int C_MARGIN = 6;
constexpr int C_GAP    = 4;
constexpr int C_COLON  = 16;
constexpr int DIG_W = (SCR_W - 2 * C_MARGIN - C_COLON - 2 * C_GAP) / 4;
constexpr int DIG_H = V(64);
constexpr int DIG_T = DIG_W / 5 < 6 ? 6 : DIG_W / 5;
constexpr int CLOCK_Y = V(56);

lv_obj_t* s_clock = nullptr;          // one container so one opa dims it all
SegDigit s_digit[4];
lv_obj_t* s_colon[2] = {nullptr};
lv_obj_t* s_date = nullptr;

lv_obj_t* s_glance = nullptr;         // the honest line (top)
lv_obj_t* s_caption = nullptr;        // the companion's few words
lv_obj_t* s_bird = nullptr;

uint32_t s_caption_bright_until = 0;  // ambient-life check-in window
VisitKind s_staged = VisitKind::None; // the visit currently on stage

lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* f, lv_color_t c) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, "");
  return l;
}

lv_obj_t* mk_seg(lv_obj_t* parent, int x, int y, int w, int h) {
  lv_obj_t* s = lv_obj_create(parent);
  lv_obj_set_size(s, w, h);
  lv_obj_set_pos(s, x, y);
  lv_obj_set_style_radius(s, (w < h ? w : h) / 2, 0);
  lv_obj_set_style_border_width(s, 0, 0);
  lv_obj_set_style_pad_all(s, 0, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

lv_obj_t* mk_digit(lv_obj_t* parent, SegDigit* d, int x, int y, int w, int h,
                   int t) {
  lv_obj_t* box = lv_obj_create(parent);
  lv_obj_set_size(box, w, h);
  lv_obj_set_pos(box, x, y);
  lv_obj_set_style_bg_opa(box, LV_OPA_0, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  const int vh = (h - 3 * t) / 2;
  d->seg[0] = mk_seg(box, t, 0, w - 2 * t, t);                    // A
  d->seg[1] = mk_seg(box, w - t, t, t, vh);                       // B
  d->seg[2] = mk_seg(box, w - t, 2 * t + vh, t, vh);              // C
  d->seg[3] = mk_seg(box, t, 2 * (t + vh), w - 2 * t, t);         // D
  d->seg[4] = mk_seg(box, 0, 2 * t + vh, t, vh);                  // E
  d->seg[5] = mk_seg(box, 0, t, t, vh);                           // F
  d->seg[6] = mk_seg(box, t, t + vh, w - 2 * t, t);               // G
  return box;
}

// value 0-9, or -1 = blank. Ghost segments keep the instrument's resting
// shape by day and vanish at night (a dark room wants digits, not
// scaffolding) — the Nightstand 7's rule, kept.
void set_digit(SegDigit* d, int value, lv_color_t color, bool night) {
  const uint8_t map = (value >= 0 && value <= 9) ? DIGIT_MAP[value] : 0;
  for (int i = 0; i < 7; i++) {
    if (!d->seg[i]) continue;
    lv_obj_set_style_bg_color(d->seg[i], color, 0);
    const bool lit = (map >> i) & 1;
    lv_obj_set_style_bg_opa(
        d->seg[i], lit ? LV_OPA_COVER : (night ? LV_OPA_0 : LV_OPA_10), 0);
  }
}

// The bird's two stage marks: its perch (low, beside the night) and center
// stage (a visit). Moved with a translate so the align anchor never changes.
constexpr int BIRD_LIFT = -1 * ((SCR_H * 66) / 320);

void stage_bird(bool up) {
  if (!s_bird) return;
  lv_obj_set_style_translate_y(s_bird, up ? BIRD_LIFT : 0, 0);
}

}  // namespace

void nightlight_ui_create() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, col_bg(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // ── The lamp field, created first so everything draws over it ──
  // Bands tile the glass exactly, each running its own two-stop gradient to
  // the next band's color (the nightstand lantern's seamless-stack rule).
  for (int i = 0; i < NL_BANDS; i++) {
    const int y0 = (SCR_H * i) / NL_BANDS;
    const int y1 = (SCR_H * (i + 1)) / NL_BANDS;
    lv_obj_t* b = lv_obj_create(scr);
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

  // ── The glance line (the honest channel) ──
  s_glance = mk_label(scr, font_caption(), col_muted());
  lv_obj_set_style_text_align(s_glance, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_glance, LV_ALIGN_TOP_MID, 0, V(10));
  lv_label_set_text(s_glance, "");

  // ── The clock ──
  s_clock = lv_obj_create(scr);
  lv_obj_set_size(s_clock, SCR_W, DIG_H + 4);
  lv_obj_set_pos(s_clock, 0, CLOCK_Y);
  lv_obj_set_style_bg_opa(s_clock, LV_OPA_0, 0);
  lv_obj_set_style_border_width(s_clock, 0, 0);
  lv_obj_set_style_pad_all(s_clock, 0, 0);
  lv_obj_clear_flag(s_clock, LV_OBJ_FLAG_SCROLLABLE);

  int x = C_MARGIN;
  for (int i = 0; i < 4; i++) {
    mk_digit(s_clock, &s_digit[i], x, 2, DIG_W, DIG_H, DIG_T);
    x += DIG_W;
    if (i == 1) {
      for (int c = 0; c < 2; c++) {
        s_colon[c] = mk_seg(s_clock, x + (C_COLON - DIG_T) / 2,
                            2 + (c == 0 ? DIG_H / 3 - DIG_T / 2
                                        : 2 * DIG_H / 3 - DIG_T / 2),
                            DIG_T, DIG_T);
      }
      x += C_COLON;
    } else if (i != 3) {
      x += C_GAP;
    }
  }

  s_date = mk_label(scr, font_caption(), col_muted());
  lv_obj_set_style_text_align(s_date, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, CLOCK_Y + DIG_H + V(14));
  lv_label_set_text(s_date, "");

  // ── The companion's words ──
  s_caption = mk_label(scr, font_body(), col_muted());
  lv_obj_set_style_text_align(s_caption, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_caption, LV_ALIGN_TOP_MID, 0, V(178));
  lv_label_set_text(s_caption, "");

  // ── The companion ──
  s_bird = canary_mark_create(scr, V(96));
  if (s_bird) lv_obj_align(s_bird, LV_ALIGN_BOTTOM_MID, 0, V(-22));
  s_staged = VisitKind::None;
}

void nightlight_ui_update(const Fleet& fleet, uint32_t now,
                          const NightlightState& st) {
  if (!s_clock) return;
  const bool night = st.night;
  const Sev worst = fleet.worst(now);
  // The lamp never encodes link state on this flavor (the glance line does);
  // only a real fleet condition takes it. Same rule as main.cpp's
  // lantern_lit — the two must agree or the backlight and the field argue.
  const bool attention = (uint8_t)worst >= (uint8_t)Sev::Warn;

  // The bird wears the mood engine's face, always.
  canary_mark_mood(st.bird);

  // ── The lamp field ──
  {
    auto& lamp = canary::care::lantern();
    const bool lit = lamp.active(now, night, attention);

    // The lamp's voice: only ever ticked while genuinely lit (the beacon-
    // less C3 has no second reader, but the shared-song discipline holds).
    auto& sng = canary::ui::song();
    sng.tick(now, night, /*gated=*/!lit);

    canary::color::LookParams lampp = look_params();
    lampp.brightness = canary::care::nightlight_lamp_bri();
    if (lit) {
      lampp.scene_idx = lamp.scene();
      lampp.night = false;   // the lamp burns at its own strength; dimming
                             // belongs to the backlight's night floor
    } else {
      // Unlit: the same scene as a soft ambient wash behind the clock —
      // roughly a third of the lamp's strength, so day reads as a tinted
      // glass, not a lit lamp.
      lampp.night = night;
      lampp.brightness = (uint8_t)(((uint16_t)lampp.brightness * 85) / 255);
    }

    // A night Moonwatch visit swells the field gently — the one visit that
    // moves light instead of the bird.
    uint8_t depth = 0;
    if (st.visit.kind == VisitKind::Moonwatch && lit) depth = 96;

    // plumage_bands hands Warn+ straight to the look engine's semantic
    // override, so an attention field is the true amber/red — the honest
    // paths are the same code they always were.
    canary::color::Rgb ws[NL_BANDS + 1];
    canary::color::plumage_bands(now, lampp, (canary::color::Sev)(uint8_t)worst,
                                 /*safe_dark=*/false, sng, depth, ws,
                                 NL_BANDS + 1);
    for (int i = 0; i < NL_BANDS; i++) {
      if (!s_band[i]) continue;
      lv_obj_set_style_bg_color(
          s_band[i], lv_color_make(ws[i].r, ws[i].g, ws[i].b), 0);
      lv_obj_set_style_bg_grad_color(
          s_band[i], lv_color_make(ws[i + 1].r, ws[i + 1].g, ws[i + 1].b), 0);
    }
  }

  // ── The clock ──
  {
    const lv_color_t c = col_text();
    int d0 = -1, d1 = -1, d2 = -1, d3 = -1;
    if (st.time_valid) {
      int hh = st.clock_hh;
      if (canary::care::nightlight_clock_12h()) {
        hh = hh % 12;
        if (hh == 0) hh = 12;
      }
      d0 = hh / 10;
      d1 = hh % 10;
      d2 = st.clock_mm / 10;
      d3 = st.clock_mm % 10;
      if (d0 == 0 && canary::care::nightlight_clock_12h()) d0 = -1;
    }
    set_digit(&s_digit[0], d0, c, night);
    set_digit(&s_digit[1], d1, c, night);
    set_digit(&s_digit[2], d2, c, night);
    set_digit(&s_digit[3], d3, c, night);

    // The colon breathes by day (alive), holds steady at night (a dark
    // room wants stillness). While the clock is unsynced it pulses slowly —
    // the face's "thinking" tell, matched by the glance line's words.
    lv_opa_t colon_opa = LV_OPA_COVER;
    if (!st.time_valid) {
      colon_opa = ((now / 1000) & 1) ? LV_OPA_40 : LV_OPA_10;
    } else if (!night) {
      colon_opa = ((now / 1000) & 1) ? LV_OPA_COVER : LV_OPA_30;
    }
    for (int i = 0; i < 2; i++) {
      if (!s_colon[i]) continue;
      lv_obj_set_style_bg_color(s_colon[i], c, 0);
      lv_obj_set_style_bg_opa(s_colon[i], colon_opa, 0);
    }

    // A visit dims the clock to a supporting role; the stage comes back
    // with the digits.
    const bool visiting = st.visit.kind != VisitKind::None &&
                          st.visit.kind != VisitKind::Moonwatch;
    lv_obj_set_style_opa(s_clock, visiting ? LV_OPA_40 : LV_OPA_COVER, 0);
    if (s_date) lv_obj_set_style_opa(s_date, visiting ? LV_OPA_40 : LV_OPA_COVER, 0);
  }

  // ── The companion's staging ──
  {
    const VisitKind k = st.visit.kind;
    if (k != s_staged) {
      s_staged = k;
      // Moonwatch never moves the bird (asleep is asleep); every other
      // visit lifts it to center stage.
      stage_bird(k != VisitKind::None && k != VisitKind::Moonwatch);
    }
    const bool bright = (int32_t)(now - s_caption_bright_until) < 0;
    if (k != VisitKind::None) {
      lv_label_set_text(s_caption, canary::care::visit_caption(k));
      lv_obj_set_style_text_color(s_caption, col_text(), 0);
    } else if (bright) {
      // Ambient-life check-in: the caption line carries the quiet hello.
      lv_label_set_text(s_caption, "here with you");
      lv_obj_set_style_text_color(s_caption, col_muted(), 0);
    } else {
      lv_label_set_text(s_caption, "");
    }
  }

  // ── The date line ──
  if (s_date) {
    if (st.time_valid && st.wday >= 0 && st.mday > 0 && st.mon > 0) {
      static const char* kWd[7] = {"sun", "mon", "tue", "wed",
                                   "thu", "fri", "sat"};
      static const char* kMo[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                    "jul", "aug", "sep", "oct", "nov", "dec"};
      lv_label_set_text_fmt(s_date, "%s \xC2\xB7 %s %d", kWd[st.wday % 7],
                            kMo[(st.mon - 1) % 12], st.mday);
    } else {
      lv_label_set_text(s_date, "");
    }
  }

  // ── The glance line (honesty; never hidden while something is true) ──
  {
    const bool wifi_bad = !st.wifi_ok;
    const bool broker_bad = !st.mqtt_ok && !st.standalone;
    if ((uint8_t)worst >= (uint8_t)Sev::Warn) {
      lv_label_set_text(s_glance,
                        worst >= Sev::Tamper ? "tamper" :
                        worst >= Sev::Alert  ? "a canary needs you"
                                             : "a canary wants a look");
      lv_obj_set_style_text_color(s_glance, sev_color(worst, night), 0);
    } else if (wifi_bad) {
      lv_label_set_text(s_glance,
                        st.wifi_reason ? st.wifi_reason : "wifi down");
      lv_obj_set_style_text_color(s_glance, sev_color(Sev::Warn, night), 0);
    } else if (broker_bad) {
      lv_label_set_text(s_glance, "hub down");
      lv_obj_set_style_text_color(s_glance, sev_color(Sev::Warn, night), 0);
    } else if (!st.time_valid) {
      lv_label_set_text(s_glance, "setting its clock...");
      lv_obj_set_style_text_color(s_glance, col_muted(), 0);
    } else {
      lv_label_set_text(s_glance, "");
    }
  }
}

void nightlight_ui_ack_hold(bool /*active*/) {
  // BOOT-button acknowledge is deliberate rather than swept (no finger on
  // the glass to track). Kept for main.cpp call symmetry.
}

void nightlight_ui_life_glance(uint32_t now) {
  s_caption_bright_until = now + 6000;
}

}  // namespace canary::ui

#endif  // CD_NIGHTLIGHT
