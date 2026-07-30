// canary/tincan/tie_ceremony.h — tying a string: the parent-opened window, the
// knot both kids compare, and the two-handed confirm. Pure, host-testable.
//
// Design: docs/design/canary_tincan_kids_watch.md §5.6. This is where "no
// strangers" stops being a policy and becomes a mechanism.
//
// The gate is the WINDOW. A watch will not tie anything unless a parent has
// opened a tie window from the household hub, on the home LAN, in the last two
// minutes. That single fact is what makes "you cannot tie a string to a
// stranger's watch in a playground" structural rather than a rule a child
// could break: the hub is not there, so the window cannot be open, so the code
// path is not reachable. There are no invite codes, no directory, and no
// discovery of watches outside the household.
//
// The ceremony itself is deliberately physical and memorable:
//
//   1. A parent opens the window.
//   2. The two kids hold their watches face to face. Each shows a KNOT — four
//      glyphs derived from the freshly-agreed shared secret. This is a short
//      authentication string, the same idea as comparing a safety number,
//      drawn as pictures because the users are six.
//   3. If the knots match, BOTH kids press and hold for three seconds, at the
//      same time.
//
// Kids remember a ceremony. They do not remember a settings toggle. And the
// knot comparison is what makes a man-in-the-middle visible to a child: an
// attacker who intercepts the exchange cannot make both watches show the same
// four pictures.

#ifndef CANARY_TINCAN_TIE_CEREMONY_H
#define CANARY_TINCAN_TIE_CEREMONY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace canary {
namespace tincan {

// How long a parent's permission lasts. Long enough for two children to walk
// into the same room; short enough that a window left open by accident is not
// a standing invitation.
static constexpr uint32_t TIE_WINDOW_MS = 2 * 60 * 1000;

// Both kids must hold for this long, together.
static constexpr uint32_t TIE_CONFIRM_HOLD_MS = 3000;

// How far apart the two presses may start and still count as "together".
// Generous, because coordinating a simultaneous press with a sibling is part
// of the fun and should not be a dexterity test.
static constexpr uint32_t TIE_CONFIRM_SKEW_MS = 1500;

// The knot: four glyphs from a fixed pictogram set.
static constexpr size_t KNOT_GLYPHS = 4;
static constexpr uint8_t KNOT_ALPHABET = 16;  // 4 bits per glyph, 16 bits total

// 16 glyphs is 65,536 knots. That is a one-in-65,536 chance an active attacker
// gets a matching pair on a single try — and they only get one try, because a
// failed compare ends the ceremony and the window is measured in minutes.
// More glyphs would buy strength a six-year-old cannot spend: the binding
// constraint is how many pictures a child can reliably compare, not entropy.
struct Knot {
  uint8_t glyph[KNOT_GLYPHS] = {0, 0, 0, 0};

  bool equals(const Knot& o) const {
    return memcmp(glyph, o.glyph, KNOT_GLYPHS) == 0;
  }
};

// Derive the knot from the HKDF output both peers computed with
// LINK_INFO_KNOT. Both sides feed in identical bytes and therefore draw
// identical pictures; a man-in-the-middle, holding two different secrets,
// cannot.
//
// Takes the first two bytes and splits them into four nibbles. Needs at least
// 2 bytes; anything shorter is a derivation bug, not a short key.
inline bool knot_from_secret(const uint8_t* sas, size_t len, Knot& out) {
  if (!sas || len < 2) return false;
  out.glyph[0] = (uint8_t)((sas[0] >> 4) & 0x0F);
  out.glyph[1] = (uint8_t)(sas[0] & 0x0F);
  out.glyph[2] = (uint8_t)((sas[1] >> 4) & 0x0F);
  out.glyph[3] = (uint8_t)(sas[1] & 0x0F);
  return true;
}

// ---------------------------------------------------------------------------
// The ceremony state machine
// ---------------------------------------------------------------------------

enum class TieStage : uint8_t {
  Idle = 0,       // no window; the tie path is not reachable
  WindowOpen,     // a parent said yes; looking for a partner
  Exchanging,     // key agreement in progress
  ShowKnot,       // both watches showing four glyphs; humans compare
  Confirming,     // at least one kid is holding
  Tied,           // done
  Failed,         // knots differed, hold broke, window closed, or table full
};

enum class TieFailure : uint8_t {
  None = 0,
  NoWindow,        // the gate did its job
  WindowExpired,
  KnotMismatch,    // the interesting one: a human caught something
  HoldBroken,
  NoSlot,          // this watch already holds its maximum strings
  SelfPair,        // identical public keys — a bug or a loopback
};

inline const char* tie_failure_token(TieFailure f) {
  switch (f) {
    case TieFailure::NoWindow:      return "no-window";
    case TieFailure::WindowExpired: return "window-expired";
    case TieFailure::KnotMismatch:  return "knot-mismatch";
    case TieFailure::HoldBroken:    return "hold-broken";
    case TieFailure::NoSlot:        return "no-slot";
    case TieFailure::SelfPair:      return "self-pair";
    case TieFailure::None:
    default:                        return "none";
  }
}

struct TieCeremony {
  TieStage stage = TieStage::Idle;
  TieFailure failure = TieFailure::None;

