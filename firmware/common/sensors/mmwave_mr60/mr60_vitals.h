/**
 * @file mr60_vitals.h
 * @brief Breathing / heart-rate lock FSM for the MR60BHA2 (wellbeing, P1).
 *
 * Vitals are **wellbeing signals, not medical data**: never sealed-logged,
 * never precise-timestamped, P1-gated, HA-local only (design doc §2.2). This
 * module produces a P0 binary "breathing confirmed" lock plus optional P1 BPM
 * numerics, and enforces the firmware safety rule that vitals are suppressed
 * whenever the target count is not exactly one.
 *
 * Build gating (layering rule): this entire translation unit compiles ONLY
 * under `-DCANARY_SENSE_VITALS`. The flag arrives as a build flag from
 * `envs/platformio/canary-sense.ini`, never via a configs/ header include. A
 * presence-only build links no vitals code at all.
 *
 * Stall-safety: like presence, tick() checks its lock deadline BEFORE the data
 * guard, so a silent radar drives the lock to Lost rather than latching the
 * last good vitals frame.
 */

#pragma once

#ifdef CANARY_SENSE_VITALS

#include <stdint.h>

#include "mr60_uart.h"

namespace securacv::mmwave {

// Breathing-lock state. The lock is the only P0 vitals signal; BPM numerics
// are P1 and only meaningful while Locked.
enum class VitalsLock : uint8_t {
    Unknown = 0,   // startup or radar silent past the deadline
    Lost,          // no confirmed breathing
    Locked,        // breathing confirmed (single target, sustained)
};

struct VitalsConfig {
    uint32_t lock_confirm_ms = 4000;   // sustained valid vitals before Locked
    uint32_t lock_lost_ms    = 6000;   // no valid vitals before Lost
    uint16_t breath_min_bpm  = 6;      // plausible breathing band (reject noise)
    uint16_t breath_max_bpm  = 30;
    uint16_t heart_min_bpm   = 40;
    uint16_t heart_max_bpm   = 130;
};

struct VitalsEvent {
    bool       lock_changed = false;
    bool       stalled      = false;   // crossed to Lost via deadline
    VitalsLock lock         = VitalsLock::Unknown;
    bool       bpm_valid    = false;   // true only while Locked with sane values
    uint16_t   breath_bpm   = 0;       // P1 numeric (0 unless bpm_valid)
    uint16_t   heart_bpm    = 0;       // P1 numeric (0 unless bpm_valid)
};

class VitalsFSM {
public:
    explicit VitalsFSM(const VitalsConfig& cfg) : cfg_(cfg) { reset(0); }

    void reset(uint32_t now_ms);

    // Advance the lock FSM. `single_target` is the presence FSM's verdict that
    // exactly one occupant is present — vitals are hard-suppressed otherwise
    // (design doc risk table: multi-person BPM attribution). Deadline check
    // runs before the data guard, so a None frame still drives the lock to Lost.
    VitalsEvent tick(const Frame& frame, bool single_target, uint32_t now_ms);

    VitalsLock lock() const { return lock_; }

private:
    bool plausible(const Frame& f) const;

    VitalsConfig cfg_;

    VitalsLock lock_ = VitalsLock::Unknown;
    uint32_t   last_valid_ms_ = 0;   // last plausible single-target vitals frame
    uint32_t   valid_since_ms_ = 0;  // start of the current valid run
    bool       was_valid_ = false;
    uint16_t   breath_bpm_ = 0;
    uint16_t   heart_bpm_  = 0;
};

}  // namespace securacv::mmwave

#endif  // CANARY_SENSE_VITALS
