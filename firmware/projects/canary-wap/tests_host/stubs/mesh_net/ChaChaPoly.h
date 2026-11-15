/* Host stand-in for rweather/Crypto's ChaChaPoly — OpenSSL's
 * ChaCha20-Poly1305 with a 12-byte IV, used the way mesh_network.cpp uses
 * the class: setKey, setIV, one encrypt or decrypt, then computeTag or
 * checkTag — and addAuthData between setIV and the cipher call, as
 * api_seal.h uses it (the bytes are kept and fed to OpenSSL when the
 * cipher starts, since the direction is not known until then). */
#ifndef STUB_MESH_NET_CHACHAPOLY_H
#define STUB_MESH_NET_CHACHAPOLY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <openssl/evp.h>

#include <vector>

class ChaChaPoly {
 public:
  ~ChaChaPoly() { clear(); }
  bool setKey(const uint8_t* key, size_t len) {
    if (len != 32) return false;
    memcpy(key_, key, 32);
    return true;
  }
  bool setIV(const uint8_t* iv, size_t len) {
    if (len != 12) return false;
    memcpy(iv_, iv, 12);
    return true;
  }
  void addAuthData(const void* data, size_t len) {
    aad_.insert(aad_.end(), (const uint8_t*)data, (const uint8_t*)data + len);
  }
  void encrypt(uint8_t* out, const uint8_t* in, size_t len) {
    start(true);
    int n = 0;
    EVP_EncryptUpdate(ctx_, out, &n, in, (int)len);
  }
  void decrypt(uint8_t* out, const uint8_t* in, size_t len) {
    start(false);
    int n = 0;
    EVP_DecryptUpdate(ctx_, out, &n, in, (int)len);
  }
  void computeTag(void* tag, size_t len) {
    uint8_t tail[16];
    int n = 0;
    EVP_EncryptFinal_ex(ctx_, tail, &n);
    EVP_CIPHER_CTX_ctrl(ctx_, EVP_CTRL_AEAD_GET_TAG, (int)len, tag);
  }
  bool checkTag(const void* tag, size_t len) {
    uint8_t t[16];
    memcpy(t, tag, len < 16 ? len : 16);
    EVP_CIPHER_CTX_ctrl(ctx_, EVP_CTRL_AEAD_SET_TAG, (int)len, t);
    uint8_t tail[16];
    int n = 0;
    return EVP_DecryptFinal_ex(ctx_, tail, &n) == 1;
  }
  void clear() {
    if (ctx_ != nullptr) EVP_CIPHER_CTX_free(ctx_);
    ctx_ = nullptr;
    memset(key_, 0, sizeof key_);
    memset(iv_, 0, sizeof iv_);
    aad_.clear();
  }
 private:
  void start(bool enc) {
    if (ctx_ != nullptr) EVP_CIPHER_CTX_free(ctx_);
    ctx_ = EVP_CIPHER_CTX_new();
    if (enc) EVP_EncryptInit_ex(ctx_, EVP_chacha20_poly1305(), nullptr, key_, iv_);
    else     EVP_DecryptInit_ex(ctx_, EVP_chacha20_poly1305(), nullptr, key_, iv_);
    if (!aad_.empty()) {
      int n = 0;
      if (enc) EVP_EncryptUpdate(ctx_, nullptr, &n, aad_.data(), (int)aad_.size());
      else     EVP_DecryptUpdate(ctx_, nullptr, &n, aad_.data(), (int)aad_.size());
    }
  }
  EVP_CIPHER_CTX* ctx_ = nullptr;
  uint8_t key_[32] = {};
  uint8_t iv_[12] = {};
  std::vector<uint8_t> aad_;
};

#endif
