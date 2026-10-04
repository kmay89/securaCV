// src/ui/onboard_ui.cpp — the first-boot welcome, in Quiet Glass.
//
// One file serves both flavors; only geometry branches on CD_FLAVOR_*. The
// scenes are deliberately spare — a setup flow earns trust by being calm,
// not busy. Every animation here is enumerated in onboard_ui.h's budget.
#include "flavor_config.h"
// Nightstand borrows the watch's small-portrait modal rendering (see
// splash.cpp for the rationale); the standing face is portrait_ui.cpp.
#if defined(CD_FLAVOR_NIGHTSTAND) && !defined(CD_FLAVOR_WATCH)
#define CD_FLAVOR_WATCH 1
#endif
#if defined(FEATURE_ONBOARDING) && FEATURE_ONBOARDING

#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "onboard_ui.h"
#include "onboard_layout.h"
#include "round_frame.h"
#include "theme.h"
#include "canary_mark.h"
#include "provision_core.h"  // shared onboarding pure helpers (common/)

namespace canary::ui {

namespace {

// Geometry per glass. The QR card is asked for here; where it and the
// Join scene's lines sit is onboard_layout.h's call (s_join below), and so
// are the halo ring (s_halo: onboardlayout::small_join on small glass, F66,
// set beside the text on landscape small glass, F157; wide_join on wide
// glass, F84, F156), the text column (s_col) and the bird's seat
// (bird_seat).
#ifdef CD_FLAVOR_WATCH
constexpr onboardlayout::CardSpec QR_SPEC = onboardlayout::kSmallGlassCard;
constexpr int BIRD_PX = onboardlayout::kSmallBirdPx;
constexpr bool WIDE = false;
#else
constexpr onboardlayout::CardSpec QR_SPEC = onboardlayout::kWideGlassCard;
constexpr int BIRD_PX = onboardlayout::kWideBirdPx;
constexpr bool WIDE = true;
#endif
constexpr lv_coord_t RING_W = onboardlayout::kRingStroke;

constexpr uint32_t FADE_MS = 260;      // scene fade-in
constexpr uint32_t BREATH_MS = 2400;   // waiting pulse period
constexpr uint32_t SWEEP_MS = 1200;    // connect sweep, per revolution
constexpr uint32_t BLOOM_MS = 500;     // success bloom
constexpr uint32_t HANDOFF_MS = 420;   // cross-fade to the normal UI

lv_obj_t* s_prev_scr = nullptr;        // the normal UI screen to return to
lv_obj_t* s_scr = nullptr;             // onboarding screen (auto-deleted)
lv_obj_t* s_ring = nullptr;            // halo: breathes waiting, sweeps joining
lv_obj_t* s_content = nullptr;         // faded as one unit per stage
lv_obj_t* s_title = nullptr;
lv_obj_t* s_body = nullptr;
lv_obj_t* s_qr_card = nullptr;
lv_obj_t* s_qr = nullptr;
lv_obj_t* s_creds = nullptr;           // SSID / password fallback text
lv_obj_t* s_hint = nullptr;
lv_obj_t* s_note = nullptr;            // small glass: the note row (join_lines)
lv_obj_t* s_bird = nullptr;            // the brand mark (its seat moves, F50)
onboardlayout::Stack s_join = {};      // the Join scene's rows (see join_rows)
// The scenes' title and body faces (scene_text, F65; every glass since
// F84): the title in the Character's body face, the body in its caption
// face, each stepping down to the default Character's face of the role.
// On small glass the Join scene's text rows set the caption pair too (see
// refresh_bottom).
const lv_font_t* s_row_font = nullptr;
const lv_font_t* s_floor_font = nullptr;
const lv_font_t* s_title_font = nullptr;
const lv_font_t* s_title_floor = nullptr;
onboardlayout::Glass s_glass = {};     // this panel (this_glass)
onboardlayout::Ring s_halo = {};       // the halo's size and seat (F66, F84)
onboardlayout::Column s_col = {};      // the text column (F157)
#ifdef CD_FLAVOR_WATCH
// The Join scene's text rows' widths: what rf_fit_top sized each label to.
int s_creds_w = 0;
int s_low_w = 0;
int s_note_w = 0;
onboardlayout::Rows s_rows = {};       // the Join scene's line heights
#endif

ObStage s_stage = ObStage::Hello;
bool s_qr_ok = false;                  // the join QR actually rendered
char s_ap_ssid[33] = {0};
char s_ap_pass[17] = {0};
char s_hint_text[96] = {0};            // the live coach line (see refresh_bottom)
char s_hint_narrow[96] = {0};          // its shorter form, for a narrow row

// A line's width in `font`, measured the way the label lays it out (see
// onboardlayout::text_width) — the same call in LVGL 8 and 9.
int text_w(const char* text, const lv_font_t* font) {
  return onboardlayout::text_width(text, [font](uint32_t a, uint32_t b) {
    return (int)lv_font_get_glyph_width(font, a, b);
  });
}

// A Join-stack row at y_top: as wide as the glass fits it, centered,
// LONG_DOT (it never cuts: the rows are fitted). rf_fit_top's row, except
// where the text stands in a column beside the halo (landscape small glass,
// F157): that column's width, at its offset.
void fit_row(lv_obj_t* label, int y_top) {
  if (!s_col.side) {
    rf_fit_top(label, y_top);
    return;
  }
  const lv_font_t* f = lv_obj_get_style_text_font(label, LV_PART_MAIN);
  lv_obj_set_size(label, s_col.w, (lv_coord_t)lv_font_get_line_height(f));
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(label, LV_ALIGN_TOP_MID, s_col.x, y_top);
}

// The width fit_row gives a row at y_top, h tall.
int row_width(int y_top, int h) {
  return s_col.side ? s_col.w : rf_row_width(y_top, h);
}

#ifndef CD_FLAVOR_WATCH
// A wide Join row: its fitted text in the face fit_line chose, the label as
// tall as that face's line (the row under the credentials carries the key
// in the credentials' face when the line splits, F156).
void set_wide_row(lv_obj_t* label, const onboardlayout::Line& line,
                  const lv_font_t* own_f, const lv_font_t* floor_f) {
  const lv_font_t* f = line.floor ? floor_f : own_f;
  lv_obj_set_style_text_font(label, f, 0);
  lv_obj_set_height(label, (lv_coord_t)lv_font_get_line_height(f));
  lv_label_set_text(label, line.text);
}
#endif

#ifdef CD_FLAVOR_WATCH

void set_row(lv_obj_t* label, const onboardlayout::Line& line) {
  lv_obj_set_style_text_font(label, line.floor ? s_floor_font : s_row_font, 0);
  lv_label_set_text(label, line.text);
}

// The small-glass Join title on the stack's title row ("Scan me" while the
// card is up), fitted like the rows under it: whole in the title's face,
// else in the default Character's (F65: "On your phone" is 154 px under
// Heirloom, on the round watch's 142 px title row).
void set_join_title() {
  const lv_font_t* own_f = s_title_font;
  const lv_font_t* floor_f = s_title_floor;
  auto measure = [own_f, floor_f](const char* t, bool fl) {
    return text_w(t, fl ? floor_f : own_f);
  };
  const int w =
      row_width(s_join.title_top, (int)lv_font_get_line_height(own_f));
  const char* forms[1] = {onboardlayout::join_title(s_qr_ok)};
  onboardlayout::Line line;
  onboardlayout::fit_line(line, forms, 1, w, measure);
  lv_obj_set_style_text_font(s_title, line.floor ? floor_f : own_f, 0);
  fit_row(s_title, s_join.title_top);
  lv_label_set_text(s_title, line.text);
}
#else
// The wide Join title, in the title face on the stack's title row (above
// the halo), fitted to the panel's row like every line here: the whole
// instruction, else its shorter form (the 480x800 portrait glass, F156).
void set_join_title() {
  const lv_font_t* own_f = font_title();
  const lv_font_t* floor_f = character_def(Character::QuietGlass).type.title;
  auto measure = [own_f, floor_f](const char* t, bool fl) {
    return text_w(t, fl ? floor_f : own_f);
  };
  const int w = s_col.w;
  const onboardlayout::Forms title = onboardlayout::wide_join_title(s_qr_ok);
  const char* forms[2] = {title.full, title.narrow};
  onboardlayout::Line line;
  onboardlayout::fit_line(line, forms, 2, w, measure);
  const lv_font_t* f = line.floor ? floor_f : own_f;
  lv_obj_set_style_text_font(s_title, f, 0);
  lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
  lv_obj_set_size(s_title, w, (lv_coord_t)lv_font_get_line_height(f));
  lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(s_title, line.text);
  lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, s_join.title_top);
}
#endif

// A scene's centered line (F65; wide glass too since F84): its forms
// through fit_line on the width at its own latitude
// (onboardlayout::scene_line: the disc's chord on round glass, the panel
// less its pads and inside the halo on rectangular glass, the text column
// beside the halo on landscape small glass, F157), in the role's face or
// the default Character's; a network name through name_line. Never
// LONG_DOT's cut: the label is as wide as the line's row.
void set_center_line(lv_obj_t* label, const char* full, const char* narrow,
                     int off, const lv_font_t* own_f,
                     const lv_font_t* floor_f, bool name) {
  auto measure = [own_f, floor_f](const char* t, bool fl) {
    return text_w(t, fl ? floor_f : own_f);
  };
  const int h = (int)lv_font_get_line_height(own_f);
  const onboardlayout::LineSeat at =
      onboardlayout::scene_line(s_glass, s_halo, s_col, off, h);
  const int w = at.w;
  onboardlayout::Line line;
  if (full == nullptr || full[0] == '\0') {
    onboardlayout::set_line(line, "", false, true);
  } else if (name) {
    onboardlayout::name_line(line, full, w, measure);
  } else {
    const char* forms[2] = {full, narrow};
    onboardlayout::fit_line(line, forms, 2, w, measure);
  }
  lv_obj_set_style_text_font(label, line.floor ? floor_f : own_f, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_set_size(label, w, (lv_coord_t)lv_font_get_line_height(
                                line.floor ? floor_f : own_f));
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(label, line.text);
  lv_obj_align(label, LV_ALIGN_CENTER, at.x, at.y);
}

// The Join scene's text, owned in one place. On the round glass the old
// single "ssid  •  pass" line at -34 outran its chord (152 px against
// ~165 px of text — the physical rim ate the password's tail), so the Join
// scene splits them: the network name rides the upper, wider band and the
// password the lower one (onboard_layout.h keeps that band's chord at
// kRoundLowRowW). Rectangular small glass splits the same way whenever the
// joined line is wider than its row — on the 172 px nightstand it always
// is (F45) — and nothing on any row is ever cut: onboardlayout::join_lines
// picks each row's text and face. The name and the key never give up
// their rows while the scene is up: when the QR does not scan (or never
// rendered) they are the only way in, and the one hint that stands here —
// the stuck phone's "forget it" — is when the phone needs the key again.
// On split glass that hint takes the note row instead (under the key on
// rectangular glass; the title's band on round glass, where the title
// yields while it stands).
//
// Every other scene leaves the credentials rows empty, so its coach line —
// PhoneJoined's "no page?", Fail's fix — has both of them (F50):
// onboardlayout::hint_lines keeps the hint whole on the hint row where it
// fits, else over both rows, else its narrow form, and never cuts it.
void refresh_bottom() {
  if (!s_creds || !s_hint) return;
#ifdef CD_FLAVOR_WATCH
  const lv_font_t* own_f = s_row_font;
  const lv_font_t* floor_f = s_floor_font;
  auto measure = [own_f, floor_f](const char* t, bool fl) {
    return text_w(t, fl ? floor_f : own_f);
  };
  if (s_stage == ObStage::Join) {
    const onboardlayout::JoinLines j = onboardlayout::join_lines(
        RF_GLASS_ROUND != 0, s_creds_w, s_low_w, s_note_w, s_ap_ssid,
        s_ap_pass, s_hint_text, s_hint_narrow, measure);
    set_row(s_creds, j.creds);
    set_row(s_hint, j.low);
    set_row(s_note, j.note);
    // The name and the key are load-bearing — muted; a hint is faint.
    lv_obj_set_style_text_color(s_creds, col_muted(), 0);
    lv_obj_set_style_text_color(s_hint, j.split ? col_muted() : col_faint(),
                                0);
#if RF_GLASS_ROUND
    if (j.note.text[0]) {
      lv_label_set_text(s_title, "");
    } else {
      set_join_title();
    }
#endif
    return;
  }
  const onboardlayout::HintLines h = onboardlayout::hint_lines(
      s_creds_w, s_low_w, s_hint_text, s_hint_narrow, measure);
  set_row(s_creds, h.upper);
  set_row(s_hint, h.lower);
  lv_obj_set_style_text_color(s_creds, col_faint(), 0);
  lv_obj_set_style_text_color(s_hint, col_faint(), 0);
  lv_label_set_text(s_note, "");
#else
  // Wide glass (F156): the worded credentials line whole where the panel's
  // row holds it, else the name over "password  <key>", both in the
  // credentials' face, and a standing hint on the note row
  // (onboardlayout::wide_join_lines); the coach line of a scene without
  // credentials whole on the hint row, else its shorter form. Every row is
  // fitted to the panel's row, so the 480x800 portrait glass cuts nothing.
  const lv_font_t* c_own = font_label();
  const lv_font_t* c_floor = character_def(Character::QuietGlass).type.label;
  const lv_font_t* h_own = s_row_font;
  const lv_font_t* h_floor = s_floor_font;
  auto creds_m = [c_own, c_floor](const char* t, bool fl) {
    return text_w(t, fl ? c_floor : c_own);
  };
  auto hint_m = [h_own, h_floor](const char* t, bool fl) {
    return text_w(t, fl ? h_floor : h_own);
  };
  const int w =
      row_width(s_join.creds_top, (int)lv_font_get_line_height(c_own));
  if (s_stage == ObStage::Join) {
    const onboardlayout::JoinLines j = onboardlayout::wide_join_lines(
        s_qr_ok, w, s_ap_ssid, s_ap_pass, s_hint_text, s_hint_narrow,
        creds_m, hint_m);
    set_wide_row(s_creds, j.creds, c_own, c_floor);
    set_wide_row(s_hint, j.low, j.split ? c_own : h_own,
                 j.split ? c_floor : h_floor);
    set_wide_row(s_note, j.note, h_own, h_floor);
    // The name and the key are load-bearing (muted); a hint is faint.
    lv_obj_set_style_text_color(s_hint, j.split ? col_muted() : col_faint(),
                                0);
    return;
  }
  const char* forms[2] = {s_hint_text, s_hint_narrow};
  onboardlayout::Line coach;
  onboardlayout::fit_line(coach, forms, 2, w, hint_m);
  set_wide_row(s_hint, coach, h_own, h_floor);
  lv_obj_set_style_text_color(s_hint, col_faint(), 0);
  lv_label_set_text(s_note, "");
#endif
}

// Scene fade, applied to the TEXT of the labels rather than as one
// style `opa` on the full-screen content container. Under LVGL 9 a group
// `opa` composites the whole 800x480 subtree through intermediate layer
// buffers — ~22 KB of contiguous LV_MEM pool per frame, on top of two live
// screens and the QR buffer. That allocation cliff halted/panicked the 7"
// glass right as the wizard's scenes changed (silent with LV_USE_LOG 0),
// while per-part text_opa draws directly on both majors and costs nothing.
// The bird, ring and QR card pop instead of fading; the ring keeps its own
// arc_opa choreography and the card wants to be scannable immediately.
void fade_cb(void* /*var*/, int32_t v) {
  if (!s_title) return;  // scene torn down mid-fade (finish handoff)
  lv_obj_set_style_text_opa(s_title, (lv_opa_t)v, 0);
  lv_obj_set_style_text_opa(s_body, (lv_opa_t)v, 0);
  lv_obj_set_style_text_opa(s_creds, (lv_opa_t)v, 0);
  lv_obj_set_style_text_opa(s_hint, (lv_opa_t)v, 0);
  if (s_note) lv_obj_set_style_text_opa(s_note, (lv_opa_t)v, 0);
}
void ring_opa_cb(void* var, int32_t v) {
  lv_obj_set_style_arc_opa((lv_obj_t*)var, (lv_opa_t)v, LV_PART_INDICATOR);
}
void ring_spin_cb(void* var, int32_t v) {
  lv_arc_set_rotation((lv_obj_t*)var, (uint16_t)v);
}

void stop_ring_anims() {
  lv_anim_del(s_ring, ring_opa_cb);
  lv_anim_del(s_ring, ring_spin_cb);
}

// The one waiting motion: the halo breathing, 2.4 s, subtle.
void ring_breathe() {
  stop_ring_anims();
  lv_arc_set_bg_angles(s_ring, 0, 360);
  lv_arc_set_angles(s_ring, 0, 360);
  lv_obj_set_style_arc_color(s_ring, col_edge(), LV_PART_INDICATOR);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_ring);
  lv_anim_set_exec_cb(&a, ring_opa_cb);
  lv_anim_set_values(&a, LV_OPA_30, LV_OPA_70);
  lv_anim_set_time(&a, BREATH_MS / 2);
  lv_anim_set_playback_time(&a, BREATH_MS / 2);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

// The joining motion: a short arc chasing its tail, 1.2 s/rev.
void ring_sweep() {
  stop_ring_anims();
  lv_obj_set_style_arc_opa(s_ring, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_ring, col_text(), LV_PART_INDICATOR);
  lv_arc_set_bg_angles(s_ring, 0, 0);
  lv_arc_set_angles(s_ring, 0, 70);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_ring);
  lv_anim_set_exec_cb(&a, ring_spin_cb);
  lv_anim_set_values(&a, 0, 360);
  lv_anim_set_time(&a, SWEEP_MS);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);
}

