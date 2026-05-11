/**
 * @file test_mesh_pairing.cpp
 * @brief Host-build conformance test for mesh_pairing primitives.
 *
 * Verifies:
 *   1. compute_confirmation_code is deterministic.
 *   2. Distinct session keys produce distinct codes.
 *   3. Code is always in [0, 999_999].
 *   4. Wire-compat regression: session_key = 32×0x00 produces code
 *      884555 (independently computed via openssl).
 *   5. Symmetric mutual-DH pairing: two simulated peers run the host
 *      X25519 shim, derive the same session_key, and therefore compute
 *      the same confirmation code — the only property the user
 *      visually verifies.
 *   6. Wire-format struct sizes match the static_asserts in the header
 *      (a runtime check duplicating the compile-time assert so a CI
 *      log surface flags this loudly if the header gets edited).
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/canary/lib/securacv_mesh/test_mesh_pairing.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_pairing.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_crypto.cpp \
 *       -I firmware/canary/lib/securacv_mesh/src \
 *       -o /tmp/test_mesh_pairing && /tmp/test_mesh_pairing
 */

#include "mesh_pairing.h"
#include "mesh_crypto.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_mesh_pairing_run() { return 0; }
#else

namespace {

void test_code_deterministic() {
  uint8_t session_key[mesh_pairing::SESSION_KEY_LEN];
  for (size_t i = 0; i < sizeof(session_key); ++i) session_key[i] = (uint8_t)(i * 7);
  uint32_t a = mesh_pairing::compute_confirmation_code(session_key);
  uint32_t b = mesh_pairing::compute_confirmation_code(session_key);
  assert(a == b);
  assert(a < mesh_pairing::CONFIRMATION_CODE_MODULUS);
  std::printf("PASS test_code_deterministic  (code=%06u)\n", a);
}

void test_code_distinct_inputs_distinct_outputs() {
  uint8_t k1[mesh_pairing::SESSION_KEY_LEN] = {0};
  uint8_t k2[mesh_pairing::SESSION_KEY_LEN] = {0};
  k2[0] = 1;   /* differ in one bit */
  uint32_t c1 = mesh_pairing::compute_confirmation_code(k1);
  uint32_t c2 = mesh_pairing::compute_confirmation_code(k2);
  assert(c1 != c2);  /* 1-in-10^6 chance of accidental collision */
  std::printf("PASS test_code_distinct_inputs_distinct_outputs  (c1=%06u c2=%06u)\n", c1, c2);
}

void test_code_range() {
  /* Sweep 256 different keys and assert every output stays in the
   * declared range. Cheap regression for off-by-one modulus bugs. */
  for (int seed = 0; seed < 256; ++seed) {
    uint8_t k[mesh_pairing::SESSION_KEY_LEN];
    for (size_t i = 0; i < sizeof(k); ++i) k[i] = (uint8_t)((seed * 13 + i) & 0xFF);
    uint32_t c = mesh_pairing::compute_confirmation_code(k);
    assert(c < mesh_pairing::CONFIRMATION_CODE_MODULUS);
  }
  std::printf("PASS test_code_range  (256 seeds, all in [0, 999999])\n");
}

void test_wire_compat_code_zero_session_key() {
  /* Pinned regression: a 32-byte zero session key produces code 884555.
   * Independently computed:
   *   $ { printf 'securacv:pair:confirm:v0'; head -c 32 /dev/zero; } \
   *       | openssl dgst -sha256
   *   = 3b460b... (first 3 bytes: 0x3b, 0x46, 0x0b)
   *   top24 = 0x3b460b = 3884555
   *   3884555 %% 1000000 = 884555
   *
   * If this fails the domain string or modulus has drifted from
   * canary-wap — paired canary + canary-wap nodes would display
   * different 6-digit codes and pairing would fail user verification. */
  uint8_t zero_key[mesh_pairing::SESSION_KEY_LEN] = {0};
  uint32_t code = mesh_pairing::compute_confirmation_code(zero_key);
  if (code != 884555u) {
    std::printf("  got code=%06u, expected 884555\n", code);
    assert(false);
  }
  std::printf("PASS test_wire_compat_code_zero_session_key  (code=%06u)\n", code);
}

void test_symmetric_mutual_dh_produces_same_code() {
  /* End-to-end check: simulate two peers, each derives the same
   * session_key via x25519, each computes the same confirmation code.
   * This is the property the pairing UI relies on for the user to
   * visually verify both screens display the same 6 digits. */
  uint8_t pub_a[mesh_crypto::PUBKEY_LEN], priv_a[mesh_crypto::PRIVKEY_LEN];
  uint8_t pub_b[mesh_crypto::PUBKEY_LEN], priv_b[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(pub_a, priv_a));
  assert(mesh_crypto::ed25519_generate_keypair(pub_b, priv_b));

  uint8_t session_a[mesh_pairing::SESSION_KEY_LEN];
  uint8_t session_b[mesh_pairing::SESSION_KEY_LEN];
  assert(mesh_crypto::x25519_derive(priv_a, pub_b, session_a));
  assert(mesh_crypto::x25519_derive(priv_b, pub_a, session_b));
  assert(std::memcmp(session_a, session_b, mesh_pairing::SESSION_KEY_LEN) == 0);

  uint32_t code_a = mesh_pairing::compute_confirmation_code(session_a);
  uint32_t code_b = mesh_pairing::compute_confirmation_code(session_b);
  assert(code_a == code_b);
  std::printf("PASS test_symmetric_mutual_dh_produces_same_code  (code=%06u)\n", code_a);
}

void test_wire_format_struct_sizes() {
  /* Mirror the static_asserts in mesh_pairing.h at runtime so a CI
   * log surface flags the drift loudly instead of just a compile fail. */
  assert(sizeof(mesh_pairing::PairDiscoverPayload) ==
         mesh_crypto::PUBKEY_LEN + (mesh_pairing::MAX_PEER_NAME_LEN + 1) + 1);
  assert(sizeof(mesh_pairing::PairOfferPayload) ==
         mesh_crypto::PUBKEY_LEN * 2 + (mesh_pairing::MAX_OPERA_NAME_LEN + 1) + 1);
  assert(sizeof(mesh_pairing::PairAcceptPayload) ==
         sizeof(mesh_pairing::PairOfferPayload));   /* aliased per canary-wap */
  assert(sizeof(mesh_pairing::PairConfirmPayload) == mesh_crypto::SHA256_OUT_LEN);
  assert(sizeof(mesh_pairing::PairCompletePayload) ==
         (mesh_crypto::OPERA_SECRET_LEN + mesh_crypto::AEAD_TAG_LEN) +
          mesh_crypto::AEAD_NONCE_LEN);
  std::printf("PASS test_wire_format_struct_sizes  (Discover=%zu Offer=%zu Confirm=%zu Complete=%zu)\n",
              sizeof(mesh_pairing::PairDiscoverPayload),
              sizeof(mesh_pairing::PairOfferPayload),
              sizeof(mesh_pairing::PairConfirmPayload),
              sizeof(mesh_pairing::PairCompletePayload));
}

}  /* namespace */

int main() {
  std::srand(0xC5101);
  test_code_deterministic();
  test_code_distinct_inputs_distinct_outputs();
  test_code_range();
  test_wire_compat_code_zero_session_key();
  test_symmetric_mutual_dh_produces_same_code();
  test_wire_format_struct_sizes();
  std::printf("\nALL MESH_PAIRING TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
