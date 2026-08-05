/*
 * SecuraCV — the fleet view (host-testable, no Arduino).
 *
 * A framed ASCII panel that answers "which OTHER Canaries can this one hear,
 * and how are they doing" — the console face of the self-* roadmap's Fleet map
 * (docs/design/self_star_roadmap.md, TODO 1). It renders the shared, pure fleet
 * roster (firmware/common/fleet_link/fleet_roster.h) — the device's own view of
 * the presence beacons it has picked up over BLE — as a width-aligned table:
 * each peer's short fingerprint, how long ago it was last heard, its
 * self-reported health/battery, its chain height, and a worded status.
 *
 * HONESTY POSTURE. Like the roster and the beacon it consumes, this is UNSIGNED
 * presence: liveness and self-reported status, NEVER a verified trust claim. The
 * card says so out loud, and every attention state that gets a color also gets
 * a word (WCAG 1.4.1) so it reads the same on a monochrome terminal.
 *
 * PURE render layer, landed ahead of its device wiring — the same pattern as
 * boot_policy.h (PR #1085): the format that could silently drift lives here in a
 * host-tested header, so CI pins the exact bytes, and the console command that
 * calls it (a read-only `n`/"nearby" Tier::Diag command in the canary firmware)
 * plus the manifest `fleet[]` field are a thin follow-up that only has to
 * snapshot the roster into FleetPeer[] and hand it over. Composed from the same
 * engine as the trust card (console_theme.h), so it aligns with — and is as
 * ASCII-floor-safe as — every other scene (see console_theme.h for the tiering
 * rule). Compiles hosted for tests_host with -Wall -Wextra -Werror.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */
#ifndef SECURACV_FLEET_VIEW_H
#define SECURACV_FLEET_VIEW_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui/console_theme.h"
#include "fleet_link/fleet_beacon.h"  // FLEET_BEACON_FLAG_* — single-source the bits
#include "fleet_link/fleet_roster.h"  // FleetRosterEntry — the conversion source

