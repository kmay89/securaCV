/**
 * @file witness_chain.h
 * @brief THE canonical witness hash-chain construction, header-only.
 *
 * Every SecuraCV firmware that chains witness records uses this exact
 * byte layout — it is what the off-device verifier
 * (tools/verify_witness_log.py) recomputes, and what the canary-wap
 * /WITNESS/records.jsonl golden tests pin:
 *
 *   payload_hash = SHA256(domain_payload || 0x00 || payload_bytes)
 *   chain_hash   = SHA256(domain_chain   || 0x00 ||
 *                         prev(32) || payload_hash(32) ||
 *                         seq(4, big-endian) || time_bucket(4, BE))
 *   genesis      = SHA256(domain_genesis || 0x00 || device_id_utf8)
 *
 * seq is the monotonic record counter; time_bucket is the firmware's
 * coarsened time value (canary-wap: millis()/TIME_BUCKET_MS;
 * canary-sense: the 10-minute uptime bucket in seconds). Both are IN the
 * hash so a record cannot be silently renumbered or time-shifted.
 *
 * History: this header used to DECLARE a full witness-store C API that
 * was never implemented anywhere (no .cpp ever existed), and the
 * per-firmware implementations diverged in the meantime (canary-sense
 * omitted seq and time_bucket from its chain hash). It is now the small,
 * real, shared core: the pure byte-layout builder plus mbedTLS-backed
 * helpers, so there is exactly one definition of the construction.
 *
 * The pure builder (wc_chain_buf) has no dependencies and is host-tested
 * (firmware/tests_host/test_witness_chain_core.cpp); the hashing helpers
 * compile wherever mbedTLS is available (all ESP32 targets, Arduino core
 * 2.x and 3.x — mbedtls_sha256_* return values are cast away for
 * cross-core compatibility, same as device_signature.cpp).
 *
 * Domain-string note: the kernel uses "securacv:pwk:*:v2" contexts;
 * firmware uses "securacv:fw:*:v1". The divergence is intentional —
 * it prevents cross-context signature reuse.
 */

#ifndef SECURACV_WITNESS_CHAIN_H
#define SECURACV_WITNESS_CHAIN_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Domain-separation strings — shared verbatim by canary-wap
 * (canary_wap.ino), canary-sense (witness.cpp), the securacv_witness
 * library, and tools/verify_witness_log.py. Never reuse one for a new
 * purpose; add a new versioned domain instead. */
#define WC_DOMAIN_CHAIN   "securacv:fw:chain:v1"
#define WC_DOMAIN_PAYLOAD "securacv:fw:payload:v1"
#define WC_DOMAIN_BOOT    "securacv:fw:boot:v1"
#define WC_DOMAIN_GENESIS "securacv:genesis:v1"

#define WC_HASH_SIZE 32
#define WC_CHAIN_BUF_SIZE 72 /* prev(32) + payload_hash(32) + seq(4) + tb(4) */

/**
 * Fill the exact 72-byte pre-image the chain hash is computed over.
 * Pure — no crypto, no platform dependencies — so the byte layout itself
 * is host-testable. Returns WC_CHAIN_BUF_SIZE.
 */
static inline size_t wc_chain_buf(const uint8_t prev[WC_HASH_SIZE],
                                  const uint8_t payload_hash[WC_HASH_SIZE],
                                  uint32_t seq, uint32_t time_bucket,
                                  uint8_t out[WC_CHAIN_BUF_SIZE]) {
  memcpy(out, prev, WC_HASH_SIZE);
  memcpy(out + WC_HASH_SIZE, payload_hash, WC_HASH_SIZE);
  out[64] = (uint8_t)(seq >> 24);
  out[65] = (uint8_t)(seq >> 16);
  out[66] = (uint8_t)(seq >> 8);
  out[67] = (uint8_t)(seq);
  out[68] = (uint8_t)(time_bucket >> 24);
  out[69] = (uint8_t)(time_bucket >> 16);
  out[70] = (uint8_t)(time_bucket >> 8);
  out[71] = (uint8_t)(time_bucket);
  return WC_CHAIN_BUF_SIZE;
}

#if defined(__has_include)
#if __has_include(<mbedtls/sha256.h>)
#include <mbedtls/sha256.h>
#define WC_HAS_MBEDTLS 1
#endif
#endif

#ifdef WC_HAS_MBEDTLS

/** SHA256(domain_ascii || 0x00 || data) — the house domain hash. */
static inline void wc_sha256_domain(const char* domain, const uint8_t* data,
                                    size_t len, uint8_t out[WC_HASH_SIZE]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  /* Casts: mbedtls_sha256_* returns int on IDF5/core-3.x, void on
   * core-2.x — the cast compiles on both (device_signature.cpp does the
   * same). */
  (void)mbedtls_sha256_starts(&ctx, 0);
  (void)mbedtls_sha256_update(&ctx, (const unsigned char*)domain,
                              strlen(domain));
  const unsigned char wc_sep = 0x00;
  (void)mbedtls_sha256_update(&ctx, &wc_sep, 1);
  (void)mbedtls_sha256_update(&ctx, data, len);
  (void)mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

/** One canonical chain step: out = H(chain domain, prev||ph||seq||tb). */
static inline void wc_chain_advance(const uint8_t prev[WC_HASH_SIZE],
                                    const uint8_t payload_hash[WC_HASH_SIZE],
                                    uint32_t seq, uint32_t time_bucket,
                                    uint8_t out[WC_HASH_SIZE]) {
  uint8_t buf[WC_CHAIN_BUF_SIZE];
  wc_chain_buf(prev, payload_hash, seq, time_bucket, buf);
  wc_sha256_domain(WC_DOMAIN_CHAIN, buf, sizeof(buf), out);
}

/** Canonical genesis head, bound to the device's stable id. */
static inline void wc_genesis(const char* device_id,
                              uint8_t out[WC_HASH_SIZE]) {
  wc_sha256_domain(WC_DOMAIN_GENESIS, (const uint8_t*)device_id,
                   strlen(device_id), out);
}

#endif /* WC_HAS_MBEDTLS */

#endif /* SECURACV_WITNESS_CHAIN_H */
