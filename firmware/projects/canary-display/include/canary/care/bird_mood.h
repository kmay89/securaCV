// include/canary/care/bird_mood.h — the living canary's feelings, as a
// gauge (display_living_canary.md). Pure logic, no LVGL/Arduino: the same
// bytes run on-device and in host tests.
//
// Two slow scalars, borrowed from the Flipper dolphin but pointed at
// SYSTEM health instead of petting:
//
//   anxiety 0..14  — rises instantly with real trouble (late witnesses,
//                    lost witnesses, a flapping hub link, alerts nobody
//                    acknowledged), decays one point per quiet hour, and
//                    snaps to zero on a fully-verified pass.
//   trust (days)   — consecutive fully-clean days. Unlocks the rare,
//                    relaxed idle flourishes: a long-healthy system is
//                    VISIBLY different from a day-one system.
//
// Honesty rule (the Pwnagotchi property): every face maps 1:1 to state a
// log line can name. No random sadness for variety, no cheerful mask over
// a degraded system.
#pragma once
#include <stdint.h>

namespace canary::care {

struct BirdInputs {
  uint8_t stale_witnesses;   // amber: late past grace
  uint8_t lost_witnesses;    // red: officially missing
  bool    links_down;        // wifi or hub link currently down
  bool    hub_flapping;      // link dropped more than once this hour
  uint8_t unacked_old;       // Warn+ conditions unacknowledged > 12 h
  bool    all_verified;      // every witness fresh AND chain-verified
  bool    night;             // quiet hours
  bool    alarm_unacked;     // live Alert/Tamper, not acknowledged
};

struct BirdMood {
  uint8_t  anxiety = 0;       // 0..14
  uint16_t trust_days = 0;    // consecutive clean days
  bool     day_clean = true;  // no trouble seen since the last rollover
  uint8_t  calm_minutes = 0;  // quiet minutes toward the hourly decay
};

constexpr uint8_t BIRD_ANXIETY_MAX = 14;

// What the system state alone says anxiety should at least be.
inline uint8_t bird_anxiety_floor(const BirdInputs& in) {
  int a = 2 * in.stale_witnesses + 4 * in.lost_witnesses +
          (in.hub_flapping ? 3 : 0) + (in.links_down ? 2 : 0) +
          1 * in.unacked_old;
  if (a > BIRD_ANXIETY_MAX) a = BIRD_ANXIETY_MAX;
  return (uint8_t)a;
}

// Once-per-minute tick: anxiety rises instantly to the floor, decays one
// point per fully-quiet hour above it, and a verified pass clears it.
// Trouble of any kind marks the day dirty for the trust ladder.
inline void bird_mood_minute(BirdMood& m, const BirdInputs& in) {
  const uint8_t floor_a = bird_anxiety_floor(in);
  if (floor_a > m.anxiety) {
    m.anxiety = floor_a;
    m.calm_minutes = 0;
  } else if (in.all_verified && floor_a == 0) {
    m.anxiety = 0;  // the full-pass snap: everything answered and proved
    m.calm_minutes = 0;
  } else if (m.anxiety > floor_a) {
    if (++m.calm_minutes >= 60) {
      m.calm_minutes = 0;
      m.anxiety--;
    }
  } else {
    m.calm_minutes = 0;
  }
  if (floor_a > 0 || in.alarm_unacked) m.day_clean = false;
}

// Local-day rollover: a clean day earns a trust day; a dirty one starts
// the streak over (consecutive means consecutive).
inline void bird_mood_rollover(BirdMood& m) {
  if (m.day_clean) {
    if (m.trust_days < 60000) m.trust_days++;
  } else {
    m.trust_days = 0;
  }
  m.day_clean = true;
}

// The face ladder. Escalation is by silhouette (calm-tech): the serious
// end hands the stage to the instrument UI entirely.
enum class BirdFace : uint8_t {
  Hidden,      // alarm handoff — never cute during a real alarm
  Asleep,      // night: stillness IS the information
  Calm,        // all quiet; the idle pool plays
  Worried,     // anxiety 4..9 — something is late, the bird shows it
  Distressed,  // anxiety 10..14 — visibly unwell, maintenance overdue
};

inline BirdFace bird_face(const BirdMood& m, const BirdInputs& in) {
  if (in.alarm_unacked) return BirdFace::Hidden;
  if (in.night) return BirdFace::Asleep;
  if (m.anxiety >= 10) return BirdFace::Distressed;
  if (m.anxiety >= 4) return BirdFace::Worried;
  return BirdFace::Calm;
}

}  // namespace canary::care
