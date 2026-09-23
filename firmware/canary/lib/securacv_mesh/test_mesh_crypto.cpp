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
 *  10. X25519 is REAL on the host (F33 part 2): the RFC 7748 §5.2 and §6.1
 *      vectors hold through x25519_derive / the keypair generator, two
 *      generated keypairs agree, and Ed25519-generated keys do NOT — the
 *      exact bug pairing shipped with, which the old host shim hid.
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

/* ── X25519 ECDH ───────────────────────────────────────────────────────── */

void from_hex(const char* hex, uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    unsigned v = 0;
    assert(std::sscanf(hex + 2 * i, "%2x", &v) == 1);
    out[i] = (uint8_t)v;
  }
}

/* RFC 7748 §5 clamping — what x25519_generate_keypair applies. The
 * RFC's X25519() clamps inside; x25519_derive (like rweather's
 * Curve25519::eval on the device) takes the scalar as given. */
void clamp(uint8_t k[32]) {
  k[0]  &= 0xF8;
  k[31]  = (uint8_t)((k[31] & 0x7F) | 0x40);
}

void test_x25519_rfc7748_vectors() {
  /* §5.2, the two single-shot vectors. The second u has bit 255 set:
   * it must be masked, not reduced into a different point. */
  struct { const char* k; const char* u; const char* out; } v[] = {
    {"a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
     "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
     "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"},
    {"4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
     "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
     "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"},
  };
  for (const auto& t : v) {
    uint8_t k[32], u[32], want[32], got[32];
    from_hex(t.k, k, 32); from_hex(t.u, u, 32); from_hex(t.out, want, 32);
    clamp(k);
    assert(mesh_crypto::x25519_derive(k, u, got));
    assert(std::memcmp(got, want, 32) == 0);
  }

  /* §5.2, the iterated vector: k = u = 9; k' = X25519(k, u), u = old k.
   * After 1 iteration and after 1000. */
  uint8_t k[32] = {9}, u[32] = {9}, next[32];
  uint8_t want1[32], want1000[32];
  from_hex("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079", want1, 32);
  from_hex("684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", want1000, 32);
  for (int i = 1; i <= 1000; ++i) {
    uint8_t kc[32];
    std::memcpy(kc, k, 32);
    clamp(kc);
    assert(mesh_crypto::x25519_derive(kc, u, next));
    std::memcpy(u, k, 32);
    std::memcpy(k, next, 32);
    if (i == 1) assert(std::memcmp(k, want1, 32) == 0);
  }
  assert(std::memcmp(k, want1000, 32) == 0);

  /* §6.1 — Alice and Bob: each public key is priv * 9, and both sides
   * derive the same K. */
  uint8_t a[32], ap[32], b[32], bp[32], shared[32];
  from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, 32);
  from_hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", ap, 32);
  from_hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b, 32);
  from_hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", bp, 32);
  from_hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", shared, 32);
  clamp(a); clamp(b);
  const uint8_t nine[32] = {9};
  uint8_t out[32];
  assert(mesh_crypto::x25519_derive(a, nine, out) && std::memcmp(out, ap, 32) == 0);
  assert(mesh_crypto::x25519_derive(b, nine, out) && std::memcmp(out, bp, 32) == 0);
  assert(mesh_crypto::x25519_derive(a, bp, out) && std::memcmp(out, shared, 32) == 0);
  assert(mesh_crypto::x25519_derive(b, ap, out) && std::memcmp(out, shared, 32) == 0);

  /* A non-canonical u (p itself, once bit 255 is masked) is refused, as
   * rweather's eval() refuses it. */
  uint8_t p[32];
  std::memset(p, 0xFF, 32);
  p[0] = 0xED; p[31] = 0x7F;
  assert(!mesh_crypto::x25519_derive(a, p, out));
  std::printf("PASS test_x25519_rfc7748_vectors  (5.2 x2, 5.2 iterated x1000, 6.1)\n");
}

