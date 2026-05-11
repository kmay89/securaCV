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
constexpr size_t FINGERPRINT_LEN    = 8;    /* truncated SHA-256 of pubkey (wire-compat: canary-wap FINGERPRINT_SIZE) */
constexpr size_t OPERA_ID_LEN       = 16;   /* truncated SHA-256 of opera secret */
constexpr size_t OPERA_SECRET_LEN   = 32;   /* opera secret */

/* Domain separation strings. Wire-compatible with canary-wap's mesh
 * (DOMAIN_* constants in firmware/projects/canary-wap/arduino/canary_wap/
 * mesh_network.cpp:42-46). Identical byte-for-byte so paired canary +
 * canary-wap nodes hash to the same fingerprint / opera_id and verify
 * each other's signatures. */
constexpr const char* DOMAIN_FINGERPRINT = "securacv:pubkey:fingerprint";
constexpr const char* DOMAIN_OPERA_ID    = "securacv:opera:id:v0";
constexpr const char* DOMAIN_MESSAGE     = "securacv:mesh:message:v0";

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
 * Both are deterministic. Truncation lengths match canary-wap so the
 * wire format is identical. 8-byte (64-bit) fingerprint = 2^32 birthday
 * bound on random collisions, which is sound for the design target
 * (sub-100-device home mesh). The 16-byte (128-bit) opera_id has 2^64
 * birthday resistance.
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
 * X25519 ECDH
 *
 * Diffie-Hellman shared-secret derivation over Curve25519. Both peers
 * compute the same `shared` from their own privkey and the other's
 * pubkey. The output is suitable as input to a KDF (the canary-wap
 * pairing flow runs the shared secret through sha256_domain with a
 * SESSION domain — that derivation lives in the pairing module, not
 * here, so this primitive stays single-purpose).
 *
 * Note on key formats:
 *   • X25519 takes 32-byte Curve25519 keys, NOT Ed25519 keys directly.
 *     rweather's Curve25519::eval clamps the private scalar internally
 *     so callers can pass either an Ed25519 seed (after clamping) or
 *     an explicit Curve25519 private. Wire-format compatibility with
 *     canary-wap requires passing the same input it expects (see
 *     mesh_network.cpp:derive_session_key).
 *   • shared MUST NOT be used directly as a key — always KDF first.
 *
 * Returns false on identity element / low-order point (a real X25519
 * implementation defends against these; the host shim accepts anything
 * non-zero). Sensitive intermediate state is zeroed before return.
 * ────────────────────────────────────────────────────────────────────────── */

constexpr size_t X25519_SHARED_LEN = 32;

bool x25519_derive(const uint8_t our_priv[PRIVKEY_LEN],
                   const uint8_t peer_pub[PUBKEY_LEN],
                   uint8_t shared_out[X25519_SHARED_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * ChaCha20-Poly1305 AEAD
 *
 * AEAD per RFC 8439: authenticates aad (associated data) + encrypts
 * plaintext, producing ciphertext (same length as plaintext) + a 16-byte
 * authentication tag. Decrypt fails (returns false) if the tag doesn't
 * match — i.e. any tamper of ciphertext, aad, nonce, or key.
 *
 * Nonce semantics (RFC 8439 §3): MUST be unique for each (key, nonce)
 * pair. A reused nonce under the same key fatally breaks confidentiality
 * AND authenticity. Callers are responsible for ensuring uniqueness —
 * the typical pattern is per-session msg-counter ∥ random.
 *
 * In-place encryption is safe (plaintext and ciphertext may alias). The
 * encrypt() output is exactly ciphertext_len = plaintext_len; the tag
 * is delivered separately so the caller controls framing.
 * ────────────────────────────────────────────────────────────────────────── */

constexpr size_t AEAD_KEY_LEN    = 32;
constexpr size_t AEAD_NONCE_LEN  = 12;
constexpr size_t AEAD_TAG_LEN    = 16;

bool aead_encrypt(const uint8_t key[AEAD_KEY_LEN],
                  const uint8_t nonce[AEAD_NONCE_LEN],
                  const uint8_t* aad, size_t aad_len,
                  const uint8_t* plaintext, size_t pt_len,
                  uint8_t* ciphertext_out,
                  uint8_t tag_out[AEAD_TAG_LEN]);

/* Returns false on tag mismatch. On failure plaintext_out is zeroed so
 * the caller cannot accidentally consume forged data. */
bool aead_decrypt(const uint8_t key[AEAD_KEY_LEN],
                  const uint8_t nonce[AEAD_NONCE_LEN],
                  const uint8_t* aad, size_t aad_len,
                  const uint8_t* ciphertext, size_t ct_len,
                  const uint8_t tag[AEAD_TAG_LEN],
                  uint8_t* plaintext_out);

/* Random AEAD-sized nonce. Sources from esp_fill_random on device, rand()
 * on host (TEST ONLY — never use the host path for real ciphertext). */
void aead_generate_nonce(uint8_t nonce_out[AEAD_NONCE_LEN]);

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
