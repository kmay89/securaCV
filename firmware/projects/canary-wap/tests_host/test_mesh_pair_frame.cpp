// Host test for canary-wap's Opera pairing-frame classifier
// (arduino/canary_wap/mesh_pair_frame.h, F14).
//
// Pins the fix for the bug the classifier exists for: the WAP used to send
// the five pairing structs raw and then drop every frame shorter than the
// 102-byte signed-header minimum on receive, so pairing never completed.
// Now a pairing frame is [MessageType 8..12][exact payload] and is
// recognized before that gate. Verifies:
//   1. build() + classify() round-trip every pairing type at its exact size,
//      and every pairing frame is shorter than the 102-byte signed minimum
//      (the reason it must be classified first).
//   2. Off-by-one lengths (short and long) are rejected, as is a bare
//      prefix, a null pointer, and every non-pairing first byte.
//   3. A signed Opera frame (PROTOCOL_VERSION = 0 at byte 0), a Chirp frame
//      (0xC4) and a Beacon frame (0xB1) are never classified as pairing.
//   4. A signed-length frame whose SECOND byte is a pairing type (the old
//      in-header pairing path) is not a pairing frame.
//   5. build() refuses a wrong payload length or a non-pairing type, so the
//      sender can only emit what the receiver accepts; classify() leaves its
//      out-parameters untouched on false.
//
// Build: see the PAIR_FRAME block at the end of the Makefile.

#include "mesh_pair_frame.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

namespace pf = mesh_pair_frame;

static const size_t SIGNED_MIN = 2 + 16 + 8 + 8 + 4 + 64;  // = 102

static void test_roundtrip_every_type() {
  const uint8_t types[] = {pf::TYPE_DISCOVER, pf::TYPE_OFFER, pf::TYPE_ACCEPT,
                           pf::TYPE_CONFIRM, pf::TYPE_COMPLETE};
  const size_t lens[] = {58, 98, 98, 32, 60};
  for (size_t i = 0; i < 5; ++i) {
    CHECK(pf::payload_len_for(types[i]) == lens[i]);
    uint8_t payload[pf::OFFER_LEN];
    for (size_t b = 0; b < sizeof(payload); ++b) payload[b] = (uint8_t)(0x40 + b);
    uint8_t frame[pf::MAX_FRAME_LEN] = {0};
    const size_t n = pf::build(types[i], payload, lens[i], frame, sizeof(frame));
    CHECK(n == 1 + lens[i]);
    CHECK(n < SIGNED_MIN);  // why it must be classified before the 102-byte gate
    CHECK(frame[0] == types[i]);

    uint8_t t = 0;
    const uint8_t* p = nullptr;
    size_t pl = 0;
    CHECK(pf::classify(frame, n, &t, &p, &pl));
    CHECK(t == types[i]);
    CHECK(p == frame + 1);
    CHECK(pl == lens[i]);
    CHECK(std::memcmp(p, payload, lens[i]) == 0);
  }
  std::printf("PASS test_roundtrip_every_type\n");
}

static void test_off_by_one_rejected() {
  uint8_t frame[pf::MAX_FRAME_LEN + 2];
  std::memset(frame, 0x11, sizeof(frame));
  for (uint8_t type = pf::TYPE_DISCOVER; type <= pf::TYPE_COMPLETE; ++type) {
    frame[0] = type;
    const size_t exact = 1 + pf::payload_len_for(type);
    uint8_t t = 0;
    const uint8_t* p = nullptr;
    size_t pl = 0;
    CHECK(pf::classify(frame, exact, &t, &p, &pl));
    CHECK(!pf::classify(frame, exact - 1, &t, &p, &pl));
    CHECK(!pf::classify(frame, exact + 1, &t, &p, &pl));
    CHECK(!pf::classify(frame, 1, &t, &p, &pl));   // bare prefix
  }
  uint8_t t = 0;
  const uint8_t* p = nullptr;
  size_t pl = 0;
  CHECK(!pf::classify(frame, 0, &t, &p, &pl));
  CHECK(!pf::classify(nullptr, 59, &t, &p, &pl));
  frame[0] = pf::TYPE_DISCOVER;
  CHECK(!pf::classify(frame, 59, nullptr, &p, &pl));
  CHECK(!pf::classify(frame, 59, &t, nullptr, &pl));
  CHECK(!pf::classify(frame, 59, &t, &p, nullptr));
  std::printf("PASS test_off_by_one_rejected\n");
}

