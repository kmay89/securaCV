#pragma once
#include <stdint.h>
#include <stddef.h>
#include <lvgl.h>
#include "canary/fleet/fleet_model.h"

// "Quiet Glass" design tokens (display_ux_design.md §Design language).
//
// Semantic hues are the HA timeline-card palette — a state means the same
// color on the wall as in the app. Everything else is a disciplined dark
// ground: true black (bezels disappear, night floors go lower), #141414
// surfaces, hairline #262626 edges, and glow-not-stripes for emphasis.
// Color never carries meaning alone (WCAG 1.4.1): every state also has a
// label, glyph, or position.

namespace canary::ui {

// ── Ground ───────────────────────────────────────────────────────────────
inline lv_color_t col_bg()      { return lv_color_hex(0x000000); }
inline lv_color_t col_surface() { return lv_color_hex(0x141414); }
inline lv_color_t col_edge()    { return lv_color_hex(0x262626); }
inline lv_color_t col_text()    { return lv_color_hex(0xEDEDED); }
inline lv_color_t col_muted()   { return lv_color_hex(0x8A8A8A); }
inline lv_color_t col_faint()   { return lv_color_hex(0x4A4A4A); }

// ── Semantics (timeline-card parity) ─────────────────────────────────────
inline lv_color_t col_ok()      { return lv_color_hex(0x43A047); }
inline lv_color_t col_warn()    { return lv_color_hex(0xFB8C00); }
inline lv_color_t col_alert()   { return lv_color_hex(0xE53935); }
inline lv_color_t col_signed()  { return lv_color_hex(0x03A9F4); }

// ── Night (red-shifted, melatonin-band-free; see UX doc §night) ─────────
inline lv_color_t ncol_text()   { return lv_color_hex(0x5A1C12); }
inline lv_color_t ncol_muted()  { return lv_color_hex(0x32100A); }
inline lv_color_t ncol_alert()  { return lv_color_hex(0x992219); }

lv_color_t sev_color(canary::fleet::Sev s, bool night);
lv_color_t badge_color(canary::fleet::Badge b, bool night);
const char* badge_text(canary::fleet::Badge b);   // "verified"/"signed"/...
const char* link_label(canary::fleet::Link l);

// ── Type scale (Montserrat AA; roles, not sizes, in calling code) ───────
inline const lv_font_t* font_hero()    { return &lv_font_montserrat_48; }
inline const lv_font_t* font_title()   { return &lv_font_montserrat_28; }
inline const lv_font_t* font_body()    { return &lv_font_montserrat_16; }
inline const lv_font_t* font_label()   { return &lv_font_montserrat_14; }
inline const lv_font_t* font_caption() { return &lv_font_montserrat_12; }
inline const lv_font_t* font_clock()   { return &lv_font_montserrat_20; }

// ── Motion budget (calm tech: rationed, purposeful) ──────────────────────
constexpr uint32_t MOTION_PAGE_MS   = 220;   // page/screen fades, ease-out
constexpr uint32_t MOTION_BREATH_MS = 2000;  // unacked-alert breathing, ONLY
constexpr uint32_t MOTION_ACK_MS    = 900;   // hold-to-ack ring sweep (= CD_LONGPRESS_MS)

// ── Language ─────────────────────────────────────────────────────────────

// Compact "how long ago": 45s, 12m, 3h, 2d.
void format_age(uint32_t now_ms, uint32_t then_ms, char* out, int cap);

// Wire vocabulary -> human words: "presence_in_restricted_zone" ->
// "Person in restricted zone". Known events get curated copy; anything
// unknown degrades to de-underscored sentence case, never raw wire text.
const char* humanize_event(const char* wire, char* buf, size_t cap);

}  // namespace canary::ui
