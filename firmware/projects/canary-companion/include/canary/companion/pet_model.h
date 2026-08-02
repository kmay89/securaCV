// canary/companion/pet_model.h — the Pocket Canary's care loop: a virtual pet
// with Tamagotchi's charm and none of its cruelty. Pure, host-testable (no
// Arduino, no LVGL, no radio).
//
// Design: docs/design/canary_companion.md §4. Read that before changing a
// constant here — every number below is an argument, not a taste.
//
// ── What the 1996 Tamagotchi actually did ────────────────────────────────────
//
// The P1 ran three visible gauges (hunger 0–4 hearts, happiness 0–4 hearts,
// discipline 0–100%), rang an attention bell when a gauge emptied, and counted
// a CARE MISTAKE if the bell went unanswered for ~15 minutes. Care mistakes and
// discipline mistakes together picked which adult you got — good care grew the
// beloved Mametchi, bad care grew the squat, short-lived Tarakotchi. Neglect it
// far enough and it got sick, then it died, and the death was YOURS: a single
// owner, solely responsible, with a grave on the screen.
//
// That last mechanic is the one that sold forty million units and the one that
// got the toy banned from classrooms. It is not a nice thing to hand a child.
//
// ── What this engine keeps, and what it refuses ──────────────────────────────
//
// KEEPS (the charm is real and worth having):
//   * Needs that move on their own, so the pet has a life between visits.
//   * A visible response to care — the bird is legibly better after you help.
//   * Growth you cannot rush, so a three-week-old bird IS different.
//   * Rare, earned flourishes: rarity is the reward.
//
// REFUSES (each of these is enforced by a test in tests_host/):
//   * DEATH. `PetState` has no dead state and no lifespan. The floor is
//     `Stage::Fledgling` and a long sleep. The bird always comes back.
//   * GUILT. Neglect never produces reproach — it produces a QUIETER bird that
//     brightens the moment you return. There is no sad face aimed at the child.
//   * THE HOURLY LEASH. Needs move on a school-day scale (see NEED_*_PER_HOUR),
//     not a 60-minute one, and the pet sleeps through the night with the child.
//   * VARIABLE REWARD. Nothing here is random. The engine is a pure function of
//     (elapsed time, care events) — the same inputs always give the same bird,
//     which is also what makes it host-testable.
//   * STREAK PUNISHMENT. A missed day costs one rung, never the ladder.
//
// ── The two channels, and why they must not mix ──────────────────────────────
//
// This repo already has a living canary that is a GAUGE, not a toy
// (docs/hardware/display_living_canary.md): every face maps 1:1 to a state a
// log line can name, because an experienced user reads the face instead of the
// log. A pet that invents feelings would destroy that property.
//
// So the Pocket Canary runs two channels that never blend:
//
//   THE WEATHER (real)  — fleet health, straight from `care::BirdMood`. Honest,
//                         diagnostic, never faked. Owns the bird's POSTURE.
//   THE BOND    (play)  — this file. The child's own care history. Owns the
//                         bird's GROWTH and its flourishes. Never claims to be
//                         system state, and is never rendered in the Weather's
//                         vocabulary.
//
// They meet at exactly one point, in one direction: real household hygiene
// (`Care::HouseholdTended`) feeds the Bond as a BONUS. A healthy, attended home
// grows a bird faster. But a *sick* home never costs the child a thing — see
// `pet_tick()`'s comment on why the penalty direction is deliberately absent.
// A child must never be made to feel responsible for a red fleet.

#ifndef CANARY_COMPANION_PET_MODEL_H
#define CANARY_COMPANION_PET_MODEL_H

#include <stdint.h>

