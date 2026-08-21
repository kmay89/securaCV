#pragma once
#include <config.h>   // CD_FLAVOR_* (Character type ladders are per-flavor)
#include <stdint.h>
#include <stddef.h>
#include <lvgl.h>
#include "canary/fleet/fleet_model.h"
#include "canary/ui/character.h"

// "Quiet Glass" design tokens (display_ux_design.md §Design language).
//
// Semantic hues are the HA timeline-card palette — a state means the same
// color on the wall as in the app. Everything else is a disciplined dark
// ground: true black (bezels disappear, night floors go lower), #141414
// surfaces, hairline #262626 edges, and glow-not-stripes for emphasis.
// Color never carries meaning alone (WCAG 1.4.1): every state also has a
// label, glyph, or position.
//
// Character wave (display_character.md): Quiet Glass is now the DEFAULT
// look, not the only one. The ground/text tiers and the type ladder read
// the active Character through this single choke point. Wave 3 (the
// light-ground Almanac) evolved the two hard lines without breaking
// their meaning:
//  - Semantics: same FAMILY, same meaning, everywhere. Every dark
//    Character serves the canonical timeline-card bytes; a light ground
//    darkens them within family (measured — canonical amber is 1.98:1 on
//    paper, and an invisible alarm color is the dishonest option). The
//    table lives in character.cpp; no Character may re-hue an alarm.
//  - Night: a MODE that outranks every Character. While the render tick
//    holds character_set_night(true), the ground/tier accessors serve
//    Quiet Glass's dark set for EVERY look — a cream glass never glows
//    at 3 a.m. The ncol_* palette below stays with the night engine.

namespace canary::ui {

// ── Ground (the active Character's palette; night forces the dark set) ──
lv_color_t col_bg();
lv_color_t col_surface();
lv_color_t col_edge();
lv_color_t col_text();
lv_color_t col_muted();
lv_color_t col_faint();
// The Character's one decorative chrome hue (10% budget; never semantic).
// At night this returns a dim ember — never a blue-band accent.
lv_color_t col_accent();

// ── Semantics (same family everywhere; light grounds darken in-family) ──
lv_color_t col_ok();
lv_color_t col_warn();
lv_color_t col_alert();
lv_color_t col_signed();

// ── Night (red-shifted, melatonin-band-free; see UX doc §night) ─────────
// The night engine outranks every Character — these never restyle.
inline lv_color_t ncol_text()   { return lv_color_hex(0x5A1C12); }
inline lv_color_t ncol_muted()  { return lv_color_hex(0x32100A); }
inline lv_color_t ncol_alert()  { return lv_color_hex(0x992219); }
// The clock hero at night, and ONLY the hero — the one thing a bedside
// glass exists to answer at 3 a.m. The chrome around it stays on ncol_muted
// so the hierarchy is the point rather than an overall brightness.
//
// It is a fully saturated red where ncol_text is a dim brick, and that reads
// as a change in kind: at the night backlight duty the old value landed as a
// muddy brown a sleepy eye had to resolve, which is the opposite of glanceable.
// This is NOT a relaxation of the night rule — it is a stricter reading of it.
// Melatonin suppression is driven by the short wavelengths, so the honest
// measure is the green and blue channels, and this value spends LESS there
// (0x14 / 0x0C against 0x1C / 0x12) while spending its whole budget on the
// long wavelength that costs nothing. A classic red alarm clock, arrived at
// by making the red redder rather than by making the screen lighter.
inline lv_color_t ncol_clock()  { return lv_color_hex(0xFF140C); }

lv_color_t sev_color(canary::fleet::Sev s, bool night);
lv_color_t badge_color(canary::fleet::Badge b, bool night);
const char* badge_text(canary::fleet::Badge b);   // "verified"/"signed"/...
const char* link_label(canary::fleet::Link l);
const char* signal_word(int dbm);   // "strong/ok/weak signal" — never dBm

// ── Type scale (Montserrat AA; roles, not sizes, in calling code) ───────
// Per-flavor: the watch is a 1.28" panel read at arm's length; the dash is
// a 4.3" panel read from across a room — same pixel sizes rendered both
// (bench finding: dash text visibly undersized), so the dash scale runs
// ~1.3-1.4x larger per role. Enabled-but-unreferenced Montserrat sizes
// cost nothing (the linker drops unused font tables). The ladder itself
// now comes from the active Character (character.cpp holds the per-flavor
// tables; Heirloom steps one enabled size up).
const lv_font_t* font_hero();
const lv_font_t* font_title();
const lv_font_t* font_body();
const lv_font_t* font_label();
const lv_font_t* font_caption();
const lv_font_t* font_clock();

// ── Motion budget (calm tech: rationed, purposeful) ──────────────────────
// These three are the historic fixed-duration motions. Everything newer
// (tiered durations, effect-class gates, the veil, the governor) lives in
// the motion engine — ui/motion.h — which also enforces the ration
// mechanically (docs/hardware/display_motion_engine.md).
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

// ── Widgets ──────────────────────────────────────────────────────────────

// QR code, dark-on-light (scanners want it), sized in pixels. One helper
// because LVGL split the creation API with its 9.x major (v8 takes
// size+colors in create; v9 takes setters) — every proof/join QR on the
// glass goes through here so the version gate lives in exactly one place.
lv_obj_t* mk_qrcode(lv_obj_t* parent, int32_t size_px);

}  // namespace canary::ui
