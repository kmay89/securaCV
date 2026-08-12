// src/ui/settings_ui.cpp — on-glass screen settings + black-point wizard.
// See settings_ui.h and docs/hardware/display_settings.md.
//
// Design contract (settings wave): five principles — Zero Layer (two levels
// deep, ever), The Screen Is the Preview (brightness applies as you adjust),
// One Screen One Decision (each editor holds exactly one value), Defaults
// You Can Come Home To (visible reset that spares the calibration), Night Is
// a Mode (schedule + glow + look + peek travel together).
#include "flavor_config.h"
// Nightstand borrows the watch's small-portrait modal rendering (see
// splash.cpp for the rationale); the standing face is portrait_ui.cpp.
#if defined(CD_FLAVOR_NIGHTSTAND) && !defined(CD_FLAVOR_WATCH)
#define CD_FLAVOR_WATCH 1
#endif
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <time.h>
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
// toggle rides Settings exactly like the siren arm — off by default, NVS'd.
#if defined(FEATURE_MIC_ALARM) && FEATURE_MIC_ALARM && \
    defined(HAS_MICROPHONE) && HAS_MICROPHONE
#define CD_SET_MIC 1
#include "mic_alarm.h"
#endif

#include "settings_ui.h"
#include "commission_ui.h"
#include "theme.h"
#include "character.h"
#include "clock_styles.h"
#include "glass_settings.h"
#include "display.h"
#include "pins.h"                    // HAS_ISOLATED_IO (board -I path)
// The 7"/dash RGB glass carries orientation + rendered brightness + a
// firmware page. These are compiled out on the round watch and the fixed-
// portrait SPI nightstands (which map to CD_FLAVOR_WATCH above).
#ifdef CD_FLAVOR_DASH
#include "lvgl_port.h"     // set_rotation / set_dim (live preview)
#include "ota_mgr.h"      // version + signed OTA facade
#endif
#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
#include "field_io.h"      // siren arm/disarm — 4.3B isolated output
#endif

namespace canary::ui {

using namespace canary::glass;

namespace {

// ── Flavor metrics: one tree, two renderers ──────────────────────────────
#ifdef CD_FLAVOR_WATCH
constexpr int PANEL_W = 240;
constexpr int ROW_H = 24;      // editor rows (the root packs tighter now —
                               // nine rows since the style row; build_root)
constexpr int ROOT_Y0 = 40;
constexpr int HIT_PAD = 8;
constexpr uint32_t IDLE_CLOSE_MS = 60000;
#else
constexpr int PANEL_W = 800;
// The sheet is deliberately non-scrollable (every row visible, always), so
// its height has to clear the tallest root: back + six shared rows + the
// display/brightness/clock/firmware quartet + the board's siren-or-mic row
// + reset + the modes doorway (Codex P2: the clock row pushed reset off a
// 420 px sheet). 460 on the 480 glass leaves a 10 px reveal of the face.
constexpr int SHEET_W = 480, SHEET_H = 460;
constexpr int ROW_H = 46;
constexpr int ROOT_Y0 = 64;
constexpr int HIT_PAD = 10;
constexpr uint32_t IDLE_CLOSE_MS = 120000;
#endif

enum class Page {
  Root,
  EditDay,     // watch only
  EditNight,   // watch only
  EditHours,
  EditLook,
  EditScreen,
#ifdef CD_FLAVOR_DASH
  EditDisplay,  // orientation: landscape / portrait / their flips (live)
  EditBright,   // rendered daytime brightness (binary-backlight glass)
  EditClock,    // the clock-face ring (segment family / Analog dial)
  EditFirmware, // installed version + signed OTA (check / install / auto)
#endif
#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
  EditSiren,   // 4.3B: arm/disarm the isolated siren output (DO0)
#endif
#ifdef CD_SET_MIC
  EditMic,     // 4.3C: the mic opt-in (alarm-pattern listening, default off)
  EditMicSens, // 4.3C: the room sensitivity preset (quiet/standard/noisy)
#endif
  EditStyle,   // the Character ring picker
  CalIntro,    // watch only — the black-point wizard
  CalDescend,
  CalComfort,
  CalBlink,
  CalWarn,
  CalDone,
  ResetConfirm,
#ifdef CD_SET_MODES
  ModesList,    // the non-fleet gears this build carries
  ModeConfirm,  // confirm-gated: latch the chosen gear + reboot into it
#endif
};

// Tap-zone ids. Zones are the built objects themselves — hit-testing reads
// their laid-out coords, the same pattern the dash sheets already use.
enum : int {
  IT_BACK = 1,
  IT_ROW_DAY, IT_ROW_NIGHT, IT_ROW_HOURS, IT_ROW_LOOK, IT_ROW_SCREEN,
  IT_ROW_STYLE, IT_ROW_CAL, IT_ROW_RESET, IT_ROW_ADD,
#ifdef CD_FLAVOR_DASH
  IT_ROW_DISPLAY, IT_ROW_BRIGHT, IT_ROW_FW, IT_ROW_CLOCK,
  IT_ROT_0, IT_ROT_90, IT_ROT_180, IT_ROT_270,   // orientation options
  IT_ROW_FW_AUTO,                                 // auto-update toggle
#endif
#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
  IT_ROW_SIREN,
#endif
#ifdef CD_SET_MIC
  IT_ROW_MIC, IT_ROW_MIC_SENS, IT_ROW_MIC_WAKE, IT_OPT_C,
#endif
#ifdef CD_SET_MODES
  IT_ROW_DEV,   // the root doorway row ("modes" / "dev mode")
  IT_ROW_MBENCH, IT_ROW_MDEMO, IT_ROW_MDEBUG, IT_ROW_MARCADE,
#endif
  IT_MINUS, IT_PLUS, IT_OPT_A, IT_OPT_B, IT_PEEK, IT_GO,
  IT_YES, IT_NO,
};

struct Item {
  lv_obj_t* obj;
  int id;
};

lv_obj_t* s_prev = nullptr;   // the face to return to
lv_obj_t* s_scr = nullptr;    // our own screen while open
lv_obj_t* s_host = nullptr;   // content parent (screen on watch, sheet on dash)
Page s_page = Page::Root;
// Worst case is the dash root: back + six shared rows + the display /
// brightness / firmware trio + reset + the modes doorway, each row up to two
// objects (name+value), plus the board's siren or mic row. The sizes below
// clear that with margin so add_item never silently drops a hit zone.
#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
Item s_items[26];
#elif defined(CD_SET_MIC)
Item s_items[24];
#else
Item s_items[22];
#endif
int s_item_n = 0;
bool s_owns_backlight = false;
uint32_t s_last_touch_ms = 0;

// Editor state
int  s_hours_sel = 0;         // 0 = starts, 1 = ends
int  s_night_lvl = 4;         // 1..NIGHT_STEPS while editing

// Wizard state
lv_timer_t* s_cal_timer = nullptr;
uint16_t s_cal_duty = 0;
uint16_t s_cal_floor = 0;
int  s_cal_blinks = 0;
int  s_cal_retries = 0;
bool s_cal_lit = false;
bool s_cal_persisted = true;  // false = floor kept for tonight only
lv_obj_t* s_cal_clock = nullptr;

void build(Page pg);

// ── Small helpers ────────────────────────────────────────────────────────

lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* f, lv_color_t c) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, "");
  return l;
}

