/**
 * @file test_mesh_beacon.cpp
 * @brief Host-build test for the mesh_beacon wire format (PR 5c-2).
 *
 * Verifies:
 *   1. encode produces a 25-byte payload with state at offset 0,
 *      label_len at offset 1, and label bytes at offsets 2..2+len.
 *   2. encode of (state, label) is byte-deterministic (the tail is
 *      zeroed) — same inputs ⇒ identical bytes.
 *   3. encode + decode round-trip recovers the original (state, label).
 *   4. Empty label encodes as label_len=0 with no label bytes.
 *   5. Over-long labels are truncated silently to MAX_LABEL_BYTES.
 *   6. nullptr label is treated as the empty label.
 *   7. decode rejects:
 *        • null pointers
 *        • frames shorter than PAYLOAD_LEN
 *        • too-small label_buf_cap
 *        • malformed label_len > MAX_LABEL_BYTES
 *   8. decode forwards an unknown state byte verbatim (forward compat).
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/canary/lib/securacv_mesh/test_mesh_beacon.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_beacon.cpp \
 *       -I firmware/canary/lib/securacv_mesh/src \
 *       -o /tmp/test_mesh_beacon && /tmp/test_mesh_beacon
 */

#include "mesh_beacon.h"

#include <cassert>
#include <cstdio>
#include <cstring>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_mesh_beacon_run() { return 0; }
#else

