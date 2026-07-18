// src/ui/settings_ui.cpp — on-glass screen settings + black-point wizard.
// See settings_ui.h and docs/hardware/display_settings.md.
//
// Design contract (settings wave): five principles — Zero Layer (two levels
// deep, ever), The Screen Is the Preview (brightness applies as you adjust),
// One Screen One Decision (each editor holds exactly one value), Defaults
// You Can Come Home To (visible reset that spares the calibration), Night Is
// a Mode (schedule + glow + look + peek travel together).
#include "flavor_config.h"
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <time.h>

#include "settings_ui.h"
#include "theme.h"
#include "glass_settings.h"
#include "display.h"

namespace canary::ui {

using namespace canary::glass;

namespace {

// ── Flavor metrics: one tree, two renderers ──────────────────────────────
#ifdef CD_FLAVOR_WATCH
constexpr int PANEL_W = 240;
constexpr int ROW_H = 27;
constexpr int ROOT_Y0 = 44;
constexpr int HIT_PAD = 8;
constexpr uint32_t IDLE_CLOSE_MS = 60000;
#else
constexpr int PANEL_W = 800;
constexpr int SHEET_W = 480, SHEET_H = 420;
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
  CalIntro,    // watch only — the black-point wizard
  CalDescend,
  CalComfort,
  CalBlink,
  CalWarn,
  CalDone,
  ResetConfirm,
};

// Tap-zone ids. Zones are the built objects themselves — hit-testing reads
// their laid-out coords, the same pattern the dash sheets already use.
enum : int {
  IT_BACK = 1,
  IT_ROW_DAY, IT_ROW_NIGHT, IT_ROW_HOURS, IT_ROW_LOOK, IT_ROW_SCREEN,
  IT_ROW_CAL, IT_ROW_RESET,
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
Item s_items[12];
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
    lv_label_set_text_fmt(l, "%s · %s", name, value);
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
  mk_back("settings · tap to leave");
  char v[24];
  int y = ROOT_Y0;
  const Settings& gs = settings();
#ifdef CD_FLAVOR_WATCH
  snprintf(v, sizeof(v), "%d%%", gs.day_pct);
  mk_row(y, "day light", v, IT_ROW_DAY);
  y += ROW_H;
  snprintf(v, sizeof(v), "lvl %d",
           night_duty_step(cal_floor_or_default(), gs.night_duty));
  mk_row(y, "night light", v, IT_ROW_NIGHT);
  y += ROW_H;
#endif
  snprintf(v, sizeof(v), "%02d-%02d", gs.night_start_hh, gs.night_end_hh);
  mk_row(y, "night hours", v, IT_ROW_HOURS);
  y += ROW_H;
  mk_row(y, "night look", gs.red_shift ? "soft red" : "plain", IT_ROW_LOOK);
  y += ROW_H;
  mk_row(y, "at night",
         gs.night_screen == NIGHT_SCREEN_OFF ? "off · peek" : "glow",
         IT_ROW_SCREEN);
  y += ROW_H;
#ifdef CD_FLAVOR_WATCH
  mk_row(y, "find the black point", nullptr, IT_ROW_CAL);
  y += ROW_H;
#endif
  mk_row(y, "reset", nullptr, IT_ROW_RESET);
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
  lv_label_set_text_fmt(cap, "of %d · shown at this glow", NIGHT_STEPS);
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
  mk_row(y, "go dark · tap peeks", off ? "on" : nullptr, IT_OPT_B, off);
#else
  mk_row(y, "stay lit, dark look", !off ? "on" : nullptr, IT_OPT_A, !off);
  y += ROW_H;
  mk_row(y, "go dark · tap wakes", off ? "on" : nullptr, IT_OPT_B, off);
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

void build_cal_intro() {
#ifdef CD_FLAVOR_WATCH
  mk_back("black point");
  lv_obj_t* body = mk_label(s_host, font_body(), col_text());
  lv_label_set_text(body,
                    "Best done at night, with\n"
                    "the room lights how you\n"
                    "sleep. The glass dims on\n"
                    "its own — tap the moment\n"
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
  lv_label_set_text(cap, "pick a 3 a.m. glow · tap here");
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
                    "That floor came out bright —\n"
                    "something looks off. Keeping\n"
                    "the factory floor · try again\n"
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
                      "never go below this —\n"
                      "redo it here anytime.");
  } else {
    // Honesty over comfort: the floor works right now but storage balked,
    // so it won't survive a restart. Say so instead of a false "saved".
    lv_label_set_text(body,
                      "Kept for tonight — but\n"
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
    case Page::CalIntro:     build_cal_intro(); break;
    case Page::CalDescend:   build_cal_descend(); break;
    case Page::CalComfort:   build_cal_comfort(); break;
    case Page::CalBlink:     build_cal_blink(); break;
    case Page::CalWarn:      build_cal_warn(); break;
    case Page::CalDone:      build_cal_done(); break;
    case Page::ResetConfirm: build_reset_confirm(); break;
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
    default:
      break;
  }
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
        case IT_ROW_CAL:    build(Page::CalIntro); return;
        case IT_ROW_RESET:  build(Page::ResetConfirm); return;
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
      if (id == IT_YES) settings_reset();
      build(Page::Root);
      return;
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
}

}  // namespace canary::ui
