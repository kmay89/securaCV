/*
 * SecuraCV Canary-WAP — mesh_crypto shim header
 *
 * canary-wap's mesh code lives in mesh_network.cpp as one big file with
 * an internal `static sha256_domain()`. The BLE Scout primitives ported
 * from firmware/canary/lib/securacv_ble_scan call `mesh_crypto::sha256_domain`
 * (the namespaced PIO entry point). This shim provides that namespace
 * locally so the same ble_scan.cpp compiles byte-identical in both
 * builds without forcing canary-wap to refactor mesh_network.cpp.
 *
 * Implementation: thin wrapper around mbedtls/sha256 (the same backend
 * mesh_network's static uses). Domain-separation pattern matches the
 * canary lib version: SHA-256("<domain>" || data).
 */

#ifndef SECURACV_CANARY_WAP_MESH_CRYPTO_H
#define SECURACV_CANARY_WAP_MESH_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

namespace mesh_crypto {

constexpr size_t SHA256_OUT_LEN = 32;

/* Domain-separated SHA-256. Mirrors firmware/canary/lib/securacv_mesh's
 * mesh_crypto::sha256_domain exactly: hashes ("<domain>" || data) into
 * out[32]. The domain string is treated as a C string up to its NUL,
 * matching the canonical implementation. */
void sha256_domain(const char* domain,
                   const uint8_t* data,
                   size_t data_len,
                   uint8_t out[SHA256_OUT_LEN]);

}  /* namespace mesh_crypto */

#endif  /* SECURACV_CANARY_WAP_MESH_CRYPTO_H */
