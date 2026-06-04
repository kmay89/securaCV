#include "device_pseudonym.h"

#include <string.h>

#if defined(ARDUINO)
  #include "mbedtls/sha256.h"
  #include <esp_random.h>   // esp_fill_random — note: NO esp_mac.h (see device_id_hex)
  #include <esp_system.h>
  #include "nvs_store.h"   // pulls in Arduino.h / Preferences.h
#else
  #include <openssl/sha.h>
#endif

namespace device_pseudonym {
namespace {

constexpr char   DOMAIN[]   = "canary:device-id:v1:";
constexpr size_t DOMAIN_LEN = sizeof(DOMAIN) - 1;  // exclude NUL

// Unambiguous alphabet — drops every case variant of the glyph-confusion
// classes (0/O/o, 1/I/i/l/L). Must stay byte-identical to the shared header
// (firmware/common/identity/device_pseudonym.h) so both trees derive the same
// token, and to the project-wide UNAMBIGUOUS_ALPHABET in securacv_crypto.
constexpr char     ALPHABET[]     = "23456789ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz";
constexpr size_t   ALPHABET_LEN   = sizeof(ALPHABET) - 1;  // 54
constexpr unsigned ALPHABET_LIMIT = 216;                   // 54*4: rejection bound (unbiased)

// Reliable scrub: a plain memset() at end-of-scope is frequently removed by the
// compiler via dead-store elimination. The volatile pointer prevents that.
void secure_zero(void* p, size_t n) {
  volatile uint8_t* vp = static_cast<volatile uint8_t*>(p);
  while (n--) *vp++ = 0;
}

void sha256_raw(const uint8_t* in, size_t len, uint8_t out[32]) {
#if defined(ARDUINO)
  mbedtls_sha256(in, len, out, 0);  // 0 => SHA-256 (not SHA-224)
#else
  SHA256(in, len, out);
#endif
}

} // namespace

bool derive(const uint8_t* secret, size_t secret_len,
            char* out_hex, size_t out_len) {
  if (secret == nullptr || out_hex == nullptr) return false;
  if (secret_len == 0 || secret_len > 64) return false;  // bound the stack buffer
  if (out_len < HEX_LEN + 1) return false;

  // input = DOMAIN || secret  (domain separation + per-device random salt).
  // No hardware MAC is mixed in: the 256-bit salt alone makes the token unique
  // and stable, so the derivation never touches a network-trackable identifier.
  uint8_t input[DOMAIN_LEN + 64];
  size_t off = 0;
  memcpy(input + off, DOMAIN, DOMAIN_LEN); off += DOMAIN_LEN;
  memcpy(input + off, secret, secret_len); off += secret_len;

  uint8_t hash[32];
  sha256_raw(input, off, hash);

  // Render the hash in the unambiguous alphabet. Rejection-sample to drop
  // modular bias (~15.6% of bytes discarded), so the 32 hash bytes comfortably
  // yield HEX_LEN chars; the vanishingly-unlikely shortfall tops up
  // deterministically so the token is always full-length and stable.
  size_t produced = 0;
  for (size_t i = 0; i < sizeof(hash) && produced < HEX_LEN; i++) {
    if (hash[i] < ALPHABET_LIMIT) {
      out_hex[produced++] = ALPHABET[hash[i] % ALPHABET_LEN];
    }
  }
  static_assert(HEX_LEN <= ALPHABET_LEN,
                "filler indexes the alphabet directly; HEX_LEN must fit");
  while (produced < HEX_LEN) {
    out_hex[produced] = ALPHABET[produced];
    produced++;
  }
  out_hex[HEX_LEN] = '\0';

  // Scrub sensitive intermediates (volatile, not optimized away).
  secure_zero(input, sizeof(input));
  secure_zero(hash, sizeof(hash));
  return true;
}

#if defined(ARDUINO)

bool device_id_hex(char* out_hex, size_t out_len) {
  if (out_hex == nullptr || out_len < HEX_LEN + 1) return false;

  // The per-device NVS salt is fixed for the device's lifetime, so the pseudonym
  // is computed once and cached in RAM. This keeps the Hardware ID stable for the
  // whole boot session even if NVS persistence fails, and avoids an NVS read (and
  // possible write) on every status/info request.
  static char cached[HEX_LEN + 1] = {0};
  static bool cached_valid = false;
  if (cached_valid) {
    memcpy(out_hex, cached, HEX_LEN + 1);
    return true;
  }

  // Load-or-create a per-device random salt in NVS (independent of other subsystems
  // so it works regardless of init order). The 256-bit salt alone makes the token
  // unique and stable; we deliberately do NOT read the hardware MAC, so no
  // network-trackable identifier ever enters this derivation. If persistence fails we
  // still derive from the freshly generated salt and cache it for session consistency.
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

  // Derive into a temp buffer; only publish on success so out_hex is never left partial.
  char tmp[HEX_LEN + 1];
  bool ok = derive(salt, sizeof(salt), tmp, sizeof(tmp));
  if (ok) {
    memcpy(cached, tmp, HEX_LEN + 1);
    cached_valid = true;
    memcpy(out_hex, tmp, HEX_LEN + 1);
  }

  secure_zero(salt, sizeof(salt));
  return ok;
}

#endif  // ARDUINO

} // namespace device_pseudonym
