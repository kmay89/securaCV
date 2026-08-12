// src/ui/nightstand7_ui.cpp — the 7" bedside face (Nightstand 7), LVGL.
//
// Design contract (display_nightstand_line.md §4/§6 + display_nightstand.md):
// the glass is a bedside companion, not a wall dashboard. By day:
//   - a WASH behind everything — the look engine's scene when the fleet is
//     calm, the true semantic color the instant anything wants attention
//   - the CLOCK HERO: segment digits drawn from LVGL primitives (the built-in
//     Montserrat tops out at 48 px — far too small for a 7" clock — and drawn
//     segments recolor per Character and red-shift at night for free)
//   - the DATE and the day's weather / comfort / sun lines
//   - a WEATHER ADVISORY banner when the hub says one is active (visual
//     only: weather never chimes)
//   - the FLEET STRIP along the bottom: the living canary, the state word in
//     the state's own hue, one chip per witness, and the honest glance line
// After dark the face is a clock: red-shifted digits, tomorrow's weather in
// a whisper, everything else black — dark-when-safe stays the law, and the
// only standing glow is the state channel telling the truth. The LANTERN
// overlay (user-summoned, timed-out, scene-lit) and the RENDERED sunrise
// (the wake alarm's visual ramp — this board's backlight is binary) also
// live here.
//
// Motion budget: the wash breath plus the bird's own engine, nothing else.
#include <config.h>
#ifdef CD_NIGHTSTAND7

#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "canary/ui/nightstand7_ui.h"
#include "canary/ui/theme.h"
#include "canary/ui/canary_mark.h"
#include "canary/ui/character.h"
#include "canary/ui/clock_styles.h"
#include "canary/ui/clock_face.h"
#include "canary/ui/calendar_math.h"
#include "canary/ui/fleet_figure.h"
#include "canary/ui/settings_ui.h"
#include "canary/ui/look_state.h"
#include "canary/glass_settings.h"
#include "core/fleet_figures.h"
#include "canary/care/bedside.h"
#if defined(FEATURE_LANTERN) && FEATURE_LANTERN
#include "canary/care/lantern.h"
#endif
#if defined(FEATURE_WAKE_ALARM) && FEATURE_WAKE_ALARM
#include "canary/care/wake_glue.h"
#endif
#include "color/look_engine.h"

#include "pins.h"

