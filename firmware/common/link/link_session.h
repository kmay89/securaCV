// common/link/link_session.h — a Canary Link session: who is A, who is B,
// which key label each direction uses, what the counters are doing, and
// whether the session has been revoked. Pure, host-testable.
//
// A "session" is one peering between exactly two nodes. In the Tin Can it is a
// *string* between two kids' watches (docs/design/canary_tincan_kids_watch.md);
// in an industrial deployment it is a link between two devices that agreed to
// talk. Nothing here knows or cares which.
//
// The three decisions that live here:
//
//  1. ROLE ASSIGNMENT IS DERIVED, NEVER NEGOTIATED. Both ends compare the two
//     X25519 public keys byte-wise; the lexicographically smaller one is A.
//     Both sides reach the same answer with no extra round trip, no tie-break
//     message, and nothing for an attacker to influence. Identical keys are
//     rejected outright — that is either a bug or a peer talking to itself.
//
//  2. THE TWO DIRECTIONS GET DIFFERENT KEYS. The shared secret is expanded
//     through HKDF into K_AB and K_BA using distinct, versioned info strings.
//     This is the primary defense against the nonce-reuse trap described in
//     link_frame.h: the two send streams never share a key, so even a nonce
//     construction bug cannot collide across directions. The HKDF itself is
//     the runtime's job (mbedtls); this header owns the *labels*, because a
//     label typo is exactly the kind of silent break that a host test catches
//     and a bench test does not.
//
//  3. REVOCATION IS A PERSISTED STATE, NOT A COMMAND. Cutting a string cannot
//     reach a lost or powered-off watch, and the two peers can still hear each
//     other over router-independent ESP-NOW. So a cut is something the
//     SURVIVING peer records durably and then enforces locally, and the
//     operator's UI reports it as *pending* until that record exists. A
//     revoked session refuses frames forever, survives reboot, and cannot be
//     un-revoked by anything arriving over the air.

#ifndef CANARY_LINK_LINK_SESSION_H
#define CANARY_LINK_LINK_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "link_frame.h"
#include "link_replay.h"

namespace canary {
namespace link {

static constexpr size_t LINK_PUBKEY_LEN = 32;  // X25519
static constexpr size_t LINK_KEY_LEN = 32;     // AEAD key

// HKDF info labels. Versioned on purpose: changing a derivation without
// changing its label would let two firmware generations agree on a key they
// compute differently. Both peers derive BOTH keys; each sends under its own.
static constexpr const char* LINK_INFO_A_TO_B = "securacv/link/v1/a->b";
static constexpr const char* LINK_INFO_B_TO_A = "securacv/link/v1/b->a";

// Label for the short authentication string ("the knot") the two humans
// compare during pairing. Separate from the traffic keys so a knot can never
// be confused with, or leak, key material.
static constexpr const char* LINK_INFO_KNOT = "securacv/link/v1/knot";

enum class SessionState : uint8_t {
  Empty = 0,    // slot unused
  Pending,      // key agreed, knot shown, not yet confirmed by both humans
  Active,       // confirmed; frames flow
  Revoked,      // cut. Terminal, durable, never reversible over the air.
};

// Compare two X25519 public keys. Returns <0, 0, >0 like memcmp. Constant-time
// is NOT required here: public keys are public, and the comparison's result is
// already broadcast implicitly by which direction each peer sends on.
inline int link_pubkey_cmp(const uint8_t a[LINK_PUBKEY_LEN],
                           const uint8_t b[LINK_PUBKEY_LEN]) {
  return memcmp(a, b, LINK_PUBKEY_LEN);
}

// Which role does the holder of `mine` take against `theirs`? Returns false if
// the keys are identical (a peer must never pair with itself) — the caller
// must abandon the pairing rather than guess.
inline bool link_assign_role(const uint8_t mine[LINK_PUBKEY_LEN],
                             const uint8_t theirs[LINK_PUBKEY_LEN],
                             bool& i_am_a) {
  const int c = link_pubkey_cmp(mine, theirs);
  if (c == 0) return false;
  i_am_a = (c < 0);
  return true;
}

// Given a role, which direction do I send on and which do I expect to receive?
inline Dir link_send_dir(bool i_am_a) {
  return i_am_a ? Dir::AtoB : Dir::BtoA;
}
inline Dir link_recv_dir(bool i_am_a) {
  return i_am_a ? Dir::BtoA : Dir::AtoB;
}

// Which HKDF label produces the key used for a given direction. Both peers
// agree because both know the roles.
inline const char* link_info_for_dir(Dir dir) {
  return dir == Dir::AtoB ? LINK_INFO_A_TO_B : LINK_INFO_B_TO_A;
}

// One peering. Fixed size, no allocation — a node holds a small array of these.
struct Session {
  SessionState state = SessionState::Empty;
  uint16_t session_id = 0;
  bool i_am_a = false;

