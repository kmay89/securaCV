#include "canary/ui/theme.h"
#include "canary/ui/character.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

namespace canary::ui {

using canary::fleet::Badge;
using canary::fleet::Link;
using canary::fleet::Sev;

// ── Ground & type: the single Character choke point ──────────────────────
// Every ground/text tier, every font role, and (wave 3) every semantic
// color reads the ACTIVE Character here — no caller ever sees the table.
// Night outranks the Character at this exact spot: while the render tick
// holds character_set_night(true), the ground/tier accessors serve Quiet
// Glass's dark set for every look, so the one light Character (Almanac)
// can never glow in a bedroom. The ncol_* set stays with the night
// engine (theme.h) — Characters don't touch it.

namespace {
const Palette& pal() { return active_character_def().pal; }
const TypeLadder& ladder() { return active_character_def().type; }

// The uniform night floor = Quiet Glass's ground/tier bytes. Named here
// (not read from k_defs[0]) so a future re-skin of the default can't
// silently change what "dark at night" means.
constexpr uint32_t NIGHT_BG = 0x000000, NIGHT_SURFACE = 0x141414,
                   NIGHT_EDGE = 0x262626, NIGHT_TEXT = 0xEDEDED,
                   NIGHT_MUTED = 0x8A8A8A, NIGHT_FAINT = 0x4A4A4A;
// Night accent: a dim ember (= ncol_muted's hue) — decorative chrome may
// never reintroduce a blue-band glow after dark.
constexpr uint32_t NIGHT_ACCENT = 0x32100A;

lv_color_t ground(uint32_t day, uint32_t night_v) {
  return lv_color_hex(character_night() ? night_v : day);
}
}  // namespace

lv_color_t col_bg()      { return ground(pal().bg, NIGHT_BG); }
lv_color_t col_surface() { return ground(pal().surface, NIGHT_SURFACE); }
lv_color_t col_edge()    { return ground(pal().edge, NIGHT_EDGE); }
lv_color_t col_text()    { return ground(pal().text, NIGHT_TEXT); }
lv_color_t col_muted()   { return ground(pal().muted, NIGHT_MUTED); }
lv_color_t col_faint()   { return ground(pal().faint, NIGHT_FAINT); }
lv_color_t col_accent()  { return ground(pal().accent, NIGHT_ACCENT); }

// Semantics: canonical bytes on every dark ground, darkened-within-family
// on the light one — and at night always the canonical set, because the
// floor is dark for everyone (active_semantics() holds the policy).
lv_color_t col_ok()      { return lv_color_hex(active_semantics().ok); }
lv_color_t col_warn()    { return lv_color_hex(active_semantics().warn); }
lv_color_t col_alert()   { return lv_color_hex(active_semantics().alert); }
lv_color_t col_signed()  { return lv_color_hex(active_semantics().signed_); }

const lv_font_t* font_hero()    { return ladder().hero; }
const lv_font_t* font_title()   { return ladder().title; }
const lv_font_t* font_body()    { return ladder().body; }
const lv_font_t* font_label()   { return ladder().label; }
const lv_font_t* font_caption() { return ladder().caption; }
const lv_font_t* font_clock()   { return ladder().clock; }

lv_color_t sev_color(Sev s, bool night) {
  if (night) return (s >= Sev::Alert) ? ncol_alert() : ncol_muted();
  switch (s) {
    case Sev::Ok:
    case Sev::Notice: return col_ok();
    case Sev::Warn:   return col_warn();
    case Sev::Alert:
    case Sev::Tamper: return col_alert();
  }
  return col_muted();
}

lv_color_t badge_color(Badge b, bool night) {
  if (night) return ncol_muted();
  switch (b) {
    case Badge::Verified: return col_ok();
    case Badge::Signed:   return col_signed();
    case Badge::Failed:   return col_alert();
    case Badge::Unsigned: return col_muted();
    case Badge::Unknown:  return col_faint();
  }
  return col_faint();
}

// The strong word is earned, never assumed (same rule as the HA card).
const char* badge_text(Badge b) {
  switch (b) {
    case Badge::Verified: return LV_SYMBOL_OK " verified";
    // The bare check: the trust mark without the earned word. Signed means
    // a signature is present; verified means it checked out against the
    // pinned key — so verified is glyph + word and signed is the glyph
    // alone. The distinction rides the wording, never color alone
    // (WCAG 1.4.1), and the mark still reads as trust at a glance.
    case Badge::Signed:   return LV_SYMBOL_OK;
    // "proof failed" over bare "failed": the signature check failed, not
    // the device — the words should say which. "no proof" over
    // "unsigned": plain words for a device that simply doesn't sign.
    case Badge::Failed:   return LV_SYMBOL_CLOSE " proof failed";
    case Badge::Unsigned: return "no proof";
    case Badge::Unknown:  return "-";
  }
  return "-";
}

const char* link_label(Link l) {
  switch (l) {
    case Link::Online:  return "online";
    // "late" over "stale": a missed check-in reads as running late, not
    // as spoiled data. Pairs with the transparency page's "check-ins".
    case Link::Stale:   return "late";
    case Link::Lost:    return "lost";
    case Link::Offline: return "offline";
    default:            return "-";
  }
}

const char* signal_word(int dbm) {
  // Same bands the setup portal's signal bars use (-60 / -72). Words, not
  // dBm: a weak-signal label tells a person to move the canary; a raw
  // negative decibel number tells them nothing.
  if (dbm > -60) return "strong signal";
  if (dbm > -72) return "ok signal";
  return "weak signal";
}

void format_age(uint32_t now_ms, uint32_t then_ms, char* out, int cap) {
  if (!out || cap <= 0) return;
  const int32_t d = (int32_t)(now_ms - then_ms);
  const uint32_t s = d > 0 ? (uint32_t)d / 1000UL : 0;
  if (s < 60)            snprintf(out, cap, "%lus", (unsigned long)s);
  else if (s < 3600)     snprintf(out, cap, "%lum", (unsigned long)(s / 60));
  else if (s < 86400)    snprintf(out, cap, "%luh", (unsigned long)(s / 3600));
  else                   snprintf(out, cap, "%lud", (unsigned long)(s / 86400));
}

namespace {

struct EventCopy {
  const char* wire;
  const char* human;
};

// Curated copy for the fleet's known vocabulary. Written from the user's
// side of the screen: what happened, not how the system classified it.
constexpr EventCopy EVENT_COPY[] = {
    {"presence_detected",              "Presence detected"},
    {"presence_cleared",               "All clear"},
    {"occupancy_changed",              "Occupancy changed"},
    {"presence_in_restricted_zone",    "Person in restricted zone"},
    {"vehicle_presence_after_hours",   "Vehicle after hours"},
    {"boundary_crossing_object_large", "Vehicle crossed boundary"},
    {"boundary_crossing_object_small", "Animal crossed boundary"},
    {"contact_state_change",           "Door or window changed"},
    {"object_removed_from_zone",       "Object removed"},
    {"acoustic_impulse_in_zone",       "Loud sound"},
    {"smoke_alarm_t3",                 "Smoke-alarm pattern heard"},
    {"co_alarm_t4",                    "CO-alarm pattern heard"},
    {"glass_break",                    "Glass break heard"},
    {"knock",                          "Knock heard"},
    {"doorbell",                       "Doorbell heard"},
    {"silent_panic",                   "Silent panic"},
    {"enclosure_tamper",               "Enclosure tampered"},
    {"camera_tamper",                  "Camera tampered"},
    {"temp_drift",                     "Temperature tamper"},
    {"chain_verify_failed",            "Chain verification FAILED"},
    {"boot",                           "Started up"},
};

}  // namespace

const char* humanize_event(const char* wire, char* buf, size_t cap) {
  if (!buf || cap == 0) return "";
  if (!wire || !wire[0]) { buf[0] = '\0'; return buf; }

  for (const auto& e : EVENT_COPY) {
    if (strcmp(e.wire, wire) == 0) {
      snprintf(buf, cap, "%s", e.human);
      return buf;
    }
  }

  // Unknown vocabulary: de-underscore + sentence case. The glass never
  // shows raw wire text, and never crashes on words it hasn't met.
  size_t o = 0;
  for (size_t i = 0; wire[i] && o + 1 < cap; i++) {
    char c = wire[i];
    if (c == '_' || c == '.') c = ' ';
    if (o == 0) c = (char)toupper((unsigned char)c);
    buf[o++] = c;
  }
  buf[o] = '\0';
  return buf;
}

lv_obj_t* mk_qrcode(lv_obj_t* parent, int32_t size_px) {
#if LVGL_VERSION_MAJOR >= 9
  // v9 split creation into setters (and renders on update).
  lv_obj_t* qr = lv_qrcode_create(parent);
  lv_qrcode_set_size(qr, size_px);
  lv_qrcode_set_dark_color(qr, lv_color_black());
  lv_qrcode_set_light_color(qr, lv_color_white());
  return qr;
#else
  return lv_qrcode_create(parent, size_px, lv_color_black(),
                          lv_color_white());
#endif
}

}  // namespace canary::ui