namespace canary::ui {

using canary::fleet::Fleet;
using canary::fleet::Sev;
using canary::fleet::Witness;

namespace {

constexpr int SCR_W = LCD_WIDTH;    // 800
constexpr int SCR_H = LCD_HEIGHT;   // 480

// ── Segment clock ─────────────────────────────────────────────────────────
// Classic seven segments per digit, rounded rects, geometry parametric on
// (w, h, thickness). Crisp at any size, recolors through the theme choke
// point, and costs 28 LVGL objects for HH:MM.

struct SegDigit {
  lv_obj_t* seg[7] = {nullptr};   // A B C D E F G
};

// Bit i set = segment lit, order A..G.
constexpr uint8_t DIGIT_MAP[10] = {
    0b0111111,  // 0: A B C D E F
    0b0000110,  // 1: B C
    0b1011011,  // 2: A B D E G
    0b1001111,  // 3: A B C D G
    0b1100110,  // 4: B C F G
    0b1101101,  // 5: A C D F G
    0b1111101,  // 6: A C D E F G
    0b0000111,  // 7: A B C
    0b1111111,  // 8
    0b1101111,  // 9: A B C D F G
};

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

// Build one digit inside a transparent w×h container at (x, y).
lv_obj_t* mk_digit(lv_obj_t* parent, SegDigit* d, int x, int y, int w, int h,
                   int t) {
  lv_obj_t* box = lv_obj_create(parent);
  lv_obj_set_size(box, w, h);
  lv_obj_set_pos(box, x, y);
  lv_obj_set_style_bg_opa(box, LV_OPA_0, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  const int vh = (h - 3 * t) / 2;         // vertical segment height
  d->seg[0] = mk_seg(box, t, 0, w - 2 * t, t);                    // A
  d->seg[1] = mk_seg(box, w - t, t, t, vh);                       // B
  d->seg[2] = mk_seg(box, w - t, 2 * t + vh, t, vh);              // C
  d->seg[3] = mk_seg(box, t, 2 * (t + vh), w - 2 * t, t);         // D
  d->seg[4] = mk_seg(box, 0, 2 * t + vh, t, vh);                  // E
  d->seg[5] = mk_seg(box, 0, t, t, vh);                           // F
  d->seg[6] = mk_seg(box, t, t + vh, w - 2 * t, t);               // G
  return box;
}

// Paint a value (0-9, or -1 = blank) in `color`; unlit segments keep a
// ghost presence by day when the style wants one (the resting shape of the
// instrument) and always vanish at night (a dark room wants digits, not
// scaffolding).
void set_digit(SegDigit* d, int value, lv_color_t color, bool night) {
  const uint8_t map =
      (value >= 0 && value <= 9) ? DIGIT_MAP[value] : 0;
  const bool ghost_day =
      seg_style(canary::glass::settings().clock_style).ghost_day;
  for (int i = 0; i < 7; i++) {
    if (!d->seg[i]) continue;
    lv_obj_set_style_bg_color(d->seg[i], color, 0);
    const bool lit = (map >> i) & 1;
    lv_obj_set_style_bg_opa(
        d->seg[i],
        lit ? LV_OPA_COVER
            : ((night || !ghost_day) ? LV_OPA_0 : LV_OPA_10), 0);
  }
}

// ── Objects ───────────────────────────────────────────────────────────────

lv_obj_t* s_wash = nullptr;      // fullscreen look-engine field (day) /
                                 // rendered sunrise (night wake ramp)
SegDigit s_digit[4];
lv_obj_t* s_digit_box[4] = {nullptr};
lv_obj_t* s_colon[2] = {nullptr};
lv_obj_t* s_date = nullptr;
lv_obj_t* s_ampm = nullptr;      // the quiet 12-hour marker (clock_12h)

// The Calendar face's mini month grid (day layout only; a dark room keeps
// the clock as its one instrument). 7 columns, up to 6 rows + a header.
constexpr int CAL_COLS = 7, CAL_ROWS = 6;
lv_obj_t* s_cal_head = nullptr;
lv_obj_t* s_cal_cell[CAL_ROWS * CAL_COLS] = {nullptr};
int s_cal_drawn_ymd = -1;        // y*10000+m*100+d the grid currently shows

// Day right column.
lv_obj_t* s_wx_now = nullptr;    // "21.4°"
lv_obj_t* s_wx_cond = nullptr;   // "some clouds"
lv_obj_t* s_wx_range = nullptr;  // "24°/13° · rain 20%"
lv_obj_t* s_wx_tmrw = nullptr;   // "tomorrow 26°/14° · rain 10%"
lv_obj_t* s_comfort = nullptr;   // "bedroom 18.5° just right"
lv_obj_t* s_sun = nullptr;       // "sun down 20:27"

// Weather advisory banner (day: top bar; night: a dim line).
lv_obj_t* s_wx_alert = nullptr;
lv_obj_t* s_wx_alert_label = nullptr;

// Day fleet strip.
// Chip geometry, sized to the glass rather than guessed: the strip runs from
// the state word's column (CHIP_X0) to where the glance line starts on the
// right, so CHIPS * CHIP_PITCH must fit inside that. Five chips at 92 px is
// the honest fit now that each carries its witness's FIGURE (the ledger's
// own picture, fleet_figure.h) beside the severity dot and name — one fewer
// chip than the text-only strip, but the strip is severity-ordered, so the
// worst are always among the visible ones and "+N" carries the rest.
lv_obj_t* s_strip = nullptr;
lv_obj_t* s_bird = nullptr;
lv_obj_t* s_state = nullptr;
constexpr int CHIPS = 5;
constexpr int CHIP_X0 = 108;
constexpr int CHIP_PITCH = 92;
constexpr int CHIP_FIG = 24;    // the figure slot, fixed so rows never reflow
static_assert(CHIP_X0 + CHIPS * CHIP_PITCH <= SCR_W - 180,
              "witness chips must not run under the glance line");
lv_obj_t* s_chip[CHIPS] = {nullptr};
lv_obj_t* s_chip_fig[CHIPS] = {nullptr};
lv_obj_t* s_chip_name[CHIPS] = {nullptr};
lv_obj_t* s_more = nullptr;
lv_obj_t* s_glance = nullptr;
// The settings doorway (day face only): a quiet gear, top-right — the same
// always-reachable promise the watch and the wall dashboard already keep.
lv_obj_t* s_gear = nullptr;
// The drawn Analog dial (ClockStyle::Analog); unused handles stay null on
// the segment styles.
AnalogClock s_analog;

// Night extras.
lv_obj_t* s_night_wx = nullptr;      // "18° now · tomorrow 26°/14°"
lv_obj_t* s_night_state = nullptr;   // honesty line (hidden when all quiet)
lv_obj_t* s_lamp_zone = nullptr;     // lantern affordance (tap target)
lv_obj_t* s_lamp_dot = nullptr;
lv_obj_t* s_lamp_label = nullptr;

// Lantern overlay (topmost; created on both layouts).
lv_obj_t* s_lantern = nullptr;
lv_obj_t* s_lantern_note = nullptr;

lv_anim_t s_wash_anim;
uint32_t s_glance_bright_until = 0;  // ambient-life status glance window

lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* f, lv_color_t c) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, "");
  return l;
}

void wash_opa_cb(void* var, int32_t v) {
  lv_obj_set_style_bg_opa((lv_obj_t*)var, (lv_opa_t)v, LV_PART_MAIN);
}

void start_wash_breath() {
  if (!s_wash) return;
  lv_anim_init(&s_wash_anim);
  lv_anim_set_var(&s_wash_anim, s_wash);
  lv_anim_set_exec_cb(&s_wash_anim, wash_opa_cb);
  lv_anim_set_values(&s_wash_anim, LV_OPA_10, LV_OPA_30);
  lv_anim_set_time(&s_wash_anim, MOTION_BREATH_MS / 2);
  lv_anim_set_playback_time(&s_wash_anim, MOTION_BREATH_MS / 2);
  lv_anim_set_repeat_count(&s_wash_anim, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&s_wash_anim, lv_anim_path_ease_in_out);
  lv_anim_start(&s_wash_anim);
}

// Severity-ordered witness index (worst first) — same selection sort the
// portrait face uses; n is small, allocation-free.
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
    default:          return active_voice().all_quiet;
  }
}

