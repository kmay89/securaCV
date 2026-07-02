/**
 * @file mr60_uart.cpp
 * @brief MR60BHA2 UART frame decoder (Phase 2 — real protocol).
 *
 * Incremental, byte-at-a-time state machine that hunts for the 0x01 SOF,
 * validates the header checksum + bounded payload length, validates the data
 * checksum, decodes the known frame types, and resynchronises after any
 * validation failure by dropping the latched SOF byte and re-hunting. Unknown
 * (but well-framed) types are skipped and counted. No dynamic allocation: a
 * fixed reassembly buffer plus a small decoded-frame ring.
 *
 * Protocol constants + layout are documented in mr60_uart.h (source URLs
 * there). The decode below mirrors the ESPHome `seeed_mr60bha2` reference:
 * big-endian header scalar fields, little-endian multi-byte payload fields,
 * XOR-then-invert checksum.
 */

#include "mr60_uart.h"

#include <string.h>  // memmove, memcpy

namespace securacv::mmwave {

// XOR-fold then bitwise-invert — the MR60 checksum for both header and data.
static uint8_t mr60_checksum(const uint8_t* data, size_t len) {
    uint8_t c = 0;
    for (size_t i = 0; i < len; ++i) {
        c ^= data[i];
    }
    return static_cast<uint8_t>(~c);
}

// Big-endian uint16 from two bytes (header id/len/type fields).
static inline uint16_t be16(uint8_t hi, uint8_t lo) {
    return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
}

// Little-endian uint32 from four payload bytes (p[0] is the least significant).
static inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Reinterpret a little-endian float32 payload field as a host float. Matches
// the reference's encode_uint32(...)+memcpy path; host + ESP32 are both
// little-endian so the reconstructed word maps straight onto the float.
static inline float le_float(const uint8_t* p) {
    uint32_t bits = le32(p);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static inline uint16_t clamp_u16(long v) {
    if (v < 0) return 0;
    if (v > 65535) return 65535;
    return static_cast<uint16_t>(v);
}

static inline uint16_t round_bpm(float bpm) {
    // The value comes off an untrusted UART stream: reject NaN/negatives and
    // clamp BEFORE the float->integer cast — casting an out-of-range float
    // (or +inf) to an integer type is undefined behavior in C++.
    if (!(bpm > 0.0f)) return 0;  // also rejects NaN
    if (bpm > 65535.0f) return 65535;
    return clamp_u16(static_cast<long>(bpm + 0.5f));
}

void FrameParser::reset() {
    len_ = 0;
    agg_has_target_   = false;
    agg_target_count_ = 0;
    agg_distance_cm_  = 0;
    agg_breath_rate_  = 0;
    agg_heart_rate_   = 0;
    q_head_  = 0;
    q_count_ = 0;
    // crc_error_count_ / unknown_count_ / dropped_count_ are intentionally
    // preserved so a long-running device keeps a monotonic health tally.
}

FrameParser::Status FrameParser::classify_(size_t* payload_len) const {
    // Caller guarantees len_ >= 1 and buf_[0] == MR60_SOF.

    // Need the whole 8-byte header before we can trust the length field.
    if (len_ < MR60_HEADER_LEN) {
        return Status::NeedMore;
    }

    // Header checksum covers bytes [0..6]; byte [7] carries it.
    if (mr60_checksum(buf_, 7) != buf_[7]) {
        return Status::BadChecksum;
    }

    const uint16_t length = be16(buf_[3], buf_[4]);
    if (length > MR60_MAX_PAYLOAD) {
        return Status::Oversized;
    }

    // Full frame = header(8) + payload(length) + data checksum(1).
    const size_t total = MR60_HEADER_LEN + length + 1;
    if (len_ < total) {
        return Status::NeedMore;
    }

    // Data checksum covers the payload bytes [8 .. 8+length-1].
    if (mr60_checksum(buf_ + MR60_HEADER_LEN, length) !=
        buf_[MR60_HEADER_LEN + length]) {
        return Status::BadChecksum;
    }

    *payload_len = length;
    return Status::Complete;
}

void FrameParser::enqueue_(FrameKind kind) {
    if (q_count_ >= MR60_FRAME_QUEUE) {
        // Burst overrun: drop the oldest so the freshest aggregate wins.
        q_head_ = (q_head_ + 1) % MR60_FRAME_QUEUE;
        --q_count_;
        ++dropped_count_;
    }
    const size_t slot = (q_head_ + q_count_) % MR60_FRAME_QUEUE;
    Frame& f = queue_[slot];
    f.kind         = kind;
    f.has_target   = agg_has_target_;
    f.target_count = agg_target_count_;
    f.distance_cm  = agg_distance_cm_;
    f.breath_rate  = agg_breath_rate_;
    f.heart_rate   = agg_heart_rate_;
    ++q_count_;
}

bool FrameParser::decode_and_queue_(size_t payload_len) {
    const uint16_t type = be16(buf_[5], buf_[6]);
    const uint8_t* p = buf_ + MR60_HEADER_LEN;
    const size_t n = payload_len;

    switch (type) {
        case MR60_TYPE_PEOPLE_EXIST: {
            if (n < 2) return false;  // malformed known type: skip, not counted
            const uint16_t raw = static_cast<uint16_t>(p[0] | (p[1] << 8));
            agg_has_target_ = (raw != 0);
            if (!agg_has_target_) {
                // Mirrors the reference: no target zeroes the other scalars so
                // the FSMs don't ride stale count/distance/vitals after exit.
                agg_target_count_ = 0;
                agg_distance_cm_  = 0;
                agg_breath_rate_  = 0;
                agg_heart_rate_   = 0;
            }
            enqueue_(FrameKind::Presence);
            return true;
        }
        case MR60_TYPE_TARGET_COUNT: {
            if (n < 4) return false;
            const uint32_t count = le32(p);
            agg_target_count_ = (count > 255) ? 255 : static_cast<uint8_t>(count);
            enqueue_(FrameKind::Presence);
            return true;
        }
        case MR60_TYPE_DISTANCE: {
            if (n < 1) return false;
            const bool valid = (p[0] != 0);
            if (valid) {
                // Valid flag set but payload too short for the float: the
                // frame is malformed — do not enqueue stale aggregate data.
                if (n < 8) return false;
                // [BENCH] float32 assumed metres -> centimetres (×100).
                // Range-check before the float->integer cast (UB otherwise
                // for NaN/inf/out-of-range values off the wire).
                const float cm = le_float(p + 4) * 100.0f;
                if (!(cm > 0.0f)) {
                    agg_distance_cm_ = 0;  // also rejects NaN
                } else if (cm > 65535.0f) {
                    agg_distance_cm_ = 65535;
                } else {
                    agg_distance_cm_ = clamp_u16(static_cast<long>(cm + 0.5f));
                }
            } else {
                agg_distance_cm_ = 0;
            }
            enqueue_(FrameKind::Presence);
            return true;
        }
        case MR60_TYPE_BREATH_RATE: {
            if (n < 4) return false;
            agg_breath_rate_ = round_bpm(le_float(p));
            enqueue_(FrameKind::Vitals);
            return true;
        }
        case MR60_TYPE_HEART_RATE: {
            if (n < 4) return false;
            agg_heart_rate_ = round_bpm(le_float(p));
            enqueue_(FrameKind::Vitals);
            return true;
        }
        default:
            // Well-framed but unrecognised type: skip silently, count it.
            ++unknown_count_;
            return false;
    }
}

void FrameParser::push(uint8_t b) {
    // Defensive: never overrun the reassembly buffer (classify_ rejects
    // oversized frames at the header stage, so this should be unreachable).
    if (len_ >= MR60_MAX_FRAME) {
        memmove(buf_, buf_ + 1, MR60_MAX_FRAME - 1);
        --len_;
    }
    buf_[len_++] = b;

    // Re-evaluate the buffer until it needs more bytes or empties. Each
    // iteration either advances a full frame, drops a byte to resync, or
    // returns to wait for more input.
    for (;;) {
        // Hunt: discard any leading non-SOF bytes.
        if (buf_[0] != MR60_SOF) {
            size_t k = 0;
            while (k < len_ && buf_[k] != MR60_SOF) ++k;
            if (k > 0) {
                if (k < len_) memmove(buf_, buf_ + k, len_ - k);
                len_ -= k;
            }
            if (len_ == 0) return;
        }

        size_t payload_len = 0;
        const Status st = classify_(&payload_len);

        if (st == Status::NeedMore) {
            return;
        }

        if (st == Status::Complete) {
            decode_and_queue_(payload_len);
            const size_t total = MR60_HEADER_LEN + payload_len + 1;
            if (total < len_) {
                memmove(buf_, buf_ + total, len_ - total);
                len_ -= total;
            } else {
                len_ = 0;
            }
            if (len_ == 0) return;
            continue;  // trailing bytes may begin the next frame
        }

        // BadChecksum / Oversized: real corruption. Count it, then resync by
        // dropping just the latched SOF byte and re-hunting from the next.
        ++crc_error_count_;
        if (len_ > 1) memmove(buf_, buf_ + 1, len_ - 1);
        --len_;
        if (len_ == 0) return;
        // loop: re-hunt / re-classify the retained bytes
    }
}

void FrameParser::push(const uint8_t* data, size_t len) {
    if (!data) return;
    for (size_t i = 0; i < len; ++i) {
        push(data[i]);
    }
}

Frame FrameParser::poll() {
    Frame f;  // defaults to FrameKind::None
    if (q_count_ == 0) {
        return f;
    }
    f = queue_[q_head_];
    q_head_ = (q_head_ + 1) % MR60_FRAME_QUEUE;
    --q_count_;
    return f;
}

}  // namespace securacv::mmwave