void add_item(lv_obj_t* obj, int id) {
  if (s_item_n < (int)(sizeof(s_items) / sizeof(s_items[0]))) {
    s_items[s_item_n].obj = obj;
    s_items[s_item_n].id = id;
    s_item_n++;
  }
}

// One row: centered single label on the round watch (a circle likes centered
// text at every latitude); name-left value-right on the dash sheet.
lv_obj_t* mk_row(int y, const char* name, const char* value, int id,
                 bool selected = false) {
#ifdef CD_FLAVOR_WATCH
  lv_obj_t* l = mk_label(s_host, font_body(), selected ? col_signed()
                                                       : col_text());
  if (value && value[0])
    lv_label_set_text_fmt(l, "%s • %s", name, value);
  else
    lv_label_set_text(l, name);
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
  add_item(l, id);
  return l;
#else
  lv_obj_t* n = mk_label(s_host, font_label(), selected ? col_signed()
                                                        : col_text());
  lv_label_set_text(n, name);
  lv_obj_align(n, LV_ALIGN_TOP_LEFT, 28, y);
  if (value && value[0]) {
    lv_obj_t* v = mk_label(s_host, font_label(),
                           selected ? col_signed() : col_muted());
    lv_label_set_text(v, value);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -28, y);
    add_item(v, id);
  }
  add_item(n, id);
  return n;
#endif
}

// Back affordance: "‹ settings"-style line at the top; tapping it ascends.
void mk_back(const char* title) {
  lv_obj_t* l = mk_label(s_host, font_caption(), col_faint());
  lv_label_set_text_fmt(l, LV_SYMBOL_LEFT " %s", title);
#ifdef CD_FLAVOR_WATCH
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 16);
#else
  lv_obj_align(l, LV_ALIGN_TOP_LEFT, 28, 18);
#endif
  add_item(l, IT_BACK);
}

// Stepper pair for the editors: big − / + at the bottom corners.
void mk_stepper() {
  lv_obj_t* m = mk_label(s_host, font_title(), col_text());
  lv_label_set_text(m, LV_SYMBOL_MINUS);
  lv_obj_t* p = mk_label(s_host, font_title(), col_text());
  lv_label_set_text(p, LV_SYMBOL_PLUS);
#ifdef CD_FLAVOR_WATCH
  lv_obj_align(m, LV_ALIGN_CENTER, -58, 62);
  lv_obj_align(p, LV_ALIGN_CENTER, 58, 62);
#else
  lv_obj_align(m, LV_ALIGN_BOTTOM_LEFT, 72, -40);
  lv_obj_align(p, LV_ALIGN_BOTTOM_RIGHT, -72, -40);
#endif
  add_item(m, IT_MINUS);
  add_item(p, IT_PLUS);
}

void clear_host() {
  // Wizard timers never outlive their page.
  if (s_cal_timer) {
    lv_timer_del(s_cal_timer);
    s_cal_timer = nullptr;
  }
  s_cal_clock = nullptr;
  lv_obj_clean(s_host);
  s_item_n = 0;
}

// A Character flip restyles the OPEN surface live — the screen is the
// preview, taken literally: the ground under your thumb changes, and the
// page rebuild repaints everything on top of it.
void restyle_open_surface() {
  if (s_scr) lv_obj_set_style_bg_color(s_scr, col_bg(), 0);
#ifndef CD_FLAVOR_WATCH
  if (s_host) {
    lv_obj_set_style_bg_color(s_host, col_surface(), 0);
    lv_obj_set_style_border_color(s_host, col_edge(), 0);
  }
#endif
}

void set_owns_backlight(bool owns) { s_owns_backlight = owns; }

#ifdef CD_FLAVOR_WATCH
uint16_t cal_floor_or_default() {
  return nightcal().valid ? nightcal().floor_duty : NIGHT_FLOOR_DFLT;
}

void preview_night_level(int lvl) {
  const uint16_t d = night_step_duty(cal_floor_or_default(), lvl);
  canary::hal::backlight_night_set(d);
}
#endif  // CD_FLAVOR_WATCH

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

// ── Wizard timers (watch only — the dash panel has no PWM to calibrate) ──
#ifdef CD_FLAVOR_WATCH

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

#endif  // CD_FLAVOR_WATCH

// ── Page builders ────────────────────────────────────────────────────────

void build_root() {
  mk_back("settings • tap to leave");
  char v[24];
#ifdef CD_FLAVOR_WATCH
  // Nine rows since the style row joined — the editor spacing would run
  // off the round glass, so the root alone packs tighter. (Ten with the
  // modes doorway: tighter still, and the last row stays clear of the rim.)
#ifdef CD_SET_MODES
  const int y0 = 32, step = 20;
#else
  const int y0 = 36, step = 22;
#endif
#else
  // The dash root packs tighter than the editors (the watch does the same):
  // up to eleven rows must fit the sheet with the back line above them.
  // 52 + 10*38 + a label's height = ~452 clears SHEET_H = 460; editors keep
  // the roomier ROW_H — none of them exceeds five rows.
  const int y0 = 52, step = 38;
#endif
  int y = y0;
  const Settings& gs = settings();
#ifdef CD_FLAVOR_WATCH
  snprintf(v, sizeof(v), "%d%%", gs.day_pct);
  mk_row(y, "day light", v, IT_ROW_DAY);
  y += step;
  snprintf(v, sizeof(v), "lvl %d",
           night_duty_step(cal_floor_or_default(), gs.night_duty));
  mk_row(y, "night light", v, IT_ROW_NIGHT);
  y += step;
#endif
  snprintf(v, sizeof(v), "%02d-%02d", gs.night_start_hh, gs.night_end_hh);
  mk_row(y, "night hours", v, IT_ROW_HOURS);
  y += step;
  mk_row(y, "night look", gs.red_shift ? "soft red" : "plain", IT_ROW_LOOK);
  y += step;
  mk_row(y, "at night",
         gs.night_screen == NIGHT_SCREEN_OFF ? "off • peek" : "glow",
         IT_ROW_SCREEN);
  y += step;
  mk_row(y, "style", character_name(active_character()), IT_ROW_STYLE);
  y += step;
#ifdef CD_FLAVOR_DASH
  // The clock-face ring (drawn-clock glass only): segment family + the
  // Analog dial, curated and named like the Character ring.
  mk_row(y, "clock", clock_style_name(settings().clock_style), IT_ROW_CLOCK);
  y += step;
#endif
#ifdef CD_FLAVOR_DASH
  // The 7"/dash RGB glass: how it's turned, how bright it sits, and what it's
  // running. Weekly-or-rarer, so they land on the root one tap from an editor
  // (Zero Layer) — never behind an "Advanced" door.
  mk_row(y, "orientation", rotation_name(gs.rotation), IT_ROW_DISPLAY);
  y += step;
  {
    char b[8];
    snprintf(b, sizeof(b), "%d%%", gs.bright_pct);
    mk_row(y, "brightness", b, IT_ROW_BRIGHT);
    y += step;
  }
  mk_row(y, "firmware", canary::net::ota_status().installed, IT_ROW_FW);
  y += step;
#endif
#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
  // 4.3B only: arm the wired siren (DO0). Disarmed by default — opt-in.
  mk_row(y, "siren", canary::io::field_io_armed() ? "armed" : "off",
         IT_ROW_SIREN);
  y += step;
#endif
#ifdef CD_SET_MIC
  // 4.3C only: the mic opt-in. The row IS part of the always-know contract:
  // it states the live state in the same words the chip and console use.
  mk_row(y, "microphone",
         !canary::io::mic_pins_ok() ? "pins unset"
         : canary::io::mic_listening() ? "listening" : "off",
         IT_ROW_MIC);
  y += step;
#endif
#ifdef CD_FLAVOR_WATCH
  mk_row(y, "find the black point", nullptr, IT_ROW_CAL);
  y += step;
  // Not a screen setting, but the watch's only always-reachable doorway to
  // commissioning (the dash has its own on the transparency sheet).
  mk_row(y, "add a canary", nullptr, IT_ROW_ADD);
  y += step;
#endif
  mk_row(y, "reset", nullptr, IT_ROW_RESET);
#ifdef CD_SET_MODES
  // The glass has gears (display_modes.md): the doorway to the non-fleet
  // modes this build carries. One gear keeps the familiar dev-mode row.
  y += step;
#if (defined(FEATURE_DEMO_MODE) && FEATURE_DEMO_MODE) ||   \
    (defined(FEATURE_DEBUG_MODE) && FEATURE_DEBUG_MODE) || \
    (defined(FEATURE_ARCADE) && FEATURE_ARCADE)
  mk_row(y, "modes", nullptr, IT_ROW_DEV);
#else
  mk_row(y, "dev mode", "bench", IT_ROW_DEV);
#endif
#endif
}

