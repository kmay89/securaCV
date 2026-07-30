// canary/tincan/ring_policy.h — the Ring: the parent's one privileged message,
// and the honesty rules around whether it actually arrived. Pure,
// host-testable.
//
// Design: docs/design/canary_tincan_kids_watch.md §7. There is exactly one
// parent→child channel and it is not a chat. Six fixed messages, no free text,
// no reply beyond an acknowledgement tap.
//
// The important part of this file is not the vocabulary — it is
// DeliveryState. A parent who learns that "come inside" always works will
// build a safety habit on a best-effort LAN message, and the day it silently
// fails is the day this design hurt someone. So:
//
//   * There is no "Sent" state. Handing a frame to a radio is not an event
//     worth showing a parent; it invites them to read it as "delivered".
//   * NotDelivered is a first-class outcome with a plain-words reason, shown
//     as loudly as success. The app says "not delivered — the string was
//     slack", never a spinner that quietly gives up.
//   * A Ring EXPIRES. One landing forty minutes late, after a parent has
//     already walked out to find their kid, is worse than one that never
//     landed at all. Late is not a weaker kind of on-time.
//
// The one thing a Ring may override is Quiet — matching the chime engine's
// existing rule that the Tier-1 alert keeps an audible floor at every volume
// (canary/hal/voice_score.h). Nothing a sibling can send has that power.

#ifndef CANARY_TINCAN_RING_POLICY_H
#define CANARY_TINCAN_RING_POLICY_H

#include <stddef.h>
#include <stdint.h>

namespace canary {
namespace tincan {

// The whole vocabulary. Values are a stable wire contract — append only.
enum class Ring : uint8_t {
  ComeInside = 0,
  Dinner = 1,
  Bedtime = 2,
  ComeFindMe = 3,
  AnswerMe = 4,   // the only one that demands a tap back
  AllClear = 5,
  Count = 6,
};

// A Ring is stale after this. Chosen so a parent's mental model stays "it
// either got there while I was calling, or it didn't".
static constexpr uint32_t RING_EXPIRY_MS = 5 * 60 * 1000;  // 5 minutes

// How long the full-screen Ring holds the glass before the watch returns to
// its face. Long enough to be seen after a delay in a pocket; short enough not
// to strand a kid on a screen they cannot dismiss.
static constexpr uint32_t RING_DISPLAY_MS = 30 * 1000;

// AllClear is the one Ring that must not interrupt — it is reassurance, and
// reassurance that yanks a child out of what they are doing is just another
// interruption.
inline bool ring_is_interrupt(Ring r) { return r != Ring::AllClear; }

// Only AnswerMe requires an acknowledgement. Requiring one for every Ring
// would train kids to dismiss reflexively, which destroys the signal on the
// single message where it matters.
inline bool ring_needs_ack(Ring r) { return r == Ring::AnswerMe; }

// May this Ring break quiet hours / a silenced watch? Every interrupting Ring
// may. This is the ONLY thing in the product with that power.
inline bool ring_overrides_quiet(Ring r) { return ring_is_interrupt(r); }

inline bool ring_valid(uint8_t raw) { return raw < (uint8_t)Ring::Count; }

// Stable short tokens for logs and the parent UI. Not user-facing copy — the
// app localizes from the enum — but stable enough to grep a log by.
inline const char* ring_token(Ring r) {
  switch (r) {
    case Ring::ComeInside: return "come-inside";
    case Ring::Dinner:     return "dinner";
    case Ring::Bedtime:    return "bedtime";
    case Ring::ComeFindMe: return "come-find-me";
    case Ring::AnswerMe:   return "answer-me";
    case Ring::AllClear:   return "all-clear";
    default:               return "?";
  }
}

// ---------------------------------------------------------------------------
// Delivery
// ---------------------------------------------------------------------------

// What the parent is shown. Three outcomes and a transient — deliberately no
// "Sent".
enum class DeliveryState : uint8_t {
  InFlight = 0,      // still inside the delivery window; the UI says "calling…"
  DeliveredAcked,    // it landed AND the kid tapped
  DeliveredNoAck,    // the watch confirmed receipt; no tap (yet)
  NotDelivered,      // it did not land. Said plainly, not hidden in a spinner.
};

// Why it did not land, in words a parent can act on.
enum class NotDeliveredReason : uint8_t {
  None = 0,
  StringSlack,   // the watch was not reachable — "she's out of range"
  Expired,       // the window closed before it landed
  WatchAsleep,   // reachable but did not wake in time
  NoRoute,       // no path through the house at all
};

inline const char* not_delivered_reason_token(NotDeliveredReason r) {
  switch (r) {
    case NotDeliveredReason::StringSlack: return "string-slack";
    case NotDeliveredReason::Expired:     return "expired";
    case NotDeliveredReason::WatchAsleep: return "watch-asleep";
    case NotDeliveredReason::NoRoute:     return "no-route";
    case NotDeliveredReason::None:
    default:                              return "none";
  }
}

// One outstanding Ring, from the sending side's point of view.
struct RingAttempt {
  Ring ring = Ring::ComeInside;
  uint16_t target_node = 0;
  uint32_t sent_ms = 0;

