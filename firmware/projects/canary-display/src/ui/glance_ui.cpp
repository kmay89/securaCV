// src/ui/glance_ui.cpp — the Watch Station's round glance face.
//
// Design contract (display_ux_design.md): readable from across a dark room
// in under a second — center = the one thing that matters (worst state),
// ring = one arc per witness, everything else lives a tap away. Color never
// carries meaning alone: every state also has a label or glyph.
#include <config.h>
#ifdef CD_FLAVOR_WATCH

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

#include "canary/ui/glance_ui.h"
#include "canary/ui/theme.h"

namespace canary::ui {

using canary::fleet::Badge;
using canary::fleet::Fleet;
using canary::fleet::Link;
using canary::fleet::Sev;
using canary::fleet::Witness;

namespace {

constexpr int16_t CX = 120, CY = 120;
constexpr int16_t RING_R_OUT = 118;
constexpr int16_t RING_R_IN  = 104;

// Cheap thick-arc: fillArc exists in Arduino_GFX, but stepping small filled
// circles along the arc keeps us off the deprecated/renamed corners of the
// library API and looks fine at ring width 14.
void ring_segment(Arduino_GFX* g, float a0, float a1, uint16_t color) {
  const float rmid = (RING_R_OUT + RING_R_IN) / 2.0f;
  const float rdot = (RING_R_OUT - RING_R_IN) / 2.0f;
  const float step = 3.0f / rmid * 57.2958f;  // ~3 px along the arc, in degrees
  for (float a = a0; a <= a1; a += step) {
    const float rad = a * 0.0174533f;
    g->fillCircle(CX + (int16_t)(rmid * cosf(rad)),
                  CY + (int16_t)(rmid * sinf(rad)),
                  (int16_t)rdot, color);
  }
}

void text_centered(Arduino_GFX* g, int16_t y, uint8_t size, uint16_t color,
                   const char* s) {
  const int16_t w = (int16_t)(strlen(s) * 6 * size - size);
  g->setTextSize(size);
  g->setTextColor(color);
  g->setCursor(CX - w / 2, y);
  g->print(s);
}

const char* link_label(Link l) {
  switch (l) {
    case Link::Online:  return "online";
    case Link::Stale:   return "stale";
    case Link::Lost:    return "lost";
    case Link::Offline: return "offline";
    default:            return "?";
  }
}

void render_overview(Arduino_GFX* g, const Fleet& fleet, uint32_t now,
                     const GlanceState& st) {
  const int n = fleet.count();
  const Sev worst = fleet.worst(now);
  const uint16_t center_col = sev_color(worst, st.night);

  // Witness ring: one segment per device, gap between segments.
  if (n > 0) {
    const float span = 360.0f / n;
    for (int i = 0; i < n; i++) {
      const Witness* w = fleet.at(i);
      if (!w) continue;
      const Sev s = fleet.witness_sev(*w, now);
      const float a0 = -90.0f + i * span + 3.0f;
      const float a1 = -90.0f + (i + 1) * span - 3.0f;
      ring_segment(g, a0, a1, sev_color(s, st.night));
    }
  } else {
    ring_segment(g, -90.0f, 269.0f, st.night ? NCOL_MUTED : COL_EDGE);
  }

  // Center: the one thing that matters.
  const uint16_t text_col = st.night ? NCOL_TEXT : COL_TEXT;
  if (n == 0) {
    text_centered(g, 96, 2, text_col, "NO");
    text_centered(g, 120, 2, text_col, "CANARIES");
    text_centered(g, 150, 1, st.night ? NCOL_MUTED : COL_MUTED,
                  st.mqtt_ok ? "listening..." : (st.wifi_ok ? "no broker" : "no wifi"));
  } else if (worst <= Sev::Notice) {
    g->fillCircle(CX, CY - 26, 22, center_col);
    // check mark
    g->drawLine(CX - 9, CY - 26, CX - 3, CY - 19, COL_BG);
    g->drawLine(CX - 3, CY - 19, CX + 10, CY - 34, COL_BG);
    text_centered(g, 116, 2, text_col, "ALL QUIET");
    char buf[24];
    snprintf(buf, sizeof(buf), "%d %s", n, n == 1 ? "canary" : "canaries");
    text_centered(g, 140, 1, st.night ? NCOL_MUTED : COL_MUTED, buf);
    if (fleet.all_verified())
      text_centered(g, 156, 1, st.night ? NCOL_MUTED : COL_OK, "verified");
  } else {
    g->fillCircle(CX, CY - 26, 22, center_col);
    text_centered(g, CY - 33, 2, COL_BG, "!");
    char label[16];
    snprintf(label, sizeof(label), "%s", canary::fleet::sev_name(worst));
    for (char* p = label; *p; p++) *p = (char)toupper(*p);
    text_centered(g, 116, 2, text_col, label);
    // Which witness demands the look (worst first hit)
    for (int i = 0; i < n; i++) {
      const Witness* w = fleet.at(i);
      if (w && fleet.witness_sev(*w, now) == worst) {
        char row[26];
        snprintf(row, sizeof(row), "%.20s", w->id);
        text_centered(g, 140, 1, st.night ? NCOL_MUTED : COL_MUTED, row);
        break;
      }
    }
    if (st.acked)
      text_centered(g, 156, 1, st.night ? NCOL_MUTED : COL_MUTED, "acked");
  }

  // Footer: clock + link chips (position carries meaning too, not just hue).
  if (st.time_valid) {
    char clk[8];
    snprintf(clk, sizeof(clk), "%02d:%02d", st.clock_hh, st.clock_mm);
    text_centered(g, 176, 2, st.night ? NCOL_MUTED : COL_MUTED, clk);
  }
  if (!st.wifi_ok)
    text_centered(g, 200, 1, st.night ? NCOL_ALERT : COL_ALERT, "wifi down");
  else if (!st.mqtt_ok)
    text_centered(g, 200, 1, st.night ? NCOL_ALERT : COL_WARN, "broker down");
}

void render_device(Arduino_GFX* g, const Fleet& fleet, uint32_t now,
                   const GlanceState& st, int idx) {
  const Witness* w = fleet.at(idx);
  if (!w) return;
  const Sev s = fleet.witness_sev(*w, now);
  const uint16_t text_col = st.night ? NCOL_TEXT : COL_TEXT;
  const uint16_t mute_col = st.night ? NCOL_MUTED : COL_MUTED;

  ring_segment(g, -90.0f, 269.0f, sev_color(s, st.night));

  char buf[40];
  snprintf(buf, sizeof(buf), "%.18s", w->id);
  text_centered(g, 62, 1, mute_col, buf);
  snprintf(buf, sizeof(buf), "%.14s", w->device_type[0] ? w->device_type : "canary");
  text_centered(g, 78, 2, text_col, buf);

  snprintf(buf, sizeof(buf), "%s", link_label(w->link));
  text_centered(g, 104, 2, sev_color(s, st.night), buf);

  if (w->has_event) {
    char age[8];
    format_age(now, w->last_event_ms, age, sizeof(age));
    snprintf(buf, sizeof(buf), "%.20s", w->last_event);
    text_centered(g, 130, 1, text_col, buf);
    snprintf(buf, sizeof(buf), "%s ago", age);
    text_centered(g, 144, 1, mute_col, buf);
  } else {
    text_centered(g, 134, 1, mute_col, "no events yet");
  }

  // Trust badge — strong tick only when Ed25519 verify passed on-device.
  snprintf(buf, sizeof(buf), "chain %s", badge_glyph(w->badge));
  text_centered(g, 164, 1, badge_color(w->badge, st.night), buf);

  if (w->battery_present && w->battery_pct >= 0) {
    snprintf(buf, sizeof(buf), "batt %d%%", (int)w->battery_pct);
    text_centered(g, 180, 1,
                  w->battery_pct < 25 ? sev_color(Sev::Warn, st.night) : mute_col,
                  buf);
  }

  snprintf(buf, sizeof(buf), "%d/%d", idx + 1, fleet.count());
  text_centered(g, 204, 1, mute_col, buf);
}

void render_events(Arduino_GFX* g, const Fleet& fleet, uint32_t now,
                   const GlanceState& st) {
  const uint16_t text_col = st.night ? NCOL_TEXT : COL_TEXT;
  const uint16_t mute_col = st.night ? NCOL_MUTED : COL_MUTED;
  text_centered(g, 42, 1, mute_col, "RECENT EVENTS");

  const int n = fleet.events_count();
  if (n == 0) {
    text_centered(g, 116, 1, mute_col, "nothing witnessed");
    return;
  }
  int16_t y = 66;
  for (int i = 0; i < n && i < 5; i++) {
    const auto* e = fleet.event_at(i);
    if (!e) break;
    char age[8];
    format_age(now, e->at_ms, age, sizeof(age));
    char row[34];
    snprintf(row, sizeof(row), "%-4s %.22s", age, e->name);
    g->setTextSize(1);
    g->setTextColor(e->sev >= Sev::Warn ? sev_color(e->sev, st.night) : text_col);
    g->setCursor(34, y);
    g->print(row);
    snprintf(row, sizeof(row), "     %.14s%s", e->device,
             e->signed_flag ? " *" : "");
    g->setTextColor(mute_col);
    g->setCursor(34, y + 11);
    g->print(row);
    y += 28;
  }
  text_centered(g, 214, 1, mute_col, "* signed");
}

}  // namespace

int glance_page_count() {
  return 2 + canary::fleet::the_fleet().count();  // overview + devices + events
}

void glance_render(Arduino_GFX* g, const Fleet& fleet, uint32_t now,
                   const GlanceState& st) {
  g->fillScreen(COL_BG);
  const int devices = fleet.count();
  int page = st.page;
  const int pages = 2 + devices;
  if (page >= pages) page = 0;

  if (page == 0)                render_overview(g, fleet, now, st);
  else if (page <= devices)     render_device(g, fleet, now, st, page - 1);
  else                          render_events(g, fleet, now, st);
}

}  // namespace canary::ui

#endif  // CD_FLAVOR_WATCH