void build_edit_day() {
#ifdef CD_FLAVOR_WATCH
  mk_back("day light");
  lv_obj_t* hero = mk_label(s_host, font_hero(), col_text());
  lv_label_set_text_fmt(hero, "%d%%", settings().day_pct);
  lv_obj_align(hero, LV_ALIGN_CENTER, 0, -14);
  lv_obj_t* cap = mk_label(s_host, font_caption(), col_muted());
  lv_label_set_text(cap, "the screen is the preview");
  lv_obj_align(cap, LV_ALIGN_CENTER, 0, 26);
  mk_stepper();
  set_owns_backlight(true);
  canary::hal::backlight_set(day_level());
#endif
}

void build_edit_night() {
#ifdef CD_FLAVOR_WATCH
  mk_back("night light");
  s_night_lvl = night_duty_step(cal_floor_or_default(), settings().night_duty);
  lv_obj_t* hero = mk_label(s_host, font_hero(), col_text());
  lv_label_set_text_fmt(hero, "%d", s_night_lvl);
  lv_obj_align(hero, LV_ALIGN_CENTER, 0, -14);
  lv_obj_t* cap = mk_label(s_host, font_caption(), col_muted());
  lv_label_set_text_fmt(cap, "of %d • shown at this glow", NIGHT_STEPS);
  lv_obj_align(cap, LV_ALIGN_CENTER, 0, 26);
  mk_stepper();
  set_owns_backlight(true);
  preview_night_level(s_night_lvl);
#endif
}

void build_edit_hours() {
  mk_back("night hours");
  char v[16];
  const Settings& gs = settings();
  int y = ROOT_Y0 + ROW_H / 2;
  snprintf(v, sizeof(v), "%02d:00", gs.night_start_hh);
  mk_row(y, "starts", v, IT_OPT_A, s_hours_sel == 0);
  y += ROW_H;
  snprintf(v, sizeof(v), "%02d:00", gs.night_end_hh);
  mk_row(y, "ends", v, IT_OPT_B, s_hours_sel == 1);
  y += ROW_H;
  lv_obj_t* cap = mk_label(s_host, font_caption(), col_faint());
  lv_label_set_text(cap, "tap a line, then step it");
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, y + 6);
  mk_stepper();
}

void build_edit_look() {
  mk_back("night look");
  const bool red = settings().red_shift != 0;
  int y = ROOT_Y0 + ROW_H / 2;
  mk_row(y, "soft red", red ? "on" : nullptr, IT_OPT_A, red);
  y += ROW_H;
  mk_row(y, "plain", !red ? "on" : nullptr, IT_OPT_B, !red);
  y += ROW_H;
  lv_obj_t* cap = mk_label(s_host, font_caption(),
                           red ? ncol_text() : col_faint());
  lv_label_set_text(cap, "red is easier on sleepy eyes");
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, y + 6);
}

void build_edit_screen() {
  mk_back("at night");
  const Settings& gs = settings();
  const bool off = gs.night_screen == NIGHT_SCREEN_OFF;
  int y = ROOT_Y0 + ROW_H / 2;
#ifdef CD_FLAVOR_WATCH
  mk_row(y, "keep a low glow", !off ? "on" : nullptr, IT_OPT_A, !off);
  y += ROW_H;
  mk_row(y, "go dark • tap peeks", off ? "on" : nullptr, IT_OPT_B, off);
#else
  mk_row(y, "stay lit, dark look", !off ? "on" : nullptr, IT_OPT_A, !off);
  y += ROW_H;
  mk_row(y, "go dark • tap wakes", off ? "on" : nullptr, IT_OPT_B, off);
#endif
  y += ROW_H;
  if (off) {
    char v[12];
    snprintf(v, sizeof(v), "%d s", gs.peek_s);
    mk_row(y, "peek for", v, IT_PEEK);
    y += ROW_H;
  }
  lv_obj_t* cap = mk_label(s_host, font_caption(), col_faint());
  lv_label_set_text(cap, "an alert always lights the glass");
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, y + 6);
}

#ifdef CD_FLAVOR_DASH
// Orientation (7"/dash glass): four quarter turns, landing IS choosing —
// the glass rotates under your thumb the instant you tap (The Screen Is the
// Preview, taken literally). The bench turns the panel; the software follows.
void build_edit_display() {
  mk_back("orientation");
  const uint8_t rot = settings().rotation;
  int y = ROOT_Y0 + ROW_H / 2;
  mk_row(y, "landscape", rot == ROT_LANDSCAPE ? "on" : nullptr,
         IT_ROT_0, rot == ROT_LANDSCAPE);
  y += ROW_H;
  mk_row(y, "portrait", rot == ROT_PORTRAIT ? "on" : nullptr,
         IT_ROT_90, rot == ROT_PORTRAIT);
  y += ROW_H;
  mk_row(y, "landscape flipped", rot == ROT_LANDSCAPE_INV ? "on" : nullptr,
         IT_ROT_180, rot == ROT_LANDSCAPE_INV);
  y += ROW_H;
  mk_row(y, "portrait flipped", rot == ROT_PORTRAIT_INV ? "on" : nullptr,
         IT_ROT_270, rot == ROT_PORTRAIT_INV);
  y += ROW_H;
  lv_obj_t* cap = mk_label(s_host, font_caption(), col_faint());
  lv_label_set_text(cap,
      "portrait suits a wall column or a tall\n"
      "bedside face; the layout follows the turn.");
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, y + 6);
}

