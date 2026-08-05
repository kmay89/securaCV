// common/link/link_replay.h — the anti-replay window for Canary Link.
// Pure, host-testable; no Arduino, no crypto.
//
// A tag proves a frame was authentic *once*. It says nothing about whether you
// have already seen it. Without a replay window an attacker who records a
// knock at 4 p.m. can play it back at 2 a.m. and the watch will buzz, tag and
// all — which is exactly the failure a kid's device must not have.
//
// This is the standard IPsec/DTLS-shaped sliding window (RFC 4303 §3.4.3):
// remember the highest counter accepted, plus a bitmap of the 64 slots below
// it. Anything at or below the window floor is refused outright; anything
// already marked is refused; anything ahead slides the window forward.
//
// Two deliberate properties:
//
//  - `accept()` MUTATES. It is the commit point, so a caller must only call it
//    after the AEAD tag has verified. Checking a counter before authenticating
//    would let unauthenticated traffic poison the window and lock out the real
//    peer — a free denial-of-service. `would_accept()` exists for tests and
//    diagnostics and never mutates.
//  - There is no "reset on gap". A huge forward jump is legal (it slides the
//    window and clears the bitmap) because a watch that has been off for a day
//    is normal; a *backward* jump never is.

#ifndef CANARY_LINK_LINK_REPLAY_H
#define CANARY_LINK_LINK_REPLAY_H

#include <stdint.h>

namespace canary {
namespace link {

static constexpr uint32_t LINK_REPLAY_WINDOW = 64;

// One window per (session, direction). Cheap enough to hold several: 16 bytes.
struct ReplayWindow {
  uint64_t highest = 0;   // highest counter accepted so far (0 = nothing yet)
  uint64_t bitmap = 0;    // bit i set => (highest - 1 - i) has been seen
  bool primed = false;    // false until the first frame is accepted

  void reset() {
    highest = 0;
    bitmap = 0;
    primed = false;
  }

  // Would this counter be accepted right now? Const — safe to call anywhere.
  bool would_accept(uint64_t ctr) const {
    // Counter 0 is never valid on the wire: senders start at 1, so a
    // zero-initialized or corrupt record can't masquerade as a live frame.
    if (ctr == 0) return false;
    if (!primed) return true;
    if (ctr > highest) return true;
    if (ctr == highest) return false;
    const uint64_t behind = highest - ctr;
    if (behind > LINK_REPLAY_WINDOW) return false;  // below the floor
    return (bitmap & (1ULL << (behind - 1))) == 0;
  }

  // Commit a counter. MUST be called only after the frame's tag has verified.
  // Returns false if it was a replay (and changes nothing).
  bool accept(uint64_t ctr) {
    if (!would_accept(ctr)) return false;

    if (!primed) {
      primed = true;
      highest = ctr;
      bitmap = 0;
      return true;
    }

    if (ctr > highest) {
      const uint64_t jump = ctr - highest;
      // Three cases, and the boundary one is easy to get wrong. `behind` runs
      // 1..LINK_REPLAY_WINDOW and maps to bits 0..63, so a jump of EXACTLY the
      // window size still leaves the old `highest` inside the window, at the
      // last slot. Folding that case in with "jump >= window -> clear" would
      // forget it, and a recorded frame at the old `highest` would then be
      // accepted a second time after ordinary packet loss — a replayable knock.
      // It cannot simply fall out of the shift arithmetic either: shifting a
      // 64-bit value by 64 is undefined behavior, which is exactly why the
      // tempting `>=` was there in the first place.
      if (jump > LINK_REPLAY_WINDOW) {
        bitmap = 0;  // the whole old window really did fall off the back
      } else if (jump == LINK_REPLAY_WINDOW) {
        bitmap = (1ULL << (LINK_REPLAY_WINDOW - 1));  // only the old highest survives
      } else {
        // Shift the old slots down, then mark where `highest` itself now sits.
        bitmap <<= jump;
        bitmap |= (1ULL << (jump - 1));
      }
      highest = ctr;
      return true;
    }

    const uint64_t behind = highest - ctr;
    bitmap |= (1ULL << (behind - 1));
    return true;
  }
};

// ---------------------------------------------------------------------------
// The send side: a counter that must never rewind.
//
// The mirror-image hazard to replay. If a device reboots and resumes counting
// from a stale value, it reuses (key, nonce) pairs — the exact catastrophe
// link_frame.h exists to prevent. So the send counter is persisted in blocks:
// we durably reserve `LINK_CTR_LEASE` values at a time and only touch storage
// when a lease runs out. After an unclean reboot the device resumes from the
// last *reserved* value, not the last *used* one, so it skips forward rather
// than repeating. Skipping is free (the receiver's window slides); repeating
// is fatal.
// ---------------------------------------------------------------------------

// How many counter values one durable write buys. Larger = fewer NVS writes
// (flash wear) but a bigger forward skip after a crash. 256 knocks per write
// is roughly one write per busy afternoon.
static constexpr uint64_t LINK_CTR_LEASE = 256;

// Counters are 64-bit, so exhaustion is theoretical — but "theoretical" is not
// "handled". Past this ceiling the session must be re-tied rather than wrap.
static constexpr uint64_t LINK_CTR_MAX = 0xFFFFFFFFFFFFF000ULL;

struct SendCounter {
  uint64_t next = 1;       // the next value to put on the wire (never 0)
  uint64_t reserved = 0;   // durably promised through this value

  // Restore from persisted state. `persisted_reserved` is whatever was last
  // written durably; we resume ABOVE it, never at it.
  void restore(uint64_t persisted_reserved) {
    reserved = persisted_reserved;
    next = persisted_reserved + 1;
    if (next == 0) next = 1;
  }

  // True when the caller must durably persist `new_reservation()` before the
  // next frame may be sent.
  bool needs_persist() const { return next > reserved; }

  // The value to write to durable storage when needs_persist() is true.
  uint64_t new_reservation() const { return next + LINK_CTR_LEASE - 1; }

  // Record that new_reservation() was written successfully.
  void on_persisted(uint64_t written) {
    if (written > reserved) reserved = written;
  }

  // Take the next counter. Returns false if a persist is still owed or the
  // counter is exhausted — the caller must not send in either case.
  bool take(uint64_t& out) {
    if (next == 0 || next > LINK_CTR_MAX) return false;
    if (needs_persist()) return false;
    out = next++;
    return true;
  }
};

}  // namespace link
}  // namespace canary

#endif  // CANARY_LINK_LINK_REPLAY_H
