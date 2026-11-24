/* mbedtls/aes.h for test_household_rpa.cpp: the calls household.cpp's ah()
 * makes (mbedtls_aes_init / setkey_enc / crypt_ecb / free), answered as
 * mbedTLS answers them, over OpenSSL's AES-128-ECB (EVP, link -lcrypto):
 * the key is the 16 bytes as given, the block one encryption with no
 * padding. fail_setkey and fail_crypt make those calls refuse (a nonzero
 * answer), so the suite can hold ah()'s refusal to a slot that does not
 * match. */
#ifndef STUB_HOUSEHOLD_MBEDTLS_AES_H
#define STUB_HOUSEHOLD_MBEDTLS_AES_H

#include <openssl/evp.h>
#include <string.h>

#define MBEDTLS_AES_ENCRYPT 1
#define MBEDTLS_AES_DECRYPT 0

typedef struct mbedtls_aes_context {
  unsigned char key[16];
  int           keyed;
} mbedtls_aes_context;

inline bool g_mbedtls_aes_fail_setkey = false;
inline bool g_mbedtls_aes_fail_crypt  = false;
inline int  g_mbedtls_aes_encryptions = 0;

inline void mbedtls_aes_init(mbedtls_aes_context* ctx) { memset(ctx, 0, sizeof(*ctx)); }
inline void mbedtls_aes_free(mbedtls_aes_context* ctx) { memset(ctx, 0, sizeof(*ctx)); }

inline int mbedtls_aes_setkey_enc(mbedtls_aes_context* ctx, const unsigned char* key,
                                  unsigned int keybits) {
  if (g_mbedtls_aes_fail_setkey || keybits != 128) return -0x0020;  // INVALID_KEY_LENGTH
  memcpy(ctx->key, key, sizeof(ctx->key));
  ctx->keyed = 1;
  return 0;
}

inline int mbedtls_aes_crypt_ecb(mbedtls_aes_context* ctx, int mode,
                                 const unsigned char input[16], unsigned char output[16]) {
  if (g_mbedtls_aes_fail_crypt || !ctx->keyed || mode != MBEDTLS_AES_ENCRYPT) return -0x0021;
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  int len = 0, fin = 0;
  const bool ok = c != nullptr &&
                  EVP_EncryptInit_ex(c, EVP_aes_128_ecb(), nullptr, ctx->key, nullptr) == 1 &&
                  EVP_CIPHER_CTX_set_padding(c, 0) == 1 &&
                  EVP_EncryptUpdate(c, output, &len, input, 16) == 1 &&
                  EVP_EncryptFinal_ex(c, output + len, &fin) == 1 && len + fin == 16;
  EVP_CIPHER_CTX_free(c);
  if (!ok) return -0x0021;
  g_mbedtls_aes_encryptions++;
  return 0;
}

#endif
