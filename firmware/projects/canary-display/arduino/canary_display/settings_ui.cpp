// src/ui/settings_ui.cpp — the on-glass settings panel + black-point wizard.
// See settings_ui.h and docs/hardware/display_settings.md.
//
// Design contract (settings wave): five principles — Zero Layer (two levels
// deep, ever), The Screen Is the Preview (brightness applies as you drag),
// One Screen One Decision (each editor holds exactly one value), Defaults
// You Can Come Home To (visible reset that spares the calibration), Night Is
// a Mode (schedule + glow + look + peek travel together) — rendered in the
// grammar every phone already taught: a navigation bar, grouped inset rows,
// a chevron where a tap opens an editor, a switch where the decision is
// on/off, a check where it is one-of-few, a slider where the screen is the
// preview, and a one-line footer under the group saying what it costs.
//
// One tree, one renderer: the layout is measured from the live canvas
// (compute_metrics), so the round 240 puck, the 240x280 and 450x600
// portraits, the 800x480 dash (a centered sheet) and its 480x800 column all
// render the same rows at their own size. Input is LVGL-native (main.cpp
// feeds the pointer device only while this is open — settings_ui.h).
#include "flavor_config.h"
#include <Arduino.h>
#include <lvgl.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "settings_ui.h"

#if defined(FEATURE_TOUCH) && FEATURE_TOUCH

#if (defined(FEATURE_DEVMODE) && FEATURE_DEVMODE) ||       \
    (defined(FEATURE_DEMO_MODE) && FEATURE_DEMO_MODE) ||   \
    (defined(FEATURE_DEBUG_MODE) && FEATURE_DEBUG_MODE) || \
    (defined(FEATURE_ARCADE) && FEATURE_ARCADE)
// The modes doorway (docs/hardware/display_modes.md): Settings lists the
// non-fleet gears this build carries; entering one is a confirm-gated
// latch-and-reboot through the mode glue (which owns the NVS grammar).
#define CD_SET_MODES 1
#include "mode_glue.h"
#endif
// Arcade is dash-first (mode_glue compiles its gear on the dash only);
// the Settings row must not offer a gear the dispatcher can't enter.
#if defined(FEATURE_ARCADE) && FEATURE_ARCADE && defined(CD_FLAVOR_DASH)
#define CD_SET_ROW_ARCADE 1
#endif
// The mic-bearing dash (4.3C, display_mic_variant.md): the opt-in listening
// switch rides Settings exactly like the siren arm — off by default, NVS'd.
#if defined(FEATURE_MIC_ALARM) && FEATURE_MIC_ALARM && \
    defined(HAS_MICROPHONE) && HAS_MICROPHONE
#define CD_SET_MIC 1
#include "mic_alarm.h"
#endif

#if defined(CD_FLAVOR_DASH) && defined(FEATURE_STANDALONE_WEATHER) && \
    FEATURE_STANDALONE_WEATHER && defined(FEATURE_HUB_WEATHER) && \
    FEATURE_HUB_WEATHER
// The standalone-weather opt-in rides Settings like the siren and the mic:
// off by default, honest about its gates. The fetcher itself is absent from
// the emulator build, so status degrades to the stored knobs there.
#define CD_SET_WX 1
#if !defined(EMU_BUILD_FLAVOR)
#include "wx_direct.h"
#endif
#endif

// The PWM / panel-dimmed glass family (everything but the binary-backlight
// RGB dash): a driven day level, a calibrated night glow, and the wizard
// that finds this panel's black point. The dash carries a rendered dim and
// the orientation / clock rows instead.
#ifndef CD_FLAVOR_DASH
#define CD_SET_PWM 1
#endif
// The round 1.28" puck: the one glass whose corners the bezel eats.
#if defined(CD_FLAVOR_WATCH) && !defined(CD_FLAVOR_NIGHTSTAND)
#define CD_SET_ROUND 1
#endif

#include <WiFi.h>                    // localIP for the network page
#include "commission_ui.h"
#include "help_verdict.h"   // the "get help" QR's pure verdict → URL
#include "lvgl_port.h"     // canvas size, rotation, dim, touch feed
#include "theme.h"
#include "character.h"
#include "clock_styles.h"
#include "glass_settings.h"
#include "display.h"
#include "runtime_config.h"   // the wifi-forget doorway
#include "mqtt_mgr.h"      // hub link state
#include "ota_mgr.h"      // version + signed OTA facade
#include "wifi_mgr.h"     // live link state for the network page
#include "hostname.h"     // the glass's .local name (one recipe)
#include "wifi_join_policy.h"  // join_failure_label (shared words)
#if !defined(EMU_BUILD_FLAVOR) && defined(FEATURE_MDNS_DISCOVERY) && \
    FEATURE_MDNS_DISCOVERY
#include "discovery.h"    // discovery_up — is the .local name real
#endif
#include "pins.h"                    // HAS_ISOLATED_IO (board -I path)
#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
#include "field_io.h"      // siren arm/disarm — 4.3B isolated output
#endif

namespace canary::ui {

using namespace canary::glass;

namespace {

// ── Layout metrics: measured from the live canvas, never from a flavor ──
// Two classes. Compact (under 300 px wide: the 240 puck, the 240x280 stick)
// stacks a row's value under its title in caption type; Regular (450x600
// AMOLED, the dash and its portrait column) sets the value inline on the
// right, as a phone does. The dash landscape poster gets a centered sheet
// so the face stays a poster behind it; every other glass is full-bleed.
struct Metrics {
  int w = 240, h = 240;         // the host canvas (sheet or full glass)
  bool round = false;
  bool sheet = false;
  bool compact = true;
  int nav_h = 44;               // navigation bar
  int row_h = 44;               // one row (≥ 44 px: the thumb's floor)
  int inset = 12;               // list edge → group edge
  int radius = 10;              // group corners
  int pad_x = 14;               // group edge → text
  int gap = 18;                 // between groups
  int sw_w = 46, sw_h = 26;     // switch
  int roller_w = 92;            // one hour roller
  int knob = 8;                 // slider knob radius pad
};
Metrics M;

void compute_metrics() {
  const int W = lvgl_port_width();
  const int H = lvgl_port_height();
#ifdef CD_SET_ROUND
  M.round = true;
#else
  M.round = false;
#endif
  M.compact = W < 300;
  M.sheet = !M.compact && W >= 640 && W > H;
  M.w = M.sheet ? 620 : W;
  M.h = M.sheet ? 440 : H;
  if (M.compact) {
    M.nav_h = M.round ? 48 : 44;
    M.row_h = 44;
    // The disc: 30 px in leaves 180 px rows, which clear the rim from the
    // nav bar down to the last fully visible row (chord math in
    // round_frame_core.h; y=48 offers 186 px).
    M.inset = M.round ? 30 : 12;
    M.radius = 10;
    M.pad_x = 14;
    M.gap = 18;
    M.sw_w = 46; M.sw_h = 26;
    M.roller_w = 92;
    M.knob = 8;
  } else {
    M.nav_h = 60;
    M.row_h = 56;
    M.inset = M.sheet ? 20 : 24;
    M.radius = 14;
    M.pad_x = 18;
    M.gap = 26;
    M.sw_w = 58; M.sw_h = 32;
    M.roller_w = 150;
    M.knob = 11;
  }
}

int row_w() { return M.w - 2 * M.inset; }

// ── Pages ────────────────────────────────────────────────────────────────
enum class Page {
  Root,
  Bright,        // day light (PWM glass) / rendered brightness (dash)
  Night,         // PWM glass: the night glow level above the floor
  Hours,         // quiet hours: two rollers
  Screen,        // at night: glow / go dark + the peek window
  Style,         // the Character picker
  Clock,         // dash: 24-hour switch + the clock-face ring
  Display,       // dash: orientation (live)
  Network,       // read-only: wifi · signal · hub · address
  Weather,       // dash + standalone weather: the opt-in and its gates
  Firmware,      // installed · available · auto-update · the one action
  Mic,           // 4.3C: listening · sensitivity · wake on sound
  MicSens,       // 4.3C: the room preset
  HelpQr,        // the Help Desk QR for the current verdict
  CalIntro,      // PWM glass — the black-point wizard
  CalDescend,
  CalComfort,
  CalBlink,
  CalWarn,
  CalDone,
  ResetConfirm,  // confirm-gated: the screen's defaults (calibration stays)
  NetForget,     // confirm-gated: forget WiFi, reboot into the join wizard
  ModesList,     // the non-fleet gears this build carries
  ModeConfirm,   // confirm-gated: latch the chosen gear + reboot into it
};

// Control ids, carried as each object's event user_data.
enum : int {
  ID_NONE = 0,
  ID_NAV,                        // the navigation bar: back, or Done on the root
  ID_ROW_BRIGHT, ID_ROW_STYLE, ID_ROW_CLOCK, ID_ROW_DISPLAY,
  ID_ROW_HOURS, ID_ROW_NIGHT, ID_ROW_LOOK, ID_ROW_SCREEN, ID_ROW_CAL,
  ID_ROW_NET, ID_ROW_WX, ID_ROW_FW, ID_ROW_SIREN, ID_ROW_MIC,
  ID_ROW_ADD, ID_ROW_HELP, ID_ROW_DEV, ID_ROW_RESET, ID_ROW_FORGET,
  ID_SLIDER, ID_ROLL_START, ID_ROLL_END,
  ID_SCREEN_GLOW, ID_SCREEN_OFF, ID_PEEK_3, ID_PEEK_5, ID_PEEK_10,
  ID_CLOCK_24H, ID_FW_AUTO, ID_FW_CHECK, ID_FW_INSTALL, ID_WX_ON,
  ID_MIC_ON, ID_MIC_SENS, ID_MIC_WAKE,
  ID_GO, ID_YES, ID_NO,
  ID_MODE_BENCH, ID_MODE_DEMO, ID_MODE_DEBUG, ID_MODE_ARCADE,
  ID_OPT_BASE = 100,             // + i: the i-th option of the open picker
};

lv_obj_t* s_prev = nullptr;      // the face to return to
lv_obj_t* s_scr = nullptr;       // our own screen while open
lv_obj_t* s_host = nullptr;      // content parent (screen, or the dash sheet)
lv_obj_t* s_nav = nullptr;       // navigation bar
lv_obj_t* s_list = nullptr;      // the scrolling list
lv_obj_t* s_group = nullptr;     // the group being built
lv_obj_t* s_prev_row = nullptr;  // last row in the group (gets the hairline)
bool s_list_first = true;        // nothing in the list yet (no leading gap)
Page s_page = Page::Root;
int  s_root_scroll = 0;          // where the root was, for the way back
bool s_owns_backlight = false;
uint32_t s_last_touch_ms = 0;

// The panel's own gesture bookkeeping (main.cpp feeds every sample).
bool s_down = false;
bool s_hold_fired = false;
bool s_hold_ok = false;          // the press began off any control
int16_t s_down_x = 0, s_down_y = 0;
uint32_t s_down_ms = 0;
constexpr int HOLD_SLOP_PX = 12; // moved past this: a drag, never a hold

#ifdef CD_SET_ROUND
constexpr uint32_t IDLE_CLOSE_MS = 60000;
#else
constexpr uint32_t IDLE_CLOSE_MS = 120000;
#endif

// Wizard timer + clock: touched by every flavor's close paths, so they live
// outside the PWM gate; the rest of the wizard state sits inside it.
lv_timer_t* s_cal_timer = nullptr;
lv_obj_t* s_cal_clock = nullptr;
#ifdef CD_SET_PWM
int  s_night_lvl = 4;            // 1..NIGHT_STEPS while editing
uint16_t s_cal_duty = 0;
uint16_t s_cal_floor = 0;
int  s_cal_blinks = 0;
int  s_cal_retries = 0;
bool s_cal_lit = false;
bool s_cal_persisted = true;     // false = floor kept for tonight only
#endif

void build(Page pg);
void dispatch(int id);

// ── Small helpers ────────────────────────────────────────────────────────

void* id_arg(int id) { return (void*)(intptr_t)id; }
int   id_of(lv_event_t* e) { return (int)(intptr_t)lv_event_get_user_data(e); }
lv_obj_t* target_of(lv_event_t* e) {
  return (lv_obj_t*)lv_event_get_target(e);
}

lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* f, lv_color_t c) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, "");
  return l;
}

