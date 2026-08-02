// canary/companion/raise_gesture.h — wake-on-raise from the QMI8658, and the
// wrist-tap the pet answers to. Pure, host-testable: the detector eats
// accelerometer samples as milli-g and knows nothing about I²C.
//
// Design: docs/design/canary_companion.md §3.4.
//
// ── The only two failure modes that matter ───────────────────────────────────
//
// A wake-on-raise detector is judged on exactly two things, and they pull in
// opposite directions:
//
//   FALSE NEGATIVE — you lift your wrist to check the time and the screen stays
//     dark. Costs a deliberate second gesture. Mildly annoying.
//   FALSE POSITIVE — you roll over at 3 a.m. and a 200-nit panel fires six
//     inches from a sleeping face. Costs the whole product.
//
// They are not symmetric, so this detector is not tuned symmetrically. It is
// biased hard toward silence, and `night_strict` biases it further after dark:
// at night it demands a steeper final tilt and a longer settle, because the one
// gesture we must not misread is a sleeper turning over.
//
// The device also has an honest fallback the wrist does not: this is a
// nightstand clock as often as it is a watch, and a tap on the glass always
// works. So a missed raise is never the only way in.

#ifndef CANARY_COMPANION_RAISE_GESTURE_H
#define CANARY_COMPANION_RAISE_GESTURE_H

#include <stdint.h>