void test_x25519_mutual_dh_symmetric() {
  /* The core DH property: both peers compute the same shared secret
   * from (own_priv, peer_pub) — with REAL X25519 keypairs. The host
   * X25519 is the RFC 7748 ladder, not a shim, so this holds for the
   * reason it holds on a device. */
  for (int round = 0; round < 8; ++round) {
    uint8_t priv_a[mesh_crypto::PRIVKEY_LEN], pub_a[mesh_crypto::PUBKEY_LEN];
    uint8_t priv_b[mesh_crypto::PRIVKEY_LEN], pub_b[mesh_crypto::PUBKEY_LEN];
    assert(mesh_crypto::x25519_generate_keypair(pub_a, priv_a));
    assert(mesh_crypto::x25519_generate_keypair(pub_b, priv_b));
    assert(std::memcmp(pub_a, pub_b, mesh_crypto::PUBKEY_LEN) != 0);

    uint8_t shared_ab[mesh_crypto::X25519_SHARED_LEN];
    uint8_t shared_ba[mesh_crypto::X25519_SHARED_LEN];
    assert(mesh_crypto::x25519_derive(priv_a, pub_b, shared_ab));
    assert(mesh_crypto::x25519_derive(priv_b, pub_a, shared_ba));
    assert(std::memcmp(shared_ab, shared_ba, mesh_crypto::X25519_SHARED_LEN) == 0);
  }
  std::printf("PASS test_x25519_mutual_dh_symmetric\n");
}

void test_x25519_over_ed25519_keys_disagrees() {
  /* F33 part 2, the bug itself: X25519 over Ed25519-generated keys. On a
   * device an Ed25519 pub is an Edwards point from SHA-512(seed), not
   * seed * 9, so the two sides land on different values; on the host the
   * Ed25519 pub is a hash of the seed, likewise not seed * 9. The old
   * host X25519 shim agreed anyway — which is how pairing shipped this. */
  uint8_t priv_a[32], pub_a[32], priv_b[32], pub_b[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub_a, priv_a));
  assert(mesh_crypto::ed25519_generate_keypair(pub_b, priv_b));
  uint8_t s1[32], s2[32];
  const bool ok1 = mesh_crypto::x25519_derive(priv_a, pub_b, s1);
  const bool ok2 = mesh_crypto::x25519_derive(priv_b, pub_a, s2);
  assert(!(ok1 && ok2 && std::memcmp(s1, s2, 32) == 0));
  std::printf("PASS test_x25519_over_ed25519_keys_disagrees\n");
}

void test_x25519_rejects_zero_pubkey() {
  /* The all-zero point is the identity element. Real Curve25519
   * either rejects it or yields a zero shared, which is a known
   * weak key — both paths in mesh_crypto refuse it. */
  uint8_t priv[mesh_crypto::PRIVKEY_LEN], pub[mesh_crypto::PUBKEY_LEN];
  assert(mesh_crypto::x25519_generate_keypair(pub, priv));
  uint8_t zero_pub[mesh_crypto::PUBKEY_LEN] = {0};
  uint8_t shared[mesh_crypto::X25519_SHARED_LEN];
  assert(!mesh_crypto::x25519_derive(priv, zero_pub, shared));
  /* On failure shared MUST be zeroed. */
  for (size_t i = 0; i < mesh_crypto::X25519_SHARED_LEN; ++i) assert(shared[i] == 0);
  std::printf("PASS test_x25519_rejects_zero_pubkey\n");
}

/* ── ChaCha20-Poly1305 AEAD ────────────────────────────────────────────── */

void test_aead_encrypt_decrypt_roundtrip() {
  uint8_t key[mesh_crypto::AEAD_KEY_LEN];
  uint8_t nonce[mesh_crypto::AEAD_NONCE_LEN];
  for (size_t i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)i;
  mesh_crypto::aead_generate_nonce(nonce);

  const uint8_t aad[] = "associated-data";
  const uint8_t pt[] = "hello world, this is a chacha20-poly1305 test message";
  const size_t pt_len = sizeof(pt) - 1;
  const size_t aad_len = sizeof(aad) - 1;

  uint8_t ct[pt_len];
  uint8_t tag[mesh_crypto::AEAD_TAG_LEN];
  assert(mesh_crypto::aead_encrypt(key, nonce, aad, aad_len, pt, pt_len, ct, tag));
  /* Ciphertext should differ from plaintext for nontrivial inputs. */
  assert(std::memcmp(ct, pt, pt_len) != 0);

  uint8_t recovered[pt_len];
  assert(mesh_crypto::aead_decrypt(key, nonce, aad, aad_len, ct, pt_len, tag, recovered));
  assert(std::memcmp(recovered, pt, pt_len) == 0);
  std::printf("PASS test_aead_encrypt_decrypt_roundtrip\n");
}