// The clock hero, positioned. Day: it owns the upper left. Night: centered,
// larger — the room's only instrument. The saved clock style picks between
// the segment family (thickness/ghost per clock_styles.h) and the drawn
// Analog dial; a style change rebuilds the face (main.cpp's ground flip).
void build_clock(lv_obj_t* scr, bool night) {
  const uint8_t style = canary::glass::settings().clock_style;
  // The Calendar face shares the hero with its month grid by day, so the
  // digits step down a size; at night every style is just the clock.
  const bool cal = clock_style_is_calendar(style) && !night;
  const int W = night ? 128 : (cal ? 88 : 104);
  const int H = night ? 224 : (cal ? 150 : 180);
  const SegStyle ss = seg_style(style);
  int T = (night ? 26 : (cal ? 18 : 22)) * ss.t_pct / 100;
  if (T < 6) T = 6;
  const int GAP = night ? 18 : 12;
  const int COLON = night ? 30 : 26;
  const int total = 4 * W + 3 * GAP + COLON + 2 * GAP;
  const int x0 = night ? (SCR_W - total) / 2 : 44;
  const int y0 = night ? 92 : 56;

  const auto mk_date = [&](int under_h) {
    s_date = mk_label(scr, font_title(), col_muted());
    lv_obj_set_style_text_align(s_date, LV_TEXT_ALIGN_CENTER, 0);
    if (night) {
      lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, y0 + under_h + 26);
    } else {
      lv_obj_set_pos(s_date, x0 + 4, y0 + under_h + 20);
    }
  };
  const auto mk_ampm = [&](int x, int y, bool center) {
    // The quiet marker: caption-sized, muted, beside the clock — visible
    // when you look for it, invisible when you don't (clock_12h).
    s_ampm = mk_label(scr, font_caption(), col_faint());
    if (center) lv_obj_align(s_ampm, LV_ALIGN_TOP_MID, x, y);
    else        lv_obj_set_pos(s_ampm, x, y);
  };

  if (clock_style_is_analog(style)) {
    // The dial sits where the digits would: day in the hero's corner (the
    // complications keep their column), night centered above the date.
    const int r = night ? 110 : 95;
    const int cx = night ? SCR_W / 2 : x0 + total / 2;
    const int cy = y0 + (night ? 110 : 95);
    analog_clock_build(&s_analog, scr, cx, cy, r);
    if (night) mk_ampm(r + 24, cy - 8, true);
    else       mk_ampm(cx + r + 12, cy + r - 22, false);
    mk_date(night ? H : 2 * r);
    return;
  }

  int x = x0;
  for (int i = 0; i < 4; i++) {
    s_digit_box[i] = mk_digit(scr, &s_digit[i], x, y0, W, H, T);
    x += W + GAP;
    if (i == 1) {
      // Colon: two dots on the midline gaps.
      for (int c = 0; c < 2; c++) {
        s_colon[c] = mk_seg(scr, x + (COLON - T) / 2,
                            y0 + (c == 0 ? H / 3 - T / 2 : 2 * H / 3 - T / 2),
                            T, T);
      }
      x += COLON + 2 * GAP;
    }
  }
  mk_ampm(x0 + total + 8, y0 + H - 18, false);
  mk_date(H);

  if (cal) {
    // The month grid, under the date, clear of the fleet strip: a header
    // ("August 2026") and up to 6 rows of day numbers, today in the
    // Character's accent. Same law as everything else on this face — it
    // recolors through the theme choke point and never outranks an alarm.
    const int gx = x0, gy = y0 + H + 46;
    const int cw = 44, ch = 18;
    s_cal_head = mk_label(scr, font_label(), col_text());
    lv_obj_set_pos(s_cal_head, gx, gy);
    for (int r = 0; r < CAL_ROWS; r++) {
      for (int c = 0; c < CAL_COLS; c++) {
        lv_obj_t* l = mk_label(scr, font_caption(), col_muted());
        lv_obj_set_pos(l, gx + c * cw, gy + 22 + r * ch);
        s_cal_cell[r * CAL_COLS + c] = l;
      }
    }
    s_cal_drawn_ymd = -1;  // force the first paint
  }
}

