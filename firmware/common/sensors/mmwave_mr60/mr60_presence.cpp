/**
 * @file mr60_presence.cpp
 * @brief Presence / count FSM implementation.
 *
 * The debounce + stall logic is host-tested against golden frames from
 * mr60_uart's decoder (firmware/tests_host/test_mr60_uart.cpp). A silent
 * radar still drives the FSM: every tick then sees a FrameKind::None frame
 * and the deadline check moves it to Unknown — the safe direction.
 *
 * What leaves Unknown, and why it matters: Unknown is the state the caller
 * signs NOTHING from (canary-sense records `presence_detected` on Present
 * and `presence_cleared` on Clear). A frame that reports a target must
 * therefore never take the FSM to Clear on its way to Present — it used to,
 * "so we report something promptly", and canary-sense sealed a signed
 * `presence_cleared` record while the radar was reporting a body. Now a
 * target frame after Unknown starts the debounce run and the state stays
 * Unknown until Present is earned; a no-target frame after Unknown goes to
 * Clear, which is what the radar said.
 */

#include "mr60_presence.h"

namespace securacv::mmwave {

// Wrap-safe "has at least `dt` ms elapsed since `then`?" using a signed delta,
// per the firmware millis() idiom. Correct across the uint32 wrap because the
// subtraction is performed in unsigned arithmetic then reinterpreted signed.
static inline bool elapsed(uint32_t now, uint32_t then, uint32_t dt) {
    return (int32_t)(now - then) >= (int32_t)dt;
}

void PresenceFSM::reset(uint32_t now_ms) {
    state_ = Presence::Unknown;
    count_ = CountBucket::Zero;
    range_ = RangeBand::Unknown;
    last_frame_ms_   = now_ms;
    target_since_ms_ = now_ms;
    target_gone_ms_  = now_ms;
    raw_target_      = false;
}

CountBucket PresenceFSM::bucket_of(uint8_t raw) {
    if (raw == 0) return CountBucket::Zero;
    if (raw == 1) return CountBucket::One;
    return CountBucket::TwoPlus;
}

RangeBand PresenceFSM::band_of(uint16_t distance_cm) const {
    if (distance_cm == 0)            return RangeBand::Unknown;
    if (distance_cm <= cfg_.near_cm) return RangeBand::Near;
    if (distance_cm <= cfg_.mid_cm)  return RangeBand::Mid;
    return RangeBand::Far;
}

PresenceEvent PresenceFSM::tick(const Frame& frame, uint32_t now_ms) {
    PresenceEvent ev;

    // ---- DEADLINE FIRST (stall-safe) -------------------------------------
    // Before trusting any incoming data, fail the FSM safe if the radar has
    // gone silent. This must precede the data guard so a dead UART can never
    // freeze us on the last good frame.
    if (elapsed(now_ms, last_frame_ms_, cfg_.stall_timeout_ms)) {
        // The target run ends with the link, whatever state the link dropped
        // in — Unknown included, since a run can be open there while the
        // debounce is still being earned. The first frame after the radar
        // comes back starts a fresh debounce, so a stale `target_since_ms_`
        // cannot promote one returning frame straight to Present.
        raw_target_ = false;
    }
    if (state_ != Presence::Unknown &&
        elapsed(now_ms, last_frame_ms_, cfg_.stall_timeout_ms)) {
        // The stall is a state change, so it carries the dropped count
        // (Zero) in ev.count and reports no separate count move (F131).
        state_ = Presence::Unknown;
        count_ = CountBucket::Zero;
        range_ = RangeBand::Unknown;
        ev.state_changed = true;
        ev.stalled       = true;
        ev.state = state_;
        ev.count = count_;
        ev.range = range_;
        return ev;
    }

    // ---- DATA GUARD ------------------------------------------------------
    // Only presence frames advance the rest of the machine. A None/other frame
    // is normal (no new data this loop) and leaves debounce timers running.
    if (frame.kind != FrameKind::Presence) {
        ev.state = state_;
        ev.count = count_;
        ev.range = range_;
        return ev;
    }

    last_frame_ms_ = now_ms;

    // Track raw target edges for debounce windows.
    if (frame.has_target && !raw_target_) {
        target_since_ms_ = now_ms;        // start of a presence run
    } else if (!frame.has_target && raw_target_) {
        target_gone_ms_ = now_ms;         // start of an absence run
    }
    raw_target_ = frame.has_target;

    const Presence prev_state = state_;

    if (frame.has_target) {
        // A target frame only ever leads to Present, after the debounce.
        // From Unknown it stays Unknown meanwhile: the caller signs nothing
        // from Unknown, and a signed "cleared" while a body is in view is
        // a false record, not a prompt one (see the file comment).
        if (state_ != Presence::Present &&
            elapsed(now_ms, target_since_ms_, cfg_.present_debounce_ms)) {
            state_ = Presence::Present;
        }
    } else {
        if (state_ != Presence::Clear &&
            elapsed(now_ms, target_gone_ms_, cfg_.clear_timeout_ms)) {
            state_ = Presence::Clear;
        } else if (state_ == Presence::Unknown) {
            state_ = Presence::Clear;
        }
    }

    // The count follows the DEBOUNCED state (sweep F131), not each frame:
    // Zero unless Present. While Present a frame that names a count updates
    // the bucket, and a frame that names none keeps the last one — a
    // no-target frame inside the clear timeout, whatever count it carries
    // (a TARGET_COUNT frame decoded after PEOPLE_EXIST(0) holds the radar's
    // count beside a cleared flag), and a target frame whose count is 0,
    // which is what the decoder emits after a no-target frame zeroed its
    // aggregate and before the radar's count frame lands. So a
    // one-frame dropout moves nothing, an emptying room keeps its count until
    // the tick that turns Clear (which then carries Zero), and no Clear row
    // ever counts an occupant during the Present debounce. A held count does
    // not decay while Present. Range keeps following each frame.
    const CountBucket prev_count = count_;
    if (state_ != Presence::Present) {
        count_ = CountBucket::Zero;
    } else if (frame.has_target && frame.target_count > 0) {
        count_ = bucket_of(frame.target_count);
    }
    range_ = frame.has_target ? band_of(frame.distance_cm) : RangeBand::Unknown;

    ev.state_changed = (state_ != prev_state);
    // A tick that changes the state carries its count in ev.count (Present
    // the bucket it was entered with, Clear Zero), so the caller signs one
    // record per transition: count_changed is a move while the state held.
    ev.count_changed = !ev.state_changed && (count_ != prev_count);
    ev.state = state_;
    ev.count = count_;
    ev.range = range_;
    return ev;
}

}  // namespace securacv::mmwave