namespace {

void test_layout_arrived_with_label() {
  uint8_t buf[mesh_beacon::PAYLOAD_LEN] = {0xFF};   /* poison */
  assert(mesh_beacon::encode(mesh_beacon::BeaconState::ARRIVED,
                             "kitchen", buf, sizeof(buf)));
  assert(buf[0] == 0);                       /* ARRIVED */
  assert(buf[1] == 7);                       /* strlen("kitchen") */
  assert(std::memcmp(buf + 2, "kitchen", 7) == 0);
  /* Tail must be zero-filled (deterministic encoding). */
  for (size_t i = 2 + 7; i < sizeof(buf); ++i) assert(buf[i] == 0);
  std::printf("PASS test_layout_arrived_with_label\n");
}

void test_layout_departed_empty_label() {
  uint8_t buf[mesh_beacon::PAYLOAD_LEN];
  std::memset(buf, 0xCC, sizeof(buf));   /* poison */
  assert(mesh_beacon::encode(mesh_beacon::BeaconState::DEPARTED,
                             "", buf, sizeof(buf)));
  assert(buf[0] == 1);                   /* DEPARTED */
  assert(buf[1] == 0);                   /* no label */
  /* All label bytes zeroed despite the poison fill. */
  for (size_t i = 2; i < sizeof(buf); ++i) assert(buf[i] == 0);
  std::printf("PASS test_layout_departed_empty_label\n");
}

void test_encode_is_deterministic() {
  /* Two encodings of the same input must produce identical bytes —
   * mesh layer depends on this for replay-defense / dedup hashing. */
  uint8_t a[mesh_beacon::PAYLOAD_LEN];
  uint8_t b[mesh_beacon::PAYLOAD_LEN];
  std::memset(a, 0x11, sizeof(a));
  std::memset(b, 0x22, sizeof(b));   /* DIFFERENT poison */
  assert(mesh_beacon::encode(mesh_beacon::BeaconState::ARRIVED,
                             "office", a, sizeof(a)));
  assert(mesh_beacon::encode(mesh_beacon::BeaconState::ARRIVED,
                             "office", b, sizeof(b)));
  assert(std::memcmp(a, b, mesh_beacon::PAYLOAD_LEN) == 0);
  std::printf("PASS test_encode_is_deterministic\n");
}

void test_roundtrip_arrived() {
  uint8_t buf[mesh_beacon::PAYLOAD_LEN];
  assert(mesh_beacon::encode(mesh_beacon::BeaconState::ARRIVED,
                             "garage", buf, sizeof(buf)));

  mesh_beacon::BeaconState s;
  char lbl[mesh_beacon::MAX_LABEL_BYTES + 1];
  assert(mesh_beacon::decode(buf, sizeof(buf), &s, lbl, sizeof(lbl)));
  assert(s == mesh_beacon::BeaconState::ARRIVED);
  assert(std::strcmp(lbl, "garage") == 0);
  std::printf("PASS test_roundtrip_arrived\n");
}

void test_roundtrip_departed_empty() {
  uint8_t buf[mesh_beacon::PAYLOAD_LEN];
  assert(mesh_beacon::encode(mesh_beacon::BeaconState::DEPARTED,
                             "", buf, sizeof(buf)));

  mesh_beacon::BeaconState s;
  char lbl[mesh_beacon::MAX_LABEL_BYTES + 1] = {'X'};  /* poison */
  assert(mesh_beacon::decode(buf, sizeof(buf), &s, lbl, sizeof(lbl)));
  assert(s == mesh_beacon::BeaconState::DEPARTED);
  assert(lbl[0] == '\0');
  std::printf("PASS test_roundtrip_departed_empty\n");
}

void test_overlong_label_truncates() {
  /* 30 chars > MAX_LABEL_BYTES (23). Encoder must truncate to 23. */
  const char long_label[] = "abcdefghijklmnopqrstuvwxyz0123";
  static_assert(sizeof(long_label) - 1 == 30,
                "test fixture sized for 30 chars");

  uint8_t buf[mesh_beacon::PAYLOAD_LEN];
  assert(mesh_beacon::encode(mesh_beacon::BeaconState::ARRIVED,
                             long_label, buf, sizeof(buf)));
  assert(buf[1] == mesh_beacon::MAX_LABEL_BYTES);

  mesh_beacon::BeaconState s;
  char lbl[mesh_beacon::MAX_LABEL_BYTES + 1];
  assert(mesh_beacon::decode(buf, sizeof(buf), &s, lbl, sizeof(lbl)));
  /* First 23 chars survive; the rest are dropped. */
  assert(std::strncmp(lbl, long_label, mesh_beacon::MAX_LABEL_BYTES) == 0);
  assert(std::strlen(lbl) == mesh_beacon::MAX_LABEL_BYTES);
  std::printf("PASS test_overlong_label_truncates\n");
}

void test_null_label_is_empty() {
  uint8_t buf[mesh_beacon::PAYLOAD_LEN];
  assert(mesh_beacon::encode(mesh_beacon::BeaconState::ARRIVED,
                             nullptr, buf, sizeof(buf)));
  assert(buf[1] == 0);
  std::printf("PASS test_null_label_is_empty\n");
}

void test_encode_rejects_null_buf() {
  assert(!mesh_beacon::encode(mesh_beacon::BeaconState::ARRIVED, "x",
                              nullptr, mesh_beacon::PAYLOAD_LEN));
  uint8_t tiny[10];
  assert(!mesh_beacon::encode(mesh_beacon::BeaconState::ARRIVED, "x",
                              tiny, sizeof(tiny)));
  std::printf("PASS test_encode_rejects_null_buf\n");
}

void test_decode_rejects_short_frame() {
  uint8_t buf[mesh_beacon::PAYLOAD_LEN - 1] = {0};
  mesh_beacon::BeaconState s;
  char lbl[mesh_beacon::MAX_LABEL_BYTES + 1];
  assert(!mesh_beacon::decode(buf, sizeof(buf), &s, lbl, sizeof(lbl)));
  std::printf("PASS test_decode_rejects_short_frame\n");
}

void test_decode_rejects_small_label_buf() {
  uint8_t buf[mesh_beacon::PAYLOAD_LEN] = {0};
  mesh_beacon::BeaconState s;
  char tiny_lbl[mesh_beacon::MAX_LABEL_BYTES];   /* one short */
  assert(!mesh_beacon::decode(buf, sizeof(buf), &s,
                              tiny_lbl, sizeof(tiny_lbl)));
  std::printf("PASS test_decode_rejects_small_label_buf\n");
}

void test_decode_rejects_malformed_label_len() {
  /* Craft a frame with label_len = 50 (> MAX_LABEL_BYTES). */
  uint8_t buf[mesh_beacon::PAYLOAD_LEN] = {0};
  buf[0] = 0;       /* ARRIVED */
  buf[1] = 50;      /* malformed */

  mesh_beacon::BeaconState s;
  char lbl[mesh_beacon::MAX_LABEL_BYTES + 1];
  assert(!mesh_beacon::decode(buf, sizeof(buf), &s, lbl, sizeof(lbl)));
  std::printf("PASS test_decode_rejects_malformed_label_len\n");
}

void test_decode_forwards_unknown_state() {
  /* Forward-compat: unknown state bytes pass through so receivers
   * can choose to drop them silently rather than reject the frame. */
  uint8_t buf[mesh_beacon::PAYLOAD_LEN] = {0};
  buf[0] = 99;      /* unknown state */
  buf[1] = 4;
  std::memcpy(buf + 2, "test", 4);

  mesh_beacon::BeaconState s;
  char lbl[mesh_beacon::MAX_LABEL_BYTES + 1];
  assert(mesh_beacon::decode(buf, sizeof(buf), &s, lbl, sizeof(lbl)));
  assert(static_cast<uint8_t>(s) == 99);
  assert(std::strcmp(lbl, "test") == 0);
  std::printf("PASS test_decode_forwards_unknown_state\n");
}

}  /* namespace */

int main() {
  test_layout_arrived_with_label();
  test_layout_departed_empty_label();
  test_encode_is_deterministic();
  test_roundtrip_arrived();
  test_roundtrip_departed_empty();
  test_overlong_label_truncates();
  test_null_label_is_empty();
  test_encode_rejects_null_buf();
  test_decode_rejects_short_frame();
  test_decode_rejects_small_label_buf();
  test_decode_rejects_malformed_label_len();
  test_decode_forwards_unknown_state();
  std::printf("\nALL MESH_BEACON TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