void test_aead_rejects_tampered_ciphertext() {
  uint8_t key[32], nonce[12];
  for (size_t i = 0; i < 32; ++i) key[i] = (uint8_t)(i * 3);
  mesh_crypto::aead_generate_nonce(nonce);
  const uint8_t pt[] = "secret";
  uint8_t ct[sizeof(pt) - 1], tag[16];
  assert(mesh_crypto::aead_encrypt(key, nonce, nullptr, 0, pt, sizeof(pt) - 1, ct, tag));
  /* Flip one bit. */
  ct[2] ^= 0x04;
  uint8_t recovered[sizeof(pt) - 1];
  assert(!mesh_crypto::aead_decrypt(key, nonce, nullptr, 0, ct, sizeof(pt) - 1, tag, recovered));
  /* On failure recovered must be zeroed — no forged plaintext leaks. */
  for (size_t i = 0; i < sizeof(recovered); ++i) assert(recovered[i] == 0);
  std::printf("PASS test_aead_rejects_tampered_ciphertext\n");
}

void test_aead_rejects_tampered_aad() {
  uint8_t key[32], nonce[12];
  for (size_t i = 0; i < 32; ++i) key[i] = 0xA0 ^ (uint8_t)i;
  mesh_crypto::aead_generate_nonce(nonce);
  const uint8_t aad_a[] = "header-A";
  const uint8_t aad_b[] = "header-B";   /* same length, different content */
  const uint8_t pt[] = "payload";
  uint8_t ct[sizeof(pt) - 1], tag[16];
  assert(mesh_crypto::aead_encrypt(key, nonce, aad_a, sizeof(aad_a) - 1,
                                   pt, sizeof(pt) - 1, ct, tag));
  uint8_t recovered[sizeof(pt) - 1];
  /* Decrypt under a DIFFERENT AAD — must fail even though ciphertext + tag
   * are bit-perfect. */
  assert(!mesh_crypto::aead_decrypt(key, nonce, aad_b, sizeof(aad_b) - 1,
                                    ct, sizeof(pt) - 1, tag, recovered));
  std::printf("PASS test_aead_rejects_tampered_aad\n");
}

void test_aead_rejects_wrong_key() {
  uint8_t key_a[32], key_b[32], nonce[12];
  for (size_t i = 0; i < 32; ++i) { key_a[i] = (uint8_t)i; key_b[i] = (uint8_t)(i ^ 0xFF); }
  mesh_crypto::aead_generate_nonce(nonce);
  const uint8_t pt[] = "value";
  uint8_t ct[sizeof(pt) - 1], tag[16];
  assert(mesh_crypto::aead_encrypt(key_a, nonce, nullptr, 0, pt, sizeof(pt) - 1, ct, tag));
  uint8_t recovered[sizeof(pt) - 1];
  /* Wrong key MUST fail. */
  assert(!mesh_crypto::aead_decrypt(key_b, nonce, nullptr, 0, ct, sizeof(pt) - 1, tag, recovered));
  std::printf("PASS test_aead_rejects_wrong_key\n");
}