void build_day(lv_obj_t* scr) {
  build_clock(scr, false);

  // Right column — the complications.
  const int cx = 568;
  int cy = 64;
  s_wx_now = mk_label(scr, font_hero(), col_text());
  lv_obj_set_pos(s_wx_now, cx, cy);
  cy += 62;
  s_wx_cond = mk_label(scr, font_body(), col_muted());
  lv_obj_set_pos(s_wx_cond, cx, cy);
  cy += 40;
  s_wx_range = mk_label(scr, font_body(), col_text());
  lv_obj_set_pos(s_wx_range, cx, cy);
  cy += 40;
  s_wx_tmrw = mk_label(scr, font_label(), col_muted());
  lv_obj_set_pos(s_wx_tmrw, cx, cy);
  cy += 36;
  s_comfort = mk_label(scr, font_body(), col_text());
  lv_obj_set_pos(s_comfort, cx, cy);
  cy += 40;
  s_sun = mk_label(scr, font_label(), col_muted());
  lv_obj_set_pos(s_sun, cx, cy);

  // Weather advisory banner (top, spans the glass; hidden until active).
  s_wx_alert = lv_obj_create(scr);
  lv_obj_set_size(s_wx_alert, SCR_W - 24, 44);
  lv_obj_align(s_wx_alert, LV_ALIGN_TOP_MID, 0, 6);
  lv_obj_set_style_radius(s_wx_alert, 10, 0);
  lv_obj_set_style_bg_color(s_wx_alert, col_surface(), 0);
  lv_obj_set_style_bg_opa(s_wx_alert, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_wx_alert, 1, 0);
  lv_obj_set_style_border_color(s_wx_alert, col_edge(), 0);
  lv_obj_set_style_pad_all(s_wx_alert, 0, 0);
  lv_obj_clear_flag(s_wx_alert, LV_OBJ_FLAG_SCROLLABLE);
  s_wx_alert_label = mk_label(s_wx_alert, font_body(), col_text());
  lv_obj_align(s_wx_alert_label, LV_ALIGN_LEFT_MID, 14, 0);
  lv_obj_add_flag(s_wx_alert, LV_OBJ_FLAG_HIDDEN);

  // The fleet strip.
  s_strip = lv_obj_create(scr);
  lv_obj_set_size(s_strip, SCR_W, 96);
  lv_obj_align(s_strip, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_radius(s_strip, 0, 0);
  lv_obj_set_style_bg_color(s_strip, col_surface(), 0);
  lv_obj_set_style_bg_opa(s_strip, LV_OPA_40, 0);
  lv_obj_set_style_border_width(s_strip, 0, 0);
  lv_obj_set_style_pad_all(s_strip, 0, 0);
  lv_obj_clear_flag(s_strip, LV_OBJ_FLAG_SCROLLABLE);

  s_bird = canary_mark_create(s_strip, 72);
  if (s_bird) lv_obj_align(s_bird, LV_ALIGN_LEFT_MID, 18, 0);

  s_state = mk_label(s_strip, font_title(), sev_color(Sev::Ok, false));
  lv_obj_align(s_state, LV_ALIGN_LEFT_MID, 108, -18);

  for (int i = 0; i < CHIPS; i++) {
    // Fixed-size figure slot first (hidden until a witness resolves), then
    // the severity dot, then the name — the slot discipline keeps the strip
    // from reflowing as figures land (FLEET_FIGURES.md).
    s_chip_fig[i] = fleet_figure_create(s_strip, nullptr, CHIP_FIG);
    s_chip[i] = lv_obj_create(s_strip);
    lv_obj_set_size(s_chip[i], 12, 12);
    lv_obj_set_style_radius(s_chip[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_chip[i], 0, 0);
    lv_obj_set_style_pad_all(s_chip[i], 0, 0);
    lv_obj_add_flag(s_chip[i], LV_OBJ_FLAG_HIDDEN);
    s_chip_name[i] = mk_label(s_strip, font_label(), col_muted());
    lv_obj_add_flag(s_chip_name[i], LV_OBJ_FLAG_HIDDEN);
  }
  s_more = mk_label(s_strip, font_caption(), col_faint());
  lv_obj_add_flag(s_more, LV_OBJ_FLAG_HIDDEN);

  s_glance = mk_label(s_strip, font_body(), col_muted());
  lv_obj_align(s_glance, LV_ALIGN_RIGHT_MID, -18, 22);

  // The settings doorway: a quiet word in the top-right corner. This face
  // used to have NO touch path to the settings surface at all — the wall
  // dashboard keeps its doorway on the transparency sheet, which this
  // flavor compiles out — so the 7" bedside glass read as touch-dead.
  s_gear = mk_label(scr, font_caption(), col_faint());
  lv_label_set_text(s_gear, LV_SYMBOL_SETTINGS " settings");
  lv_obj_align(s_gear, LV_ALIGN_TOP_RIGHT, -18, 12);
}

void build_night(lv_obj_t* scr) {
  build_clock(scr, true);

  // One whisper of weather under the date: now + tomorrow.
  s_night_wx = mk_label(scr, font_body(), col_muted());
  lv_obj_set_style_text_align(s_night_wx, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_night_wx, LV_ALIGN_TOP_MID, 0, 404);

  // Advisory line (dim; weather never chimes, and at night never glares).
  s_wx_alert_label = mk_label(scr, font_label(), col_muted());
  lv_obj_set_style_text_align(s_wx_alert_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_wx_alert_label, LV_ALIGN_TOP_MID, 0, 440);
  lv_obj_add_flag(s_wx_alert_label, LV_OBJ_FLAG_HIDDEN);

  // Honesty line: hidden while everything is quiet, first-class when not.
  s_night_state = mk_label(scr, font_title(), sev_color(Sev::Warn, true));
  lv_obj_set_style_text_align(s_night_state, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_night_state, LV_ALIGN_TOP_MID, 0, 34);
  lv_obj_add_flag(s_night_state, LV_OBJ_FLAG_HIDDEN);

#if defined(FEATURE_LANTERN) && FEATURE_LANTERN
  // The lantern affordance: a small warm dot + word, bottom-right, faint —
  // reachable half-asleep, invisible from across the room.
  s_lamp_zone = lv_obj_create(scr);
  lv_obj_set_size(s_lamp_zone, 96, 72);
  lv_obj_align(s_lamp_zone, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_bg_opa(s_lamp_zone, LV_OPA_0, 0);
  lv_obj_set_style_border_width(s_lamp_zone, 0, 0);
  lv_obj_set_style_pad_all(s_lamp_zone, 0, 0);
  lv_obj_clear_flag(s_lamp_zone, LV_OBJ_FLAG_SCROLLABLE);
  s_lamp_dot = lv_obj_create(s_lamp_zone);
  lv_obj_set_size(s_lamp_dot, 10, 10);
  lv_obj_align(s_lamp_dot, LV_ALIGN_CENTER, 0, -10);
  lv_obj_set_style_radius(s_lamp_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(s_lamp_dot, 0, 0);
  lv_obj_set_style_bg_color(s_lamp_dot, lv_color_hex(0x3A2410), 0);
  lv_obj_set_style_bg_opa(s_lamp_dot, LV_OPA_COVER, 0);
  s_lamp_label = mk_label(s_lamp_zone, font_caption(), col_faint());
  lv_label_set_text(s_lamp_label, "lantern");
  lv_obj_align(s_lamp_label, LV_ALIGN_CENTER, 0, 12);
#endif
}

// The lantern overlay — created last so it sits above either layout.
void build_lantern(lv_obj_t* scr) {
  s_lantern = lv_obj_create(scr);
  lv_obj_set_size(s_lantern, SCR_W, SCR_H);
  lv_obj_set_pos(s_lantern, 0, 0);
  lv_obj_set_style_radius(s_lantern, 0, 0);
  lv_obj_set_style_border_width(s_lantern, 0, 0);
  lv_obj_set_style_pad_all(s_lantern, 0, 0);
  lv_obj_set_style_bg_opa(s_lantern, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_grad_dir(s_lantern, LV_GRAD_DIR_VER, 0);
  lv_obj_clear_flag(s_lantern, LV_OBJ_FLAG_SCROLLABLE);
  s_lantern_note = mk_label(s_lantern, font_caption(), lv_color_hex(0x1A1A1A));
  lv_obj_align(s_lantern_note, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_add_flag(s_lantern, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

void nightstand7_ui_create() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, col_bg(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Reset every handle: main.cpp rebuilds the face with lv_obj_clean() on a
  // ground flip, which deletes the old objects out from under these statics.
  s_wash = nullptr;
  for (auto& d : s_digit) d = SegDigit{};
  for (auto& b : s_digit_box) b = nullptr;
  s_colon[0] = s_colon[1] = nullptr;
  s_ampm = s_cal_head = nullptr;
  for (auto& c : s_cal_cell) c = nullptr;
  s_cal_drawn_ymd = -1;
  s_date = s_wx_now = s_wx_cond = s_wx_range = s_wx_tmrw = nullptr;
  s_comfort = s_sun = s_wx_alert = s_wx_alert_label = nullptr;
  s_strip = s_bird = s_state = s_more = s_glance = s_gear = nullptr;
  for (int i = 0; i < CHIPS; i++)
    s_chip[i] = s_chip_fig[i] = s_chip_name[i] = nullptr;
  s_analog = AnalogClock{};
  s_night_wx = s_night_state = nullptr;
  s_lamp_zone = s_lamp_dot = s_lamp_label = nullptr;
  s_lantern = s_lantern_note = nullptr;

  const bool night = character_night();

  // The wash first, under everything. At night it stays transparent except
  // for the rendered sunrise; by day it is the look engine's field.
  s_wash = lv_obj_create(scr);
  lv_obj_set_size(s_wash, SCR_W, SCR_H);
  lv_obj_set_pos(s_wash, 0, 0);
  lv_obj_set_style_radius(s_wash, 0, 0);
  lv_obj_set_style_border_width(s_wash, 0, 0);
  lv_obj_set_style_pad_all(s_wash, 0, 0);
  lv_obj_set_style_bg_grad_dir(s_wash, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(s_wash, night ? LV_OPA_0 : LV_OPA_20, 0);
  lv_obj_clear_flag(s_wash, LV_OBJ_FLAG_SCROLLABLE);
  if (!night) start_wash_breath();

  // Two layouts, not one skinned one. main.cpp rebuilds the face whenever
  // the ground flips, which is exactly when this must swap.
  if (night) build_night(scr);
  else build_day(scr);

  build_lantern(scr);
}

void nightstand7_ui_update(const Fleet& fleet, uint32_t now,
                           const Nightstand7State& st) {
  if (!s_wash) return;
  const bool night_mode = character_night();
  const bool red = st.night;   // red-shift preference (night look)
  const Sev worst = fleet.worst(now);
  const bool link_down = !st.wifi_ok || !st.mqtt_ok;

  canary_mark_mood(st.bird);

  // ── Clock + date (both layouts) ──
  const auto& gsx = canary::glass::settings();
  const bool twelve = gsx.clock_12h != 0;
  const lv_color_t digit_col =
      night_mode ? (red ? ncol_text() : col_text()) : col_text();
  // The Analog dial, when built; the segment calls below no-op on its null
  // handles, so one update path serves every style.
  analog_clock_update(&s_analog, st.clock_hh, st.clock_mm, digit_col,
                      night_mode ? (red ? ncol_muted() : col_muted())
                                 : col_muted(),
                      st.time_valid);
  bool pm = false;
  if (st.time_valid) {
    const int dh = clock_display_hour(st.clock_hh, twelve, &pm);
    // 12-hour blanks the leading zero (" 9:41"); 24-hour keeps it (09:41).
    set_digit(&s_digit[0], (twelve && dh < 10) ? -1 : dh / 10, digit_col,
              night_mode);
    set_digit(&s_digit[1], dh % 10, digit_col, night_mode);
    set_digit(&s_digit[2], st.clock_mm / 10, digit_col, night_mode);
    set_digit(&s_digit[3], st.clock_mm % 10, digit_col, night_mode);
  } else {
    for (auto& d : s_digit) set_digit(&d, -1, digit_col, night_mode);
  }
  for (auto* c : s_colon) {
    if (!c) continue;
    lv_obj_set_style_bg_color(c, digit_col, 0);
    lv_obj_set_style_bg_opa(c, st.time_valid ? LV_OPA_COVER : LV_OPA_10, 0);
  }
  if (s_ampm) {
    if (twelve && st.time_valid) {
      lv_label_set_text(s_ampm, pm ? "PM" : "AM");
      lv_obj_set_style_text_color(
          s_ampm, night_mode ? (red ? ncol_muted() : col_faint()) : col_faint(),
          0);
      lv_obj_clear_flag(s_ampm, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_ampm, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (s_date) {
    const lv_color_t date_col =
        night_mode ? (red ? ncol_muted() : col_muted()) : col_muted();
    lv_obj_set_style_text_color(s_date, date_col, 0);
    time_t t = time(nullptr);
    if (st.time_valid && t > 1700000000) {
      struct tm lt;
      localtime_r(&t, &lt);
      static const char* WD[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                 "Thursday", "Friday", "Saturday"};
      static const char* MO[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
      // The clock-trust whisper rides the date line (day only): a bedside
      // clock that has not been VERIFIED against its time sources for days
      // says so, quietly, instead of drifting in silence.
      char trust[40] = "";
      if (!night_mode) {
        if (st.sync_age_h == 0xFFFF) {
          snprintf(trust, sizeof(trust), " \xE2\x80\xA2 clock unverified");
        } else if (st.sync_age_h >= 48) {
          snprintf(trust, sizeof(trust), " \xE2\x80\xA2 clock checked %ud ago",
                   (unsigned)(st.sync_age_h / 24));
        }
      }
      lv_label_set_text_fmt(s_date, "%s \xE2\x80\xA2 %s %d%s", WD[lt.tm_wday],
                            MO[lt.tm_mon], lt.tm_mday, trust);
    } else {
      lv_label_set_text(s_date, "waiting for the clock");
    }
  }

  // ── The month grid (Calendar face, day layout) ──
  if (s_cal_head) {
    time_t t = time(nullptr);
    struct tm lt;
    if (st.time_valid && t > 1700000000) {
      localtime_r(&t, &lt);
      const int y = lt.tm_year + 1900, m = lt.tm_mon + 1, d = lt.tm_mday;
      const int ymd = y * 10000 + m * 100 + d;
      if (ymd != s_cal_drawn_ymd) {
        s_cal_drawn_ymd = ymd;
        lv_label_set_text_fmt(s_cal_head, "%s %d", cal_month_name(m), y);
        const int days = cal_days_in_month(y, m);
        for (int r = 0; r < CAL_ROWS; r++)
          for (int c = 0; c < CAL_COLS; c++)
            lv_label_set_text(s_cal_cell[r * CAL_COLS + c], "");
        for (int day = 1; day <= days; day++) {
          int r, c;
          cal_cell_of(y, m, day, &r, &c);
          lv_obj_t* cell = s_cal_cell[r * CAL_COLS + c];
          lv_label_set_text_fmt(cell, "%d", day);
          lv_obj_set_style_text_color(
              cell, day == d ? col_accent() : col_muted(), 0);
        }
      }
      lv_obj_set_style_text_color(s_cal_head, col_text(), 0);
      lv_obj_clear_flag(s_cal_head, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_cal_head, LV_OBJ_FLAG_HIDDEN);
      for (auto* c : s_cal_cell)
        if (c) lv_label_set_text(c, "");
      s_cal_drawn_ymd = -1;
    }
  }

  // ── Weather + advisory ──
  canary::care::BedsideWeather wx;
  const bool have_wx = canary::care::bedside_weather(&wx);
  canary::care::BedsideWxAlert wxa;
  const bool have_wxa = canary::care::bedside_weather_alert(&wxa);

  if (!night_mode) {
    // Day complications.
    if (s_wx_now) {
      if (have_wx && wx.t_now_c10 > -9990) {
        const int at10 = wx.t_now_c10 < 0 ? -wx.t_now_c10 : wx.t_now_c10;
        lv_label_set_text_fmt(s_wx_now, "%s%d.%d\xC2\xB0",
                              wx.t_now_c10 < 0 ? "-" : "", at10 / 10,
                              at10 % 10);
      } else if (have_wx) {
        lv_label_set_text_fmt(s_wx_now, "%d\xC2\xB0",
                              (wx.hi_c10 + (wx.hi_c10 >= 0 ? 5 : -5)) / 10);
      } else {
        lv_label_set_text(s_wx_now, "");
      }
    }
    if (s_wx_cond)
      lv_label_set_text(s_wx_cond, have_wx && wx.cond[0] ? wx.cond : "");
    if (s_wx_range) {
      if (have_wx) {
        char line[64];
        size_t o = (size_t)snprintf(
            line, sizeof(line), "%d\xC2\xB0/%d\xC2\xB0",
            (wx.hi_c10 + (wx.hi_c10 >= 0 ? 5 : -5)) / 10,
            (wx.lo_c10 + (wx.lo_c10 >= 0 ? 5 : -5)) / 10);
        if (wx.rain_pct > 0 && o < sizeof(line))
          snprintf(line + o, sizeof(line) - o, " \xE2\x80\xA2 rain %d%%",
                   wx.rain_pct);
        lv_label_set_text(s_wx_range, line);
      } else {
        lv_label_set_text(s_wx_range, "");
      }
    }
    if (s_wx_tmrw) {
      char line[80];
      lv_label_set_text(
          s_wx_tmrw,
          canary::care::bedside_tomorrow_line(line, sizeof(line)) ? line : "");
    }
    if (s_comfort) {
      char line[80];
      lv_label_set_text(
          s_comfort,
          canary::care::bedside_comfort_line(fleet, line, sizeof(line)) ? line
                                                                        : "");
    }
    if (s_sun) {
      char line[48];
      lv_label_set_text(
          s_sun,
          canary::care::bedside_evening_line(line, sizeof(line)) ? line : "");
    }
    if (s_wx_alert && s_wx_alert_label) {
      if (have_wxa) {
        lv_label_set_text_fmt(s_wx_alert_label, "%s \xE2\x80\xA2 %s", wxa.text,
                              wxa.sev ? "warning" : "advisory");
        // A warning borrows the semantic amber; an advisory stays chrome.
        lv_obj_set_style_text_color(
            s_wx_alert_label, wxa.sev ? col_warn() : col_text(), 0);
        lv_obj_clear_flag(s_wx_alert, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(s_wx_alert, LV_OBJ_FLAG_HIDDEN);
      }
    }
  } else {
    // Night: one whisper — current temp + tomorrow.
    if (s_night_wx) {
      char line[96];
      line[0] = 0;
      size_t o = 0;
      if (have_wx && wx.t_now_c10 > -9990) {
        const int at10 = wx.t_now_c10 < 0 ? -wx.t_now_c10 : wx.t_now_c10;
        o = (size_t)snprintf(line, sizeof(line), "%s%d\xC2\xB0 now",
                             wx.t_now_c10 < 0 ? "-" : "", (at10 + 5) / 10);
      }
      char tl[72];
      if (canary::care::bedside_tomorrow_line(tl, sizeof(tl)) &&
          o < sizeof(line)) {
        snprintf(line + o, sizeof(line) - o, "%s%s", o ? " \xE2\x80\xA2 " : "",
                 tl);
      }
      lv_label_set_text(s_night_wx, line);
      lv_obj_set_style_text_color(s_night_wx,
                                  red ? ncol_muted() : col_muted(), 0);
    }
    if (s_wx_alert_label) {
      if (have_wxa) {
        lv_label_set_text_fmt(s_wx_alert_label, "%s \xE2\x80\xA2 %s", wxa.text,
                              wxa.sev ? "warning" : "advisory");
        lv_obj_set_style_text_color(
            s_wx_alert_label,
            wxa.sev ? (red ? ncol_alert() : col_warn())
                    : (red ? ncol_muted() : col_muted()),
            0);
        lv_obj_clear_flag(s_wx_alert_label, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(s_wx_alert_label, LV_OBJ_FLAG_HIDDEN);
      }
    }
    if (s_night_state) {
      if (link_down) {
        lv_label_set_text(s_night_state,
                          !st.wifi_ok
                              ? (st.wifi_reason ? st.wifi_reason : "wifi down")
                              : "broker down");
        lv_obj_set_style_text_color(s_night_state, sev_color(Sev::Warn, red),
                                    0);
        lv_obj_clear_flag(s_night_state, LV_OBJ_FLAG_HIDDEN);
      } else if ((uint8_t)worst >= (uint8_t)Sev::Warn) {
        lv_label_set_text(s_night_state, state_word(worst));
        lv_obj_set_style_text_color(s_night_state, sev_color(worst, red), 0);
        lv_obj_clear_flag(s_night_state, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(s_night_state, LV_OBJ_FLAG_HIDDEN);
      }
    }
  }

  // ── The wash ──
  auto& lp = look_params();
  lp.night = night_mode;
  if (!night_mode) {
    canary::color::Rgb ws[2];
    canary::color::wash_stops(now, lp, (canary::color::Sev)(uint8_t)worst,
                              /*safe_dark=*/false, ws, 2);
    lv_obj_set_style_bg_color(s_wash, lv_color_make(ws[0].r, ws[0].g, ws[0].b),
                              0);
    lv_obj_set_style_bg_grad_color(
        s_wash, lv_color_make(ws[1].r, ws[1].g, ws[1].b), 0);
  } else {
#if defined(FEATURE_WAKE_ALARM) && FEATURE_WAKE_ALARM
    // The rendered sunrise: this board cannot PWM its backlight, so the
    // gentle wake's dawn is DRAWN — a warm field rising from the bottom,
    // opacity tracking the ramp level.
    const int wl = canary::care::wake_alarm_backlight();
    if (wl > 0) {
      lv_obj_set_style_bg_color(s_wash, lv_color_hex(0x000000), 0);
      lv_obj_set_style_bg_grad_color(s_wash, lv_color_hex(0xFF9A3C), 0);
      lv_obj_set_style_bg_opa(
          s_wash, (lv_opa_t)(LV_OPA_20 + (wl * (LV_OPA_COVER - LV_OPA_20)) / 255),
          0);
    } else {
      lv_obj_set_style_bg_opa(s_wash, LV_OPA_0, 0);
    }
#else
    lv_obj_set_style_bg_opa(s_wash, LV_OPA_0, 0);
#endif
  }

  // ── Day fleet strip ──
  if (!night_mode && s_strip) {
    lv_label_set_text(s_state, state_word(worst));
    lv_obj_set_style_text_color(s_state, sev_color(worst, false), 0);

    int order[CD_FLEET_MAX_DEVICES];
    const int m = order_by_severity(fleet, now, order,
                                    (int)(sizeof(order) / sizeof(order[0])));
    const int shown = m < CHIPS ? m : CHIPS;
    int x = CHIP_X0;
    const int y = 22;
    for (int i = 0; i < CHIPS; i++) {
      if (i < shown) {
        const Witness* w = fleet.at(order[i]);
        const Sev s = w ? fleet.witness_sev(*w, now) : Sev::Ok;
        // The witness's own picture, from the shared ledger — the wire
        // device type resolves it exactly like the phone does, and an
        // unresolvable type keeps a hidden slot (never a guessed product).
        const auto* fig =
            w ? canary::figures::figure_for(w->device_type) : nullptr;
        if (s_chip_fig[i]) {
          fleet_figure_set(s_chip_fig[i], fig ? fig->figure_id : nullptr);
          lv_obj_align(s_chip_fig[i], LV_ALIGN_LEFT_MID, x, y);
        }
        lv_obj_set_style_bg_color(s_chip[i], sev_color(s, false), 0);
        lv_obj_align(s_chip[i], LV_ALIGN_LEFT_MID, x + CHIP_FIG + 4, y);
        // Bounded name: an LVGL label self-sizes, so a long room name would
        // walk over its neighbor. Truncate in C (the same %.Ns the comfort
        // line uses) rather than depend on a long-mode enum that was renamed
        // across the LVGL major boundary this project straddles.
        lv_label_set_text_fmt(s_chip_name[i], "%.8s",
                              w ? Fleet::display_name(*w) : "-");
        lv_obj_set_style_text_color(
            s_chip_name[i],
            (uint8_t)s >= (uint8_t)Sev::Warn ? col_text() : col_muted(), 0);
        lv_obj_align(s_chip_name[i], LV_ALIGN_LEFT_MID, x + CHIP_FIG + 20, y);
        lv_obj_clear_flag(s_chip[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_chip_name[i], LV_OBJ_FLAG_HIDDEN);
        x += CHIP_PITCH;
      } else {
        if (s_chip_fig[i]) lv_obj_add_flag(s_chip_fig[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_chip[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_chip_name[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
    if (m > CHIPS) {
      lv_label_set_text_fmt(s_more, "+%d", m - CHIPS);
      lv_obj_align(s_more, LV_ALIGN_LEFT_MID, x, y);
      lv_obj_clear_flag(s_more, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_more, LV_OBJ_FLAG_HIDDEN);
    }

    // Glance line — honesty first, then the count; the ambient-life status
    // glance brightens it briefly so the check-in is visible, then it fades
    // back to muted.
    const bool bright = (int32_t)(now - s_glance_bright_until) < 0;
    if (link_down) {
      lv_label_set_text(s_glance,
                        !st.wifi_ok
                            ? (st.wifi_reason ? st.wifi_reason : "wifi down")
                            : "broker down");
      lv_obj_set_style_text_color(s_glance, sev_color(Sev::Warn, false), 0);
    } else if (fleet.count() == 0) {
      lv_label_set_text(s_glance, "no canaries yet");
      lv_obj_set_style_text_color(s_glance, col_muted(), 0);
    } else {
      lv_label_set_text_fmt(s_glance, "%d %s \xE2\x80\xA2 online", fleet.count(),
                            fleet.count() == 1 ? "canary" : "canaries");
      lv_obj_set_style_text_color(s_glance, bright ? col_text() : col_muted(),
                                  0);
    }
  }

  // ── Lantern overlay ──
#if defined(FEATURE_LANTERN) && FEATURE_LANTERN
  {
    auto& lamp = canary::care::lantern();
    // The honest veto: anything the glass must SHOW takes it back from the
    // lamp — a Warn+ condition or a dead link (silence is never safety).
    const bool attention = (uint8_t)worst >= (uint8_t)Sev::Warn || link_down;
    if (lamp.active(now, night_mode, attention)) {
      // The lantern is a LAMP: its light is the chosen scene, rendered
      // day-bright (night dimming is for the state channel, not for a lamp
      // someone just asked for).
      canary::color::LookParams lampp = lp;
      lampp.scene_idx = lamp.scene();
      lampp.night = false;
      canary::color::Rgb ws[2];
      canary::color::wash_stops(now, lampp, canary::color::Sev::Ok,
                                /*safe_dark=*/false, ws, 2);
      lv_obj_set_style_bg_color(s_lantern,
                                lv_color_make(ws[0].r, ws[0].g, ws[0].b), 0);
      lv_obj_set_style_bg_grad_color(
          s_lantern, lv_color_make(ws[1].r, ws[1].g, ws[1].b), 0);
      const uint32_t rem = lamp.remaining_s(now);
      if (rem) {
        lv_label_set_text_fmt(s_lantern_note,
                              "lantern \xE2\x80\xA2 %lu min \xE2\x80\xA2 tap for the "
                              "next look \xE2\x80\xA2 hold to put it out",
                              (unsigned long)((rem + 59) / 60));
      } else {
        lv_label_set_text(s_lantern_note,
                          "lantern hours \xE2\x80\xA2 tap for the next look");
      }
      lv_obj_clear_flag(s_lantern, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_lantern, LV_OBJ_FLAG_HIDDEN);
    }
  }
#endif
}

void nightstand7_ui_ack_hold(bool /*active*/) {
  // The bedside glass keeps acknowledgement quiet: no sweep ring — the
  // long-press just works (the chime-less strip makes a visible ring feel
  // like an alarm UI, which this face deliberately is not).
}

bool nightstand7_ui_handle_tap(int16_t x, int16_t y, uint32_t now) {
  // The settings doorway (day face): the gear corner opens the shared
  // settings surface. Day only — at night the glass is a clock and the
  // affordance corner belongs to the lantern; settings can wait for light.
  if (!character_night() && x >= SCR_W - 170 && y <= 56) {
    settings_ui_open();
    return true;
  }
#if defined(FEATURE_LANTERN) && FEATURE_LANTERN
  auto& lamp = canary::care::lantern();
  if (s_lantern && !lv_obj_has_flag(s_lantern, LV_OBJ_FLAG_HIDDEN)) {
    // Lantern lit: a tap walks the scene ring (Rainbow included).
    lamp.cycle_scene(canary::color::kSceneCount);
    // A hand on the device picks a scene too — put the wheel away.
    canary::ui::look_set_custom_hue(-1);
    canary::care::lantern_prefs_changed();
    return true;
  }
  if (character_night() && x >= SCR_W - 120 && y >= SCR_H - 96) {
    // The affordance corner: summon the lantern.
    lamp.summon(now);
    return true;
  }
#else
  (void)x; (void)y; (void)now;
#endif
  return false;
}

void nightstand7_ui_life_glance(uint32_t now) {
  s_glance_bright_until = now + 6000;
}

}  // namespace canary::ui

#endif  // CD_NIGHTSTAND7
