// common/link/link_node_table.h — who else is on the air, how close, how
// fresh, and how many hops away. Pure, host-testable.
//
// This is the "many nodes, seamlessly" half of Canary Link. A house has two
// watches, some Canaries, a dash and a hub; a warehouse has a hundred nodes.
// Both want the same three answers and nothing more:
//
//   * Is this node here right now?   (liveness, with an honest middle state)
//   * How strong is the link?        (smoothed RSSI, not the last sample)
//   * How do I reach it?             (hop count + which neighbour to hand to)
//
// The design rule inherited from the displays
// (docs/hardware/display_discovery_and_resilience.md): ABSENCE IS A STATE, NOT
// A GAP. A node that stops being heard becomes Stale and then Lost on a clock,
// visibly — it never silently keeps its last-known look. On the Tin Can that
// state is literally the picture: a taut line versus a slack one. A UI that
// draws a taut string to a watch that left the house half an hour ago is worse
// than one that draws nothing.
//
// Smoothing exists because raw RSSI on a wrist is violent — a turning body
// swings it 20 dB. An exponential moving average is enough to stop the string
// flickering, and is the same input the Tin Can's warmer/colder game consumes.

#ifndef CANARY_LINK_LINK_NODE_TABLE_H
#define CANARY_LINK_LINK_NODE_TABLE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace canary {
namespace link {

// Liveness. Deliberately three states, not two: "I am not sure yet" is real
// information and hiding it is how a link-loss bug becomes a trust bug.
enum class NodeLiveness : uint8_t {
  Fresh = 0,  // heard within the grace period — the string is taut
  Stale,      // overdue but not written off — the string sags
  Lost,       // gone past the give-up horizon — the string is slack
};

// Defaults tuned for a ~2 s beacon cadence on a wrist.
static constexpr uint32_t NODE_FRESH_MS = 8000;    // 4 missed beacons
static constexpr uint32_t NODE_LOST_MS = 45000;    // written off

// EMA weight, in 1/16ths, applied to each new sample. 4/16 = a quarter, which
// settles in about a second at beacon cadence while still surviving one bad
// body-block sample.
static constexpr int32_t NODE_RSSI_ALPHA_16 = 4;

// A node id is the 4-hex-character fingerprint the fleet already beacons
// ("fp4"), kept as a raw u16 here so the table costs nothing. The mapping to
// display text belongs to the UI, not to the radio.
struct Node {
  bool used = false;
  uint16_t id = 0;
  int16_t rssi_ema = 0;      // dBm, smoothed. Only meaningful when heard_once.
  bool heard_once = false;
  uint32_t last_heard_ms = 0;
  uint8_t hops = 0;          // 0 = direct neighbour
  uint16_t via = 0;          // next hop to use; 0 when direct
  uint8_t caps = 0;          // payload-layer capability bits, opaque here

  NodeLiveness liveness(uint32_t now_ms) const {
    if (!heard_once) return NodeLiveness::Lost;
    const uint32_t age = now_ms - last_heard_ms;  // wrap-safe on uint32
    if (age <= NODE_FRESH_MS) return NodeLiveness::Fresh;
    if (age <= NODE_LOST_MS) return NodeLiveness::Stale;
    return NodeLiveness::Lost;
  }

  bool reachable(uint32_t now_ms) const {
    return liveness(now_ms) != NodeLiveness::Lost;
  }
};

// Fold a new RSSI sample into the EMA. First sample seeds it outright — a
// quarter-weighted average starting from zero would report a wildly optimistic
// -12 dBm for the first second, and on this device that means drawing a taut
// string that isn't there.
inline int16_t node_rssi_fold(int16_t ema, bool primed, int16_t sample) {
  if (!primed) return sample;
  const int32_t e = ema;
  const int32_t s = sample;
  return (int16_t)(e + ((s - e) * NODE_RSSI_ALPHA_16) / 16);
}

template <size_t N>
struct NodeTable {
  Node slots[N];

  static constexpr size_t capacity() { return N; }

  Node* find(uint16_t id) {
    for (size_t i = 0; i < N; i++) {
      if (slots[i].used && slots[i].id == id) return &slots[i];
    }
    return nullptr;
  }

  const Node* find(uint16_t id) const {
    for (size_t i = 0; i < N; i++) {
      if (slots[i].used && slots[i].id == id) return &slots[i];
    }
    return nullptr;
  }

  // Record that `id` was heard. Creates the node if it is new, evicting the
  // deadest slot when full.
  //
  // The route rule: a strictly shorter path always wins, and an EQUAL-length
  // path does NOT displace the incumbent. Ties would otherwise flap between
  // two equally good neighbours every beacon, and route flap in a house full
  // of nodes is what turns "seamless" into "why did that take four seconds".
  Node* observe(uint16_t id, int16_t rssi, uint8_t hops, uint16_t via,
                uint32_t now_ms) {
    Node* n = find(id);
    if (!n) {
      n = alloc_or_evict(now_ms);
      if (!n) return nullptr;
      n->used = true;
      n->id = id;
      n->heard_once = false;
      n->hops = hops;
      n->via = via;
      n->caps = 0;
    } else if (hops < n->hops || n->liveness(now_ms) == NodeLiveness::Lost) {
      n->hops = hops;
      n->via = via;
    }

    n->rssi_ema = node_rssi_fold(n->rssi_ema, n->heard_once, rssi);
    n->heard_once = true;
    n->last_heard_ms = now_ms;
    return n;
  }

  size_t count_reachable(uint32_t now_ms) const {
    size_t c = 0;
    for (size_t i = 0; i < N; i++) {
      if (slots[i].used && slots[i].reachable(now_ms)) c++;
    }
    return c;
  }

 private:
  // Prefer a genuinely free slot; otherwise take the node we have not heard
  // from in longest, and only if it is already Lost. A table full of live
  // nodes refuses to admit another rather than silently dropping a neighbour
  // that is still talking to us.
  Node* alloc_or_evict(uint32_t now_ms) {
    for (size_t i = 0; i < N; i++) {
      if (!slots[i].used) return &slots[i];
    }
    Node* worst = nullptr;
    uint32_t worst_age = 0;
    for (size_t i = 0; i < N; i++) {
      if (slots[i].liveness(now_ms) != NodeLiveness::Lost) continue;
      const uint32_t age = now_ms - slots[i].last_heard_ms;
      if (!worst || age > worst_age) {
        worst = &slots[i];
        worst_age = age;
      }
    }
    return worst;
  }
};

}  // namespace link
}  // namespace canary

#endif  // CANARY_LINK_LINK_NODE_TABLE_H
