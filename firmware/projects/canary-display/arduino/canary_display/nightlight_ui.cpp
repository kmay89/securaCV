// src/ui/nightlight_ui.cpp — the Canary Nightlight face (C3 1.47" glass),
// LVGL. See include/canary/ui/nightlight_ui.h for the layer contract.
//
// The face lays itself out in the LOGICAL canvas (ui/lvgl_port.cpp) at
// create time, so one composition serves all four orientations: the panel
// rotates in hardware (hal display_set_rotation), LVGL adopts the new
// shape, and main.cpp rebuilds this face into it.
//
// PORTRAIT (180x320-ish), top to bottom:
//   glance line / 7-segment clock / date / caption / the companion low
// LANDSCAPE (320x180-ish), left to right:
//   the clock owns the left ~two-thirds (date under it), the companion
//   perches in the right column with the caption beneath — a wide bedside
//   instrument rather than a squeezed portrait.
//
// The TUMBLE: on an orientation commit the companion drops in from the
// edge that WAS up (bounce path — real physics, honest direction), wearing
// its own Startle reaction (the device really was handled; that is exactly
// what Startle means), while the clock breathes back in behind it.
#include "flavor_config.h"
#ifdef CD_NIGHTLIGHT

#include <lvgl.h>
#include <stdio.h>

#include "nightlight_ui.h"
#include "lvgl_port.h"
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

// ── Runtime geometry (set in create() from the logical canvas) ──────────
int s_w = TFT_WIDTH;
int s_h = TFT_HEIGHT;
bool s_land = false;               // landscape composition?
int V(int y320) { return (y320 * s_h) / 320; }

// ── The lamp field ──
// Same band budget as the nightstand's lantern: ~23 px per band on the
// long axis, cheap on the PSRAM-less C3 (colors change, nothing reflows).
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
// Digit geometry, solved at create() from the zone the clock owns.
int s_dig_w = 30, s_dig_h = 64, s_dig_t = 7;

lv_obj_t* s_clock = nullptr;          // one container so one opa dims it all
SegDigit s_digit[4];
lv_obj_t* s_colon[2] = {nullptr};
lv_obj_t* s_date = nullptr;

lv_obj_t* s_glance = nullptr;         // the honest line (top)
lv_obj_t* s_caption = nullptr;        // the companion's few words
lv_obj_t* s_bird = nullptr;
int s_bird_px = 96;                   // bird size this composition chose

uint32_t s_caption_bright_until = 0;  // ambient-life check-in window
uint32_t s_tumble_until = 0;          // caption shows the tumble word
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
// shape by day and vanish at night — the Nightstand 7's rule, kept.
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

// Build HH:MM into `parent` starting at (x0, y0) with the solved geometry.
void build_clock_digits(lv_obj_t* parent, int x0, int gap, int colon_w) {
  int x = x0;
  for (int i = 0; i < 4; i++) {
    mk_digit(parent, &s_digit[i], x, 2, s_dig_w, s_dig_h, s_dig_t);
    x += s_dig_w;
    if (i == 1) {
      for (int c = 0; c < 2; c++) {
        s_colon[c] = mk_seg(parent, x + (colon_w - s_dig_t) / 2,
                            2 + (c == 0 ? s_dig_h / 3 - s_dig_t / 2
                                        : 2 * s_dig_h / 3 - s_dig_t / 2),
                            s_dig_t, s_dig_t);
      }
      x += colon_w;
    } else if (i != 3) {
      x += gap;
    }
  }
}

// The bird's stage lift for visits (portrait only lifts; the landscape
// perch is already center stage).
int bird_lift() { return s_land ? 0 : -((s_h * 66) / 320); }

void stage_bird(bool up) {
  if (!s_bird) return;
  lv_obj_set_style_translate_y(s_bird, up ? bird_lift() : 0, 0);
}

// ── Tumble plumbing ──────────────────────────────────────────────────────
void anim_ty_cb(void* var, int32_t v) {
  lv_obj_set_style_translate_y((lv_obj_t*)var, (lv_coord_t)v, 0);
}
void anim_tx_cb(void* var, int32_t v) {
  lv_obj_set_style_translate_x((lv_obj_t*)var, (lv_coord_t)v, 0);
}
void anim_opa_cb(void* var, int32_t v) {
  lv_obj_set_style_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
}

}  // namespace

