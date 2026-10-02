#pragma once

// Splash Layout — where the boot splash seats the bird and the speech bubble.
//
// splash.cpp seats the first meeting's bird a fixed distance above the
// canvas's center (CENTER -70 on small glass), a seat drawn for the round
// watch's 240 px disc. The nightlight can boot into its saved landscape
// rotation, a 320x180 canvas, and there that seat put the bird's top at
// y -12: its head was off the glass, and its intro hops lifted it to y -25
// (F88, measured in native LVGL 8.4). This header is the one place the
// splash's seats are named, so a host test can hold them on every canvas
// the splash runs on.
//
// The rule:
//  * The bird keeps its seat wherever the canvas holds it, with its hop:
//    every canvas the splash ran on before F88 but the landscape nightlight
//    draws exactly as it did.
//  * Where it does not, the seat drops just enough that the hop's apex
//    (kHopReach above the seat) stays on the canvas — clamped to the
//    canvas, not scaled.
//  * Then the bird sits lower than the speech bubble's centered seat
//    expects, so the bubble hangs from just under the bird instead (its top
//    fixed, growing down), clear of the bird's breath.
//
// C++11 and constexpr-free functions, like onboard_layout.h: the Arduino
// parity sketch compiles it on esp32 core 2.0.17 (gnu++11). Pure integer
// math, no LVGL — host-tested by tests_host/test_splash_layout.cpp.
//
// Coordinates follow LVGL: y grows downward, 0 is the canvas's top edge.

namespace canary::ui::splashlayout {

// The splash's geometry for a glass family: small glass is splash.cpp's
// watch branch (the round watch and the portrait nightstand line, the
// nightlight, the touch169 and the AMOLED), wide glass the dash line.
// Offsets are a center's offset from the canvas's center.
struct Family {
  int bird;        // the brand mark's square
  int intro_off;   // the first meeting's bird (the speaker, over the bubble)
  int hello_off;   // the hello-again bird (over the wordmark)
  int word_off;    // the wordmark
  int tag_off;     // the tagline
  int bubble_w;    // the speech bubble's width
  int bubble_off;  // the speech bubble, centered (its text grows both ways)
};

constexpr Family kSmallGlass = {64, -70, -42, 26, 56, 196, 5};
constexpr Family kWideGlass = {96, -80, -66, 34, 78, 420, 24};

// The speech bubble's frame (splash.cpp): its padding and border, and how
// much narrower than the bubble its wrapped line is.
constexpr int kBubblePad = 10;
constexpr int kBubbleBorder = 1;
constexpr int kBubbleTextInset = 24;

// How far a hop lifts the bird above its seat at most: canary_mark's 12 px
// apex times the temperament's ceiling (1.25, so 15 px), plus the 9.4 %
// LVGL's overshoot path swings past the apex (16). The host test derives
// it from canary_mark.cpp and LVGL's curve and holds this number to it.
constexpr int kHopReach = 16;
constexpr int kBreath = 2;  // canary_mark's breath, either way of the seat
constexpr int kGap = 2;     // the least air between the bird and the bubble

struct Seat {
  int bird_off;    // the bird's center offset from the canvas's center
  bool hang;       // the bubble hangs from under the bird (bubble_top)
  int bubble_top;  // y of the bubble's top edge when it hangs
};

// The bird's top edge for a center offset on a canvas h px tall (LVGL's
// LV_ALIGN_CENTER: half the parent less half the child, plus the offset).
inline int bird_top(int canvas_h, int bird, int off) {
  return canvas_h / 2 - bird / 2 + off;
}

// The splash's seats on a canvas h px tall (see the rule above).
inline Seat seat(int canvas_h, const Family& f, bool first_meeting) {
  const int off = first_meeting ? f.intro_off : f.hello_off;
  const int top = bird_top(canvas_h, f.bird, off);
  Seat s;
  s.bird_off = top < kHopReach ? off + (kHopReach - top) : off;
  s.hang = s.bird_off != off;
  s.bubble_top =
      bird_top(canvas_h, f.bird, s.bird_off) + f.bird + kBreath + kGap;
  return s;
}

}  // namespace canary::ui::splashlayout
