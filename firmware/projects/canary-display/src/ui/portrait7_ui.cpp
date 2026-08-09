// src/ui/portrait7_ui.cpp — the 7" glass turned on its side (see the header).
//
// One portrait column for both 7" personalities. The 800x480 panel is
// software-rotated to a 480x800 logical canvas (lvgl_port), and this face
// lays itself out top-to-bottom: segment clock hero, one-word household
// state, the living canary, a vertical witness list (worst floated up), and
// an honest glance line. Night red-shifts and strips it to the clock + the
// state channel — dark-when-safe still holds; the backlight-off itself is
// main.cpp's job (this board's backlight is binary).
#include <config.h>
#ifdef CD_FLAVOR_DASH

#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "canary/ui/portrait7_ui.h"
#include "canary/ui/theme.h"
#include "canary/ui/canary_mark.h"
#include "canary/ui/character.h"
#ifdef CD_NIGHTSTAND7
// The bedside column borrows the same hub weather + comfort source the
// landscape bedside face uses (nightstand7_ui.cpp). Only the Nightstand 7
// build links it; the wall Dash 7 column stays fleet-first with no weather.
#include "canary/care/bedside.h"
#endif
#include "pins.h"

namespace canary::ui {

using canary::fleet::Fleet;
using canary::fleet::Sev;
using canary::fleet::Witness;

namespace {

// Portrait logical canvas: the panel's landscape axes swapped (see
// lvgl_port_set_rotation). 480 wide, 800 tall.
constexpr int W = LCD_HEIGHT;   // 480
constexpr int H = LCD_WIDTH;    // 800

// ── Segment clock (shared geometry with the bedside face) ────────────────
struct SegDigit { lv_obj_t* seg[7] = {nullptr}; };

// Bit i set = segment lit, order A..G.
constexpr uint8_t DIGIT_MAP[10] = {
    0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
    0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111,
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
  d->seg[0] = mk_seg(box, t, 0, w - 2 * t, t);                 // A
  d->seg[1] = mk_seg(box, w - t, t, t, vh);                    // B
  d->seg[2] = mk_seg(box, w - t, 2 * t + vh, t, vh);           // C
  d->seg[3] = mk_seg(box, t, 2 * (t + vh), w - 2 * t, t);      // D
  d->seg[4] = mk_seg(box, 0, 2 * t + vh, t, vh);               // E
  d->seg[5] = mk_seg(box, 0, t, t, vh);                        // F
  d->seg[6] = mk_seg(box, t, t + vh, w - 2 * t, t);            // G
  return box;
}

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

// ── Objects ──────────────────────────────────────────────────────────────
// Emphasis is a compile-time personality (display_nightstand_line.md): the
// wall Dash 7 leans on the fleet, so its column gives more rows to the
// witness list; the bedside Nightstand 7 leans on the clock, keeps the list
// shorter, and spends the freed room on a day weather + comfort line. The
// runtime st.bedside flag matches this and only ever rides the same build.
#ifdef CD_NIGHTSTAND7
constexpr int LIST_Y0    = 508;
constexpr int LIST_PITCH = 40;
constexpr int LIST_CAP   = 5;    // fewer rows; weather + comfort take the foot
#else
constexpr int LIST_Y0    = 500;
constexpr int LIST_PITCH = 38;
constexpr int LIST_CAP   = 7;    // the wall column is fleet-first
#endif
constexpr int ROWS = 7;   // object capacity (>= LIST_CAP on either personality)

lv_obj_t* s_wash = nullptr;      // full-glass severity/scene tint
SegDigit  s_digit[4];
lv_obj_t* s_digit_box[4] = {nullptr};
lv_obj_t* s_colon = nullptr;
lv_obj_t* s_date = nullptr;
lv_obj_t* s_state = nullptr;     // one-word household state, in its own hue
lv_obj_t* s_link = nullptr;      // link/health line under the state
lv_obj_t* s_bird = nullptr;
lv_obj_t* s_dot[ROWS] = {nullptr};
lv_obj_t* s_name[ROWS] = {nullptr};
lv_obj_t* s_word[ROWS] = {nullptr};
lv_obj_t* s_glance = nullptr;
#ifdef CD_NIGHTSTAND7
lv_obj_t* s_wx = nullptr;        // "21.4° · some clouds · 24°/13°" (day)
lv_obj_t* s_comfort = nullptr;   // "bedroom 18.5° · just right" (day)
#endif

uint32_t s_glance_bright_until = 0;

lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* f, lv_color_t c) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, "");
  return l;
}

// Worst-first witness order (allocation-free selection sort; n is small).
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

