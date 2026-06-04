#pragma once
//
// device_pseudonym (shared) — stable, non-reversible device identifier for
// operator-facing diagnostics (serial logs, MQTT client IDs, boot banners).
//
// Privacy (Invariant III / F-03): firmware MUST NOT surface the raw hardware MAC.
// This derives a stable pseudonymous token from a per-device random salt persisted
// in NVS — no hardware MAC is ever read — so diagnostics can still show a stable
// handle without leaking a network-trackable identifier.
//
// Header-only so any firmware tree adopts it with a single include (no
// build_src_filter wiring), and the pure derive() compiles unchanged in the host
// test harness. The construction mirrors the canary-wap arduino device_pseudonym;
// the on-device backend is mbedTLS SHA-256, the host backend is OpenSSL.
//
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(ARDUINO)
  #include "mbedtls/sha256.h"
  #include <esp_random.h>   // esp_fill_random — deliberately NO esp_mac.h (no MAC read)
  #include <Preferences.h>
#else
  #include <openssl/sha.h>
#endif

namespace device_pseudonym {

constexpr size_t SECRET_LEN  = 32;               // per-device salt length
constexpr size_t TOKEN_BYTES = 8;                // SHA-256 bytes compared in tests
constexpr size_t HEX_LEN     = TOKEN_BYTES * 2;  // 16-char token (excl. NUL); name
                                                 // kept for caller-buffer compatibility

namespace detail {

constexpr char   DOMAIN[]    = "canary:device-id:v1:";
constexpr size_t DOMAIN_LEN  = sizeof(DOMAIN) - 1;  // exclude NUL

// Unambiguous alphabet — drops every case variant of the glyph-confusion
// classes (0/O/o, 1/I/i/l/L) so the diagnostic handle is safe to read off a
// screen or aloud. Same 54-char set the API token / WiFi password / device_id
// use, so "no confusing characters" holds for every human-facing identifier.
constexpr char     ALPHABET[]     = "23456789ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz";
constexpr size_t   ALPHABET_LEN   = sizeof(ALPHABET) - 1;  // 54
constexpr unsigned ALPHABET_LIMIT = 216;                   // 54*4: rejection bound (unbiased)

// Reliable scrub: a plain memset() at end-of-scope is frequently removed by the
// compiler via dead-store elimination. The volatile pointer prevents that.
inline void secure_zero(void* p, size_t n) {
  volatile uint8_t* vp = static_cast<volatile uint8_t*>(p);
  while (n--) *vp++ = 0;
}

inline void sha256_raw(const uint8_t* in, size_t len, uint8_t out[32]) {
#if defined(ARDUINO)
  mbedtls_sha256(in, len, out, 0);  // 0 => SHA-256 (not SHA-224)
#else
  SHA256(in, len, out);
#endif
}

}  // namespace detail

// Pure, non-reversible derivation:
//   token = SHA256("canary:device-id:v1:" || secret), rendered as HEX_LEN chars
//           of the unambiguous alphabet (no 0/O/o, 1/I/i/l/L).
// Stable for a fixed secret; the raw secret cannot be recovered from the output.
// Writes HEX_LEN+1 bytes (incl. NUL) into out_hex. Returns false on bad args/buffer.
inline bool derive(const uint8_t* secret, size_t secret_len,
                   char* out_hex, size_t out_len) {
  if (secret == nullptr || out_hex == nullptr) return false;
  if (secret_len == 0 || secret_len > 64) return false;  // bound the stack buffer
  if (out_len < HEX_LEN + 1) return false;

  // input = DOMAIN || secret  (domain separation + per-device random salt).
  // No hardware MAC is mixed in: the 256-bit salt alone makes the token unique and
  // stable, so the derivation never touches a network-trackable identifier.
  uint8_t input[detail::DOMAIN_LEN + 64];
  size_t off = 0;
  memcpy(input + off, detail::DOMAIN, detail::DOMAIN_LEN); off += detail::DOMAIN_LEN;
  memcpy(input + off, secret, secret_len);                off += secret_len;

  uint8_t hash[32];
  detail::sha256_raw(input, off, hash);

  // Render the hash in the unambiguous alphabet. Rejection-sample to drop
  // modular bias (~15.6% of bytes discarded), so the 32 hash bytes comfortably
  // yield HEX_LEN chars; the vanishingly-unlikely shortfall tops up
  // deterministically so the token is always full-length and stable.
  size_t produced = 0;
  for (size_t i = 0; i < sizeof(hash) && produced < HEX_LEN; i++) {
    if (hash[i] < detail::ALPHABET_LIMIT) {
      out_hex[produced++] = detail::ALPHABET[hash[i] % detail::ALPHABET_LEN];
    }
  }
  static_assert(HEX_LEN <= detail::ALPHABET_LEN,
                "filler indexes the alphabet directly; HEX_LEN must fit");
  while (produced < HEX_LEN) {
    out_hex[produced] = detail::ALPHABET[produced];
    produced++;
  }
  out_hex[HEX_LEN] = '\0';

  detail::secure_zero(input, sizeof(input));
  detail::secure_zero(hash, sizeof(hash));
  return true;
}

#if defined(ARDUINO)
// Device-side convenience: lazily load-or-create a per-device random salt in NVS
// (Arduino Preferences, namespace "securacv_id", key "id_salt") and write the
// pseudonym token. Reads no hardware MAC; never exposes a trackable ID. The salt is
// fixed for the device's lifetime, so the result is computed once and cached in RAM.
//
// This is a single `inline` function with external linkage, so its `cached` static
// is ONE object shared across every translation unit that includes this header
// (guaranteed by [dcl.fct.spec]). The first caller in a boot populates it and all
// later callers — e.g. the boot banner and the MQTT client ID — return the same
// value, including on the NVS-unavailable fallback path. The handle never diverges
// by caller within a boot.
inline bool device_id_hex(char* out_hex, size_t out_len) {
  if (out_hex == nullptr || out_len < HEX_LEN + 1) return false;

  static char cached[HEX_LEN + 1] = {0};
  static bool cached_valid = false;
  if (cached_valid) {
    memcpy(out_hex, cached, HEX_LEN + 1);
    return true;
  }

  uint8_t salt[SECRET_LEN];
  bool have = false;

  Preferences prefs;
  if (prefs.begin("securacv_id", /*readOnly=*/false)) {
    if (prefs.getBytesLength("id_salt") == SECRET_LEN) {
      have = (prefs.getBytes("id_salt", salt, SECRET_LEN) == SECRET_LEN);
    }
    bool nonzero = false;
    if (have) {
      for (size_t i = 0; i < SECRET_LEN; i++) {
        if (salt[i] != 0) { nonzero = true; break; }
      }
    }
    if (!have || !nonzero) {
      esp_fill_random(salt, sizeof(salt));
      prefs.putBytes("id_salt", salt, sizeof(salt));  // best-effort persist
    }
    prefs.end();
  } else {
    // NVS unavailable: derive from a fresh salt so the boot session still has a
    // stable handle (it just won't survive a reboot). Still no MAC is read.
    esp_fill_random(salt, sizeof(salt));
  }

  char tmp[HEX_LEN + 1];
  bool ok = derive(salt, sizeof(salt), tmp, sizeof(tmp));
  if (ok) {
    memcpy(cached, tmp, HEX_LEN + 1);
    cached_valid = true;
    memcpy(out_hex, tmp, HEX_LEN + 1);
  }

  detail::secure_zero(salt, sizeof(salt));
  return ok;
}
#endif  // ARDUINO

}  // namespace device_pseudonym
