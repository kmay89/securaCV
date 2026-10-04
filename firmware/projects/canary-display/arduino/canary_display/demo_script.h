// Demo storyline — pure, host-testable core (no Arduino, no LVGL, no clock).
//
// Demo mode (docs/hardware/display_modes.md §Demo) is the REAL product code
// rendering a story: a fixed cast of synthetic witnesses and a looping
// timeline of beats, fed through the real FleetModel into the real faces.
// Nothing is mocked downstream of this table — if the demo looks good, the
// product is good, and if a UI regression lands, the demo shows it.
//
// Honesty rails (enforced by the host test + the runtime):
//   - The glass wears a persistent "DEMO" chip; a demo can never impersonate
//     a live fleet (mode_policy(Demo).banner).
//   - Witness ids use the reserved "demo-" prefix — they can never collide
//     with a real device id on a real broker (and demo mode never has a
//     broker anyway: mode_policy(Demo).network == false).
//   - Every beat's `intent` severity is drift-locked to the REAL classifier:
//     the host test runs each event name through fleet_model.cpp's
//     classify_event() and fails if the storyline and the vocabulary ever
//     disagree. A renamed event can't quietly turn the demo's alarm beat
//     into a shrug.
//   - The loop resolves: the last beat returns the story to all-quiet, so a
//     shelf unit looping all day doesn't hold a standing alarm across the
//     wrap (and the ~2 min cadence exercises every severity tier + ack UX).

#ifndef CANARY_MODE_DEMO_SCRIPT_H
#define CANARY_MODE_DEMO_SCRIPT_H

#include <stdint.h>

#include "canary/fleet/fleet_model.h"

namespace canary {
namespace mode {

// The cast: a believable four-canary household. Rooms match the names &
// rooms wave's retained-meta rendering ("Name · room" on detail lines).
struct DemoWitness {
  const char* id;      // reserved "demo-" prefix, never a real device id
  const char* name;    // friendly name, as a meta payload would carry
  const char* room;
  uint8_t     battery;  // steady percentage the health row shows
};

inline constexpr DemoWitness DEMO_CAST[] = {
    {"demo-front-door", "Front Door", "entry", 96},
    {"demo-kitchen",    "Kitchen",    "kitchen", 88},
    {"demo-garage",     "Garage",     "garage", 71},
    {"demo-nursery",    "Nursery",    "nursery", 93},
};
inline constexpr uint8_t DEMO_CAST_COUNT =
    sizeof(DEMO_CAST) / sizeof(DEMO_CAST[0]);

// One beat: at `at_s` seconds into the loop, witness `witness` reports
// `event`. `intent` documents the severity the story MEANS — and is what the
// host test pins against the real classifier, so it can never silently lie.
struct DemoBeat {
  uint16_t     at_s;
  uint8_t      witness;  // index into DEMO_CAST
  const char*  event;    // real fleet vocabulary (classify_event grammar)
  fleet::Sev   intent;
};

// The storyline (loop length DEMO_LOOP_S). Arc: a quiet house wakes, routine
// presence, a doorbell moment, an after-hours warning, the one alarm spike a
// demo must show (with time to demonstrate hold-to-ack), then tamper — the
// ladder's top — and a clean resolve back to all-quiet before the wrap.
inline constexpr uint16_t DEMO_LOOP_S = 150;

inline constexpr DemoBeat DEMO_BEATS[] = {
    {5,   1, "boot",                 fleet::Sev::Ok},      // kitchen checks in
    {15,  1, "presence_detected",    fleet::Sev::Notice},  // morning stir
    {30,  3, "presence_detected",    fleet::Sev::Notice},  // nursery stirs too
    {45,  0, "doorbell",             fleet::Sev::Warn},    // someone's here
    {60,  0, "cleared",              fleet::Sev::Ok},      // they left
    {75,  2, "after_hours_motion",   fleet::Sev::Warn},    // garage, odd hour
    {90,  2, "glass_break",          fleet::Sev::Alert},   // the alarm beat
    {110, 2, "tamper_contact",       fleet::Sev::Tamper},  // ladder top
    {135, 2, "cleared",              fleet::Sev::Ok},      // resolve
};
inline constexpr uint16_t DEMO_BEAT_COUNT =
    sizeof(DEMO_BEATS) / sizeof(DEMO_BEATS[0]);

// ── Loop stepping ───────────────────────────────────────────────────────────
// Which beats fire in the half-open window (prev_s, now_s], on the looping
// clock? The runtime calls this once per tick with its last position; beats
// land exactly once per lap, including across the wrap. Returns the count
// written to `out` (indices into DEMO_BEATS, capped at `cap`).
//
// Positions are loop-local seconds (0..DEMO_LOOP_S-1); the runtime keeps
// them that way by moduloing its uptime. prev_s == now_s means no time has
// passed (never "a whole lap") — a lap takes a wrap: prev_s > now_s.

inline uint8_t demo_beats_between(uint16_t prev_s, uint16_t now_s,
                                  uint16_t* out, uint8_t cap) {
  uint8_t n = 0;
  for (uint16_t i = 0; i < DEMO_BEAT_COUNT && n < cap; i++) {
    const uint16_t at = DEMO_BEATS[i].at_s;
    bool fire;
    if (prev_s == now_s) {
      fire = false;
    } else if (prev_s < now_s) {
      fire = (at > prev_s) && (at <= now_s);
    } else {  // wrapped: (prev_s, LOOP) then [0, now_s]
      fire = (at > prev_s) || (at <= now_s);
    }
    if (fire) out[n++] = i;
  }
  return n;
}

}  // namespace mode
}  // namespace canary

#endif  // CANARY_MODE_DEMO_SCRIPT_H