// Brightness (7"/dash glass): the backlight is on/off in hardware, so this
// is a RENDERED dim — a black scrim over the glass, live as you step it. It
// bottoms out at 50% on purpose (BRIGHT_PCT_MIN): darker is Night's job, and
// a scrim can't lower real backlight power, so an all-day floor stays honest.
void build_edit_bright() {
  mk_back("brightness");
  lv_obj_t* hero = mk_label(s_host, font_hero(), col_text());
  lv_label_set_text_fmt(hero, "%d%%", settings().bright_pct);
  lv_obj_align(hero, LV_ALIGN_CENTER, 0, -14);
  lv_obj_t* cap = mk_label(s_host, font_caption(), col_muted());
  lv_label_set_text(cap, "the screen is the preview");
  lv_obj_align(cap, LV_ALIGN_CENTER, 0, 26);
  mk_stepper();
  // Dim to the current setting right now, so the hero and the glass agree.
  lvgl_port_set_dim(bright_scrim_opa(settings().bright_pct));
}

// The clock-face picker: a flip-through on the same ring idiom as the
// Character picker — every stop is a validated face, flipping IS choosing.
// The hero digits live UNDER this sheet, so the caption says when the new
// face shows; the ground-flip tracker in main.cpp rebuilds the face the
// moment the sheet closes.
void build_edit_clock() {
  mk_back("clock");
  const uint8_t cur = settings().clock_style;
  lv_obj_t* name = mk_label(s_host, font_title(), col_text());
  lv_label_set_text(name, clock_style_name(cur));
  lv_obj_align(name, LV_ALIGN_CENTER, 0, -30);
  lv_obj_t* cap = mk_label(s_host, font_caption(), col_muted());
  lv_label_set_text(cap, clock_style_caption(cur));
  lv_obj_align(cap, LV_ALIGN_CENTER, 0, 2);
  char dots[2 * 8 + 4];
  int n = 0;
  for (uint8_t i = 0; i < clock_style_count() && n < (int)sizeof(dots) - 4; i++)
    n += snprintf(dots + n, sizeof(dots) - n, "%s%s", i ? " " : "",
                  i == cur ? "\xE2\x80\xA2" : "o");
  lv_obj_t* ring = mk_label(s_host, font_body(), col_accent());
  lv_label_set_text(ring, dots);
  lv_obj_align(ring, LV_ALIGN_CENTER, 0, 34);
  lv_obj_t* note = mk_label(s_host, font_caption(), col_faint());
  lv_label_set_text(note, "the face wears it when you leave settings");
  lv_obj_align(note, LV_ALIGN_CENTER, 0, 62);
  lv_obj_t* l = mk_label(s_host, font_title(), col_text());
  lv_label_set_text(l, LV_SYMBOL_LEFT);
  lv_obj_t* r = mk_label(s_host, font_title(), col_text());
  lv_label_set_text(r, LV_SYMBOL_RIGHT);
  lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 72, -40);
  lv_obj_align(r, LV_ALIGN_BOTTOM_RIGHT, -72, -40);
  add_item(l, IT_MINUS);
  add_item(r, IT_PLUS);
}

// Firmware: the version this glass is running, and the one signed-and-
// rollback-safe path to a newer one (same engine HA drives). No raw version
// strings typed anywhere — the manifest and the release key are the truth.
void build_edit_firmware() {
  mk_back("firmware");
  const canary::net::OtaStatus ota = canary::net::ota_status();
  int y = ROOT_Y0 + ROW_H / 2;
  mk_row(y, "installed", ota.installed, /*inert*/ 0);
  y += ROW_H;
  if (ota.update_available)
    mk_row(y, "available", ota.latest, /*inert*/ 0);
  else
    mk_row(y, "status", ota.state_text, /*inert*/ 0);
  y += ROW_H;
  mk_row(y, "auto-update", ota.auto_update ? "on" : "off",
         IT_ROW_FW_AUTO, ota.auto_update);
  y += ROW_H;
  // The single action, chosen by state: install what's waiting, or go look.
  lv_obj_t* act = mk_label(s_host, font_body(),
                           ota.busy ? col_muted() : col_signed());
  if (ota.busy) {
    lv_label_set_text_fmt(act, "installing" LV_SYMBOL_DOWNLOAD " %u%%",
                          (unsigned)ota.progress);
  } else if (ota.update_available) {
    lv_label_set_text(act, "install now");
    add_item(act, IT_YES);
  } else {
    lv_label_set_text(act, "check for updates");
    add_item(act, IT_GO);
  }
  lv_obj_align(act, LV_ALIGN_BOTTOM_MID, 0, -34);
  lv_obj_t* cap = mk_label(s_host, font_caption(), col_faint());
  lv_label_set_text(cap, ota.dev_channel
      ? "dev channel • signed + rollback-safe"
      : "release channel • signed + rollback-safe");
  lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, -8);
}
#endif  // CD_FLAVOR_DASH

#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
// Siren arming (4.3B): one decision, two options — same shape as "night look".
// The alert always shows on the glass; this only governs the wired output, so
// the caption says so plainly (silence must never be mistaken for safety).
void build_edit_siren() {
  mk_back("siren");
  const bool armed = canary::io::field_io_armed();
  int y = ROOT_Y0 + ROW_H / 2;
  mk_row(y, "armed", armed ? "on" : nullptr, IT_OPT_A, armed);
  y += ROW_H;
  mk_row(y, "silent", !armed ? "on" : nullptr, IT_OPT_B, !armed);
  y += ROW_H;
  lv_obj_t* cap = mk_label(s_host, font_caption(), col_faint());
  lv_label_set_text(cap,
                    "drives the wired siren on an unacked\n"
                    "alert - the glass shows it either way");
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, y + 6);
}
#endif

// The Character picker (display_character.md §7): a flip-through, not a
// swatch grid. The screen IS the preview — by the time this builds, the
// palette and type ladder already belong to the look being named.
void build_edit_style() {
  mk_back("style");
  const Character cur = active_character();
  lv_obj_t* name = mk_label(s_host, font_title(), col_text());
  lv_label_set_text(name, character_name(cur));
  lv_obj_align(name, LV_ALIGN_CENTER, 0, -30);
  lv_obj_t* cap = mk_label(s_host, font_caption(), col_muted());
  lv_label_set_text(cap, character_caption(cur));
  lv_obj_align(cap, LV_ALIGN_CENTER, 0, 2);
  // Where you are on the ring: the filled stop is here. "•" is the one
  // round glyph the built-in Montserrat range carries (the root rows
  // already trust it); "o" stands in for the hollow stops.
  char dots[2 * (int)Character::Count + 4];
  int n = 0;
  const uint8_t here = character_ring_pos(cur);
  for (uint8_t i = 0; i < character_count() && n < (int)sizeof(dots) - 4; i++)
    n += snprintf(dots + n, sizeof(dots) - n, "%s%s", i ? " " : "",
                  i == here ? "•" : "o");
  lv_obj_t* ring = mk_label(s_host, font_body(), col_accent());
  lv_label_set_text(ring, dots);
  lv_obj_align(ring, LV_ALIGN_CENTER, 0, 34);
  // Flip affordances, stepper-cornered: every stop on the ring is a
  // validated look, so flipping IS choosing — no apply, no confirm.
  lv_obj_t* l = mk_label(s_host, font_title(), col_text());
  lv_label_set_text(l, LV_SYMBOL_LEFT);
  lv_obj_t* r = mk_label(s_host, font_title(), col_text());
  lv_label_set_text(r, LV_SYMBOL_RIGHT);
#ifdef CD_FLAVOR_WATCH
  lv_obj_align(l, LV_ALIGN_CENTER, -58, 62);
  lv_obj_align(r, LV_ALIGN_CENTER, 58, 62);
#else
  lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 72, -40);
  lv_obj_align(r, LV_ALIGN_BOTTOM_RIGHT, -72, -40);
