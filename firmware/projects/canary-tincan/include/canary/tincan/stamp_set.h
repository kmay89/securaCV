// canary/tincan/stamp_set.h — the sixteen stamps: the Tin Can's entire
// non-rhythmic vocabulary. Pure, host-testable.
//
// Design: docs/design/canary_tincan_kids_watch.md §5.4. Sixteen fixed
// pictograms, and that is the whole set. The fixed vocabulary is doing three
// jobs at once:
//
//   * NO READING AGE. A four-year-old and a ten-year-old use the same device.
//     A picture ring works for both; a text field works for neither.
//   * NO SCALABLE CRUELTY. You cannot spell anything with sixteen pictures.
//     There is no moderation surface here because there is nothing to moderate.
//   * NO KEYBOARD, EVER. Once a device has a text field it grows autocomplete,
//     then emoji, then a reason to store history. Declining the first step
//     declines all of them.
//
// Each stamp carries its own haptic and (where an output transducer exists) a
// chime phrase, so a stamp is recognisable in a pocket without looking. Ids
// are a stable wire contract: APPEND ONLY, never reorder — a reordered table
// would silently turn one child's "sandwich" into another's "bedtime" across a
// firmware version boundary.

#ifndef CANARY_TINCAN_STAMP_SET_H
#define CANARY_TINCAN_STAMP_SET_H

#include <stddef.h>
#include <stdint.h>

namespace canary {
namespace tincan {

enum class Stamp : uint8_t {
  Bird = 0,       // the canary — "it's me"
  Wave = 1,       // hello / bye
  Question = 2,   // "what?" — the workhorse
  Yes = 3,
  No = 4,
  Heart = 5,
  Laugh = 6,
  Ball = 7,       // come and play
  Controller = 8,
  Bricks = 9,     // building something
  Sandwich = 10,  // food, hunger, snack
  Moon = 11,      // tired / bedtime
  Star = 12,      // "look at this"
  Rain = 13,      // weather, or a mood
  Five = 14,      // "five minutes" — the one number kids actually need
  Secret = 15,    // a keyhole: "come here, don't tell"
  Count = 16,
};

// How hard the far wrist is nudged. Kept small and blunt: three levels is what
// a person can distinguish through a strap without training.
enum class StampHaptic : uint8_t {
  Tick = 0,    // one light click
  Double,      // two quick clicks
  Rise,        // a short ramp — used for the attention-seeking stamps
};

struct StampDef {
  Stamp id;
  const char* token;     // stable, greppable; the UI localizes from the enum
  StampHaptic haptic;
  bool loud;             // may make a sound when sound is enabled and fitted
};

// The table. Order matches the enum exactly; the host test asserts it.
inline const StampDef* stamp_table() {
  static const StampDef k[] = {
      {Stamp::Bird,       "bird",       StampHaptic::Tick,   false},
      {Stamp::Wave,       "wave",       StampHaptic::Tick,   false},
      {Stamp::Question,   "question",   StampHaptic::Double, true},
      {Stamp::Yes,        "yes",        StampHaptic::Tick,   false},
      {Stamp::No,         "no",         StampHaptic::Double, false},
      {Stamp::Heart,      "heart",      StampHaptic::Rise,   false},
      {Stamp::Laugh,      "laugh",      StampHaptic::Double, false},
      {Stamp::Ball,       "ball",       StampHaptic::Rise,   true},
      {Stamp::Controller, "controller", StampHaptic::Rise,   true},
      {Stamp::Bricks,     "bricks",     StampHaptic::Tick,   false},
      {Stamp::Sandwich,   "sandwich",   StampHaptic::Rise,   true},
      {Stamp::Moon,       "moon",       StampHaptic::Tick,   false},
      {Stamp::Star,       "star",       StampHaptic::Double, false},
      {Stamp::Rain,       "rain",       StampHaptic::Tick,   false},
      {Stamp::Five,       "five",       StampHaptic::Double, false},
      {Stamp::Secret,     "secret",     StampHaptic::Tick,   false},
  };
  return k;
}

inline constexpr size_t stamp_count() { return (size_t)Stamp::Count; }

inline bool stamp_valid(uint8_t raw) { return raw < (uint8_t)Stamp::Count; }

inline const StampDef* stamp_def(Stamp s) {
  if (!stamp_valid((uint8_t)s)) return nullptr;
  return &stamp_table()[(size_t)s];
}

inline const char* stamp_token(Stamp s) {
  const StampDef* d = stamp_def(s);
  return d ? d->token : "?";
}

// Wire: one byte. A stamp is the cheapest thing the radio ever carries.
inline size_t stamp_encode(Stamp s, uint8_t* out, size_t cap) {
  if (!out || cap < 1) return 0;
  if (!stamp_valid((uint8_t)s)) return 0;
  out[0] = (uint8_t)s;
  return 1;
}

// Total: an unknown stamp id is refused, not rendered as a placeholder. A
// future firmware that adds stamp 16 must not make today's watches draw a
// question mark and pretend it received something meaningful.
inline bool stamp_decode(const uint8_t* in, size_t len, Stamp& out) {
  if (!in || len != 1) return false;
  if (!stamp_valid(in[0])) return false;
  out = (Stamp)in[0];
  return true;
}

}  // namespace tincan
}  // namespace canary

#endif  // CANARY_TINCAN_STAMP_SET_H