// A bare box: no chrome, no scroll — the building block of every row.
lv_obj_t* mk_box(lv_obj_t* parent) {
  lv_obj_t* b = lv_obj_create(parent);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_set_style_radius(b, 0, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_0, 0);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  return b;
}

void str_upper(char* dst, size_t cap, const char* src) {
  size_t i = 0;
  for (; src[i] && i + 1 < cap; i++) dst[i] = (char)toupper((unsigned char)src[i]);
  dst[i] = '\0';
}

// Every control routes here on release. The guard matters: a screen that is
// fading out still holds live objects for MOTION_PAGE_MS, and LVGL delivers
// the release that ended a long-press exit to whatever was under the thumb.
void on_click(lv_event_t* e) {
  if (!s_scr) return;
  dispatch(id_of(e));
}

void set_owns_backlight(bool owns) { s_owns_backlight = owns; }

void clear_host() {
  // Wizard timers never outlive their page.
  if (s_cal_timer) {
    lv_timer_del(s_cal_timer);
    s_cal_timer = nullptr;
  }
  s_cal_clock = nullptr;
  lv_obj_clean(s_host);
  s_nav = s_list = s_group = s_prev_row = nullptr;
  s_list_first = true;
}

// The Character's ground under the panel (screen + sheet). A Character flip
// restyles the OPEN surface live — the screen is the preview, taken
// literally: the ground under your thumb changes, then the page rebuild
// repaints everything on top of it.
void restyle_open_surface() {
  if (s_scr) {
    // Behind a sheet the poster's ground is the surface tier, so the sheet
    // (on the true ground) reads as a card lifted off it. Full-bleed glass
    // is the ground itself.
    lv_obj_set_style_bg_color(s_scr, M.sheet ? col_surface() : col_bg(), 0);
  }
  if (s_host && M.sheet) {
    lv_obj_set_style_bg_color(s_host, col_bg(), 0);
    lv_obj_set_style_border_color(s_host, col_edge(), 0);
  }
}

// Size the host to the (possibly re-oriented) canvas. Called on open and
// after a live rotation, when the whole frame changes shape under the panel.
void layout_host() {
  compute_metrics();
  if (M.sheet) {
    lv_obj_set_size(s_host, M.w, M.h);
    lv_obj_align(s_host, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_host, 16, 0);
    lv_obj_set_style_border_width(s_host, 1, 0);
    lv_obj_set_style_bg_opa(s_host, LV_OPA_COVER, 0);
  } else {
    lv_obj_set_size(s_host, M.w, M.h);
    lv_obj_set_pos(s_host, 0, 0);
    lv_obj_set_style_radius(s_host, 0, 0);
    lv_obj_set_style_border_width(s_host, 0, 0);
    lv_obj_set_style_bg_opa(s_host, LV_OPA_0, 0);
  }
  restyle_open_surface();
}

#ifdef CD_SET_PWM
uint16_t cal_floor_or_default() {
  return nightcal().valid ? nightcal().floor_duty : NIGHT_FLOOR_DFLT;
}

void preview_night_level(int lvl) {
  const uint16_t d = night_step_duty(cal_floor_or_default(), lvl);
  canary::hal::backlight_night_set(d);
}
#endif

void fmt_clock(char* out, size_t cap) {
  time_t t = time(nullptr);
  if (t < 1700000000) {
    snprintf(out, cap, "12:34");
    return;
  }
  struct tm lt;
  localtime_r(&t, &lt);
  snprintf(out, cap, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

// ── The page skeleton: navigation bar + list ─────────────────────────────

// Navigation bar. Rectangular glass: "‹ Back" on the left in the accent,
// the page title centered, "Done" on the right of the root. The disc gets
// one centered line — "‹ Title" (leave means up) or "Settings" (the whole
// bar closes) — because its top chord is 126 px wide and a left-aligned
// back label would sit under the rim. The whole bar is one tap target.
// inert = a title only (the wizard's middle steps have one way forward).
void mk_nav(const char* title, const char* back, bool inert = false) {
  s_nav = mk_box(s_host);
  lv_obj_set_size(s_nav, M.w, M.nav_h);
  lv_obj_set_pos(s_nav, 0, 0);
  if (inert) {
    lv_obj_clear_flag(s_nav, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* t = mk_label(s_nav, font_body(), col_text());
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, M.round ? 2 : 0);
    return;
  }
  lv_obj_add_event_cb(s_nav, on_click, LV_EVENT_CLICKED, id_arg(ID_NAV));
  if (M.round) {
    lv_obj_t* t = mk_label(s_nav, font_body(), back ? col_accent() : col_text());
    if (back) lv_label_set_text_fmt(t, LV_SYMBOL_LEFT "  %s", title);
    else lv_label_set_text(t, title);
    lv_obj_set_width(t, 126);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 2);
    return;
  }
  lv_obj_t* t = mk_label(s_nav, font_body(), col_text());
  lv_label_set_text(t, title);
  lv_obj_set_width(t, M.w / 2);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);
  if (back) {
    lv_obj_t* b = mk_label(s_nav, font_body(), col_accent());
    lv_label_set_text_fmt(b, LV_SYMBOL_LEFT "  %s", back);
    lv_obj_set_width(b, M.w / 4);
    lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, M.inset, 0);
  } else {
    lv_obj_t* d = mk_label(s_nav, font_body(), col_accent());
    lv_label_set_text(d, "Done");
    lv_obj_align(d, LV_ALIGN_RIGHT_MID, -M.inset, 0);
  }
}

// The list under the bar: a vertical flex column with the inset the groups
// hang from. scrolls=false parks it for pages whose one control is itself a
// drag (sliders, rollers) — they always fit, and a list that also scrolls
// would fight the thumb.
void mk_list(bool scrolls) {
  s_list = mk_box(s_host);
  lv_obj_set_pos(s_list, 0, M.nav_h);
  lv_obj_set_size(s_list, M.w, M.h - M.nav_h);
  lv_obj_set_style_pad_left(s_list, M.inset, 0);
  lv_obj_set_style_pad_right(s_list, M.inset, 0);
  lv_obj_set_style_pad_top(s_list, 2, 0);
  // The disc: room to bring the last row up out of the bottom rim.
  lv_obj_set_style_pad_bottom(s_list, M.round ? 56 : M.gap, 0);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_list, 0, 0);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_color(s_list, col_faint(), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(s_list, LV_OPA_60, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(s_list, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(s_list, 2, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_right(s_list, 2, LV_PART_SCROLLBAR);
  if (scrolls) lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
  else lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
  s_list_first = true;
}

void page_begin(const char* title, const char* back, bool scrolls = true) {
  clear_host();
  mk_nav(title, back);
  mk_list(scrolls);
}

// A little vertical air in the list (between groups without a header).
void spacer(int h) {
  lv_obj_t* s = mk_box(s_list);
  lv_obj_set_size(s, LV_PCT(100), h);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);
}

// ── Groups: header · rounded surface of rows · footer ────────────────────

void group_begin(const char* header) {
  if (header) {
    char up[32];
    str_upper(up, sizeof(up), header);
    lv_obj_t* h = mk_label(s_list, font_caption(), col_muted());
    lv_label_set_text(h, up);
    lv_obj_set_style_text_letter_space(h, 1, 0);
    lv_obj_set_width(h, LV_PCT(100));
    lv_obj_set_style_pad_left(h, M.pad_x, 0);
    lv_obj_set_style_pad_top(h, s_list_first ? 6 : M.gap, 0);
    lv_obj_set_style_pad_bottom(h, 6, 0);
  } else if (!s_list_first) {
    spacer(M.gap);
  } else {
    spacer(6);
  }
  s_list_first = false;
  s_group = mk_box(s_list);
  lv_obj_set_width(s_group, LV_PCT(100));
  lv_obj_set_height(s_group, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_group, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_group, 0, 0);
  lv_obj_set_style_bg_color(s_group, col_surface(), 0);
  lv_obj_set_style_bg_opa(s_group, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_group, M.radius, 0);
  lv_obj_set_style_clip_corner(s_group, true, 0);
  s_prev_row = nullptr;
}

void group_end(const char* footer) {
  if (footer) {
    lv_obj_t* f = mk_label(s_list, font_caption(), col_faint());
    lv_label_set_text(f, footer);
    lv_label_set_long_mode(f, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(f, LV_PCT(100));
    lv_obj_set_style_pad_left(f, M.pad_x, 0);
    lv_obj_set_style_pad_right(f, M.pad_x, 0);
    lv_obj_set_style_pad_top(f, 8, 0);
  }
  s_group = nullptr;
  s_prev_row = nullptr;
}

// One row. The hairline between rows starts at the text inset, as a phone
// draws it, and is added to the PREVIOUS row when the next one arrives — so
// the last row of a group never carries one.
lv_obj_t* mk_row(int id) {
  if (s_prev_row) {
    lv_obj_t* hl = mk_box(s_prev_row);
    lv_obj_set_size(hl, row_w() - M.pad_x, 1);
    lv_obj_align(hl, LV_ALIGN_BOTTOM_LEFT, M.pad_x, 0);
    lv_obj_set_style_bg_color(hl, col_edge(), 0);
    lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, 0);
    lv_obj_clear_flag(hl, LV_OBJ_FLAG_CLICKABLE);
  }
  lv_obj_t* r = mk_box(s_group);
  lv_obj_set_size(r, LV_PCT(100), M.row_h);
  if (id != ID_NONE) {
    // Press feedback: the edge tier, only while the thumb is down.
    lv_obj_set_style_bg_color(r, col_edge(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(r, on_click, LV_EVENT_CLICKED, id_arg(id));
  }
  s_prev_row = r;
  return r;
}

// Title (+ value): inline on Regular glass, stacked on Compact — a 180 px
// row cannot hold "Screen at Night" and "Glow" side by side in body type,
// and a watch face has always answered that by putting the value underneath.
// right_reserve = pixels the row's right edge already spends (chevron,
// switch, check) so the text never runs under them.
void mk_title_value(lv_obj_t* row, const char* title, const char* value,
                    int right_reserve, lv_color_t title_col, lv_color_t value_col) {
  const int avail = row_w() - 2 * M.pad_x - right_reserve;
  if (M.compact) {
    lv_obj_t* t = mk_label(row, font_body(), title_col);
    lv_label_set_text(t, title);
    lv_obj_set_width(t, avail);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    if (value && value[0]) {
      lv_obj_align(t, LV_ALIGN_LEFT_MID, M.pad_x, -8);
      lv_obj_t* v = mk_label(row, font_caption(), value_col);
      lv_label_set_text(v, value);
      lv_obj_set_width(v, avail);
      lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
      lv_obj_align(v, LV_ALIGN_LEFT_MID, M.pad_x, 10);
    } else {
      lv_obj_align(t, LV_ALIGN_LEFT_MID, M.pad_x, 0);
    }
    return;
  }
  lv_obj_t* t = mk_label(row, font_body(), title_col);
  lv_label_set_text(t, title);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, M.pad_x, 0);
  if (value && value[0]) {
    lv_obj_t* v = mk_label(row, font_label(), value_col);
    lv_label_set_text(v, value);
    const int vw = avail / 2;
    lv_obj_set_width(t, avail - vw - 8);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v, vw);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -(M.pad_x + right_reserve), 0);
  } else {
    lv_obj_set_width(t, avail);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  }
}

int chevron_reserve() { return M.compact ? 18 : 24; }

// A row that opens an editor: title, its current value, a chevron.
void row_nav(const char* title, const char* value, int id) {
  lv_obj_t* r = mk_row(id);
  mk_title_value(r, title, value, chevron_reserve(), col_text(), col_muted());
  lv_obj_t* c = mk_label(r, font_label(), col_faint());
  lv_label_set_text(c, LV_SYMBOL_RIGHT);
  lv_obj_align(c, LV_ALIGN_RIGHT_MID, -M.pad_x, 0);
}

// A read-only fact: title and value, nothing to tap.
void row_info(const char* title, const char* value, bool highlight = false) {
  lv_obj_t* r = mk_row(ID_NONE);
  mk_title_value(r, title, value, 0, col_text(),
                 highlight ? col_signed() : col_muted());
}

// The switch flips itself and reports; the row's own tap flips it too (the
// whole row is the target — a thumb should not have to find a 46 px pill).
void on_switch(lv_event_t* e) {
  if (!s_scr) return;
  lv_obj_t* sw = target_of(e);
  dispatch(id_of(e) | (lv_obj_has_state(sw, LV_STATE_CHECKED) ? 0x10000 : 0));
}
void on_switch_row(lv_event_t* e) {
  if (!s_scr) return;
  lv_obj_t* row = target_of(e);
  lv_obj_t* sw = nullptr;
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(row); i++) {
    lv_obj_t* c = lv_obj_get_child(row, (int32_t)i);
    if (lv_obj_check_type(c, &lv_switch_class)) { sw = c; break; }
  }
  if (!sw) return;
  const bool on = !lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  else lv_obj_clear_state(sw, LV_STATE_CHECKED);
  dispatch(id_of(e) | (on ? 0x10000 : 0));
}