#endif
  add_item(l, IT_MINUS);
  add_item(r, IT_PLUS);
}

void build_cal_intro() {
#ifdef CD_FLAVOR_WATCH
  mk_back("black point");
  lv_obj_t* body = mk_label(s_host, font_body(), col_text());
  lv_label_set_text(body,
                    "Best done at night, with\n"
                    "the room lights how you\n"
                    "sleep. The glass dims on\n"
                    "its own - tap the moment\n"
                    "the glow disappears.");
  lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(body, LV_ALIGN_CENTER, 0, -8);
  lv_obj_t* go = mk_label(s_host, font_body(), col_signed());
  lv_label_set_text(go, "begin");
  lv_obj_align(go, LV_ALIGN_CENTER, 0, 74);
  add_item(go, IT_GO);
#endif
}

void build_cal_descend() {
#ifdef CD_FLAVOR_WATCH
  s_cal_clock = mk_label(s_host, font_hero(), col_text());
  char c[8];
  fmt_clock(c, sizeof(c));
  lv_label_set_text(s_cal_clock, c);
  lv_obj_align(s_cal_clock, LV_ALIGN_CENTER, 0, -16);
  lv_obj_t* cap = mk_label(s_host, font_caption(), col_muted());
  lv_label_set_text(cap, "tap when the glow is gone");
  lv_obj_align(cap, LV_ALIGN_CENTER, 0, 34);

  set_owns_backlight(true);
  s_cal_duty = 164;  // ~2%: clearly visible start
  s_cal_blinks = 0;
  canary::hal::backlight_night_set(s_cal_duty);
  s_cal_timer = lv_timer_create(cal_descend_cb, 1200, nullptr);
#endif
}

void build_cal_comfort() {
#ifdef CD_FLAVOR_WATCH
  s_night_lvl = 3;
  lv_obj_t* hero = mk_label(s_host, font_hero(), col_text());
  lv_label_set_text_fmt(hero, "%d", s_night_lvl);
  lv_obj_align(hero, LV_ALIGN_CENTER, 0, -14);
  lv_obj_t* cap = mk_label(s_host, font_caption(), col_muted());
  lv_label_set_text(cap, "pick a 3 a.m. glow • tap here");
  lv_obj_align(cap, LV_ALIGN_CENTER, 0, 26);
  add_item(cap, IT_GO);
  add_item(hero, IT_GO);
  mk_stepper();
  set_owns_backlight(true);
  canary::hal::backlight_night_set(night_step_duty(s_cal_floor, s_night_lvl));
#endif
}

void build_cal_blink() {
#ifdef CD_FLAVOR_WATCH
  lv_obj_t* body = mk_label(s_host, font_body(), col_text());
  lv_label_set_text(body, "Did the glass wink\nat you just now?");
  lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(body, LV_ALIGN_CENTER, 0, -34);
  lv_obj_t* yes = mk_label(s_host, font_body(), col_signed());
  lv_label_set_text(yes, "yes");
  lv_obj_align(yes, LV_ALIGN_CENTER, -46, 40);
  add_item(yes, IT_YES);
  lv_obj_t* no = mk_label(s_host, font_body(), col_muted());
  lv_label_set_text(no, "no");
  lv_obj_align(no, LV_ALIGN_CENTER, 46, 40);
  add_item(no, IT_NO);

  set_owns_backlight(true);
  s_cal_blinks = 0;
  s_cal_lit = false;
  s_cal_timer = lv_timer_create(cal_blink_cb, 350, nullptr);
#endif
}

void build_cal_warn() {
  mk_back("black point");
  lv_obj_t* body = mk_label(s_host, font_body(), col_text());
  lv_label_set_text(body,
                    "That floor came out bright -\n"
                    "something looks off. Keeping\n"
                    "the factory floor • try again\n"
                    "on a darker night.");
  lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(body, LV_ALIGN_CENTER, 0, 8);
}

void build_cal_done() {
  mk_back("black point");
  lv_obj_t* body = mk_label(s_host, font_body(), col_text());
  if (s_cal_persisted) {
    lv_label_set_text(body,
                      "Saved. Night light will\n"
                      "never go below this -\n"
                      "redo it here anytime.");
  } else {
    // Honesty over comfort: the floor works right now but storage balked,
    // so it won't survive a restart. Say so instead of a false "saved".
    lv_label_set_text(body,
                      "Kept for tonight - but\n"
                      "storage balked, so it\n"
                      "resets on restart.\n"
                      "Redo it here anytime.");
  }
  lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(body, LV_ALIGN_CENTER, 0, 8);
}

void build_reset_confirm() {
  mk_back("reset");
  lv_obj_t* body = mk_label(s_host, font_body(), col_text());
  lv_label_set_text(body, "Back to the defaults?\nYour black point stays.");
  lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(body, LV_ALIGN_CENTER, 0, -26);
  lv_obj_t* yes = mk_label(s_host, font_body(), col_signed());
  lv_label_set_text(yes, "reset");
  lv_obj_align(yes, LV_ALIGN_CENTER, -52, 44);
  add_item(yes, IT_YES);
  lv_obj_t* no = mk_label(s_host, font_body(), col_muted());
  lv_label_set_text(no, "keep");
  lv_obj_align(no, LV_ALIGN_CENTER, 52, 44);
  add_item(no, IT_NO);
}

#ifdef CD_SET_MIC
// Mic opt-in (4.3C): one decision, two options — the siren page's shape.
// The caption carries the whole contract in the fewest honest words: what
// listening means, and how you always know.
void build_edit_mic() {
  mk_back("microphone");
  const bool pins = canary::io::mic_pins_ok();
  const bool on = canary::io::mic_armed();
  int y = ROOT_Y0 + ROW_H / 2;
  mk_row(y, "listening", (pins && on) ? "on" : nullptr, IT_OPT_A, pins && on);
  y += ROW_H;
  mk_row(y, "off", !(pins && on) ? "on" : nullptr, IT_OPT_B, !(pins && on));
  y += ROW_H;
  // The room preset — how loud a sound must be to count as a beep. Detection
  // cadence is standards-fixed; this only sets the noise-floor margin.
  mk_row(y, "sensitivity", canary::io::mic_sensitivity_name(), IT_ROW_MIC_SENS);
  y += ROW_H;
  // Opt-in: a loud sound (a door close) wakes the screen. Same envelope the
  // alarm uses — never speech, nothing recorded. Off by default.
  mk_row(y, "wake on sound", canary::io::mic_wake_on_sound() ? "on" : "off",
         IT_ROW_MIC_WAKE, canary::io::mic_wake_on_sound());
  y += ROW_H;
  lv_obj_t* cap = mk_label(s_host, font_caption(), col_faint());
  lv_label_set_text(cap,
      pins ? "hears alarm patterns - and, with wake on,\n"
             "loud sounds (a door) to light the screen.\n"
             "never speech; nothing recorded, nothing\n"
             "leaves this board. amber chip = listening;\n"
             "off is a real mute (driver uninstalled)."
           : "audio pins are unset (VERIFY in pins.h) -\n"
             "the mics are provably un-driven until the\n"
             "bench fills them. see the board README.");
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, y + 6);
}