namespace canary {
namespace companion {

// ── Needs ────────────────────────────────────────────────────────────────────
//
// Three needs, each 0..NEED_MAX, where MAX is contentment and 0 is "would
// really like some attention". The P1 used four hearts; we use a wider range
// because the decay is slower and a 0–4 gauge would visibly lurch.
//
// The names are deliberately soft. The P1 called it HUNGER, which frames the
// child as the reason a creature is starving. Ours are states of the bird, not
// accusations aimed at the owner.

static constexpr uint8_t NEED_MAX = 24;

// A need at or below this is what the bird will ask about when you next look.
// It is NOT an alarm and it does not ring: see `PetAsk` below.
static constexpr uint8_t NEED_LOW = 8;

// The floor a need can decay to. Deliberately NOT zero: a need that bottoms out
// completely is what makes a Tamagotchi feel like an emergency, and there are
// no emergencies here. The bird gets quiet and sleepy, never desperate.
static constexpr uint8_t NEED_FLOOR = 2;

struct Needs {
  uint8_t fed = NEED_MAX;    // seed and water        (P1: hunger)
  uint8_t spark = NEED_MAX;  // play and company      (P1: happiness)
  uint8_t rest = NEED_MAX;   // sleep, spent by play  (no P1 equivalent)
};

// Decay per waking hour. The P1 emptied a heart roughly every 30–60 minutes,
// which is what forced the once-an-hour check that got it confiscated in
// classrooms. At 2/hour a fed bird stays content for eight waking hours — a
// school day — and a bird left alone all weekend is sleepy, not in crisis.
static constexpr uint8_t FED_DECAY_PER_HOUR = 2;
static constexpr uint8_t SPARK_DECAY_PER_HOUR = 2;

// Rest recovers on its own and is spent only by play (see pet_play.h). The bird
// resting is the mechanism that ENDS a play session, so it recovers slowly
// enough to be a real bound and fast enough to be back by morning.
static constexpr uint8_t REST_RECOVER_PER_HOUR = 3;

// ── Care events ──────────────────────────────────────────────────────────────

enum class Care : uint8_t {
  Feed = 0,         // seed: the small, always-available kindness
  Water,            // water: the same, on the other need's schedule
  Play,             // a finished play session (pet_play.h)
  Preen,            // sit with the bird and do nothing — costs no need, and
                    // still counts as care. Doing nothing together is care.
  HouseholdTended,  // the one bridge from the real fleet: an alert an adult
                    // acknowledged, or a fully-verified pass. Bonus only.
};

// What each care event restores. Feed and Water are intentionally partial: a
// single tap should not solve the bird, or there is no relationship, just a
// button. Preen restores nothing and still counts toward the Bond.
static constexpr uint8_t FEED_RESTORES = 8;
static constexpr uint8_t WATER_RESTORES = 6;
static constexpr uint8_t PLAY_RESTORES_SPARK = 10;
static constexpr uint8_t PLAY_COSTS_REST = 6;

// ── Overfeeding, without the punishment ──────────────────────────────────────
//
// The P1 punished overfeeding: keep pressing FEED and the pet got fat, then
// sick, then died sooner. That is a trap laid for a seven-year-old who has
// worked out which button makes the animation play.
//
// Here, a full bird simply declines: the seed goes back in the pouch, the bird
// does a small "no thank you" flourish, and NOTHING BAD HAPPENS. The child
// learns the limit from the bird's manners rather than from a punishment they
// only understand three days later.

static constexpr uint8_t FULL_ENOUGH = NEED_MAX - 2;

// ── The ask ──────────────────────────────────────────────────────────────────
//
// The P1 BEEPED. That beep is the engine of the whole guilt loop: it interrupts
// a classroom, it demands, and ignoring it is scored against you forever.
//
// The Pocket Canary never interrupts. `pet_ask()` reports what the bird would
// mention IF THE CHILD HAPPENS TO LOOK — it is a pull, not a push. The runtime
// is forbidden from turning this into a notification (there is no bell, no
// vibration, and no screen wake wired to it), and `test_ask_never_rings`
// exists to keep it that way.

enum class PetAsk : uint8_t {
  Nothing = 0,  // content, or asleep — the common case, by design
  Peckish,      // fed is low
  Thirsty,      // fed is low and water is the older of the two kindnesses
  Bored,        // spark is low
  Sleepy,       // rest is low — the bird is about to end play itself
};

// ── Growth stages ────────────────────────────────────────────────────────────
//
// The P1 ran baby → child → teen → adult on a fixed clock and then killed the
// adult. Ours grows on the same shape and then simply KEEPS GOING: `Elder` is
// not a countdown, it is the destination.
//
// Stage advance is gated on BOND POINTS, not on wall-clock age alone, so a bird
// that is genuinely lived-with grows faster than a bird in a drawer — but the
// minimum-days gate below means it can never be rushed in an afternoon of
// button-mashing. Both gates must pass.

enum class Stage : uint8_t {
  Hatchling = 0,  // day 0        — big head, unsteady, sleeps constantly
  Fledgling,      // day 1+       — the floor: neglect can never go below here
  Songbird,       // day 4+       — the first real personality
  Companion,      // day 10+      — full plumage, the earned flourishes unlock
  Elder,          // day 30+      — calm, rare, and permanent
};

static constexpr uint8_t STAGE_COUNT = 5;

// Minimum days at the previous stage before the next is even considered.
// Index by the stage you are LEAVING.
static constexpr uint16_t STAGE_MIN_DAYS[STAGE_COUNT] = {1, 3, 6, 20, 0};

// Bond points needed to leave each stage. Reachable by ordinary daily care —
// roughly a week of a few visits a day gets a Companion — and NOT reachable by
// grinding, because `bond_add()` rations points per day (see BOND_DAILY_CAP).
static constexpr uint16_t STAGE_BOND_GATE[STAGE_COUNT] = {6, 40, 160, 600, 0};

// ── The Bond ─────────────────────────────────────────────────────────────────

static constexpr uint8_t BOND_FOR_CARE = 2;
static constexpr uint8_t BOND_FOR_PLAY = 4;
static constexpr uint8_t BOND_FOR_HOUSEHOLD = 3;

// The anti-grind rule, and the anti-addiction rule, in one constant. Once the
// day's bond is earned, further care still WORKS (the bird is still fed, still
// happier) but stops paying growth. There is no reason to keep tapping.
//
// A device whose reward curve goes flat is a device a child puts down. That is
// the intended outcome. It is the opposite of a daily-login streak, and it is
// the single most important number in this file.
static constexpr uint16_t BOND_DAILY_CAP = 30;

// ── State ────────────────────────────────────────────────────────────────────

struct PetState {
  Needs needs;
  Stage stage = Stage::Hatchling;
  uint16_t bond = 0;            // total, monotonic — never decreases
  uint16_t bond_today = 0;      // resets at the local-day rollover
  uint16_t days_alive = 0;      // local days since hatch
  uint16_t days_at_stage = 0;   // days since the last advance
  bool asleep = false;          // follows the child's quiet hours
  uint8_t warmth = 0;           // 0..WARMTH_MAX — see below
  uint8_t last_feed_kind = 0;   // 0 = seed next, 1 = water next (alternates)
};

// Warmth is the "has been visited lately" scalar, and it is the ONLY thing that
// decays from absence. It exists so a returning child gets a visible, immediate
// welcome (a cold bird warms up fast and conspicuously) rather than a scolding.
//
// Note what it is NOT: it is not scored, not gated on, and no growth depends on
// it. Absence costs nothing permanent. That is the whole point.
static constexpr uint8_t WARMTH_MAX = 12;
static constexpr uint8_t WARMTH_PER_VISIT = 4;
static constexpr uint8_t WARMTH_DECAY_PER_DAY = 3;

// ── Engine ───────────────────────────────────────────────────────────────────

inline uint8_t need_sub(uint8_t v, uint8_t d) {
  const int r = static_cast<int>(v) - static_cast<int>(d);
  return r < NEED_FLOOR ? NEED_FLOOR : static_cast<uint8_t>(r);
}

inline uint8_t need_add(uint8_t v, uint8_t d) {
  const int r = static_cast<int>(v) + static_cast<int>(d);
  return r > NEED_MAX ? NEED_MAX : static_cast<uint8_t>(r);
}

// One waking hour of drift. A sleeping bird does not get hungry — the child is
// asleep too, and a pet that decays overnight is a pet that greets a
// seven-year-old with a deficit they had no way to prevent.
inline void pet_tick_hour(PetState& p) {
  if (p.asleep) {
    // Night is for recovering, not for accruing debt.
    p.needs.rest = need_add(p.needs.rest, REST_RECOVER_PER_HOUR);
    return;
  }
  p.needs.fed = need_sub(p.needs.fed, FED_DECAY_PER_HOUR);
  p.needs.spark = need_sub(p.needs.spark, SPARK_DECAY_PER_HOUR);
  p.needs.rest = need_add(p.needs.rest, REST_RECOVER_PER_HOUR);
}

// True if the bird would decline this care right now. Declining is a MANNER,
// not a failure: the caller shows a small "no thank you" and moves on.
inline bool pet_would_decline(const PetState& p, Care c) {
  switch (c) {
    case Care::Feed:
    case Care::Water:
      return p.needs.fed >= FULL_ENOUGH;
    case Care::Play:
      // A tired bird ends play itself. This is the device's own off-switch,
      // spoken in the bird's voice rather than as a lockout dialog.
      return p.needs.rest < PLAY_COSTS_REST;
    case Care::Preen:
    case Care::HouseholdTended:
      return false;  // always welcome
  }
  return false;
}

// The ceiling `bond` saturates at. At the permitted 30 points a day, a uint16_t
// fills in about six years — which is well inside the life of a device whose
// entire premise is a bird you keep. Wrapping there would take a six-year-old
// Elder's bond from 65535 to single digits, and "monotonic, never decreases" is
// a promise this file makes in three places and a test asserts.
//
// So the add saturates. Growth gates top out in the hundreds (STAGE_BOND_GATE),
// so nothing above this ceiling is ever read for a decision — the counter is a
// keepsake number, and a keepsake number that wraps is worse than one that
// stops.
static constexpr uint16_t BOND_MAX = 65535;

// Award bond, respecting the daily cap. Returns what was actually awarded.
inline uint8_t bond_add(PetState& p, uint8_t points) {
  if (p.bond_today >= BOND_DAILY_CAP) return 0;
  const uint16_t room = static_cast<uint16_t>(BOND_DAILY_CAP - p.bond_today);
  const uint8_t give = points < room ? points : static_cast<uint8_t>(room);
  const uint16_t headroom = static_cast<uint16_t>(BOND_MAX - p.bond);
  const uint8_t banked = give < headroom ? give : static_cast<uint8_t>(headroom);
  p.bond = static_cast<uint16_t>(p.bond + banked);
  // The daily counter still advances by the full award even when the lifetime
  // total is saturated: the daily cap is the anti-grind rule, and it must keep
  // flattening the reward curve on a six-year-old device exactly as it does on
  // a six-day-old one.
  p.bond_today = static_cast<uint16_t>(p.bond_today + give);
  return give;
}

// Apply one act of care. Returns false if the bird declined (nothing changed
// except that the child got a small piece of characterful feedback).
inline bool pet_care(PetState& p, Care c) {
  if (pet_would_decline(p, c)) return false;

  switch (c) {
    case Care::Feed:
      p.needs.fed = need_add(p.needs.fed, FEED_RESTORES);
      p.last_feed_kind = 1;
      bond_add(p, BOND_FOR_CARE);
      break;
    case Care::Water:
      p.needs.fed = need_add(p.needs.fed, WATER_RESTORES);
      p.last_feed_kind = 0;
      bond_add(p, BOND_FOR_CARE);
      break;
    case Care::Play:
      p.needs.spark = need_add(p.needs.spark, PLAY_RESTORES_SPARK);
      p.needs.rest = need_sub(p.needs.rest, PLAY_COSTS_REST);
      bond_add(p, BOND_FOR_PLAY);
      break;
    case Care::Preen:
      // Restores no need on purpose. Sitting with someone is not a transaction,
      // and a child who works out that Preen is "the useless button" has
      // learned the wrong lesson — so it pays the Bond like everything else.
      bond_add(p, BOND_FOR_CARE);
      break;
    case Care::HouseholdTended:
      bond_add(p, BOND_FOR_HOUSEHOLD);
      break;
  }

  // Every visit warms the bird, including a declined one's sibling acts.
  p.warmth = p.warmth + WARMTH_PER_VISIT > WARMTH_MAX
                 ? WARMTH_MAX
                 : static_cast<uint8_t>(p.warmth + WARMTH_PER_VISIT);
  return true;
}

// What the bird would mention if the child looked right now. Pull, never push.
inline PetAsk pet_ask(const PetState& p) {
  if (p.asleep) return PetAsk::Nothing;  // never wake a sleeping bird
  if (p.needs.rest < PLAY_COSTS_REST) return PetAsk::Sleepy;
  if (p.needs.fed <= NEED_LOW) {
    return p.last_feed_kind == 1 ? PetAsk::Thirsty : PetAsk::Peckish;
  }
  if (p.needs.spark <= NEED_LOW) return PetAsk::Bored;
  return PetAsk::Nothing;
}

// The local-day rollover: the only place growth is decided and the only place
// anything decays from absence.
inline void pet_day_rollover(PetState& p) {
  p.bond_today = 0;
  p.days_alive = static_cast<uint16_t>(p.days_alive + 1);
  p.days_at_stage = static_cast<uint16_t>(p.days_at_stage + 1);

  // Warmth is the one absence-sensitive scalar, and it costs nothing permanent.
  p.warmth = p.warmth > WARMTH_DECAY_PER_DAY
                 ? static_cast<uint8_t>(p.warmth - WARMTH_DECAY_PER_DAY)
                 : 0;

  // Growth: BOTH gates, so it can be neither rushed nor missed.
  const uint8_t s = static_cast<uint8_t>(p.stage);
  if (s + 1 < STAGE_COUNT) {
    if (p.days_at_stage >= STAGE_MIN_DAYS[s] && p.bond >= STAGE_BOND_GATE[s]) {
      p.stage = static_cast<Stage>(s + 1);
      p.days_at_stage = 0;
    }
  }

  // The floor. A bird left in a drawer for a year is a Fledgling that has been
  // asleep — never a Hatchling again, and never gone. There is deliberately no
  // branch here that lowers `stage`, and `test_stage_never_regresses` walks a
  // year of total neglect to prove it.
  if (p.stage == Stage::Hatchling && p.days_alive >= 1) {
    p.stage = Stage::Fledgling;
  }
}

}  // namespace companion
}  // namespace canary

#endif  // CANARY_COMPANION_PET_MODEL_H
