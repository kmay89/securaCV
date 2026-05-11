/**
 * @file test_mesh_crypto.cpp
 * @brief Host-build conformance test for the mesh_crypto primitives.
 *
 * Verifies:
 *   1. Vendored SHA-256 matches FIPS 180-4 test vectors for "" and "abc".
 *   2. Domain separation works: sha256_domain("a", "bc") == sha256("abc"),
 *      and sha256_domain("", "abc") == sha256("abc").
 *   3. compute_fingerprint truncates SHA-256(DOMAIN_FINGERPRINT || pubkey)
 *      to FINGERPRINT_LEN (8) bytes deterministically.
 *   4. compute_opera_id truncates SHA-256(DOMAIN_OPERA_ID || secret) to
 *      OPERA_ID_LEN (16) bytes deterministically.
 *   5. Domain separation actually separates: fingerprint(zero_input)
 *      differs from opera_id(zero_input).
 *   6. ct_equal returns true for equal inputs, false for any single-bit
 *      difference, false for null inputs.
 *   7. Ed25519 sign/verify shim: roundtrip succeeds; tamper on msg or
 *      pubkey causes verify() to fail.
 *   8. ed25519_generate_keypair produces non-zero pubkey + privkey on
 *      first call, and distinct values across calls.
 *   9. active_backend() returns HOST_TEST_SHIM under CSI_TEST_HOST_BUILD.
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/canary/lib/securacv_mesh/test_mesh_crypto.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_crypto.cpp \
 *       -I firmware/canary/lib/securacv_mesh/src \
 *       -o /tmp/test_mesh_crypto && /tmp/test_mesh_crypto
 */

#include "mesh_crypto.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_mesh_crypto_run() { return 0; }
#else

