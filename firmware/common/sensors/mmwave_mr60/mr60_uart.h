/**
 * @file mr60_uart.h
 * @brief Board-agnostic UART frame parser interface for the Seeed MR60BHA2.
 *
 * The MR60BHA2 radar module ships its pre-digested scalar claims (presence,
 * target count, distance, breath/heart rate) to the host over a 115200 8N1
 * UART. This module owns the wire protocol: it consumes raw bytes, reassembles
 * framed records, and emits decoded scalar payloads. It knows NOTHING about
 * pins, boards, or feature flags — the caller hands it bytes and reads frames.
 *
 * Layering (firmware/ARCHITECTURE.md): this lives in `common/` and therefore
 * never includes `boards/` (no pin numbers here) or `configs/` (no feature
 * flags via header). The vitals build switch reaches the .cpp as the
 * `-DCANARY_SENSE_VITALS` build flag only.
 *
 * Protocol status: the exact MR60 frame layout (field offsets, CRC) is being
 * confirmed against the Seeed Arduino mmWave library. The frame DELIMITERS and
 * the public decode interface are defined here; the per-type field extraction
 * is a clearly-marked TODO that currently yields no frames (see mr60_uart.cpp).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace securacv::mmwave {

// ============================================================================
// WIRE PROTOCOL CONSTANTS
// ============================================================================
//
// Per the Seeed MR60 family framing (Seeed_Arduino_mmWave reference), records
// begin with a fixed start-of-frame header byte followed by an ID/length
// preamble. We pin the documented start byte; remaining offsets are resolved
// in the parser body once verified on the bench.

// Frame header / start-of-frame byte (Seeed MR60 protocol header = 0x01).
static constexpr uint8_t MR60_SOF = 0x01;

// Defensive upper bound on a single reassembled frame. Keeps the ring buffer
// and scratch stack-allocated — no dynamic allocation in the hot path.
static constexpr size_t MR60_MAX_FRAME = 64;

// ============================================================================
// DECODED FRAME
// ============================================================================

// Logical record kinds the radar reports. Distance/count ride the presence
// frame; vitals come on their own record. Kept deliberately small.
enum class FrameKind : uint8_t {
    None = 0,       // no complete frame this call
    Presence,       // has_target + target_count + distance
    Vitals,         // breath_rate + heart_rate (only meaningful when VITALS)
    Unknown,        // well-formed framing, unrecognised type id
};

// One decoded radar record. POD; safe to copy by value. Fields not carried by
// the current frame kind are left at their reset defaults.
struct Frame {
    FrameKind kind        = FrameKind::None;
    bool      has_target  = false;   // binary presence flag
    uint8_t   target_count = 0;      // raw count (caller buckets to 0/1/2+)
    uint16_t  distance_cm = 0;       // distance-to-target, centimetres
    uint16_t  breath_rate = 0;       // breaths per minute (vitals frame)
    uint16_t  heart_rate  = 0;       // beats per minute (vitals frame)
};

// ============================================================================
// PARSER
// ============================================================================
//
// Byte-oriented incremental parser. The caller pumps received UART bytes in
// (one or many at a time); when a full, valid frame is assembled, poll()
// returns it. No allocation, no blocking, no Arduino dependency — unit-testable
// on the host.
class FrameParser {
public:
    FrameParser() { reset(); }

    // Drop any partial frame and return to hunting for a start byte.
    void reset();

    // Feed a single received byte into the reassembly buffer.
    void push(uint8_t b);

    // Feed a span of received bytes.
    void push(const uint8_t* data, size_t len);

    // Pull the next fully-decoded frame, if one has completed since the last
    // call. Returns a frame with kind == FrameKind::None when nothing is ready.
    //
    // This is also the deadline-safe entry point: callers that need stall
    // detection track frame *age* themselves (see mr60_presence) and must not
    // rely on this ever being called to advance their timeout — poll() only
    // ever reports real radar data.
    Frame poll();

    // Diagnostics: count of frames dropped to CRC / framing errors since reset.
    uint32_t error_count() const { return error_count_; }

private:
    enum class State : uint8_t { HuntSof, Collect };

    State    state_ = State::HuntSof;
    uint8_t  buf_[MR60_MAX_FRAME];
    size_t   len_ = 0;
    uint32_t error_count_ = 0;

    // Attempt to decode the bytes currently in buf_ into `out`. Returns true on
    // a valid, complete frame. Implemented in mr60_uart.cpp.
    bool try_decode(Frame& out);
};

}  // namespace securacv::mmwave
