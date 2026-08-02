// canary/companion/pet_play.h — the two things you can actually DO with the
// Pocket Canary, and the rule that makes them stop. Pure, host-testable.
//
// Design: docs/design/canary_companion.md §4.4.
//
// ── What was wrong with the original game ────────────────────────────────────
//
// The 1996 Tamagotchi's only game was left-or-right: the pet faces a direction,
// you guess which, you are right half the time. That is a coin toss with a
// sprite on it. It cannot be practised, it cannot be understood, and getting
// better at it is not possible — which means the only thing driving a child
// back to it is the reward schedule, not the game. It is, precisely, a slot
// machine for seven-year-olds.
//
// Both games here are chosen against that. They are games of SKILL that a child
// visibly improves at, they are deterministic (the same round played the same
// way scores the same), and neither has a random reward. If a child comes back
// it is because they got better at something, which is the only reason worth
// engineering for.
//
// ── The rule that makes them stop ────────────────────────────────────────────
//
// Play costs the bird's `rest`, and `rest` recovers slowly (pet_model.h). After
// a few rounds the bird is tired and declines — the device says no, in the
// bird's own voice, and the child has been told a story rather than shown a
// lockout. `MAX_ROUNDS_PER_SESSION` is the hard ceiling underneath that soft
// one, so a session ends even if a caller mismanages the need.
//
// A companion device that pushes you away when it has had enough is the whole
// design brief. Everything else here is decoration on that.

#ifndef CANARY_COMPANION_PET_PLAY_H
#define CANARY_COMPANION_PET_PLAY_H

#include <stdint.h>

