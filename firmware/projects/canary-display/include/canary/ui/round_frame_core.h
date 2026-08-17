#pragma once
#include <stdint.h>

// Round Frame — the geometry engine for circular glass.
//
// The watch face is a 240 px disc, and for its first year every surface
// respected the circle by hand: a centered label here, a "packs tighter"
// comment there, one file deriving the inscribed square in a comment while
// another eyeballed a bottom margin. That discipline held until it didn't —
// captions near the rim run past the chord and the physical glass simply
// cuts them off mid-character (the emulator's square canvas never shows it).
//
// This header is the one place that knows the circle. Pure integer math, no
// LVGL, no Arduino — host-tested by tests_host/test_round_frame_core.cpp.
// The LVGL-facing helpers live in round_frame.h; faces should route every
// "how wide can this row be?" question through here rather than growing a
// new magic number.
//
// Coordinates follow LVGL: y grows downward, (0,0) is the panel's top-left,
// the disc center sits at (dia/2, dia/2).

namespace canary::ui::roundframe {

// The one disc this engine serves today: the GC9A01 round glass.
constexpr int kDiscDiameter = 240;

// Pixels held back from the physical edge: the panel's outermost rows are
// under the bezel aperture's tolerance and the arc's anti-aliased falloff —
// text laid to the mathematical edge reads as touching the rim.
constexpr int kEdgeMargin = 5;

// Integer square root (floor). 64-bit compare so r*r never overflows int.
constexpr int isqrt(int32_t v) {
  if (v <= 0) return 0;
  int lo = 0, hi = 46340;  // isqrt(INT32_MAX)
  while (lo < hi) {
    const int mid = (lo + hi + 1) / 2;
    if ((int64_t)mid * mid <= (int64_t)v) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

// Half-chord of a circle of radius r at |dy| px from its center; 0 outside.
constexpr int half_chord_at(int r, int dy) {
  const int d = dy < 0 ? -dy : dy;
  if (d >= r) return 0;
  return isqrt((int32_t)r * r - (int32_t)d * d);
}

// Widest half-width honestly available to a horizontal band [y_top,
// y_top + h) on a disc of diameter dia, staying margin px inside the rim.
// The binding latitude is the band edge farther from the equator — content
// fills its whole band, so the narrowest row of pixels governs.
constexpr int band_half_chord(int dia, int margin, int y_top, int h) {
  const int r = dia / 2 - margin;
  if (r <= 0 || h <= 0) return 0;
  const int c = dia / 2;
  const int d_top = y_top - c;
  const int d_bot = (y_top + h) - c;
  const int a = d_top < 0 ? -d_top : d_top;
  const int b = d_bot < 0 ? -d_bot : d_bot;
  return half_chord_at(r, a > b ? a : b);
}

// Full usable width of that band (the number faces actually want).
constexpr int band_chord(int dia, int margin, int y_top, int h) {
  return 2 * band_half_chord(dia, margin, y_top, h);
}

// Band width on THE disc (240 px, house margin).
constexpr int chord(int y_top, int h) {
  return band_chord(kDiscDiameter, kEdgeMargin, y_top, h);
}

// A stack of n fixed-pitch rows centered on the equator — the widest
// latitudes the disc has. Returns row i's top y (i in [0, n)). bias shifts
// the whole stack (negative = up), for a stack that shares the disc with a
// title above it.
constexpr int row_stack_y(int dia, int n, int pitch, int i, int bias) {
  const int stack_h = n * pitch;
  return dia / 2 - stack_h / 2 + i * pitch + bias;
}

// The largest square inscribed in the disc (margin applied): the budget a
// square payload — a QR code and its quiet zone — must fit inside. This is
// the number commission_ui derived by hand as "~169 px" (240/sqrt(2)).
constexpr int inscribed_square(int dia, int margin) {
  // side = r * sqrt(2), computed as isqrt(2*r^2) to stay integer-exact.
  const int r = dia / 2 - margin;
  return isqrt(2 * (int32_t)r * r);
}

// Polar placement: angle in degrees (0 = 12 o'clock, clockwise — the same
// convention the halo's arcs use), radius in px from the disc center.
// Returns offsets from center, x right / y down, for LV_ALIGN_CENTER math.
// sin table in 0.001 steps per degree keeps this integer and host-exact.
constexpr int kSinMilli[91] = {
    0,   17,  35,  52,  70,  87,  105, 122, 139, 156, 174, 191, 208,
    225, 242, 259, 276, 292, 309, 326, 342, 358, 375, 391, 407, 423,
    438, 454, 469, 485, 500, 515, 530, 545, 559, 574, 588, 602, 616,
    629, 643, 656, 669, 682, 695, 707, 719, 731, 743, 755, 766, 777,
    788, 799, 809, 819, 829, 839, 848, 857, 866, 875, 883, 891, 899,
    906, 914, 921, 927, 934, 940, 946, 951, 956, 961, 966, 970, 974,
    978, 982, 985, 988, 990, 993, 995, 996, 998, 999, 999, 1000, 1000};

constexpr int sin_milli(int deg) {
  int a = deg % 360;
  if (a < 0) a += 360;
  if (a <= 90) return kSinMilli[a];
  if (a <= 180) return kSinMilli[180 - a];
  if (a <= 270) return -kSinMilli[a - 180];
  return -kSinMilli[360 - a];
}

constexpr int cos_milli(int deg) { return sin_milli(deg + 90); }

constexpr int polar_dx(int angle_deg, int radius) {
  // 0 deg is 12 o'clock; clockwise. Screen x = r*sin(a).
  return (radius * sin_milli(angle_deg) + (sin_milli(angle_deg) >= 0 ? 500 : -500)) / 1000;
}

constexpr int polar_dy(int angle_deg, int radius) {
  // Screen y grows downward; 12 o'clock is -y.
  return (-radius * cos_milli(angle_deg) + (cos_milli(angle_deg) <= 0 ? 500 : -500)) / 1000;
}

}  // namespace canary::ui::roundframe
