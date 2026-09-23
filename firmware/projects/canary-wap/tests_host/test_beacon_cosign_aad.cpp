// Host test for beacon_cosign_aad.h — the associated data the Beacon COSIGN
// envelope binds into its ChaCha20-Poly1305 tag (spec/beacon_channel_v0.md
// §6.3), and the all-zero X25519 shared-secret refusal.
//
// The AEAD and X25519 calls themselves live in beacon_channel.cpp against the
// Arduino Crypto library, which is not vendored here; what this pins is the
// part both ends must agree on byte for byte — which clear fields are
// authenticated, in which order, in which byte order — and that every field
// an attacker could flip in flight changes those bytes.
/* Build & run (the canary-wap tests_host Makefile's `run` target does this):
 *
 *   g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_beacon_cosign_aad.cpp \
 *       -o /tmp/test_beacon_cosign_aad && /tmp/test_beacon_cosign_aad
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "beacon_wire.h"
#include "beacon_cosign_aad.h"

using namespace beacon_channel;
using namespace beacon_cosign_aad;

namespace {

int failures = 0;
int checks = 0;
#define EXPECT(cond, msg) do { \
    checks++; \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
  } while (0)

void test_req_layout() {
  uint8_t a[DEVICE_FP_SIZE]; std::memset(a, 0xA1, sizeof(a));
  uint8_t b[DEVICE_FP_SIZE]; std::memset(b, 0xB2, sizeof(b));
  uint8_t out[REQ_AAD_LEN];
  cosign_req_aad(out, a, b, 0x0148);
  EXPECT(REQ_AAD_LEN == 35, "REQ associated data is 35 bytes");
  EXPECT(out[0] == BEACON_MSG_COSIGN_REQ && out[0] == 7, "leads with msg_type COSIGN_REQ");
  EXPECT(std::memcmp(out + 1, a, DEVICE_FP_SIZE) == 0, "then the originator fingerprint");
  EXPECT(std::memcmp(out + 17, b, DEVICE_FP_SIZE) == 0, "then the candidate cosigner fingerprint");
  EXPECT(out[33] == 0x48 && out[34] == 0x01, "then ciphertext_len, little-endian");
}

void test_resp_layout() {
  uint8_t a[DEVICE_FP_SIZE]; std::memset(a, 0xA1, sizeof(a));
  uint8_t b[DEVICE_FP_SIZE]; std::memset(b, 0xB2, sizeof(b));
  uint8_t out[RESP_AAD_LEN];
  cosign_resp_aad(out, a, b, 1);
  EXPECT(RESP_AAD_LEN == 34, "RESP associated data is 34 bytes");
  EXPECT(out[0] == BEACON_MSG_COSIGN_RESP && out[0] == 8, "leads with msg_type COSIGN_RESP");
  EXPECT(std::memcmp(out + 1, a, DEVICE_FP_SIZE) == 0, "then the originator fingerprint");
  EXPECT(std::memcmp(out + 17, b, DEVICE_FP_SIZE) == 0, "then the cosigner fingerprint");
  EXPECT(out[33] == 1, "then the accept byte");
}

// Every clear field an attacker could rewrite in flight must change the
// associated data — otherwise the tag would not notice the rewrite.
void test_every_clear_field_is_bound() {
  uint8_t a[DEVICE_FP_SIZE]; std::memset(a, 0xA1, sizeof(a));
  uint8_t b[DEVICE_FP_SIZE]; std::memset(b, 0xB2, sizeof(b));

  uint8_t accept1[RESP_AAD_LEN], accept0[RESP_AAD_LEN];
  cosign_resp_aad(accept1, a, b, 1);
  cosign_resp_aad(accept0, a, b, 0);
  EXPECT(std::memcmp(accept1, accept0, RESP_AAD_LEN) != 0,
         "flipping accept 1 -> 0 changes the RESP associated data (no silent discard of a valid cosign)");

  uint8_t swapped[RESP_AAD_LEN];
  cosign_resp_aad(swapped, b, a, 1);
  EXPECT(std::memcmp(accept1, swapped, RESP_AAD_LEN) != 0,
         "swapping the RESP fingerprints changes its associated data");

  uint8_t len72[REQ_AAD_LEN], len73[REQ_AAD_LEN], other_fp[REQ_AAD_LEN];
  cosign_req_aad(len72, a, b, 72);
  cosign_req_aad(len73, a, b, 73);
  uint8_t c[DEVICE_FP_SIZE]; std::memset(c, 0xC3, sizeof(c));
  cosign_req_aad(other_fp, a, c, 72);
  EXPECT(std::memcmp(len72, len73, REQ_AAD_LEN) != 0,
         "a rewritten ciphertext_len changes the REQ associated data");
  EXPECT(std::memcmp(len72, other_fp, REQ_AAD_LEN) != 0,
         "a rewritten candidate fingerprint changes the REQ associated data");

  // The two layouts differ in length and in their leading byte, so a REQ's
  // associated data can never equal a RESP's.
  EXPECT(REQ_AAD_LEN != RESP_AAD_LEN && len72[0] != accept1[0],
         "REQ and RESP associated data are domain-separated");
}

void test_zero_shared_secret_refused() {
  uint8_t s[32] = {};
  EXPECT(shared_secret_is_zero(s), "an all-zero X25519 secret is detected");
  for (size_t i = 0; i < 32; i++) {
    uint8_t t[32] = {};
    t[i] = 0x01;
    EXPECT(!shared_secret_is_zero(t), "any non-zero byte means a real secret");
  }
  uint8_t full[32];
  std::memset(full, 0xFF, sizeof(full));
  EXPECT(!shared_secret_is_zero(full), "an all-0xFF secret is not zero");
}

}  // namespace

int main() {
  test_req_layout();
  test_resp_layout();
  test_every_clear_field_is_bound();
  test_zero_shared_secret_refused();
  if (failures == 0) {
    std::printf("ALL %d beacon cosign AAD checks PASSED\n", checks);
    return 0;
  }
  std::printf("%d of %d beacon cosign AAD checks FAILED\n", failures, checks);
  return 1;
}
