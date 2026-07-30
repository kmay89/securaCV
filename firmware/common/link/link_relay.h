// common/link/link_relay.h — should this frame be forwarded, and how?
// Pure, host-testable.
//
// The reason a string works from the treehouse to the kitchen is that the
// Canaries in between are willing to pass a frame along. That makes every node
// a relay, and relaying is where "seamless" quietly becomes "the channel is on
// fire". Three rules keep it honest:
//
//  1. RELAYS ARE BLIND. A forwarded frame is not decrypted, not inspected, not
//     logged. A relay can see that *a* frame for session 0x1234 went past; it
//     cannot see a knock, a stamp, or a doodle. This is what makes it safe for
//     a household Canary — a device with a camera in it — to carry a child's
//     traffic, and it is the property an industrial deployment will want too.
//
//  2. DEDUPE BEFORE FORWARD. In a mesh a frame arrives by several paths at
//     once. Forwarding each copy is how one knock becomes forty. The dedupe
//     cache keys on (session, dir, ctr) — which is exactly the tuple the AEAD
//     nonce is built from, so it is unique per frame by construction.
//
//  3. `hops`/`ttl` ARE UNTRUSTED. They are outside the AEAD (link_frame.h), so
//     anyone can rewrite them. They may be used for loop prevention and
//     NOTHING else: never to decide trust, never to infer distance, never to
//     accept a frame that would otherwise be refused. Treating a mutable field
//     as evidence is the classic mesh security bug.

#ifndef CANARY_LINK_LINK_RELAY_H
#define CANARY_LINK_LINK_RELAY_H

#include <stddef.h>
#include <stdint.h>

#include "link_frame.h"

namespace canary {
namespace link {

// How long a seen-frame record blocks a duplicate. Long enough to cover the
// spread of a multi-path arrival, short enough that a legitimate retransmit
// after a real loss still gets through.
static constexpr uint32_t RELAY_DEDUPE_MS = 4000;

enum class RelayVerdict : uint8_t {
  Forward = 0,   // pass it on, with hops/ttl rewritten
  Duplicate,     // already carried this exact frame
  Expired,       // ttl exhausted — the loop stops here
  Malformed,     // not a well-formed link frame
  Mine,          // addressed to a session this node holds; consume, don't relay
};

// One remembered frame. 16 bytes.
struct RelaySeen {
  bool used = false;
  uint16_t session_id = 0;
  uint8_t dir = 0;
  uint64_t ctr = 0;
  uint32_t at_ms = 0;
};

template <size_t N>
struct RelayCache {
  RelaySeen slots[N];

  static constexpr size_t capacity() { return N; }

  bool seen(uint16_t sid, uint8_t dir, uint64_t ctr, uint32_t now_ms) const {
    for (size_t i = 0; i < N; i++) {
      const RelaySeen& s = slots[i];
      if (!s.used) continue;
      if (now_ms - s.at_ms > RELAY_DEDUPE_MS) continue;  // aged out
      if (s.session_id == sid && s.dir == dir && s.ctr == ctr) return true;
    }
    return false;
  }

  // Remember a frame, reusing the oldest slot. Deliberately unconditional:
  // a full cache overwrites rather than refusing, because forgetting an old
  // frame costs a duplicate while refusing to record a new one costs a storm.
  void remember(uint16_t sid, uint8_t dir, uint64_t ctr, uint32_t now_ms) {
    RelaySeen* victim = nullptr;
    uint32_t oldest = 0;
    for (size_t i = 0; i < N; i++) {
      if (!slots[i].used) {
        victim = &slots[i];
        break;
      }
      const uint32_t age = now_ms - slots[i].at_ms;
      if (!victim || age > oldest) {
        victim = &slots[i];
        oldest = age;
      }
    }
    if (!victim) return;
    victim->used = true;
    victim->session_id = sid;
    victim->dir = dir;
    victim->ctr = ctr;
    victim->at_ms = now_ms;
  }
};

// Decide what to do with an inbound frame, from the relay's point of view.
//
// `holds_session` answers "is this frame for a session I am an endpoint of?"
// — the caller supplies it because only the session table knows. A frame for
// one of my own sessions is consumed, never relayed: relaying my own traffic
// back onto the air would race my decrypt and duplicate work for everyone.
template <size_t N>
RelayVerdict link_relay_decide(RelayCache<N>& cache, const uint8_t* data,
                               size_t len, bool holds_session,
                               uint32_t now_ms) {
  LinkHeader h;
  if (!link_parse_header(data, len, h)) return RelayVerdict::Malformed;
  if (holds_session) return RelayVerdict::Mine;

  if (cache.seen(h.session_id, (uint8_t)h.dir, h.ctr, now_ms)) {
    return RelayVerdict::Duplicate;
  }
  // Record before the ttl check: a frame that dies here should still suppress
  // its own duplicates arriving by other paths.
  cache.remember(h.session_id, (uint8_t)h.dir, h.ctr, now_ms);

  if (h.ttl <= 1) return RelayVerdict::Expired;
  if (h.hops >= LINK_MAX_TTL) return RelayVerdict::Expired;  // clamp a liar
  return RelayVerdict::Forward;
}

// Rewrite the mutable relay bytes in place for onward transmission. Only valid
// after a Forward verdict. The AEAD tag is untouched because these two bytes
// were deliberately left outside the AAD.
inline bool link_relay_rewrite(uint8_t* data, size_t len) {
  LinkHeader h;
  if (!link_parse_header(data, len, h)) return false;
  if (h.ttl <= 1) return false;
  data[16] = (uint8_t)(h.hops + 1);
  data[17] = (uint8_t)(h.ttl - 1);
  return true;
}

}  // namespace link
}  // namespace canary

#endif  // CANARY_LINK_LINK_RELAY_H