  uint32_t window_opened_ms = 0;
  bool window_open = false;

  Knot mine;
  bool knot_ready = false;

  // Confirm hold tracking. `peer_hold_ms` is when the far watch reported its
  // press starting; both are needed because the ceremony is two-handed.
  uint32_t my_hold_start_ms = 0;
  uint32_t peer_hold_start_ms = 0;
  bool my_hold = false;
  bool peer_hold = false;

  void reset() { *this = TieCeremony(); }

  // A parent opened the gate.
  void open_window(uint32_t now_ms) {
    reset();
    window_open = true;
    window_opened_ms = now_ms;
    stage = TieStage::WindowOpen;
  }

  bool window_valid(uint32_t now_ms) const {
    if (!window_open) return false;
    return (now_ms - window_opened_ms) < TIE_WINDOW_MS;
  }

  void fail(TieFailure why) {
    stage = TieStage::Failed;
    failure = why;
    my_hold = peer_hold = false;
  }

  // May a tie even be attempted right now? The gate, in one call.
  bool may_tie(uint32_t now_ms) const { return window_valid(now_ms); }

  // Begin key agreement. Refuses outside an open window — this is the
  // structural stranger gate, and it is checked here rather than by callers so
  // there is exactly one place to audit.
  bool begin_exchange(uint32_t now_ms, bool have_slot) {
    if (!window_open) {
      fail(TieFailure::NoWindow);
      return false;
    }
    if (!window_valid(now_ms)) {
      fail(TieFailure::WindowExpired);
      return false;
    }
    if (!have_slot) {
      fail(TieFailure::NoSlot);
      return false;
    }
    stage = TieStage::Exchanging;
    return true;
  }

  // Key agreement finished; show the humans their four glyphs.
  bool present_knot(const Knot& k, uint32_t now_ms) {
    if (stage != TieStage::Exchanging) return false;
    if (!window_valid(now_ms)) {
      fail(TieFailure::WindowExpired);
      return false;
    }
    mine = k;
    knot_ready = true;
    stage = TieStage::ShowKnot;
    return true;
  }

  // A kid says the knots match (or don't). The mismatch path is the one that
  // matters: it is a human detecting an active attacker, and it ends the
  // ceremony rather than offering a retry that would let them try again.
  bool knots_match(bool matched) {
    if (stage != TieStage::ShowKnot) return false;
    if (!matched) {
      fail(TieFailure::KnotMismatch);
      return false;
    }
    stage = TieStage::Confirming;
    return true;
  }

  void hold_start(uint32_t now_ms) {
    if (stage != TieStage::Confirming) return;
    my_hold = true;
    my_hold_start_ms = now_ms;
  }

  void hold_release() {
    if (stage != TieStage::Confirming) return;
    my_hold = false;
  }

  void peer_hold_start(uint32_t now_ms) {
    if (stage != TieStage::Confirming) return;
    peer_hold = true;
    peer_hold_start_ms = now_ms;
  }

  void peer_hold_release() {
    if (stage != TieStage::Confirming) return;
    peer_hold = false;
  }

  // Are both hands on, and have they been on together for long enough?
  //
  // The hold is measured from the LATER of the two starts, so one kid pressing
  // early does not shorten the shared three seconds — the ceremony is about
  // doing it together, and a shortcut here would quietly remove the part that
  // makes it memorable.
  bool confirmed(uint32_t now_ms) const {
    if (stage != TieStage::Confirming) return false;
    if (!my_hold || !peer_hold) return false;
    const uint32_t a = my_hold_start_ms;
    const uint32_t b = peer_hold_start_ms;
    const uint32_t skew = (a > b) ? (a - b) : (b - a);
    if (skew > TIE_CONFIRM_SKEW_MS) return false;
    const uint32_t later = (a > b) ? a : b;
    return (now_ms - later) >= TIE_CONFIRM_HOLD_MS;
  }

  // Drive the state machine. Call every loop pass while a ceremony is live.
  TieStage step(uint32_t now_ms) {
    if (stage == TieStage::Tied || stage == TieStage::Failed) return stage;
    if (window_open && !window_valid(now_ms)) {
      fail(TieFailure::WindowExpired);
      return stage;
    }
    if (stage == TieStage::Confirming && confirmed(now_ms)) {
      stage = TieStage::Tied;
    }
    return stage;
  }
};

}  // namespace tincan
}  // namespace canary

#endif  // CANARY_TINCAN_TIE_CEREMONY_H
