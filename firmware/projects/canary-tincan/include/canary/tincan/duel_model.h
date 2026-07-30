// canary/tincan/duel_model.h — the step duel: one integer each way, reset
// every midnight, no history. Pure, host-testable.
//
// Design: docs/design/canary_tincan_kids_watch.md §6.3.
//
// Three decisions here are deliberate anti-features, and they are the reason
// this file is short:
//
//  1. NO HISTORY. Yesterday is gone at midnight. There is no streak to break,
//     no chart, no trend, and nothing to feel bad about. A streak that punishes
//     a sick day is an engagement mechanic pointed at a child, and the design
//     refuses those outright.
//
//  2. THE CHEAT IS ADMITTED, NOT POLICED. Shaking your wrist counts, the
//     firmware says so, and the daily cap is visible. A cheat you can see is a
//     game — siblings will race each other shaking their arms and that is
//     fine. A hidden cheat detector is an argument with a parent about whether
//     the watch is lying, and the watch always loses that argument.
//
//  3. STEPS NEVER LEAVE THE PAIR. One integer goes to one sibling. Nothing is
//     stored off-wrist, aggregated, or exported. The 2025 COPPA amendments made
//     biometric identifiers personal information; the cleanest compliance
//     posture is not to hold any.

#ifndef CANARY_TINCAN_DUEL_MODEL_H
#define CANARY_TINCAN_DUEL_MODEL_H

#include <stdint.h>

namespace canary {
namespace tincan {

// A generous ceiling, well past what a child walks in a day. It exists so a
// stuck IMU or an afternoon of deliberate shaking tops out visibly instead of
// producing an absurd number that makes the whole game feel fake.
static constexpr uint32_t DUEL_DAILY_CAP = 60000;

// Rate ceiling: steps credited per second. Real sprinting is ~4/s; 8 leaves
// headroom for a genuinely fast kid while flattening a shaken wrist.
static constexpr uint32_t DUEL_MAX_STEPS_PER_SEC = 8;

enum class DuelStanding : uint8_t {
  Even = 0,
  Ahead,
  Behind,
  Unknown,  // the peer has not reported today — say so, don't imply a win
};

struct Duel {
  uint32_t my_steps = 0;
  uint32_t peer_steps = 0;
  bool peer_reported = false;

  // Local day index (days since epoch, computed by the runtime from the RTC).
  // Stored so a reset happens on the first update after midnight rather than
  // needing a timer to fire at exactly the right moment.
  uint32_t day = 0;

  uint32_t last_credit_ms = 0;
  bool primed = false;

  // Roll over if the day changed. Returns true if a reset happened, so the UI
  // can show the one flourish this game gets: a fresh start.
  bool roll_day(uint32_t today) {
    if (day == today) return false;
    day = today;
    my_steps = 0;
    peer_steps = 0;
    peer_reported = false;
    return true;
  }

  // Credit steps observed since the last call, rate-limited and capped.
  // Returns how many were actually credited — the UI shows the credited
  // number, never the raw one, so what a kid sees is what counts.
  uint32_t credit(uint32_t raw_steps, uint32_t now_ms) {
    if (!primed) {
      primed = true;
      last_credit_ms = now_ms;
      // Drop the first batch: it spans an unknown window (boot, or a wake from
      // sleep), so crediting it at full value would reward being switched off.
      return 0;
    }

    const uint32_t elapsed_ms = now_ms - last_credit_ms;
    last_credit_ms = now_ms;

    // Allow at least one step through so slow walking is never silently lost
    // to integer division on short polling intervals.
    uint32_t allowance = (elapsed_ms * DUEL_MAX_STEPS_PER_SEC) / 1000;
    if (allowance == 0) allowance = 1;

    uint32_t credited = raw_steps < allowance ? raw_steps : allowance;
    const uint32_t room = (my_steps >= DUEL_DAILY_CAP)
                              ? 0
                              : (DUEL_DAILY_CAP - my_steps);
    if (credited > room) credited = room;

    my_steps += credited;
    return credited;
  }

  void on_peer_steps(uint32_t steps) {
    peer_steps = steps > DUEL_DAILY_CAP ? DUEL_DAILY_CAP : steps;
    peer_reported = true;
  }

  bool capped() const { return my_steps >= DUEL_DAILY_CAP; }

  DuelStanding standing() const {
    if (!peer_reported) return DuelStanding::Unknown;
    if (my_steps > peer_steps) return DuelStanding::Ahead;
    if (my_steps < peer_steps) return DuelStanding::Behind;
    return DuelStanding::Even;
  }

  // The gap, always as a positive magnitude — the UI adds the direction from
  // standing(). Zero when the peer has not reported, because inventing a lead
  // against silence is the kind of small lie that makes a kid distrust the
  // whole device.
  uint32_t gap() const {
    if (!peer_reported) return 0;
    return my_steps > peer_steps ? (my_steps - peer_steps)
                                 : (peer_steps - my_steps);
  }
};

inline const char* duel_standing_token(DuelStanding s) {
  switch (s) {
    case DuelStanding::Ahead:  return "ahead";
    case DuelStanding::Behind: return "behind";
    case DuelStanding::Even:   return "even";
    case DuelStanding::Unknown:
    default:                   return "unknown";
  }
}

}  // namespace tincan
}  // namespace canary

#endif  // CANARY_TINCAN_DUEL_MODEL_H
