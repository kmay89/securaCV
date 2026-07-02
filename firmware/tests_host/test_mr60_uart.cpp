/**
 * @file test_mr60_uart.cpp
 * @brief Host-build unit tests for the MR60BHA2 UART frame decoder.
 *
 * Compiles Arduino-free with a plain host toolchain (see the sibling Makefile):
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/common/sensors/mmwave_mr60 \
 *       firmware/tests_host/test_mr60_uart.cpp \
 *       firmware/common/sensors/mmwave_mr60/mr60_uart.cpp \
 *       firmware/common/sensors/mmwave_mr60/mr60_presence.cpp \
 *       firmware/common/sensors/mmwave_mr60/mr60_vitals.cpp \
 *       -o /tmp/test_mr60_uart && /tmp/test_mr60_uart
 *
 * Building with -DCANARY_SENSE_VITALS additionally links the vitals FSM and
 * runs the vitals-lock integration checks.
 *
 * The golden frames are constructed here from a self-contained checksum helper
 * (mr60_checksum below), so the test doubles as executable documentation of
 * the wire format decoded in mr60_uart.cpp.
 */

#include "mr60_presence.h"
#include "mr60_uart.h"
#ifdef CANARY_SENSE_VITALS
#include "mr60_vitals.h"
#endif

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace securacv::mmwave;

