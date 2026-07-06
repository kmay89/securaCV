#pragma once
#include <stdint.h>
#include "canary/fleet/fleet_model.h"

// Display color semantics — RGB565 versions of the EXACT palette the HA
// timeline card uses (custom_components/securacv/www/securacv-timeline-card.js),
// so a state means the same color on the wall as it does in the app:
//
//   green #43a047  ok / verified / presence-good
//   red   #e53935  warn / error / tamper / offline
//   blue  #03a9f4  signed-but-unverified
//   amber #fb8c00  attention / stale
//   grey           logged-only / muted
//
// Night palette is red-shifted and dim: blue-heavy light (~460-500 nm) is
// the melatonin-suppressing band, so anything that must stay visible in a
// bedroom renders in dim red/amber only (see display_ux_design.md §night).

namespace canary::ui {

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Day palette (timeline-card parity)
constexpr uint16_t COL_BG      = rgb565(0x00, 0x00, 0x00);
constexpr uint16_t COL_TEXT    = rgb565(0xE0, 0xE0, 0xE0);
constexpr uint16_t COL_MUTED   = rgb565(0x9E, 0x9E, 0x9E);
constexpr uint16_t COL_OK      = rgb565(0x43, 0xA0, 0x47);
constexpr uint16_t COL_WARN    = rgb565(0xFB, 0x8C, 0x00);
constexpr uint16_t COL_ALERT   = rgb565(0xE5, 0x39, 0x35);
constexpr uint16_t COL_SIGNED  = rgb565(0x03, 0xA9, 0xF4);
constexpr uint16_t COL_CARD    = rgb565(0x1A, 0x1A, 0x1A);
constexpr uint16_t COL_EDGE    = rgb565(0x33, 0x33, 0x33);

// Night palette — red-shifted, floor-dim. Color alone never carries the
// message (WCAG 1.4.1): severity is also position/shape/label, so losing
// hue at night loses nothing semantic.
constexpr uint16_t NCOL_TEXT   = rgb565(0x50, 0x18, 0x10);
constexpr uint16_t NCOL_MUTED  = rgb565(0x28, 0x0C, 0x08);
constexpr uint16_t NCOL_ALERT  = rgb565(0x90, 0x20, 0x18);

// Severity -> paint. Severity is also always rendered as a text label or
// glyph next to the color (colorblind-safe, ~8% of men can't split
// red/green dots).
inline uint16_t sev_color(canary::fleet::Sev s, bool night) {
  using canary::fleet::Sev;
  if (night) {
    return (s >= Sev::Alert) ? NCOL_ALERT : NCOL_MUTED;
  }
  switch (s) {
    case Sev::Ok:     return COL_OK;
    case Sev::Notice: return COL_OK;
    case Sev::Warn:   return COL_WARN;
    case Sev::Alert:  return COL_ALERT;
    case Sev::Tamper: return COL_ALERT;
  }
  return COL_MUTED;
}

// Trust badge -> paint (strong green tick ONLY for Verified — the display
// must not overclaim, same rule as the HA card).
inline uint16_t badge_color(canary::fleet::Badge b, bool night) {
  using canary::fleet::Badge;
  if (night) return NCOL_MUTED;
  switch (b) {
    case Badge::Verified: return COL_OK;
    case Badge::Signed:   return COL_SIGNED;
    case Badge::Failed:   return COL_ALERT;
    case Badge::Unsigned: return COL_MUTED;
    case Badge::Unknown:  return COL_EDGE;
  }
  return COL_EDGE;
}

inline const char* badge_glyph(canary::fleet::Badge b) {
  using canary::fleet::Badge;
  switch (b) {
    case Badge::Verified: return "OK";   // strong tick — earned only by Ed25519
    case Badge::Signed:   return "S";    // signed, key not pinned yet
    case Badge::Failed:   return "X";    // verify failed — loud
    case Badge::Unsigned: return "-";
    case Badge::Unknown:  return "?";
  }
  return "?";
}

// Compact "how long ago" for glance rows: 45s, 12m, 3h, 2d.
void format_age(uint32_t now_ms, uint32_t then_ms, char* out, int cap);

}  // namespace canary::ui
