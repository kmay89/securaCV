/*
 * Host-build test for mesh_channel_hop wire format + HopTracker.
 * Compile: g++ -std=c++17 -DCSI_TEST_HOST_BUILD -I src \
 *          src/mesh_channel_hop.cpp test_mesh_channel_hop.cpp -o test && ./test
 */

#include "mesh_channel_hop.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace mesh_channel_hop;

/* ── Wire format encode/decode ─────────────────────────────────────────── */

static void test_encode_decode_roundtrip() {
  uint8_t buf[PAYLOAD_LEN];
  assert(encode(6, Reason::UTILIZATION, buf, sizeof(buf)));
  assert(buf[0] == 6);
  assert(buf[1] == 0);

  uint8_t ch = 0;
  Reason  r  = Reason::MANUAL;
  assert(decode(buf, sizeof(buf), &ch, &r));
  assert(ch == 6);
  assert(r == Reason::UTILIZATION);
  printf("  PASS: encode_decode_roundtrip\n");
}

static void test_encode_all_channels() {
  uint8_t buf[PAYLOAD_LEN];
  for (uint8_t ch = MIN_CHANNEL; ch <= MAX_CHANNEL; ++ch) {
    assert(encode(ch, Reason::INITIAL, buf, sizeof(buf)));
    uint8_t out_ch = 0;
    Reason  out_r  = Reason::UTILIZATION;
    assert(decode(buf, sizeof(buf), &out_ch, &out_r));
    assert(out_ch == ch);
    assert(out_r == Reason::INITIAL);
  }
  printf("  PASS: encode_all_channels (1..14)\n");
}

static void test_encode_rejects_channel_zero() {
  uint8_t buf[PAYLOAD_LEN];
  assert(!encode(0, Reason::UTILIZATION, buf, sizeof(buf)));
  printf("  PASS: encode_rejects_channel_zero\n");
}

static void test_encode_rejects_channel_above_max() {
  uint8_t buf[PAYLOAD_LEN];
  assert(!encode(15, Reason::UTILIZATION, buf, sizeof(buf)));
  assert(!encode(255, Reason::UTILIZATION, buf, sizeof(buf)));
  printf("  PASS: encode_rejects_channel_above_max\n");
}

static void test_encode_rejects_null_buffer() {
  assert(!encode(6, Reason::UTILIZATION, nullptr, PAYLOAD_LEN));
  printf("  PASS: encode_rejects_null_buffer\n");
}

static void test_encode_rejects_small_buffer() {
  uint8_t buf[1];
  assert(!encode(6, Reason::UTILIZATION, buf, sizeof(buf)));
  printf("  PASS: encode_rejects_small_buffer\n");
}

static void test_decode_rejects_wrong_length() {
  uint8_t buf[3] = {6, 0, 0};
  uint8_t ch;
  Reason  r;
  assert(!decode(buf, 3, &ch, &r));
  assert(!decode(buf, 1, &ch, &r));
  assert(!decode(buf, 0, &ch, &r));
  printf("  PASS: decode_rejects_wrong_length\n");
}

static void test_decode_rejects_bad_channel() {
  uint8_t buf[PAYLOAD_LEN] = {0, 0};  /* channel 0 */
  uint8_t ch;
  Reason  r;
  assert(!decode(buf, sizeof(buf), &ch, &r));

  buf[0] = 15;
  assert(!decode(buf, sizeof(buf), &ch, &r));
  printf("  PASS: decode_rejects_bad_channel\n");
}

static void test_decode_rejects_null_pointers() {
  uint8_t buf[PAYLOAD_LEN] = {6, 0};
  uint8_t ch;
  Reason  r;
  assert(!decode(nullptr, PAYLOAD_LEN, &ch, &r));
  assert(!decode(buf, PAYLOAD_LEN, nullptr, &r));
  assert(!decode(buf, PAYLOAD_LEN, &ch, nullptr));
  printf("  PASS: decode_rejects_null_pointers\n");
}

static void test_all_reason_codes() {
  uint8_t buf[PAYLOAD_LEN];
  const Reason reasons[] = {
    Reason::UTILIZATION, Reason::INTERFERENCE,
    Reason::MANUAL, Reason::INITIAL
  };
  for (Reason r : reasons) {
    assert(encode(1, r, buf, sizeof(buf)));
    uint8_t ch;
    Reason  out;
    assert(decode(buf, sizeof(buf), &ch, &out));
    assert(out == r);
  }
  printf("  PASS: all_reason_codes\n");
}

