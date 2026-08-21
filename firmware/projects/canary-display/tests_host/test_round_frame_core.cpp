// Host test for the Round Frame geometry engine (canary/ui/round_frame_core.h):
// the integer sqrt, the chord-at-latitude math every fitted label rides, the
// equator-centered row stacks, the inscribed square commission_ui budgets its
// QR from, and the polar helpers. Pure integer math, no Arduino, no LVGL —
// the exact values asserted here are the exact values the glass computes.
//
// Prints "ALL ROUND FRAME TESTS PASSED" on success. Build (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//     firmware/projects/canary-display/tests_host/test_round_frame_core.cpp -o t && ./t

#include "canary/ui/round_frame_core.h"

#include <cstdio>

using namespace canary::ui::roundframe;

static int g_fail = 0;

#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                      \
    }                                                                \
  } while (0)

// ── integer sqrt ────────────────────────────────────────────────────────────

static void test_isqrt() {
  std::printf("isqrt:\n");
  CHECK(isqrt(0) == 0, "isqrt(0)");
  CHECK(isqrt(1) == 1, "isqrt(1)");
  CHECK(isqrt(2) == 1, "isqrt(2) floors");
  CHECK(isqrt(3) == 1, "isqrt(3) floors");
  CHECK(isqrt(4) == 2, "isqrt(4)");
  CHECK(isqrt(115 * 115) == 115, "perfect square 115");
  CHECK(isqrt(115 * 115 - 1) == 114, "one below a square floors");
  CHECK(isqrt(2147483647) == 46340, "INT32_MAX does not overflow");
  CHECK(isqrt(-5) == 0, "negative clamps to 0");
}

// ── chord at latitude ───────────────────────────────────────────────────────

static void test_half_chord() {
  std::printf("half_chord_at:\n");
  CHECK(half_chord_at(115, 0) == 115, "equator is the full radius");
  CHECK(half_chord_at(115, 115) == 0, "the pole is a point");
  CHECK(half_chord_at(115, 200) == 0, "outside the disc is zero");
  CHECK(half_chord_at(115, -60) == half_chord_at(115, 60),
        "latitude is symmetric");
  CHECK(half_chord_at(100, 60) == 80, "3-4-5 triple lands exact");
  // Monotone: chords only narrow toward the poles.
  for (int d = 1; d <= 115; d++) {
    if (half_chord_at(115, d) > half_chord_at(115, d - 1)) {
      CHECK(false, "chord widened toward the rim");
      break;
    }
  }
}

static void test_band_chord() {
  std::printf("band_half_chord / chord:\n");
  // The binding edge is the one farther from the equator.
  CHECK(band_half_chord(240, 5, 100, 40) == half_chord_at(115, 20),
        "band straddling the equator binds at its farther edge");
  CHECK(band_half_chord(240, 5, 20, 14) == half_chord_at(115, 100),
        "top band binds at its top edge");
  CHECK(band_half_chord(240, 5, 200, 14) == half_chord_at(115, 94),
        "bottom band binds at its bottom edge");
  // Mirror bands get mirror widths.
  CHECK(band_half_chord(240, 5, 30, 20) == band_half_chord(240, 5, 190, 20),
        "equidistant bands match");
  // The margin ring is honored: nothing ever reaches the physical edge.
  CHECK(chord(0, 240) == 0, "the full-height band has no safe width");
  for (int y = 0; y < 240; y += 7) {
    if (chord(y, 14) > 2 * (120 - kEdgeMargin)) {
      CHECK(false, "chord exceeded the margin ring");
      break;
    }
  }
  // Degenerate input.
  CHECK(band_half_chord(240, 5, 60, 0) == 0, "empty band is zero");
  CHECK(band_half_chord(10, 5, 2, 2) == 0, "margin swallowing the disc is zero");
  // Pinned values for the glance list rows (integer math — these are the
  // exact widths the glass computes; a change here is a layout change).
  CHECK(chord(56, 14) == 190, "top list row width");
  CHECK(chord(172, 14) == 188, "bottom list row width");
}

// ── equator-centered row stacks ─────────────────────────────────────────────

static void test_row_stack() {
  std::printf("row_stack_y:\n");
  // 4 rows x 32 px on the 240 disc: stack spans 56..184, centered on 120.
  CHECK(row_stack_y(240, 4, 32, 0, 0) == 56, "first row of four");
  CHECK(row_stack_y(240, 4, 32, 3, 0) == 152, "last row of four");
  const int top_gap = row_stack_y(240, 4, 32, 0, 0);
  const int bot_gap = 240 - (row_stack_y(240, 4, 32, 3, 0) + 32);
  CHECK(top_gap == bot_gap, "stack is equator-centered");
  CHECK(row_stack_y(240, 4, 32, 1, 10) == row_stack_y(240, 4, 32, 1, 0) + 10,
        "bias shifts the whole stack");
  // Every row of a 4x32 stack keeps a readable chord (>= 180 px).
  for (int i = 0; i < 4; i++) {
    const int y = row_stack_y(240, 4, 32, i, 0);
    if (chord(y, 14) < 180) {
      CHECK(false, "a stacked row fell below 180 px");
      break;
    }
  }
}

// ── inscribed square ────────────────────────────────────────────────────────

static void test_inscribed_square() {
  std::printf("inscribed_square:\n");
  // The number commission_ui derived by hand: ~169 px on the bare 240 disc.
  CHECK(inscribed_square(240, 0) == 169, "240 disc inscribes 169");
  CHECK(inscribed_square(240, kEdgeMargin) == 162, "margin ring inscribes 162");
  // The square's diagonal fits the diameter (the defining property).
  const int s = inscribed_square(240, 0);
  CHECK(2 * s * s <= 240 * 240, "diagonal fits");
  CHECK(2 * (s + 1) * (s + 1) > 240 * 240, "and it is the largest such");
}

// ── polar placement ─────────────────────────────────────────────────────────

static void test_polar() {
  std::printf("polar:\n");
  CHECK(sin_milli(0) == 0 && sin_milli(30) == 500 && sin_milli(90) == 1000,
        "sin table anchors");
  CHECK(cos_milli(60) == 500 && cos_milli(0) == 1000, "cos anchors");
  CHECK(sin_milli(-30) == -500 && sin_milli(390) == 500, "wraps both ways");
  // 12 o'clock is straight up (screen -y), and the compass reads clockwise.
  CHECK(polar_dx(0, 100) == 0 && polar_dy(0, 100) == -100, "12 o'clock");
  CHECK(polar_dx(90, 100) == 100 && polar_dy(90, 100) == 0, "3 o'clock");
  CHECK(polar_dx(180, 100) == 0 && polar_dy(180, 100) == 100, "6 o'clock");
  CHECK(polar_dx(270, 100) == -100 && polar_dy(270, 100) == 0, "9 o'clock");
  // 45 degrees lands on the diagonal within a pixel.
  const int dx = polar_dx(45, 100), dy = polar_dy(45, 100);
  CHECK(dx >= 70 && dx <= 71 && dy <= -70 && dy >= -71, "45 deg diagonal");
}

int main() {
  test_isqrt();
  test_half_chord();
  test_band_chord();
  test_row_stack();
  test_inscribed_square();
  test_polar();
  if (g_fail == 0) {
    std::printf("ALL ROUND FRAME TESTS PASSED\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", g_fail);
  return 1;
}