namespace {

using Bytes = std::vector<uint8_t>;

// --------------------------------------------------------------------------
// Frame construction helpers — mirror the protocol so tests self-document.
// --------------------------------------------------------------------------

// XOR-fold then bitwise-invert: the MR60 checksum for header and data alike.
uint8_t mr60_checksum(const uint8_t* data, size_t len) {
    uint8_t c = 0;
    for (size_t i = 0; i < len; ++i) c ^= data[i];
    return static_cast<uint8_t>(~c);
}

void put_le32(Bytes& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void put_le_float(Bytes& b, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    put_le32(b, bits);
}

// Build the 8-byte header (SOF, big-endian id/len/type, header checksum) for an
// arbitrary declared payload length — used both for well-formed frames and for
// the oversized-length rejection test.
Bytes build_header(uint16_t type, uint16_t length, uint16_t id = 0x0102) {
    Bytes h;
    h.push_back(MR60_SOF);
    h.push_back(static_cast<uint8_t>(id >> 8));
    h.push_back(static_cast<uint8_t>(id & 0xFF));
    h.push_back(static_cast<uint8_t>(length >> 8));
    h.push_back(static_cast<uint8_t>(length & 0xFF));
    h.push_back(static_cast<uint8_t>(type >> 8));
    h.push_back(static_cast<uint8_t>(type & 0xFF));
    h.push_back(mr60_checksum(h.data(), 7));  // header checksum over [0..6]
    return h;
}

// Build a complete, checksum-valid frame from a payload.
Bytes build_frame(uint16_t type, const Bytes& payload, uint16_t id = 0x0102) {
    Bytes f = build_header(type, static_cast<uint16_t>(payload.size()), id);
    f.insert(f.end(), payload.begin(), payload.end());
    f.push_back(mr60_checksum(payload.data(), payload.size()));
    return f;
}

// Per-type payload builders.
Bytes pl_people_exist(bool present) {
    Bytes p;
    uint16_t v = present ? 1 : 0;
    p.push_back(static_cast<uint8_t>(v & 0xFF));
    p.push_back(static_cast<uint8_t>(v >> 8));
    return p;
}
Bytes pl_target_count(uint32_t n) {
    Bytes p;
    put_le32(p, n);
    return p;
}
Bytes pl_distance(float metres, bool valid = true) {
    Bytes p;
    p.push_back(valid ? 1 : 0);
    p.push_back(0);
    p.push_back(0);
    p.push_back(0);
    put_le_float(p, metres);
    return p;
}
Bytes pl_breath(float bpm) {
    Bytes p;
    put_le_float(p, bpm);
    return p;
}
Bytes pl_heart(float bpm) {
    Bytes p;
    put_le_float(p, bpm);
    return p;
}

Bytes frame_people(bool present) { return build_frame(MR60_TYPE_PEOPLE_EXIST, pl_people_exist(present)); }
Bytes frame_count(uint32_t n)    { return build_frame(MR60_TYPE_TARGET_COUNT, pl_target_count(n)); }
Bytes frame_distance(float m)    { return build_frame(MR60_TYPE_DISTANCE, pl_distance(m)); }
Bytes frame_breath(float bpm)    { return build_frame(MR60_TYPE_BREATH_RATE, pl_breath(bpm)); }
Bytes frame_heart(float bpm)     { return build_frame(MR60_TYPE_HEART_RATE, pl_heart(bpm)); }

// Drain every queued frame from a parser.
std::vector<Frame> drain(FrameParser& p) {
    std::vector<Frame> out;
    for (Frame f = p.poll(); f.kind != FrameKind::None; f = p.poll()) {
        out.push_back(f);
    }
    return out;
}

bool frame_eq(const Frame& a, const Frame& b) {
    return a.kind == b.kind && a.has_target == b.has_target &&
           a.target_count == b.target_count && a.distance_cm == b.distance_cm &&
           a.breath_rate == b.breath_rate && a.heart_rate == b.heart_rate;
}

void feed(FrameParser& p, const Bytes& b) { p.push(b.data(), b.size()); }

// --------------------------------------------------------------------------
// Tests
// --------------------------------------------------------------------------

void test_golden_each_type() {
    FrameParser p;

    feed(p, frame_people(true));
    auto v = drain(p);
    assert(v.size() == 1);
    assert(v[0].kind == FrameKind::Presence);
    assert(v[0].has_target == true);

    feed(p, frame_count(3));
    v = drain(p);
    assert(v.size() == 1);
    assert(v[0].kind == FrameKind::Presence);
    assert(v[0].target_count == 3);
    assert(v[0].has_target == true);  // aggregated from the earlier presence

    feed(p, frame_distance(1.5f));
    v = drain(p);
    assert(v.size() == 1);
    assert(v[0].kind == FrameKind::Presence);
    assert(v[0].distance_cm == 150);  // 1.5 m -> 150 cm

    feed(p, frame_breath(16.0f));
    v = drain(p);
    assert(v.size() == 1);
    assert(v[0].kind == FrameKind::Vitals);
    assert(v[0].breath_rate == 16);

    feed(p, frame_heart(72.0f));
    v = drain(p);
    assert(v.size() == 1);
    assert(v[0].kind == FrameKind::Vitals);
    assert(v[0].heart_rate == 72);
    assert(v[0].breath_rate == 16);  // aggregated

    // Rounding + zero handling.
    feed(p, frame_breath(15.5f));
    v = drain(p);
    assert(v.size() == 1 && v[0].breath_rate == 16);

    assert(p.error_count() == 0);
    assert(p.unknown_count() == 0);
    std::printf("PASS test_golden_each_type\n");
}

void test_presence_absent_zeroes_aggregate() {
    FrameParser p;
    feed(p, frame_people(true));
    feed(p, frame_count(2));
    feed(p, frame_distance(2.0f));
    feed(p, frame_breath(18.0f));
    feed(p, frame_heart(66.0f));
    (void)drain(p);

    feed(p, frame_people(false));
    auto v = drain(p);
    assert(v.size() == 1);
    assert(v[0].kind == FrameKind::Presence);
    assert(v[0].has_target == false);
    assert(v[0].target_count == 0);
    assert(v[0].distance_cm == 0);
    assert(v[0].breath_rate == 0);
    assert(v[0].heart_rate == 0);
    std::printf("PASS test_presence_absent_zeroes_aggregate\n");
}

void test_byte_vs_buffer_equivalence() {
    // Concatenate one of every frame type into a single stream.
    Bytes stream;
    for (const Bytes& f : {frame_people(true), frame_count(2), frame_distance(2.0f),
                           frame_breath(18.0f), frame_heart(66.0f), frame_people(false)}) {
        stream.insert(stream.end(), f.begin(), f.end());
    }

    FrameParser whole;
    whole.push(stream.data(), stream.size());
    auto va = drain(whole);

    FrameParser bytewise;
    for (uint8_t b : stream) bytewise.push(b);
    auto vb = drain(bytewise);

    assert(va.size() == 6);
    assert(va.size() == vb.size());
    for (size_t i = 0; i < va.size(); ++i) {
        assert(frame_eq(va[i], vb[i]));
    }
    std::printf("PASS test_byte_vs_buffer_equivalence\n");
}

void test_corrupt_header_checksum_then_resync() {
    FrameParser p;
    Bytes bad = frame_people(true);
    bad[7] ^= 0xFF;  // clobber the header checksum
    feed(p, bad);
    assert(drain(p).empty());
    assert(p.error_count() >= 1);

    // A valid frame after the corruption still decodes (resync works).
    feed(p, frame_people(true));
    auto v = drain(p);
    assert(v.size() == 1 && v[0].has_target == true);
    std::printf("PASS test_corrupt_header_checksum_then_resync\n");
}

void test_corrupt_data_checksum_then_resync() {
    FrameParser p;
    Bytes bad = frame_distance(1.5f);
    bad.back() ^= 0xFF;  // clobber the data checksum
    feed(p, bad);
    assert(drain(p).empty());
    assert(p.error_count() >= 1);

    feed(p, frame_count(1));
    auto v = drain(p);
    assert(v.size() == 1 && v[0].target_count == 1);
    std::printf("PASS test_corrupt_data_checksum_then_resync\n");
}

void test_truncated_then_valid_frame() {
    FrameParser p;
    // A valid frame missing its trailing data-checksum byte...
    Bytes trunc = frame_distance(3.0f);
    trunc.pop_back();
    feed(p, trunc);
    assert(drain(p).empty());  // incomplete: nothing yet

    // ...followed by a complete valid frame. The decoder must recover it.
    feed(p, frame_people(true));
    auto v = drain(p);
    assert(v.size() >= 1);
    assert(v.back().kind == FrameKind::Presence);
    assert(v.back().has_target == true);
    std::printf("PASS test_truncated_then_valid_frame\n");
}

void test_garbage_flood_no_frame_no_crash() {
    FrameParser p;
    // Deterministic non-frame noise, including plenty of 0x01 SOF bytes to
    // exercise the hunt/reject/resync path without ever forming a valid frame.
    uint32_t x = 0x12345678u;
    for (int i = 0; i < 4000; ++i) {
        x = x * 1103515245u + 12345u;
        uint8_t b = (i % 5 == 0) ? MR60_SOF : static_cast<uint8_t>(x >> 16);
        p.push(b);
    }
    assert(drain(p).empty());  // no complete valid frame ever emerged
    std::printf("PASS test_garbage_flood_no_frame_no_crash\n");
}

void test_oversized_length_rejected() {
    FrameParser p;
    // Well-formed header whose declared length blows past MR60_MAX_PAYLOAD.
    Bytes hdr = build_header(MR60_TYPE_DISTANCE,
                             static_cast<uint16_t>(MR60_MAX_PAYLOAD + 200));
    feed(p, hdr);
    hdr.clear();
    // A few filler payload bytes; the parser should already have rejected.
    for (int i = 0; i < 4; ++i) p.push(0x55);
    assert(drain(p).empty());
    assert(p.error_count() >= 1);

    // Recovers on the next valid frame.
    feed(p, frame_people(true));
    auto v = drain(p);
    assert(v.size() == 1 && v[0].has_target == true);
    std::printf("PASS test_oversized_length_rejected\n");
}

void test_unknown_type_skipped_counted() {
    FrameParser p;
    // A checksum-valid frame with a type id we don't decode.
    Bytes payload;
    put_le32(payload, 0xDEADBEEFu);
    feed(p, build_frame(0x1234, payload));
    assert(drain(p).empty());
    assert(p.unknown_count() == 1);
    assert(p.error_count() == 0);  // not corruption — just unrecognised

    // Still in sync for the next known frame.
    feed(p, frame_heart(70.0f));
    auto v = drain(p);
    assert(v.size() == 1 && v[0].heart_rate == 70);
    std::printf("PASS test_unknown_type_skipped_counted\n");
}

void test_burst_queue_multiple_frames() {
    FrameParser p;
    Bytes stream;
    for (int i = 0; i < 5; ++i) {
        Bytes f = frame_count(static_cast<uint32_t>(i + 1));
        stream.insert(stream.end(), f.begin(), f.end());
    }
    feed(p, stream);
    auto v = drain(p);
    assert(v.size() == 5);
    for (size_t i = 0; i < v.size(); ++i) {
        assert(v[i].target_count == static_cast<uint8_t>(i + 1));
    }
    std::printf("PASS test_burst_queue_multiple_frames\n");
}

// --------------------------------------------------------------------------
// FSM integration: golden presence frames through PresenceFSM.
// --------------------------------------------------------------------------

// Push a people-exist frame through the parser and return the decoded Frame
// (or a None frame). Presence frames are one-per-poll here.
Frame decode_one(FrameParser& p, const Bytes& raw) {
    feed(p, raw);
    return p.poll();
}

void test_presence_fsm_integration() {
    PresenceConfig cfg;  // defaults: 300 debounce, 1500 clear, 5000 stall
    PresenceFSM fsm(cfg);
    FrameParser p;

    uint32_t t = 1000;
    fsm.reset(t);
    assert(fsm.state() == Presence::Unknown);

    // First present frame lifts Unknown -> Clear (debounce not yet met).
    PresenceEvent e = fsm.tick(decode_one(p, frame_people(true)), t);
    assert(e.state_changed && fsm.state() == Presence::Clear);

    // Sustained presence past the debounce window -> Present.
    t = 1400;
    e = fsm.tick(decode_one(p, frame_people(true)), t);
    assert(fsm.state() == Presence::Present);
    assert(e.state == Presence::Present);

    // Count + range track the aggregate once those frames arrive.
    (void)fsm.tick(decode_one(p, frame_count(1)), t);
    e = fsm.tick(decode_one(p, frame_distance(1.0f)), t);
    assert(fsm.count() == CountBucket::One);
    assert(fsm.range() == RangeBand::Near);  // 100 cm <= near_cm(150)

    // Target leaves; Present holds until the clear timeout elapses.
    t = 1500;
    e = fsm.tick(decode_one(p, frame_people(false)), t);
    assert(fsm.state() == Presence::Present);
    t = 3100;  // 1600 ms of absence > clear_timeout_ms(1500)
    e = fsm.tick(decode_one(p, frame_people(false)), t);
    assert(fsm.state() == Presence::Clear);
    assert(fsm.count() == CountBucket::Zero);

    // Radar goes silent: the deadline check drives the FSM to Unknown even
    // though we only ever pass empty frames from here.
    t = 3100 + 5001;
    e = fsm.tick(Frame(), t);
    assert(e.stalled && fsm.state() == Presence::Unknown);
    std::printf("PASS test_presence_fsm_integration\n");
}

#ifdef CANARY_SENSE_VITALS
void test_vitals_fsm_integration() {
    VitalsConfig cfg;  // 4000 confirm, 6000 lost, breath 6..30, heart 40..130
    VitalsFSM fsm(cfg);
    FrameParser p;

    uint32_t t = 0;
    fsm.reset(t);
    assert(fsm.lock() == VitalsLock::Unknown);

    // Seed both scalars so the aggregate carries a plausible breath+heart pair.
    (void)fsm.tick(decode_one(p, frame_breath(16.0f)), true, t);
    VitalsEvent e = fsm.tick(decode_one(p, frame_heart(70.0f)), true, t);
    assert(fsm.lock() == VitalsLock::Lost);  // seen data, not yet confirmed

    // Sustain a valid single-target vitals stream past the confirm window.
    for (t = 500; t <= 4500; t += 500) {
        e = fsm.tick(decode_one(p, frame_heart(70.0f)), true, t);
    }
    assert(fsm.lock() == VitalsLock::Locked);
    assert(e.bpm_valid && e.heart_bpm == 70 && e.breath_bpm == 16);

    // A second occupant immediately suppresses BPM reporting.
    e = fsm.tick(decode_one(p, frame_heart(70.0f)), /*single_target=*/false, t);
    assert(!e.bpm_valid && e.heart_bpm == 0);

    // Radar silence drives the lock to Lost via the deadline.
    e = fsm.tick(Frame(), true, t + 6001);
    assert(e.stalled && fsm.lock() == VitalsLock::Lost);
    std::printf("PASS test_vitals_fsm_integration\n");
}
#endif

}  // namespace

int main() {
    test_golden_each_type();
    test_presence_absent_zeroes_aggregate();
    test_byte_vs_buffer_equivalence();
    test_corrupt_header_checksum_then_resync();
    test_corrupt_data_checksum_then_resync();
    test_truncated_then_valid_frame();
    test_garbage_flood_no_frame_no_crash();
    test_oversized_length_rejected();
    test_unknown_type_skipped_counted();
    test_burst_queue_multiple_frames();
    test_presence_fsm_integration();
#ifdef CANARY_SENSE_VITALS
    test_vitals_fsm_integration();
#endif
    std::printf("ALL MR60 UART TESTS PASSED\n");
    return 0;
}
