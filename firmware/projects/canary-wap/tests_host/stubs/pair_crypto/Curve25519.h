/* Host stand-in for rweather/Crypto's Curve25519 (canary-wap host tests
 * only) — a REAL X25519, OpenSSL's, not a mock: test_mesh_pair_crypto.cpp
 * needs the two sides of a pairing to agree exactly when they would on a
 * device.
 *
 * One semantic difference, stated so nobody leans on it: rweather's eval()
 * takes the scalar as given (bits 254..0, no clamping) and OpenSSL's X25519
 * clamps it (RFC 7748 §5). For the clamped scalars mesh_pair_crypto.h
 * generates the two compute the same value, which the test pins against the
 * RFC 7748 §6.1 vector; for an unclamped scalar they would differ. eval()
 * returns false for a non-canonical u like rweather's, and also where
 * OpenSSL refuses (an all-zero result), which mesh_pair_crypto.h refuses
 * anyway. */
#ifndef STUB_PAIR_CRYPTO_CURVE25519_H
#define STUB_PAIR_CRYPTO_CURVE25519_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <openssl/evp.h>

class Curve25519 {
 public:
  /* result = s * x (x == nullptr: the base point, u = 9). */
  static bool eval(uint8_t result[32], const uint8_t s[32], const uint8_t x[32]) {
    uint8_t u[32] = {9};
    if (x != nullptr) memcpy(u, x, 32);
    u[31] &= 0x7F;
    bool canonical = true;
    if (u[31] == 0x7F && u[0] >= 0xED) {
      canonical = false;
      for (int i = 1; i <= 30; ++i) if (u[i] != 0xFF) { canonical = true; break; }
    }
    EVP_PKEY* priv = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, s, 32);
    EVP_PKEY* peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, u, 32);
    bool ok = false;
    if (priv != nullptr && peer != nullptr) {
      EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(priv, nullptr);
      size_t len = 32;
      ok = ctx != nullptr && EVP_PKEY_derive_init(ctx) == 1 &&
           EVP_PKEY_derive_set_peer(ctx, peer) == 1 &&
           EVP_PKEY_derive(ctx, result, &len) == 1 && len == 32;
      EVP_PKEY_CTX_free(ctx);
    }
    EVP_PKEY_free(peer);
    EVP_PKEY_free(priv);
    if (!ok) memset(result, 0, 32);
    return ok && canonical;
  }
};

#endif
