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

void sha256_domain(const char* domain,
                   const uint8_t* data,
                   size_t data_len,
                   uint8_t out[SHA256_OUT_LEN]) {
  if (out == nullptr) return;

  /* On arduino-esp32 2.0.x (this build) mbedtls_sha256_starts/update/finish
   * return void — same call style as beacon_channel.cpp, rf_presence.cpp,
   * ble_ota.cpp. Newer (ESP-IDF 5.x) APIs return int; this build uses the
   * older signature so we do not check return codes here. */
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  if (domain != nullptr && domain[0] != '\0') {
    mbedtls_sha256_update(&ctx,
                          reinterpret_cast<const uint8_t*>(domain),
                          strlen(domain));
  }
  if (data != nullptr && data_len > 0) {
    mbedtls_sha256_update(&ctx, data, data_len);
  }
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

}  /* namespace mesh_crypto */
