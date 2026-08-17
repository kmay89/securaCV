// src/ui/commission_ui.cpp — "add a canary" commissioning surface.
// See commission_ui.h and docs/hardware/canary_qr_onboarding.md.
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
#include <esp_random.h>

#include "commission_ui.h"
#include "round_frame_core.h"
#include "theme.h"
#include "runtime_config.h"
#include "mqtt_mgr.h"
#include "display.h"
#include "provision_qr.h"

namespace canary::ui {

namespace {

// Rendering budgets from the onboarding research: the round display's
// clipped corners leave a 169 px inscribed square (the Round Frame engine
// computes it; the assert below pins this file to it), and a fixed-focus
// camera lens wants big modules more than it wants error correction — so
// the watch caps the payload at 78 bytes (QR v4-L) and renders 164 px.
// The dash has room for the full payload at a comfortable 360 px.
#ifdef CD_FLAVOR_WATCH
// 148 px keeps the full white quiet zone inside the circle at the card's
// corners (a black bezel touching a finder pattern kills decodes); the
// payload cap keeps the code at QR v4/v5 so a fixed-focus lens still
// resolves the modules from a hand-width away.
constexpr int QR_PX = 148;
constexpr size_t PAYLOAD_CAP = 84;
constexpr int HIT_PAD = 8;
// The card (QR + 16 px of quiet zone, see card_px below) must live inside
// the disc's inscribed square — the engine owns that number now, so a
// future display or margin change breaks the build here, not the decode.
static_assert(QR_PX + 16 <= roundframe::inscribed_square(
                                roundframe::kDiscDiameter, 0),
              "commissioning card outgrew the round glass's inscribed square");
#else
constexpr int QR_PX = 360;
constexpr size_t PAYLOAD_CAP = 320;
constexpr int HIT_PAD = 10;
#endif

constexpr uint32_t TOKEN_TTL_S = 600;      // fresh code every 10 min
constexpr uint32_t IDLE_CLOSE_MS = 300000; // scanning takes a while; 5 min
constexpr uint32_t JOINED_HOLD_MS = 6000;

enum class Face { Code, NoWifi, TooLong, Joined };

lv_obj_t* s_prev = nullptr;
lv_obj_t* s_scr = nullptr;
Face s_face = Face::Code;
lv_obj_t* s_qr = nullptr;
lv_obj_t* s_card = nullptr;
lv_obj_t* s_count = nullptr;
lv_obj_t* s_close = nullptr;   // tap target: leave
lv_obj_t* s_fresh = nullptr;   // tap target: new code
char s_token[24] = {0};
char s_payload[336] = {0};
uint32_t s_minted_ms = 0;
uint32_t s_last_touch_ms = 0;
uint32_t s_joined_at_ms = 0;
// Joined-celebration baseline. Count alone is not enough (review catch):
// opened during a broker reconnect, the retained replay of ALREADY-PAIRED
// witnesses would grow the count and fake a success. The baseline is only
// captured after the hub link has been continuously up for a settle
// window (retained replays land within moments of subscribing), and a
// link drop invalidates it until the link steadies again.
constexpr uint32_t BASELINE_SETTLE_MS = 3000;
int s_baseline_count = -1;
uint32_t s_link_up_since_ms = 0;   // 0 = link down

lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* f, lv_color_t c) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, "");
  return l;
}

