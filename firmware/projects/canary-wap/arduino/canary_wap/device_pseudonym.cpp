#include "device_pseudonym.h"

#include <string.h>

#if defined(ARDUINO)
  #include "mbedtls/sha256.h"
#else
  #include <openssl/sha.h>
#endif

namespace device_pseudonym {
namespace {

constexpr char   DOMAIN[]   = "canary:device-id:v1:";
constexpr size_t DOMAIN_LEN = sizeof(DOMAIN) - 1;  // exclude NUL
constexpr char   HEXD[]     = "0123456789abcdef";

void sha256_raw(const uint8_t* in, size_t len, uint8_t out[32]) {
#if defined(ARDUINO)
  mbedtls_sha256(in, len, out, 0);  // 0 => SHA-256 (not SHA-224)
#else
  SHA256(in, len, out);
#endif
}

} // namespace

bool derive(const uint8_t* mac, size_t mac_len,
            const uint8_t* secret, size_t secret_len,
            char* out_hex, size_t out_len) {
  if (mac == nullptr || secret == nullptr || out_hex == nullptr) return false;
  if (mac_len == 0 || secret_len == 0) return false;
  if (secret_len > 64 || mac_len > 16) return false;     // bound the stack buffer
  if (out_len < HEX_LEN + 1) return false;

  // input = DOMAIN || secret || mac  (domain separation + per-device salt)
  uint8_t input[DOMAIN_LEN + 64 + 16];
  size_t off = 0;
  memcpy(input + off, DOMAIN, DOMAIN_LEN); off += DOMAIN_LEN;
  memcpy(input + off, secret, secret_len); off += secret_len;
  memcpy(input + off, mac, mac_len);       off += mac_len;

  uint8_t hash[32];
  sha256_raw(input, off, hash);

  for (size_t i = 0; i < TOKEN_BYTES; i++) {
    out_hex[i * 2]     = HEXD[(hash[i] >> 4) & 0x0F];
    out_hex[i * 2 + 1] = HEXD[hash[i] & 0x0F];
  }
  out_hex[HEX_LEN] = '\0';

  // Best-effort scrub of sensitive intermediates.
  memset(input, 0, sizeof(input));
  memset(hash, 0, sizeof(hash));
  return true;
}

#if defined(ARDUINO)

#include <esp_mac.h>
#include <esp_system.h>
#include "nvs_store.h"

bool device_id_hex(char* out_hex, size_t out_len) {
  if (out_hex == nullptr || out_len < HEX_LEN + 1) return false;

  // Load-or-create a per-device salt in NVS (independent of other subsystems so it
  // works regardless of init order).
  uint8_t salt[SECRET_LEN];
  bool have = nvs_store::get_blob(NVS_SALT_KEY, salt, sizeof(salt));
  bool nonzero = false;
  if (have) {
    for (size_t i = 0; i < sizeof(salt); i++) {
      if (salt[i] != 0) { nonzero = true; break; }
    }
  }
  if (!have || !nonzero) {
    esp_fill_random(salt, sizeof(salt));
    nvs_store::set_blob(NVS_SALT_KEY, salt, sizeof(salt));  // best-effort persist
  }

  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);

  bool ok = derive(mac, sizeof(mac), salt, sizeof(salt), out_hex, out_len);

  memset(salt, 0, sizeof(salt));
  memset(mac, 0, sizeof(mac));
  return ok;
}

#endif  // ARDUINO

} // namespace device_pseudonym