namespace {

/* FIPS 180-4 SHA-256 test vectors (NIST). */
constexpr uint8_t SHA256_EMPTY[32] = {
  0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
  0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
  0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
  0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
};
constexpr uint8_t SHA256_ABC[32] = {
  0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
  0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
  0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
  0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};

void print_hex(const char* label, const uint8_t* p, size_t n) {
  std::printf("  %s: ", label);
  for (size_t i = 0; i < n; ++i) std::printf("%02x", p[i]);
  std::printf("\n");
}

void test_sha256_empty_vector() {
  uint8_t out[mesh_crypto::SHA256_OUT_LEN];
  mesh_crypto::sha256_domain("", nullptr, 0, out);
  if (std::memcmp(out, SHA256_EMPTY, sizeof(out)) != 0) {
    print_hex("got", out, 32);
    print_hex("expected", SHA256_EMPTY, 32);
    assert(false);
  }
  std::printf("PASS test_sha256_empty_vector\n");
}

void test_sha256_abc_vector() {
  /* sha256_domain("abc", null, 0) ≡ SHA-256("abc") because the domain
   * is prepended to the data. */
  uint8_t out[32];
  mesh_crypto::sha256_domain("abc", nullptr, 0, out);
  if (std::memcmp(out, SHA256_ABC, sizeof(out)) != 0) {
    print_hex("got", out, 32);
    print_hex("expected", SHA256_ABC, 32);
    assert(false);
  }
  std::printf("PASS test_sha256_abc_vector\n");
}

void test_domain_data_concat_equivalence() {
  /* sha256_domain("a", "bc") == sha256_domain("", "abc") == SHA-256("abc"). */
  uint8_t out1[32], out2[32], out3[32];
  const uint8_t bc[] = "bc";
  const uint8_t abc[] = "abc";
  mesh_crypto::sha256_domain("a", bc, 2, out1);
  mesh_crypto::sha256_domain("", abc, 3, out2);
  mesh_crypto::sha256_domain("abc", nullptr, 0, out3);
  assert(std::memcmp(out1, SHA256_ABC, 32) == 0);
  assert(std::memcmp(out2, SHA256_ABC, 32) == 0);
  assert(std::memcmp(out3, SHA256_ABC, 32) == 0);
  std::printf("PASS test_domain_data_concat_equivalence\n");
}

void test_fingerprint_deterministic() {
  uint8_t pub[mesh_crypto::PUBKEY_LEN] = {0};
  for (size_t i = 0; i < mesh_crypto::PUBKEY_LEN; ++i) pub[i] = (uint8_t)i;
  uint8_t fp1[mesh_crypto::FINGERPRINT_LEN];
  uint8_t fp2[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(pub, fp1);
  mesh_crypto::compute_fingerprint(pub, fp2);
  assert(std::memcmp(fp1, fp2, mesh_crypto::FINGERPRINT_LEN) == 0);

  /* Sanity: cross-check against sha256_domain truncation. */
  uint8_t expected_hash[32];
  mesh_crypto::sha256_domain(mesh_crypto::DOMAIN_FINGERPRINT, pub,
                             mesh_crypto::PUBKEY_LEN, expected_hash);
  assert(std::memcmp(fp1, expected_hash, mesh_crypto::FINGERPRINT_LEN) == 0);
  std::printf("PASS test_fingerprint_deterministic\n");
}

void test_opera_id_deterministic() {
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN] = {0};
  for (size_t i = 0; i < mesh_crypto::OPERA_SECRET_LEN; ++i) secret[i] = (uint8_t)(0x40 + i);
  uint8_t id1[mesh_crypto::OPERA_ID_LEN];
  uint8_t id2[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(secret, id1);
  mesh_crypto::compute_opera_id(secret, id2);
  assert(std::memcmp(id1, id2, mesh_crypto::OPERA_ID_LEN) == 0);

  uint8_t expected_hash[32];
  mesh_crypto::sha256_domain(mesh_crypto::DOMAIN_OPERA_ID, secret,
                             mesh_crypto::OPERA_SECRET_LEN, expected_hash);
  assert(std::memcmp(id1, expected_hash, mesh_crypto::OPERA_ID_LEN) == 0);
  std::printf("PASS test_opera_id_deterministic\n");
}

void test_wire_compat_fingerprint_zero_pubkey() {
  /* Pinned regression: the fingerprint of a 32-byte zero pubkey under
   * DOMAIN_FINGERPRINT must match this value, which is the first 8
   * bytes of SHA-256("securacv:pubkey:fingerprint" || 32×0x00).
   *
   * If gemini's wire-compat concern recurs (someone changes either
   * the domain string or the truncation length), this test fails on
   * the spot. Verified by independent computation; matches what
   * canary-wap's compute_fingerprint() produces for the same input. */
  uint8_t zero_pub[mesh_crypto::PUBKEY_LEN] = {0};
  uint8_t fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(zero_pub, fp);

  /* Computed via:
   *   { printf 'securacv:pubkey:fingerprint'; head -c 32 /dev/zero; } \
   *     | openssl dgst -sha256 | awk '{print $2}' | cut -c1-16
   *   = fbee2c22347f2560 */
  static const uint8_t expected[8] = {
    0xfb, 0xee, 0x2c, 0x22, 0x34, 0x7f, 0x25, 0x60
  };
  if (std::memcmp(fp, expected, 8) != 0) {
    print_hex("got fp ", fp, 8);
    print_hex("expected", expected, 8);
    assert(false);
  }
  std::printf("PASS test_wire_compat_fingerprint_zero_pubkey\n");
}

void test_wire_compat_opera_id_zero_secret() {
  /* Pinned regression: opera_id of a 32-byte zero secret under
   * DOMAIN_OPERA_ID ("securacv:opera:id:v0") must match this value.
   * First 16 bytes of SHA-256("securacv:opera:id:v0" || 32×0x00). */
  uint8_t zero_secret[mesh_crypto::OPERA_SECRET_LEN] = {0};
  uint8_t id[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_opera_id(zero_secret, id);

  /* Computed via:
   *   { printf 'securacv:opera:id:v0'; head -c 32 /dev/zero; } \
   *     | openssl dgst -sha256 | awk '{print $2}' | cut -c1-32
   *   = 8ffd53352ea04fe3dcd1a4ed43d0e2be */
  static const uint8_t expected[16] = {
    0x8f, 0xfd, 0x53, 0x35, 0x2e, 0xa0, 0x4f, 0xe3,
    0xdc, 0xd1, 0xa4, 0xed, 0x43, 0xd0, 0xe2, 0xbe
  };
  if (std::memcmp(id, expected, 16) != 0) {
    print_hex("got id  ", id, 16);
    print_hex("expected", expected, 16);
    assert(false);
  }
  std::printf("PASS test_wire_compat_opera_id_zero_secret\n");
}

void test_domain_separation() {
  /* fingerprint(zero_input32) MUST differ from opera_id(zero_input32)
   * even though the input bytes are identical. */
  uint8_t zeros[32] = {0};
  uint8_t fp[mesh_crypto::FINGERPRINT_LEN];
  uint8_t id[mesh_crypto::OPERA_ID_LEN];
  mesh_crypto::compute_fingerprint(zeros, fp);
  mesh_crypto::compute_opera_id(zeros, id);
  assert(std::memcmp(fp, id, mesh_crypto::FINGERPRINT_LEN) != 0);
  std::printf("PASS test_domain_separation\n");
}

void test_ct_equal() {
  uint8_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  assert(mesh_crypto::ct_equal(a, b, 8));

  /* Single-bit difference. */
  b[3] ^= 0x01;
  assert(!mesh_crypto::ct_equal(a, b, 8));

  /* Null safety. */
  assert(!mesh_crypto::ct_equal(nullptr, b, 8));
  assert(!mesh_crypto::ct_equal(a, nullptr, 8));
  std::printf("PASS test_ct_equal\n");
}

void test_ed25519_sign_verify_roundtrip() {
  uint8_t pub[mesh_crypto::PUBKEY_LEN];
  uint8_t priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));

  const uint8_t msg[] = "the quick brown fox jumps over the lazy dog";
  uint8_t sig[mesh_crypto::SIGNATURE_LEN];
  assert(mesh_crypto::ed25519_sign(priv, pub, msg, sizeof(msg) - 1, sig));
  assert(mesh_crypto::ed25519_verify(pub, msg, sizeof(msg) - 1, sig));
  std::printf("PASS test_ed25519_sign_verify_roundtrip\n");
}

void test_ed25519_verify_rejects_tampered_message() {
  uint8_t pub[mesh_crypto::PUBKEY_LEN], priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));

  uint8_t msg[16];
  for (size_t i = 0; i < sizeof(msg); ++i) msg[i] = (uint8_t)i;
  uint8_t sig[mesh_crypto::SIGNATURE_LEN];
  assert(mesh_crypto::ed25519_sign(priv, pub, msg, sizeof(msg), sig));

  /* Flip a bit in the message — verify must reject. */
  msg[7] ^= 0x80;
  assert(!mesh_crypto::ed25519_verify(pub, msg, sizeof(msg), sig));
  std::printf("PASS test_ed25519_verify_rejects_tampered_message\n");
}

