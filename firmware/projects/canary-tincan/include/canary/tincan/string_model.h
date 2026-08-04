// canary/tincan/string_model.h — the strings a watch holds: whether each one
// is taut, who is holding on, and how often it may be buzzed. Pure,
// host-testable.
//
// A "string" is the kid-facing face of a canary::link session. The tin-can
// research this design is built on (docs/design/canary_tincan_kids_watch.md §2)
// gives the model its shape, and the shape is the UI:
//
//   TAUT   the peer is reachable — the line on screen is drawn tight
//   SLACK  the peer is not — the line sags, visibly, with no words
//   CUT    revoked — and it stays cut
//
// A six-year-old reads a sagging line in about a second and needs no
// vocabulary for it. That is the entire reason link state is a picture here
// rather than a "last seen 14:02" string.
//
// Two behaviors worth testing without hardware:
//
//  * THE COOLDOWN IS SHOWN TO THE SENDER, NOT THE RECEIVER. Sibling knock-spam
//    is real and the obvious fixes are all wrong: a block list teaches a kid to
//    cut a sibling off, a silent drop teaches them the device is broken. A
//    visible "the string is still buzzing" on the *sender's* screen is legible
//    to a small child, self-correcting, and needs no adjudication.
//
//  * BOTH-HOLDING IS THE FEATURE. When two kids hold their screens at the same
//    moment, both watches lock into a shared pulse. It is a boolean each way
//    and it is the moment the product is actually about.

#ifndef CANARY_TINCAN_STRING_MODEL_H
#define CANARY_TINCAN_STRING_MODEL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace canary {
namespace tincan {

// A relationship, not a follower count. Six is already more siblings than most
// houses have, and a cap this low is what keeps the UI a list you can see at
// once rather than something you scroll.
static constexpr size_t TINCAN_MAX_STRINGS = 6;

// A kid's whole identity: three glyph indices they pick at first boot. No name,
// no birthday, no photo, no account. Nothing here is personal information
// because there is no personal information to hold.
static constexpr size_t MARK_GLYPHS = 3;

// Knock cooldown. Long enough that a rapid-fire kid feels the brake, short
// enough that a real conversation of knocks still flows.
static constexpr uint32_t KNOCK_COOLDOWN_MS = 2500;

// A tug is a live gesture, so it expires on its own: if the peer's hold
// message stops arriving, they let go (or walked away) and the pulse must
// stop. Never latch a held state waiting for an explicit release that may
// never come.
static constexpr uint32_t TUG_HOLD_TIMEOUT_MS = 1200;

enum class StringState : uint8_t {
  Empty = 0,
  Pending,   // tied but not yet confirmed by both kids
  Taut,      // peer reachable
  Slack,     // peer not reachable
  Cut,       // revoked — terminal
};

// The color a kid picks for their canary. Ordering is a stable wire contract.
enum class BirdColor : uint8_t {
  Sun = 0,
  Sky,
  Leaf,
  Berry,
  Ember,
  Snow,
  Count,
};

struct Mark {
  BirdColor color = BirdColor::Sun;
  uint8_t glyph[MARK_GLYPHS] = {0, 0, 0};
};

struct String {
  StringState state = StringState::Empty;
  uint16_t session_id = 0;
  uint16_t peer_node = 0;
  Mark peer_mark;

  // Liveness, fed from canary::link::NodeTable.
  bool peer_reachable = false;

  // Tug: who is holding right now.
  bool i_hold = false;
  uint32_t peer_hold_until_ms = 0;

  // Knock rate limiting, sender side.
  uint32_t knock_ready_at_ms = 0;

  bool tied() const {
    return state == StringState::Taut || state == StringState::Slack;
  }

  // Both hands on the string. The one that matters.
  bool both_holding(uint32_t now_ms) const {
    return i_hold && peer_holding(now_ms);
  }

  bool peer_holding(uint32_t now_ms) const {
    if (peer_hold_until_ms == 0) return false;
    return (int32_t)(peer_hold_until_ms - now_ms) > 0;
  }

  bool may_knock(uint32_t now_ms) const {
    if (state != StringState::Taut) return false;
    if (knock_ready_at_ms == 0) return true;
    return (int32_t)(knock_ready_at_ms - now_ms) <= 0;
  }

  // Milliseconds left on the cooldown, for the sender's own progress ring.
  uint32_t knock_cooldown_left(uint32_t now_ms) const {
    if (knock_ready_at_ms == 0) return 0;
    const int32_t left = (int32_t)(knock_ready_at_ms - now_ms);
    return left > 0 ? (uint32_t)left : 0;
  }

  void on_knock_sent(uint32_t now_ms) {
    knock_ready_at_ms = now_ms + KNOCK_COOLDOWN_MS;
  }

  void on_peer_hold(uint32_t now_ms) {
    peer_hold_until_ms = now_ms + TUG_HOLD_TIMEOUT_MS;
  }

  // Fold in reachability. Pending and Cut are NOT touched: a string that was
  // never confirmed must not become Taut just because the peer is on the air,
  // and a cut string must never come back for any reason at all.
  void on_reachable(bool reachable) {
    peer_reachable = reachable;
    if (state == StringState::Taut || state == StringState::Slack) {
      state = reachable ? StringState::Taut : StringState::Slack;
    }
    if (!reachable) {
      peer_hold_until_ms = 0;  // they cannot still be holding if they are gone
    }
  }

  void cut() {
    state = StringState::Cut;
    i_hold = false;
    peer_hold_until_ms = 0;
  }

  void clear() { *this = String(); }
};

// How tight to draw the line, 0..255. Pure presentation, but it lives here so
// the on-device UI and any simulator agree exactly.
//
// Slack is not zero: a tied-but-unreachable string is still a string, and
// drawing nothing would read as "you have no sister", which is a different and
// much worse message than "she is out of range".
inline uint8_t string_tension(const String& s, uint32_t now_ms) {
  switch (s.state) {
    case StringState::Taut:
      if (s.both_holding(now_ms)) return 255;
      if (s.i_hold || s.peer_holding(now_ms)) return 200;
      return 150;
    case StringState::Slack:
      return 40;
    case StringState::Pending:
      return 90;
    case StringState::Cut:
    case StringState::Empty:
    default:
      return 0;
  }
}

struct StringBook {
  String strings[TINCAN_MAX_STRINGS];
  Mark me;

  String* find_by_session(uint16_t session_id) {
    for (size_t i = 0; i < TINCAN_MAX_STRINGS; i++) {
      if (strings[i].state != StringState::Empty &&
          strings[i].session_id == session_id) {
        return &strings[i];
      }
    }
    return nullptr;
  }

  String* alloc() {
    for (size_t i = 0; i < TINCAN_MAX_STRINGS; i++) {
      if (strings[i].state == StringState::Empty) return &strings[i];
    }
    return nullptr;
  }

  size_t count_tied() const {
    size_t n = 0;
    for (size_t i = 0; i < TINCAN_MAX_STRINGS; i++) {
      if (strings[i].tied()) n++;
    }
    return n;
  }

  size_t count_taut() const {
    size_t n = 0;
    for (size_t i = 0; i < TINCAN_MAX_STRINGS; i++) {
      if (strings[i].state == StringState::Taut) n++;
    }
    return n;
  }

  bool full() const {
    for (size_t i = 0; i < TINCAN_MAX_STRINGS; i++) {
      if (strings[i].state == StringState::Empty) return false;
    }
    return true;
  }
};

}  // namespace tincan
}  // namespace canary

#endif  // CANARY_TINCAN_STRING_MODEL_H
