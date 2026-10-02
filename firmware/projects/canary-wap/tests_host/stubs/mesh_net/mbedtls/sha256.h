/* Host stand-in for mbedtls' SHA-256 context API — OpenSSL's SHA-256. */
#ifndef STUB_MESH_NET_MBEDTLS_SHA256_H
#define STUB_MESH_NET_MBEDTLS_SHA256_H

#include <stddef.h>

#include <openssl/evp.h>

typedef struct { EVP_MD_CTX* md; } mbedtls_sha256_context;

inline void mbedtls_sha256_init(mbedtls_sha256_context* c) { c->md = EVP_MD_CTX_new(); }
inline int mbedtls_sha256_starts(mbedtls_sha256_context* c, int /*is224*/) {
  return EVP_DigestInit_ex(c->md, EVP_sha256(), nullptr) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_update(mbedtls_sha256_context* c, const unsigned char* d, size_t n) {
  return EVP_DigestUpdate(c->md, d, n) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* c, unsigned char out[32]) {
  unsigned int n = 32;
  return EVP_DigestFinal_ex(c->md, out, &n) == 1 ? 0 : -1;
}
inline void mbedtls_sha256_free(mbedtls_sha256_context* c) { EVP_MD_CTX_free(c->md); c->md = nullptr; }

#endif