// Plain-literal state words (no look-engine dependency — this TU compiles on
// the wall Dash 7 too, which never links the bedside voice tables).
const char* state_word(Sev s) {
  switch (s) {
    case Sev::Warn:   return "wants a look";
    case Sev::Alert:  return "needs you";
    case Sev::Tamper: return "tamper";
    default:          return "all quiet";
  }
}

// Clock digits, centered across the column. Bigger at night — a dark room's
// one instrument. Colon between HH and MM.
void build_clock(lv_obj_t* scr) {
  const int dw = 92, dh = 156, t = 18, gap = 10, cw = 22;
  const int total = 4 * dw + 3 * gap + cw + 2 * gap;
  const int x0 = (W - total) / 2;
  const int y0 = 78;
  int x = x0;
  for (int i = 0; i < 4; i++) {
    s_digit_box[i] = mk_digit(scr, &s_digit[i], x, y0, dw, dh, t);
    x += dw + gap;
    if (i == 1) {  // colon slot after HH
      s_colon = lv_obj_create(scr);
      lv_obj_set_size(s_colon, cw, dh);
      lv_obj_set_pos(s_colon, x, y0);
      lv_obj_set_style_bg_opa(s_colon, LV_OPA_0, 0);
      lv_obj_set_style_border_width(s_colon, 0, 0);
      lv_obj_clear_flag(s_colon, LV_OBJ_FLAG_SCROLLABLE);
      for (int k = 0; k < 2; k++) {
        lv_obj_t* d = lv_obj_create(s_colon);
        lv_obj_set_size(d, t, t);
        lv_obj_set_pos(d, (cw - t) / 2, dh / 3 + k * dh / 3 - t / 2);
        lv_obj_set_style_radius(d, t / 2, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_bg_color(d, col_text(), 0);
      }
      x += cw + 2 * gap;
    }
  }
}

}  // namespace

void portrait7_ui_create() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, col_bg(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Wash behind everything — the semantic hue when something wants a look,
  // otherwise invisible. A single full-glass rect; opacity carries meaning.
  s_wash = lv_obj_create(scr);
  lv_obj_set_size(s_wash, W, H);
  lv_obj_set_pos(s_wash, 0, 0);
  lv_obj_set_style_border_width(s_wash, 0, 0);
  lv_obj_set_style_radius(s_wash, 0, 0);
  lv_obj_set_style_bg_opa(s_wash, LV_OPA_0, 0);
  lv_obj_clear_flag(s_wash, LV_OBJ_FLAG_SCROLLABLE);

  build_clock(scr);

  s_date = mk_label(scr, font_body(), col_muted());
  lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, 250);

  s_state = mk_label(scr, font_title(), col_ok());
  lv_obj_align(s_state, LV_ALIGN_TOP_MID, 0, 288);

  s_link = mk_label(scr, font_caption(), col_faint());
  lv_obj_align(s_link, LV_ALIGN_TOP_MID, 0, 330);

  // The living canary, the column's emotional anchor.
  s_bird = canary_mark_create(scr, 128);
  lv_obj_align(s_bird, LV_ALIGN_TOP_MID, 0, 360);

  // Witness list: dot + name (left), state word (right), worst at top.
  const int y0 = LIST_Y0, pitch = LIST_PITCH;
  for (int i = 0; i < ROWS; i++) {
    const int y = y0 + i * pitch;
    s_dot[i] = lv_obj_create(scr);
    lv_obj_set_size(s_dot[i], 14, 14);
    lv_obj_set_style_radius(s_dot[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot[i], 0, 0);
    lv_obj_set_style_pad_all(s_dot[i], 0, 0);
    lv_obj_align(s_dot[i], LV_ALIGN_TOP_LEFT, 40, y + 6);
    lv_obj_add_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
    s_name[i] = mk_label(scr, font_label(), col_text());
    lv_obj_align(s_name[i], LV_ALIGN_TOP_LEFT, 66, y);
    lv_obj_add_flag(s_name[i], LV_OBJ_FLAG_HIDDEN);
    s_word[i] = mk_label(scr, font_label(), col_muted());
    lv_obj_align(s_word[i], LV_ALIGN_TOP_RIGHT, -40, y);
    lv_obj_add_flag(s_word[i], LV_OBJ_FLAG_HIDDEN);
  }

#ifdef CD_NIGHTSTAND7
  // Bedside foot: the day's weather + how the bedroom actually feels, stacked
  // above the glance line. Day-only; night strips the column to clock + state.
  s_wx = mk_label(scr, font_label(), col_muted());
  lv_obj_align(s_wx, LV_ALIGN_BOTTOM_MID, 0, -78);
  s_comfort = mk_label(scr, font_caption(), col_faint());
  lv_obj_align(s_comfort, LV_ALIGN_BOTTOM_MID, 0, -52);
#endif

  s_glance = mk_label(scr, font_body(), col_muted());
  lv_obj_align(s_glance, LV_ALIGN_BOTTOM_MID, 0, -24);

  canary_mark_mood(CanaryMood::Idle);
}

