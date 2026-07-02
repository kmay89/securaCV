/**
 * @file device_signature.cpp
 * @brief Implementation of device_signature.h — Ed25519 sigs over the
 *        outbound MQTT publish set (shared common copy).
 *
 * Crypto: Arduino Crypto's Ed25519 (rweather/arduino-cryptography). We
 * feed it the canonical message bytes; Ed25519 internally hashes with
 * SHA-512 so we don't pre-hash.
 *
 * Base64url: ESP-IDF ships mbedtls_base64_encode (standard alphabet
 * with '+/' and padding '='). We post-process to translate to the URL
 * alphabet ('-_') and strip padding — same trick HA uses on the
 * verification side, so the wire format matches.
 *
 * Ported from the canary-wap tree's proven module (see the header for
 * the sync relationship); the only addition is the `sense` canonical.
 */

#include "device_signature.h"

#ifdef ARDUINO
#include <Ed25519.h>
#endif
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifdef ARDUINO
#include <mbedtls/base64.h>
#else
/* Host-test path: vendor a tiny standard-b64 encoder so the unit test
 * doesn't pull mbedTLS. The encoded output is then re-mapped to b64url
 * by the shared post-processor below. Behavior matches mbedtls's
 * mbedtls_base64_encode for the small inputs we feed it (sig only). */
static int mbedtls_base64_encode(unsigned char* dst, size_t dst_cap,
                                 size_t* olen,
                                 const unsigned char* src, size_t slen) {
  static const char tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t need = ((slen + 2) / 3) * 4 + 1;
  if (dst_cap < need) { *olen = need; return -1; }
  size_t o = 0;
  size_t i = 0;
  while (i + 3 <= slen) {
    unsigned v = (src[i] << 16) | (src[i+1] << 8) | src[i+2];
    dst[o++] = tab[(v >> 18) & 0x3F];
    dst[o++] = tab[(v >> 12) & 0x3F];
    dst[o++] = tab[(v >>  6) & 0x3F];
    dst[o++] = tab[(v >>  0) & 0x3F];
    i += 3;
  }
  if (i < slen) {
    unsigned v = src[i] << 16;
    if (i + 1 < slen) v |= src[i+1] << 8;
    dst[o++] = tab[(v >> 18) & 0x3F];
    dst[o++] = tab[(v >> 12) & 0x3F];
    if (i + 1 < slen) {
      dst[o++] = tab[(v >> 6) & 0x3F];
      dst[o++] = '=';
    } else {
      dst[o++] = '=';
      dst[o++] = '=';
    }
  }
  dst[o] = '\0';
  *olen = o;
  return 0;
}
#endif