void row_switch(const char* title, const char* value, bool on, int id,
                bool enabled = true) {
  lv_obj_t* r = mk_row(ID_NONE);
  mk_title_value(r, title, value, M.sw_w + 8,
                 enabled ? col_text() : col_muted(), col_muted());
  lv_obj_t* sw = lv_switch_create(r);
  lv_obj_set_size(sw, M.sw_w, M.sw_h);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -M.pad_x, 0);
  lv_obj_set_style_bg_color(sw, col_edge(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, col_accent(), LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sw, col_text(), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(sw, -3, LV_PART_KNOB);
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  if (!enabled) {
    lv_obj_add_state(sw, LV_STATE_DISABLED);
    lv_obj_clear_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(sw, LV_OPA_50, LV_PART_MAIN);
    return;
  }
  lv_obj_add_event_cb(sw, on_switch, LV_EVENT_VALUE_CHANGED, id_arg(id));
  lv_obj_set_style_bg_color(r, col_edge(), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_add_event_cb(r, on_switch_row, LV_EVENT_CLICKED, id_arg(id));
}

// One-of-few: a check in the accent on the chosen line.
void row_check(const char* title, const char* value, bool checked, int id) {
  lv_obj_t* r = mk_row(id);
  mk_title_value(r, title, value, checked ? chevron_reserve() : 0,
                 col_text(), col_muted());
  if (checked) {
    lv_obj_t* c = mk_label(r, font_label(), col_accent());
    lv_label_set_text(c, LV_SYMBOL_OK);
    lv_obj_align(c, LV_ALIGN_RIGHT_MID, -M.pad_x, 0);
  }
}

// A centered verb. destructive = the alert family (the words carry the
// meaning; the color only agrees). Disabled rows say so in muted type.
enum class Verb { Plain, Destructive, Disabled };
void row_action(const char* title, int id, Verb kind = Verb::Plain) {
  lv_obj_t* r = mk_row(kind == Verb::Disabled ? ID_NONE : id);
  lv_obj_t* t = mk_label(r, font_body(),
                         kind == Verb::Destructive ? col_alert()
                         : kind == Verb::Disabled  ? col_muted()
                                                   : col_accent());
  lv_label_set_text(t, title);
  lv_obj_set_width(t, row_w() - 2 * M.pad_x);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  lv_obj_center(t);
}

// The screen is the preview: a slider that reports every step of the drag.
void on_slider(lv_event_t* e) {
  if (!s_scr) return;
  dispatch(id_of(e) | ((int)lv_slider_get_value(target_of(e)) << 8));
}

void row_slider(int min, int max, int value, int id) {
  lv_obj_t* r = mk_row(ID_NONE);
  lv_obj_t* sl = lv_slider_create(r);
  lv_obj_set_size(sl, row_w() - 2 * M.pad_x - 2 * M.knob, 6);
  lv_obj_center(sl);
  lv_slider_set_range(sl, min, max);
  lv_slider_set_value(sl, value, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sl, col_edge(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, col_accent(), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, col_text(), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, M.knob, LV_PART_KNOB);
  lv_obj_add_event_cb(sl, on_slider, LV_EVENT_VALUE_CHANGED, id_arg(id));
}

// A big number over its editor — the value IS the screen (Nest).
void mk_hero(const char* text, const char* caption) {
  lv_obj_t* h = mk_label(s_list, font_hero(), col_text());
  lv_label_set_text(h, text);
  lv_obj_set_width(h, LV_PCT(100));
  lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(h, M.compact ? 6 : 14, 0);
  if (caption) {
    lv_obj_t* c = mk_label(s_list, font_caption(), col_muted());
    lv_label_set_text(c, caption);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(c, 6, 0);
  }
}

// Body copy for the confirm pages and the wizard — centered, wrapped.
void mk_body(const char* text, lv_color_t col, const lv_font_t* f) {
  lv_obj_t* b = mk_label(s_list, f, col);
  lv_label_set_text(b, text);
  lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(b, LV_PCT(100));
  lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(b, M.compact ? 8 : 18, 0);
  lv_obj_set_style_pad_bottom(b, M.compact ? 8 : 18, 0);
  lv_obj_set_style_pad_left(b, 4, 0);
  lv_obj_set_style_pad_right(b, 4, 0);
}
void mk_body(const char* text) { mk_body(text, col_text(), font_body()); }

// A filled button, full width of the list: the one primary verb of a confirm
// page (accent, or the alert family when it destroys something), and the
// quiet "Cancel" under it (surface tier).
enum class Button { Primary, Destructive, Quiet };
void mk_button(const char* text, int id, Button kind) {
  lv_obj_t* b = mk_box(s_list);
  lv_obj_set_size(b, LV_PCT(100), M.row_h);
  lv_obj_set_style_radius(b, M.radius, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_color_t bg = kind == Button::Primary       ? col_accent()
                  : kind == Button::Destructive ? col_alert()
                                                : col_surface();
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_bg_color(b, col_edge(), LV_STATE_PRESSED);
  lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, id_arg(id));
  lv_obj_t* t = mk_label(b, font_body(),
                         kind == Button::Quiet ? col_text() : col_bg());
  lv_label_set_text(t, text);
  lv_obj_center(t);
  spacer(M.compact ? 8 : 12);
}

// ── Wizard timers (PWM glass — the dash panel has no floor to calibrate) ─
#ifdef CD_SET_PWM

void cal_descend_cb(lv_timer_t*) {
  // Step ~18% down each beat: log-spaced, so every step looks like the
  // same-sized dimming to the eye. Two extra beats at the bottom, then
  // conclude the panel glows even at duty 1.
  if (s_cal_duty > 1) {
    uint16_t next = (uint16_t)(((uint32_t)s_cal_duty * 82u) / 100u);
    if (next >= s_cal_duty) next = s_cal_duty - 1;
    if (next < 1) next = 1;
    s_cal_duty = next;
    canary::hal::backlight_night_set(s_cal_duty);
    return;
  }
  if (++s_cal_blinks >= 2) {  // reached 1 and dwelled: floor is the bottom
    s_cal_floor = 1;
    build(Page::CalComfort);
  }
}

void cal_blink_cb(lv_timer_t*) {
  if (s_cal_blinks < 6) {
    s_cal_lit = !s_cal_lit;
    canary::hal::backlight_night_set(s_cal_lit ? s_cal_floor : 0);
    s_cal_blinks++;
    return;
  }
  canary::hal::backlight_night_set(s_cal_floor);
  lv_timer_del(s_cal_timer);
  s_cal_timer = nullptr;
}

#endif  // CD_SET_PWM

// ── Page builders ────────────────────────────────────────────────────────

const char* wifi_row_value(char* buf, size_t cap) {
  // The live link, not just the stored intent: the joined network's name
  // while up, the honest in-between while it retries.
  if (canary::cfg::wifi_is_placeholder()) snprintf(buf, cap, "Not set up");
  else if (canary::net::wifi_connected())
    snprintf(buf, cap, "%.24s", canary::cfg::get().wifi_ssid);
  else snprintf(buf, cap, "Reconnecting");
  return buf;
}

void build_root() {
  page_begin("Settings", nullptr);
  char v[32];
  const Settings& gs = settings();

  // ── Display ──
  group_begin("Display");
#ifdef CD_SET_PWM
  snprintf(v, sizeof(v), "%d%%", gs.day_pct);
#else
  snprintf(v, sizeof(v), "%d%%", gs.bright_pct);
#endif
  row_nav("Brightness", v, ID_ROW_BRIGHT);
  row_nav("Appearance", character_name(active_character()), ID_ROW_STYLE);
#ifdef CD_FLAVOR_DASH
  // The drawn-clock glass: the face ring + 12/24, and how the glass is turned.
  row_nav("Clock", clock_style_name(gs.clock_style), ID_ROW_CLOCK);
  {
    char rn[24];
    snprintf(rn, sizeof(rn), "%s", rotation_name(gs.rotation));
    rn[0] = (char)toupper((unsigned char)rn[0]);
    row_nav("Orientation", rn, ID_ROW_DISPLAY);
  }
#endif
  group_end(nullptr);

  // ── Night (a mode, not a menu: the four travel together) ──
  group_begin("Night");
  snprintf(v, sizeof(v), "%02d:00 - %02d:00", gs.night_start_hh, gs.night_end_hh);
  row_nav("Quiet Hours", v, ID_ROW_HOURS);
#ifdef CD_SET_PWM
  snprintf(v, sizeof(v), "Level %d of %d",
           night_duty_step(cal_floor_or_default(), gs.night_duty), NIGHT_STEPS);
  row_nav("Night Light", v, ID_ROW_NIGHT);
#endif
  row_switch("Red Shift", nullptr, gs.red_shift != 0, ID_ROW_LOOK);
  row_nav("Screen at Night",
          gs.night_screen == NIGHT_SCREEN_OFF ? "Off, tap to peek" : "Low glow",
          ID_ROW_SCREEN);
#ifdef CD_SET_PWM
  row_nav("Find the Black Point", nightcal().valid ? "Calibrated" : "Not yet",
          ID_ROW_CAL);
#endif
  group_end("During quiet hours the glass dims and goes quiet. "
            "An unacknowledged alert always lights it.");

  // ── Connection ──
  group_begin("Connection");
  row_nav("Wi-Fi", wifi_row_value(v, sizeof(v)), ID_ROW_NET);
#ifdef CD_SET_WX
  {
    // The honest one-word state of the standalone forecast: who owns
    // weather right now, not just which way the switch points.
    const char* wxv;
#if !defined(EMU_BUILD_FLAVOR)
    switch (canary::net::wx_direct_status()) {
      case 2:  wxv = "Hub"; break;             // a hub owns weather
      case 1:  wxv = "Needs a location"; break;
      case 3:  wxv = "On"; break;
      case 4:  wxv = "Retrying"; break;
      default: wxv = "Off"; break;
    }
#else
    wxv = gs.wx_direct ? "On" : "Off";
#endif
    row_nav("Weather", wxv, ID_ROW_WX);
  }
#endif
  row_nav("Firmware", canary::net::ota_status().installed, ID_ROW_FW);
  group_end(nullptr);

#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
  // 4.3B only: arm the wired siren (DO0). Disarmed by default — opt-in. The
  // alert always shows on the glass; this only governs the wired output.
  group_begin("Siren");
  row_switch("Wired Siren", nullptr, canary::io::field_io_armed(), ID_ROW_SIREN);
  group_end("Sounds the wired siren on an unacknowledged alert. "
            "The glass shows the alert either way.");
#endif
#ifdef CD_SET_MIC
  // 4.3C only: the mic opt-in. The row IS part of the always-know contract:
  // it states the live state in the same words the chip and console use.
  group_begin("Microphone");
  row_nav("Listening",
          !canary::io::mic_pins_ok() ? "Pins unset"
          : canary::io::mic_listening() ? "On" : "Off",
          ID_ROW_MIC);
  group_end(nullptr);
#endif

  // ── Doorways ──
  group_begin(nullptr);
#ifdef CD_SET_PWM
  // Not a screen setting, but this glass's one always-reachable doorway to
  // commissioning (the dash has its own on the transparency sheet).
  row_nav("Add a Canary", nullptr, ID_ROW_ADD);
#endif
  // Every flavor: the Help Desk QR for whatever is wrong right now. A row,
  // not a banner — help is always reachable, never shouted.
  row_nav("Get Help", nullptr, ID_ROW_HELP);
#ifdef CD_SET_MODES
  // The glass has gears (display_modes.md): the doorway to the non-fleet
  // modes this build carries. One gear keeps the familiar dev-mode row.
#if (defined(FEATURE_DEMO_MODE) && FEATURE_DEMO_MODE) ||   \
    (defined(FEATURE_DEBUG_MODE) && FEATURE_DEBUG_MODE) || \
    (defined(FEATURE_ARCADE) && FEATURE_ARCADE)
  row_nav("Modes", nullptr, ID_ROW_DEV);
#else
  row_nav("Developer Mode", "Bench", ID_ROW_DEV);
#endif
#endif
  group_end(nullptr);

  // ── Reset ──
  group_begin(nullptr);
  row_action("Reset Settings", ID_ROW_RESET, Verb::Destructive);
  // A never-provisioned unit (compiled placeholder, nothing in NVS) has no
  // network to forget; say so instead of offering a no-op.
  if (canary::cfg::wifi_is_placeholder())
    row_action("Wi-Fi Not Set Up", ID_NONE, Verb::Disabled);
  else
    row_action("Forget Wi-Fi Network", ID_ROW_FORGET, Verb::Destructive);
#ifdef CD_SET_PWM
  group_end("Reset keeps your black point. Forgetting Wi-Fi reopens setup.");
#else
  group_end("Forgetting Wi-Fi restarts this glass into setup.");
#endif
}

// Brightness: the screen is the preview. PWM glass drives the day level;
// the dash dims a rendered scrim over an always-on backlight, and bottoms
// out at 50% on purpose (darker is Night's job — a scrim can't lower real
// backlight power, so an all-day floor stays honest).
void build_bright() {
  page_begin("Brightness", "Settings", /*scrolls=*/false);
  char v[8];
#ifdef CD_SET_PWM
  snprintf(v, sizeof(v), "%d%%", settings().day_pct);
  mk_hero(v, "the screen is the preview");
  group_begin(nullptr);
  row_slider(20, 100, settings().day_pct, ID_SLIDER);
  group_end(nullptr);
  set_owns_backlight(true);
  canary::hal::backlight_set(day_level());
#else
  snprintf(v, sizeof(v), "%d%%", settings().bright_pct);
  mk_hero(v, "the screen is the preview");
  group_begin(nullptr);
  row_slider(BRIGHT_PCT_MIN, BRIGHT_PCT_MAX, settings().bright_pct, ID_SLIDER);
  group_end("Dims the picture, not the backlight - it is on or off in "
            "hardware. Night mode goes darker.");
  // Dim to the current setting right now, so the hero and the glass agree.
  lvgl_port_set_dim(bright_scrim_opa(settings().bright_pct));
#endif
}

#ifdef CD_SET_PWM
void build_night() {
  page_begin("Night Light", "Settings", /*scrolls=*/false);
  s_night_lvl = night_duty_step(cal_floor_or_default(), settings().night_duty);
  char v[8], cap[40];
  snprintf(v, sizeof(v), "%d", s_night_lvl);
  snprintf(cap, sizeof(cap), "of %d, shown at this glow", NIGHT_STEPS);
  mk_hero(v, cap);
  group_begin(nullptr);
  row_slider(1, NIGHT_STEPS, s_night_lvl, ID_SLIDER);
  group_end(nightcal().valid
                ? "Level 1 is this panel's calibrated black point."
                : "Find the black point to calibrate level 1 to this panel.");
  set_owns_backlight(true);
  preview_night_level(s_night_lvl);
}
#endif

// Quiet hours: two rollers, the hour wheels every phone alarm taught. Landing
// IS choosing — the schedule updates as the wheel settles.
void on_roller(lv_event_t* e) {
  if (!s_scr) return;
  dispatch(id_of(e) | ((int)lv_roller_get_selected(target_of(e)) << 8));
}

lv_obj_t* mk_hour_roller(lv_obj_t* parent, int hh, int id) {
  static char opts[24 * 6 + 1];
  if (!opts[0]) {
    int n = 0;
    for (int h = 0; h < 24; h++)
      n += snprintf(opts + n, sizeof(opts) - n, "%02d:00%s", h, h < 23 ? "\n" : "");
  }
  lv_obj_t* r = lv_roller_create(parent);
  lv_roller_set_options(r, opts, LV_ROLLER_MODE_INFINITE);
  lv_roller_set_visible_row_count(r, 3);
  lv_obj_set_width(r, M.roller_w);
  lv_obj_set_style_text_font(r, font_body(), LV_PART_MAIN);
  lv_obj_set_style_text_color(r, col_muted(), LV_PART_MAIN);
  lv_obj_set_style_text_line_space(r, M.compact ? 10 : 16, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(r, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_style_border_width(r, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(r, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(r, col_edge(), LV_PART_SELECTED);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
  lv_obj_set_style_radius(r, 8, LV_PART_SELECTED);
  lv_obj_set_style_text_color(r, col_text(), LV_PART_SELECTED);
  lv_roller_set_selected(r, (uint16_t)(hh % 24), LV_ANIM_OFF);
  lv_obj_add_event_cb(r, on_roller, LV_EVENT_VALUE_CHANGED, id_arg(id));
  return r;
}

void build_hours() {
  page_begin("Quiet Hours", "Settings", /*scrolls=*/false);
  const Settings& gs = settings();
  spacer(M.compact ? 4 : 10);
  lv_obj_t* wheels = mk_box(s_list);
  lv_obj_set_width(wheels, LV_PCT(100));
  lv_obj_set_height(wheels, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(wheels, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(wheels, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  const char* names[2] = {"Starts", "Ends"};
  const int hours[2] = {gs.night_start_hh, gs.night_end_hh};
  const int ids[2] = {ID_ROLL_START, ID_ROLL_END};
  for (int i = 0; i < 2; i++) {
    lv_obj_t* col = mk_box(wheels);
    lv_obj_set_width(col, M.roller_w);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 6, 0);
    char up[8];
    str_upper(up, sizeof(up), names[i]);
    lv_obj_t* h = mk_label(col, font_caption(), col_muted());
    lv_label_set_text(h, up);
    lv_obj_set_style_text_letter_space(h, 1, 0);
    mk_hour_roller(col, hours[i], ids[i]);
  }
  lv_obj_t* f = mk_label(s_list, font_caption(), col_faint());
  lv_label_set_text(f, gs.night_start_hh == gs.night_end_hh
                           ? "Same hour twice: night never comes."
                           : "Quiet hours never hide an alert.");
  lv_label_set_long_mode(f, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(f, LV_PCT(100));
  lv_obj_set_style_text_align(f, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(f, M.compact ? 8 : 16, 0);
}

void build_screen() {
  page_begin("Screen at Night", "Settings");
  const Settings& gs = settings();
  const bool off = gs.night_screen == NIGHT_SCREEN_OFF;
  group_begin(nullptr);
  row_check("Keep a Low Glow", nullptr, !off, ID_SCREEN_GLOW);
#ifdef CD_SET_PWM
  row_check("Go Dark", "A tap peeks the clock", off, ID_SCREEN_OFF);
#else
  row_check("Go Dark", "A tap wakes the glass", off, ID_SCREEN_OFF);
#endif
  group_end(off ? "An alert always lights the glass, and so does a dead link "
                  "- silence is never rendered as safety."
                : "The dark look stays lit at the night level. An alert "
                  "always lights the glass.");
  if (off) {
    group_begin("Peek For");
    row_check("3 seconds", nullptr, gs.peek_s == 3, ID_PEEK_3);
    row_check("5 seconds", nullptr, gs.peek_s == 5, ID_PEEK_5);
    row_check("10 seconds", nullptr, gs.peek_s == 10, ID_PEEK_10);
    group_end(nullptr);
  }
}

// The Character picker (display_character.md §7): every stop is a validated
// look, so landing IS choosing — by the time this rebuilds, the palette and
// type ladder already belong to the look being named.
void build_style() {
  page_begin("Appearance", "Settings");
  const Character cur = active_character();
  group_begin(nullptr);
  for (uint8_t i = 0; i < character_count(); i++) {
    const Character c = character_at_ring(i);
    row_check(character_name(c), M.compact ? nullptr : character_caption(c),
              c == cur, ID_OPT_BASE + (int)c);
  }
  group_end(character_caption(cur));
}

#ifdef CD_FLAVOR_DASH
// The clock face: 12/24 lives with the faces it changes; the AM/PM marker
// is deliberately quiet (caption-sized, faint) on every face. The hero
// digits live UNDER this sheet, so the footer says when the new face shows;
// the ground-flip tracker in main.cpp rebuilds the face the moment the
// sheet closes.
void build_clock() {
  page_begin("Clock", "Settings");
  const uint8_t cur = settings().clock_style;
  group_begin(nullptr);
  row_switch("24-Hour Time", nullptr, settings().clock_12h == 0, ID_CLOCK_24H);
  group_end("12-hour keeps a quiet AM/PM under the digits.");
  group_begin("Face");
  for (uint8_t i = 0; i < clock_style_count(); i++)
    row_check(clock_style_name(i), clock_style_caption(i), i == cur,
              ID_OPT_BASE + i);
  group_end("The face wears it when you leave Settings.");
}

// Orientation (7"/dash glass): four quarter turns, landing IS choosing —
// the glass rotates under your thumb the instant you tap (The Screen Is the
// Preview, taken literally). The bench turns the panel; the software follows.
void build_display() {
  page_begin("Orientation", "Settings");
  const uint8_t rot = settings().rotation;
  group_begin(nullptr);
  row_check("Landscape", nullptr, rot == ROT_LANDSCAPE, ID_OPT_BASE + ROT_LANDSCAPE);
  row_check("Portrait", nullptr, rot == ROT_PORTRAIT, ID_OPT_BASE + ROT_PORTRAIT);
  row_check("Landscape, Flipped", nullptr, rot == ROT_LANDSCAPE_INV,
            ID_OPT_BASE + ROT_LANDSCAPE_INV);
  row_check("Portrait, Flipped", nullptr, rot == ROT_PORTRAIT_INV,
            ID_OPT_BASE + ROT_PORTRAIT_INV);
  group_end("Portrait suits a wall column or a tall bedside face. "
            "The layout follows the turn.");
}

#ifdef CD_SET_WX
// The standalone-weather page: one decision (fetch itself / don't), and the
// truth about its gates. The footer carries the whole privacy contract in
// the fewest honest words — what is sent, when it never runs, and where the
// location comes from.
void build_weather() {
  page_begin("Weather", "Settings");
  const bool on = settings().wx_direct != 0;
  group_begin(nullptr);
  row_switch("Fetch Weather Itself", nullptr, on, ID_WX_ON);
  const char* st = nullptr;
#if !defined(EMU_BUILD_FLAVOR)
  char age[24];
  switch (canary::net::wx_direct_status()) {
    case 2: st = "Your hub provides weather"; break;
    case 1: st = "Set a location from the app"; break;
    case 3: {
      const uint16_t m = canary::net::wx_direct_age_min(millis());
      if (m == 0xFFFF) st = "Waiting for the first fetch";
      else { snprintf(age, sizeof(age), "Fetched %um ago", (unsigned)m); st = age; }
      break;
    }
    case 4: st = "Last fetch failed, retrying"; break;
    default: st = "Off"; break;
  }
#else
  st = on ? "On" : "Off";
#endif
  row_info("Status", st);
  group_end("Hub-less homes only: with a hub, the hub stays the one thing "
            "that talks to the internet. Sends a ~11 km coarse location to "
            "a public forecast service. No account, no identifiers.");
}
#endif  // CD_SET_WX
#endif  // CD_FLAVOR_DASH

// The network page: what the glass's own web page says about its link, said
// on the glass itself. Read-only by design — joining is the wizard's job,
// forgetting is reset's — so this page is pure honest state: the joined
// network, the signal in a word, the hub link, and the one address where
// this glass answers on the LAN. It refreshes ~1 Hz while open
// (settings_ui_tick), so carrying the panel around IS the signal survey.
void build_network() {
  page_begin("Wi-Fi", "Settings");
  const bool set_up = !canary::cfg::wifi_is_placeholder();
  const bool up = set_up && canary::net::wifi_connected();
  char v[40];
  group_begin(nullptr);
  row_info("Network", wifi_row_value(v, sizeof(v)), up);
  // Signal in a word, never dBm (theme's signal_word rule); while the link
  // is down the row carries WHY, in the same shared words the onboarding
  // portal and the serial log use — one vocabulary, three surfaces.
  if (up) row_info("Signal", signal_word(canary::net::wifi_rssi()));
  else if (set_up)
    row_info("Signal",
             canary::net::join_failure_label(canary::net::wifi_last_failure()));
  else row_info("Signal", "-");
  row_info("Hub Link",
           canary::net::mqtt_broker_is_placeholder() ? "Not set up"
           : canary::net::mqtt_connected()           ? "Connected"
                                                     : "Retrying",
           canary::net::mqtt_connected());
  group_end(nullptr);

  // The one address that answers: this glass's own page — a live mirror,
  // these settings, and a 3D model of the device you can spin. The .local
  // name is composed by the same recipe mDNS registered (canary/net/
  // hostname.h) — and it is only ever CLAIMED when mDNS actually came up
  // this boot (Codex P2: a failed MDNS.begin left the page advertising a
  // name that cannot resolve). The honest fallback is the numeric IP,
  // which glass_web answers on regardless of naming.
#if defined(EMU_BUILD_FLAVOR)
  const bool named = true;   // the emulated LAN has no real mDNS to fail
#elif defined(FEATURE_MDNS_DISCOVERY) && FEATURE_MDNS_DISCOVERY
  const bool named = canary::net::discovery_up();
#else
  const bool named = false;  // no mDNS in this build — the name would lie
#endif
  if (up || named) {
    char host[48];
    canary::net::make_hostname(canary::cfg::get().device_id, host, sizeof(host));
    String ip = WiFi.localIP().toString();
    char url[72];
    if (named) snprintf(url, sizeof(url), "http://%s.local", host);
    else snprintf(url, sizeof(url), "http://%s", ip.c_str());
    group_begin("This Glass");
    row_info("Address", url, true);
    char note[96];
    if (up && named) snprintf(note, sizeof(note), "Right now that's %s. ", ip.c_str());
    else if (up) snprintf(note, sizeof(note), ".local naming didn't start this boot. ");
    else snprintf(note, sizeof(note), "Answers once Wi-Fi is back. ");
    strncat(note,
            "Open it in any browser on your home network for a live mirror, "
            "these settings, and a 3D model you can spin.",
            sizeof(note) - strlen(note) - 1);
    group_end(note);
  }
  // Honesty over a tidy slogan (AGENTS.md rule 4): the PAGE is LAN-only,
  // but this firmware does touch the internet for signed update checks —
  // and, on an opted-in hub-less build, the coarse weather fetch. Say so.
  lv_obj_t* cap = mk_label(s_list, font_caption(), col_faint());
  lv_label_set_text(cap,
#ifdef CD_SET_WX
      "Witness data never leaves your home. The internet is touched only "
      "for signed update checks - and weather, only if you opted in.");
#else
      "Witness data never leaves your home. The internet is touched only "
      "for signed update checks.");
#endif
  lv_label_set_long_mode(cap, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(cap, LV_PCT(100));
  lv_obj_set_style_pad_left(cap, M.pad_x, 0);
  lv_obj_set_style_pad_right(cap, M.pad_x, 0);
  lv_obj_set_style_pad_top(cap, M.gap, 0);
}

// Firmware: the version this glass is running, and the one signed-and-
// rollback-safe path to a newer one (same engine HA drives). No raw version
// strings typed anywhere — the manifest and the release key are the truth.
void build_firmware() {
  page_begin("Firmware", "Settings");
  const canary::net::OtaStatus ota = canary::net::ota_status();
  group_begin(nullptr);
  row_info("Installed", ota.installed);
  if (ota.update_available) row_info("Available", ota.latest, true);
  else row_info("Status", ota.state_text);
  group_end(nullptr);
  group_begin(nullptr);
  row_switch("Auto-Update", "Installs overnight", ota.auto_update, ID_FW_AUTO);
  group_end(ota.dev_channel ? "Dev channel. Signed and rollback-safe."
                            : "Release channel. Signed and rollback-safe.");
  // The single action, chosen by state: install what's waiting, or go look.
  group_begin(nullptr);
  if (ota.busy) {
    char p[32];
    snprintf(p, sizeof(p), "Installing  %u%%", (unsigned)ota.progress);
    row_action(p, ID_NONE, Verb::Disabled);
  } else if (ota.update_available) {
    row_action("Install Now", ID_FW_INSTALL);
  } else {
    row_action("Check for Updates", ID_FW_CHECK);
  }
  group_end(nullptr);
}

#ifdef CD_SET_MIC
// Mic opt-in (4.3C). The footer carries the whole contract in the fewest
// honest words: what listening means, and how you always know.
void build_mic() {
  page_begin("Microphone", "Settings");
  const bool pins = canary::io::mic_pins_ok();
  const bool on = canary::io::mic_armed();
  group_begin(nullptr);
  row_switch("Listening", pins ? nullptr : "Pins unset", pins && on, ID_MIC_ON,
             pins);
  // The room preset — how loud a sound must be to count as a beep. Detection
  // cadence is standards-fixed; this only sets the noise-floor margin.
  row_nav("Sensitivity", canary::io::mic_sensitivity_name(), ID_MIC_SENS);
  // Opt-in: a loud sound (a door close) wakes the screen. Same envelope the
  // alarm uses — never speech, nothing recorded. Off by default.
  row_switch("Wake on Sound", nullptr, canary::io::mic_wake_on_sound(),
             ID_MIC_WAKE, pins);
  group_end(pins ? "Hears alarm patterns - and, with wake on, loud sounds (a "
                   "door) to light the screen. Never speech; nothing recorded, "
                   "nothing leaves this board. Amber chip = listening; off is "
                   "a real mute (driver uninstalled)."
                 : "Audio pins are unset (VERIFY in pins.h) - the mics are "
                   "provably un-driven until the bench fills them. See the "
                   "board README.");
}

// Sensitivity preset picker (landing IS choosing). Tuned for the room, not
// the alarm: quiet = bedroom (most sensitive), standard = living areas,
// noisy = kitchen/workshop (least twitchy). The cadence match is unchanged.
void build_mic_sens() {
  page_begin("Sensitivity", "Microphone");
  const uint8_t cur = canary::io::mic_sensitivity();
  group_begin(nullptr);
  row_check("Quiet", "A bedroom", cur == 0, ID_OPT_BASE + 0);
  row_check("Standard", "Living areas", cur == 1, ID_OPT_BASE + 1);
  row_check("Noisy", "A kitchen or workshop", cur == 2, ID_OPT_BASE + 2);
  group_end("Quiet catches a faint or distant alarm; noisy ignores clatter. "
            "It adapts to the room either way - this sets the margin.");
}
#endif

// "Get help" — a white card holding the Help Desk QR for the current
// verdict, composed by the pure help_verdict header (host-tested). The
// verdict inputs are what Settings can reach without new plumbing: the hub
// link via mqtt_mgr. Witness staleness / verification live in the fleet
// model main.cpp owns — the bare Help Desk still covers those, and the
// caption says what the code encodes so nobody scans a mystery. The URL
// carries no secrets (coarse anchor only), so no teardown scrub is owed.
void build_help_qr() {
  page_begin("Get Help", "Settings", /*scrolls=*/false);
  lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  static char s_url[96];
  const bool hub_down = !canary::net::mqtt_broker_is_placeholder() &&
                        !canary::net::mqtt_connected();
  const size_t n = help_verdict::compose(
      s_url, sizeof(s_url), "https://securacv.com/help",
      /*any_verify_failed=*/false, hub_down,
      /*any_witness_quiet=*/false);

  const int avail = M.h - M.nav_h - (M.round ? 70 : 60);
  int qr_px = M.compact ? 132 : 240;
  if (qr_px + 16 > avail) qr_px = avail - 16;
  // PROVE the QR rendered before presenting it (the onboard_ui lesson): the
  // v9 widget leaves a buffer-less canvas behind when its draw-buffer
  // allocation loses, and update reports its own verdict. A failure here
  // degrades to the plain URL — never an empty white card on the exact
  // page someone opened because something is already wrong.
  bool qr_ok = false;
  if (n > 0 && qr_px >= 64) {
    lv_obj_t* card = lv_obj_create(s_list);
    lv_obj_set_size(card, qr_px + 16, qr_px + 16);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 8, 0);   // the quiet zone
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* qr = mk_qrcode(card, qr_px);
    if (qr != nullptr) {
      lv_obj_center(qr);
#if LVGL_VERSION_MAJOR >= 9
      qr_ok = lv_canvas_get_draw_buf(qr) != NULL &&
              lv_qrcode_update(qr, s_url, (uint32_t)strlen(s_url)) == LV_RESULT_OK;
#else
      qr_ok = lv_qrcode_update(qr, s_url, (uint32_t)strlen(s_url)) == LV_RES_OK;
#endif
    }
    if (!qr_ok) lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
  }
  if (!qr_ok) {
    // No code to scan — the URL becomes the page content, said plainly.
    mk_body("securacv.com/help");
  }
  lv_obj_t* cap = mk_label(s_list, font_caption(), col_muted());
  lv_label_set_text(cap,
      !qr_ok     ? "Type it into any browser."
      : hub_down ? "Scan for the fix for your hub link."
                 : "Scan to open the Help Desk.");
  lv_label_set_long_mode(cap, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(cap, LV_PCT(100));
  lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(cap, 10, 0);
}

#ifdef CD_SET_PWM
void build_cal_intro() {
  page_begin("Black Point", "Settings");
  mk_body("Best done at night, with the room lights how you sleep. The glass "
          "dims on its own - tap the moment the glow disappears.");
  mk_button("Begin", ID_GO, Button::Primary);
}

void build_cal_descend() {
  // No bar, no list chrome: the whole glass is the tap target while you
  // squint at a dying glow.
  clear_host();
  mk_list(false);
  lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_pos(s_list, 0, 0);
  lv_obj_set_size(s_list, M.w, M.h);
  lv_obj_add_event_cb(s_list, on_click, LV_EVENT_CLICKED, id_arg(ID_GO));
  s_cal_clock = mk_label(s_list, font_hero(), col_text());
  char c[8];
  fmt_clock(c, sizeof(c));
  lv_label_set_text(s_cal_clock, c);
  lv_obj_t* cap = mk_label(s_list, font_caption(), col_muted());
  lv_label_set_text(cap, "tap when the glow is gone");
  lv_obj_set_style_pad_top(cap, 10, 0);

  set_owns_backlight(true);
  s_cal_duty = 164;  // ~2%: clearly visible start
  s_cal_blinks = 0;
  canary::hal::backlight_night_set(s_cal_duty);
  s_cal_timer = lv_timer_create(cal_descend_cb, 1200, nullptr);
}

void build_cal_comfort() {
  clear_host();
  mk_nav("Night Glow", nullptr, /*inert=*/true);  // one way forward
  mk_list(false);
  s_night_lvl = 3;
  char v[8];
  snprintf(v, sizeof(v), "%d", s_night_lvl);
  mk_hero(v, "pick a 3 a.m. glow");
  group_begin(nullptr);
  row_slider(1, NIGHT_STEPS, s_night_lvl, ID_SLIDER);
  group_end(nullptr);
  spacer(M.compact ? 6 : 12);
  mk_button("Keep This", ID_GO, Button::Primary);
  set_owns_backlight(true);
  canary::hal::backlight_night_set(night_step_duty(s_cal_floor, s_night_lvl));
}

void build_cal_blink() {
  clear_host();
  mk_list(false);
  lv_obj_set_pos(s_list, 0, 0);
  lv_obj_set_size(s_list, M.w, M.h);
  lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  mk_body("Did the glass wink at you just now?");
  mk_button("Yes", ID_YES, Button::Primary);
  mk_button("No", ID_NO, Button::Quiet);

  set_owns_backlight(true);
  s_cal_blinks = 0;
  s_cal_lit = false;
  s_cal_timer = lv_timer_create(cal_blink_cb, 350, nullptr);
}

void build_cal_warn() {
  page_begin("Black Point", "Settings");
  mk_body("That floor came out bright - something looks off. Keeping the "
          "factory floor. Try again on a darker night.");
  mk_button("OK", ID_GO, Button::Quiet);
}

void build_cal_done() {
  page_begin("Black Point", "Settings");
  if (s_cal_persisted) {
    mk_body("Saved. Night light will never go below this - redo it here "
            "anytime.");
  } else {
    // Honesty over comfort: the floor works right now but storage balked,
    // so it won't survive a restart. Say so instead of a false "saved".
    mk_body("Kept for tonight - but storage balked, so it resets on restart. "
            "Redo it here anytime.");
  }
  mk_button("Done", ID_GO, Button::Quiet);
}
#endif  // CD_SET_PWM

void build_reset_confirm() {
  page_begin("Reset Settings", "Settings");
#ifdef CD_SET_PWM
  mk_body("Back to the defaults? Your black point stays.");
#else
  mk_body("Back to the defaults?");
#endif
  mk_button("Reset Settings", ID_YES, Button::Destructive);
  mk_button("Cancel", ID_NO, Button::Quiet);
}

// Forget WiFi, confirm-gated like every destructive act on this glass. On
// yes: erase the NVS credentials and reboot — the unit comes back up in the
// join wizard (its own SoftAP + QR), exactly like first boot. MQTT broker
// pins and identity stay; a re-join on the same network resumes seamlessly.
void build_net_forget() {
  page_begin("Forget Wi-Fi", "Settings");
  char body[96];
  snprintf(body, sizeof(body), "Leave \"%.24s\"? This glass restarts into setup.",
           canary::cfg::get().wifi_ssid);
  mk_body(body);
  mk_button("Forget Network", ID_YES, Button::Destructive);
  mk_button("Cancel", ID_NO, Button::Quiet);
}

#ifdef CD_SET_MODES
// Which gear is awaiting its confirm tap (ModesList -> ModeConfirm).
canary::mode::Mode s_pending_mode = canary::mode::Mode::Fleet;

void build_modes_list() {
  page_begin("Modes", "Settings");
  group_begin(nullptr);
#if defined(FEATURE_DEVMODE) && FEATURE_DEVMODE
  row_nav("Bench", "Peripheral test", ID_MODE_BENCH);
#endif
#if defined(FEATURE_DEMO_MODE) && FEATURE_DEMO_MODE
  row_nav("Demo", "Scripted fleet", ID_MODE_DEMO);
#endif
#if defined(FEATURE_DEBUG_MODE) && FEATURE_DEBUG_MODE
  row_nav("Debug", "Diagnostics", ID_MODE_DEBUG);
#endif
#ifdef CD_SET_ROW_ARCADE
  row_nav("Arcade", "Touch QA", ID_MODE_ARCADE);
#endif
  group_end("Entering a mode restarts the glass. Hold 3 seconds in any mode "
            "to come back.");
}

void build_mode_confirm() {
  using canary::mode::Mode;
  const char* blurb = "";
  const char* verb = "Enter";
  switch (s_pending_mode) {
    case Mode::Bench:
      blurb = "Enter developer mode? Restarts into the peripheral bench - "
              "no fleet, no network.";
      break;
    case Mode::Demo:
      blurb = "Enter demo mode? A scripted household plays on this glass - "
              "no fleet, no network.";
      break;
    case Mode::Debug:
      blurb = "Enter debug mode? On-glass diagnostics - network up, no "
              "updates, read-mostly.";
      break;
    case Mode::Arcade:
      blurb = "Enter arcade mode? Canary Catch, the touch QA round - no "
              "fleet, no network.";
      break;
    default:
      break;
  }
  page_begin(canary::mode::mode_token(s_pending_mode), "Modes");
  mk_body(blurb);
  mk_button(verb, ID_YES, Button::Primary);
  mk_button("Cancel", ID_NO, Button::Quiet);
}
#endif

void build(Page pg) {
  set_owns_backlight(false);
  s_page = pg;
  switch (pg) {
    case Page::Root:         build_root(); break;
    case Page::Bright:       build_bright(); break;
#ifdef CD_SET_PWM
    case Page::Night:        build_night(); break;
    case Page::CalIntro:     build_cal_intro(); break;
    case Page::CalDescend:   build_cal_descend(); break;
    case Page::CalComfort:   build_cal_comfort(); break;
    case Page::CalBlink:     build_cal_blink(); break;
    case Page::CalWarn:      build_cal_warn(); break;
    case Page::CalDone:      build_cal_done(); break;
#endif
    case Page::Hours:        build_hours(); break;
    case Page::Screen:       build_screen(); break;
    case Page::Style:        build_style(); break;
#ifdef CD_FLAVOR_DASH
    case Page::Clock:        build_clock(); break;
    case Page::Display:      build_display(); break;
#ifdef CD_SET_WX
    case Page::Weather:      build_weather(); break;
#endif
#endif
    case Page::Network:      build_network(); break;
    case Page::Firmware:     build_firmware(); break;
#ifdef CD_SET_MIC
    case Page::Mic:          build_mic(); break;
    case Page::MicSens:      build_mic_sens(); break;
#endif
    case Page::HelpQr:       build_help_qr(); break;
    case Page::ResetConfirm: build_reset_confirm(); break;
    case Page::NetForget:    build_net_forget(); break;
#ifdef CD_SET_MODES
    case Page::ModesList:    build_modes_list(); break;
    case Page::ModeConfirm:  build_mode_confirm(); break;
#endif
    default:                 build_root(); s_page = Page::Root; break;
  }
  if (s_page == Page::Root && s_root_scroll > 0 && s_list) {
    // Come back to where you were on the root, as a phone does.
    lv_obj_update_layout(s_list);
    lv_obj_scroll_to_y(s_list, s_root_scroll, LV_ANIM_OFF);
  }
}

// Leave the root for a sub-page, remembering the scroll for the way back.
void go(Page pg) {
  if (s_page == Page::Root && s_list) s_root_scroll = lv_obj_get_scroll_y(s_list);
  build(pg);
}

// Instant, animation-free close for the commissioning handoff: the fade
// close leaves lv_scr_act() ambiguous mid-anim, and the commissioning
// surface must capture the FACE as its return screen, never a dying
// settings screen.
void close_instant() {
  if (!s_scr) return;
  if (s_cal_timer) {
    lv_timer_del(s_cal_timer);
    s_cal_timer = nullptr;
  }
  s_cal_clock = nullptr;
  set_owns_backlight(false);
  lvgl_port_touch_feed(false, 0, 0);
  lv_obj_t* dying = s_scr;
  s_scr = nullptr;
  s_host = s_nav = s_list = s_group = s_prev_row = nullptr;
  lv_scr_load(s_prev);
  s_prev = nullptr;
  lv_obj_del(dying);
}

// ── Dispatch ─────────────────────────────────────────────────────────────
// Switches arrive as id | 0x10000 (on) / id (off); sliders and rollers as
// id | value << 8. Pickers arrive as ID_OPT_BASE + option.

void dispatch(int raw) {
  const int id = raw & 0xFF;
  const int val = (raw >> 8) & 0xFF;
  const bool on = (raw & 0x10000) != 0;
  Settings& gs = settings_mut();

  switch (s_page) {
    case Page::Root:
      switch (id) {
        case ID_NAV:        settings_ui_close(); return;
        case ID_ROW_BRIGHT: go(Page::Bright); return;
        case ID_ROW_STYLE:  go(Page::Style); return;
        case ID_ROW_HOURS:  go(Page::Hours); return;
        case ID_ROW_SCREEN: go(Page::Screen); return;
        case ID_ROW_LOOK:
          gs.red_shift = on ? 1 : 0;
          settings_mark_dirty();
          return;
#ifdef CD_SET_PWM
        case ID_ROW_NIGHT:  go(Page::Night); return;
        case ID_ROW_CAL:    go(Page::CalIntro); return;
        case ID_ROW_ADD:
          close_instant();
          commission_ui_open();
          return;
#endif
#ifdef CD_FLAVOR_DASH
        case ID_ROW_CLOCK:   go(Page::Clock); return;
        case ID_ROW_DISPLAY: go(Page::Display); return;
#ifdef CD_SET_WX
        case ID_ROW_WX:      go(Page::Weather); return;
#endif
#endif
        case ID_ROW_NET:    go(Page::Network); return;
        case ID_ROW_FW:     go(Page::Firmware); return;
#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
        case ID_ROW_SIREN:
          // Persist the arm state to NVS through field_io; the switch already
          // shows it.
          canary::io::field_io_set_armed(on);
          return;
#endif
#ifdef CD_SET_MIC
        case ID_ROW_MIC:    go(Page::Mic); return;
#endif
        case ID_ROW_HELP:   go(Page::HelpQr); return;
#ifdef CD_SET_MODES
        case ID_ROW_DEV:    go(Page::ModesList); return;
#endif
        case ID_ROW_RESET:  go(Page::ResetConfirm); return;
        case ID_ROW_FORGET: go(Page::NetForget); return;
      }
      return;

    case Page::Bright:
      if (id == ID_NAV) { build(Page::Root); return; }
      if (id == ID_SLIDER) {
#ifdef CD_SET_PWM
        int v = val;
        if (v < 20) v = 20;
        if (v > 100) v = 100;
        gs.day_pct = (uint8_t)v;
        settings_mark_dirty();
        canary::hal::backlight_set(day_level());   // live preview
#else
        gs.bright_pct = bright_pct_clamp(val);
        settings_mark_dirty();
        lvgl_port_set_dim(bright_scrim_opa(gs.bright_pct));  // live preview
#endif
        // The hero is the first thing in the list; keep it honest without
        // rebuilding under the thumb.
        lv_obj_t* hero = lv_obj_get_child(s_list, 0);
        if (hero) {
#ifdef CD_SET_PWM
          lv_label_set_text_fmt(hero, "%d%%", gs.day_pct);
#else
          lv_label_set_text_fmt(hero, "%d%%", gs.bright_pct);
#endif
        }
      }
      return;

#ifdef CD_SET_PWM
    case Page::Night:
      if (id == ID_NAV) { build(Page::Root); return; }
      if (id == ID_SLIDER) {
        int v = val;
        if (v < 1) v = 1;
        if (v > NIGHT_STEPS) v = NIGHT_STEPS;
        s_night_lvl = v;
        gs.night_duty = night_step_duty(cal_floor_or_default(), v);
        settings_mark_dirty();
        preview_night_level(v);
        lv_obj_t* hero = lv_obj_get_child(s_list, 0);
        if (hero) lv_label_set_text_fmt(hero, "%d", v);
      }
      return;
#endif

    case Page::Hours:
      if (id == ID_NAV) { build(Page::Root); return; }
      if (id == ID_ROLL_START || id == ID_ROLL_END) {
        uint8_t* hh = id == ID_ROLL_START ? &gs.night_start_hh : &gs.night_end_hh;
        *hh = (uint8_t)(val % 24);
        settings_mark_dirty();
        // The footer is the last thing in the list.
        const uint32_t n = lv_obj_get_child_cnt(s_list);
        lv_obj_t* f = n ? lv_obj_get_child(s_list, (int32_t)n - 1) : nullptr;
        if (f)
          lv_label_set_text(f, gs.night_start_hh == gs.night_end_hh
                                   ? "Same hour twice: night never comes."
                                   : "Quiet hours never hide an alert.");
      }
      return;

    case Page::Screen:
      if (id == ID_NAV) { build(Page::Root); return; }
      if (id == ID_SCREEN_GLOW || id == ID_SCREEN_OFF) {
        gs.night_screen = id == ID_SCREEN_OFF ? NIGHT_SCREEN_OFF : NIGHT_SCREEN_DIM;
        settings_mark_dirty();
        build(Page::Screen);
      }
      if (id == ID_PEEK_3 || id == ID_PEEK_5 || id == ID_PEEK_10) {
        gs.peek_s = id == ID_PEEK_3 ? 3 : id == ID_PEEK_5 ? 5 : 10;
        settings_mark_dirty();
        build(Page::Screen);
      }
      return;

    case Page::Style:
      if (id == ID_NAV) { build(Page::Root); return; }
      if (id >= ID_OPT_BASE && id < ID_OPT_BASE + (int)character_count()) {
        // Landing IS choosing: apply + persist, restyle the open surface,
        // and rebuild so every row arrives in the new look.
        const Character next = (Character)(id - ID_OPT_BASE);
        character_apply(next);
        gs.character = (uint8_t)next;
        settings_mark_dirty();
        restyle_open_surface();
        build(Page::Style);
      }
      return;

#ifdef CD_FLAVOR_DASH
    case Page::Clock:
      if (id == ID_NAV) { build(Page::Root); return; }
      if (id == ID_CLOCK_24H) {
        // The running face re-reads the store every tick, so the digits and
        // their AM/PM follow without a rebuild.
        gs.clock_12h = on ? 0 : 1;
        settings_mark_dirty();
        return;
      }
      if (id >= ID_OPT_BASE && id < ID_OPT_BASE + (int)clock_style_count()) {
        gs.clock_style = (uint8_t)(id - ID_OPT_BASE);
        settings_mark_dirty();
        build(Page::Clock);
      }
      return;

    case Page::Display:
      if (id == ID_NAV) { build(Page::Root); return; }
      if (id >= ID_OPT_BASE && id < ID_OPT_BASE + 4) {
        // Landing IS choosing: persist + rotate the live glass (panel and
        // all), re-measure the canvas, then rebuild so the check moves.
        gs.rotation = (uint8_t)(id - ID_OPT_BASE);
        settings_mark_dirty();
        lvgl_port_set_rotation(gs.rotation);
        layout_host();
        build(Page::Display);
      }
      return;

#ifdef CD_SET_WX
    case Page::Weather:
      if (id == ID_NAV) { build(Page::Root); return; }
      if (id == ID_WX_ON) {
        // Persist the opt-in; the fetcher's own three gates (opt-in,
        // location, hub-less) decide whether anything runs.
        gs.wx_direct = on ? 1 : 0;
        settings_mark_dirty();
        build(Page::Weather);
      }
      return;
#endif
#endif  // CD_FLAVOR_DASH

    case Page::Network:
      // Read-only page: back is the one affordance. Joining lives in the
      // wizard, forgetting under reset — no network decision is made here.
      if (id == ID_NAV) build(Page::Root);
      return;

    case Page::Firmware:
      if (id == ID_NAV) { build(Page::Root); return; }
      if (id == ID_FW_AUTO) {
        // Flip the nightly auto-install preference (NVS'd).
        canary::net::ota_set_auto_update(on);
        return;
      }
      if (id == ID_FW_CHECK) {
        canary::net::ota_request_check();
        build(Page::Firmware);
        return;
      }
      if (id == ID_FW_INSTALL) {  // may reboot on success
        canary::net::ota_request_install();
        build(Page::Firmware);
        return;
      }
      return;

#ifdef CD_SET_MIC
    case Page::Mic:
      if (id == ID_NAV) { build(Page::Root); return; }
      if (id == ID_MIC_SENS) { go(Page::MicSens); return; }
      if (id == ID_MIC_WAKE) {
        canary::io::mic_set_wake_on_sound(on);
        return;
      }
      if (id == ID_MIC_ON) {
        // mic_set_armed persists to NVS and performs the gate action in the
        // same call — driver AND the amber chip together; with pins unset
        // the gate refuses regardless of choice.
        canary::io::mic_set_armed(on);
        build(Page::Mic);
      }
      return;
    case Page::MicSens:
      if (id == ID_NAV) { build(Page::Mic); return; }
      if (id >= ID_OPT_BASE && id <= ID_OPT_BASE + 2) {
        // Persist the preset and re-seed the floor.
        canary::io::mic_set_sensitivity((uint8_t)(id - ID_OPT_BASE));
        build(Page::MicSens);
      }
      return;
#endif

    case Page::HelpQr:
      // A QR page has one affordance: back. Any other tap is a person
      // aiming a camera, not navigating — leave the code on the glass.
      if (id == ID_NAV) build(Page::Root);
      return;

#ifdef CD_SET_PWM
    case Page::CalIntro:
      if (id == ID_NAV) { build(Page::Root); return; }
      if (id == ID_GO) { s_cal_retries = 0; build(Page::CalDescend); }
      return;

    case Page::CalDescend:
      // ANY tap = "it just disappeared": step back up a touch for margin.
      s_cal_floor = (uint16_t)(s_cal_duty + (s_cal_duty / 4 > 2 ? s_cal_duty / 4
                                                                : 2));
      if (s_cal_floor > NIGHT_FLOOR_CAP) build(Page::CalWarn);
      else build(Page::CalComfort);
      return;

    case Page::CalComfort:
      if (id == ID_SLIDER) {
        int v = val;
        if (v < 1) v = 1;
        if (v > NIGHT_STEPS) v = NIGHT_STEPS;
        s_night_lvl = v;
        canary::hal::backlight_night_set(night_step_duty(s_cal_floor, v));
        lv_obj_t* hero = lv_obj_get_child(s_list, 0);
        if (hero) lv_label_set_text_fmt(hero, "%d", v);
        return;
      }
      if (id == ID_GO) build(Page::CalBlink);
      return;

    case Page::CalBlink:
      if (id == ID_YES) {
        // The floor provably emits light on this panel: keep it. RAM holds
        // it for tonight regardless; the return says whether it survives a
        // reboot, and CalDone tells the truth either way.
        s_cal_persisted = nightcal_put(s_cal_floor);
        gs.night_duty = night_step_duty(s_cal_floor, s_night_lvl);
        settings_mark_dirty();
        build(Page::CalDone);
        return;
      }
      if (id == ID_NO) {
        // Confirmed duty was capacitance, not light — raise and re-test.
        s_cal_floor = (uint16_t)(s_cal_floor * 2 + 2);
        if (s_cal_floor > NIGHT_FLOOR_CAP || ++s_cal_retries >= 3)
          build(Page::CalWarn);
        else
          build(Page::CalBlink);
      }
      return;

    case Page::CalWarn:
    case Page::CalDone:
      build(Page::Root);
      return;
#endif

    case Page::ResetConfirm:
      if (id == ID_YES) {
        settings_reset();
        // Reset comes home in look too (defaults() holds Quiet Glass):
        // re-apply so the glass doesn't keep wearing a Character the
        // blob no longer holds until the next boot.
        character_apply((Character)settings().character);
#ifdef CD_FLAVOR_DASH
        // ...and in orientation: the defaults hold landscape, so a panel
        // opened on a turned glass must turn back NOW and re-measure its
        // canvas, or the root rebuilds into the old 480x800 frame and the
        // render tick's rotation tracker (which never relayouts an open
        // panel) leaves it there until the next navigation (Codex P2).
        lvgl_port_set_rotation(settings().rotation);
        layout_host();
#else
        restyle_open_surface();
#endif
      }
      build(Page::Root);
      return;

    case Page::NetForget:
      if (id == ID_YES && canary::cfg::forget_wifi_credentials()) {
        // Reboot into the join wizard. Does not return on hardware; the
        // emulator's shim falls through, so land somewhere sane anyway.
        ESP.restart();
      }
      // No / NVS balked (the log says which): back to the root, still joined.
      build(Page::Root);
      return;

#ifdef CD_SET_MODES
    case Page::ModesList:
      switch (id) {
        case ID_NAV: build(Page::Root); return;
#if defined(FEATURE_DEVMODE) && FEATURE_DEVMODE
        case ID_MODE_BENCH:
          s_pending_mode = canary::mode::Mode::Bench;
          build(Page::ModeConfirm);
          return;
#endif
#if defined(FEATURE_DEMO_MODE) && FEATURE_DEMO_MODE
        case ID_MODE_DEMO:
          s_pending_mode = canary::mode::Mode::Demo;
          build(Page::ModeConfirm);
          return;
#endif
#if defined(FEATURE_DEBUG_MODE) && FEATURE_DEBUG_MODE
        case ID_MODE_DEBUG:
          s_pending_mode = canary::mode::Mode::Debug;
          build(Page::ModeConfirm);
          return;
#endif
#ifdef CD_SET_ROW_ARCADE
        case ID_MODE_ARCADE:
          s_pending_mode = canary::mode::Mode::Arcade;
          build(Page::ModeConfirm);
          return;
#endif
      }
      return;

    case Page::ModeConfirm:
      if (id == ID_YES) {
        // Latch the chosen gear for the NEXT boot and reboot into it. The
        // glue owns the NVS grammar (token + the legacy devmode bool for
        // the bench); every gear's own 3 s long-press exits back here.
        canary::mode::mode_request(s_pending_mode);  // does not return
        return;
      }
      build(Page::ModesList);  // Cancel / back
      return;
#endif

    default:
      if (id == ID_NAV) build(Page::Root);
      return;
  }
}

// A stationary hold is the quick exit — but never on a control a thumb may
// legitimately rest on (a slider knob held at a value, a wheel mid-turn, a
// switch). Rows, the bar and the ground all qualify.
bool hold_allowed_at(int16_t x, int16_t y) {
  if (!s_scr) return false;
  lv_point_t p;
  p.x = x;
  p.y = y;
  lv_obj_t* o = lv_indev_search_obj(s_scr, &p);
  while (o && o != s_scr) {
    if (lv_obj_check_type(o, &lv_slider_class) ||
        lv_obj_check_type(o, &lv_switch_class) ||
        lv_obj_check_type(o, &lv_roller_class))
      return false;
    o = lv_obj_get_parent(o);
  }
  return true;
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────

void settings_ui_open() {
  if (s_scr) return;
  s_prev = lv_scr_act();
  s_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
  s_host = mk_box(s_scr);
  layout_host();
  s_last_touch_ms = millis();
  s_down = false;
  s_hold_fired = false;
  s_root_scroll = 0;
  lvgl_port_touch_feed(false, 0, 0);
  build(Page::Root);
  // Keep the face alive underneath; no auto-delete of the previous screen.
  lv_scr_load_anim(s_scr, LV_SCR_LOAD_ANIM_FADE_ON, MOTION_PAGE_MS, 0, false);
}

void settings_ui_close() {
  if (!s_scr) return;
  // Kill wizard timers first; the screen itself dies via the transition's
  // auto-delete AFTER the fade (the splash pattern — deleting it ourselves
  // mid-anim would be a use-after-free inside the screen loader).
  if (s_cal_timer) {
    lv_timer_del(s_cal_timer);
    s_cal_timer = nullptr;
  }
  s_cal_clock = nullptr;
  set_owns_backlight(false);
  // LVGL sees the finger lift now, on the dying screen, whose callbacks all
  // check s_scr and stand down.
  lvgl_port_touch_feed(false, 0, 0);
  s_scr = nullptr;
  s_host = s_nav = s_list = s_group = s_prev_row = nullptr;
  lv_scr_load_anim(s_prev, LV_SCR_LOAD_ANIM_FADE_ON, MOTION_PAGE_MS, 0, true);
  s_prev = nullptr;
}

bool settings_ui_active() { return s_scr != nullptr; }

bool settings_ui_owns_backlight() {
  return s_scr != nullptr && s_owns_backlight;
}

void settings_ui_handle_touch(bool down, int16_t x, int16_t y,
                              uint32_t now_ms) {
  if (!s_scr) return;
  lvgl_port_touch_feed(down, x, y);
  if (down) {
    s_last_touch_ms = now_ms;
    if (!s_down) {
      s_down = true;
      s_down_ms = now_ms;
      s_down_x = x;
      s_down_y = y;
      s_hold_fired = false;
      s_hold_ok = hold_allowed_at(x, y);
    } else if (!s_hold_fired) {
      const int dx = x - s_down_x, dy = y - s_down_y;
      if (dx * dx + dy * dy > HOLD_SLOP_PX * HOLD_SLOP_PX) {
        s_hold_fired = true;   // it moved: a scroll or a drag, never a hold
      } else if (s_hold_ok &&
                 (int32_t)(now_ms - s_down_ms) >= (int32_t)CD_LONGPRESS_MS) {
        s_hold_fired = true;
        settings_ui_close();
      }
    }
  } else if (s_down) {
    s_down = false;
  }
}

void settings_ui_tick(uint32_t now_ms, bool urgent) {
  if (!s_scr) return;
  if (urgent) {
    // Calm-tech boundary: nothing on this sheet outranks a live alarm.
    settings_ui_close();
    return;
  }
  if ((int32_t)(now_ms - s_last_touch_ms) >= (int32_t)IDLE_CLOSE_MS) {
    settings_ui_close();
    return;
  }
#ifdef CD_SET_PWM
  if (s_page == Page::CalDescend && s_cal_clock) {
    char c[8];
    fmt_clock(c, sizeof(c));
    lv_label_set_text(s_cal_clock, c);
  }
#endif
  // The firmware page mirrors a live state machine (checking → downloading →
  // %), and the network page a live radio (signal word, reconnects, the hub
  // link). Rebuild the open one ~1 Hz so they advance without a tap —
  // carrying the panel around the house IS the network page's signal survey.
  // Never under a finger (a rebuild would pull the list out from under a
  // scroll), and always back to the same scroll offset.
  if ((s_page == Page::Firmware || s_page == Page::Network) && !s_down && s_list) {
    static uint32_t s_live_next_ms = 0;
    if ((int32_t)(now_ms - s_live_next_ms) >= 0) {
      s_live_next_ms = now_ms + 1000;
      const int y = lv_obj_get_scroll_y(s_list);
      build(s_page);
      if (s_list) {
        lv_obj_update_layout(s_list);
        lv_obj_scroll_to_y(s_list, y, LV_ANIM_OFF);
      }
    }
  }
}

}  // namespace canary::ui

#else  // !FEATURE_TOUCH

// Touch-less glass (the 1.47" nightstands, the nightlight): nothing can open
// the panel, and the lean C6 image has no flash to carry it — the phone page
// (net/glass_web.cpp) is the settings surface there. The stubs keep every
// call site in main.cpp honest without a gate of its own.
namespace canary::ui {
void settings_ui_open() {}
void settings_ui_close() {}
bool settings_ui_active() { return false; }
bool settings_ui_owns_backlight() { return false; }
void settings_ui_handle_touch(bool, int16_t, int16_t, uint32_t) {}
void settings_ui_tick(uint32_t, bool) {}
}  // namespace canary::ui

#endif  // FEATURE_TOUCH
