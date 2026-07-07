// src/ui/glance_ui.cpp — the Watch Station's round face, LVGL edition.
//
// Design contract (display_ux_design.md §Design language): readable across
// a dark room in under a second; motion is rationed — a 220 ms page fade,
// a 2 s breath on an unacked alert, the 900 ms hold-to-ack sweep, and
// nothing else moves, ever.
#include <config.h>
#ifdef CD_FLAVOR_WATCH

#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "canary/ui/glance_ui.h"
#include "canary/ui/theme.h"
#include "canary/trust.h"

namespace canary::ui {

using canary::fleet::Fleet;
using canary::fleet::Sev;
using canary::fleet::Witness;
using canary::fleet::the_fleet;

namespace {

constexpr int MAX_ARCS = CD_FLEET_MAX_DEVICES;
constexpr int EV_ROWS = 5;

lv_obj_t* s_scr = nullptr;

// Halo page
lv_obj_t* s_pg_halo = nullptr;
lv_obj_t* s_arcs[MAX_ARCS] = {nullptr};
lv_obj_t* s_hero = nullptr;       // big center word / count
lv_obj_t* s_hero_sub = nullptr;   // "5 canaries" / device demanding a look
lv_obj_t* s_hero_badge = nullptr; // "verified" / "acknowledged"
lv_obj_t* s_clock = nullptr;
lv_obj_t* s_banner = nullptr;     // wifi/broker honesty line

// Detail page
lv_obj_t* s_pg_dev = nullptr;
lv_obj_t* s_dev_ring = nullptr;
lv_obj_t* s_dev_name = nullptr;
lv_obj_t* s_dev_state = nullptr;
lv_obj_t* s_dev_event = nullptr;
lv_obj_t* s_dev_meta = nullptr;   // chain badge + battery
lv_obj_t* s_dev_pos = nullptr;    // "2 of 5"

// Events page
lv_obj_t* s_pg_ev = nullptr;
lv_obj_t* s_ev_title = nullptr;
lv_obj_t* s_ev_name[EV_ROWS] = {nullptr};
lv_obj_t* s_ev_meta[EV_ROWS] = {nullptr};

// Proof page (trailblazer spec §1)
lv_obj_t* s_pg_proof = nullptr;
lv_obj_t* s_proof_card = nullptr;   // white ground behind the QR
lv_obj_t* s_proof_qr = nullptr;
lv_obj_t* s_proof_who = nullptr;
lv_obj_t* s_proof_cap = nullptr;

// Heartbeat ring (spec §4) — the sanctioned 4th motion
lv_obj_t* s_beat_ring = nullptr;

// Ack hold ring (overlay, all pages)
lv_obj_t* s_ack_ring = nullptr;
lv_anim_t s_ack_anim;
bool s_ack_holding = false;

// Breathing animation target (the arc demanding attention)
lv_anim_t s_breath_anim;
lv_obj_t* s_breathing = nullptr;

int s_shown_page = -1;

lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* f, lv_color_t c) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, "");
  return l;
}

