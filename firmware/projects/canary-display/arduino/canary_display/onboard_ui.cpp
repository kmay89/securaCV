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
// Join scene's lines sit is onboard_layout.h's call (s_join below).
#ifdef CD_FLAVOR_WATCH
constexpr onboardlayout::CardSpec QR_SPEC = onboardlayout::kSmallGlassCard;
constexpr lv_coord_t RING_D = 236, RING_W = 3;
#else
constexpr onboardlayout::CardSpec QR_SPEC = onboardlayout::kWideGlassCard;
constexpr lv_coord_t RING_D = 300, RING_W = 3;
#endif

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
onboardlayout::Stack s_join = {};      // the Join scene's rows (see join_rows)
#ifdef CD_FLAVOR_WATCH
// The two low rows' faces and widths (see refresh_bottom): the Character's
// caption, and the default Character's — the floor a row that would be cut
// steps down to. Widths are what rf_fit_top sized each label to.
const lv_font_t* s_row_font = nullptr;
const lv_font_t* s_floor_font = nullptr;
int s_creds_w = 0;
int s_low_w = 0;
#endif

ObStage s_stage = ObStage::Hello;
bool s_qr_ok = false;                  // the join QR actually rendered
char s_ap_ssid[33] = {0};
char s_ap_pass[17] = {0};
char s_hint_text[96] = {0};            // the live coach line (see refresh_bottom)
char s_hint_narrow[96] = {0};          // its shorter form, for a narrow row

#ifdef CD_FLAVOR_WATCH
// A line's width in `font`, measured the way the label lays it out (see
// onboardlayout::text_width) — the same call in LVGL 8 and 9.
int text_w(const char* text, const lv_font_t* font) {
  return onboardlayout::text_width(text, [font](uint32_t a, uint32_t b) {
    return (int)lv_font_get_glyph_width(font, a, b);
  });
}

void set_row(lv_obj_t* label, const onboardlayout::Line& line) {
  lv_obj_set_style_text_font(label, line.floor ? s_floor_font : s_row_font, 0);
  lv_label_set_text(label, line.text);
}
#endif

// The two low lines, owned in one place. On the round glass the old single
// "ssid  •  pass" line at -34 outran its chord (152 px against ~165 px of
// text — the physical rim ate the password's tail), so the Join scene splits
// them: the network name rides the upper, wider band and the password the
// lower one (onboard_layout.h keeps that band's chord at kRoundLowRowW).
// Rectangular small glass splits the same way whenever the joined line is
// wider than its row — on the 172 px nightstand it always is (F45) — and
// nothing on either row is ever cut: onboardlayout::join_lines picks each
// row's text and face. A live hint OUTRANKS the password line while it
// stands — every hint fires either when the phone has already joined
// (password moot) or when the fix is on the phone itself, and the QR keeps
// carrying both credentials the whole time.
void refresh_bottom() {
  if (!s_creds || !s_hint) return;
#ifdef CD_FLAVOR_WATCH
  if (s_stage == ObStage::Join) {
    const lv_font_t* own_f = s_row_font;
    const lv_font_t* floor_f = s_floor_font;
    const onboardlayout::JoinLines j = onboardlayout::join_lines(
        RF_GLASS_ROUND != 0, s_creds_w, s_low_w, s_ap_ssid, s_ap_pass,
        s_hint_text, s_hint_narrow, [own_f, floor_f](const char* t, bool fl) {
          return text_w(t, fl ? floor_f : own_f);
        });
    set_row(s_creds, j.creds);
    set_row(s_hint, j.low);
    // The password is load-bearing — muted, not faint.
    lv_obj_set_style_text_color(s_hint,
                                s_hint_text[0] ? col_faint() : col_muted(), 0);
    return;
  }
  lv_obj_set_style_text_font(s_creds, s_row_font, 0);
  lv_obj_set_style_text_font(s_hint, s_row_font, 0);
  lv_obj_set_style_text_color(s_hint, col_faint(), 0);
#endif
  lv_label_set_text(s_hint, s_hint_text);
}

// Scene fade, applied to the TEXT of the four labels rather than as one
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
onboardlayout::Stack join_rows() {
  onboardlayout::Glass g;
  g.w = (int)lv_disp_get_hor_res(NULL);
  g.h = (int)lv_disp_get_ver_res(NULL);
  g.round = RF_GLASS_ROUND != 0;
  onboardlayout::Rows r;
  r.title_h = line_h(s_title);
  r.card = QR_SPEC;
  r.creds_h = line_h(s_creds);
  r.hint_h = line_h(s_hint);
  return onboardlayout::join_stack(g, r);
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

  // Halo ring — the one persistent element across every scene, so the eye
  // has continuity while text changes.
  s_ring = lv_arc_create(s_scr);
  lv_obj_set_size(s_ring, RING_D, RING_D);
  lv_obj_center(s_ring);
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
  // boot. Hidden while the QR needs the room, hops once on success.
#ifdef CD_FLAVOR_WATCH
  lv_obj_t* bird = canary_mark_create(s_content, 40);
  lv_obj_align(bird, LV_ALIGN_CENTER, 0, -64);
#else
  lv_obj_t* bird = canary_mark_create(s_content, 64);
  lv_obj_align(bird, LV_ALIGN_CENTER, 0, -104);
#endif
  (void)bird;

#ifdef CD_FLAVOR_WATCH
  s_title = mk(font_body(), col_text());
  rf_fit_top(s_title, 28);
  s_qr_card = lv_obj_create(s_content);
  s_body = mk(font_caption(), col_muted());
  rf_fit_center(s_body, 0);
  s_creds = mk(font_caption(), col_muted());
  s_hint = mk(font_caption(), col_faint());
  // The two low lines ride the stack's rows (split glass: the network name,
  // then the password or a live hint — see refresh_bottom).
  s_join = join_rows();
  rf_fit_top(s_creds, s_join.creds_top);
  rf_fit_top(s_hint, s_join.hint_top);
  s_row_font = font_caption();
  s_floor_font = character_def(Character::QuietGlass).type.caption;
  s_creds_w = rf_row_width(s_join.creds_top, line_h(s_creds));
  s_low_w = rf_row_width(s_join.hint_top, line_h(s_hint));
#else
  s_title = mk(font_title(), col_text());
  lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 96);
  s_qr_card = lv_obj_create(s_content);
  s_body = mk(font_body(), col_muted());
  lv_obj_align(s_body, LV_ALIGN_CENTER, 0, 10);
  s_creds = mk(font_label(), col_muted());
  s_hint = mk(font_caption(), col_faint());
  s_join = join_rows();
  lv_obj_align(s_creds, LV_ALIGN_TOP_MID, 0, s_join.creds_top);
  lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, s_join.hint_top);
