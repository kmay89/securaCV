/* Minimal securacv_crypto stub for compiling the flagship's REAL
 * securacv_auth.cpp under test_securacv_auth.cpp: the NvsManager calls
 * auth_load_or_derive() makes, over a store that never opens (so the token
 * is derived, and its persist is the one Serial line), and a
 * derive_api_token() that writes a fixed token of the real shape ("cv_" and
 * 32 base62 characters). The real ones are securacv_crypto.cpp's, compiled
 * by CI for the device; nothing here stands in for the backoff. */
#ifndef STUB_SECURACV_AUTH_SECURACV_CRYPTO_H
#define STUB_SECURACV_AUTH_SECURACV_CRYPTO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

class NvsManager {
 public:
  static NvsManager& instance() {
    static NvsManager nvs;
    return nvs;
  }
  bool beginReadOnly() { return false; }
  bool beginReadWrite() { return false; }
  void end() {}
  size_t getBytesLength(const char*) { return 0; }
  size_t getBytes(const char*, void*, size_t) { return 0; }
  size_t putBytes(const char*, const void*, size_t) { return 0; }
};

inline const char kHostDerivedToken[] = "cv_7kX9mN2pQ4rS6tU8vW0xY2zA4bC6dE8f";

inline bool derive_api_token(const uint8_t privkey[32], char* token_str,
                             size_t token_str_len) {
  if (!privkey || !token_str || token_str_len < sizeof(kHostDerivedToken)) return false;
  memcpy(token_str, kHostDerivedToken, sizeof(kHostDerivedToken));
  return true;
}

#endif
