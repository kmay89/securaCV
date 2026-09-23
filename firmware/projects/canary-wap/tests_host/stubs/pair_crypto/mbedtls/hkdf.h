/* Host stand-in for mbedTLS's HKDF (RFC 5869) — canary-wap host tests only,
 * built on OpenSSL's HMAC-SHA256 so the key derivation under test is real.
 * Same contract as mbedtls_hkdf(): an absent salt is HashLen zero bytes;
 * returns 0 on success. */
#ifndef STUB_PAIR_CRYPTO_MBEDTLS_HKDF_H
#define STUB_PAIR_CRYPTO_MBEDTLS_HKDF_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "md.h"

inline int mbedtls_hkdf(const mbedtls_md_info_t* md,
                        const unsigned char* salt, size_t salt_len,
                        const unsigned char* ikm, size_t ikm_len,
                        const unsigned char* info, size_t info_len,
                        unsigned char* okm, size_t okm_len) {
  if (md == nullptr || md->type != MBEDTLS_MD_SHA256 || okm_len > 255 * 32) return -1;
  const unsigned char zeros[32] = {0};
  if (salt == nullptr || salt_len == 0) { salt = zeros; salt_len = sizeof(zeros); }
  unsigned char prk[32];
  unsigned int prk_len = 0;
  if (HMAC(EVP_sha256(), salt, (int)salt_len, ikm, ikm_len, prk, &prk_len) == nullptr) return -1;
  unsigned char t[32];
  size_t t_len = 0, done = 0;
  for (unsigned char n = 1; done < okm_len; ++n) {
    unsigned char buf[32 + 256 + 1];
    if (info_len > 256) return -1;
    memcpy(buf, t, t_len);
    if (info_len > 0) memcpy(buf + t_len, info, info_len);
    buf[t_len + info_len] = n;
    unsigned int out_len = 0;
    if (HMAC(EVP_sha256(), prk, (int)prk_len, buf, t_len + info_len + 1, t, &out_len) == nullptr) return -1;
    t_len = out_len;
    const size_t take = (okm_len - done < t_len) ? okm_len - done : t_len;
    memcpy(okm + done, t, take);
    done += take;
  }
  return 0;
}

#endif
