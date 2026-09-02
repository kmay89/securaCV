// canary-local/emulator/shim/mbedtls/sha256.h — SHA-256 for the pseudonym.
//
// device_pseudonym.h (common/identity) hashes its salted token through
// mbedtls on-device. The emulator supplies the same entry point backed by
// a compact public-domain-style SHA-256 (validated against FIPS 180-4
// vectors in emulator/test/sha256_vectors.c (the "SHA-256 shim vectors (FIPS 180-4)" step in canary-local.yml)), so the emulated device's
// "Hardware ID" is derived exactly the way a real one derives it —
// stable, salted, MAC-free.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// mbedTLS signature: is224 = 0 selects SHA-256. Returns 0 on success.
int mbedtls_sha256(const unsigned char* input, size_t ilen,
                   unsigned char output[32], int is224);

#ifdef __cplusplus
}
#endif
