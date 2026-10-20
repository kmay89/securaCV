// Host stand-in for mbedTLS's md.h: only what common/network/fleet_key.h's
// device branch (#if defined(ARDUINO)) names, so the host suites can compile
// that branch and define the two functions themselves (hmac_fail_seam.h).
// The declarations, enumerators and error codes are mbedTLS 2.28's, the
// version the release core (Arduino-ESP32 2.0.17) ships.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBEDTLS_ERR_MD_BAD_INPUT_DATA -0x5100
#define MBEDTLS_ERR_MD_ALLOC_FAILED   -0x5180

typedef enum {
  MBEDTLS_MD_NONE = 0,
  MBEDTLS_MD_MD2,
  MBEDTLS_MD_MD4,
  MBEDTLS_MD_MD5,
  MBEDTLS_MD_SHA1,
  MBEDTLS_MD_SHA224,
  MBEDTLS_MD_SHA256,
  MBEDTLS_MD_SHA384,
  MBEDTLS_MD_SHA512,
  MBEDTLS_MD_RIPEMD160,
} mbedtls_md_type_t;

typedef struct mbedtls_md_info_t mbedtls_md_info_t;

const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t md_type);

int mbedtls_md_hmac(const mbedtls_md_info_t* md_info, const unsigned char* key, size_t keylen,
                    const unsigned char* input, size_t ilen, unsigned char* output);

#ifdef __cplusplus
}
#endif
