/**
 * @file mr60_vitals.cpp
 * @brief Breathing/heart lock FSM (Phase 0 skeleton, vitals build only).
 *
 * Entire file compiles to nothing unless CANARY_SENSE_VITALS is defined, so a
 * presence-only flavor links zero vitals code. The lock + suppression logic is
 * real and host-testable; it runs correctly even while the UART decoder is a
 * stub (every tick sees FrameKind::None and the lock falls to Lost on its
 * deadline — the safe direction).
 */

#ifdef CANARY_SENSE_VITALS

#include "mr60_vitals.h"

namespace securacv::mmwave {

static inline bool elapsed(uint32_t now, uint32_t then, uint32_t dt) {
    return (int32_t)(now - then) >= (int32_t)dt;
}

void VitalsFSM::reset(uint32_t now_ms) {
    lock_ = VitalsLock::Unknown;
    last_valid_ms_  = now_ms;
    valid_since_ms_ = now_ms;
    was_valid_  = false;
    breath_bpm_ = 0;
    heart_bpm_  = 0;
}

bool VitalsFSM::plausible(const Frame& f) const {
    if (f.kind != FrameKind::Vitals) return false;
    if (f.breath_rate < cfg_.breath_min_bpm || f.breath_rate > cfg_.breath_max_bpm) {
        return false;
    }
    if (f.heart_rate < cfg_.heart_min_bpm || f.heart_rate > cfg_.heart_max_bpm) {
        return false;
    }
    return true;
}

VitalsEvent VitalsFSM::tick(const Frame& frame, bool single_target, uint32_t now_ms) {
    VitalsEvent ev;
    const VitalsLock prev = lock_;

    // ---- DEADLINE FIRST (stall-safe) -------------------------------------
    if (lock_ != VitalsLock::Lost && lock_ != VitalsLock::Unknown &&
        elapsed(now_ms, last_valid_ms_, cfg_.lock_lost_ms)) {
        lock_ = VitalsLock::Lost;
        breath_bpm_ = 0;
        heart_bpm_  = 0;
        ev.lock_changed = true;
        ev.stalled      = true;
        ev.lock = lock_;
        return ev;
    }

    // ---- SUPPRESSION + DATA GUARD ----------------------------------------
    // Hard rule: no vitals unless exactly one target. Multi-person ambiguity
    // would mis-attribute BPM, so we don't even consider the frame.
    const bool valid = single_target && plausible(frame);

    if (valid) {
        if (!was_valid_) valid_since_ms_ = now_ms;   // start of a valid run
        was_valid_ = true;
        last_valid_ms_ = now_ms;
        breath_bpm_ = frame.breath_rate;
        heart_bpm_  = frame.heart_rate;

        if (lock_ != VitalsLock::Locked &&
            elapsed(now_ms, valid_since_ms_, cfg_.lock_confirm_ms)) {
            lock_ = VitalsLock::Locked;
        } else if (lock_ == VitalsLock::Unknown) {
            lock_ = VitalsLock::Lost;   // seen data, not yet confirmed
        }
    } else {
        was_valid_ = false;
        if (lock_ == VitalsLock::Unknown) {
            lock_ = VitalsLock::Lost;
        }
        // Loss is driven by the deadline above, not by a single bad frame, so a
        // brief vitals dropout doesn't flap the lock.
    }

    ev.lock_changed = (lock_ != prev);
    ev.lock = lock_;
    // bpm_valid additionally requires single_target RIGHT NOW: the moment a
    // second person appears, BPM reporting stops immediately rather than
    // riding out the lock-loss window on the last single-target values.
    ev.bpm_valid  = (lock_ == VitalsLock::Locked) && single_target;
    ev.breath_bpm = ev.bpm_valid ? breath_bpm_ : 0;
    ev.heart_bpm  = ev.bpm_valid ? heart_bpm_  : 0;
    return ev;
}

}  // namespace securacv::mmwave

#endif  // CANARY_SENSE_VITALS
