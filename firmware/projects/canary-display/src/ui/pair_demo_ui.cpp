// src/ui/pair_demo_ui.cpp — the First Light pair demo surface.
// See canary/ui/pair_demo_ui.h for the contract and canary/pair/pair_demo.h
// for the pure core every decision here defers to. docs/first_light_demo.md
// is the runbook.
//
// Voice notes, because this card is easy to get wrong:
//   * Words carry the meaning; the amber wash only echoes them (WCAG 1.4.1,
//     the theme's own rule). Detection renders in the WARN family — never
//     red, which stays reserved for lost/tamper/urgent.
//   * The chip says PAIR DEMO the whole time, the demo_mode precedent: a
//     surface that behaves differently from the product face announces
//     itself. And the badge machinery is untouched — an unsigned presence
//     hint can never dress up as a verified witness.
//   * The one raw-milliseconds readout ("glass react") is the point of the
//     surface — trigger timing — and it is a number THIS device measured on
//     its own clock: micros at the receive drain to micros at the paint
//     that shows the edge. The panel flush after it costs at most one
//     frame. No cross-device clock is claimed anywhere; the camera logs its
//     own numbers on its own serial.
#include <config.h>

#if defined(FEATURE_PAIR_DEMO) && FEATURE_PAIR_DEMO

#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>
#include <esp_wifi.h>
#include <stdio.h>

#include "canary/ui/pair_demo_ui.h"
#include "canary/ui/theme.h"
#include "canary/pair/pair_demo.h"
#include "canary/fleet/fleet_instance.h"
#include "canary/net/wifi_mgr.h"
#include "canary/runtime_config.h"
#include "canary/log.h"
#include "fleet_link/fleet_beacon_espnow.h"  // fallback-channel contract

namespace canary::ui {

namespace {

canary::pair::PairDemo s_demo;

lv_obj_t* s_prev = nullptr;
lv_obj_t* s_scr = nullptr;
lv_obj_t* s_wash = nullptr;    // full-glass edge pulse (opa toggled)
lv_obj_t* s_chip = nullptr;    // "PAIR DEMO" — always on, demo_mode precedent
lv_obj_t* s_hero = nullptr;    // PERSON / watching / listening...
lv_obj_t* s_sub = nullptr;     // confidence line
lv_obj_t* s_who = nullptr;     // SCV-XXXX + band
lv_obj_t* s_react = nullptr;   // glass react N ms
lv_obj_t* s_count = nullptr;   // triggers + last age
lv_obj_t* s_honest = nullptr;  // the honesty footer
lv_obj_t* s_hint = nullptr;    // button hints

uint32_t s_pulse_until_ms = 0;
bool s_edge_paint_pending = false;
bool s_auto_open_req = false;
canary::pair::PairStage s_built_stage = canary::pair::PairStage::Idle;

constexpr const char* NVS_NS = "scv-nl";   // the nightlight's own namespace
constexpr const char* NVS_KEY = "pairfp";  // remembered witness suffix

void store_lock(const char* fp4) {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  p.putString(NVS_KEY, fp4 ? fp4 : "");
  p.end();
}

void load_lock(char out[5]) {
  out[0] = '\0';
  Preferences p;
  if (!p.begin(NVS_NS, true)) return;
  const String v = p.getString(NVS_KEY, "");
  p.end();
  if (v.length() == 4) {
    strncpy(out, v.c_str(), 4);
    out[4] = '\0';
  }
}

const char* via_word(canary::fleet::Via v) {
  switch (v) {
    case canary::fleet::Via::Mesh: return "direct radio";
    case canary::fleet::Via::Wifi: return "home wifi";
    default:                       return "ble";
  }
}

const char* class_word(uint8_t detect_class) {
  switch (detect_class) {
    case 1:  return "PERSON";
    case 2:  return "VEHICLE";
    case 3:  return "ANIMAL";
    case 4:  return "PACKAGE";
    default: return "";
  }
}

lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* f, lv_color_t c) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(l, "");
  return l;
}

// Park the radio on the shared fallback channel while nothing owns it, so
// this glass hears a factory-fresh Vision (which parks on the same channel —
// the contract in fleet_beacon_espnow.h). Associated STA = the radio is
// spoken for; never retune under it.
void park_channel_if_idle() {
  if (canary::net::wifi_connected()) return;
  uint8_t ch = 0;
  wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&ch, &sec) != ESP_OK) return;
  if (ch == FLEET_BEACON_ESPNOW_FALLBACK_CHANNEL) return;
  esp_wifi_set_channel(FLEET_BEACON_ESPNOW_FALLBACK_CHANNEL,
                       WIFI_SECOND_CHAN_NONE);
}