// Sensitivity preset picker (landing IS choosing). Tuned for the room, not
// the alarm: quiet = bedroom (most sensitive), standard = living areas,
// noisy = kitchen/workshop (least twitchy). The cadence match is unchanged.
void build_edit_mic_sens() {
  mk_back("sensitivity");
  const uint8_t cur = canary::io::mic_sensitivity();
  int y = ROOT_Y0 + ROW_H / 2;
  mk_row(y, "quiet", cur == 0 ? "on" : nullptr, IT_OPT_A, cur == 0);
  y += ROW_H;
  mk_row(y, "standard", cur == 1 ? "on" : nullptr, IT_OPT_B, cur == 1);
  y += ROW_H;
  mk_row(y, "noisy", cur == 2 ? "on" : nullptr, IT_OPT_C, cur == 2);
  y += ROW_H;
  lv_obj_t* cap = mk_label(s_host, font_caption(), col_faint());
  lv_label_set_text(cap,
      "quiet: a bedroom - catches a faint or\n"
      "distant alarm. noisy: a kitchen/workshop -\n"
      "ignores clatter. it adapts to the room\n"
      "either way; this sets the margin.");
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, y + 6);
}
#endif

#ifdef CD_SET_MODES
// Which gear is awaiting its confirm tap (ModesList -> ModeConfirm).
canary::mode::Mode s_pending_mode = canary::mode::Mode::Fleet;

void build_modes_list() {
  mk_back("modes");
#ifdef CD_FLAVOR_WATCH
  const int y0 = 48, step = 26;
#else
  const int y0 = ROOT_Y0, step = ROW_H;
#endif
  int y = y0;
#if defined(FEATURE_DEVMODE) && FEATURE_DEVMODE
  mk_row(y, "bench", "peripheral test", IT_ROW_MBENCH);
  y += step;
#endif
#if defined(FEATURE_DEMO_MODE) && FEATURE_DEMO_MODE
  mk_row(y, "demo", "scripted fleet", IT_ROW_MDEMO);
  y += step;
#endif
#if defined(FEATURE_DEBUG_MODE) && FEATURE_DEBUG_MODE
  mk_row(y, "debug", "diagnostics", IT_ROW_MDEBUG);
  y += step;
#endif
#ifdef CD_SET_ROW_ARCADE
  mk_row(y, "arcade", "touch QA", IT_ROW_MARCADE);
  y += step;
#endif
  (void)y;
}

void build_mode_confirm() {
  using canary::mode::Mode;
  const char* title = canary::mode::mode_token(s_pending_mode);
  const char* blurb = "";
  switch (s_pending_mode) {
    case Mode::Bench:
      blurb = "Enter dev mode?\nReboots into the peripheral\nbench - no fleet, no network.";
      break;
    case Mode::Demo:
      blurb = "Enter demo mode?\nA scripted household plays on\nthis glass - no fleet, no network.";
      break;
    case Mode::Debug:
      blurb = "Enter debug mode?\nOn-glass diagnostics - network\nup, no updates, read-mostly.";
      break;
    case Mode::Arcade:
      blurb = "Enter arcade mode?\nCanary Catch, the touch QA\nround - no fleet, no network.";
      break;
    default:
      break;
  }
  mk_back(title);
  lv_obj_t* body = mk_label(s_host, font_body(), col_text());
  lv_label_set_text(body, blurb);
  lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(body, LV_ALIGN_CENTER, 0, -26);
  lv_obj_t* yes = mk_label(s_host, font_body(), col_signed());
  lv_label_set_text(yes, "enter");
  lv_obj_align(yes, LV_ALIGN_CENTER, -52, 44);
  add_item(yes, IT_YES);
  lv_obj_t* no = mk_label(s_host, font_body(), col_muted());
  lv_label_set_text(no, "stay");
  lv_obj_align(no, LV_ALIGN_CENTER, 52, 44);
  add_item(no, IT_NO);
}
#endif

void build(Page pg) {
  clear_host();
  set_owns_backlight(false);
  s_page = pg;
  switch (pg) {
    case Page::Root:         build_root(); break;
    case Page::EditDay:      build_edit_day(); break;
    case Page::EditNight:    build_edit_night(); break;
    case Page::EditHours:    build_edit_hours(); break;
    case Page::EditLook:     build_edit_look(); break;
    case Page::EditScreen:   build_edit_screen(); break;
#ifdef CD_FLAVOR_DASH
    case Page::EditDisplay:  build_edit_display(); break;
    case Page::EditBright:   build_edit_bright(); break;
    case Page::EditClock:    build_edit_clock(); break;
    case Page::EditFirmware: build_edit_firmware(); break;
#endif
#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
    case Page::EditSiren:    build_edit_siren(); break;
#endif
#ifdef CD_SET_MIC
    case Page::EditMic:      build_edit_mic(); break;
    case Page::EditMicSens:  build_edit_mic_sens(); break;
#endif
    case Page::EditStyle:    build_edit_style(); break;
    case Page::CalIntro:     build_cal_intro(); break;
    case Page::CalDescend:   build_cal_descend(); break;
    case Page::CalComfort:   build_cal_comfort(); break;
    case Page::CalBlink:     build_cal_blink(); break;
    case Page::CalWarn:      build_cal_warn(); break;
    case Page::CalDone:      build_cal_done(); break;
    case Page::ResetConfirm: build_reset_confirm(); break;
#ifdef CD_SET_MODES
    case Page::ModesList:    build_modes_list(); break;
    case Page::ModeConfirm:  build_mode_confirm(); break;
#endif
  }
}

// ── Tap dispatch ─────────────────────────────────────────────────────────

void step_value(int dir) {
  Settings& gs = settings_mut();
  switch (s_page) {
#ifdef CD_FLAVOR_WATCH
    case Page::EditDay: {
      int v = gs.day_pct + dir * 10;
      if (v < 20) v = 20;
      if (v > 100) v = 100;
      gs.day_pct = (uint8_t)v;
      settings_mark_dirty();
      build(Page::EditDay);  // rebuild refreshes numeral + live preview
      break;
    }
    case Page::EditNight: {
      int v = s_night_lvl + dir;
      if (v < 1) v = 1;
      if (v > NIGHT_STEPS) v = NIGHT_STEPS;
      s_night_lvl = v;
      gs.night_duty = night_step_duty(cal_floor_or_default(), v);
      settings_mark_dirty();
      build(Page::EditNight);
      break;
    }
    case Page::CalComfort: {
      int v = s_night_lvl + dir;
      if (v < 1) v = 1;
      if (v > NIGHT_STEPS) v = NIGHT_STEPS;
      s_night_lvl = v;
      build(Page::CalComfort);
      break;
    }
#endif
    case Page::EditHours: {
      uint8_t* hh = s_hours_sel == 0 ? &settings_mut().night_start_hh
                                     : &settings_mut().night_end_hh;
      *hh = (uint8_t)((*hh + 24 + dir) % 24);
      settings_mark_dirty();
      build(Page::EditHours);
      break;
    }
#ifdef CD_FLAVOR_DASH
    case Page::EditBright: {
      gs.bright_pct = bright_pct_clamp((int)gs.bright_pct + dir * BRIGHT_PCT_STEP);
      settings_mark_dirty();
      lvgl_port_set_dim(bright_scrim_opa(gs.bright_pct));  // live preview
      build(Page::EditBright);
      break;
    }
#endif
    default:
      break;
  }
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
  s_item_n = 0;
  set_owns_backlight(false);
  lv_obj_t* dying = s_scr;
  s_scr = nullptr;
  s_host = nullptr;
  lv_scr_load(s_prev);
  s_prev = nullptr;
  lv_obj_del(dying);
}

