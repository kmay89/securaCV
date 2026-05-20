/*
 * SecuraCV Canary — Mesh cryptographic primitives — Implementation
 *
 * Two paths via #ifdef CSI_TEST_HOST_BUILD:
 *   • Device path uses ESP-IDF's vendored mbedtls (SHA-256) and the
 *     rweather/Crypto library (Ed25519). This is the production code.
 *   • Host path uses a vendored reference SHA-256 (FIPS 180-4, public
 *     domain) and a non-cryptographic Ed25519 shim. Real Ed25519
 *     correctness is exercised on-device.
 *
 * The shape mirrors firmware/projects/canary-wap/arduino/canary_wap/
 * mesh_network.cpp's static crypto helpers, so the wire format will
 * match canary-wap byte-for-byte once the message-dispatch layer
 * (PR 2c) lands.
 */

#include "mesh_crypto.h"

#include <string.h>

#ifdef CSI_TEST_HOST_BUILD
  #include <stdlib.h>   /* rand() for host-only keypair generation */
#else
  #include <Arduino.h>
  extern "C" {
    #include <mbedtls/sha256.h>
    #include <esp_system.h>      /* esp_fill_random */
    #include <esp_idf_version.h> /* ESP_IDF_VERSION_MAJOR — picks between
                                    mbedTLS 2.x void-return (IDF 4.x;
                                    arduino-esp32 2.0.x, the canary PIO
                                    target today) and mbedTLS 3.x int-
                                    return (IDF 5.x; arduino-esp32 3.x). */
  }
  /* rweather/Crypto is pulled in by canary platformio.ini. Headers are
   * top-level on the include path. */
  #include <Ed25519.h>
  #include <Curve25519.h>
  #include <ChaChaPoly.h>
#endif

namespace mesh_crypto {

/* ──────────────────────────────────────────────────────────────────────────
 * SECURE-ZERO
 *
 * memset() of about-to-go-out-of-scope memory is eligible for dead-store
 * elimination — CodeQL flagged exactly this on x25519_derive's local
 * privkey copy. secure_zero() uses a volatile pointer + a memory-barrier
 * inline asm so the writes can't be optimized away. Same pattern as
 * firmware/common/csi/src/csi_hal.cpp:secure_wipe().
 * ────────────────────────────────────────────────────────────────────────── */

static void secure_zero(void* ptr, size_t len) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
  while (len--) { *p++ = 0; }
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" ::: "memory");
#endif
}

/* ──────────────────────────────────────────────────────────────────────────
 * HOST-BUILD VENDORED SHA-256 (FIPS 180-4 reference, public domain)
 *
 * Used by sha256_domain() in host builds so unit tests can verify
 * fingerprint / opera_id against the published Ed25519 / SHA-256 test
 * vectors. The device build uses mbedtls instead.
 * ────────────────────────────────────────────────────────────────────────── */