  bool receipt = false;      // the watch confirmed it rendered the Ring
  uint32_t receipt_ms = 0;
  bool acked = false;        // the kid tapped
  uint32_t acked_ms = 0;

  bool gave_up = false;
  NotDeliveredReason reason = NotDeliveredReason::None;

  bool expired(uint32_t now_ms) const {
    return (now_ms - sent_ms) >= RING_EXPIRY_MS;
  }

  // The state to show right now. Note the ordering: a receipt that arrived
  // before expiry still counts even if we are asked later — expiry stops us
  // *waiting*, it does not retroactively unsend a Ring that landed.
  DeliveryState state(uint32_t now_ms) const {
    if (acked) return DeliveryState::DeliveredAcked;
    if (receipt) return DeliveryState::DeliveredNoAck;
    if (gave_up) return DeliveryState::NotDelivered;
    if (expired(now_ms)) return DeliveryState::NotDelivered;
    return DeliveryState::InFlight;
  }

  // The reason to show alongside NotDelivered. Never empty when the state is
  // NotDelivered — "it didn't work" with no reason is what teaches a parent to
  // distrust the whole device.
  NotDeliveredReason failure(uint32_t now_ms) const {
    if (state(now_ms) != DeliveryState::NotDelivered) {
      return NotDeliveredReason::None;
    }
    if (reason != NotDeliveredReason::None) return reason;
    return NotDeliveredReason::Expired;
  }

  // Should the sender keep retrying? Stop the moment it landed or the window
  // closed. A Ring is not a message queue.
  bool should_retry(uint32_t now_ms) const {
    if (receipt || acked || gave_up) return false;
    return !expired(now_ms);
  }

  void on_receipt(uint32_t now_ms) {
    if (expired(now_ms)) return;  // too late to count
    receipt = true;
    receipt_ms = now_ms;
  }

  void on_ack(uint32_t now_ms) {
    if (expired(now_ms)) return;
    receipt = true;  // an ack implies it landed
    if (receipt_ms == 0) receipt_ms = now_ms;
    acked = true;
    acked_ms = now_ms;
  }

  void on_give_up(NotDeliveredReason why) {
    if (receipt || acked) return;  // it already landed; don't rewrite history
    gave_up = true;
    reason = why;
  }
};

// ---------------------------------------------------------------------------
// Receiving side
// ---------------------------------------------------------------------------

// Should a watch that is quiet/asleep light up for this Ring right now?
// `quiet` is the kid's quiet-hours or silenced state.
inline bool ring_should_wake(Ring r, bool quiet) {
  if (!quiet) return true;
  return ring_overrides_quiet(r);
}

// A Ring that arrives after its own expiry must NOT be shown. This is the
// receiving half of "late is not a weaker kind of on-time": a watch that was
// in a bag for an hour should show nothing when it comes out, because the
// parent has long since stopped waiting and has been told it did not land.
//
// `age_ms` is the age the receiver can establish from the frame's own
// timestamp field, not from trusting a relay.
inline bool ring_should_render(uint32_t age_ms) {
  return age_ms < RING_EXPIRY_MS;
}

}  // namespace tincan
}  // namespace canary

#endif  // CANARY_TINCAN_RING_POLICY_H
