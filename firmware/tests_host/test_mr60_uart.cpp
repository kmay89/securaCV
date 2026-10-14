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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
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
Bytes pl_distance(float meters, bool valid = true) {
    Bytes p;
    p.push_back(valid ? 1 : 0);
    p.push_back(0);
    p.push_back(0);
    p.push_back(0);
    put_le_float(p, meters);
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
    assert(p.error_count() == 0);  // not corruption — just unrecognized

    // Still in sync for the next known frame.
    feed(p, frame_heart(70.0f));
    auto v = drain(p);
    assert(v.size() == 1 && v[0].heart_rate == 70);
    std::printf("PASS test_unknown_type_skipped_counted\n");
}

void test_hostile_floats_clamped_not_ub() {
    // Checksums only prove byte integrity — a hostile/corrupt module can
    // still put NaN/Inf/huge floats on the wire. Those must clamp, never hit
    // the C++ UB of casting an out-of-range float to an integer type.
    FrameParser parser;
    feed(parser, frame_breath(std::numeric_limits<float>::infinity()));
    feed(parser, frame_heart(std::nanf("")));
    auto frames = drain(parser);
    assert(!frames.empty());
    assert(frames.back().breath_rate == 65535);  // +inf clamps high
    assert(frames.back().heart_rate == 0);       // NaN rejects to 0

    feed(parser, frame_breath(1e30f));
    frames = drain(parser);
    assert(frames.back().breath_rate == 65535);

    feed(parser, frame_distance(std::numeric_limits<float>::infinity()));
    frames = drain(parser);
    assert(frames.back().distance_cm == 65535);
    feed(parser, frame_distance(std::nanf("")));
    frames = drain(parser);
    assert(frames.back().distance_cm == 0);
    feed(parser, frame_distance(-3.0f));
    frames = drain(parser);
    assert(frames.back().distance_cm == 0);
    std::printf("PASS test_hostile_floats_clamped_not_ub\n");
}