#endif
  lv_obj_set_size(s_qr_card, s_join.card, s_join.card);
  lv_obj_align(s_qr_card, LV_ALIGN_TOP_MID, 0, s_join.card_top);

  // QR on a white card — scanners want dark-on-light (proof-sheet lesson).
  lv_obj_set_style_bg_color(s_qr_card, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(s_qr_card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_qr_card, 10, 0);
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

void onboard_ui_stage(ObStage st, const char* detail) {
  if (!s_scr) return;
  s_stage = st;
  s_hint_text[0] = '\0';  // a scene change retires the coach line
  s_hint_narrow[0] = '\0';

  switch (st) {
    case ObStage::Hello:
      show_qr(false);
      canary_mark_mood(CanaryMood::Idle);
      lv_label_set_text(s_title, "Hello.");
      lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -12);
      lv_label_set_text(s_body, "Let's get you connected.");
      lv_obj_align(s_body, LV_ALIGN_CENTER, 0, 16);
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
#ifdef CD_FLAVOR_WATCH
      rf_fit_top(s_title, s_join.title_top);
      lv_label_set_text(s_title, s_qr_ok ? "Scan me" : "On your phone");
      lv_label_set_text(s_body, "");
      // The credentials rows are refresh_bottom's (below): joined or split
      // by what fits this glass (onboardlayout::join_lines, F45).
#else
      lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, s_join.title_top);
      lv_label_set_text(s_title, s_qr_ok ? "Scan with your phone camera"
                                         : "On your phone, join this network");
      lv_label_set_text(s_body, "");
      lv_label_set_text_fmt(s_creds,
                            s_qr_ok ? onboardlayout::kWideScanFmt
                                    : onboardlayout::kWideTypeFmt,
                            s_ap_ssid, s_ap_pass);
#endif
      ring_breathe();
      break;

    case ObStage::PhoneJoined:
      show_qr(false);
      canary_mark_mood(CanaryMood::Idle);
      lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -16);
      lv_label_set_text(s_title, "Nice - check your phone");
      lv_obj_align(s_body, LV_ALIGN_CENTER, 0, 14);
      lv_label_set_text(s_body, "a setup page is opening");
      lv_label_set_text(s_creds, "");
      ring_breathe();
      break;

    case ObStage::Connecting:
      show_qr(false);
      canary_mark_mood(CanaryMood::Idle);
      lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -16);
      lv_label_set_text(s_title, "Joining");
      lv_obj_align(s_body, LV_ALIGN_CENTER, 0, 14);
      lv_label_set_text_fmt(s_body, "%.28s", detail ? detail : "your network");
      lv_label_set_text(s_creds, "");
      ring_sweep();
      break;

    case ObStage::Fail:
      show_qr(false);
      canary_mark_mood(CanaryMood::Hidden);  // no mascot on a miss
      lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -16);
      lv_obj_set_style_text_color(s_title, col_warn(), 0);
      lv_label_set_text(s_title, detail ? detail : "That didn't work");
      lv_obj_align(s_body, LV_ALIGN_CENTER, 0, 14);
      lv_label_set_text(s_body, "try again on your phone");
      lv_label_set_text(s_creds, "");
      ring_still(col_warn(), LV_OPA_50);
      break;

    case ObStage::Success:
      show_qr(false);
      canary_mark_mood(CanaryMood::Happy);   // the hop is earned
      lv_obj_set_style_text_color(s_title, col_text(), 0);
      lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -16);
      lv_label_set_text(s_title, "You're in.");
      lv_obj_align(s_body, LV_ALIGN_CENTER, 0, 14);
      lv_label_set_text(s_body, "looking for your canaries");
      lv_label_set_text(s_creds, "");
      ring_bloom();
      break;
  }
  // Fail tints the title; every other stage restores it.
  if (st != ObStage::Fail && st != ObStage::Success) {
    lv_obj_set_style_text_color(s_title, col_text(), 0);
  }
  refresh_bottom();
  content_enter();
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
  s_qr_card = s_qr = s_creds = s_hint = nullptr;
  s_qr_ok = false;
  s_hint_text[0] = '\0';
  s_hint_narrow[0] = '\0';
}

}  // namespace canary::ui

#endif  // FEATURE_ONBOARDING