namespace device_signature {

namespace {

/* Cached identity — populated by init(). Heap-free; the whole module
 * is statically sized so a runtime allocation failure can't leave the
 * sig path in a half-initialized state. */
uint8_t s_priv[32]              = {};
uint8_t s_pub[32]               = {};
char    s_device_id[49]         = {};
char    s_fingerprint_hex[17]   = {};
char    s_pubkey_hex[65]        = {};
bool    s_ready                 = false;

/* Hex-encode `in_len` bytes from `in` into `out`. out_cap must be
 * >= 2*in_len + 1. Lowercase, no separators. */
void hex_encode(const uint8_t* in, size_t in_len, char* out, size_t out_cap) {
  static const char H[] = "0123456789abcdef";
  if (!out || out_cap < 2 * in_len + 1) {
    if (out && out_cap) out[0] = '\0';
    return;
  }
  for (size_t i = 0; i < in_len; ++i) {
    out[2*i]     = H[(in[i] >> 4) & 0xF];
    out[2*i + 1] = H[(in[i] >> 0) & 0xF];
  }
  out[2*in_len] = '\0';
}

/* Perform the Ed25519 sign + b64url-encode dance. Returns true on
 * success. We sign the raw canonical-message bytes (Ed25519 hashes
 * internally with SHA-512, so pre-hashing would just double work).
 *
 * Host build (no ARDUINO define) routes to a stub that returns
 * false — host tests cover the canonical builders + b64url encoder
 * directly and don't go through this path. The full sign/verify
 * round-trip is exercised on the HA side via Python's cryptography. */
bool sign_and_encode(const char* canonical, size_t canon_len,
                     char* sig_out, size_t sig_cap) {
  if (!s_ready) return false;
  if (sig_cap < SIG_B64URL_CAP) return false;

#ifdef ARDUINO
  uint8_t sig_bin[64] = {};
  Ed25519::sign(sig_bin, s_priv, s_pub,
                reinterpret_cast<const uint8_t*>(canonical), canon_len);

  size_t written = b64url_encode_nopad(sig_bin, sizeof(sig_bin),
                                       sig_out, sig_cap);
  /* SIG_B64URL_LEN is fixed for a 64-byte input; a different length
   * signals a programming error worth surfacing. */
  return written == SIG_B64URL_LEN;
#else
  (void)canonical;
  (void)canon_len;
  if (sig_cap) sig_out[0] = '\0';
  return false;
#endif
}

}  /* namespace */

void init(const uint8_t priv[32],
          const uint8_t pub[32],
          const char*   device_id,
          const char*   fingerprint_hex) {
  if (!priv || !pub || !device_id || !fingerprint_hex) {
    s_ready = false;
    return;
  }
  memcpy(s_priv, priv, sizeof(s_priv));
  memcpy(s_pub,  pub,  sizeof(s_pub));
  strncpy(s_device_id,       device_id,       sizeof(s_device_id) - 1);
  strncpy(s_fingerprint_hex, fingerprint_hex, sizeof(s_fingerprint_hex) - 1);
  s_device_id[sizeof(s_device_id) - 1]             = '\0';
  s_fingerprint_hex[sizeof(s_fingerprint_hex) - 1] = '\0';
  hex_encode(pub, 32, s_pubkey_hex, sizeof(s_pubkey_hex));
  s_ready = true;
}

const char* fingerprint_hex() { return s_fingerprint_hex; }
const char* pubkey_hex()      { return s_pubkey_hex; }
const char* device_id()       { return s_device_id; }

/* ──────────────────────────────────────────────────────────────────
 * Canonical message builders. Format is locked against
 * custom_components/securacv/signature.py reconstruction; any change
 * here must bump SCHEMA_V and land in lockstep on the HA side. */

size_t build_chain_canonical(uint32_t      length,
                             const uint8_t latest_hash_32[32],
                             const char*   device_id,
                             char*         out,
                             size_t        cap) {
  if (!out || cap == 0) return 0;
  char hash_hex[65];
  hex_encode(latest_hash_32, 32, hash_hex, sizeof(hash_hex));
  int n = snprintf(out, cap, "%s|v%d|chain|%s|%lu|%s",
                   SIG_PREFIX, SCHEMA_V,
                   device_id ? device_id : "",
                   (unsigned long)length,
                   hash_hex);
  if (n <= 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n;
}

size_t build_event_canonical(uint32_t      event_id,
                             const char*   state,
                             const char*   category_str,
                             const char*   privacy_str,
                             int           motion,
                             int           breath,
                             int           bpm,
                             const char*   device_id,
                             char*         out,
                             size_t        cap) {
  if (!out || cap == 0) return 0;
  int n = snprintf(out, cap, "%s|v%d|event|%s|%lu|%s|%s|%s|%d|%d|%d",
                   SIG_PREFIX, SCHEMA_V,
                   device_id ? device_id : "",
                   (unsigned long)event_id,
                   state ? state : "",
                   category_str ? category_str : "",
                   privacy_str ? privacy_str : "",
                   motion, breath, bpm);
  if (n <= 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n;
}

size_t build_counts_canonical(uint32_t      total,
                              const char*   device_id,
                              char*         out,
                              size_t        cap) {
  if (!out || cap == 0) return 0;
  int n = snprintf(out, cap, "%s|v%d|counts|%s|%lu",
                   SIG_PREFIX, SCHEMA_V,
                   device_id ? device_id : "",
                   (unsigned long)total);
  if (n <= 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n;
}

size_t build_sense_canonical(uint32_t    seq,
                             const char* event_name,
                             const char* presence,
                             const char* occupants,
                             const char* range,
                             uint32_t    bucket_uptime_s,
                             const char* device_id,
                             char*       out,
                             size_t      cap) {
  if (!out || cap == 0) return 0;
  int n = snprintf(out, cap, "%s|v%d|sense|%s|%lu|%s|%s|%s|%s|%lu",
                   SIG_PREFIX, SCHEMA_V,
                   device_id ? device_id : "",
                   (unsigned long)seq,
                   event_name ? event_name : "",
                   presence ? presence : "",
                   occupants ? occupants : "",
                   range ? range : "",
                   (unsigned long)bucket_uptime_s);
  if (n <= 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n;
}

/* ──────────────────────────────────────────────────────────────────
 * Public sign_*() — caller hands us scalar fields; we build the
 * canonical string + sign + encode in one call. */

bool sign_chain(uint32_t       length,
                const uint8_t  latest_hash_32[32],
                char*          sig_b64url_out,
                size_t         sig_cap) {
  char canon[256];
  size_t n = build_chain_canonical(length, latest_hash_32, s_device_id,
                                   canon, sizeof(canon));
  if (n == 0) return false;
  return sign_and_encode(canon, n, sig_b64url_out, sig_cap);
}

bool sign_event(uint32_t      event_id,
                const char*   state,
                const char*   category_str,
                const char*   privacy_str,
                int           motion,
                int           breath,
                int           bpm,
                char*         sig_b64url_out,
                size_t        sig_cap) {
  char canon[256];
  size_t n = build_event_canonical(event_id, state, category_str, privacy_str,
                                   motion, breath, bpm,
                                   s_device_id, canon, sizeof(canon));
  if (n == 0) return false;
  return sign_and_encode(canon, n, sig_b64url_out, sig_cap);
}

bool sign_counts(uint32_t total,
                 char*    sig_b64url_out,
                 size_t   sig_cap) {
  char canon[128];
  size_t n = build_counts_canonical(total, s_device_id, canon, sizeof(canon));
  if (n == 0) return false;
  return sign_and_encode(canon, n, sig_b64url_out, sig_cap);
}

bool sign_sense(uint32_t    seq,
                const char* event_name,
                const char* presence,
                const char* occupants,
                const char* range,
                uint32_t    bucket_uptime_s,
                char*       sig_b64url_out,
                size_t      sig_cap) {
  char canon[256];
  size_t n = build_sense_canonical(seq, event_name, presence, occupants,
                                   range, bucket_uptime_s,
                                   s_device_id, canon, sizeof(canon));
  if (n == 0) return false;
  return sign_and_encode(canon, n, sig_b64url_out, sig_cap);
}

/* ──────────────────────────────────────────────────────────────────
 * Base64url helper. mbedtls gives us standard b64; we post-process to
 * the URL-safe alphabet and strip '=' padding so the wire bytes match
 * Python's `base64.urlsafe_b64encode(...).rstrip(b'=')`. */

size_t b64url_encode_nopad(const uint8_t* in,
                           size_t         in_len,
                           char*          out,
                           size_t         out_cap) {
  if (!in || !out || out_cap == 0) return 0;
  /* Standard b64 needs ceil(in_len/3)*4 + 1 bytes; for 64-byte sig
   * that's 88 + 1 = 89 bytes. The b64url no-pad form is 86 chars + NUL.
   * Use a stack buffer sized for the largest input we'll ever pass
   * (sig = 64 bytes). */
  unsigned char b64_buf[96];
  size_t olen = 0;
  int rc = mbedtls_base64_encode(b64_buf, sizeof(b64_buf), &olen,
                                 in, in_len);
  if (rc != 0) return 0;
  /* Translate to b64url alphabet + strip padding. Skip '=' rather than
   * compute the trim length up front — it's two characters max for
   * the inputs we care about. */
  size_t w = 0;
  for (size_t i = 0; i < olen; ++i) {
    char c = (char)b64_buf[i];
    if (c == '=') continue;
    if (c == '+') c = '-';
    else if (c == '/') c = '_';
    if (w + 1 >= out_cap) {
      /* Truncation: clear and bail so callers never see a half-built
       * sig that would silently fail verification on the HA side. */
      out[0] = '\0';
      return 0;
    }
    out[w++] = c;
  }
  out[w] = '\0';
  return w;
}

}  /* namespace device_signature */
