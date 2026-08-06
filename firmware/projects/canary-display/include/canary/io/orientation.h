// include/canary/io/orientation.h — gravity-settled orientation, pure model.
//
// The nightlight turns with the room: stand it on either end or either
// side and the clock rights itself, like a phone — but a bedside lamp
// gets handled, carried, knocked, and hugged, so the model's whole job is
// telling REAL re-orientation from mere movement. The rule that does it:
//
//   an orientation is only believed when gravity is the only thing
//   the accelerometer is measuring — i.e. the device has SETTLED.
//
// Three gates, all of which must hold across a dwell window before a flip
// commits (host-tested in tests_host/test_orientation.cpp):
//
//   1. SETTLED — |a| within a band around 1 g. A shake, a carry, a bump
//      all push the magnitude off 1 g; those samples carry no opinion.
//   2. DECISIVE — the in-plane component dominates: the winning screen
//      axis must carry most of gravity, the out-of-plane (z) component
//      must stay small (lying face-up/face-down keeps the LAST
//      orientation — a flat lamp has no "up" worth acting on), and the
//      winner must beat the other in-plane axis by a real margin (no
//      45° flapping).
//   3. PATIENT — the same candidate must hold for DWELL_MS of settled
//      samples, and after a commit a cooldown passes before the next
//      flip can even start. Turning the lamp feels immediate — the
//      commit lands the moment it comes to rest — but a wobbling hand
//      can never double-flip it.
//
// Pure integer math (int32 inputs, int64 intermediates), no Arduino, no
// clock reads: the glue feeds screen-mapped accel samples and now_ms.
// Axis mapping from the IMU's mounting to screen coordinates is the
// BOARD's business (pins.h IMU_TO_SCREEN_* macros) — this model already
// thinks in screen axes: +x right, +y DOWN the glass (gravity pulls +y
// when the device stands upright).
#pragma once
#include <stdint.h>

namespace canary::io {

// Screen rotations, matching Arduino_GFX's numbering: the value is how
// far the FACE must rotate clockwise so "up" is up again.
enum class Orient : uint8_t {
  R0 = 0,    // upright portrait (USB down, per the pocket case's keyhole)
  R90 = 1,   // turned clockwise onto its left edge -> landscape
  R180 = 2,  // upside down portrait
  R270 = 3,  // turned counter-clockwise -> the other landscape
};

class OrientationModel {
 public:
  // `one_g` = the accel scale's LSB-per-g (e.g. 8192 at +-4g). Bounds are
  // compile-time facts, not preferences — they are what "settled" means.
  static constexpr uint32_t DWELL_MS    = 1200;  // candidate must hold this long
  static constexpr uint32_t COOLDOWN_MS = 1500;  // after a commit, rest first

  void begin(int32_t one_g, Orient initial) {
    g_ = one_g < 1 ? 1 : one_g;
    current_ = initial;
    candidate_ = initial;
    candidate_since_ms_ = 0;
    cooldown_until_ms_ = 0;
  }

  Orient current() const { return current_; }

  // Feed one screen-mapped sample. Returns true exactly when a new
  // orientation COMMITS (the tumble moment); current() then reports it.
  bool step(int32_t ax, int32_t ay, int32_t az, uint32_t now_ms) {
    if ((int32_t)(now_ms - cooldown_until_ms_) < 0) {
      candidate_since_ms_ = 0;  // motion during cooldown restarts patience
      return false;
    }

    // Gate 1 — settled: |a|^2 within [0.75g, 1.30g]^2. Squared compare
    // keeps it integer-exact (int64: 3 * (32g * 8192)^2 still fits).
    const int64_t m2 = (int64_t)ax * ax + (int64_t)ay * ay + (int64_t)az * az;
    const int64_t g2 = (int64_t)g_ * g_;
    if (m2 < (g2 * 9) / 16 || m2 > (g2 * 169) / 100) {  // (0.75)^2, (1.3)^2
      candidate_since_ms_ = 0;
      return false;
    }

    // Gate 2 — decisive. All thresholds scale from g_ so any range works.
    const int32_t axa = ax < 0 ? -ax : ax;
    const int32_t aya = ay < 0 ? -ay : ay;
    const int32_t aza = az < 0 ? -az : az;
    const int32_t dominant = axa > aya ? axa : aya;
    const int32_t other    = axa > aya ? aya : axa;
    if (aza > (g_ * 13) / 20) {        // > 0.65 g out of plane: lying flat
      candidate_since_ms_ = 0;         // — no opinion, keep what we have
      return false;
    }
    if (dominant < g_ / 2) {           // winner must carry >= 0.5 g
      candidate_since_ms_ = 0;
      return false;
    }
    if ((int64_t)dominant * 2 < (int64_t)other * 3) {  // beat it 1.5x
      candidate_since_ms_ = 0;         // (diagonals have no opinion either)
      return false;
    }

    // +y is DOWN the glass, so gravity along +y = upright.
    const Orient seen =
        axa > aya ? (ax > 0 ? Orient::R90 : Orient::R270)
                  : (ay > 0 ? Orient::R0 : Orient::R180);

    if (seen == current_) {
      candidate_since_ms_ = 0;         // home again; nothing pending
      return false;
    }

    // Gate 3 — patient.
    if (candidate_ != seen || candidate_since_ms_ == 0) {
      candidate_ = seen;
      candidate_since_ms_ = now_ms ? now_ms : 1;
      return false;
    }
    if (now_ms - candidate_since_ms_ < DWELL_MS) return false;

    current_ = seen;
    candidate_since_ms_ = 0;
    cooldown_until_ms_ = now_ms + COOLDOWN_MS;
    return true;
  }

 private:
  int32_t g_ = 8192;
  Orient current_ = Orient::R0;
  Orient candidate_ = Orient::R0;
  uint32_t candidate_since_ms_ = 0;
  uint32_t cooldown_until_ms_ = 0;
};

}  // namespace canary::io