  uint8_t peer_pub[LINK_PUBKEY_LEN] = {0};
  uint8_t key_send[LINK_KEY_LEN] = {0};
  uint8_t key_recv[LINK_KEY_LEN] = {0};

  SendCounter tx;
  ReplayWindow rx;

  Dir send_dir() const { return link_send_dir(i_am_a); }
  Dir recv_dir() const { return link_recv_dir(i_am_a); }

  bool usable() const { return state == SessionState::Active; }

  // Wipe everything. Used when a slot is recycled; NOT what revocation does —
  // a revoked slot must keep its identity so it can keep saying no.
  void clear() {
    state = SessionState::Empty;
    session_id = 0;
    i_am_a = false;
    memset(peer_pub, 0, sizeof(peer_pub));
    memset(key_send, 0, sizeof(key_send));
    memset(key_recv, 0, sizeof(key_recv));
    tx = SendCounter();
    rx.reset();
  }

  // Cut this session. Terminal: keys are zeroed so a later bug cannot use
  // them, the identity is kept so inbound frames are refused by ID, and no
  // over-the-air message can ever move the state back.
  void revoke() {
    state = SessionState::Revoked;
    memset(key_send, 0, sizeof(key_send));
    memset(key_recv, 0, sizeof(key_recv));
    tx = SendCounter();
    rx.reset();
  }
};

// Should this parsed header be admitted to `s` for tag verification?
//
// Everything cheap and certain happens here, BEFORE any crypto: right session,
// right direction (a frame carrying my own send direction is either a
// reflection or an attacker replaying my traffic at me — never legitimate),
// session actually active, counter not obviously replayed. The counter is only
// *committed* after the tag verifies; see ReplayWindow::accept.
inline bool link_admit(const Session& s, const LinkHeader& h) {
  if (!s.usable()) return false;
  if (h.session_id != s.session_id) return false;
  if (h.dir != s.recv_dir()) return false;
  if (!s.rx.would_accept(h.ctr)) return false;
  return true;
}

// A fixed-capacity set of sessions. Small on purpose: a string is a
// relationship, not a follower count. The Tin Can caps kid strings at 6.
template <size_t N>
struct SessionTable {
  Session slots[N];

  static constexpr size_t capacity() { return N; }

  Session* find(uint16_t session_id) {
    for (size_t i = 0; i < N; i++) {
      if (slots[i].state != SessionState::Empty &&
          slots[i].session_id == session_id) {
        return &slots[i];
      }
    }
    return nullptr;
  }

  const Session* find(uint16_t session_id) const {
    for (size_t i = 0; i < N; i++) {
      if (slots[i].state != SessionState::Empty &&
          slots[i].session_id == session_id) {
        return &slots[i];
      }
    }
    return nullptr;
  }

  // A free slot, or nullptr. Revoked slots are NOT free: they are still doing
  // a job (refusing a cut peer). Reclaiming one is an explicit operator act.
  Session* alloc() {
    for (size_t i = 0; i < N; i++) {
      if (slots[i].state == SessionState::Empty) return &slots[i];
    }
    return nullptr;
  }

  size_t count_active() const {
    size_t n = 0;
    for (size_t i = 0; i < N; i++) {
      if (slots[i].state == SessionState::Active) n++;
    }
    return n;
  }
};

}  // namespace link
}  // namespace canary

#endif  // CANARY_LINK_LINK_SESSION_H