#ifdef CSI_TEST_HOST_BUILD
namespace host_sha256 {

struct Ctx {
  uint32_t state[8];
  uint64_t bitlen;
  uint32_t datalen;
  uint8_t  buffer[64];
};

static const uint32_t K[64] = {
  0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
  0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
  0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
  0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
  0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
  0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
  0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
  0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void transform(Ctx* c, const uint8_t* d) {
  uint32_t m[64], a, b, cc, dd, e, f, g, h, t1, t2;
  for (uint32_t i = 0, j = 0; i < 16; ++i, j += 4) {
    m[i] = ((uint32_t)d[j] << 24) | ((uint32_t)d[j+1] << 16) | ((uint32_t)d[j+2] << 8) | (uint32_t)d[j+3];
  }
  for (uint32_t i = 16; i < 64; ++i) {
    uint32_t s0 = rotr(m[i-15], 7) ^ rotr(m[i-15], 18) ^ (m[i-15] >> 3);
    uint32_t s1 = rotr(m[i-2], 17) ^ rotr(m[i-2], 19) ^ (m[i-2] >> 10);
    m[i] = m[i-16] + s0 + m[i-7] + s1;
  }
  a = c->state[0]; b = c->state[1]; cc = c->state[2]; dd = c->state[3];
  e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];
  for (uint32_t i = 0; i < 64; ++i) {
    t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + m[i];
    t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
    h = g; g = f; f = e; e = dd + t1; dd = cc; cc = b; b = a; a = t1 + t2;
  }
  c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += dd;
  c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static void init(Ctx* c) {
  c->datalen = 0; c->bitlen = 0;
  c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
  c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
  c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
  c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
}

static void update(Ctx* c, const uint8_t* d, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    c->buffer[c->datalen++] = d[i];
    if (c->datalen == 64) {
      transform(c, c->buffer);
      c->bitlen += 512;
      c->datalen = 0;
    }
  }
}

static void final_out(Ctx* c, uint8_t out[32]) {
  uint32_t i = c->datalen;
  if (c->datalen < 56) {
    c->buffer[i++] = 0x80;
    while (i < 56) c->buffer[i++] = 0;
  } else {
    c->buffer[i++] = 0x80;
    while (i < 64) c->buffer[i++] = 0;
    transform(c, c->buffer);
    memset(c->buffer, 0, 56);
  }
  c->bitlen += (uint64_t)c->datalen * 8;
  for (i = 0; i < 8; ++i) c->buffer[63 - i] = (uint8_t)(c->bitlen >> (i * 8));
  transform(c, c->buffer);
  for (i = 0; i < 8; ++i) {
    out[i*4]     = (uint8_t)(c->state[i] >> 24);
    out[i*4 + 1] = (uint8_t)(c->state[i] >> 16);
    out[i*4 + 2] = (uint8_t)(c->state[i] >> 8);
    out[i*4 + 3] = (uint8_t)c->state[i];
  }
}

}  /* namespace host_sha256 */
#endif  /* CSI_TEST_HOST_BUILD */

/* ──────────────────────────────────────────────────────────────────────────
 * SHA-256 (domain-separated)
 * ────────────────────────────────────────────────────────────────────────── */

void sha256_domain(const char* domain,
                   const uint8_t* data, size_t data_len,
                   uint8_t out[SHA256_OUT_LEN]) {
  if (out == nullptr) return;
  const size_t domain_len = (domain != nullptr) ? strlen(domain) : 0;

#ifdef CSI_TEST_HOST_BUILD
  host_sha256::Ctx ctx;
  host_sha256::init(&ctx);
  if (domain_len > 0) host_sha256::update(&ctx, (const uint8_t*)domain, domain_len);
  if (data != nullptr && data_len > 0) host_sha256::update(&ctx, data, data_len);
  host_sha256::final_out(&ctx, out);
#else
  /* mbedTLS API differs between IDF 4.x and IDF 5.x:
   *   • IDF 5.x (mbedTLS 3.x; arduino-esp32 3.x): non-suffixed names
   *     return int — error-checking path.
   *   • IDF 4.x (mbedTLS 2.x; arduino-esp32 2.0.x, this build today):
   *     non-suffixed names return void; the _ret variants are the
   *     int-returning ones but were REMOVED in mbedTLS 3.x. To stay
   *     portable across both, we branch on ESP_IDF_VERSION_MAJOR.
   * The same domain-separation contract holds either way:
   * SHA-256("<domain>" || data) → out[32]. */
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
#if ESP_IDF_VERSION_MAJOR >= 5
  /* mbedTLS 3.x — defensive return checks so a future upstream API
   * tightening or stack-use-after-free trap leaves the output buffer
   * zeroed rather than uninitialized. */
  int rc = mbedtls_sha256_starts(&ctx, 0);
  if (rc == 0 && domain_len > 0) {
    rc = mbedtls_sha256_update(&ctx, (const uint8_t*)domain, domain_len);
  }
  if (rc == 0 && data != nullptr && data_len > 0) {
    rc = mbedtls_sha256_update(&ctx, data, data_len);
  }
  if (rc == 0) {
    rc = mbedtls_sha256_finish(&ctx, out);
  }
  if (rc != 0) {
    secure_zero(out, SHA256_OUT_LEN);
  }
#else
  /* mbedTLS 2.x — void return. The whole arduino-esp32 2.0.x project
   * (beacon_channel.cpp, rf_presence.cpp, ble_ota.cpp, canary-wap's
   * mesh_network.cpp) calls these without checks; we match that
   * convention here. */
  mbedtls_sha256_starts(&ctx, 0);
  if (domain_len > 0) {
    mbedtls_sha256_update(&ctx, (const uint8_t*)domain, domain_len);
  }
  if (data != nullptr && data_len > 0) {
    mbedtls_sha256_update(&ctx, data, data_len);
  }
  mbedtls_sha256_finish(&ctx, out);
#endif
  mbedtls_sha256_free(&ctx);
#endif  /* CSI_TEST_HOST_BUILD */
}