// The earned motion: bloom to the ok green, once.
void ring_bloom() {
  stop_ring_anims();
  lv_arc_set_bg_angles(s_ring, 0, 360);
  lv_arc_set_angles(s_ring, 0, 360);
  lv_obj_set_style_arc_color(s_ring, col_ok(), LV_PART_INDICATOR);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_ring);
  lv_anim_set_exec_cb(&a, ring_opa_cb);
  lv_anim_set_values(&a, LV_OPA_20, LV_OPA_COVER);
  lv_anim_set_time(&a, BLOOM_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

void ring_still(lv_color_t c, lv_opa_t opa) {
  stop_ring_anims();
  lv_arc_set_bg_angles(s_ring, 0, 360);
  lv_arc_set_angles(s_ring, 0, 360);
  lv_obj_set_style_arc_color(s_ring, c, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(s_ring, opa, LV_PART_INDICATOR);
}

// Fade the scene's text in as one unit (the scene transition). The anim's
// var is only a dedup handle — fade_cb touches the labels directly.
void content_enter() {
  lv_anim_del(s_content, fade_cb);
  fade_cb(nullptr, LV_OPA_TRANSP);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_content);
  lv_anim_set_exec_cb(&a, fade_cb);
  lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_time(&a, FADE_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

int line_h(lv_obj_t* label) {
  return (int)lv_font_get_line_height(
      lv_obj_get_style_text_font(label, LV_PART_MAIN));
}

// Stack the Join scene from THIS panel and the fonts its labels actually
// carry (the active Character's ladder): title, QR card, credentials, hint.
// The old per-glass offsets placed the card from the center and the captions
// from the bottom edge independently, and on the dash and the round watch
// the credentials line landed inside the card (F43).
// This panel, as onboard_layout.h's functions take it.
onboardlayout::Glass this_glass() {
  onboardlayout::Glass g;
  g.w = (int)lv_disp_get_hor_res(NULL);
  g.h = (int)lv_disp_get_ver_res(NULL);
  g.round = RF_GLASS_ROUND != 0;
  return g;
}

onboardlayout::Stack join_rows() {
  const onboardlayout::Glass g = s_glass;
  onboardlayout::Rows r;
  r.title_h = line_h(s_title);
  r.card = QR_SPEC;
  r.creds_h = line_h(s_creds);
  r.hint_h = line_h(s_hint);
#ifdef CD_FLAVOR_WATCH
  s_rows = r;
  // The halo: the rim's ring on round glass; on rectangular glass the band
  // between the stack's title and credentials rows, with the card's corners
  // inside it (small_join, F66); on landscape small glass beside the text
  // column (F157).
  const onboardlayout::SmallJoin j = onboardlayout::small_join(g, r);
#else
  // The dash line's 300 px halo, centered (wide_ring, F84), and its rows
  // (wide_join: the note row under a credentials-tall row, F156).
  const onboardlayout::SmallJoin j = onboardlayout::wide_join(g, r);
#endif
  s_halo = j.halo;
  s_col = j.col;
  return j.stack;
}

// A scene's title and body (F65). The words are onboard_layout.h's
// (scene_copy), except where the stage's detail carries them: Fail's reason
// (with its narrow form) and Connecting's network name.
void scene_text(ObStage st, const char* detail, const char* narrow) {
  const onboardlayout::SceneCopy c = onboardlayout::scene_copy(st);
  const char* title = c.title.full;
  const char* title_narrow = c.title.narrow;
  if (st == ObStage::Fail && detail != nullptr) {
    title = detail;
    title_narrow = narrow;
  }
  const bool name = c.body.full == nullptr;
  const char* body = name ? (detail ? detail : "your network") : c.body.full;
  if (st == ObStage::Join) {
    set_join_title();
    lv_label_set_text(s_body, "");
    return;
  }
  // Every glass, the wide one too (F84): fitted inside the halo's chord.
  set_center_line(s_title, title, title_narrow, c.title_off, s_title_font,
                  s_title_floor, false);
  set_center_line(s_body, body, c.body.narrow, c.body_off, s_row_font,
                  s_floor_font, name);
}

void show_qr(bool show) {
  // A card whose QR never rendered stays hidden — the Join scene's text
  // path (AP name + password) carries setup on its own, so a generator
  // failure degrades to instructions instead of a blank white square.
  if (show && s_qr_ok) lv_obj_clear_flag(s_qr_card, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(s_qr_card, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

void onboard_ui_create(const char* ap_ssid, const char* ap_pass) {
  snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", ap_ssid ? ap_ssid : "");
  snprintf(s_ap_pass, sizeof(s_ap_pass), "%s", ap_pass ? ap_pass : "");

  s_prev_scr = lv_scr_act();
  s_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

  s_glass = this_glass();

  // Halo ring — the one persistent element across every scene, so the eye
  // has continuity while text changes. Its size and seat wait for the Join
  // stack (below): on small glass it sits clear of the stack's rows.
  s_ring = lv_arc_create(s_scr);
  lv_arc_set_rotation(s_ring, 270);
  lv_obj_set_style_arc_width(s_ring, RING_W, LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_ring, RING_W, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(s_ring, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(s_ring, true, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);

  s_content = lv_obj_create(s_scr);
  lv_obj_set_size(s_content, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_content, 0, 0);
  lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

  auto mk = [&](const lv_font_t* f, lv_color_t c) {
    lv_obj_t* l = lv_label_create(s_content);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    return l;
  };

  // The brand canary welcomes — the first thing anyone meets on first
  // boot. Hidden while the QR needs the room, hops once on success. Its
  // seat is onboard_layout.h's (bird_seat), set per scene below.
  s_bird = canary_mark_create(s_content, BIRD_PX);

#ifdef CD_FLAVOR_WATCH
  s_title = mk(font_body(), col_text());
  rf_fit_top(s_title, 28);
  s_qr_card = lv_obj_create(s_content);
  s_body = mk(font_caption(), col_muted());
  rf_fit_center(s_body, 0);
  s_creds = mk(font_caption(), col_muted());
  s_hint = mk(font_caption(), col_faint());
  s_note = mk(font_caption(), col_faint());
  // The text lines ride the stack's rows (split glass: the network name,
  // then the key, and a standing hint on the note row — see refresh_bottom).
  s_join = join_rows();
  fit_row(s_creds, s_join.creds_top);
  fit_row(s_hint, s_join.hint_top);
  fit_row(s_note, s_join.note_top);
  s_creds_w = row_width(s_join.creds_top, line_h(s_creds));
  s_low_w = row_width(s_join.hint_top, line_h(s_hint));
  s_note_w = row_width(s_join.note_top, line_h(s_note));
#else
  // The Join title rides the stack's title row in the title face (the
  // stack is sized from it); every other scene's title and body are fitted
  // inside the halo (scene_text, F84).
  s_title = mk(font_title(), col_text());
  s_qr_card = lv_obj_create(s_content);
  s_body = mk(font_caption(), col_muted());
  s_creds = mk(font_label(), col_muted());
  s_hint = mk(font_caption(), col_faint());
  s_note = mk(font_caption(), col_faint());
  s_join = join_rows();
  // The rows are fitted to the panel's row (refresh_bottom), so a split
  // or a shorter form never runs off a narrow (portrait) glass's edges.
  fit_row(s_creds, s_join.creds_top);
  fit_row(s_hint, s_join.hint_top);
  fit_row(s_note, s_join.note_top);
#endif
  // The scenes' title and body faces, and their floors (F65, F84).
  s_row_font = font_caption();
  s_floor_font = character_def(Character::QuietGlass).type.caption;
  s_title_font = font_body();
  s_title_floor = character_def(Character::QuietGlass).type.body;
  // The halo, as join_rows() placed it (small_join, wide_join), and the
  // card concentric with it (beside the text on landscape small glass).
  lv_obj_set_size(s_ring, s_halo.d, s_halo.d);
  lv_obj_align(s_ring, LV_ALIGN_TOP_MID, s_halo.x, s_halo.top);
  lv_obj_set_size(s_qr_card, s_join.card, s_join.card);
  lv_obj_align(s_qr_card, LV_ALIGN_TOP_MID, s_halo.x, s_join.card_top);

  // QR on a white card — scanners want dark-on-light (proof-sheet lesson).
  lv_obj_set_style_bg_color(s_qr_card, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(s_qr_card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_qr_card, onboardlayout::kCardRadius, 0);
  lv_obj_set_style_border_width(s_qr_card, 0, 0);
  lv_obj_set_style_pad_all(s_qr_card, QR_SPEC.pad, 0);
  lv_obj_clear_flag(s_qr_card, LV_OBJ_FLAG_SCROLLABLE);
  // Mint and render the join QR — and PROVE it rendered before ever showing
  // the card. Every step can fail (the v9 widget leaves a buffer-less canvas
  // behind when its draw-buffer allocation loses, and update reports its own
  // verdict); a failure here must degrade to the text instructions, never
  // crash the wizard or present an empty card.
  s_qr = mk_qrcode(s_qr_card, s_join.qr);
  s_qr_ok = false;
  if (s_qr != nullptr) {
    lv_obj_center(s_qr);
    char payload[224];
    const size_t n = canary::net::wifi_qr_payload(s_ap_ssid, s_ap_pass,
                                                  payload, sizeof(payload));
#if LVGL_VERSION_MAJOR >= 9
    s_qr_ok = n > 0 && lv_canvas_get_draw_buf(s_qr) != NULL &&
              lv_qrcode_update(s_qr, payload, (uint32_t)n) == LV_RESULT_OK;
#else
    s_qr_ok = n > 0 && lv_qrcode_update(s_qr, payload, (uint32_t)n) == LV_RES_OK;
#endif
  }
  show_qr(false);

  lv_scr_load(s_scr);
  onboard_ui_stage(ObStage::Hello, nullptr);
}

void onboard_ui_stage(ObStage st, const char* detail, const char* narrow) {
  if (!s_scr) return;
  s_stage = st;
  s_hint_text[0] = '\0';  // a scene change retires the coach line
  s_hint_narrow[0] = '\0';

  // The bird's seat, BEFORE any mood below runs: the mark records its base
  // at the first on-stage mood after a placement (canary_mark_rebase), so
  // the seat must be set first or the pose snaps back to the old one on
  // the next animation frame (review catch on F50). The seat is
  // onboard_layout.h's (bird_seat). On small glass's Join scene the bird is
  // only visible while the QR is away (mood Hidden otherwise), and its old
  // center-relative perch put its top 12 px inside the round watch's title
  // band — so there it takes the hidden card's empty seat, which the stack
  // keeps clear of the title and the credentials on every glass by
  // construction (join_bird_top). Every other scene centers its text, well
  // under the usual perch, inside the halo (scene_bird_top). Wide glass
  // keeps one seat, inside its halo over the title. The mark draws the bird
  // at exactly this seat (its base is the align's offset, F64).
  if (s_bird) {
    const onboardlayout::Seat seat =
        onboardlayout::bird_seat(s_glass, WIDE, s_join, s_halo, st);
    lv_obj_align(s_bird, LV_ALIGN_TOP_MID,
                 seat.x - (s_glass.w / 2 - seat.d / 2), seat.y);
    canary_mark_rebase();
  }

  switch (st) {
    case ObStage::Hello:
      show_qr(false);
      canary_mark_mood(CanaryMood::Idle);
      lv_label_set_text(s_creds, "");
      ring_still(col_edge(), LV_OPA_40);
      break;

    case ObStage::Join:
      show_qr(true);
      // No QR (generator or buffer failed): the bird stays for warmth and
      // the copy flips to plain join instructions — same destination, no
      // dead end, nothing on the glass admits a fault.
      canary_mark_mood(s_qr_ok ? CanaryMood::Hidden  // the QR owns the room
                               : CanaryMood::Idle);
      // The credentials rows are refresh_bottom's (below): joined or split
      // by what fits this glass (join_lines, F45; wide_join_lines, F156).
      ring_breathe();
      break;

    case ObStage::PhoneJoined:
      show_qr(false);
      canary_mark_mood(CanaryMood::Idle);
      lv_label_set_text(s_creds, "");
      ring_breathe();
      break;

    case ObStage::Connecting:
      show_qr(false);
      canary_mark_mood(CanaryMood::Idle);
      lv_label_set_text(s_creds, "");
      ring_sweep();
      break;

    case ObStage::Fail:
      show_qr(false);
      canary_mark_mood(CanaryMood::Hidden);  // no mascot on a miss
      lv_obj_set_style_text_color(s_title, col_warn(), 0);
      lv_label_set_text(s_creds, "");
      ring_still(col_warn(), LV_OPA_50);
      break;

    case ObStage::Success:
      show_qr(false);
      canary_mark_mood(CanaryMood::Happy);   // the hop is earned
      lv_obj_set_style_text_color(s_title, col_text(), 0);
      lv_label_set_text(s_creds, "");
      ring_bloom();
      break;
  }
  // The scene's title and body: onboard_layout.h's words, fitted (F65).
  scene_text(st, detail, narrow);
  // Fail tints the title; every other stage restores it.
  if (st != ObStage::Fail && st != ObStage::Success) {
    lv_obj_set_style_text_color(s_title, col_text(), 0);
  }
  refresh_bottom();
  content_enter();
}

bool onboard_ui_bird_seat(int* x, int* y, int* side) {
  if (!s_scr || !s_bird) return false;
  const onboardlayout::Seat seat =
      onboardlayout::bird_seat(s_glass, WIDE, s_join, s_halo, s_stage);
  if (x) *x = seat.x;
  if (y) *y = seat.y;
  if (side) *side = seat.d;
  return true;
}

bool onboard_ui_join_layout(OnboardJoinBoxes* out) {
  if (!s_scr || !s_ring || !s_qr_card || out == nullptr) return false;
  // lv_obj_align(TOP_MID, s_halo.x, top) on the panel, as onboard_ui_create()
  // seats both (the content layer the card rides is the whole panel).
  out->ring_x = s_glass.w / 2 - s_halo.d / 2 + s_halo.x;
  out->ring_y = s_halo.top;
  out->ring_d = s_halo.d;
  out->card_x = s_glass.w / 2 - s_join.card / 2 + s_halo.x;
  out->card_y = s_join.card_top;
  out->card_side = s_join.card;
  return true;
}

void onboard_ui_hint(const char* line, const char* narrow) {
  if (!s_hint) return;
  snprintf(s_hint_text, sizeof(s_hint_text), "%s", line ? line : "");
  snprintf(s_hint_narrow, sizeof(s_hint_narrow), "%s", narrow ? narrow : "");
  refresh_bottom();
}

void onboard_ui_tick(uint32_t /*now_ms*/) {
  // Animations are LVGL-driven (lv_timer_handler advances them); the tick
  // exists so provision.cpp has a seam for future per-frame needs without
  // an API change.
}

void onboard_ui_finish() {
  if (!s_scr || !s_prev_scr) return;
  // Go home and free every onboarding object (auto_del) — the normal UI
  // was created before provisioning and is ready underneath.
#if LVGL_VERSION_MAJOR >= 9
  // No cross-fade on v9: a screen-load FADE composites both 800x480
  // screens through the same per-frame layer chunks the scene fades gave
  // up (see fade_cb) — the pool is at its fullest right here, with every
  // onboarding object still alive. Cut to the finished face instead.
  lv_scr_load_anim(s_prev_scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
#else
  lv_scr_load_anim(s_prev_scr, LV_SCR_LOAD_ANIM_FADE_ON, HANDOFF_MS, 0, true);
#endif
  s_scr = nullptr;
  s_ring = s_content = s_title = s_body = nullptr;
  s_qr_card = s_qr = s_creds = s_hint = s_note = s_bird = nullptr;
  s_qr_ok = false;
  s_hint_text[0] = '\0';
  s_hint_narrow[0] = '\0';
}

}  // namespace canary::ui

#endif  // FEATURE_ONBOARDING