namespace canary {
namespace companion {

enum class Game : uint8_t {
  Echo = 0,  // the bird taps a rhythm; you tap it back
  Steady,    // hold the glass level while the bird preens on your wrist
};

// A session is a handful of rounds, then the bird is done. Small on purpose.
static constexpr uint8_t MAX_ROUNDS_PER_SESSION = 5;

// ── Echo ─────────────────────────────────────────────────────────────────────
//
// The bird plays a short rhythm on whatever channel it has (haptic if a motor
// is fitted, otherwise the glass — haptic_voice.h decides), and the child taps
// it back on the screen or by knocking the case.
//
// This deliberately reuses the Tin Can's reasoning about rhythm
// (canary/tincan/knock_codec.h): a human wrist cannot reproduce 3 ms of
// precision, so gaps are compared on a tolerance, and the tolerance LOOSENS for
// younger play rather than the pattern getting shorter. A four-year-old and a
// ten-year-old should play the same game at different difficulties, not two
// different games.

static constexpr uint8_t ECHO_MIN_TAPS = 2;
static constexpr uint8_t ECHO_MAX_TAPS = 6;

// How far off a gap may be and still count, by difficulty rung. These are
// generous by the standards of a rhythm game and that is the point: this is a
// bird you are playing with, not a metronome you are auditioning for.
static constexpr uint16_t ECHO_TOLERANCE_MS[3] = {320, 220, 140};  // easy/mid/hard

// The pattern for round `n` at a given difficulty. Deterministic by
// construction — there is no RNG in this file at all. The sequence is a fixed
// table walked by round, so a child can genuinely LEARN round three.
//
// Gaps are in ms, `taps` long minus one, in the same layout as knock_codec.
struct EchoPattern {
  uint8_t taps = 0;
  uint16_t gap_ms[ECHO_MAX_TAPS - 1] = {0};
};

// A small hand-picked table of rhythms that are pleasant to feel: even pairs,
// a long-short limp, a triplet, a heartbeat. Not generated — generated rhythms
// feel like noise, and the whole point is that these become familiar.
inline EchoPattern echo_pattern(uint8_t round, uint8_t difficulty) {
  static const uint16_t kSeed[MAX_ROUNDS_PER_SESSION][ECHO_MAX_TAPS - 1] = {
      {400, 0, 0, 0, 0},          // two taps, even
      {300, 600, 0, 0, 0},        // short-long
      {250, 250, 500, 0, 0},      // triplet then a rest
      {200, 500, 200, 500, 0},    // heartbeat
      {300, 300, 300, 300, 600},  // a run, then a landing
  };
  static const uint8_t kTaps[MAX_ROUNDS_PER_SESSION] = {2, 3, 4, 5, 6};

  EchoPattern p;
  const uint8_t r = round < MAX_ROUNDS_PER_SESSION ? round : MAX_ROUNDS_PER_SESSION - 1;
  p.taps = kTaps[r];
  // Harder difficulties play the same rhythms faster. The SHAPE is preserved,
  // which is what lets a child who learned round three at easy recognise it.
  const uint16_t num = difficulty >= 2 ? 7 : (difficulty == 1 ? 85 : 100);
  const uint16_t den = difficulty >= 2 ? 10 : 100;
  for (uint8_t i = 0; i + 1 < p.taps; i++) {
    p.gap_ms[i] = static_cast<uint16_t>(kSeed[r][i] * num / den);
  }
  return p;
}

// Score one attempt. Returns the number of gaps within tolerance.
//
// Note what is NOT here: no pass/fail, no streak, no "you lost". The caller
// renders "three of four" and the bird is pleased either way — a child who
// matched two gaps out of five did something real and is told so.
inline uint8_t echo_score(const EchoPattern& want, const uint16_t* got_gaps,
                          uint8_t got_taps, uint8_t difficulty) {
  if (want.taps < ECHO_MIN_TAPS) return 0;
  const uint16_t tol = ECHO_TOLERANCE_MS[difficulty < 3 ? difficulty : 2];
  const uint8_t n = (got_taps < want.taps ? got_taps : want.taps);
  uint8_t hits = 0;
  for (uint8_t i = 0; i + 1 < n; i++) {
    const int diff = static_cast<int>(got_gaps[i]) - static_cast<int>(want.gap_ms[i]);
    const int mag = diff < 0 ? -diff : diff;
    if (mag <= static_cast<int>(tol)) hits++;
  }
  return hits;
}

// ── Steady ───────────────────────────────────────────────────────────────────
//
// Hold your wrist level and still while the bird preens. The glass shows a
// bubble level; the IMU is the input. It is the calmest possible thing to do
// with a motion sensor and it is deliberately the opposite of the shake-based
// games this hardware invites — a device that teaches a child to whip their arm
// around is a device that ends up thrown across a room.
//
// It is also the one game that works with the screen almost entirely dark,
// which makes it the game the Night Watch can offer at 3 a.m. without lighting
// a bedroom.

// How level counts as level, in milli-g of off-axis tilt.
static constexpr int16_t STEADY_TOLERANCE_MG = 180;
// How long you must hold it, per round. Rises gently across a session.
static constexpr uint16_t STEADY_HOLD_MS[MAX_ROUNDS_PER_SESSION] = {1500, 2000, 2500,
                                                                    3000, 4000};

struct SteadyRound {
  uint32_t held_ms = 0;
  uint32_t last_ms = 0;
  bool have_last = false;
  bool complete = false;
};

// Feed tilt magnitude (milli-g away from level) and a timestamp. Returns true
// on the tick the round completes.
//
// Losing the hold does NOT reset to zero — it stops the clock. A child who gets
// most of the way there and sneezes keeps their progress, because resetting to
// zero on a wobble is how a calm game becomes a frustrating one.
inline bool steady_feed(SteadyRound& r, int16_t tilt_mg, uint32_t now_ms,
                        uint8_t round) {
  if (r.complete) return false;
  if (!r.have_last) {
    r.last_ms = now_ms;
    r.have_last = true;
    return false;
  }
  const uint32_t dt = now_ms - r.last_ms;
  r.last_ms = now_ms;

  const int16_t mag = tilt_mg < 0 ? static_cast<int16_t>(-tilt_mg) : tilt_mg;
  if (mag <= STEADY_TOLERANCE_MG) r.held_ms += dt;

  const uint8_t idx = round < MAX_ROUNDS_PER_SESSION ? round : MAX_ROUNDS_PER_SESSION - 1;
  if (r.held_ms >= STEADY_HOLD_MS[idx]) {
    r.complete = true;
    return true;
  }
  return false;
}

// ── Sessions ─────────────────────────────────────────────────────────────────

struct PlaySession {
  Game game = Game::Echo;
  uint8_t round = 0;
  uint8_t difficulty = 0;  // 0 easy / 1 mid / 2 hard
  bool over = false;
  bool ended_by_bird = false;  // the bird got tired: the good ending
};

// Advance a round. The session ends at the ceiling OR when the bird is tired —
// and which of those happened matters, because only one of them gets to be
// narrated as the bird's own choice.
inline void play_next_round(PlaySession& s, bool bird_is_tired) {
  if (s.over) return;
  s.round = static_cast<uint8_t>(s.round + 1);
  if (bird_is_tired) {
    s.over = true;
    s.ended_by_bird = true;
    return;
  }
  if (s.round >= MAX_ROUNDS_PER_SESSION) s.over = true;
}

}  // namespace companion
}  // namespace canary

#endif  // CANARY_COMPANION_PET_PLAY_H
