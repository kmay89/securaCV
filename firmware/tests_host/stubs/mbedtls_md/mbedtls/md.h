// Host stand-in for mbedTLS's md.h: only what two device HMACs name, so the
// host suites can compile them and define the functions themselves.
//   - common/network/fleet_key.h's device branch (#if defined(ARDUINO)): the
//     one-shot mbedtls_md_hmac() and mbedtls_md_info_from_type(), defined by
//     hmac_fail_seam.h;
//   - canary-wap's hmac_sha256() (canary_wap.ino, sweep F359): the streaming
//     context, mbedtls_md_init/_setup/_hmac_starts/_hmac_update/_hmac_finish/
//     _free, defined by
//     firmware/projects/canary-wap/tests_host/test_wap_hmac_fail.cpp.
// The declarations, enumerators and error codes are mbedTLS 2.28's, the
// version the release core (Arduino-ESP32 2.0.17) ships; the functions keep
// these signatures in mbedTLS 3.x (core 3.x), which only makes the context's
// fields private.
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

typedef struct mbedtls_md_context_t {
  const mbedtls_md_info_t* md_info;
  void* md_ctx;
  void* hmac_ctx;
} mbedtls_md_context_t;

const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t md_type);

int mbedtls_md_hmac(const mbedtls_md_info_t* md_info, const unsigned char* key, size_t keylen,
                    const unsigned char* input, size_t ilen, unsigned char* output);

void mbedtls_md_init(mbedtls_md_context_t* ctx);
void mbedtls_md_free(mbedtls_md_context_t* ctx);
int mbedtls_md_setup(mbedtls_md_context_t* ctx, const mbedtls_md_info_t* md_info, int hmac);
int mbedtls_md_hmac_starts(mbedtls_md_context_t* ctx, const unsigned char* key, size_t keylen);
int mbedtls_md_hmac_update(mbedtls_md_context_t* ctx, const unsigned char* input, size_t ilen);
int mbedtls_md_hmac_finish(mbedtls_md_context_t* ctx, unsigned char* output);

#ifdef __cplusplus
}
#endif
