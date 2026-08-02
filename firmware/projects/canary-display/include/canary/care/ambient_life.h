// include/canary/care/ambient_life.h — rationed "life moments", pure model.
//
// The Flipper-style organic layer: every few minutes an idle, lit glass does
// one small living thing — the bird throws a flourish, or the face surfaces
// a brief status glance ("all quiet · 6 canaries") — so the device reads as
// a companion checking in, not a static screen. The motion budget stays the
// law (theme.h: motion is rationed and purposeful): moments are MINUTES
// apart, deterministic, and only happen while the glass is allowed to live
// (lit + idle; never on a dark honest night, never over an alarm — the
// caller gates that and the scheduler simply doesn't advance).
//
// Deterministic by construction (a seeded xorshift, no wall clock): the same
// seed and tick sequence produce the same moments, which is what makes this
// host-testable and keeps the cadence identical across reboots of a bench.
#pragma once
#include <stdint.h>

namespace canary::care {

enum class LifeMoment : uint8_t {
  None = 0,
  Flourish,   // a bird one-shot (Tilt-class; the mark rations these further)
  Glance,     // a brief status line surfacing, then fading
};

class AmbientLife {
 public:
  // Bounds are compile-time facts, not preferences: the layer must stay
  // subtle. Day: one moment every 3–7 minutes. Night (lantern hours): every
  // 8–15 — a hallway lamp stirs, it does not perform.
  static constexpr uint32_t DAY_MIN_MS = 3UL * 60000UL;
  static constexpr uint32_t DAY_MAX_MS = 7UL * 60000UL;
  static constexpr uint32_t NIGHT_MIN_MS = 8UL * 60000UL;
  static constexpr uint32_t NIGHT_MAX_MS = 15UL * 60000UL;

  void seed(uint32_t s) {
    rng_ = s ? s : 0xC0FFEEu;
    next_at_ms_ = 0;   // re-schedule from the next allowed tick
  }

  // Advance and maybe emit. `allowed` = the glass is lit and idle (day
  // ambient, or lantern light at night); while false the clock simply holds
  // — a moment never fires into darkness or over an interaction, and the
  // wait restarts from re-allowance (no burst of stored-up moments).
  LifeMoment step(uint32_t now_ms, bool allowed, bool night) {
    if (!allowed) {
      next_at_ms_ = 0;
      return LifeMoment::None;
    }
    if (next_at_ms_ == 0) {
      schedule(now_ms, night);
      return LifeMoment::None;
    }
    if ((int32_t)(now_ms - next_at_ms_) < 0) return LifeMoment::None;
    schedule(now_ms, night);
    // Mostly the bird lives (70%); sometimes the status surfaces (30%).
    return (next_rand() % 10) < 7 ? LifeMoment::Flourish : LifeMoment::Glance;
  }

 private:
  void schedule(uint32_t now_ms, bool night) {
    const uint32_t lo = night ? NIGHT_MIN_MS : DAY_MIN_MS;
    const uint32_t hi = night ? NIGHT_MAX_MS : DAY_MAX_MS;
    next_at_ms_ = now_ms + lo + next_rand() % (hi - lo);
    if (next_at_ms_ == 0) next_at_ms_ = 1;   // 0 is the "unscheduled" mark
  }

  uint32_t next_rand() {
    // xorshift32 — tiny, deterministic, plenty for cadence jitter.
    uint32_t x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
    return x;
  }

  uint32_t rng_ = 0xC0FFEEu;
  uint32_t next_at_ms_ = 0;
};

}  // namespace canary::care
