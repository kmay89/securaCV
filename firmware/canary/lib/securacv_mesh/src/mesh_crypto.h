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
 *     180-4 reference) for the deterministic helpers and a real X25519
 *     (RFC 7748 ladder), and stubs Ed25519 sign/verify and the AEAD to
 *     deterministic non-cryptographic shims. Tests verify SHA-256 and
 *     X25519 against published vectors; the Ed25519 path is exercised
 *     on-device.
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
 *
 * Answers whether every mbedTLS step ran (sweep F364). On the ESP32's
 * hardware SHA a start, update or finish can answer an error, and after
 * one the digest skipped bytes or was never written; then `out` is zeroed
 * and the answer is false, and every caller refuses what the hash was for
 * (a pairing code, a CONFIRM, a rekey key, a signature, a hashed beacon
 * id) instead of using 32 zero bytes as a digest. [[nodiscard]]: a caller
 * that drops the answer draws -Wunused-result, a warning only, since
 * neither CI's host builds of this library (firmware.yml's mesh and
 * ble_scan suites) nor its PlatformIO envs pass -Werror; the host cases
 * that run each caller's refusal are the guard. False also for a null
 * `out`.
 * ────────────────────────────────────────────────────────────────────────── */

[[nodiscard]] bool sha256_domain(const char* domain,
                                 const uint8_t* data, size_t data_len,
                                 uint8_t out[SHA256_OUT_LEN]);

#ifdef CSI_TEST_HOST_BUILD
/* Host-test hook (sweep F364). The host build's vendored SHA-256 has no
 * failing step, so a test that needs sha256_domain() to refuse arms one:
 * the next `count` hashes whose domain is exactly `domain` (nullptr: any
 * domain) fail. A failed hash writes 0xA5 over `out` first, as a step the
 * engine left half done might, so the zeros a caller reads are
 * sha256_domain()'s own. fail_sha256_domain(nullptr, 0) disarms; each call
 * re-arms and restarts the count sha256_domain_failures() reads. */
namespace test {
void fail_sha256_domain(const char* domain, int count);
int  sha256_domain_failures();
}  /* namespace test */
#endif

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
 *
 * When the hash fails (sha256_domain() answers false) the output is all
 * zeros. Both keep their void signature for now: what each of their ten
 * callers in mesh_session.cpp and mesh_state.cpp should refuse mirrors the
 * canary-wap's open policy calls (sweep F456-F458), so it is decided there
 * first (the sweep ledger's follow-up to F364).
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
 *
 * A message whose hash fails is neither signed nor verified (sweep F364):
 * sign answers false with sig_out zeroed, verify answers false. Before,
 * each went on over whatever the failed hash left, so a sender signed, and
 * a receiver checked, a digest that was not the message's.
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
 *   • X25519 takes 32-byte Curve25519 keys, NOT Ed25519 keys. An Ed25519
 *     public key is an Edwards-curve point derived from SHA-512(seed), not
 *     the Montgomery u-coordinate of the seed times the base point, so two
 *     sides that feed Ed25519 keys in derive DIFFERENT values. Generate
 *     both halves with x25519_generate_keypair() below (F33 part 2: pairing
 *     used ed25519_generate_keypair() here until then).
 *   • rweather's Curve25519::eval does NOT clamp: it takes bits 254..0 of
 *     the scalar as given. The generator clamps (RFC 7748 §5).
 *   • shared MUST NOT be used directly as a key — always KDF first.
 *
 * Returns false when the peer's u-coordinate is not canonical or the
 * result is all-zero (a low-order point). The host build runs a REAL
 * X25519 — the RFC 7748 ladder in mesh_crypto.cpp, checked against the
 * RFC's vectors — not a shim, so a host test's two sides agree only when
 * a device's would. Sensitive intermediate state is zeroed before return.
 * ────────────────────────────────────────────────────────────────────────── */

constexpr size_t X25519_SHARED_LEN = 32;

bool x25519_derive(const uint8_t our_priv[PRIVKEY_LEN],
                   const uint8_t peer_pub[PUBKEY_LEN],
                   uint8_t shared_out[X25519_SHARED_LEN]);

/* Generate a fresh X25519 keypair for x25519_derive() (F10-rekey).
 *
 * This is NOT ed25519_generate_keypair(): an Ed25519 public key is an
 * Edwards-curve point derived from SHA-512(seed), not the Curve25519
 * u-coordinate of the seed times the base point, so feeding Ed25519 keys
 * to x25519_derive() gives the two sides DIFFERENT shared values on a
 * real device. Device path: 32 bytes from esp_fill_random(), clamped per
 * RFC 7748 (rweather's Curve25519::eval does not clamp), pub =
 * Curve25519::eval(priv, basepoint), retried on the (negligible) eval
 * refusal. Host path: the same sequence with rand() (TEST USE ONLY) and
 * the host X25519 ladder. Used by pairing's ephemeral keys (F33 part 2)
 * and by the F10 rotation. Returns false only on null pointers or an
 * RNG/eval failure. */
bool x25519_generate_keypair(uint8_t pub_out [PUBKEY_LEN],
                             uint8_t priv_out[PRIVKEY_LEN]);

/* Fill `out` with `len` random bytes: esp_fill_random() on device (RF
 * must be up for full entropy — true whenever the mesh is running),
 * rand() on host (TEST ONLY). Used for fresh opera secrets (F10-rekey). */
void fill_random(uint8_t* out, size_t len);

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