static void test_every_other_first_byte_rejected() {
  // Any first byte outside 8..12 is not a pairing frame, at ANY length that
  // would be valid for a pairing type.
  uint8_t frame[256];
  std::memset(frame, 0, sizeof(frame));
  const size_t lens[] = {59, 99, 33, 61};
  for (int b = 0; b < 256; ++b) {
    if (b >= pf::TYPE_DISCOVER && b <= pf::TYPE_COMPLETE) continue;
    frame[0] = (uint8_t)b;
    for (size_t len : lens) {
      uint8_t t = 0;
      const uint8_t* p = nullptr;
      size_t pl = 0;
      CHECK(!pf::classify(frame, len, &t, &p, &pl));
    }
  }
  std::printf("PASS test_every_other_first_byte_rejected\n");
}

static void test_other_protocols_not_classified() {
  uint8_t t = 0;
  const uint8_t* p = nullptr;
  size_t pl = 0;

  // A signed Opera frame: version byte 0 (mesh_network::PROTOCOL_VERSION),
  // then msg_type — here a pairing type, i.e. the OLD in-header pairing
  // shape. It is signed-length and starts with 0: never a pairing frame.
  uint8_t opera[SIGNED_MIN + 32];
  std::memset(opera, 0xAB, sizeof(opera));
  opera[0] = 0;                  // PROTOCOL_VERSION (Opera)
  opera[1] = pf::TYPE_OFFER;     // msg_type
  CHECK(!pf::classify(opera, sizeof(opera), &t, &p, &pl));
  CHECK(!pf::classify(opera, SIGNED_MIN, &t, &p, &pl));

  // Chirp (CHIRP_MAGIC) and Beacon (BEACON_MAGIC) frames.
  uint8_t chirp[99];
  std::memset(chirp, 0, sizeof(chirp));
  chirp[0] = 0xC4;
  CHECK(!pf::classify(chirp, sizeof(chirp), &t, &p, &pl));
  uint8_t beacon[99];
  std::memset(beacon, 0, sizeof(beacon));
  beacon[0] = 0xB1;
  CHECK(!pf::classify(beacon, sizeof(beacon), &t, &p, &pl));
  std::printf("PASS test_other_protocols_not_classified\n");
}

static void test_build_refuses_what_classify_would() {
  uint8_t payload[pf::OFFER_LEN] = {0};
  uint8_t frame[pf::MAX_FRAME_LEN];
  CHECK(pf::build(pf::TYPE_OFFER, payload, pf::OFFER_LEN - 1, frame, sizeof(frame)) == 0);
  CHECK(pf::build(pf::TYPE_CONFIRM, payload, pf::OFFER_LEN, frame, sizeof(frame)) == 0);
  CHECK(pf::build(7, payload, 32, frame, sizeof(frame)) == 0);     // MSG_PEER_LIST
  CHECK(pf::build(13, payload, 32, frame, sizeof(frame)) == 0);    // MSG_LEAVE_OPERA
  CHECK(pf::build(pf::TYPE_OFFER, payload, pf::OFFER_LEN, frame, pf::OFFER_LEN) == 0);
  CHECK(pf::build(pf::TYPE_OFFER, nullptr, pf::OFFER_LEN, frame, sizeof(frame)) == 0);
  CHECK(pf::build(pf::TYPE_OFFER, payload, pf::OFFER_LEN, nullptr, sizeof(frame)) == 0);

  // classify() leaves the out-parameters alone on false.
  uint8_t t = 0x5A;
  const uint8_t* p = frame;
  size_t pl = 777;
  frame[0] = 0x00;
  CHECK(!pf::classify(frame, 59, &t, &p, &pl));
  CHECK(t == 0x5A && p == frame && pl == 777);
  std::printf("PASS test_build_refuses_what_classify_would\n");
}

int main() {
  test_roundtrip_every_type();
  test_off_by_one_rejected();
  test_every_other_first_byte_rejected();
  test_other_protocols_not_classified();
  test_build_refuses_what_classify_would();
  if (g_failures != 0) {
    std::fprintf(stderr, "\n%d MESH_PAIR_FRAME CHECK(S) FAILED\n", g_failures);
    return 1;
  }
  std::printf("\nALL MESH_PAIR_FRAME TESTS PASSED\n");
  return 0;
}
