/*
 * SecuraCV Canary-WAP — mesh_crypto shim implementation
 *
 * Wraps mbedtls SHA-256 so the ble_scan / ble_scout port from
 * firmware/canary/lib/securacv_ble_scan can call mesh_crypto::sha256_domain
 * without depending on the PIO-only securacv_mesh library layout.
 */

#include "mesh_crypto.h"

#include <string.h>
#include <mbedtls/sha256.h>

namespace mesh_crypto {

/* The canary library's secure_zero(), copied (firmware/canary/lib/
 * securacv_mesh/src/mesh_crypto.cpp): a volatile pointer and a memory
 * barrier, so the zeroing of a failed digest cannot be optimized away. */
static void secure_zero(void* ptr, size_t len) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
  while (len--) { *p++ = 0; }
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" ::: "memory");
#endif
}

bool sha256_domain(const char* domain,
                   const uint8_t* data,
                   size_t data_len,
                   uint8_t out[SHA256_OUT_LEN]) {
  if (out == nullptr) return false;
  const size_t domain_len = (domain != nullptr) ? strlen(domain) : 0;

  /* This sketch builds on arduino-esp32 3.x only (mbedTLS 3.x, where the
   * non-suffixed names return int and the `_ret` variants are gone), so
   * this is the canary library's ESP_IDF_VERSION_MAJOR >= 5 body, step for
   * step: every answer checked, a step after a failed one not run, and the
   * output zeroed and false answered when one is an error (sweep F364), so
   * it never holds a part-written digest or the stack's old bytes and no
   * caller takes 32 zero bytes for a hash. Neither check_mesh_sync.sh nor
   * check_ble_scan_sync.sh covers mesh_crypto.*, so a change to either copy
   * is made to both by hand; tests_host/test_mesh_crypto_lines.cpp runs this
   * body and the canary library's on both IDF lines through one failing
   * mbedTLS stand-in and holds their answers equal. Its callers' inputs are
   * short — ble_scan's hashed beacon id (a 30-byte domain, a 32-byte key
   * and a 6-byte MAC: 68 bytes) and mesh_pair_crypto.h's confirmation code
   * and CONFIRM hash (the domain, a 32-byte session key and, for the
   * CONFIRM, a 4-byte code) — and take the SHA engine's block mode on the
   * S3 and C3, not the DMA path the multi-KiB hashes take (the F364
   * triage), so a failure is not expected; the answer is checked so that
   * one, if it comes, is refused and not read as a hash. */
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
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
  mbedtls_sha256_free(&ctx);
  if (rc != 0) {
    secure_zero(out, SHA256_OUT_LEN);
    return false;
  }
  return true;
}

}  /* namespace mesh_crypto */
