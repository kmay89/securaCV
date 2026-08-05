// include/canary/care/nightlight.h — the nightlight's companion visits,
// pure model (no Arduino, no clock reads, no storage — host-tested in
// tests_host/test_nightlight.cpp).
//
// The Canary Nightlight is a bedside clock with a friend living in it. Most
// of the time the glass is a quiet 7-segment clock over a soft lamp wash;
// every so often the companion VISITS — takes the stage for a few seconds
// with a small piece of theater matched to the time of day — and then gives
// the clock back. This model decides WHEN a visit happens and WHICH kind;
// the face (ui/nightlight_ui.cpp) owns how it looks.
//
// Honesty rules, inherited from the living-canary wave (canary_mark.h: the
// bird is a gauge, not a toy):
//
//   1. A visit is STAGING, never a face. The bird's expression stays owned
//      by the mood engine (care/bird_mood.h) — a visit moves the bird and
//      dresses the scene, it never paints a happy face over a Worried one.
//      The caller gates `allowed` on calm + idle, so a visit can't even
//      begin while anything wants attention.
//   2. Asleep is asleep. During quiet hours the companion sleeps beside the
//      clock; night "visits" are limited to the rare, motionless Moonwatch
//      (a caption and a slow wash swell — the bird itself never stirs).
//      Never startle a sleeping bird.
//   3. Deterministic. Seeded xorshift, same idiom as care/ambient_life.h:
//      the same seed and tick sequence produce the same visits, which is
//      what makes the layer host-testable and bench-reproducible.
//
// Cadence: a visit every 6–14 minutes by day (a companion checks in; it
// does not perform), every 25–45 at night, each lasting ~12 s. The caller's
// `allowed` gate holds the schedule rather than banking it — no burst of
// stored-up visits after an hour of interaction.
#pragma once
#include <stdint.h>

namespace canary::care {

// What the companion does when it takes the stage. Kinds are picked by the
// local hour (minute-of-day), so the friend keeps the household's rhythm.
enum class VisitKind : uint8_t {
  None = 0,
  Stretch,    // morning (06–10): up with you — a slow rise to center stage
  Song,       // midday (10–17): a little tune — the wash ripples with it
  Curious,    // afternoon/any: drifts over, peers at the room, drifts back
  Winddown,   // evening (17 to quiet hours): settling lower, dimmer scene
  Moonwatch,  // night, rare: the bird stays asleep; the lamp swells gently
};

struct Visit {
  VisitKind kind = VisitKind::None;
  uint32_t until_ms = 0;   // stage returns to the clock at this time
};

class NightlightVisits {
 public:
  static constexpr uint32_t DAY_MIN_MS   = 6UL * 60000UL;
  static constexpr uint32_t DAY_MAX_MS   = 14UL * 60000UL;
  static constexpr uint32_t NIGHT_MIN_MS = 25UL * 60000UL;
  static constexpr uint32_t NIGHT_MAX_MS = 45UL * 60000UL;
  static constexpr uint32_t VISIT_MS     = 12000UL;

  void seed(uint32_t s) {
    rng_ = s ? s : 0xB1BDu;
    next_at_ms_ = 0;
    live_ = Visit{};
  }

  // Advance the schedule and return the CURRENT visit (kind None when the
  // clock owns the stage). `allowed` = lit + idle + calm, the same gate
  // ambient-life uses — while false the schedule holds and any live visit
  // ends immediately (an alarm mid-visit takes the stage back this pass).
  // `minute_of_day` is local wall time, or -1 while the clock is unsynced
  // (then only Curious visits happen — the friend has no schedule to keep
  // without a clock, and the face is already saying the time is unknown).
  // `night` = inside quiet hours.
  Visit step(uint32_t now_ms, int minute_of_day, bool night, bool allowed) {
    if (!allowed) {
      next_at_ms_ = 0;
      live_ = Visit{};
      return live_;
    }
    if (live_.kind != VisitKind::None) {
      if ((int32_t)(now_ms - live_.until_ms) >= 0) live_ = Visit{};
      return live_;
    }
    if (next_at_ms_ == 0) {
      schedule(now_ms, night);
      return live_;
    }
    if ((int32_t)(now_ms - next_at_ms_) < 0) return live_;
    schedule(now_ms, night);
    live_.kind = pick(minute_of_day, night);
    live_.until_ms = now_ms + VISIT_MS;
    return live_;
  }

  // Exposed for tests: the kind the given wall minute would pick.
  VisitKind pick(int minute_of_day, bool night) const {
    if (night) return VisitKind::Moonwatch;
    if (minute_of_day < 0) return VisitKind::Curious;
    const int hh = minute_of_day / 60;
    if (hh >= 6 && hh < 10) return VisitKind::Stretch;
    if (hh >= 10 && hh < 17) {
      // Midday alternates song and curiosity — a friend, not a jukebox.
      return (minute_of_day & 1) ? VisitKind::Song : VisitKind::Curious;
    }
    if (hh >= 17 && hh < 22) return VisitKind::Winddown;
    return VisitKind::Curious;
  }

 private:
  void schedule(uint32_t now_ms, bool night) {
    const uint32_t lo = night ? NIGHT_MIN_MS : DAY_MIN_MS;
    const uint32_t hi = night ? NIGHT_MAX_MS : DAY_MAX_MS;
    next_at_ms_ = now_ms + lo + next_rand() % (hi - lo);
    if (next_at_ms_ == 0) next_at_ms_ = 1;
  }

  uint32_t next_rand() {
    uint32_t x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
    return x;
  }

  uint32_t rng_ = 0xB1BDu;
  uint32_t next_at_ms_ = 0;
  Visit live_;
};

// The words the visits are allowed to say on the glass — one tested surface,
// same discipline as care/greeting.h: lowercase, short, never a severity
// word (the honest lines belong to the glance line, not the friend), and
// nothing Montserrat can't draw.
inline const char* visit_caption(VisitKind k) {
  switch (k) {
    case VisitKind::Stretch:   return "good morning";
    case VisitKind::Song:      return "a little song";
    case VisitKind::Curious:   return "just checking in";
    case VisitKind::Winddown:  return "winding down";
    case VisitKind::Moonwatch: return "sweet dreams";
    default:                   return "";
  }
}

}  // namespace canary::care