void dispatch(int id) {
  switch (s_page) {
    case Page::Root:
      switch (id) {
        case IT_BACK:       settings_ui_close(); return;
        case IT_ROW_DAY:    build(Page::EditDay); return;
        case IT_ROW_NIGHT:  build(Page::EditNight); return;
        case IT_ROW_HOURS:  s_hours_sel = 0; build(Page::EditHours); return;
        case IT_ROW_LOOK:   build(Page::EditLook); return;
        case IT_ROW_SCREEN: build(Page::EditScreen); return;
#ifdef CD_FLAVOR_DASH
        case IT_ROW_DISPLAY: build(Page::EditDisplay); return;
        case IT_ROW_BRIGHT:  build(Page::EditBright); return;
        case IT_ROW_CLOCK:   build(Page::EditClock); return;
        case IT_ROW_FW:      build(Page::EditFirmware); return;
#endif
#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
        case IT_ROW_SIREN:  build(Page::EditSiren); return;
#endif
#ifdef CD_SET_MIC
        case IT_ROW_MIC:    build(Page::EditMic); return;
#endif
        case IT_ROW_STYLE:  build(Page::EditStyle); return;
        case IT_ROW_CAL:    build(Page::CalIntro); return;
        case IT_ROW_RESET:  build(Page::ResetConfirm); return;
        case IT_ROW_ADD:
          close_instant();
          commission_ui_open();
          return;
#ifdef CD_SET_MODES
        case IT_ROW_DEV:
          build(Page::ModesList);
          return;
#endif
      }
      return;

    case Page::EditDay:
    case Page::EditNight:
      if (id == IT_BACK) { build(Page::Root); return; }
      if (id == IT_MINUS) step_value(-1);
      if (id == IT_PLUS) step_value(+1);
      return;

    case Page::EditHours:
      if (id == IT_BACK) { build(Page::Root); return; }
      if (id == IT_OPT_A) { s_hours_sel = 0; build(Page::EditHours); return; }
      if (id == IT_OPT_B) { s_hours_sel = 1; build(Page::EditHours); return; }
      if (id == IT_MINUS) step_value(-1);
      if (id == IT_PLUS) step_value(+1);
      return;

    case Page::EditLook:
      if (id == IT_BACK) { build(Page::Root); return; }
      if (id == IT_OPT_A || id == IT_OPT_B) {
        settings_mut().red_shift = (id == IT_OPT_A) ? 1 : 0;
        settings_mark_dirty();
        build(Page::EditLook);
      }
      return;

    case Page::EditScreen:
      if (id == IT_BACK) { build(Page::Root); return; }
      if (id == IT_OPT_A || id == IT_OPT_B) {
        settings_mut().night_screen =
            (id == IT_OPT_B) ? NIGHT_SCREEN_OFF : NIGHT_SCREEN_DIM;
        settings_mark_dirty();
        build(Page::EditScreen);
      }
      if (id == IT_PEEK) {
        Settings& gs = settings_mut();
        gs.peek_s = gs.peek_s == 3 ? 5 : gs.peek_s == 5 ? 10 : 3;
        settings_mark_dirty();
        build(Page::EditScreen);
      }
      return;

#ifdef CD_FLAVOR_DASH
    case Page::EditDisplay: {
      if (id == IT_BACK) { build(Page::Root); return; }
      int nr = -1;
      switch (id) {
        case IT_ROT_0:   nr = ROT_LANDSCAPE;     break;
        case IT_ROT_90:  nr = ROT_PORTRAIT;      break;
        case IT_ROT_180: nr = ROT_LANDSCAPE_INV; break;
        case IT_ROT_270: nr = ROT_PORTRAIT_INV;  break;
      }
      if (nr >= 0) {
        // Landing IS choosing: persist + rotate the live glass (settings
        // sheet and all), then rebuild so the check moves to the new turn.
        settings_mut().rotation = (uint8_t)nr;
        settings_mark_dirty();
        lvgl_port_set_rotation((uint8_t)nr);
        build(Page::EditDisplay);
      }
      return;
    }

    case Page::EditBright:
      if (id == IT_BACK) { build(Page::Root); return; }
      if (id == IT_MINUS) step_value(-1);
      if (id == IT_PLUS) step_value(+1);
      return;

    case Page::EditClock:
      if (id == IT_BACK) { build(Page::Root); return; }
      if (id == IT_MINUS || id == IT_PLUS) {
        // Landing IS choosing: persist on every flip. The face itself
        // rebuilds when the sheet closes (main.cpp's ground-flip tracker) —
        // the hero lives under this sheet, so there is nothing to preview
        // here beyond the name and its caption.
        Settings& gs = settings_mut();
        gs.clock_style =
            clock_style_step(gs.clock_style, id == IT_PLUS ? +1 : -1);
        settings_mark_dirty();
        build(Page::EditClock);
      }
      return;

    case Page::EditFirmware:
      if (id == IT_BACK) { build(Page::Root); return; }
      if (id == IT_ROW_FW_AUTO) {
        // Toggle-on-tap: flip the nightly auto-install preference (NVS'd),
        // then redraw the row's on/off value.
        canary::net::ota_set_auto_update(!canary::net::ota_status().auto_update);
        build(Page::EditFirmware);
        return;
      }
      if (id == IT_GO) {  // "check for updates"
        canary::net::ota_request_check();
        build(Page::EditFirmware);
        return;
      }
      if (id == IT_YES) {  // "install now" — may reboot on success
        canary::net::ota_request_install();
        build(Page::EditFirmware);
        return;
      }
      return;
#endif  // CD_FLAVOR_DASH

#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
    case Page::EditSiren:
      if (id == IT_BACK) { build(Page::Root); return; }
      if (id == IT_OPT_A || id == IT_OPT_B) {
        // Landing IS choosing (like night look): persist the arm state to NVS
        // through field_io, then rebuild so the row reflects it immediately.
        canary::io::field_io_set_armed(id == IT_OPT_A);
        build(Page::EditSiren);
      }
      return;
#endif
#ifdef CD_SET_MIC
    case Page::EditMic:
      if (id == IT_BACK) { build(Page::Root); return; }
      if (id == IT_ROW_MIC_SENS) { build(Page::EditMicSens); return; }
      if (id == IT_ROW_MIC_WAKE) {
        // Toggle-on-tap: flip the opt-in and redraw the row's on/off value.
        canary::io::mic_set_wake_on_sound(!canary::io::mic_wake_on_sound());
        build(Page::EditMic);
        return;
      }
      if (id == IT_OPT_A || id == IT_OPT_B) {
        // Landing IS choosing. mic_set_armed persists to NVS and performs
        // the gate action in the same call — driver AND the amber chip
        // together; with pins unset the gate refuses regardless of choice.
        canary::io::mic_set_armed(id == IT_OPT_A);
        build(Page::EditMic);
      }
      return;
    case Page::EditMicSens:
      if (id == IT_BACK) { build(Page::EditMic); return; }
      if (id == IT_OPT_A || id == IT_OPT_B || id == IT_OPT_C) {
        // Landing IS choosing: persist the preset and re-seed the floor.
        canary::io::mic_set_sensitivity(id == IT_OPT_A ? 0
                                        : id == IT_OPT_B ? 1
                                                         : 2);
        build(Page::EditMicSens);
      }
      return;
#endif

    case Page::EditStyle:
      if (id == IT_BACK) { build(Page::Root); return; }
      if (id == IT_MINUS || id == IT_PLUS) {
        // Landing IS choosing: apply + persist on every flip, restyle
        // the open surface, and rebuild so name, caption, dots, and the
        // type ladder all arrive in the new look.
        const Character next =
            character_ring_step(active_character(), id == IT_PLUS ? +1 : -1);
        character_apply(next);
        settings_mut().character = (uint8_t)next;
        settings_mark_dirty();
        restyle_open_surface();
        build(Page::EditStyle);
      }
      return;

    case Page::CalIntro:
      if (id == IT_BACK) { build(Page::Root); return; }
      if (id == IT_GO) { s_cal_retries = 0; build(Page::CalDescend); }
      return;

    case Page::CalDescend:
      // ANY tap = "it just disappeared": step back up a touch for margin.
      s_cal_floor = (uint16_t)(s_cal_duty + (s_cal_duty / 4 > 2 ? s_cal_duty / 4
                                                                : 2));
      if (s_cal_floor > NIGHT_FLOOR_CAP) {
        build(Page::CalWarn);
      } else {
        build(Page::CalComfort);
      }
      return;

    case Page::CalComfort:
      if (id == IT_MINUS || id == IT_PLUS) {
        step_value(id == IT_PLUS ? +1 : -1);
        canary::hal::backlight_night_set(
            night_step_duty(s_cal_floor, s_night_lvl));
        return;
      }
      if (id == IT_GO) build(Page::CalBlink);
      return;

    case Page::CalBlink:
      if (id == IT_YES) {
        // The floor provably emits light on this panel: keep it. RAM holds
        // it for tonight regardless; the return says whether it survives a
        // reboot, and CalDone tells the truth either way.
        s_cal_persisted = nightcal_put(s_cal_floor);
        Settings& gs = settings_mut();
        gs.night_duty = night_step_duty(s_cal_floor, s_night_lvl);
        settings_mark_dirty();
        build(Page::CalDone);
        return;
      }
      if (id == IT_NO) {
        // Confirmed duty was capacitance, not light — raise and re-test.
        s_cal_floor = (uint16_t)(s_cal_floor * 2 + 2);
        if (s_cal_floor > NIGHT_FLOOR_CAP || ++s_cal_retries >= 3) {
          build(Page::CalWarn);
        } else {
          build(Page::CalBlink);
        }
      }
      return;

    case Page::CalWarn:
    case Page::CalDone:
      build(Page::Root);
      return;

    case Page::ResetConfirm:
      if (id == IT_YES) {
        settings_reset();
        // Reset comes home in look too (defaults() holds Quiet Glass):
        // re-apply so the glass doesn't keep wearing a Character the
        // blob no longer holds until the next boot.
        character_apply((Character)settings().character);
        restyle_open_surface();
      }
      build(Page::Root);
      return;

#ifdef CD_SET_MODES
    case Page::ModesList:
      switch (id) {
        case IT_BACK: build(Page::Root); return;
#if defined(FEATURE_DEVMODE) && FEATURE_DEVMODE
        case IT_ROW_MBENCH:
          s_pending_mode = canary::mode::Mode::Bench;
          build(Page::ModeConfirm);
          return;
#endif
#if defined(FEATURE_DEMO_MODE) && FEATURE_DEMO_MODE
        case IT_ROW_MDEMO:
          s_pending_mode = canary::mode::Mode::Demo;
          build(Page::ModeConfirm);
          return;
#endif
#if defined(FEATURE_DEBUG_MODE) && FEATURE_DEBUG_MODE
        case IT_ROW_MDEBUG:
          s_pending_mode = canary::mode::Mode::Debug;
          build(Page::ModeConfirm);
          return;
#endif
#ifdef CD_SET_ROW_ARCADE
        case IT_ROW_MARCADE:
          s_pending_mode = canary::mode::Mode::Arcade;
          build(Page::ModeConfirm);
          return;
#endif
      }
      return;

    case Page::ModeConfirm:
      if (id == IT_YES) {
        // Latch the chosen gear for the NEXT boot and reboot into it. The
        // glue owns the NVS grammar (token + the legacy devmode bool for
        // the bench); every gear's own 3 s long-press exits back here.
        canary::mode::mode_request(s_pending_mode);  // does not return
        return;
      }
      build(Page::ModesList);  // "stay" / back
      return;
#endif
  }
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────

void settings_ui_open() {
  if (s_scr) return;
  s_prev = lv_scr_act();
  s_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s_scr, col_bg(), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
#ifdef CD_FLAVOR_WATCH
  s_host = s_scr;
#else
  // Roomy centered sheet — the dash face stays a poster, settings a card.
  s_host = lv_obj_create(s_scr);
  lv_obj_set_size(s_host, SHEET_W, SHEET_H);
  lv_obj_align(s_host, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(s_host, col_surface(), 0);
  lv_obj_set_style_bg_opa(s_host, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_host, col_edge(), 0);
  lv_obj_set_style_border_width(s_host, 1, 0);
  lv_obj_set_style_radius(s_host, 12, 0);
  lv_obj_set_style_pad_all(s_host, 0, 0);
  lv_obj_clear_flag(s_host, LV_OBJ_FLAG_SCROLLABLE);
#endif
  s_last_touch_ms = millis();
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
  s_item_n = 0;
  set_owns_backlight(false);
  s_scr = nullptr;
  s_host = nullptr;
  lv_scr_load_anim(s_prev, LV_SCR_LOAD_ANIM_FADE_ON, MOTION_PAGE_MS, 0, true);
  s_prev = nullptr;
}

bool settings_ui_active() { return s_scr != nullptr; }

bool settings_ui_owns_backlight() {
  return s_scr != nullptr && s_owns_backlight;
}

void settings_ui_handle_tap(int16_t x, int16_t y) {
  if (!s_scr) return;
  s_last_touch_ms = millis();
  for (int i = 0; i < s_item_n; i++) {
    lv_area_t a;
    lv_obj_get_coords(s_items[i].obj, &a);
    if (x >= a.x1 - HIT_PAD && x <= a.x2 + HIT_PAD && y >= a.y1 - HIT_PAD &&
        y <= a.y2 + HIT_PAD) {
      dispatch(s_items[i].id);
      return;
    }
  }
  // A miss during the descend still means "it disappeared" — the whole
  // screen is the tap target when the user is squinting at a dying glow.
  if (s_page == Page::CalDescend) dispatch(IT_GO);
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
  if (s_page == Page::CalDescend && s_cal_clock) {
    char c[8];
    fmt_clock(c, sizeof(c));
    lv_label_set_text(s_cal_clock, c);
  }
#ifdef CD_FLAVOR_DASH
  // The firmware page mirrors a live state machine (checking → downloading →
  // %). Rebuild it ~1 Hz so the status line and the install progress advance
  // without a tap; other pages are static and never self-rebuild.
  if (s_page == Page::EditFirmware) {
    static uint32_t s_fw_next_ms = 0;
    if ((int32_t)(now_ms - s_fw_next_ms) >= 0) {
      s_fw_next_ms = now_ms + 1000;
      build(Page::EditFirmware);
    }
  }
#endif
}

}  // namespace canary::ui
