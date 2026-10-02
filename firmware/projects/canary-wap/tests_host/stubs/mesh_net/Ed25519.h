/* Host stand-in for rweather/Crypto's Ed25519 — a REAL Ed25519 (OpenSSL's,
 * RFC 8032), not a mock, so a frame signed by one simulated device verifies
 * at another exactly when it would on a device, and a flipped bit does not. */
#ifndef STUB_MESH_NET_ED25519_H
#define STUB_MESH_NET_ED25519_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <openssl/evp.h>

#include "Arduino.h"   // host_sim::fill_random

class Ed25519 {
 public:
  static void sign(uint8_t signature[64], const uint8_t private_key[32],
                   const uint8_t /*public_key*/[32], const void* message, size_t len) {
    memset(signature, 0, 64);
    EVP_PKEY* k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, private_key, 32);
    EVP_MD_CTX* c = EVP_MD_CTX_new();
    size_t sl = 64;
    if (k != nullptr && c != nullptr && EVP_DigestSignInit(c, nullptr, nullptr, nullptr, k) == 1) {
      EVP_DigestSign(c, signature, &sl, static_cast<const unsigned char*>(message), len);
    }
    EVP_MD_CTX_free(c);
    EVP_PKEY_free(k);
  }
  static bool verify(const uint8_t signature[64], const uint8_t public_key[32],
                     const void* message, size_t len) {
    EVP_PKEY* k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, public_key, 32);
    EVP_MD_CTX* c = EVP_MD_CTX_new();
    bool ok = k != nullptr && c != nullptr &&
              EVP_DigestVerifyInit(c, nullptr, nullptr, nullptr, k) == 1 &&
              EVP_DigestVerify(c, signature, 64, static_cast<const unsigned char*>(message), len) == 1;
    EVP_MD_CTX_free(c);
    EVP_PKEY_free(k);
    return ok;
  }
  // A fresh private key: 32 bytes from the harness's fixed-seed generator
  // (chirp_channel.cpp's session identity, test_chirp_commands_wap).
  static void generatePrivateKey(uint8_t private_key[32]) {
    host_sim::fill_random(private_key, 32);
  }
  static void derivePublicKey(uint8_t public_key[32], const uint8_t private_key[32]) {
    memset(public_key, 0, 32);
    EVP_PKEY* k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, private_key, 32);
    size_t n = 32;
    if (k != nullptr) EVP_PKEY_get_raw_public_key(k, public_key, &n);
    EVP_PKEY_free(k);
  }
};

#endif