void nightlight_ui_create() {
  // Adopt the logical canvas the port is currently shaped to.
  s_w = lvgl_port_width();
  s_h = lvgl_port_height();
  if (s_w <= 0 || s_h <= 0) { s_w = TFT_WIDTH; s_h = TFT_HEIGHT; }
  s_land = s_w > s_h;

  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, col_bg(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // ── The lamp field, created first so everything draws over it ──
  for (int i = 0; i < NL_BANDS; i++) {
    const int y0 = (s_h * i) / NL_BANDS;
    const int y1 = (s_h * (i + 1)) / NL_BANDS;
    lv_obj_t* b = lv_obj_create(scr);
    lv_obj_set_size(b, s_w, y1 - y0);
    lv_obj_set_pos(b, 0, y0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    s_band[i] = b;
  }

  // ── The glance line (the honest channel; both compositions: top) ──
  s_glance = mk_label(scr, font_caption(), col_muted());
  lv_obj_set_style_text_align(s_glance, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_glance, LV_ALIGN_TOP_MID, 0, s_land ? 6 : V(10));
  lv_label_set_text(s_glance, "");

  if (!s_land) {
    // ── PORTRAIT ──
    const int margin = 6, gap = 4, colon_w = 16;
    s_dig_w = (s_w - 2 * margin - colon_w - 2 * gap) / 4;
    s_dig_h = V(64);
    s_dig_t = s_dig_w / 5 < 6 ? 6 : s_dig_w / 5;
    const int clock_y = V(56);

    s_clock = lv_obj_create(scr);
    lv_obj_set_size(s_clock, s_w, s_dig_h + 4);
    lv_obj_set_pos(s_clock, 0, clock_y);
    lv_obj_set_style_bg_opa(s_clock, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_clock, 0, 0);
    lv_obj_set_style_pad_all(s_clock, 0, 0);
    lv_obj_clear_flag(s_clock, LV_OBJ_FLAG_SCROLLABLE);
    build_clock_digits(s_clock, margin, gap, colon_w);

    s_date = mk_label(scr, font_caption(), col_muted());
    lv_obj_set_style_text_align(s_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, clock_y + s_dig_h + V(14));

    s_caption = mk_label(scr, font_body(), col_muted());
    lv_obj_set_style_text_align(s_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_caption, LV_ALIGN_TOP_MID, 0, V(178));

    s_bird_px = V(96);
    s_bird = canary_mark_create(scr, s_bird_px);
    if (s_bird) lv_obj_align(s_bird, LV_ALIGN_BOTTOM_MID, 0, V(-22));
  } else {
    // ── LANDSCAPE ──
    // The clock owns the left zone; the companion takes the right column.
    const int bird_col = (s_w * 30) / 100;          // right ~30%
    const int zone_w = s_w - bird_col;
    const int margin = 8, gap = 5, colon_w = 18;
    s_dig_w = (zone_w - 2 * margin - colon_w - 2 * gap) / 4;
    s_dig_h = (s_h * 52) / 100;                     // ~52% of the height
    s_dig_t = s_dig_w / 5 < 7 ? 7 : s_dig_w / 5;
    const int clock_y = (s_h - s_dig_h) / 2 - s_h / 20;

    s_clock = lv_obj_create(scr);
    lv_obj_set_size(s_clock, zone_w, s_dig_h + 4);
    lv_obj_set_pos(s_clock, 0, clock_y);
    lv_obj_set_style_bg_opa(s_clock, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_clock, 0, 0);
    lv_obj_set_style_pad_all(s_clock, 0, 0);
    lv_obj_clear_flag(s_clock, LV_OBJ_FLAG_SCROLLABLE);
    build_clock_digits(s_clock, margin, gap, colon_w);

    s_date = mk_label(scr, font_caption(), col_muted());
    lv_obj_set_style_text_align(s_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_date, 0, clock_y + s_dig_h + 10);
    lv_obj_set_width(s_date, zone_w);

    s_bird_px = (s_h * 52) / 100;
    s_bird = canary_mark_create(scr, s_bird_px);
    if (s_bird)
      lv_obj_align(s_bird, LV_ALIGN_RIGHT_MID, -(bird_col - s_bird_px) / 2,
                   -(s_h / 12));

    s_caption = mk_label(scr, font_body(), col_muted());
    lv_obj_set_style_text_align(s_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_caption, bird_col + 24);
    lv_obj_align(s_caption, LV_ALIGN_BOTTOM_RIGHT, -(bird_col - s_bird_px) / 2 + 8,
                 -8);
  }

  s_staged = VisitKind::None;
  s_tumble_until = 0;
}

void nightlight_ui_tumble(uint8_t delta_cw, uint32_t now) {
  if (!s_bird) return;
  // The world turned delta_cw quarter turns clockwise, so the OLD "up"
  // now points: 1 -> screen left, 2 -> screen bottom... but a fall always
  // arrives from wherever up used to be, and after the panel re-addresses,
  // "the old up" in NEW screen coords is: 1 quarter CW -> the right edge,
  // 2 -> below, 3 -> the left edge. Enter from there, land on the perch.
  int from_x = 0, from_y = 0;
  const int fling = (s_w > s_h ? s_w : s_h) / 2;
  switch (delta_cw & 3) {
    case 1: from_x = fling; break;          // came over the right edge
    case 2: from_y = -fling; break;         // fell from above
    case 3: from_x = -fling; break;         // came over the left edge
    default: from_y = -fling; break;
  }

  // The companion FEELS the handling — Startle is exactly the honest
  // reaction (the mark rations it; asleep birds refuse it, so a night
  // flip stays still, which is also right).
  canary_mark_react(CanaryReact::Startle);

  // Drop in with a bounce; both axes so diagonal entries read naturally.
  lv_anim_t a;
  if (from_y != 0) {
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_bird);
    lv_anim_set_exec_cb(&a, anim_ty_cb);
    lv_anim_set_values(&a, from_y, 0);
    lv_anim_set_time(&a, 700);
    lv_anim_set_path_cb(&a, lv_anim_path_bounce);
    lv_anim_start(&a);
  }
  if (from_x != 0) {
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_bird);
    lv_anim_set_exec_cb(&a, anim_tx_cb);
    lv_anim_set_values(&a, from_x, 0);
    lv_anim_set_time(&a, 700);
    lv_anim_set_path_cb(&a, lv_anim_path_bounce);
    lv_anim_start(&a);
  }

  // The clock breathes back in behind the landing.
  if (s_clock) {
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_clock);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_20, LV_OPA_COVER);
    lv_anim_set_time(&a, 900);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
  }

  s_tumble_until = now + 1400;   // the caption's one word of glee
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

    auto& sng = canary::ui::song();
    sng.tick(now, night, /*gated=*/!lit);

    canary::color::LookParams lampp = look_params();
    lampp.brightness = canary::care::nightlight_lamp_bri();
    if (lit) {
      lampp.scene_idx = lamp.scene();
      lampp.night = false;
    } else {
      lampp.night = night;
      lampp.brightness = (uint8_t)(((uint16_t)lampp.brightness * 85) / 255);
    }

    uint8_t depth = 0;
    if (st.visit.kind == VisitKind::Moonwatch && lit) depth = 96;

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

    const bool visiting = st.visit.kind != VisitKind::None &&
                          st.visit.kind != VisitKind::Moonwatch;
    const bool tumbling = (int32_t)(now - s_tumble_until) < 0;
    if (!tumbling) {  // the tumble's own opa animation owns it briefly
      lv_obj_set_style_opa(s_clock, visiting ? LV_OPA_40 : LV_OPA_COVER, 0);
    }
    if (s_date) lv_obj_set_style_opa(s_date, visiting ? LV_OPA_40 : LV_OPA_COVER, 0);
  }

  // ── The companion's staging ──
  {
    const VisitKind k = st.visit.kind;
    if (k != s_staged) {
      s_staged = k;
      stage_bird(k != VisitKind::None && k != VisitKind::Moonwatch);
    }
    const bool tumbling = (int32_t)(now - s_tumble_until) < 0;
    const bool bright = (int32_t)(now - s_caption_bright_until) < 0;
    if (tumbling) {
      lv_label_set_text(s_caption, "wheee!");
      lv_obj_set_style_text_color(s_caption, col_text(), 0);
    } else if (k != VisitKind::None) {
      lv_label_set_text(s_caption, canary::care::visit_caption(k));
      lv_obj_set_style_text_color(s_caption, col_text(), 0);
    } else if (bright) {
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
      lv_label_set_text_fmt(s_date, "%s \xE2\x80\xA2 %s %d", kWd[st.wday % 7],
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