/* ──────────────────────────────────────────────────────────────────────────
 * IDENTITY DERIVATION
 * ────────────────────────────────────────────────────────────────────────── */

void compute_fingerprint(const uint8_t pubkey[PUBKEY_LEN],
                         uint8_t fp_out[FINGERPRINT_LEN]) {
  uint8_t hash[SHA256_OUT_LEN];
  sha256_domain(DOMAIN_FINGERPRINT, pubkey, PUBKEY_LEN, hash);
  memcpy(fp_out, hash, FINGERPRINT_LEN);
}

void compute_opera_id(const uint8_t secret[OPERA_SECRET_LEN],
                      uint8_t id_out[OPERA_ID_LEN]) {
  uint8_t hash[SHA256_OUT_LEN];
  sha256_domain(DOMAIN_OPERA_ID, secret, OPERA_SECRET_LEN, hash);
  memcpy(id_out, hash, OPERA_ID_LEN);
}

bool ct_equal(const uint8_t* a, const uint8_t* b, size_t n) {
  if (a == nullptr || b == nullptr) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < n; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Ed25519
 * ────────────────────────────────────────────────────────────────────────── */

bool ed25519_generate_keypair(uint8_t pubkey[PUBKEY_LEN],
                              uint8_t privkey[PRIVKEY_LEN]) {
  if (pubkey == nullptr || privkey == nullptr) return false;
#ifdef CSI_TEST_HOST_BUILD
  /* Test-only RNG. NEVER use this for real key material. */
  for (size_t i = 0; i < PRIVKEY_LEN; ++i) privkey[i] = (uint8_t)(rand() & 0xFF);
  /* Deterministic "pub" derivation for the shim: pub = SHA-256(priv)
   * truncated to 32. Real Ed25519 derives pub via scalar multiplication;
   * here we just need a value that's distinct and reproducible from
   * the priv, so verify() can roundtrip. */
  uint8_t h[SHA256_OUT_LEN];
  sha256_domain("securacv:host-shim:pub", privkey, PRIVKEY_LEN, h);
  memcpy(pubkey, h, PUBKEY_LEN);
  return true;
#else
  uint8_t seed[PRIVKEY_LEN];
  esp_fill_random(seed, sizeof(seed));
  memcpy(privkey, seed, PRIVKEY_LEN);
  Ed25519::derivePublicKey(pubkey, privkey);
  return true;
#endif
}

bool ed25519_sign(const uint8_t privkey[PRIVKEY_LEN],
                  const uint8_t pubkey[PUBKEY_LEN],
                  const uint8_t* msg, size_t msg_len,
                  uint8_t sig_out[SIGNATURE_LEN]) {
  if (privkey == nullptr || pubkey == nullptr || sig_out == nullptr) return false;
  if (msg == nullptr && msg_len > 0) return false;

  uint8_t msg_hash[SHA256_OUT_LEN];
  sha256_domain(DOMAIN_MESSAGE, msg, msg_len, msg_hash);

#ifdef CSI_TEST_HOST_BUILD
  /* Deterministic non-cryptographic signature for the host shim.
   * sig = SHA-256("sign-shim:v1" || privkey || msg_hash) || SHA-256(
   *       "sign-shim:v1:tag" || pubkey || msg_hash).
   * 64 bytes total. Verify() recomputes the second half (pubkey-only
   * derivable) and trust-checks the first half by exact match. The
   * shim doesn't prove possession of the privkey; tests using it
   * verify wiring, not crypto. */
  uint8_t buf[12 + PRIVKEY_LEN + SHA256_OUT_LEN];
  memcpy(buf, "sign-shim:v1", 12);
  memcpy(buf + 12, privkey, PRIVKEY_LEN);
  memcpy(buf + 12 + PRIVKEY_LEN, msg_hash, SHA256_OUT_LEN);
  sha256_domain("", buf, sizeof(buf), sig_out);

  uint8_t buf2[16 + PUBKEY_LEN + SHA256_OUT_LEN];
  memcpy(buf2, "sign-shim:v1:tag", 16);
  memcpy(buf2 + 16, pubkey, PUBKEY_LEN);
  memcpy(buf2 + 16 + PUBKEY_LEN, msg_hash, SHA256_OUT_LEN);
  sha256_domain("", buf2, sizeof(buf2), sig_out + 32);
  return true;
#else
  Ed25519::sign(sig_out, privkey, pubkey, msg_hash, SHA256_OUT_LEN);
  return true;
#endif
}

bool ed25519_verify(const uint8_t pubkey[PUBKEY_LEN],
                    const uint8_t* msg, size_t msg_len,
                    const uint8_t sig[SIGNATURE_LEN]) {
  if (pubkey == nullptr || sig == nullptr) return false;
  if (msg == nullptr && msg_len > 0) return false;

  uint8_t msg_hash[SHA256_OUT_LEN];
  sha256_domain(DOMAIN_MESSAGE, msg, msg_len, msg_hash);

#ifdef CSI_TEST_HOST_BUILD
  /* Host shim: recompute the pubkey-derivable second half and check it
   * matches sig[32..63]. The first half is NOT verifiable without the
   * privkey, so the shim treats roundtrip-only — sign() then verify()
   * must succeed; ANY tampering with msg or pubkey makes the second
   * half mismatch. This is enough to exercise the API wiring; on-device
   * crypto handles real signature correctness. */
  uint8_t expected_tag[SHA256_OUT_LEN];
  uint8_t buf[16 + PUBKEY_LEN + SHA256_OUT_LEN];
  memcpy(buf, "sign-shim:v1:tag", 16);
  memcpy(buf + 16, pubkey, PUBKEY_LEN);
  memcpy(buf + 16 + PUBKEY_LEN, msg_hash, SHA256_OUT_LEN);
  sha256_domain("", buf, sizeof(buf), expected_tag);
  return ct_equal(sig + 32, expected_tag, SHA256_OUT_LEN);
#else
  return Ed25519::verify(sig, pubkey, msg_hash, SHA256_OUT_LEN);
#endif
}

/* ──────────────────────────────────────────────────────────────────────────
 * X25519 ECDH
 * ────────────────────────────────────────────────────────────────────────── */

static bool is_all_zero(const uint8_t* p, size_t n) {
  uint8_t accum = 0;
  for (size_t i = 0; i < n; ++i) accum |= p[i];
  return accum == 0;
}

bool x25519_derive(const uint8_t our_priv[PRIVKEY_LEN],
                   const uint8_t peer_pub[PUBKEY_LEN],
                   uint8_t shared_out[X25519_SHARED_LEN]) {
  if (our_priv == nullptr || peer_pub == nullptr || shared_out == nullptr) return false;

#ifdef CSI_TEST_HOST_BUILD
  /* Host shim: derive a symmetric "shared" from the canonical ordering
   * of (priv, peer_pub) — concretely SHA-256("dh-shim:v1" || min(priv,
   * peer_pub) || max(priv, peer_pub)). Two peers that swap roles
   * (A.priv + B.pub) vs (B.priv + A.pub) compute the same value IFF
   * the host shim's keypair derives pub = SHA-256("securacv:host-
   * shim:pub" || priv) — which it does (see ed25519_generate_keypair).
   *
   * Strictly: shared = SHA-256("dh-shim:v1" || sorted(pub_a, pub_b)),
   * computed using the LOCAL priv's derived pub to reconstruct A's pub
   * and the peer_pub directly. We don't need the actual DH property
   * (since the priv is never used in the math) — only that both sides
   * compute the same shared. The test asserts this. */
  uint8_t our_pub[PUBKEY_LEN];
  uint8_t h[SHA256_OUT_LEN];
  sha256_domain("securacv:host-shim:pub", our_priv, PRIVKEY_LEN, h);
  memcpy(our_pub, h, PUBKEY_LEN);

  const uint8_t* lo = our_pub;
  const uint8_t* hi = peer_pub;
  if (memcmp(our_pub, peer_pub, PUBKEY_LEN) > 0) { lo = peer_pub; hi = our_pub; }

  uint8_t concat[PUBKEY_LEN * 2];
  memcpy(concat, lo, PUBKEY_LEN);
  memcpy(concat + PUBKEY_LEN, hi, PUBKEY_LEN);
  sha256_domain("dh-shim:v1", concat, sizeof(concat), h);
  memcpy(shared_out, h, X25519_SHARED_LEN);

  /* Refuse if peer_pub is the all-zero element (mirrors the device
   * path's behavior; in real Curve25519 the all-zero point is
   * low-order and DH produces the zero shared). */
  if (is_all_zero(peer_pub, PUBKEY_LEN)) {
    memset(shared_out, 0, X25519_SHARED_LEN);
    return false;
  }
  return true;
#else
  /* Real X25519 via rweather's Curve25519. eval() returns false on
   * low-order / identity points; on any failure we zero the output
   * to avoid leaking residual stack bytes.
   *
   * The local_priv_copy is wiped with secure_zero, NOT memset — the
   * latter is eligible for dead-store elimination since the buffer
   * goes out of scope immediately after (CodeQL flagged this on the
   * earlier revision). */
  uint8_t local_priv_copy[PRIVKEY_LEN];
  memcpy(local_priv_copy, our_priv, PRIVKEY_LEN);
  bool ok = Curve25519::eval(shared_out, local_priv_copy, peer_pub);
  secure_zero(local_priv_copy, sizeof(local_priv_copy));
  if (!ok || is_all_zero(shared_out, X25519_SHARED_LEN)) {
    secure_zero(shared_out, X25519_SHARED_LEN);
    return false;
  }
  return true;
#endif
}

/* ──────────────────────────────────────────────────────────────────────────
 * ChaCha20-Poly1305 AEAD
 *
 * Device path uses rweather/Crypto's ChaChaPoly. Host shim uses a
 * keyed XOR stream + HMAC-SHA-256-derived tag — NOT cryptographic, but
 * has the right API contract (encrypt then decrypt roundtrips; any
 * tamper of key, nonce, aad, ciphertext, or tag makes decrypt fail).
 * ────────────────────────────────────────────────────────────────────────── */

#ifdef CSI_TEST_HOST_BUILD
/* Host shim helpers. */
static void shim_aead_keystream(const uint8_t key[AEAD_KEY_LEN],
                                const uint8_t nonce[AEAD_NONCE_LEN],
                                uint8_t* out, size_t len) {
  /* keystream[i] = first byte of SHA-256("aead-shim:v1" || key ||
   *                 nonce || u32_be(block_index)), one byte per block.
   * Block index advances every byte (deliberately wasteful — this is
   * for testing, not throughput). */
  for (size_t i = 0; i < len; ++i) {
    uint8_t buf[12 + AEAD_KEY_LEN + AEAD_NONCE_LEN + 4];
    memcpy(buf, "aead-shim:v1", 12);
    memcpy(buf + 12, key, AEAD_KEY_LEN);
    memcpy(buf + 12 + AEAD_KEY_LEN, nonce, AEAD_NONCE_LEN);
    buf[12 + AEAD_KEY_LEN + AEAD_NONCE_LEN]     = (uint8_t)(i >> 24);
    buf[12 + AEAD_KEY_LEN + AEAD_NONCE_LEN + 1] = (uint8_t)(i >> 16);
    buf[12 + AEAD_KEY_LEN + AEAD_NONCE_LEN + 2] = (uint8_t)(i >> 8);
    buf[12 + AEAD_KEY_LEN + AEAD_NONCE_LEN + 3] = (uint8_t)i;
    uint8_t h[SHA256_OUT_LEN];
    sha256_domain("", buf, sizeof(buf), h);
    out[i] = h[0];
  }
}

static void shim_aead_tag(const uint8_t key[AEAD_KEY_LEN],
                          const uint8_t nonce[AEAD_NONCE_LEN],
                          const uint8_t* aad, size_t aad_len,
                          const uint8_t* ciphertext, size_t ct_len,
                          uint8_t tag_out[AEAD_TAG_LEN]) {
  /* tag = first 16 bytes of SHA-256("aead-shim:v1:tag" || key || nonce
   *       || u32_be(aad_len) || aad || u32_be(ct_len) || ciphertext).
   * Each field length-prefixed so unrelated inputs can't collide
   * (extension attacks). */
  uint8_t h[SHA256_OUT_LEN];
  /* Pass everything through a single sha256_domain call by allocating
   * a small heap buffer. Keep it simple; tests use small messages. */
  size_t total = 16 + AEAD_KEY_LEN + AEAD_NONCE_LEN + 4 + aad_len + 4 + ct_len;
  uint8_t* buf = (uint8_t*)malloc(total);
  if (buf == nullptr) {
    /* Out-of-memory shouldn't happen in a host test, but if it does,
     * zero the tag so any verify will fail loudly. */
    memset(tag_out, 0, AEAD_TAG_LEN);
    return;
  }
  size_t off = 0;
  memcpy(buf + off, "aead-shim:v1:tag", 16);                       off += 16;
  memcpy(buf + off, key, AEAD_KEY_LEN);                            off += AEAD_KEY_LEN;
  memcpy(buf + off, nonce, AEAD_NONCE_LEN);                        off += AEAD_NONCE_LEN;
  buf[off++] = (uint8_t)(aad_len >> 24);
  buf[off++] = (uint8_t)(aad_len >> 16);
  buf[off++] = (uint8_t)(aad_len >> 8);
  buf[off++] = (uint8_t)aad_len;
  if (aad != nullptr && aad_len > 0) { memcpy(buf + off, aad, aad_len); off += aad_len; }
  buf[off++] = (uint8_t)(ct_len >> 24);
  buf[off++] = (uint8_t)(ct_len >> 16);
  buf[off++] = (uint8_t)(ct_len >> 8);
  buf[off++] = (uint8_t)ct_len;
  if (ciphertext != nullptr && ct_len > 0) { memcpy(buf + off, ciphertext, ct_len); off += ct_len; }
  sha256_domain("", buf, off, h);
  secure_zero(buf, total);
  free(buf);
  memcpy(tag_out, h, AEAD_TAG_LEN);
}
#endif

bool aead_encrypt(const uint8_t key[AEAD_KEY_LEN],
                  const uint8_t nonce[AEAD_NONCE_LEN],
                  const uint8_t* aad, size_t aad_len,
                  const uint8_t* plaintext, size_t pt_len,
                  uint8_t* ciphertext_out,
                  uint8_t tag_out[AEAD_TAG_LEN]) {
  if (key == nullptr || nonce == nullptr || tag_out == nullptr) return false;
  if (pt_len > 0 && (plaintext == nullptr || ciphertext_out == nullptr)) return false;
  if (aad_len > 0 && aad == nullptr) return false;

#ifdef CSI_TEST_HOST_BUILD
  /* Encrypt: ciphertext = plaintext XOR keystream(key, nonce). */
  if (pt_len > 0) {
    uint8_t* stream = (uint8_t*)malloc(pt_len);
    if (stream == nullptr) return false;
    shim_aead_keystream(key, nonce, stream, pt_len);
    for (size_t i = 0; i < pt_len; ++i) ciphertext_out[i] = plaintext[i] ^ stream[i];
    secure_zero(stream, pt_len);
    free(stream);
  }
  shim_aead_tag(key, nonce, aad, aad_len, ciphertext_out, pt_len, tag_out);
  return true;
#else
  /* Match canary-wap's pattern: check setKey/setIV returns, always
   * call clear() at end to wipe key material from the ChaChaPoly
   * object's internal stack-allocated state. */
  ChaChaPoly chacha;
  if (!chacha.setKey(key, AEAD_KEY_LEN) || !chacha.setIV(nonce, AEAD_NONCE_LEN)) {
    chacha.clear();
    return false;
  }
  if (aad_len > 0) chacha.addAuthData(aad, aad_len);
  if (pt_len > 0) chacha.encrypt(ciphertext_out, plaintext, pt_len);
  chacha.computeTag(tag_out, AEAD_TAG_LEN);
  chacha.clear();
  return true;
#endif
}

bool aead_decrypt(const uint8_t key[AEAD_KEY_LEN],
                  const uint8_t nonce[AEAD_NONCE_LEN],
                  const uint8_t* aad, size_t aad_len,
                  const uint8_t* ciphertext, size_t ct_len,
                  const uint8_t tag[AEAD_TAG_LEN],
                  uint8_t* plaintext_out) {
  if (key == nullptr || nonce == nullptr || tag == nullptr) return false;
  if (ct_len > 0 && (ciphertext == nullptr || plaintext_out == nullptr)) return false;
  if (aad_len > 0 && aad == nullptr) return false;

#ifdef CSI_TEST_HOST_BUILD
  /* Verify tag BEFORE decrypting to avoid releasing forged plaintext. */
  uint8_t computed_tag[AEAD_TAG_LEN];
  shim_aead_tag(key, nonce, aad, aad_len, ciphertext, ct_len, computed_tag);
  if (!ct_equal(computed_tag, tag, AEAD_TAG_LEN)) {
    if (ct_len > 0) memset(plaintext_out, 0, ct_len);
    return false;
  }
  if (ct_len > 0) {
    uint8_t* stream = (uint8_t*)malloc(ct_len);
    if (stream == nullptr) return false;
    shim_aead_keystream(key, nonce, stream, ct_len);
    for (size_t i = 0; i < ct_len; ++i) plaintext_out[i] = ciphertext[i] ^ stream[i];
    memset(stream, 0, ct_len);
    free(stream);
  }
  return true;
#else
  ChaChaPoly chacha;
  if (!chacha.setKey(key, AEAD_KEY_LEN) || !chacha.setIV(nonce, AEAD_NONCE_LEN)) {
    chacha.clear();
    return false;
  }
  if (aad_len > 0) chacha.addAuthData(aad, aad_len);
  if (ct_len > 0) chacha.decrypt(plaintext_out, ciphertext, ct_len);
  const bool tag_ok = chacha.checkTag(tag, AEAD_TAG_LEN);
  chacha.clear();
  if (!tag_ok) {
    if (ct_len > 0) secure_zero(plaintext_out, ct_len);
    return false;
  }
  return true;
#endif
}

void aead_generate_nonce(uint8_t nonce_out[AEAD_NONCE_LEN]) {
  if (nonce_out == nullptr) return;
#ifdef CSI_TEST_HOST_BUILD
  for (size_t i = 0; i < AEAD_NONCE_LEN; ++i) nonce_out[i] = (uint8_t)(rand() & 0xFF);
#else
  esp_fill_random(nonce_out, AEAD_NONCE_LEN);
#endif
}

/* ──────────────────────────────────────────────────────────────────────────
 * BACKEND IDENTITY
 * ────────────────────────────────────────────────────────────────────────── */

Backend active_backend() {
#ifdef CSI_TEST_HOST_BUILD
  return Backend::HOST_TEST_SHIM;
#else
  return Backend::PRODUCTION;
#endif
}

}  /* namespace mesh_crypto */
