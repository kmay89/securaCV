/**
 * @file mr60_presence.cpp
 * @brief Presence / count FSM implementation (Phase 0 skeleton).
 *
 * The debounce + stall logic is real and host-testable; it runs correctly even
 * while mr60_uart's decoder is still a stub (every tick then sees a
 * FrameKind::None frame and the FSM drives itself to Unknown on the stall
 * deadline — the safe direction).
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
    if (state_ != Presence::Unknown &&
        elapsed(now_ms, last_frame_ms_, cfg_.stall_timeout_ms)) {
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
        if (state_ != Presence::Present &&
            elapsed(now_ms, target_since_ms_, cfg_.present_debounce_ms)) {
            state_ = Presence::Present;
        } else if (state_ == Presence::Unknown) {
            // First data after a stall: leave Unknown promptly so we report
            // *something* rather than waiting a full debounce while clearly
            // seeing a target. Settle to Present via the debounce above.
            state_ = Presence::Clear;
        }
    } else {
        if (state_ != Presence::Clear &&
            elapsed(now_ms, target_gone_ms_, cfg_.clear_timeout_ms)) {
            state_ = Presence::Clear;
        } else if (state_ == Presence::Unknown) {
            state_ = Presence::Clear;
        }
    }

    // Count + range track the latest frame's scalars.
    const CountBucket prev_count = count_;
    count_ = frame.has_target ? bucket_of(frame.target_count) : CountBucket::Zero;
    range_ = frame.has_target ? band_of(frame.distance_cm)    : RangeBand::Unknown;

    ev.state_changed = (state_ != prev_state);
    ev.count_changed = (count_ != prev_count);
    ev.state = state_;
    ev.count = count_;
    ev.range = range_;
    return ev;
}

}  // namespace securacv::mmwave
