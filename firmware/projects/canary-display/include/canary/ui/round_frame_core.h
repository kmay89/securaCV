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
// C++11-constexpr on purpose: the Arduino parity builds ride esp32 core
// 2.0.17, which compiles as gnu++11 — a constexpr body there is a single
// return statement, so everything below is expression-form (the sqrt is a
// bounded binary-search recursion, depth <= 16). The PlatformIO, emulator
// and host builds are C++17 and accept the same form unchanged.
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

constexpr int iabs(int v) { return v < 0 ? -v : v; }

// Binary-search step for the integer square root; 64-bit compare so r*r
// never overflows int. lo/hi invariant: lo*lo <= v < (hi+1)*(hi+1).
constexpr int isqrt_step(int32_t v, int lo, int hi) {
  return lo >= hi
             ? lo
             : ((int64_t)((lo + hi + 1) / 2) * ((lo + hi + 1) / 2) <=
                        (int64_t)v
                    ? isqrt_step(v, (lo + hi + 1) / 2, hi)
                    : isqrt_step(v, lo, (lo + hi + 1) / 2 - 1));
}

// Integer square root (floor).
constexpr int isqrt(int32_t v) {
  return v <= 0 ? 0 : isqrt_step(v, 0, 46340 /* isqrt(INT32_MAX) */);
}

// Half-chord of a circle of radius r at |dy| px from its center; 0 outside.
constexpr int half_chord_at(int r, int dy) {
  return iabs(dy) >= r
             ? 0
             : isqrt((int32_t)r * r - (int32_t)iabs(dy) * iabs(dy));
}

// Widest half-width honestly available to a horizontal band [y_top,
// y_top + h) on a disc of diameter dia, staying margin px inside the rim.
// The binding latitude is the band edge farther from the equator — content
// fills its whole band, so the narrowest row of pixels governs.
constexpr int band_half_chord(int dia, int margin, int y_top, int h) {
  return (dia / 2 - margin <= 0 || h <= 0)
             ? 0
             : half_chord_at(dia / 2 - margin,
                             iabs(y_top - dia / 2) > iabs(y_top + h - dia / 2)
                                 ? iabs(y_top - dia / 2)
                                 : iabs(y_top + h - dia / 2));
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
  return dia / 2 - (n * pitch) / 2 + i * pitch + bias;
}

// The largest square inscribed in the disc (margin applied): the budget a
// square payload — a QR code and its quiet zone — must fit inside. This is
// the number commission_ui derived by hand as "~169 px" (240/sqrt(2)),
// computed as isqrt(2*r^2) to stay integer-exact.
constexpr int inscribed_square(int dia, int margin) {
  return isqrt(2 * (int32_t)(dia / 2 - margin) * (dia / 2 - margin));
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

// Quarter-table lookup for an angle already wrapped to [0, 360).
constexpr int sin_wrapped(int a) {
  return a <= 90    ? kSinMilli[a]
         : a <= 180 ? kSinMilli[180 - a]
         : a <= 270 ? -kSinMilli[a - 180]
                    : -kSinMilli[360 - a];
}

constexpr int sin_milli(int deg) {
  return sin_wrapped(((deg % 360) + 360) % 360);
}

constexpr int cos_milli(int deg) { return sin_milli(deg + 90); }

constexpr int polar_dx(int angle_deg, int radius) {
  // 0 deg is 12 o'clock; clockwise. Screen x = r*sin(a).
  return (radius * sin_milli(angle_deg) +
          (sin_milli(angle_deg) >= 0 ? 500 : -500)) /
         1000;
}

constexpr int polar_dy(int angle_deg, int radius) {
  // Screen y grows downward; 12 o'clock is -y.
  return (-radius * cos_milli(angle_deg) +
          (cos_milli(angle_deg) <= 0 ? 500 : -500)) /
         1000;
}

}  // namespace canary::ui::roundframe
