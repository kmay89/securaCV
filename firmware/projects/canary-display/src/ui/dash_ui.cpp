// src/ui/dash_ui.cpp — the Dash's 800x480 wall/desk face.
//
// Layout: 56 px header (fleet state sentence + clock), witness card grid on
// the left (2 x 4 cards of 250x94), event timeline column on the right
// (280 px). Same severity/trust vocabulary as the watch and the HA card.
#include <config.h>
#ifdef CD_FLAVOR_DASH

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "canary/ui/dash_ui.h"
#include "canary/ui/theme.h"

namespace canary::ui {

using canary::fleet::Fleet;
using canary::fleet::Link;
using canary::fleet::Sev;
using canary::fleet::Witness;

namespace {

constexpr int16_t W = 800, H = 480;
constexpr int16_t HDR_H = 56;
constexpr int16_t TL_W = 280;                 // timeline column
constexpr int16_t GRID_X = 8, GRID_Y = HDR_H + 8;
constexpr int16_t CARD_W = 250, CARD_H = 94, CARD_GAP = 8;
constexpr int GRID_COLS = 2, GRID_ROWS = 4;   // 8 cards; beyond that "+N more"

const char* link_label(Link l) {
  switch (l) {
    case Link::Online:  return "online";
    case Link::Stale:   return "stale";
    case Link::Lost:    return "lost";
    case Link::Offline: return "offline";
    default:            return "?";
  }
}

void header(Arduino_GFX* g, const Fleet& fleet, uint32_t now, const DashState& st) {
  const Sev worst = fleet.worst(now);
  g->fillRect(0, 0, W, HDR_H, st.night ? COL_BG : COL_CARD);
  g->fillRect(0, HDR_H - 4, W, 4, sev_color(worst, st.night));

  g->setTextSize(3);
  g->setTextColor(st.night ? NCOL_TEXT : COL_TEXT);
  g->setCursor(16, 16);
  const int n = fleet.count();
  char line[64];
  if (n == 0) {
    snprintf(line, sizeof(line), "No canaries %s",
             st.mqtt_ok ? "yet - listening" : (st.wifi_ok ? "- broker down" : "- wifi down"));
  } else if (worst <= Sev::Notice) {
    snprintf(line, sizeof(line), "All quiet - %d %s%s", n,
             n == 1 ? "canary" : "canaries",
             fleet.all_verified() ? " - verified" : "");
  } else {
    char sev[12];
    snprintf(sev, sizeof(sev), "%s", canary::fleet::sev_name(worst));
    for (char* p = sev; *p; p++) *p = (char)toupper(*p);
    snprintf(line, sizeof(line), "%s%s - check the grid", sev,
             st.acked ? " (acked)" : "");
  }
  g->print(line);

  if (st.time_valid) {
    char clk[8];
    snprintf(clk, sizeof(clk), "%02d:%02d", st.clock_hh, st.clock_mm);
    g->setCursor(W - 16 - 5 * 18, 16);
    g->setTextColor(st.night ? NCOL_MUTED : COL_MUTED);
    g->print(clk);
  }
}

void card(Arduino_GFX* g, const Fleet& fleet, const Witness& w, uint32_t now,
          const DashState& st, int16_t x, int16_t y) {
  const Sev s = fleet.witness_sev(w, now);
  const uint16_t edge = sev_color(s, st.night);
  g->fillRect(x, y, CARD_W, CARD_H, st.night ? COL_BG : COL_CARD);
  g->drawRect(x, y, CARD_W, CARD_H, st.night ? NCOL_MUTED : COL_EDGE);
  g->fillRect(x, y, 6, CARD_H, edge);   // severity spine (position + color)

  char buf[40];
  g->setTextSize(2);
  g->setTextColor(st.night ? NCOL_TEXT : COL_TEXT);
  g->setCursor(x + 14, y + 8);
  snprintf(buf, sizeof(buf), "%.18s", w.id);
  g->print(buf);

  g->setTextSize(1);
  g->setTextColor(edge);
  g->setCursor(x + 14, y + 30);
  snprintf(buf, sizeof(buf), "%s", link_label(w.link));
  g->print(buf);

  // Trust badge, right-aligned on the state row.
  g->setTextColor(badge_color(w.badge, st.night));
  g->setCursor(x + CARD_W - 60, y + 30);
  snprintf(buf, sizeof(buf), "ch %s", badge_glyph(w.badge));
  g->print(buf);

  g->setTextColor(st.night ? NCOL_MUTED : COL_MUTED);
  g->setCursor(x + 14, y + 46);
  if (w.has_event) {
    char age[8];
    format_age(now, w.last_event_ms, age, sizeof(age));
    snprintf(buf, sizeof(buf), "%.22s  %s", w.last_event, age);
  } else {
    snprintf(buf, sizeof(buf), "no events yet");
  }
  g->print(buf);

  g->setCursor(x + 14, y + 62);
  if (w.battery_present && w.battery_pct >= 0) {
    snprintf(buf, sizeof(buf), "batt %d%%  %.10s", (int)w.battery_pct, w.fw);
    g->setTextColor(w.battery_pct < 25 ? sev_color(Sev::Warn, st.night)
                                       : (st.night ? NCOL_MUTED : COL_MUTED));
  } else {
    snprintf(buf, sizeof(buf), "%.14s", w.fw[0] ? w.fw : "");
  }
  g->print(buf);

  if (w.tamper) {
    g->setTextSize(2);
    g->setTextColor(sev_color(Sev::Tamper, st.night));
    g->setCursor(x + 14, y + 72);
    g->print("TAMPER");
  }
}

void grid(Arduino_GFX* g, const Fleet& fleet, uint32_t now, const DashState& st) {
  const int n = fleet.count();
  const int max_cards = GRID_COLS * GRID_ROWS;
  for (int i = 0; i < n && i < max_cards; i++) {
    const Witness* w = fleet.at(i);
    if (!w) break;
    const int col = i % GRID_COLS, row = i / GRID_COLS;
    card(g, fleet, *w, now, st,
         GRID_X + col * (CARD_W + CARD_GAP),
         GRID_Y + row * (CARD_H + CARD_GAP));
  }
  if (n > max_cards) {
    char buf[24];
    snprintf(buf, sizeof(buf), "+%d more", n - max_cards);
    g->setTextSize(2);
    g->setTextColor(st.night ? NCOL_MUTED : COL_MUTED);
    g->setCursor(GRID_X + 8, GRID_Y + GRID_ROWS * (CARD_H + CARD_GAP) + 2);
    g->print(buf);
  }
  if (n == 0) {
    g->setTextSize(2);
    g->setTextColor(st.night ? NCOL_MUTED : COL_MUTED);
    g->setCursor(GRID_X + 24, GRID_Y + 32);
    g->print("Waiting for witnesses...");
    g->setTextSize(1);
    g->setCursor(GRID_X + 24, GRID_Y + 64);
    g->print("Canaries publishing to this broker appear here.");
  }
}

void timeline(Arduino_GFX* g, const Fleet& fleet, uint32_t now, const DashState& st) {
  const int16_t x0 = W - TL_W;
  g->drawLine(x0 - 4, HDR_H, x0 - 4, H, st.night ? NCOL_MUTED : COL_EDGE);
  g->setTextSize(1);
  g->setTextColor(st.night ? NCOL_MUTED : COL_MUTED);
  g->setCursor(x0 + 8, HDR_H + 10);
  g->print("EVENTS  (* signed)");

  const int n = fleet.events_count();
  int16_t y = HDR_H + 30;
  for (int i = 0; i < n && y < H - 40; i++) {
    const auto* e = fleet.event_at(i);
    if (!e) break;
    char age[8];
    format_age(now, e->at_ms, age, sizeof(age));

    g->fillCircle(x0 + 12, y + 6, 4, sev_color(e->sev, st.night));
    char row[44];
    g->setTextSize(2);
    g->setTextColor(e->sev >= Sev::Warn ? sev_color(e->sev, st.night)
                                        : (st.night ? NCOL_TEXT : COL_TEXT));
    g->setCursor(x0 + 24, y);
    snprintf(row, sizeof(row), "%.19s", e->name);
    g->print(row);

    g->setTextSize(1);
    g->setTextColor(st.night ? NCOL_MUTED : COL_MUTED);
    g->setCursor(x0 + 24, y + 18);
    snprintf(row, sizeof(row), "%s ago  %.14s%s", age, e->device,
             e->signed_flag ? " *" : "");
    g->print(row);
    y += 38;
  }
  if (n == 0) {
    g->setCursor(x0 + 24, HDR_H + 36);
    g->print("nothing witnessed yet");
  }
}

}  // namespace

void dash_render(Arduino_GFX* g, const Fleet& fleet, uint32_t now,
                 const DashState& st) {
  g->fillScreen(COL_BG);
  header(g, fleet, now, st);
  grid(g, fleet, now, st);
  timeline(g, fleet, now, st);

  // Footer strip: link state + honesty line. An informational display, not
  // an alarm — the glass itself says so (see the regulatory notes in the
  // UX doc).
  g->setTextSize(1);
  g->setTextColor(st.night ? NCOL_MUTED : COL_MUTED);
  g->setCursor(GRID_X + 2, H - 14);
  if (!st.wifi_ok)      g->print("WIFI DOWN - showing last known state");
  else if (!st.mqtt_ok) g->print("BROKER DOWN - showing last known state");
  else                  g->print("status display - not a life-safety device");
}

}  // namespace canary::ui

#endif  // CD_FLAVOR_DASH
