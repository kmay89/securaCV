/*
 * SecuraCV Canary — Mesh cryptographic primitives
 * Version 0.1.0
 *
 * Layered on top of mesh_transport. PR 2b ports the canary-wap mesh's
 * crypto primitives: domain-separated SHA-256, fingerprint derivation,
 * Opera ID derivation, and Ed25519 sign/verify. X25519 ECDH +
 * ChaCha20-Poly1305 AEAD arrive in PR 2c with the pairing state
 * machine they unlock.
 *
 * Implementation:
 *   • Device build  — uses ESP-IDF's vendored mbedtls for SHA-256 and
 *     the rweather/Crypto library (already in canary platformio.ini)
 *     for Ed25519. Matches the canary-wap implementation byte-for-byte
 *     so on-the-wire compatibility holds when both lanes coexist.
 *   • Host build (CSI_TEST_HOST_BUILD) — uses a vendored SHA-256 (FIPS
 *     180-4 reference) for the deterministic helpers, and stubs Ed25519
 *     sign/verify to a deterministic non-cryptographic shim. Tests
 *     verify the deterministic helpers against published vectors; the
 *     Ed25519 path is exercised on-device in CI.
 *
 * Domain separation:
 *   sha256_domain(domain, data) ≡ SHA-256(utf8(domain) || data).
 *   Every distinct use (fingerprint, opera_id, message-sign) carries a
 *   distinct domain string so identical inputs cannot collide across
 *   contexts. Domain strings are public; security comes from the
 *   collision-resistance of SHA-256, not from secrecy.
 */

#ifndef SECURACV_MESH_CRYPTO_H
#define SECURACV_MESH_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace mesh_crypto {

/* ──────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ────────────────────────────────────────────────────────────────────────── */

constexpr size_t SHA256_OUT_LEN     = 32;
constexpr size_t PUBKEY_LEN         = 32;   /* Ed25519 public key */
constexpr size_t PRIVKEY_LEN        = 32;   /* Ed25519 private key (seed) */
constexpr size_t SIGNATURE_LEN      = 64;   /* Ed25519 signature */
constexpr size_t FINGERPRINT_LEN    = 16;   /* truncated SHA-256 of pubkey */
constexpr size_t OPERA_ID_LEN       = 16;   /* truncated SHA-256 of opera secret */
constexpr size_t OPERA_SECRET_LEN   = 32;   /* opera secret */

/* Domain separation strings. Wire-compatible with canary-wap's mesh
 * (see DOMAIN_* constants in firmware/projects/canary-wap/arduino/
 * canary_wap/mesh_network.cpp). */
constexpr const char* DOMAIN_FINGERPRINT = "securacv:pubkey:fingerprint";
constexpr const char* DOMAIN_OPERA_ID    = "securacv:opera:id:v1";
constexpr const char* DOMAIN_MESSAGE     = "securacv:mesh:msg:v1";

/* ──────────────────────────────────────────────────────────────────────────
 * SHA-256 (domain-separated)
 *
 * out must point to at least SHA256_OUT_LEN bytes. domain is a
 * null-terminated UTF-8 string. data may be empty (data=nullptr,
 * data_len=0).
 * ────────────────────────────────────────────────────────────────────────── */

void sha256_domain(const char* domain,
                   const uint8_t* data, size_t data_len,
                   uint8_t out[SHA256_OUT_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * IDENTITY DERIVATION
 *
 * compute_fingerprint(pubkey)  →  first FINGERPRINT_LEN bytes of
 *   sha256_domain(DOMAIN_FINGERPRINT, pubkey).
 *
 * compute_opera_id(secret)     →  first OPERA_ID_LEN bytes of
 *   sha256_domain(DOMAIN_OPERA_ID, secret).
 *
 * Both are deterministic. Truncation to 16 bytes follows the canary-wap
 * convention; collision resistance is 2^64 which is the design target
 * for a sub-2^32-device home mesh.
 * ────────────────────────────────────────────────────────────────────────── */

void compute_fingerprint(const uint8_t pubkey[PUBKEY_LEN],
                         uint8_t fp_out[FINGERPRINT_LEN]);

void compute_opera_id(const uint8_t secret[OPERA_SECRET_LEN],
                      uint8_t id_out[OPERA_ID_LEN]);

/* Constant-time equality for fingerprints / opera IDs / hashes.
 * Returns true if all `n` bytes are equal. */
bool ct_equal(const uint8_t* a, const uint8_t* b, size_t n);

/* ──────────────────────────────────────────────────────────────────────────
 * Ed25519
 *
 * sign:    domain-separated message signature.
 *          sig = Ed25519::sign(privkey, pubkey, sha256_domain(DOMAIN_MESSAGE, msg)).
 * verify:  recompute and check.
 *
 * Both require pubkey alongside privkey for sign(): rweather's Ed25519
 * needs both, and the canary-wap impl keeps the same shape.
 *
 * generate_keypair: derives a fresh keypair. Privkey is sourced from
 *   esp_fill_random() on device; on host build it comes from the libc
 *   rand() — TEST USE ONLY, never call this in a host-only path that
 *   handles real key material.
 *
 * On host build, sign/verify use a deterministic non-cryptographic
 * shim so the API surface is exercisable without rweather/Crypto. Real
 * Ed25519 correctness is validated on-device.
 * ────────────────────────────────────────────────────────────────────────── */

bool ed25519_generate_keypair(uint8_t pubkey[PUBKEY_LEN],
                              uint8_t privkey[PRIVKEY_LEN]);

bool ed25519_sign(const uint8_t privkey[PRIVKEY_LEN],
                  const uint8_t pubkey[PUBKEY_LEN],
                  const uint8_t* msg, size_t msg_len,
                  uint8_t sig_out[SIGNATURE_LEN]);

bool ed25519_verify(const uint8_t pubkey[PUBKEY_LEN],
                    const uint8_t* msg, size_t msg_len,
                    const uint8_t sig[SIGNATURE_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * BACKEND IDENTITY
 *
 * Reports which crypto backend is active at runtime. Used by tests and
 * by the integration layer (so e.g. a host build doesn't accidentally
 * sign anything that would persist).
 * ────────────────────────────────────────────────────────────────────────── */

enum class Backend : uint8_t {
  PRODUCTION = 0,     /* device: mbedtls + rweather/Crypto */
  HOST_TEST_SHIM = 1  /* host build: vendored SHA-256 + Ed25519 deterministic stub */
};

Backend active_backend();

}  /* namespace mesh_crypto */

#endif  /* SECURACV_MESH_CRYPTO_H */