void test_ed25519_verify_rejects_wrong_pubkey() {
  uint8_t pub_a[32], priv_a[32], pub_b[32], priv_b[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub_a, priv_a));
  assert(mesh_crypto::ed25519_generate_keypair(pub_b, priv_b));
  /* Distinct keypairs from rand() — vanishingly small chance of collision. */
  assert(std::memcmp(pub_a, pub_b, 32) != 0);

  const uint8_t msg[] = "hello";
  uint8_t sig[mesh_crypto::SIGNATURE_LEN];
  assert(mesh_crypto::ed25519_sign(priv_a, pub_a, msg, sizeof(msg) - 1, sig));
  /* Signed by A's key, verify under B's pubkey — must reject. */
  assert(!mesh_crypto::ed25519_verify(pub_b, msg, sizeof(msg) - 1, sig));
  std::printf("PASS test_ed25519_verify_rejects_wrong_pubkey\n");
}

void test_active_backend_is_host_shim() {
  assert(mesh_crypto::active_backend() == mesh_crypto::Backend::HOST_TEST_SHIM);
  std::printf("PASS test_active_backend_is_host_shim\n");
}

}  /* namespace */

int main() {
  std::srand(0xC51);  /* deterministic — see ed25519_generate_keypair host shim */
  test_sha256_empty_vector();
  test_sha256_abc_vector();
  test_domain_data_concat_equivalence();
  test_fingerprint_deterministic();
  test_opera_id_deterministic();
  test_wire_compat_fingerprint_zero_pubkey();
  test_wire_compat_opera_id_zero_secret();
  test_domain_separation();
  test_ct_equal();
  test_ed25519_sign_verify_roundtrip();
  test_ed25519_verify_rejects_tampered_message();
  test_ed25519_verify_rejects_wrong_pubkey();
  test_active_backend_is_host_shim();
  std::printf("\nALL MESH_CRYPTO TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
