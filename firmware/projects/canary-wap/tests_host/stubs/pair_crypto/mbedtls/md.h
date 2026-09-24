/* Host stand-in for the slice of mbedTLS's md.h mesh_pair_crypto.h uses
 * (canary-wap host tests only): the SHA-256 selector, nothing else. */
#ifndef STUB_PAIR_CRYPTO_MBEDTLS_MD_H
#define STUB_PAIR_CRYPTO_MBEDTLS_MD_H

typedef enum { MBEDTLS_MD_NONE = 0, MBEDTLS_MD_SHA256 = 6 } mbedtls_md_type_t;
typedef struct mbedtls_md_info_t { mbedtls_md_type_t type; } mbedtls_md_info_t;

inline const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t t) {
  static const mbedtls_md_info_t sha256 = { MBEDTLS_MD_SHA256 };
  return t == MBEDTLS_MD_SHA256 ? &sha256 : nullptr;
}

#endif