lv_obj_t* mk_page(lv_obj_t* parent) {
  lv_obj_t* p = lv_obj_create(parent);
  lv_obj_set_size(p, 240, 240);
  lv_obj_set_pos(p, 0, 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(p, 0, 0);
  lv_obj_set_style_pad_all(p, 0, 0);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
  return p;
}

// A bare, non-interactive arc: Quiet Glass draws with the background arc
// only (smooth AA stroke, rounded caps); indicator and knob are silenced.
lv_obj_t* mk_ring(lv_obj_t* parent, int16_t d, int16_t width) {
  lv_obj_t* a = lv_arc_create(parent);
  lv_obj_set_size(a, d, d);
  lv_obj_center(a);
  lv_arc_set_rotation(a, 270);           // 12 o'clock start
  lv_arc_set_bg_angles(a, 0, 360);
  lv_arc_set_value(a, 0);
  lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  return a;
}

void breath_cb(void* var, int32_t v) {
  lv_obj_set_style_arc_opa((lv_obj_t*)var, (lv_opa_t)v, LV_PART_MAIN);
}

void breathe(lv_obj_t* arc, bool on) {
  if (s_breathing == arc && on) return;
  if (s_breathing) {
    lv_anim_del(s_breathing, breath_cb);
    lv_obj_set_style_arc_opa(s_breathing, LV_OPA_COVER, LV_PART_MAIN);
    s_breathing = nullptr;
  }
  if (!on || !arc) return;
  s_breathing = arc;
  lv_anim_init(&s_breath_anim);
  lv_anim_set_var(&s_breath_anim, arc);
  lv_anim_set_exec_cb(&s_breath_anim, breath_cb);
  lv_anim_set_values(&s_breath_anim, LV_OPA_COVER, LV_OPA_40);
  lv_anim_set_time(&s_breath_anim, MOTION_BREATH_MS / 2);
  lv_anim_set_playback_time(&s_breath_anim, MOTION_BREATH_MS / 2);
  lv_anim_set_repeat_count(&s_breath_anim, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&s_breath_anim, lv_anim_path_ease_in_out);
  lv_anim_start(&s_breath_anim);
}

void fade_cb(void* var, int32_t v) {
  lv_obj_set_style_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
}

void show_page(lv_obj_t* page) {
  // Leaving the halo also parks its breathing anim — no animation may keep
  // ticking against a hidden object (review catch: rationed motion includes
  // rationed CPU).
  if (page != s_pg_halo) breathe(nullptr, false);
  lv_obj_t* pages[] = {s_pg_halo, s_pg_dev, s_pg_ev, s_pg_proof};
  for (lv_obj_t* p : pages) {
    if (p == page) {
      lv_obj_clear_flag(p, LV_OBJ_FLAG_HIDDEN);
      // lv_anim_start dedups same var+exec_cb in v8; the explicit del makes
      // the no-stacking intent visible rather than folklore.
      lv_anim_del(p, fade_cb);
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, p);
      lv_anim_set_exec_cb(&a, fade_cb);
      lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
      lv_anim_set_time(&a, MOTION_PAGE_MS);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
      lv_anim_start(&a);
    } else {
      lv_anim_del(p, fade_cb);
      lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void upper(char* s) {
  for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

// ── Page renderers ───────────────────────────────────────────────────────

void update_halo(const Fleet& fleet, uint32_t now, const GlanceState& st) {
  const int n = fleet.count();
  const Sev worst = fleet.worst(now);
  const lv_color_t tcol = st.night ? ncol_text() : col_text();
  const lv_color_t mcol = st.night ? ncol_muted() : col_muted();

  // Witness halo: one smooth arc per device, 12 o'clock start, 4° gaps.
  lv_obj_t* attention = nullptr;
  for (int i = 0; i < MAX_ARCS; i++) {
    if (i >= n || n == 0) {
      lv_obj_add_flag(s_arcs[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const Witness* w = fleet.at(i);
    if (!w) continue;
    lv_obj_clear_flag(s_arcs[i], LV_OBJ_FLAG_HIDDEN);
    // Signed math on purpose: the fleet caps at 8 (span >= 45°), but if
    // that cap ever grows, a tiny span must degrade to a sliver — never
    // underflow uint16 into a full-circle arc (review catch).
    const int span = 360 / n;
    const int a0 = i * span + 2;
    const int a1 = (i + 1) * span - 2;
    lv_arc_set_bg_angles(s_arcs[i], (uint16_t)a0,
                         (uint16_t)(a1 > a0 ? a1 : a0 + 1));
    const Sev s = fleet.witness_sev(*w, now);
    lv_obj_set_style_arc_color(s_arcs[i], sev_color(s, st.night), LV_PART_MAIN);
    if (s >= Sev::Alert && !st.acked && !attention) attention = s_arcs[i];
  }
  // Empty fleet: a single faint full ring says "listening".
  if (n == 0) {
    lv_obj_clear_flag(s_arcs[0], LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_bg_angles(s_arcs[0], 0, 360);
    lv_obj_set_style_arc_color(s_arcs[0], st.night ? ncol_muted() : col_edge(),
                               LV_PART_MAIN);
  }
  breathe(attention, attention != nullptr);

  // Hero: the one thing that matters.
  lv_obj_set_style_text_color(s_hero, tcol, 0);
  lv_obj_set_style_text_color(s_hero_sub, mcol, 0);
  if (n == 0) {
    lv_obj_set_style_text_font(s_hero, font_title(), 0);
    lv_label_set_text(s_hero, "Listening");
    lv_label_set_text(s_hero_sub,
                      st.mqtt_ok ? "for canaries" :
                      (st.wifi_ok ? "no broker yet" : "no wifi"));
    lv_label_set_text(s_hero_badge, "");
  } else if (worst <= Sev::Notice) {
    lv_obj_set_style_text_font(s_hero, font_title(), 0);
    lv_label_set_text(s_hero, "All quiet");
    lv_label_set_text_fmt(s_hero_sub, "%d %s", n,
                          n == 1 ? "canary" : "canaries");
    if (fleet.all_verified()) {
      lv_label_set_text(s_hero_badge, LV_SYMBOL_OK "  verified");
      lv_obj_set_style_text_color(s_hero_badge,
                                  st.night ? ncol_muted() : col_ok(), 0);
    } else {
      lv_label_set_text(s_hero_badge, "");
    }
  } else {
    char word[16];
    snprintf(word, sizeof(word), "%s", canary::fleet::sev_name(worst));
    upper(word);
    lv_obj_set_style_text_font(s_hero, font_title(), 0);
    lv_obj_set_style_text_color(s_hero, sev_color(worst, st.night), 0);
    lv_label_set_text(s_hero, word);
    // Which witness demands the look (worst first hit).
    lv_label_set_text(s_hero_sub, "");
    for (int i = 0; i < n; i++) {
      const Witness* w = fleet.at(i);
      if (w && fleet.witness_sev(*w, now) == worst) {
        lv_label_set_text_fmt(s_hero_sub, "%.20s", w->id);
        break;
      }
    }
    lv_label_set_text(s_hero_badge, st.acked ? "acknowledged" : "hold to acknowledge");
    lv_obj_set_style_text_color(s_hero_badge, mcol, 0);
  }

  // Clock + honesty banner.
  if (st.time_valid) {
    lv_label_set_text_fmt(s_clock, "%02d:%02d", st.clock_hh, st.clock_mm);
  } else {
    lv_label_set_text(s_clock, "");
  }
  lv_obj_set_style_text_color(s_clock, mcol, 0);
  if (!st.wifi_ok) {
    lv_label_set_text(s_banner, LV_SYMBOL_WIFI "  wifi down");
    lv_obj_set_style_text_color(s_banner,
                                st.night ? ncol_alert() : col_alert(), 0);
  } else if (!st.mqtt_ok) {
    lv_label_set_text(s_banner, "broker down · last known");
    lv_obj_set_style_text_color(s_banner,
                                st.night ? ncol_alert() : col_warn(), 0);
  } else {
    lv_label_set_text(s_banner, "");
  }
}

void update_device(const Fleet& fleet, uint32_t now, const GlanceState& st,
                   int idx) {
  const Witness* w = fleet.at(idx);
  if (!w) return;
  const Sev s = fleet.witness_sev(*w, now);
  const lv_color_t tcol = st.night ? ncol_text() : col_text();
  const lv_color_t mcol = st.night ? ncol_muted() : col_muted();

  lv_obj_set_style_arc_color(s_dev_ring, sev_color(s, st.night), LV_PART_MAIN);

  lv_obj_set_style_text_color(s_dev_name, tcol, 0);
  lv_label_set_text_fmt(s_dev_name, "%.18s", w->id);

  lv_obj_set_style_text_color(s_dev_state, sev_color(s, st.night), 0);
  lv_label_set_text(s_dev_state, link_label(w->link));

  lv_obj_set_style_text_color(s_dev_event, mcol, 0);
  if (w->has_event) {
    char human[48], age[8];
    humanize_event(w->last_event, human, sizeof(human));
    format_age(now, w->last_event_ms, age, sizeof(age));
    lv_label_set_text_fmt(s_dev_event, "%s\n%s ago", human, age);
  } else {
    lv_label_set_text(s_dev_event, "no events yet");
  }

  char batt[20] = "";
  if (w->battery_present && w->battery_pct >= 0) {
    snprintf(batt, sizeof(batt), "  ·  %s %d%%",
             w->battery_pct < 25 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_3,
             (int)w->battery_pct);
  }
  lv_obj_set_style_text_color(s_dev_meta, badge_color(w->badge, st.night), 0);
  lv_label_set_text_fmt(s_dev_meta, "%s%s", badge_text(w->badge), batt);

  lv_obj_set_style_text_color(s_dev_pos, st.night ? ncol_muted() : col_faint(), 0);
  lv_label_set_text_fmt(s_dev_pos, "%d of %d", idx + 1, fleet.count());
}

void update_events(const Fleet& fleet, uint32_t now, const GlanceState& st) {
  const lv_color_t tcol = st.night ? ncol_text() : col_text();
  const lv_color_t mcol = st.night ? ncol_muted() : col_muted();
  lv_obj_set_style_text_color(s_ev_title, mcol, 0);

  const int n = fleet.events_count();
  for (int i = 0; i < EV_ROWS; i++) {
    const auto* e = (i < n) ? fleet.event_at(i) : nullptr;
    if (!e) {
      lv_label_set_text(s_ev_name[i], i == 0 && n == 0 ? "Nothing witnessed" : "");
      lv_label_set_text(s_ev_meta[i], "");
      lv_obj_set_style_text_color(s_ev_name[i], mcol, 0);
      continue;
    }
    char human[40], age[8];
    humanize_event(e->name, human, sizeof(human));
    format_age(now, e->at_ms, age, sizeof(age));
    lv_obj_set_style_text_color(
        s_ev_name[i], e->sev >= Sev::Warn ? sev_color(e->sev, st.night) : tcol, 0);
    lv_label_set_text_fmt(s_ev_name[i], "%.24s", human);
    lv_obj_set_style_text_color(s_ev_meta[i], mcol, 0);
    lv_label_set_text_fmt(s_ev_meta[i], "%s · %.14s%s", age, e->device,
                          e->signed_flag ? " · signed" : "");
  }
}

// Proof page: QR of the most urgent witness's signed chain head — the
// exact bytes it published, plus the pinned pubkey (spec §1). Dark-on-
// light on purpose: scanners want it, and scanning implies the user is
// awake and active.
void update_proof(const Fleet& fleet, uint32_t now, const GlanceState& st) {
  (void)st;
  const int n = fleet.count();
  const Witness* pick = nullptr;
  for (int i = 0; i < n; i++) {
    const Witness* w = fleet.at(i);
    if (w && fleet.witness_sev(*w, now) >= Sev::Alert) { pick = w; break; }
  }
  if (!pick && n > 0) pick = fleet.at(0);

  char pk[65];
  if (!pick || !pick->chain_raw[0] ||
      !canary::trust::pinned_pubkey_hex(pick->id, pk)) {
    lv_obj_add_flag(s_proof_card, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_proof_who, pick ? pick->id : "");
    lv_label_set_text(s_proof_cap,
                      pick ? "No signed chain to prove yet"
                           : "No witnesses yet");
    return;
  }

  static char body[640];
  const int len = snprintf(body, sizeof(body),
                           "{\"v\":1,\"t\":\"securacv/%s/chain\",\"pk\":\"%s\","
                           "\"p\":%s}",
                           pick->id, pk, pick->chain_raw);
  if (len <= 0 || (size_t)len >= sizeof(body)) {
    lv_obj_add_flag(s_proof_card, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_proof_cap, "Proof payload too large");
    return;
  }
  lv_obj_clear_flag(s_proof_card, LV_OBJ_FLAG_HIDDEN);
  lv_qrcode_update(s_proof_qr, body, (uint32_t)len);
  lv_label_set_text_fmt(s_proof_who, "%.18s", pick->id);
  lv_label_set_text(s_proof_cap, "Scan to verify · no cloud");
}

void ack_cb(void* var, int32_t v) {
  (void)var;
  lv_arc_set_bg_angles(s_ack_ring, 0, (uint16_t)v);
}

void beat_cb(void* var, int32_t v) {
  lv_obj_set_style_arc_opa((lv_obj_t*)var, (lv_opa_t)v, LV_PART_MAIN);
}

// The heartbeat (spec §4): one soft swell, cryptographically earned — only
// fired by glance_ui_update when everything is reachable AND verified.
void heartbeat_pulse() {
  if (!s_beat_ring) return;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_beat_ring);
  lv_anim_set_exec_cb(&a, beat_cb);
  lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_30);
  lv_anim_set_time(&a, 800);
  lv_anim_set_playback_time(&a, 800);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────

int glance_page_count() { return 3 + the_fleet().count(); }

void glance_ui_create() {
  s_scr = lv_scr_act();
  lv_obj_set_style_bg_color(s_scr, col_bg(), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

  // ── Halo page ──
  s_pg_halo = mk_page(s_scr);
  for (int i = 0; i < MAX_ARCS; i++) {
    s_arcs[i] = mk_ring(s_pg_halo, 232, 10);
    lv_obj_add_flag(s_arcs[i], LV_OBJ_FLAG_HIDDEN);
  }
  s_hero = mk_label(s_pg_halo, font_title(), col_text());
  lv_obj_align(s_hero, LV_ALIGN_CENTER, 0, -26);
  s_hero_sub = mk_label(s_pg_halo, font_label(), col_muted());
  lv_obj_align(s_hero_sub, LV_ALIGN_CENTER, 0, 6);
  s_hero_badge = mk_label(s_pg_halo, font_caption(), col_muted());
  lv_obj_set_style_text_letter_space(s_hero_badge, 1, 0);
  lv_obj_align(s_hero_badge, LV_ALIGN_CENTER, 0, 28);
  s_clock = mk_label(s_pg_halo, font_clock(), col_muted());
  lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, 62);
  s_banner = mk_label(s_pg_halo, font_caption(), col_alert());
  lv_obj_align(s_banner, LV_ALIGN_CENTER, 0, 88);

  // ── Device page ──
  s_pg_dev = mk_page(s_scr);
  s_dev_ring = mk_ring(s_pg_dev, 232, 6);
  s_dev_name = mk_label(s_pg_dev, font_body(), col_text());
  lv_obj_align(s_dev_name, LV_ALIGN_CENTER, 0, -62);
  s_dev_state = mk_label(s_pg_dev, font_title(), col_ok());
  lv_obj_align(s_dev_state, LV_ALIGN_CENTER, 0, -28);
  s_dev_event = mk_label(s_pg_dev, font_label(), col_muted());
  lv_obj_set_style_text_align(s_dev_event, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_dev_event, LV_ALIGN_CENTER, 0, 16);
  s_dev_meta = mk_label(s_pg_dev, font_caption(), col_muted());
  lv_obj_align(s_dev_meta, LV_ALIGN_CENTER, 0, 56);
  s_dev_pos = mk_label(s_pg_dev, font_caption(), col_faint());
  lv_obj_align(s_dev_pos, LV_ALIGN_CENTER, 0, 84);

  // ── Events page ──
  s_pg_ev = mk_page(s_scr);
  s_ev_title = mk_label(s_pg_ev, font_caption(), col_muted());
  lv_obj_set_style_text_letter_space(s_ev_title, 2, 0);
  lv_label_set_text(s_ev_title, "RECENT");
  lv_obj_align(s_ev_title, LV_ALIGN_TOP_MID, 0, 32);
  for (int i = 0; i < EV_ROWS; i++) {
    s_ev_name[i] = mk_label(s_pg_ev, font_label(), col_text());
    lv_obj_align(s_ev_name[i], LV_ALIGN_TOP_MID, 0, 58 + i * 32);
    s_ev_meta[i] = mk_label(s_pg_ev, font_caption(), col_muted());
    lv_obj_align(s_ev_meta[i], LV_ALIGN_TOP_MID, 0, 74 + i * 32);
  }

  // ── Proof page ──
  s_pg_proof = mk_page(s_scr);
  s_proof_who = mk_label(s_pg_proof, font_caption(), col_muted());
  lv_obj_align(s_proof_who, LV_ALIGN_TOP_MID, 0, 26);
  s_proof_card = lv_obj_create(s_pg_proof);
  lv_obj_set_size(s_proof_card, 156, 156);
  lv_obj_align(s_proof_card, LV_ALIGN_CENTER, 0, 2);
  lv_obj_set_style_bg_color(s_proof_card, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(s_proof_card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_proof_card, 10, 0);
  lv_obj_set_style_border_width(s_proof_card, 0, 0);
  lv_obj_set_style_pad_all(s_proof_card, 8, 0);
  lv_obj_clear_flag(s_proof_card, LV_OBJ_FLAG_SCROLLABLE);
  s_proof_qr = lv_qrcode_create(s_proof_card, 140, lv_color_black(),
                                lv_color_white());
  lv_obj_center(s_proof_qr);
  s_proof_cap = mk_label(s_pg_proof, font_caption(), col_muted());
  lv_obj_align(s_proof_cap, LV_ALIGN_BOTTOM_MID, 0, -26);

  // ── Heartbeat ring (starts invisible; pulses only when earned) ──
  s_beat_ring = mk_ring(s_scr, 238, 2);
  lv_obj_set_style_arc_color(s_beat_ring, col_ok(), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(s_beat_ring, LV_OPA_TRANSP, LV_PART_MAIN);

  // ── Ack hold ring (overlay) ──
  s_ack_ring = mk_ring(s_scr, 220, 4);
  lv_obj_set_style_arc_color(s_ack_ring, col_text(), LV_PART_MAIN);
  lv_obj_add_flag(s_ack_ring, LV_OBJ_FLAG_HIDDEN);

  lv_obj_add_flag(s_pg_dev, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_pg_ev, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_pg_proof, LV_OBJ_FLAG_HIDDEN);
  s_shown_page = 0;
}

void glance_ui_update(const Fleet& fleet, uint32_t now, const GlanceState& st) {
  if (!s_scr) return;
  const int devices = fleet.count();
  const int pages = 3 + devices;
  int page = st.page;
  if (page >= pages || page < 0) page = 0;

  if (page != s_shown_page) {
    s_shown_page = page;
    if (page == 0)                show_page(s_pg_halo);
    else if (page <= devices)     show_page(s_pg_dev);
    else if (page == devices + 1) show_page(s_pg_ev);
    else                          show_page(s_pg_proof);
  }

  if (page == 0)                update_halo(fleet, now, st);
  else if (page <= devices)     update_device(fleet, now, st, page - 1);
  else if (page == devices + 1) update_events(fleet, now, st);
  else                          update_proof(fleet, now, st);

  // The heartbeat: earned, daytime, once a minute (spec §4). Absence is
  // information — any lesser state and the ring stays dark.
  static uint32_t s_last_beat_ms = 0;
  if (!st.night && page == 0 && fleet.count() > 0 &&
      fleet.worst(now) <= Sev::Notice && fleet.all_verified() &&
      st.wifi_ok && st.mqtt_ok &&
      (int32_t)(now - s_last_beat_ms) >= (int32_t)CD_HEARTBEAT_UI_MS) {
    s_last_beat_ms = now;
    heartbeat_pulse();
  }
}

void glance_ui_ack_hold(bool active) {
  if (!s_ack_ring) return;
  if (active && !s_ack_holding) {
    s_ack_holding = true;
    lv_obj_clear_flag(s_ack_ring, LV_OBJ_FLAG_HIDDEN);
    lv_anim_init(&s_ack_anim);
    lv_anim_set_var(&s_ack_anim, s_ack_ring);
    lv_anim_set_exec_cb(&s_ack_anim, ack_cb);
    lv_anim_set_values(&s_ack_anim, 1, 360);
    lv_anim_set_time(&s_ack_anim, MOTION_ACK_MS);
    lv_anim_set_path_cb(&s_ack_anim, lv_anim_path_linear);
    lv_anim_start(&s_ack_anim);
  } else if (!active && s_ack_holding) {
    s_ack_holding = false;
    lv_anim_del(s_ack_ring, ack_cb);
    lv_obj_add_flag(s_ack_ring, LV_OBJ_FLAG_HIDDEN);
  }
}

}  // namespace canary::ui

#endif  // CD_FLAVOR_WATCH