void portrait7_ui_update(const Fleet& fleet, uint32_t now,
                         const Portrait7State& st) {
  if (!s_state) return;
  // Two distinct signals: `night` is the red-shift LOOK preference (which
  // palette), `quiet` is the real quiet-hours state (whether to go dark).
  // Dark-when-safe must key off quiet hours, never the color preference —
  // st.night is night && red_shift, so a plain-look user would otherwise
  // keep the bird, list and weather lit all night. character_night() is the
  // schedule-driven flag main.cpp sets, the same one the landscape face uses.
  const bool night = st.night;
  const bool quiet = character_night();
  const Sev worst = fleet.worst(now);
  const bool link_down = !st.wifi_ok || !st.mqtt_ok;

  // Clock + colon.
  const lv_color_t clk = night ? ncol_text() : col_text();
  const int hh = st.time_valid ? st.clock_hh : -1;
  const int mm = st.time_valid ? st.clock_mm : -1;
  set_digit(&s_digit[0], hh < 0 ? -1 : hh / 10, clk, quiet);
  set_digit(&s_digit[1], hh < 0 ? -1 : hh % 10, clk, quiet);
  set_digit(&s_digit[2], mm < 0 ? -1 : mm / 10, clk, quiet);
  set_digit(&s_digit[3], mm < 0 ? -1 : mm % 10, clk, quiet);
  if (s_colon) {
    lv_obj_t* c0 = lv_obj_get_child(s_colon, 0);
    lv_obj_t* c1 = lv_obj_get_child(s_colon, 1);
    if (c0) lv_obj_set_style_bg_color(c0, clk, 0);
    if (c1) lv_obj_set_style_bg_color(c1, clk, 0);
  }

  // Date line.
  if (s_date) {
    if (st.time_valid) {
      time_t t = time(nullptr);
      struct tm lt;
      localtime_r(&t, &lt);
      char buf[24];
      // %d (zero-padded) not %-d: the glibc no-pad flag isn't in ESP newlib.
      strftime(buf, sizeof(buf), "%a %b %d", &lt);
      lv_label_set_text(s_date, buf);
    } else {
      lv_label_set_text(s_date, "setting the clock...");
    }
    lv_obj_set_style_text_color(s_date, night ? ncol_muted() : col_muted(), 0);
  }

  // Household state word, in the state's own hue.
  lv_label_set_text(s_state, state_word(worst));
  lv_obj_set_style_text_color(s_state, sev_color(worst, night), 0);

  // Wash: the semantic hue rising behind everything when attention is wanted.
  if (s_wash) {
    if ((uint8_t)worst >= (uint8_t)Sev::Warn) {
      lv_obj_set_style_bg_color(s_wash, sev_color(worst, night), 0);
      lv_obj_set_style_bg_opa(s_wash, night ? LV_OPA_20 : LV_OPA_10, 0);
    } else {
      lv_obj_set_style_bg_opa(s_wash, LV_OPA_0, 0);
    }
  }

  // Link/health line — name the cause, never just the symptom.
  if (s_link) {
    if (link_down) {
      lv_label_set_text(s_link, !st.wifi_ok
                                    ? (st.wifi_reason ? st.wifi_reason : "wifi down")
                                    : "hub link down");
      lv_obj_set_style_text_color(s_link, sev_color(Sev::Warn, night), 0);
    } else if (st.acked) {
      lv_label_set_text(s_link, "acknowledged");
      lv_obj_set_style_text_color(s_link, night ? ncol_muted() : col_muted(), 0);
    } else {
      lv_label_set_text(s_link, "");
    }
  }

  // Quiet hours strip the column to the clock + the honest state channel;
  // the bird sleeps and the witness list stands down (dark-when-safe, keyed
  // on the schedule, not the red-shift look).
  const bool show_list = !quiet;
  if (s_bird) {
    if (quiet) lv_obj_add_flag(s_bird, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_clear_flag(s_bird, LV_OBJ_FLAG_HIDDEN);
  }
  canary_mark_mood(quiet ? CanaryMood::Asleep : st.bird);

  // Witness list. When the fleet overflows the cap, the LAST shown slot
  // becomes a "+N more" summary (name only) rather than a separate label
  // below the list — that kept the overflow row inside the list band, off
  // the bedside weather foot and the glance line.
  if (show_list) {
    int order[CD_FLEET_MAX_DEVICES];
    const int total = order_by_severity(fleet, now, order,
                                        (int)(sizeof(order) / sizeof(order[0])));
    const bool overflow = total > LIST_CAP;
    const int witnesses = overflow ? LIST_CAP - 1 : total;  // reserve a slot
    for (int i = 0; i < ROWS; i++) {
      if (i < witnesses) {
        const Witness* w = fleet.at(order[i]);
        const Sev s = w ? fleet.witness_sev(*w, now) : Sev::Ok;
        lv_obj_set_style_bg_color(s_dot[i], sev_color(s, false), 0);
        lv_label_set_text_fmt(s_name[i], "%.14s", w ? Fleet::display_name(*w) : "-");
        lv_obj_set_style_text_color(
            s_name[i], (uint8_t)s >= (uint8_t)Sev::Warn ? col_text() : col_muted(), 0);
        lv_label_set_text(s_word[i], state_word(s));
        lv_obj_set_style_text_color(s_word[i], sev_color(s, false), 0);
        lv_obj_clear_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_name[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_word[i], LV_OBJ_FLAG_HIDDEN);
      } else if (overflow && i == witnesses) {
        lv_label_set_text_fmt(s_name[i], "+%d more", total - witnesses);
        lv_obj_set_style_text_color(s_name[i], col_muted(), 0);
        lv_obj_add_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_name[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_word[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_name[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_word[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
  } else {
    for (int i = 0; i < ROWS; i++) {
      lv_obj_add_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_name[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_word[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  // Glance line — honesty first, then the count. The ambient-life status
  // glance brightens it briefly, then it settles back to muted.
  if (s_glance) {
    const bool bright = (int32_t)(now - s_glance_bright_until) < 0;
    if (quiet) {
      lv_label_set_text(s_glance, "");
    } else if (link_down) {
      lv_label_set_text(s_glance, "not everything is reporting in");
      lv_obj_set_style_text_color(s_glance, sev_color(Sev::Warn, false), 0);
    } else if (fleet.count() == 0) {
      lv_label_set_text(s_glance, "no canaries yet");
      lv_obj_set_style_text_color(s_glance, col_muted(), 0);
    } else {
      lv_label_set_text_fmt(s_glance, "%d %s \xE2\x80\xA2 online", fleet.count(),
                            fleet.count() == 1 ? "canary" : "canaries");
      lv_obj_set_style_text_color(s_glance, bright ? col_text() : col_muted(), 0);
    }
  }
#ifdef CD_NIGHTSTAND7
  // Bedside foot: the day's weather + how the room feels, from the same hub
  // source the landscape bedside face reads. Day-only and only when this
  // really is the bedside personality; night blacks them out with the rest.
  if (s_wx && s_comfort) {
    if (quiet || !st.bedside) {
      lv_label_set_text(s_wx, "");
      lv_label_set_text(s_comfort, "");
    } else {
      canary::care::BedsideWeather wx;
      char line[80];
      if (canary::care::bedside_weather(&wx)) {
        size_t o = 0;
        if (wx.t_now_c10 > -9990) {
          const int a = wx.t_now_c10 < 0 ? -wx.t_now_c10 : wx.t_now_c10;
          o += (size_t)snprintf(line + o, sizeof(line) - o, "%s%d.%d\xC2\xB0",
                                wx.t_now_c10 < 0 ? "-" : "", a / 10, a % 10);
        }
        if (wx.cond && wx.cond[0] && o < sizeof(line))
          o += (size_t)snprintf(line + o, sizeof(line) - o, "%s%s",
                                o ? " \xE2\x80\xA2 " : "", wx.cond);
        if (o < sizeof(line))
          snprintf(line + o, sizeof(line) - o, "%s%d\xC2\xB0/%d\xC2\xB0",
                   o ? " \xE2\x80\xA2 " : "",
                   (wx.hi_c10 + (wx.hi_c10 >= 0 ? 5 : -5)) / 10,
                   (wx.lo_c10 + (wx.lo_c10 >= 0 ? 5 : -5)) / 10);
        lv_label_set_text(s_wx, line);
      } else {
        lv_label_set_text(s_wx, "");
      }
      lv_label_set_text(
          s_comfort,
          canary::care::bedside_comfort_line(fleet, line, sizeof(line)) ? line
                                                                        : "");
    }
  }
#else
  (void)st.bedside;  // the wall column is fleet-first; no weather foot
#endif
}

void portrait7_ui_ack_hold(bool) {
  // No dedicated ack ring on this column yet; the hold still acknowledges via
  // main.cpp's household path. Kept for interface symmetry with the other
  // faces (main calls ui_ack_hold uniformly).
}

void portrait7_ui_life_glance(uint32_t now) {
  s_glance_bright_until = now + 4000;
}

}  // namespace canary::ui

#endif  // CD_FLAVOR_DASH