namespace canary {
namespace companion {

// One accelerometer sample, in milli-g, in the board's frame. +Z is out of the
// glass, so a watch face turned toward the eyes reads Z near +1000 mg.
struct AccelSample {
  int16_t x_mg = 0;
  int16_t y_mg = 0;
  int16_t z_mg = 1000;
  uint32_t t_ms = 0;
};

// ── Thresholds ───────────────────────────────────────────────────────────────
//
// A raise is a three-part story and all three parts must be present:
//   1. MOTION   — the arm actually moved (an arm at rest never wakes anything)
//   2. TILT     — it ended with the glass turned toward a face
//   3. SETTLE   — and then it HELD there, which is what separates "reading the
//                 time" from "reaching for a glass of water"
//
// Step 3 is the one that most implementations skip and the one that stops the
// 3 a.m. false positive, because a roll-over passes through every tilt angle on
// its way to somewhere else and never rests at any of them.

static constexpr int16_t RAISE_MOTION_MG = 260;      // vector delta between samples
static constexpr int16_t RAISE_TILT_Z_MG = 620;      // day: glass toward the face
static constexpr int16_t RAISE_TILT_Z_MG_NIGHT = 800;  // night: steeper, deliberate
static constexpr uint16_t RAISE_SETTLE_MS = 220;     // day hold
static constexpr uint16_t RAISE_SETTLE_MS_NIGHT = 420;  // night hold
static constexpr int16_t RAISE_SETTLE_JITTER_MG = 120;  // "held still" tolerance

// After a raise fires, ignore raises for this long. Without it a wrist held up
// while reading re-triggers continuously and the hold timer never lapses.
static constexpr uint16_t RAISE_REFRACTORY_MS = 2500;

// A raise must complete within this window of the motion that started it.
static constexpr uint16_t RAISE_WINDOW_MS = 1200;

enum class RaisePhase : uint8_t {
  Idle = 0,   // at rest, nothing started
  Moving,     // motion seen, waiting for the tilt to arrive
  Settling,   // tilt reached, counting the hold
};

struct RaiseDetector {
  RaisePhase phase = RaisePhase::Idle;
  uint32_t phase_since_ms = 0;
  uint32_t refractory_until_ms = 0;
  AccelSample prev;
  bool have_prev = false;
  AccelSample settle_ref;  // the pose the settle is measured against
};

inline int32_t accel_delta_mg(const AccelSample& a, const AccelSample& b) {
  const int32_t dx = a.x_mg - b.x_mg;
  const int32_t dy = a.y_mg - b.y_mg;
  const int32_t dz = a.z_mg - b.z_mg;
  // Manhattan rather than Euclidean: no sqrt, and the threshold is empirical
  // either way. Monotonic in the same direction, which is all it must be.
  const int32_t ax = dx < 0 ? -dx : dx;
  const int32_t ay = dy < 0 ? -dy : dy;
  const int32_t az = dz < 0 ? -dz : dz;
  return ax + ay + az;
}

// Feed one sample. Returns true exactly once per completed raise.
inline bool raise_feed(RaiseDetector& d, const AccelSample& s, bool night_strict) {
  const int16_t tilt_gate = night_strict ? RAISE_TILT_Z_MG_NIGHT : RAISE_TILT_Z_MG;
  const uint16_t settle_ms = night_strict ? RAISE_SETTLE_MS_NIGHT : RAISE_SETTLE_MS;

  if (!d.have_prev) {
    d.prev = s;
    d.have_prev = true;
    return false;
  }

  const int32_t moved = accel_delta_mg(s, d.prev);
  d.prev = s;

  // Refractory: still count time, but nothing fires.
  if (d.refractory_until_ms != 0) {
    if (static_cast<uint32_t>(s.t_ms - d.refractory_until_ms) < 0x80000000u) {
      d.refractory_until_ms = 0;
    } else {
      return false;
    }
  }

  switch (d.phase) {
    case RaisePhase::Idle:
      if (moved >= RAISE_MOTION_MG) {
        d.phase = RaisePhase::Moving;
        d.phase_since_ms = s.t_ms;
      }
      return false;

    case RaisePhase::Moving: {
      if (s.t_ms - d.phase_since_ms > RAISE_WINDOW_MS) {
        d.phase = RaisePhase::Idle;  // a move that never became a raise
        return false;
      }
      if (s.z_mg >= tilt_gate) {
        d.phase = RaisePhase::Settling;
        d.phase_since_ms = s.t_ms;
        d.settle_ref = s;
      }
      return false;
    }

    case RaisePhase::Settling: {
      // Still tilted, and still STILL. Either one failing drops the gesture:
      // a wrist that keeps moving is going somewhere, not reading a clock.
      //
      // Stillness is measured on ALL THREE AXES against the pose the settle
      // started from, not on Z alone. A roll-over holds Z roughly constant for
      // stretches while X and Y sweep through a whole rotation — checking only
      // the axis that happens to gate the tilt is exactly how a tumble gets
      // mistaken for someone reading a clock (test_rollover_does_not_fire_at_night).
      const bool still_tilted = s.z_mg >= tilt_gate;
      const bool held = accel_delta_mg(s, d.settle_ref) <= RAISE_SETTLE_JITTER_MG;
      if (!still_tilted || !held) {
        d.phase = RaisePhase::Idle;
        return false;
      }
      if (s.t_ms - d.phase_since_ms >= settle_ms) {
        d.phase = RaisePhase::Idle;
        d.refractory_until_ms = s.t_ms + RAISE_REFRACTORY_MS;
        return true;
      }
      return false;
    }
  }
  return false;
}

// ── The wrist tap ────────────────────────────────────────────────────────────
//
// A sharp spike on the accelerometer with the wrist otherwise still: knuckle on
// the case. It is the pet's "hello" gesture and the one input that works with
// the glass dark, gloves on, or the watch under a sleeve.
//
// Reuses the knock codec's debounce reasoning (canary/tincan/knock_codec.h):
// below ~60 ms two contacts are one tap with a bouncing finger, not a rhythm.

static constexpr int32_t TAP_SPIKE_MG = 900;
static constexpr uint16_t TAP_DEBOUNCE_MS = 60;

struct TapDetector {
  uint32_t last_tap_ms = 0;
  AccelSample prev;
  bool have_prev = false;
};

inline bool tap_feed(TapDetector& t, const AccelSample& s) {
  if (!t.have_prev) {
    t.prev = s;
    t.have_prev = true;
    return false;
  }
  const int32_t jolt = accel_delta_mg(s, t.prev);
  t.prev = s;
  if (jolt < TAP_SPIKE_MG) return false;
  if (t.last_tap_ms != 0 && s.t_ms - t.last_tap_ms < TAP_DEBOUNCE_MS) return false;
  t.last_tap_ms = s.t_ms;
  return true;
}

}  // namespace companion
}  // namespace canary

#endif  // CANARY_COMPANION_RAISE_GESTURE_H