// (Re)build the static composition for the current stage. Label TEXT is
// refreshed every tick; this only runs when the stage itself changes.
void build(uint32_t now) {
  using canary::pair::PairStage;
  lv_obj_clean(s_scr);
  s_wash = s_chip = s_hero = s_sub = s_who = nullptr;
  s_react = s_count = s_honest = s_hint = nullptr;

  const int16_t H = lv_disp_get_ver_res(NULL);

  // The wash sits UNDER the text: the pulse echoes the words, never
  // replaces them.
  s_wash = lv_obj_create(s_scr);
  lv_obj_set_size(s_wash, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(s_wash, col_warn(), 0);
  lv_obj_set_style_bg_opa(s_wash, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_wash, 0, 0);
  lv_obj_set_style_radius(s_wash, 0, 0);
  lv_obj_clear_flag(s_wash, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(s_wash, LV_ALIGN_CENTER, 0, 0);

  s_chip = mk_label(s_scr, font_caption(), col_warn());
  lv_label_set_text(s_chip, "PAIR DEMO");
  lv_obj_set_style_border_width(s_chip, 1, 0);
  lv_obj_set_style_border_color(s_chip, col_warn(), 0);
  lv_obj_set_style_radius(s_chip, 4, 0);
  lv_obj_set_style_pad_left(s_chip, 6, 0);
  lv_obj_set_style_pad_right(s_chip, 6, 0);
  lv_obj_set_style_pad_top(s_chip, 2, 0);
  lv_obj_set_style_pad_bottom(s_chip, 2, 0);
  lv_obj_align(s_chip, LV_ALIGN_TOP_MID, 0, 8);

  switch (s_demo.stage()) {
    case PairStage::Listening: {
      s_hero = mk_label(s_scr, font_title(), col_text());
      lv_label_set_text(s_hero, "First Light");
      lv_obj_align(s_hero, LV_ALIGN_CENTER, 0, -H / 6);
      s_sub = mk_label(s_scr, font_body(), col_muted());
      lv_label_set_text(s_sub,
                        "power your\nCanary Vision\nnearby");
      lv_obj_align(s_sub, LV_ALIGN_CENTER, 0, 0);
      s_hint = mk_label(s_scr, font_caption(), col_faint());
      lv_label_set_text(s_hint, "hold: leave");
      lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
      break;
    }
    case PairStage::Found:
    case PairStage::Live: {
      s_hero = mk_label(s_scr, font_title(), col_muted());
      lv_obj_align(s_hero, LV_ALIGN_CENTER, 0, -H / 4);
      s_sub = mk_label(s_scr, font_body(), col_muted());
      lv_obj_align(s_sub, LV_ALIGN_CENTER, 0, -H / 4 + 34);
      s_who = mk_label(s_scr, font_caption(), col_text());
      lv_obj_align(s_who, LV_ALIGN_CENTER, 0, -6);
      s_react = mk_label(s_scr, font_body(), col_signed());
      lv_obj_align(s_react, LV_ALIGN_CENTER, 0, 24);
      s_count = mk_label(s_scr, font_caption(), col_muted());
      lv_obj_align(s_count, LV_ALIGN_CENTER, 0, 52);
      s_honest = mk_label(s_scr, font_caption(), col_faint());
      lv_label_set_text(s_honest, "unsigned presence hint\nnot a recording");
      lv_obj_align(s_honest, LV_ALIGN_BOTTOM_MID, 0, -26);
      s_hint = mk_label(s_scr, font_caption(), col_faint());
      lv_label_set_text(s_hint, s_demo.stage() == PairStage::Found
                                    ? "tap: keep it - hold: leave"
                                    : "double: forget - hold: leave");
      lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
      break;
    }
    default:
      break;
  }
  s_built_stage = s_demo.stage();
  (void)now;
}

// Refresh the live text. Runs at the tick cadence; every set_text is cheap
// and LVGL only repaints labels whose text actually changed.
void refresh(uint32_t now) {
  using canary::pair::PairStage;
  const PairStage st = s_demo.stage();
  if (st != s_built_stage) build(now);
  if (st == PairStage::Idle || st == PairStage::Listening) return;

  const bool stale = s_demo.stale(now);
  if (!s_demo.heard() || stale) {
    lv_obj_set_style_text_color(s_hero, col_muted(), 0);
    lv_label_set_text(s_hero, "listening...");
    lv_label_set_text(s_sub, stale ? "the camera went quiet" : "");
  } else if (s_demo.alert_now()) {
    lv_obj_set_style_text_color(s_hero, col_warn(), 0);
    lv_label_set_text(s_hero, class_word(s_demo.detect_class()));
    if (s_demo.detect_score() >= 0) {
      lv_label_set_text_fmt(s_sub, "%d%% sure", (int)s_demo.detect_score());
    } else {
      lv_label_set_text(s_sub, "");
    }
  } else {
    lv_obj_set_style_text_color(s_hero, col_ok(), 0);
    lv_label_set_text(s_hero, "watching");
    lv_label_set_text(s_sub, "wave at the camera");
  }

  lv_label_set_text_fmt(s_who, "SCV-%s - %s", s_demo.fp4(),
                        via_word(s_demo.via()));

  if (s_demo.react_us() > 0) {
    lv_label_set_text_fmt(s_react, "glass react %lu ms",
                          (unsigned long)((s_demo.react_us() + 500) / 1000));
  } else {
    lv_label_set_text(s_react, s_demo.edges() ? "" : "glass react - ms");
  }

  if (s_demo.edges() > 0) {
    char age[16];
    format_age(now, s_demo.last_edge_ms(), age, sizeof(age));
    lv_label_set_text_fmt(s_count, "%lu trigger%s - last %s",
                          (unsigned long)s_demo.edges(),
                          s_demo.edges() == 1 ? "" : "s", age);
  } else {
    lv_label_set_text(s_count, "no triggers yet");
  }

  // The edge pulse: an amber wash under the words while the window rides.
  const bool washed = (int32_t)(now - s_pulse_until_ms) < 0;
  lv_obj_set_style_bg_opa(s_wash, washed ? LV_OPA_30 : LV_OPA_TRANSP, 0);
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────

void pair_demo_ui_open(uint32_t now) {
  if (s_scr) return;
  char remembered[5];
  load_lock(remembered);
  s_demo.open(now, remembered);
  s_pulse_until_ms = 0;
  s_edge_paint_pending = false;

  s_prev = lv_scr_act();
  s_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s_scr, col_bg(), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
  build(now);
  lv_scr_load_anim(s_scr, LV_SCR_LOAD_ANIM_FADE_ON, MOTION_PAGE_MS, 0, false);
  canary::log_line("PAIR", remembered[0]
                               ? "First Light demo open (remembered camera)"
                               : "First Light demo open (listening)");
}

void pair_demo_ui_close() {
  if (!s_scr) return;
  s_demo.close();
  s_scr = nullptr;
  s_wash = s_chip = s_hero = s_sub = s_who = nullptr;
  s_react = s_count = s_honest = s_hint = nullptr;
  s_built_stage = canary::pair::PairStage::Idle;
  lv_scr_load_anim(s_prev, LV_SCR_LOAD_ANIM_FADE_ON, MOTION_PAGE_MS, 0, true);
  s_prev = nullptr;
  canary::log_line("PAIR", "First Light demo closed");
}

bool pair_demo_ui_active() { return s_scr != nullptr; }

void pair_demo_ui_tick(uint32_t now) {
  if (!s_scr) return;

  // The modal contract: a live urgent alert takes the glass back instantly.
  {
    auto& fleet = canary::fleet::the_fleet();
    if (fleet.worst(now) >= canary::fleet::Sev::Alert &&
        !fleet.ack_active(now)) {
      pair_demo_ui_close();
      return;
    }
  }

  // Keep the router-free band audible while nothing owns the radio.
  park_channel_if_idle();

  refresh(now);

  // The react clock: the paint that first shows an edge closes the
  // measurement this device started at its own receive drain. The flush
  // that follows in this same loop pass costs at most one frame more.
  if (s_edge_paint_pending) {
    s_edge_paint_pending = false;
    s_demo.note_react_us(micros() - s_demo.edge_rx_us());
    refresh(now);  // show the number on the same pass it was measured
  }
}

void pair_demo_note_beacon(const char* fp4,
                           const canary::fleet::BeaconStatus& s,
                           bool have_status, uint32_t now,
                           canary::fleet::Via via) {
  if (!s_scr) {
    // Closed: the only decision is the boxed-pair auto-open. Mailboxed —
    // the receive drains never build LVGL surfaces themselves.
    auto& fleet = canary::fleet::the_fleet();
    const bool urgent = fleet.worst(now) >= canary::fleet::Sev::Alert &&
                        !fleet.ack_active(now);
    if (canary::pair::pair_should_auto_open(canary::cfg::wifi_is_placeholder(),
                                            via, false, urgent)) {
      s_auto_open_req = true;
    }
    return;
  }
  const uint32_t rx_us = micros();
  if (s_demo.observe(fp4, s, have_status, now, via, rx_us)) {
    s_pulse_until_ms = now + canary::pair::PAIR_PULSE_MS;
    s_edge_paint_pending = true;
  }
}

bool pair_demo_take_auto_open() {
  const bool req = s_auto_open_req;
  s_auto_open_req = false;
  return req;
}

void pair_demo_ui_button_short(uint32_t now) {
  if (!s_scr) return;
  if (s_demo.can_lock()) {
    s_demo.lock();
    store_lock(s_demo.fp4());
    refresh(now);
    canary::log_line("PAIR", "camera kept (lock stored)");
  }
}

void pair_demo_ui_button_double(uint32_t now) {
  if (!s_scr) return;
  if (s_demo.locked() || s_demo.fp4()[0]) {
    s_demo.forget();
    store_lock("");
    build(now);
    canary::log_line("PAIR", "camera forgotten (lock cleared)");
  }
}

}  // namespace canary::ui

#endif  // FEATURE_PAIR_DEMO
