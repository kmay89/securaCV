/**
 * @file mr60_presence.h
 * @brief Presence / target-count FSM for the MR60BHA2 (P0, board-agnostic).
 *
 * Consumes decoded Presence frames (see mr60_uart.h) and a millisecond clock,
 * and produces a debounced presence state plus a bucketed occupant count. This
 * is the P0 witness path that maps onto `PresenceInRestrictedZone` at the
 * privacy chokepoint; raw distance never leaves the device (coarse range band
 * only, computed here for host-side zone gating).
 *
 * Stall-safety (design doc §3 implementation notes): tick() runs its DEADLINE
 * check BEFORE any data-presence guard, so a silent radar UART still drives the
 * FSM to Unknown (and a health event) instead of freezing on the last good
 * frame. All time math is wrap-safe signed-delta: `(int32_t)(now - then) >= d`.
 *
 * Layering: no boards/ or configs/ includes. Thresholds are passed in as a
 * Config struct that the composition layer (projects/ + configs/) fills from
 * config.h — the header itself stays flag-free.
 */

#pragma once

#include <stdint.h>

#include "mr60_uart.h"

namespace securacv::mmwave {

// Debounced presence states.
enum class Presence : uint8_t {
    Unknown = 0,   // startup, or radar silent past the stall deadline
    Clear,         // no target
    Present,       // target debounced as present
};

// Coarse, privacy-safe range band derived from distance. Raw centimetres are
// never exported; only this band may appear in diagnostics.
enum class RangeBand : uint8_t {
    Unknown = 0,
    Near,          // <= near_cm
    Mid,           // <= mid_cm
    Far,           // beyond mid_cm
};

// Bucketed occupant count (never a per-target track log): 0 / 1 / 2+.
enum class CountBucket : uint8_t {
    Zero = 0,
    One,
    TwoPlus,
};

// Tuning, supplied by the composition layer from config.h. Defaults are
// conservative placeholders; configs/canary-sense/*/config.h is the source of
// truth at build time.
struct PresenceConfig {
    uint32_t present_debounce_ms = 300;     // sustained target before "Present"
    uint32_t clear_timeout_ms    = 1500;    // no target before "Clear"
    uint32_t stall_timeout_ms    = 5000;    // no frame at all before "Unknown"
    uint16_t near_cm             = 150;     // <= -> Near
    uint16_t mid_cm              = 350;     // <= -> Mid, else Far
};

// What changed on a tick — the caller decides what to publish/seal.
struct PresenceEvent {
    bool        state_changed = false;
    bool        count_changed = false;
    bool        stalled       = false;   // crossed into Unknown via stall
    Presence    state         = Presence::Unknown;
    CountBucket count         = CountBucket::Zero;
    RangeBand   range         = RangeBand::Unknown;
};

class PresenceFSM {
public:
    explicit PresenceFSM(const PresenceConfig& cfg) : cfg_(cfg) { reset(0); }

    // Clear all state. `now_ms` seeds the deadline clocks.
    void reset(uint32_t now_ms);

    // Advance the FSM. Call EVERY loop with the current millis(), passing the
    // most recent decoded frame (or a FrameKind::None frame when nothing new
    // arrived). Deadline checks run first, so passing None still drives stalls.
    // Returns the change summary for this tick.
    PresenceEvent tick(const Frame& frame, uint32_t now_ms);

    Presence    state() const { return state_; }
    CountBucket count() const { return count_; }
    RangeBand   range() const { return range_; }

private:
    static CountBucket bucket_of(uint8_t raw);
    RangeBand          band_of(uint16_t distance_cm) const;

    PresenceConfig cfg_;

    Presence    state_ = Presence::Unknown;
    CountBucket count_ = CountBucket::Zero;
    RangeBand   range_ = RangeBand::Unknown;

    // Wrap-safe timestamps (compared via signed deltas, never subtracted into
    // an unsigned result).
    uint32_t last_frame_ms_   = 0;   // last frame of ANY kind
    uint32_t target_since_ms_ = 0;   // first frame in the current target run
    uint32_t target_gone_ms_  = 0;   // first frame with no target after presence
    bool     raw_target_      = false;
};

}  // namespace securacv::mmwave