void mint_token() {
  // 16 random bytes -> 22-char base64url. Physical proximity + expiry are
  // the secret; the token only fast-tracks the blessing.
  static const char* B64 =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  uint8_t raw[16];
  for (int i = 0; i < 16; i += 4) {
    const uint32_t r = esp_random();
    raw[i] = (uint8_t)(r);
    raw[i + 1] = (uint8_t)(r >> 8);
    raw[i + 2] = (uint8_t)(r >> 16);
    raw[i + 3] = (uint8_t)(r >> 24);
  }
  int o = 0;
  for (int i = 0; i < 15; i += 3) {
    const uint32_t v = ((uint32_t)raw[i] << 16) | ((uint32_t)raw[i + 1] << 8) |
                       raw[i + 2];
    s_token[o++] = B64[(v >> 18) & 63];
    s_token[o++] = B64[(v >> 12) & 63];
    s_token[o++] = B64[(v >> 6) & 63];
    s_token[o++] = B64[v & 63];
  }
  s_token[o++] = B64[(raw[15] >> 2) & 63];
  s_token[o++] = B64[(raw[15] << 4) & 63];
  s_token[o] = '\0';  // 22 chars
}

// Build the payload. Returns false when it can't fit this glass's budget.
bool mint_payload() {
  const auto& cfg = canary::cfg::get();
  mint_token();
  const bool hub_known = !canary::net::mqtt_broker_is_placeholder();
  const time_t now_epoch = time(nullptr);
  const int64_t expiry =
      now_epoch > 1700000000 ? (int64_t)now_epoch + TOKEN_TTL_S : 0;

  size_t n = securacv::qr::mint_scv1(
      s_payload, sizeof(s_payload), cfg.wifi_ssid, cfg.wifi_pass,
      hub_known ? canary::net::mqtt_broker_host() : nullptr,
      hub_known ? canary::net::mqtt_broker_port() : 1883, s_token, expiry,
      nullptr);
#ifdef CD_FLAVOR_WATCH
  if (n > PAYLOAD_CAP) {
    // Shed weight for the small glass: hub address first (the canary can
    // ask the fleet), then the expiry stamp. The token always stays.
    n = securacv::qr::mint_scv1(s_payload, sizeof(s_payload), cfg.wifi_ssid,
                                cfg.wifi_pass, nullptr, 1883, s_token, expiry,
                                nullptr);
    if (n > PAYLOAD_CAP) {
      n = securacv::qr::mint_scv1(s_payload, sizeof(s_payload), cfg.wifi_ssid,
                                  cfg.wifi_pass, nullptr, 1883, s_token, 0,
                                  nullptr);
    }
  }
#endif
  if (n == 0 || n > PAYLOAD_CAP) return false;
  s_minted_ms = millis();
  return true;
}

void fmt_countdown(char* out, size_t cap, uint32_t now_ms) {
  // Seconds domain (repo rule): ms math on uint32 wraps at ~49.7 days.
  const uint32_t age_s = (now_ms - s_minted_ms) / 1000;
  const uint32_t s = age_s >= TOKEN_TTL_S ? 0 : TOKEN_TTL_S - age_s;
#ifdef CD_FLAVOR_WATCH
  snprintf(out, cap, "fresh %lu:%02lu • tap = new", (unsigned long)(s / 60),
           (unsigned long)(s % 60));
#else
  snprintf(out, cap, "fresh for %lu:%02lu", (unsigned long)(s / 60),
           (unsigned long)(s % 60));
#endif
}

void build();

void set_face(Face f) {
  s_face = f;
  build();
}

