/**
 * @file mr60_uart.cpp
 * @brief MR60BHA2 UART frame parser — Phase 0 skeleton.
 *
 * The reassembly state machine (hunt for SOF, collect a bounded frame) is real
 * and exercised; the per-type field extraction in try_decode() is a documented
 * TODO that currently reports no frames. This keeps the public interface stable
 * and compiling while the exact Seeed MR60 frame layout (field offsets + CRC)
 * is confirmed against the Seeed_Arduino_mmWave reference on the bench
 * (Phase 0 spike checklist).
 */

#include "mr60_uart.h"

namespace securacv::mmwave {

void FrameParser::reset() {
    state_ = State::HuntSof;
    len_ = 0;
    // error_count_ is intentionally preserved across reset() so a long-running
    // device keeps a monotonic framing-error tally for health reporting.
}

void FrameParser::push(uint8_t b) {
    switch (state_) {
        case State::HuntSof:
            if (b == MR60_SOF) {
                buf_[0] = b;
                len_ = 1;
                state_ = State::Collect;
            }
            // else: stay hunting, drop the byte.
            break;

        case State::Collect:
            if (len_ >= MR60_MAX_FRAME) {
                // Overrun without a valid frame: treat as a framing error and
                // resynchronise from the next start byte.
                ++error_count_;
                reset();
                if (b == MR60_SOF) {
                    buf_[0] = b;
                    len_ = 1;
                    state_ = State::Collect;
                }
                break;
            }
            buf_[len_++] = b;
            break;
    }
}

void FrameParser::push(const uint8_t* data, size_t len) {
    if (!data) return;
    for (size_t i = 0; i < len; ++i) {
        push(data[i]);
    }
}

bool FrameParser::try_decode(Frame& out) {
    // TODO(phase0): implement the real MR60BHA2 frame decode here once the
    // layout is confirmed against Seeed_Arduino_mmWave:
    //   - validate the length/ID preamble after MR60_SOF,
    //   - verify the trailing CRC (increment error_count_ on mismatch),
    //   - map type id -> FrameKind and extract scalars (has_target,
    //     target_count, distance_cm, breath_rate, heart_rate).
    // Until then the parser is wire-shaped but yields nothing, so downstream
    // FSMs run purely on their deadline timeouts (the safe failure direction).
    (void)out;
    return false;
}

Frame FrameParser::poll() {
    Frame f;  // defaults to FrameKind::None
    if (state_ != State::Collect || len_ == 0) {
        return f;
    }
    if (try_decode(f)) {
        // A complete frame was consumed; rearm for the next one.
        reset();
        return f;
    }
    // No complete frame yet (or decode not implemented): keep collecting.
    return f;
}

}  // namespace securacv::mmwave
