// Field-I/O decision logic — pure, host-testable core (no Arduino, no HAL).
//
// The runtime layer (canary/io/field_io.cpp, gated on HAS_ISOLATED_IO) reads the
// 4.3B's two isolated inputs and drives an isolated output; this header holds the
// two decisions worth testing without a board: input debouncing (reject contact
// bounce / bus glitches before an edge counts) and the bounded siren controller
// (drive an alert output only while a real unacked alert stands, never forever).
//
// Provenance note: a display has NO signing identity — it verifies others'
// chains, it never mints its own (see src/trust.cpp + the mqtt health comment).
// So a contact read here becomes an honestly UNSIGNED local report, exactly like
// the fleet model's on_chirp/on_beacon observations — never a forged witness.

#ifndef CANARY_IO_FIELD_IO_LOGIC_H
#define CANARY_IO_FIELD_IO_LOGIC_H

#include <cstdint>

namespace canary {
namespace io {
namespace field {

// Result of feeding one sample to a Debounce.
enum class Edge : uint8_t { None, Rising, Falling };

// N-sample debouncer for one boolean line (already de-inverted: `raw == true`
// means the contact is ACTIVE). A new level must persist for `need` consecutive
// samples before it becomes the stable state and emits an edge; a shorter blip
// is rejected. Matches the playground's 2-sample contact debounce.
struct Debounce {
  bool stable = false;   // current committed level
  bool cand = false;     // level currently being counted toward
  uint8_t count = 0;     // consecutive samples of `cand`

  Edge update(bool raw, uint8_t need) {
    if (need == 0) need = 1;
    if (raw != cand) {
      cand = raw;
      count = 1;
    } else if (count < 255) {
      count++;
    }
    if (count >= need && stable != cand) {
      stable = cand;
      return stable ? Edge::Rising : Edge::Falling;
    }
    return Edge::None;
  }
};

// Bounded siren/strobe controller for an isolated output.
//
// Drives while a real alert stands unacknowledged, but never indefinitely: after
// `max_on_ms` of continuous drive it caps OFF (neighbour-friendly, and a stuck
// alert can't hold a physical output on forever). It re-arms only once the alert
// clears or is acked — so a *fresh* alert can sound again, but the same standing
// one can't re-trigger without human acknowledgement.
//
// `armed` is the user's opt-in: a disarmed siren behaves exactly like a resolved
// alert (never drives, always re-armed), so the on-glass toggle can hold the
// physical output silent while the alert still shows and journals. Defaulted true
// so the pure host tests and any armed-by-policy caller read unchanged.
struct SirenController {
  bool on = false;
  bool capped = false;
  uint32_t on_since = 0;

  // `alerting` = fleet worst severity is at/above the alert threshold.
  // Returns true if the output should conduct this tick.
  bool update(uint32_t now, bool alerting, bool acked, uint32_t max_on_ms,
              bool armed = true) {
    if (!armed || acked || !alerting) {  // disarmed/resolved -> release, re-arm
      on = false;
      capped = false;
      return false;
    }
    if (capped) return false;  // already ran its bounded course this episode
    if (!on) {
      on = true;
      on_since = now;
      return true;
    }
    if ((uint32_t)(now - on_since) >= max_on_ms) {
      on = false;
      capped = true;
      return false;
    }
    return true;
  }
};

}  // namespace field
}  // namespace io
}  // namespace canary

#endif  // CANARY_IO_FIELD_IO_LOGIC_H