void build() {
  lv_obj_clean(s_scr);
  s_qr = s_card = s_count = s_close = s_fresh = nullptr;

  s_close = mk_label(s_scr, font_caption(), col_faint());
  lv_label_set_text_fmt(s_close, LV_SYMBOL_LEFT " %s",
                        s_face == Face::Joined ? "done" : "add a canary");
#ifdef CD_FLAVOR_WATCH
  lv_obj_align(s_close, LV_ALIGN_TOP_MID, 0, 14);
#else
  lv_obj_align(s_close, LV_ALIGN_TOP_LEFT, 24, 16);
#endif

  switch (s_face) {
    case Face::Code: {
      // Dark-on-white card, never inverted (the Smart-Invert failure
      // class), white margin as the quiet zone.
      s_card = lv_obj_create(s_scr);
      const int card_px = QR_PX + 16;
      lv_obj_set_size(s_card, card_px, card_px);
      lv_obj_set_style_bg_color(s_card, lv_color_white(), 0);
      lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(s_card, 8, 0);
      lv_obj_set_style_border_width(s_card, 0, 0);
      lv_obj_set_style_pad_all(s_card, 8, 0);
      lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
      s_qr = mk_qrcode(s_card, QR_PX);
      lv_obj_center(s_qr);
      lv_qrcode_update(s_qr, s_payload, (uint32_t)strlen(s_payload));

#ifdef CD_FLAVOR_WATCH
      lv_obj_align(s_card, LV_ALIGN_CENTER, 0, 0);
      // One bottom line does double duty: countdown + tap-for-fresh-code.
      s_fresh = mk_label(s_scr, font_caption(), col_signed());
      lv_obj_align(s_fresh, LV_ALIGN_BOTTOM_MID, 0, -12);
      s_count = s_fresh;
#else
      lv_obj_align(s_card, LV_ALIGN_LEFT_MID, 60, 10);
      lv_obj_t* coach = mk_label(s_scr, font_body(), col_text());
      lv_label_set_text(coach,
                        "Power the canary near this\n"
                        "screen. Point its lens at the\n"
                        "code - a hand-width to two\n"
                        "away - and hold it steady.\n\n"
                        "Tilt the glass away from\n"
                        "lights if it glares. When the\n"
                        "canary joins, this screen\n"
                        "celebrates on its own.");
      lv_obj_set_style_text_line_space(coach, 6, 0);
      lv_obj_align(coach, LV_ALIGN_LEFT_MID, 60 + QR_PX + 60, -30);
      s_count = mk_label(s_scr, font_caption(), col_muted());
      lv_obj_align(s_count, LV_ALIGN_LEFT_MID, 60 + QR_PX + 60, 150);
      s_fresh = mk_label(s_scr, font_caption(), col_signed());
      lv_label_set_text(s_fresh, "new code");
      lv_obj_align(s_fresh, LV_ALIGN_BOTTOM_RIGHT, -28, -16);
#endif
      char c[40];
      fmt_countdown(c, sizeof(c), millis());
      lv_label_set_text(s_count, c);
      break;
    }

    case Face::NoWifi: {
      lv_obj_t* body = mk_label(s_scr, font_body(), col_text());
      lv_label_set_text(body,
                        "This glass needs its own\n"
                        "wifi first - finish setup,\n"
                        "then add canaries here.");
      lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(body, LV_ALIGN_CENTER, 0, 0);
      break;
    }

    case Face::TooLong: {
      lv_obj_t* body = mk_label(s_scr, font_body(), col_text());
      lv_label_set_text(body,
                        "Your wifi name + password\n"
                        "are too long for a code\n"
                        "this small - add canaries\n"
                        "from the wall panel.");
      lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(body, LV_ALIGN_CENTER, 0, 0);
      break;
    }

    case Face::Joined: {
      lv_obj_t* hero = mk_label(s_scr, font_title(), col_ok());
      lv_label_set_text(hero, "It's in the fleet");
      lv_obj_align(hero, LV_ALIGN_CENTER, 0, -18);
      lv_obj_t* sub = mk_label(s_scr, font_caption(), col_muted());
      lv_label_set_text(sub, "a new canary just joined");
      lv_obj_align(sub, LV_ALIGN_CENTER, 0, 16);
      break;
    }
  }
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────

void commission_ui_open() {
  if (s_scr) return;
  s_prev = lv_scr_act();
  s_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s_scr, col_bg(), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
  s_last_touch_ms = millis();
  s_joined_at_ms = 0;
  s_baseline_count = -1;  // tick() captures it once the hub link steadies
  s_link_up_since_ms = 0;

  if (canary::cfg::wifi_is_placeholder()) {
    s_face = Face::NoWifi;
    s_token[0] = '\0';
  } else if (!mint_payload()) {
    s_face = Face::TooLong;
    s_token[0] = '\0';
  } else {
    s_face = Face::Code;
  }
  build();
  lv_scr_load_anim(s_scr, LV_SCR_LOAD_ANIM_FADE_ON, MOTION_PAGE_MS, 0, false);
}

void commission_ui_close() {
  if (!s_scr) return;
  s_scr = nullptr;
  s_qr = s_card = s_count = s_close = s_fresh = nullptr;
  s_token[0] = '\0';
  // Never leave credentials lingering in a dead buffer.
  memset(s_payload, 0, sizeof(s_payload));
  lv_scr_load_anim(s_prev, LV_SCR_LOAD_ANIM_FADE_ON, MOTION_PAGE_MS, 0, true);
  s_prev = nullptr;
}

bool commission_ui_active() { return s_scr != nullptr; }

const char* commission_ui_token() { return s_scr ? s_token : ""; }

void commission_ui_handle_tap(int16_t x, int16_t y) {
  if (!s_scr) return;
  s_last_touch_ms = millis();
  auto hit = [&](lv_obj_t* o) {
    if (!o) return false;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return x >= a.x1 - HIT_PAD && x <= a.x2 + HIT_PAD && y >= a.y1 - HIT_PAD &&
           y <= a.y2 + HIT_PAD;
  };
  if (hit(s_close)) {
    commission_ui_close();
    return;
  }
  if (s_face == Face::Code && hit(s_fresh)) {
    if (mint_payload()) build();
    return;
  }
  if (s_face == Face::Joined) commission_ui_close();
}

void commission_ui_tick(uint32_t now_ms, int fleet_count, bool urgent) {
  if (!s_scr) return;
  if (urgent) {
    // A live alarm outranks onboarding, always.
    commission_ui_close();
    return;
  }
  // Baseline discipline: capture the count only after the hub link has
  // been continuously up for the settle window; drop it the moment the
  // link drops, so a reconnect's retained replay of old witnesses can
  // never masquerade as a fresh join (review catch).
  if (canary::net::mqtt_connected()) {
    if (s_link_up_since_ms == 0) s_link_up_since_ms = now_ms;
    if (s_baseline_count < 0 &&
        (int32_t)(now_ms - s_link_up_since_ms) >= (int32_t)BASELINE_SETTLE_MS)
      s_baseline_count = fleet_count;
  } else {
    s_link_up_since_ms = 0;
    s_baseline_count = -1;
  }

  if (s_face == Face::Joined) {
    if ((int32_t)(now_ms - s_joined_at_ms) >= (int32_t)JOINED_HOLD_MS)
      commission_ui_close();
    return;
  }

  // The moment of truth: a new witness appeared over a steady link while
  // this surface was open. Transport-agnostic on purpose — QR, phone wifi
  // code, or captive portal all end at the same celebration.
  if (s_face == Face::Code && s_baseline_count >= 0 &&
      fleet_count > s_baseline_count) {
    s_joined_at_ms = now_ms;
    set_face(Face::Joined);
    return;
  }

  if (s_face == Face::Code) {
    const uint32_t age_s = (now_ms - s_minted_ms) / 1000;  // seconds domain
    if (age_s >= TOKEN_TTL_S) {
      // Expired codes silently refresh — no error state to squint at. A
      // failed re-mint (creds changed under us to something unfittable)
      // closes rather than leaving a stale code up or retrying per-pass
      // (review catch).
      if (mint_payload()) {
        build();
      } else {
        commission_ui_close();
        return;
      }
    } else if (s_count) {
      char c[40];
      fmt_countdown(c, sizeof(c), now_ms);
      lv_label_set_text(s_count, c);
    }
  }

  if ((int32_t)(now_ms - s_last_touch_ms) >= (int32_t)IDLE_CLOSE_MS) {
    commission_ui_close();
  }
}

}  // namespace canary::ui
