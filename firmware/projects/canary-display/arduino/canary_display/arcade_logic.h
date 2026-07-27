// Arcade logic — pure, host-testable core (no Arduino, no LVGL, no rand()).
//
// Arcade mode (docs/hardware/display_modes.md §arcade) is the QA suite in a
// costume: one round of "Canary Catch" must, by construction, exercise every
// touch zone of the panel and measure tap latency per zone — the score screen
// IS the factory report. This header holds everything measurable about that
// claim, so the host test can pin it without a board:
//
//   - RoundPlan: a seeded shuffle that visits EVERY zone exactly once per
//     round (Fisher–Yates over an LCG — deterministic, replayable from the
//     seed printed in the report, and libc-rand-free).
//   - target_for_zone: the target rect for a zone, jittered inside the zone
//     cell but never outside it (a target that strays out of its cell would
//     void the "this tap tested THAT zone" claim).
//   - RoundStats: hit/miss/latency accounting + the zone-coverage bitmap and
//     the QA verdict (all zones hit, worst latency under the bar).
//
// The runtime TU (src/mode/arcade_mode.cpp, FEATURE_ARCADE) draws targets
// with LVGL and feeds taps back here; it contains no decisions of its own.

#ifndef CANARY_MODE_ARCADE_LOGIC_H
#define CANARY_MODE_ARCADE_LOGIC_H

#include <stdint.h>

namespace canary {
namespace mode {
namespace arcade {

// Deterministic LCG (Numerical Recipes constants). The round seed is shown
// on the report screen, so a factory failure can be replayed exactly.
inline uint32_t lcg_next(uint32_t& s) {
  s = s * 1664525u + 1013904223u;
  return s;
}

// ── Round plan: every zone exactly once, shuffled ───────────────────────────

inline constexpr uint8_t MAX_ZONES = 64;  // 8x5 on the dash uses 40

struct RoundPlan {
  uint8_t order[MAX_ZONES] = {0};
  uint8_t count = 0;

  // Fisher–Yates over the LCG. `zones` is clamped to MAX_ZONES.
  void build(uint8_t zones, uint32_t seed) {
    count = (zones > MAX_ZONES) ? MAX_ZONES : zones;
    for (uint8_t i = 0; i < count; i++) order[i] = i;
    for (uint8_t i = count; i > 1; i--) {
      const uint8_t j = (uint8_t)(lcg_next(seed) % i);
      const uint8_t tmp = order[i - 1];
      order[i - 1] = order[j];
      order[j] = tmp;
    }
  }
};

// ── Target placement: jittered, never outside its zone cell ─────────────────

struct Target {
  int16_t x = 0, y = 0;  // top-left
  int16_t size = 0;      // square target, side length
  uint8_t zone = 0;
};

// Zone cells tile the screen as cols x rows (zone = row * cols + col). The
// target lands fully inside its cell: jitter shrinks to whatever slack the
// cell leaves, and a target larger than the cell is clamped to the cell.
inline Target target_for_zone(uint8_t zone, uint8_t cols, uint8_t rows,
                              int16_t scr_w, int16_t scr_h, int16_t size,
                              uint32_t& seed) {
  Target t;
  t.zone = zone;
  if (cols == 0 || rows == 0) return t;
  const int16_t cell_w = (int16_t)(scr_w / cols);
  const int16_t cell_h = (int16_t)(scr_h / rows);
  int16_t sz = size;
  if (sz > cell_w) sz = cell_w;
  if (sz > cell_h) sz = cell_h;
  t.size = sz;
  const uint8_t col = (uint8_t)(zone % cols);
  const uint8_t row = (uint8_t)(zone / cols);
  const int16_t x0 = (int16_t)(col * cell_w);
  const int16_t y0 = (int16_t)(row * cell_h);
  const int16_t slack_x = (int16_t)(cell_w - sz);
  const int16_t slack_y = (int16_t)(cell_h - sz);
  t.x = (int16_t)(x0 + (slack_x > 0 ? (int16_t)(lcg_next(seed) % (uint16_t)(slack_x + 1)) : 0));
  t.y = (int16_t)(y0 + (slack_y > 0 ? (int16_t)(lcg_next(seed) % (uint16_t)(slack_y + 1)) : 0));
  return t;
}

inline bool target_contains(const Target& t, int16_t x, int16_t y) {
  return x >= t.x && x < (int16_t)(t.x + t.size) && y >= t.y &&
         y < (int16_t)(t.y + t.size);
}

// ── Round accounting + the QA verdict ───────────────────────────────────────

struct RoundStats {
  uint8_t  hits = 0;
  uint8_t  misses = 0;          // taps that landed outside the live target
  uint16_t worst_ms = 0;        // slowest hit latency
  uint32_t sum_ms = 0;
  uint64_t zone_bits = 0;       // coverage bitmap (bit = zone hit)

  void on_hit(uint8_t zone, uint16_t latency_ms) {
    if (hits < 255) hits++;
    if (latency_ms > worst_ms) worst_ms = latency_ms;
    sum_ms += latency_ms;
    if (zone < 64) zone_bits |= (1ULL << zone);
  }
  void on_miss() {
    if (misses < 255) misses++;
  }
  uint16_t avg_ms() const {
    return hits ? (uint16_t)(sum_ms / hits) : 0;
  }
  uint8_t zones_hit(uint8_t zones) const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < zones && i < 64; i++) {
      if (zone_bits & (1ULL << i)) n++;
    }
    return n;
  }
  bool all_zones(uint8_t zones) const { return zones_hit(zones) == zones; }
};

// The factory bar: every zone answered, and no zone answered slowly. A
// missed zone is a dead spot; a slow zone is a controller/wiring problem —
// both are exactly what an end-of-line test exists to catch.
inline bool qa_pass(const RoundStats& st, uint8_t zones,
                    uint16_t worst_allowed_ms) {
  return st.all_zones(zones) && st.worst_ms <= worst_allowed_ms &&
         st.misses == 0;
}

}  // namespace arcade
}  // namespace mode
}  // namespace canary

#endif  // CANARY_MODE_ARCADE_LOGIC_H