namespace scene {

// Interior columns; total visible width FLEET_INNER + 2 = 54, matching the
// trust card (console_scenes.h TRUST_INNER) so the two panels align in a session.
static const int FLEET_INNER = 52;

// One peer row, decoupled from FleetRoster so the renderer is pure and
// host-testable. The device wiring fills these from the shared roster
// (firmware/common/fleet_link/fleet_roster.h): fp4 = entry.fp4, age_s =
// (now_ms - entry.last_seen_ms) / 1000, health/battery/chain/flags straight off
// the entry. rssi is carried for completeness but not shown — it keeps the card
// narrow, and dBm is noise to a human reading a roster.
struct FleetPeer {
  const char* fp4;         // 4-hex short fingerprint — the peer id/key
  uint32_t    age_s;       // seconds since last heard
  int16_t     health_pct;  // -1 = unknown
  int16_t     battery_pct; // -1 = unknown
  uint16_t    chain_lo;    // low 16 bits of the peer's chain height
  uint8_t     flags;       // FLEET_BEACON_FLAG_* bits
  int8_t      rssi;        // dBm; 0 = unknown (reserved; not rendered)
};

struct FleetView {
  const char*      self_id;   // this Canary's device id, e.g. "canary-7fA3"
  const char*      self_fp4;  // this Canary's own 4-hex fp (may be null)
  const FleetPeer* peers;     // heard peers (may be null iff count == 0)
  size_t           count;     // number of peers heard
  size_t           capacity;  // roster capacity (FLEET_ROSTER_MAX) for "n of cap"
};

// Project a raw roster entry onto a view peer at time `now_ms`. Pure and
// host-tested so the drift-prone bridge (esp. the millis→age math) is proven,
// leaving the device wiring a thin snapshot-and-render loop. NOTE: `fp4` aliases
// the entry's storage, so the FleetPeer must not outlive the entry it came from.
inline FleetPeer fleet_peer_from_entry(const FleetRosterEntry& e, uint32_t now_ms) {
  FleetPeer p{};
  p.fp4         = e.fp4;
  p.age_s       = (uint32_t)(now_ms - e.last_seen_ms) / 1000u;  // wrap-safe unsigned
  p.health_pct  = e.health_pct;
  p.battery_pct = e.battery_pct;
  p.chain_lo    = e.chain_lo;
  p.flags       = e.flags;
  p.rssi        = e.rssi;
  return p;
}

// ── small pure formatters (host-tested directly) ────────────────────────────

// "3s" / "47m" / "5h" / ">1d" — a compact last-heard age, right-alignable.
inline void fleet_fmt_age(uint32_t age_s, char* out, size_t cap) {
  if (age_s < 60)         snprintf(out, cap, "%us", (unsigned)age_s);
  else if (age_s < 3600)  snprintf(out, cap, "%um", (unsigned)(age_s / 60));
  else if (age_s < 86400) snprintf(out, cap, "%uh", (unsigned)(age_s / 3600));
  else                    snprintf(out, cap, ">1d");
}

// "72%" / "100%" / "--" (unknown). Clamps to 100.
inline void fleet_fmt_pct(int pct, char* out, size_t cap) {
  if (pct < 0) snprintf(out, cap, "--");
  else         snprintf(out, cap, "%d%%", pct > 100 ? 100 : pct);
}

// Decode the beacon flag bits into short, comma-separated words — never color
// alone. Writes "ok" when nothing is set. Bounded: always NUL-terminates within
// `cap`. Order is worst-first (tamper, alert) so a truncated cell still leads
// with the thing that matters.
inline void fleet_fmt_flags(uint8_t flags, char* out, size_t cap) {
  if (!out || cap == 0) return;
  static const struct { uint8_t bit; const char* word; } kMap[5] = {
    { FLEET_BEACON_FLAG_TAMPER,      "tamper"   },
    { FLEET_BEACON_FLAG_ALERT,       "alert"    },
    { FLEET_BEACON_FLAG_DEGRADED,    "degraded" },
    { FLEET_BEACON_FLAG_MIC_MUTED,   "muted"    },
    { FLEET_BEACON_FLAG_ON_WIFI_STA, "wifi"     },
  };
  size_t o = 0;
  int n = 0;
  for (int i = 0; i < 5; ++i) {
    if (!(flags & kMap[i].bit)) continue;
    if (n && o + 2 < cap) { out[o++] = ','; out[o++] = ' '; }
    for (const char* w = kMap[i].word; *w && o + 1 < cap; ++w) out[o++] = *w;
    n++;
  }
  if (n == 0) for (const char* w = "ok"; *w && o + 1 < cap; ++w) out[o++] = *w;
  out[o < cap ? o : cap - 1] = '\0';
}

// True if any flag warrants an attention color. The row is worded regardless;
// color is the bonus, never the meaning.
inline bool fleet_flags_alarming(uint8_t flags) {
  return (flags & (FLEET_BEACON_FLAG_TAMPER | FLEET_BEACON_FLAG_ALERT |
                   FLEET_BEACON_FLAG_DEGRADED)) != 0;
}

// ── the card ────────────────────────────────────────────────────────────────

// The fleet roster as a framed panel. Pass the same inner width as the trust
// card so they align; ASCII-floor safe at caps_ascii(), lit up at caps_full().
inline void fleet_card(const Renderer& r, const FleetView& v) {
  const int inner = FLEET_INNER;

  hrule(r, "Fleet  -  nearby Canaries", inner, 0);

  rowf(r, inner, COL_NONE, "This Canary  %s%s%s",
       v.self_id ? v.self_id : "?",
       (v.self_fp4 && *v.self_fp4) ? "   fp " : "",
       (v.self_fp4 && *v.self_fp4) ? v.self_fp4 : "");

  if (v.count == 0 || !v.peers) {
    row_blank(r, inner);
    row(r, inner, COL_DIM, "No other Canaries heard yet.");
    row(r, inner, COL_DIM, "They announce themselves over BLE every few");
    row(r, inner, COL_DIM, "seconds; this list fills as they come in range.");
  } else {
    rowf(r, inner, COL_NONE, "Heard %u of %u nearby   (within ~2 min)",
         (unsigned)v.count, (unsigned)v.capacity);
    hrule(r, "peers", inner, 1);
    // Column header — same field widths as the rows below.
    rowf(r, inner, COL_DIM, "%-4s %4s %4s %4s %-6s %s",
         "peer", "age", "hlth", "batt", "chain", "status");
    for (size_t i = 0; i < v.count; ++i) {
      const FleetPeer& p = v.peers[i];
      char age[8], hl[8], bt[8], ch[10], st[40];
      fleet_fmt_age(p.age_s, age, sizeof age);
      fleet_fmt_pct(p.health_pct, hl, sizeof hl);
      fleet_fmt_pct(p.battery_pct, bt, sizeof bt);
      snprintf(ch, sizeof ch, "#%04x", (unsigned)p.chain_lo);
      fleet_fmt_flags(p.flags, st, sizeof st);
      const int col = fleet_flags_alarming(p.flags) ? COL_AMBER : COL_NONE;
      rowf(r, inner, col, "%-4s %4s %4s %4s %-6s %s",
           p.fp4 ? p.fp4 : "?", age, hl, bt, ch, st);
    }
  }

  hrule(r, "", inner, 1);
  row(r, inner, COL_DIM, "Presence is UNSIGNED - liveness only, never a");
  row(r, inner, COL_DIM, "verified trust claim (like the beacon it reads).");
  hrule(r, "", inner, 2);
}

}  // namespace scene

#endif  // SECURACV_FLEET_VIEW_H