void test_truncated_valid_distance_emits_nothing() {
    // A distance frame with the valid flag set but a payload too short for
    // the float is malformed: it must NOT enqueue a Presence frame carrying
    // stale aggregate data.
    FrameParser parser;
    feed(parser, frame_distance(2.0f));
    auto frames = drain(parser);
    assert(frames.size() == 1 && frames.back().distance_cm == 200);

    Bytes short_payload;
    short_payload.push_back(1);  // valid flag, but no float follows
    feed(parser, build_frame(MR60_TYPE_DISTANCE, short_payload));
    assert(drain(parser).empty());
    std::printf("PASS test_truncated_valid_distance_emits_nothing\n");
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

    // First present frame starts the debounce run and changes NOTHING yet:
    // Unknown never passes through Clear on the way to Present, because the
    // caller signs `presence_cleared` on Clear and a body is in view.
    PresenceEvent e = fsm.tick(decode_one(p, frame_people(true)), t);
    assert(!e.state_changed && fsm.state() == Presence::Unknown);

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

// A radar that stalls while a target is in view, then comes back still
// reporting the target: the FSM must not sign a "cleared" on the way back,
// and must not promote the first returning frame straight to Present off a
// debounce clock that was running before the link dropped.
void test_presence_fsm_stall_recovery_never_reports_clear_with_a_target() {
    PresenceConfig cfg;  // defaults: 300 debounce, 1500 clear, 5000 stall
    PresenceFSM fsm(cfg);
    FrameParser p;

    uint32_t t = 1000;
    fsm.reset(t);
    (void)fsm.tick(decode_one(p, frame_people(true)), t);
    t = 1400;
    PresenceEvent e = fsm.tick(decode_one(p, frame_people(true)), t);
    assert(fsm.state() == Presence::Present);

    // Link drops with the target still present: stall -> Unknown.
    t = 1400 + 5001;
    e = fsm.tick(Frame(), t);
    assert(e.stalled && fsm.state() == Presence::Unknown);

    // Link returns, target still there: no state change, no Clear, and no
    // instant Present (the pre-stall debounce clock must not count).
    t += 10;
    e = fsm.tick(decode_one(p, frame_people(true)), t);
    assert(!e.state_changed);
    assert(fsm.state() == Presence::Unknown);

    // Target sustained past a fresh debounce window -> Present, once.
    t += 299;
    e = fsm.tick(decode_one(p, frame_people(true)), t);
    assert(fsm.state() == Presence::Unknown);
    t += 1;
    e = fsm.tick(decode_one(p, frame_people(true)), t);
    assert(e.state_changed && fsm.state() == Presence::Present);

    // A stall while STILL Unknown (a run opened, the debounce not yet
    // earned, then silence) must also end the run: the returning frame is
    // a fresh start, not the tail of a run that spanned the outage.
    t += 5001;
    e = fsm.tick(Frame(), t);
    assert(e.stalled && fsm.state() == Presence::Unknown);
    t += 10;
    (void)fsm.tick(decode_one(p, frame_people(true)), t);   // opens a run
    assert(fsm.state() == Presence::Unknown);
    t += 5001;                                                // silence, still Unknown
    e = fsm.tick(Frame(), t);
    assert(!e.state_changed && fsm.state() == Presence::Unknown);
    t += 10;
    e = fsm.tick(decode_one(p, frame_people(true)), t);
    assert(fsm.state() == Presence::Unknown);               // fresh debounce, not instant Present
    t += 300;
    e = fsm.tick(decode_one(p, frame_people(true)), t);
    assert(e.state_changed && fsm.state() == Presence::Present);

    // And the other way: link returns reporting NO target -> Clear at once,
    // which is what the radar said.
    t += 5001;
    e = fsm.tick(Frame(), t);
    assert(e.stalled && fsm.state() == Presence::Unknown);
    t += 10;
    e = fsm.tick(decode_one(p, frame_people(false)), t);
    assert(e.state_changed && fsm.state() == Presence::Clear);
    std::printf("PASS test_presence_fsm_stall_recovery_never_reports_clear_with_a_target\n");
}

// --------------------------------------------------------------------------
// The count follows the debounced state (sweep F131).
//
// `Witness` drives PresenceFSM the way canary-sense main.cpp's drive_fsms
// does, as far as what it records and when it dirties the state row:
// presence_detected on a tick that turns Present, presence_cleared on one
// that turns Clear (Unknown signs nothing), occupancy_changed on a count
// change while Present, and a state row on any state or count change. Each
// carries the FSM's state and count after the tick (record_event_now and
// publish_state_now both refresh the snapshot first). The heartbeat
// republishes the row on any tick, so a tick that leaves a count beside a
// state other than Present is a row the device could publish: `contradicted`
// counts them.
// --------------------------------------------------------------------------

struct Record {
    const char* event;     // "state" for a state row
    Presence    presence;
    CountBucket occupants;
};

bool rec_eq(const Record& r, const char* event, Presence p, CountBucket c) {
    return std::strcmp(r.event, event) == 0 && r.presence == p && r.occupants == c;
}

struct Witness {
    PresenceFSM&        fsm;
    std::vector<Record> records;
    std::vector<Record> rows;
    int                 contradicted = 0;

    PresenceEvent drive(const Frame& f, uint32_t t) {
        const PresenceEvent ev = fsm.tick(f, t);
        if (ev.state_changed) {
            if (ev.state == Presence::Present) {
                records.push_back({"presence_detected", fsm.state(), fsm.count()});
            } else if (ev.state == Presence::Clear) {
                records.push_back({"presence_cleared", fsm.state(), fsm.count()});
            }
        }
        if (ev.count_changed && fsm.state() == Presence::Present) {
            records.push_back({"occupancy_changed", fsm.state(), fsm.count()});
        }
        if (ev.state_changed || ev.count_changed) {
            rows.push_back({"state", fsm.state(), fsm.count()});
        }
        if (fsm.state() != Presence::Present && fsm.count() != CountBucket::Zero) {
            ++contradicted;
        }
        return ev;
    }
};

// Walk a room from boot to Present with one occupant the way the decoder
// reports it: the presence flag first (the aggregate's count still 0), then
// the radar's count frame, then presence held past the debounce. Returns the
// time of the tick that turned Present.
uint32_t enter_with_one(Witness& w, FrameParser& p, uint32_t t) {
    w.fsm.reset(t);
    (void)w.drive(decode_one(p, frame_people(true)), t);
    (void)w.drive(decode_one(p, frame_count(1)), t + 100);
    const PresenceEvent e = w.drive(decode_one(p, frame_people(true)), t + 300);
    assert(e.state_changed && w.fsm.state() == Presence::Present);
    assert(w.fsm.count() == CountBucket::One);
    return t + 300;
}

// The room empties: Present (and its count) holds through the clear timeout,
// and the tick that turns Clear carries the zero. The device signs
// presence_cleared, and no occupancy_changed to "0" while still Present (the
// old count followed each frame and signed one 1.5 s before the clear).
void test_emptying_room_signs_presence_cleared_only() {
    PresenceConfig cfg;  // defaults: 300 debounce, 1500 clear, 5000 stall
    PresenceFSM fsm(cfg);
    FrameParser p;
    Witness w{fsm, {}, {}, 0};

    uint32_t t = enter_with_one(w, p, 1000);
    assert(w.records.size() == 1);
    assert(rec_eq(w.records[0], "presence_detected", Presence::Present, CountBucket::One));

    // Everyone leaves. Every no-target frame inside the clear timeout keeps
    // the state Present and the count One.
    t += 100;
    const uint32_t gone = t;
    for (; t < gone + cfg.clear_timeout_ms; t += 100) {
        const PresenceEvent e = w.drive(decode_one(p, frame_people(false)), t);
        assert(!e.state_changed && !e.count_changed);
        assert(fsm.state() == Presence::Present && fsm.count() == CountBucket::One);
    }
    // The tick that turns Clear drops the count in the same tick.
    const PresenceEvent e = w.drive(decode_one(p, frame_people(false)), t);
    assert(e.state_changed && e.state == Presence::Clear);
    assert(e.count == CountBucket::Zero && !e.count_changed);
    assert(fsm.count() == CountBucket::Zero);

    assert(w.records.size() == 2);
    assert(rec_eq(w.records[1], "presence_cleared", Presence::Clear, CountBucket::Zero));
    assert(w.rows.size() == 2);   // the Present row and the Clear row, nothing between
    assert(rec_eq(w.rows[0], "state", Presence::Present, CountBucket::One));
    assert(rec_eq(w.rows[1], "state", Presence::Clear, CountBucket::Zero));
    assert(w.contradicted == 0);
    std::printf("PASS test_emptying_room_signs_presence_cleared_only\n");
}

// A target that drops out for one frame signs nothing and moves no row. On
// the real wire the return is two frames: the presence flag first, with the
// aggregate's count still 0 (the no-target frame zeroed it), then the
// radar's count frame. A target frame that names no count keeps the bucket
// like a no-target frame does, so the 0 in between is not signed either.
void test_one_frame_dropout_signs_nothing() {
    PresenceConfig cfg;
    PresenceFSM fsm(cfg);
    FrameParser p;
    Witness w{fsm, {}, {}, 0};

    uint32_t t = enter_with_one(w, p, 1000);
    const size_t records = w.records.size(), rows = w.rows.size();

    t += 100;
    PresenceEvent e = w.drive(decode_one(p, frame_people(false)), t);   // the dropout
    assert(!e.state_changed && !e.count_changed && fsm.count() == CountBucket::One);
    t += 100;
    const Frame back = decode_one(p, frame_people(true));
    assert(back.has_target && back.target_count == 0);                 // what the decoder emits
    e = w.drive(back, t);
    assert(!e.state_changed && !e.count_changed && fsm.count() == CountBucket::One);
    e = w.drive(decode_one(p, frame_count(1)), t);
    assert(!e.state_changed && !e.count_changed && fsm.count() == CountBucket::One);

    assert(w.records.size() == records && w.rows.size() == rows);

    // A real change while Present is still signed, once, with its bucket.
    t += 100;
    e = w.drive(decode_one(p, frame_count(2)), t);
    assert(e.count_changed && !e.state_changed && fsm.count() == CountBucket::TwoPlus);
    assert(w.records.size() == records + 1);
    assert(rec_eq(w.records.back(), "occupancy_changed", Presence::Present, CountBucket::TwoPlus));
    assert(w.contradicted == 0);
    std::printf("PASS test_one_frame_dropout_signs_nothing\n");
}

// While Clear (or Unknown) the count reads Zero, through the whole Present
// debounce: no state row counts an occupant beside presence "clear" (the old
// count followed the first target frame and dirtied a Clear row counting
// one). The tick that earns Present carries the bucket it was entered with
// and signs presence_detected alone, not an occupancy_changed beside it.
void test_no_occupant_counted_while_clear_during_debounce() {
    PresenceConfig cfg;
    PresenceFSM fsm(cfg);
    FrameParser p;
    Witness w{fsm, {}, {}, 0};

    uint32_t t = 1000;
    fsm.reset(t);
    (void)w.drive(decode_one(p, frame_people(false)), t);   // Unknown -> Clear
    assert(fsm.state() == Presence::Clear);
    assert(w.rows.size() == 1 && rec_eq(w.rows[0], "state", Presence::Clear, CountBucket::Zero));

    // A debounce that is never earned: a target for 200 ms, then gone.
    t += 1000;
    for (uint32_t s = t; t < s + 200; t += 50) {
        if (t == s) (void)w.drive(decode_one(p, frame_people(true)), t);
        else        (void)w.drive(decode_one(p, frame_count(1)), t);
        assert(fsm.state() == Presence::Clear && fsm.count() == CountBucket::Zero);
    }
    (void)w.drive(decode_one(p, frame_people(false)), t);
    assert(w.rows.size() == 1 && w.records.size() == 1);   // only the boot Clear

    // Two people walk in: the count stays Zero until Present is earned, and
    // Present carries 2+.
    t += 1000;
    const uint32_t in = t;
    (void)w.drive(decode_one(p, frame_people(true)), t);
    (void)w.drive(decode_one(p, frame_count(2)), t + 50);
    for (t = in + 100; t < in + cfg.present_debounce_ms; t += 100) {
        (void)w.drive(decode_one(p, frame_people(true)), t);
        assert(fsm.state() == Presence::Clear && fsm.count() == CountBucket::Zero);
    }
    assert(w.rows.size() == 1);   // nothing dirtied the row during the debounce
    const PresenceEvent e = w.drive(decode_one(p, frame_people(true)), in + cfg.present_debounce_ms);
    assert(e.state_changed && e.state == Presence::Present);
    assert(e.count == CountBucket::TwoPlus && !e.count_changed);

    assert(w.records.size() == 2);
    assert(rec_eq(w.records[1], "presence_detected", Presence::Present, CountBucket::TwoPlus));
    assert(w.rows.size() == 2 && rec_eq(w.rows[1], "state", Presence::Present, CountBucket::TwoPlus));
    assert(w.contradicted == 0);
    std::printf("PASS test_no_occupant_counted_while_clear_during_debounce\n");
}

// A stall drops the count in the same tick as the state, and the stall is a
// state change: it carries Zero and reports no separate count move, so it
// signs nothing (Unknown is radar-link health). A count held through a
// dropout rides until the stall, not to "0" ahead of it.
void test_stall_drops_the_count() {
    PresenceConfig cfg;
    PresenceFSM fsm(cfg);
    FrameParser p;
    Witness w{fsm, {}, {}, 0};

    // Straight from Present with 2+.
    uint32_t t = enter_with_one(w, p, 1000);
    (void)w.drive(decode_one(p, frame_count(2)), t + 50);
    assert(fsm.count() == CountBucket::TwoPlus);
    t += 50 + cfg.stall_timeout_ms;
    PresenceEvent e = w.drive(Frame(), t);
    assert(e.stalled && e.state_changed && fsm.state() == Presence::Unknown);
    assert(e.count == CountBucket::Zero && fsm.count() == CountBucket::Zero);
    assert(!e.count_changed);

    // From a dropout: the target leaves and the radar goes silent before the
    // clear timeout. The count holds One until the stall's tick.
    const size_t before = w.records.size();
    t = enter_with_one(w, p, t + 100);
    const size_t entered = w.records.size();
    assert(entered == before + 1);
    t += 100;
    (void)w.drive(decode_one(p, frame_people(false)), t);
    const uint32_t last = t;
    for (t = last + 500; t < last + cfg.stall_timeout_ms; t += 500) {
        (void)w.drive(Frame(), t);   // loop()'s empty-frame ticks
        assert(fsm.state() == Presence::Present && fsm.count() == CountBucket::One);
    }
    e = w.drive(Frame(), last + cfg.stall_timeout_ms);
    assert(e.stalled && fsm.state() == Presence::Unknown && fsm.count() == CountBucket::Zero);
    assert(!e.count_changed);
    assert(w.records.size() == entered);   // no occupancy_changed, no presence_cleared
    assert(w.contradicted == 0);
    std::printf("PASS test_stall_drops_the_count\n");
}

#ifdef CANARY_SENSE_VITALS
// The vitals gate (canary-sense main.cpp): `single_target = (pev.count ==
// CountBucket::One)`, so VitalsFSM now sees the debounced count. Decided and
// pinned here (F131): a held One on a no-target frame inside the clear
// timeout is still single_target, so a held lock keeps reporting its last
// BPMs on that tick and an acquiring run is not restarted by a one-frame
// dropout; the tick that turns Clear zeroes the count and suppresses the
// BPMs at once. During the Present debounce the count is Zero, so vitals
// cannot start acquiring before Present is earned.
void test_vitals_gate_sees_the_debounced_count() {
    PresenceConfig pcfg;   // 300 debounce, 1500 clear
    VitalsConfig vcfg;     // 4000 confirm, 6000 lost
    PresenceFSM presence(pcfg);
    VitalsFSM vitals(vcfg);
    FrameParser p;

    struct Tick { PresenceEvent pev; VitalsEvent vev; };
    auto drive = [&](const Frame& f, uint32_t t) {
        Tick k;
        k.pev = presence.tick(f, t);
        const bool single_target = (k.pev.count == CountBucket::One);   // main.cpp's gate
        k.vev = vitals.tick(f, single_target, t);
        return k;
    };

    uint32_t t = 1000;
    presence.reset(t);
    vitals.reset(t);
    (void)drive(decode_one(p, frame_people(true)), t);
    (void)drive(decode_one(p, frame_count(1)), t + 50);
    // A plausible vitals pair during the debounce: single_target is false
    // (the count is Zero until Present), so no acquiring run starts on it.
    (void)drive(decode_one(p, frame_breath(16.0f)), t + 100);
    Tick k = drive(decode_one(p, frame_heart(70.0f)), t + 100);
    assert(presence.state() != Presence::Present && k.pev.count == CountBucket::Zero);
    assert(!k.vev.bpm_valid);
    k = drive(decode_one(p, frame_people(true)), t + pcfg.present_debounce_ms);
    assert(presence.state() == Presence::Present && k.pev.count == CountBucket::One);

    // Vitals once a second, presence frames between: the run starts at the
    // first vitals pair seen while Present, so the lock lands lock_confirm_ms
    // after it, not after the debounce-window pair.
    const uint32_t present_at = t + pcfg.present_debounce_ms;
    const uint32_t first = present_at + 1000;
    for (t = first; t < first + vcfg.lock_confirm_ms; t += 1000) {
        (void)drive(decode_one(p, frame_breath(16.0f)), t);
        k = drive(decode_one(p, frame_heart(70.0f)), t);
        assert(vitals.lock() != VitalsLock::Locked);
        (void)drive(decode_one(p, frame_people(true)), t + 500);
    }
    (void)drive(decode_one(p, frame_breath(16.0f)), t);
    k = drive(decode_one(p, frame_heart(70.0f)), t);
    assert(vitals.lock() == VitalsLock::Locked && k.vev.bpm_valid);
    assert(k.vev.breath_bpm == 16 && k.vev.heart_bpm == 70);

    // The room empties. On every no-target tick inside the clear timeout the
    // presence FSM still says one occupant, so the lock keeps reporting its
    // last BPMs (the old gate, fed by each frame, suppressed them on the
    // first no-target frame).
    t += 100;
    const uint32_t gone = t;
    for (; t < gone + pcfg.clear_timeout_ms; t += 100) {
        k = drive(decode_one(p, frame_people(false)), t);
        assert(presence.state() == Presence::Present && k.pev.count == CountBucket::One);
        assert(k.vev.bpm_valid && k.vev.breath_bpm == 16 && k.vev.heart_bpm == 70);
    }
    // The Clear tick zeroes the count: the BPMs go in the same tick, while
    // the lock itself rides out lost_ms as before.
    k = drive(decode_one(p, frame_people(false)), t);
    assert(k.pev.state_changed && presence.state() == Presence::Clear);
    assert(k.pev.count == CountBucket::Zero);
    assert(!k.vev.bpm_valid && k.vev.breath_bpm == 0 && k.vev.heart_bpm == 0);
    assert(vitals.lock() == VitalsLock::Locked);
    std::printf("PASS test_vitals_gate_sees_the_debounced_count\n");
}

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

// Regression: on the real wire, presence/count/distance frames interleave
// between every vitals report (and loop() ticks an empty frame when nothing
// arrived at all). Those non-vitals ticks must NOT break the valid run — the
// original tick() treated them as invalid observations, resetting
// valid_since on every interleave, so the confirm window could never elapse
// against real traffic and the lock was unreachable. (Found by the Sense Lab
// bench, canary-local/senselab.html, whose stream models the real frame mix.)
void test_vitals_lock_survives_interleaved_presence() {
    VitalsConfig cfg;  // 4000 confirm, 6000 lost
    VitalsFSM fsm(cfg);
    FrameParser p;

    uint32_t t = 0;
    fsm.reset(t);

    // Seed the aggregate, then stream the realistic mix: presence frames at
    // 10 Hz, an empty tick between them, vitals once a second.
    (void)fsm.tick(decode_one(p, frame_breath(16.0f)), true, t);
    VitalsEvent e = fsm.tick(decode_one(p, frame_heart(70.0f)), true, t);
    assert(fsm.lock() == VitalsLock::Lost);

    for (t = 100; t <= 5000; t += 100) {
        if (t % 1000 == 0) {
            (void)fsm.tick(decode_one(p, frame_breath(16.0f)), true, t);
            e = fsm.tick(decode_one(p, frame_heart(70.0f)), true, t);
        } else {
            (void)fsm.tick(decode_one(p, frame_people(true)), true, t);
            e = fsm.tick(Frame(), true, t);  // loop()'s empty-frame tick
        }
    }
    assert(fsm.lock() == VitalsLock::Locked);
    assert(e.bpm_valid && e.heart_bpm == 70 && e.breath_bpm == 16);

    // The non-vitals ticks still honor immediate multi-person suppression...
    e = fsm.tick(Frame(), /*single_target=*/false, t);
    assert(!e.bpm_valid && e.heart_bpm == 0);

    // ...and the lost deadline still fires through pure presence traffic.
    e = fsm.tick(decode_one(p, frame_people(true)), true, t + 6001);
    assert(fsm.lock() == VitalsLock::Lost);
    std::printf("PASS test_vitals_lock_survives_interleaved_presence\n");
}

// An ambiguous (multi-person) interval must reset the ACQUIRING run even
// when the ambiguity is only ever visible on non-vitals ticks — a lock may
// never be acquired on confirmation credit that straddles a count != 1
// interval (Codex review on the interleave fix: without this, a second
// person entering and leaving between 1 Hz vitals reports let the next
// single-target sample inherit the old run and lock instantly).
void test_vitals_ambiguity_resets_acquiring_run() {
    VitalsConfig cfg;  // 4000 confirm, 6000 lost
    VitalsFSM fsm(cfg);
    FrameParser p;

    uint32_t t = 0;
    fsm.reset(t);

    // Build 3 s of valid single-target credit (not yet the 4 s confirm).
    (void)fsm.tick(decode_one(p, frame_breath(16.0f)), true, t);
    for (t = 1000; t <= 3000; t += 1000) {
        (void)fsm.tick(decode_one(p, frame_heart(70.0f)), true, t);
    }
    assert(fsm.lock() == VitalsLock::Lost);  // seen, unconfirmed

    // A second person flickers through between vitals reports: only
    // presence-kind ticks see single_target == false.
    (void)fsm.tick(decode_one(p, frame_people(true)), /*single_target=*/false, 3300);

    // Back to one person. The next vitals sample lands where the OLD run
    // would have satisfied the confirm window (4 s+) — it must NOT lock.
    VitalsEvent e = fsm.tick(decode_one(p, frame_heart(70.0f)), true, 4500);
    assert(fsm.lock() == VitalsLock::Lost);
    assert(!e.bpm_valid);

    // A clean sustained run from here does lock, on the restarted window.
    for (t = 5500; t <= 4500 + cfg.lock_confirm_ms; t += 1000) {
        e = fsm.tick(decode_one(p, frame_heart(70.0f)), true, t);
    }
    assert(fsm.lock() == VitalsLock::Locked);
    assert(e.bpm_valid && e.heart_bpm == 70);
    std::printf("PASS test_vitals_ambiguity_resets_acquiring_run\n");
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
    test_hostile_floats_clamped_not_ub();
    test_truncated_valid_distance_emits_nothing();
    test_presence_fsm_integration();
    test_presence_fsm_stall_recovery_never_reports_clear_with_a_target();
    test_emptying_room_signs_presence_cleared_only();
    test_one_frame_dropout_signs_nothing();
    test_no_occupant_counted_while_clear_during_debounce();
    test_stall_drops_the_count();
#ifdef CANARY_SENSE_VITALS
    test_vitals_gate_sees_the_debounced_count();
    test_vitals_fsm_integration();
    test_vitals_lock_survives_interleaved_presence();
    test_vitals_ambiguity_resets_acquiring_run();
#endif
    std::printf("ALL MR60 UART TESTS PASSED\n");
    return 0;
}