void test_aead_rejects_tampered_nonce() {
  uint8_t key[32], nonce[12];
  for (size_t i = 0; i < 32; ++i) key[i] = (uint8_t)(i + 1);
  mesh_crypto::aead_generate_nonce(nonce);
  const uint8_t pt[] = "secret";
  uint8_t ct[sizeof(pt) - 1], tag[16];
  assert(mesh_crypto::aead_encrypt(key, nonce, nullptr, 0, pt, sizeof(pt) - 1, ct, tag));
  /* Flip a bit in the nonce. */
  uint8_t bad_nonce[12]; std::memcpy(bad_nonce, nonce, 12); bad_nonce[5] ^= 0x10;
  uint8_t recovered[sizeof(pt) - 1];
  assert(!mesh_crypto::aead_decrypt(key, bad_nonce, nullptr, 0,
                                    ct, sizeof(pt) - 1, tag, recovered));
  std::printf("PASS test_aead_rejects_tampered_nonce\n");
}

void test_aead_aad_only_no_plaintext() {
  /* Empty plaintext path — AEAD must still authenticate the AAD. */
  uint8_t key[32], nonce[12];
  for (size_t i = 0; i < 32; ++i) key[i] = 0;
  mesh_crypto::aead_generate_nonce(nonce);
  const uint8_t aad[] = "auth-only";
  uint8_t tag[16];
  assert(mesh_crypto::aead_encrypt(key, nonce, aad, sizeof(aad) - 1, nullptr, 0, nullptr, tag));
  assert(mesh_crypto::aead_decrypt(key, nonce, aad, sizeof(aad) - 1, nullptr, 0, tag, nullptr));
  /* Tampered AAD still fails. */
  uint8_t bad_aad[] = "auth-only";
  bad_aad[0] ^= 0x40;
  assert(!mesh_crypto::aead_decrypt(key, nonce, bad_aad, sizeof(bad_aad) - 1,
                                    nullptr, 0, tag, nullptr));
  std::printf("PASS test_aead_aad_only_no_plaintext\n");
}

void test_x25519_generate_keypair_host_contract() {
  /* The X25519 keypair generator pairing (F33) and the rotation (F10)
   * use: the private scalar is clamped per RFC 7748 §5 and the public key
   * is that scalar times the base point — the same sequence as the
   * device path, on the host ladder. */
  uint8_t pa[32], ka[32], pb[32], kb[32];
  assert(mesh_crypto::x25519_generate_keypair(pa, ka));
  assert(mesh_crypto::x25519_generate_keypair(pb, kb));
  assert(std::memcmp(pa, pb, 32) != 0);
  assert((ka[0] & 0x07) == 0 && (ka[31] & 0x80) == 0 && (ka[31] & 0x40) != 0);
  assert((kb[0] & 0x07) == 0 && (kb[31] & 0x80) == 0 && (kb[31] & 0x40) != 0);
  const uint8_t nine[32] = {9};
  uint8_t pub_again[32];
  assert(mesh_crypto::x25519_derive(ka, nine, pub_again));
  assert(std::memcmp(pub_again, pa, 32) == 0);
  uint8_t s1[32], s2[32];
  assert(mesh_crypto::x25519_derive(ka, pb, s1));
  assert(mesh_crypto::x25519_derive(kb, pa, s2));
  assert(std::memcmp(s1, s2, 32) == 0);
  assert(!mesh_crypto::x25519_generate_keypair(nullptr, ka));
  assert(!mesh_crypto::x25519_generate_keypair(pa, nullptr));

  uint8_t r1[32] = {0}, r2[32] = {0};
  mesh_crypto::fill_random(r1, sizeof(r1));
  mesh_crypto::fill_random(r2, sizeof(r2));
  assert(std::memcmp(r1, r2, 32) != 0);
  mesh_crypto::fill_random(nullptr, 8);   /* no crash */
  std::printf("PASS test_x25519_generate_keypair_host_contract\n");
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
  test_x25519_rfc7748_vectors();
  test_x25519_mutual_dh_symmetric();
  test_x25519_over_ed25519_keys_disagrees();
  test_x25519_rejects_zero_pubkey();
  test_x25519_generate_keypair_host_contract();
  test_aead_encrypt_decrypt_roundtrip();
  test_aead_rejects_tampered_ciphertext();
  test_aead_rejects_tampered_aad();
  test_aead_rejects_wrong_key();
  test_aead_rejects_tampered_nonce();
  test_aead_aad_only_no_plaintext();
  std::printf("\nALL MESH_CRYPTO TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
