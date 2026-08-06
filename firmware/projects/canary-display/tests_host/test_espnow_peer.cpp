// Host test for the pure ESP-NOW peer-decode logic
// (include/canary/net/espnow_peer_logic.h).
//
// Builds standalone with g++ — no Arduino, no esp_now radio. Run in CI by the
// "ESP-NOW peer-decode host test" step in .github/workflows/firmware.yml. Prints
// "ALL ESPNOW TESTS PASSED" on success (the CI grep makes a silent pass
// impossible to fake). Build (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//     firmware/projects/canary-display/tests_host/test_espnow_peer.cpp -o t && ./t

#include "canary/net/espnow_peer_logic.h"

#include <cstdio>
#include <cstring>

using canary::net::espnow_decode;
using canary::fleet::BeaconStatus;

static int g_fail = 0;
#define CHECK(cond, msg)                                             \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                     \
    }                                                              \
  } while (0)

// The same 11-byte fleet-link beacon blob the WAP puts on air and the BLE path
// parses: company FFFF, type 0x10, ver 0x01, then flags/batt/health/LE-chain/fp.
static void make_good(uint8_t out[11], uint8_t flags, uint8_t batt,
                      uint8_t health, uint8_t chain_lo, uint8_t chain_hi,
                      uint8_t fp0, uint8_t fp1) {
  out[0] = 0xFF; out[1] = 0xFF; out[2] = 0x10; out[3] = 0x01;
  out[4] = flags; out[5] = batt; out[6] = health;
  out[7] = chain_lo; out[8] = chain_hi; out[9] = fp0; out[10] = fp1;
}

// A well-formed ESP-NOW payload decodes to the same fp4 + status the BLE path
// would produce — so an ESP-NOW frame and a BLE advert from one canary resolve
// to one witness.
static void test_good_payload_decodes() {
  uint8_t m[11];
  const uint8_t flags = canary::net::BEACON_FLAG_TAMPER |
                        canary::net::BEACON_FLAG_ALERT;
  make_good(m, flags, 55, 88, 0x34, 0x12, 0xAB, 0xCD);
  char fp4[5] = {0};
  BeaconStatus s;
  bool have = false;
  CHECK(espnow_decode(m, 11, fp4, s, have), "good payload: decodes");
  CHECK(std::strcmp(fp4, "abcd") == 0, "good payload: fp4 = abcd");
  CHECK(have, "good payload: status present");
  CHECK(s.tamper, "good payload: tamper flag decoded");
  CHECK(s.battery_present && s.battery_pct == 55, "good payload: battery 55");
  CHECK(s.chain_present && s.chain_seq == 0x1234, "good payload: LE chain 0x1234");
}

// Wrong length / company / type / version must be rejected before the fleet.
static void test_noise_rejected() {
  char fp4[5] = {0};
  BeaconStatus s;
  bool have = false;

  uint8_t m[11];
  make_good(m, 0, 64, 77, 0x00, 0x00, 0x11, 0x22);

  CHECK(!espnow_decode(m, 10, fp4, s, have), "short frame (10) rejected");
  CHECK(!espnow_decode(m, 12, fp4, s, have), "long frame (12) rejected");
  CHECK(!espnow_decode(nullptr, 11, fp4, s, have), "null payload rejected");

  uint8_t wrong_company[11];
  make_good(wrong_company, 0, 64, 77, 0, 0, 0x11, 0x22);
  wrong_company[0] = 0x4C;  // not 0xFFFF
  CHECK(!espnow_decode(wrong_company, 11, fp4, s, have),
        "foreign ESP-NOW traffic (wrong company id) rejected");

  uint8_t wrong_type[11];
  make_good(wrong_type, 0, 64, 77, 0, 0, 0x11, 0x22);
  wrong_type[2] = 0x01;  // chirp type, not the presence beacon 0x10
  CHECK(!espnow_decode(wrong_type, 11, fp4, s, have),
        "wrong beacon type rejected");
}

// A v2 (13-byte) frame decodes with the detection surface — the ESP-NOW twin
// hears the same alert a BLE scan would.
static void test_v2_payload_decodes() {
  uint8_t p[13];
  make_good(p, canary::net::BEACON_FLAG_ALERT, 0xFF, 90, 0x01, 0x00,
            0xAB, 0xCD);
  p[3] = canary::net::BEACON_VERSION_2;
  p[11] = canary::net::BEACON_DETECT_PERSON;
  p[12] = 87;

  char fp4[5] = {0};
  canary::fleet::BeaconStatus s;
  bool have = false;
  CHECK(espnow_decode(p, 13, fp4, s, have), "v2 frame decodes");
  CHECK(have, "v2 frame carries status");
  CHECK(std::strcmp(fp4, "abcd") == 0, "v2 fp4 matches");
  CHECK(s.alert && s.detect_class == canary::net::BEACON_DETECT_PERSON &&
        s.detect_score == 87,
        "v2 detection surface decodes (person 87)");

  p[3] = canary::net::BEACON_VERSION;  // v1 version at v2 length
  CHECK(!espnow_decode(p, 13, fp4, s, have),
        "v1 version at v2 length rejected");
  CHECK(!espnow_decode(p, 12, fp4, s, have), "12-byte frame rejected");
}

int main() {
  test_good_payload_decodes();
  test_noise_rejected();
  test_v2_payload_decodes();

  if (g_fail == 0) {
    std::printf("ALL ESPNOW TESTS PASSED\n");
    return 0;
  }
  std::printf("%d ESPNOW TEST(S) FAILED\n", g_fail);
  return 1;
}
