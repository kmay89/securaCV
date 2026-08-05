/**
 * @file mr60_uart.h
 * @brief Board-agnostic UART frame decoder for the Seeed MR60BHA2.
 *
 * The MR60BHA2 radar module ships its pre-digested scalar claims (presence,
 * target count, distance, breath/heart rate) to the host over a 115200 8N1
 * UART. This module owns the wire protocol: it consumes raw bytes, reassembles
 * framed records, validates their checksums, and emits decoded scalars. It
 * knows NOTHING about pins, boards, or feature flags — the caller hands it
 * bytes and reads frames.
 *
 * Layering (firmware/ARCHITECTURE.md): this lives in `common/` and therefore
 * never includes `boards/` (no pin numbers here) or `configs/` (no feature
 * flags via header). The vitals build switch reaches the .cpp as the
 * `-DCANARY_SENSE_VITALS` build flag only.
 *
 * ============================================================================
 * WIRE PROTOCOL — MR60BHA2 UART frame format
 * ============================================================================
 *
 * Source of truth (extracted verbatim, Phase 2):
 *   ESPHome `seeed_mr60bha2` component, which decodes the exact frames we need:
 *   - https://raw.githubusercontent.com/esphome/esphome/dev/esphome/components/seeed_mr60bha2/seeed_mr60bha2.h
 *   - https://raw.githubusercontent.com/esphome/esphome/dev/esphome/components/seeed_mr60bha2/seeed_mr60bha2.cpp
 *   (Cross-checked against the Seeed_Arduino_mmWave library framing family.)
 *
 * Frame layout (a fixed 8-byte header, then payload, then 1 checksum byte):
 *
 *   off  size  field
 *   ---  ----  -----------------------------------------------------------
 *    0    1    SOF / frame header, always 0x01
 *    1    2    frame id      (uint16, BIG-endian: byte1<<8 | byte2)
 *    3    2    payload len   (uint16, BIG-endian: byte3<<8 | byte4)
 *    5    2    frame type    (uint16, BIG-endian: byte5<<8 | byte6)
 *    7    1    header checksum over bytes [0..6]
 *    8    N    payload (N = payload len)
 *   8+N   1    data checksum over payload bytes [8 .. 8+N-1]
 *
 * NOTE the deliberate mixed endianness (confirmed in the reference): the
 * HEADER scalar fields (id/len/type) are BIG-endian, but every MULTI-BYTE
 * PAYLOAD field is LITTLE-endian (see per-type decode below).
 *
 * Checksum algorithm (identical for header and data): XOR-fold every covered
 * byte into an 8-bit accumulator, then bitwise-invert:
 *     uint8_t c = 0; for (b : bytes) c ^= b; c = ~c;
 *
 * Frame types we decode (16-bit type id, on the wire big-endian):
 *   0x0F09  PEOPLE_EXIST   payload: uint16 LE has_target (nonzero == present)
 *   0x0A04  PRINT_CLOUD    payload: uint32 LE target count (num_targets)
 *   0x0A16  DISTANCE       payload: byte[0] valid flag, float32 LE @ [4..7] (m)
 *   0x0A14  BREATH_RATE    payload: float32 LE @ [0..3] (breaths/min)
 *   0x0A15  HEART_RATE     payload: float32 LE @ [0..3] (beats/min)
 *
 * Any well-framed record (both checksums valid) whose type is none of the
 * above is counted as `unknown` and skipped — the parser stays in sync rather
 * than treating it as corruption (a strict improvement over the reference,
 * which rejects unknown types at the header stage).
 *
 * Bench-verification notes (judgment calls made from the published protocol,
 * to confirm against real hardware in Phase 2):
 *   [BENCH] DISTANCE float32 is assumed to be in METERS and scaled ×100 to the
 *           centimeter integer the presence FSM consumes. If the module already
 *           reports centimeters, drop the scale in decode_distance_().
 *   [BENCH] BREATH/HEART float32 are assumed to be already in BPM and rounded
 *           to the nearest integer for the vitals FSM's plausibility bands.
 *   [BENCH] A single UART frame carries ONE scalar. The MR60 sends presence,
 *           count, distance and vitals as separate frames, so this decoder
 *           AGGREGATES the latest value of each into every emitted Frame (see
 *           "Aggregation" below) — the FSMs expect a combined snapshot. Confirm
 *           the per-scalar frame cadence on the bench.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace securacv::mmwave {

// ============================================================================
// WIRE PROTOCOL CONSTANTS
// ============================================================================

// Start-of-frame byte (Seeed MR60 protocol header).
static constexpr uint8_t MR60_SOF = 0x01;

// Fixed header: SOF(1) + id(2) + len(2) + type(2) + header-checksum(1).
static constexpr size_t MR60_HEADER_LEN = 8;

// Frame type ids (16-bit; big-endian on the wire).
static constexpr uint16_t MR60_TYPE_PEOPLE_EXIST = 0x0F09;  // presence flag
static constexpr uint16_t MR60_TYPE_TARGET_COUNT = 0x0A04;  // "PRINT_CLOUD"
static constexpr uint16_t MR60_TYPE_DISTANCE     = 0x0A16;  // distance-to-target
static constexpr uint16_t MR60_TYPE_BREATH_RATE  = 0x0A14;  // breaths per minute
static constexpr uint16_t MR60_TYPE_HEART_RATE   = 0x0A15;  // beats per minute

// Defensive upper bound on a single payload. The real payloads top out at 8
// bytes (DISTANCE); anything claiming more than this is rejected as a framing
// error rather than buffered. Keeps the scratch buffer stack-sized — no
// dynamic allocation in the hot path.
static constexpr size_t MR60_MAX_PAYLOAD = 32;

// Full reassembly-buffer bound: header + max payload + trailing checksum.
static constexpr size_t MR60_MAX_FRAME = MR60_HEADER_LEN + MR60_MAX_PAYLOAD + 1;

// How many decoded frames the parser can hold between poll()s. A real drain
// loop empties this every iteration; the depth only absorbs bursts.
static constexpr size_t MR60_FRAME_QUEUE = 8;

// ============================================================================
// DECODED FRAME
// ============================================================================

// Logical record kinds the radar reports. Distance/count ride the presence
// snapshot; vitals come on the vitals snapshot. Kept deliberately small.
enum class FrameKind : uint8_t {
    None = 0,       // no complete frame this call
    Presence,       // has_target + target_count + distance
    Vitals,         // breath_rate + heart_rate (only meaningful when VITALS)
    Unknown,        // well-formed framing, unrecognized type id
};

// One decoded radar record. POD; safe to copy by value.
//
// Because each MR60 UART frame carries a single scalar, the parser folds the
// latest value of every scalar into an aggregate and stamps a full snapshot
// onto every emitted Frame; `kind` reflects the class of the frame that just
// updated the aggregate (Presence for presence/count/distance, Vitals for
// breath/heart). The FSMs read a combined snapshot, so this is what keeps
// mr60_presence / mr60_vitals working against per-scalar hardware frames.
struct Frame {
    FrameKind kind         = FrameKind::None;
    bool      has_target   = false;   // binary presence flag
    uint8_t   target_count = 0;       // raw count (caller buckets to 0/1/2+)
    uint16_t  distance_cm  = 0;       // distance-to-target, centimeters
    uint16_t  breath_rate  = 0;       // breaths per minute (vitals)
    uint16_t  heart_rate   = 0;       // beats per minute (vitals)
};

// ============================================================================
// PARSER
// ============================================================================
//
// Byte-oriented incremental state machine. The caller pumps received UART
// bytes in (one or many at a time) with push(); completed frames are decoded
// and queued as they finish, and drained one at a time with poll(). No
// allocation, no blocking, no Arduino dependency — unit-testable on the host.
//
// Resynchronisation: on ANY validation failure (bad header checksum, bad data
// checksum, oversized length) the parser drops the single byte it had latched
// as the start-of-frame and re-hunts from the next byte — it never discards
// already-buffered bytes that might begin a real frame.
class FrameParser {
public:
    FrameParser() { reset(); }

    // Drop any partial frame and queued output, and return to hunting. The
    // monotonic health counters are intentionally preserved across reset().
    void reset();

    // Feed a single received byte. Decodes and queues any frame it completes.
    void push(uint8_t b);

    // Feed a span of received bytes (equivalent to push()-ing each in order).
    void push(const uint8_t* data, size_t len);

    // Pull the next fully-decoded frame, or a FrameKind::None frame when the
    // queue is empty. Call in a loop to drain a burst.
    Frame poll();

    // ---- Radar-link health metrics (monotonic; survive reset()) ------------

    // Frames dropped to a checksum mismatch or an oversized length field.
    uint32_t error_count() const { return crc_error_count_; }
    // Same value, named for the health log's CRC-error metric.
    uint32_t crc_error_count() const { return crc_error_count_; }
    // Well-framed records whose type id was not recognized (skipped, not an
    // error) — a "radar firmware speaks a dialect we don't decode" signal.
    uint32_t unknown_count() const { return unknown_count_; }
    // Decoded frames dropped because the output queue was full (burst overrun).
    uint32_t dropped_count() const { return dropped_count_; }

private:
    // Byte-window classification result for the current reassembly buffer.
    enum class Status : uint8_t {
        NeedMore,       // consistent so far, waiting for more bytes
        Complete,       // a full, checksum-valid frame is buffered
        BadChecksum,    // header or data checksum mismatch
        Oversized,      // payload length field exceeds MR60_MAX_PAYLOAD
    };

    // Classify buf_[0..len_-1], assuming buf_[0] == MR60_SOF. On Complete,
    // *payload_len receives the payload byte count.
    Status classify_(size_t* payload_len) const;

    // Decode the complete frame in buf_ (payload of `payload_len` bytes) into
    // the aggregate and queue a snapshot. Returns false + counts `unknown` if
    // the type id is unrecognized.
    bool decode_and_queue_(size_t payload_len);

    // Queue a snapshot of the current aggregate with the given kind.
    void enqueue_(FrameKind kind);

    // Reassembly buffer. buf_[0] is the latched SOF once we are past hunting.
    uint8_t buf_[MR60_MAX_FRAME];
    size_t  len_ = 0;

    // Aggregate of the latest scalar from each frame type.
    bool     agg_has_target_   = false;
    uint8_t  agg_target_count_ = 0;
    uint16_t agg_distance_cm_  = 0;
    uint16_t agg_breath_rate_  = 0;
    uint16_t agg_heart_rate_   = 0;

    // Fixed-capacity decoded-frame ring (no dynamic allocation).
    Frame  queue_[MR60_FRAME_QUEUE];
    size_t q_head_  = 0;
    size_t q_count_ = 0;

    // Monotonic health counters.
    uint32_t crc_error_count_ = 0;
    uint32_t unknown_count_   = 0;
    uint32_t dropped_count_   = 0;
};

}  // namespace securacv::mmwave