/* ── next_channel ──────────────────────────────────────────────────────── */

static void test_next_channel_rotation() {
  assert(next_channel(1)  == 6);
  assert(next_channel(6)  == 11);
  assert(next_channel(11) == 1);
  printf("  PASS: next_channel_rotation\n");
}

static void test_next_channel_unknown_returns_zero() {
  assert(next_channel(0)  == 0);
  assert(next_channel(3)  == 0);
  assert(next_channel(14) == 0);
  printf("  PASS: next_channel_unknown_returns_zero\n");
}

/* ── HopTracker ────────────────────────────────────────────────────────── */

static void test_tracker_fires_after_sustain() {
  HopTracker t = make_tracker(5000, 60000, 120000);

  assert(!tick(t, 0,     5000));
  assert(!tick(t, 30000, 5000));
  assert(!tick(t, 59999, 5000));
  assert( tick(t, 60000, 5000));
  printf("  PASS: tracker_fires_after_sustain\n");
}

static void test_tracker_resets_on_dip() {
  HopTracker t = make_tracker(5000, 60000, 120000);

  assert(!tick(t, 0,     5000));
  assert(!tick(t, 50000, 5000));
  /* Dip below threshold resets the counter */
  assert(!tick(t, 55000, 4999));
  assert(!tick(t, 55001, 5000));
  /* Now need another full 60s from 55001 */
  assert(!tick(t, 115000, 5000));
  assert( tick(t, 115001, 5000));
  printf("  PASS: tracker_resets_on_dip\n");
}

static void test_tracker_cooldown() {
  HopTracker t = make_tracker(5000, 60000, 120000);

  assert(!tick(t, 0,     5000));
  assert( tick(t, 60000, 5000));
  reset(t, 60000);

  /* During cooldown, high utilization is ignored */
  assert(!tick(t, 60001,  5000));
  assert(!tick(t, 100000, 5000));
  assert(!tick(t, 179999, 5000));

  /* Cooldown expires at 60000 + 120000 = 180000; tick re-arms but
   * returns false on the arming tick itself. */
  assert(!tick(t, 180000, 5000));
  /* Re-armed — above_since starts at next tick (200000) */
  assert(!tick(t, 200000, 5000));
  assert(!tick(t, 259999, 5000));
  /* 200000 + 60000 = 260000 → fires */
  assert( tick(t, 260000, 5000));
  printf("  PASS: tracker_cooldown\n");
}

static void test_tracker_below_threshold_no_fire() {
  HopTracker t = make_tracker(5000, 60000, 120000);

  assert(!tick(t, 0,      4999));
  assert(!tick(t, 60000,  4999));
  assert(!tick(t, 120000, 4999));
  printf("  PASS: tracker_below_threshold_no_fire\n");
}

static void test_tracker_fires_once_until_reset() {
  HopTracker t = make_tracker(5000, 1000, 5000);

  assert(!tick(t, 0,    5000));
  assert( tick(t, 1000, 5000));
  /* Without reset, tick keeps returning true */
  assert( tick(t, 1001, 5000));
  /* After reset, enters cooldown */
  reset(t, 1001);
  assert(!tick(t, 1002, 5000));
  printf("  PASS: tracker_fires_once_until_reset\n");
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main() {
  printf("mesh_channel_hop tests:\n");

  test_encode_decode_roundtrip();
  test_encode_all_channels();
  test_encode_rejects_channel_zero();
  test_encode_rejects_channel_above_max();
  test_encode_rejects_null_buffer();
  test_encode_rejects_small_buffer();
  test_decode_rejects_wrong_length();
  test_decode_rejects_bad_channel();
  test_decode_rejects_null_pointers();
  test_all_reason_codes();
  test_next_channel_rotation();
  test_next_channel_unknown_returns_zero();
  test_tracker_fires_after_sustain();
  test_tracker_resets_on_dip();
  test_tracker_cooldown();
  test_tracker_below_threshold_no_fire();
  test_tracker_fires_once_until_reset();

  printf("ALL 17 PASSED\n");
  return 0;
}
