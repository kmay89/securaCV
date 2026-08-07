/**
 * @file device_signature.cpp
 * @brief Implementation of device_signature.h — Ed25519 sigs over the
 *        outbound MQTT publish set.
 *
 * Crypto: Arduino Crypto's Ed25519 (rweather/arduino-cryptography) is
 * already on the include path (used by canary_wap.ino's witness-record
 * signer). We just feed it the canonical message bytes; Ed25519
 * internally hashes with SHA-512 so we don't pre-hash.
 *
 * Base64url: ESP-IDF ships mbedtls_base64_encode (standard alphabet
 * with '+/' and padding '='). We post-process to translate to the URL
 * alphabet ('-_') and strip padding — same trick HA uses on the
 * verification side, so the wire format matches.
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
char    s_device_id[33]         = {};
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

size_t build_whoami_canonical(const char*   nonce_hex,
                              const char*   device_id,
                              char*         out,
                              size_t        cap) {
  if (!out || cap == 0) return 0;
  int n = snprintf(out, cap, "%s|v%d|whoami|%s|%s",
                   SIG_PREFIX, SCHEMA_V,
                   device_id ? device_id : "",
                   nonce_hex ? nonce_hex : "");
  if (n <= 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n;
}

bool whoami_nonce_ok(const char* nonce_hex) {
  if (!nonce_hex) return false;
  size_t len = 0;
  while (nonce_hex[len]) {
    const char c = nonce_hex[len];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    len++;
    if (len > 64) return false;
  }
  return len >= 16;
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

bool sign_whoami(const char* nonce_hex,
                 char*       sig_hex_out,
                 size_t      sig_cap) {
  if (!s_ready || !sig_hex_out || sig_cap < SIG_HEX_CAP) return false;
  /* Belt and suspenders: the HTTP handler validates before calling, but
   * this key must never sign an unvalidated nonce through ANY caller. */
  if (!whoami_nonce_ok(nonce_hex)) return false;
  char canon[160];
  size_t n = build_whoami_canonical(nonce_hex, s_device_id, canon, sizeof(canon));
  if (n == 0) return false;
#ifdef ARDUINO
  uint8_t sig_bin[64] = {};
  Ed25519::sign(sig_bin, s_priv, s_pub,
                reinterpret_cast<const uint8_t*>(canon), n);
  hex_encode(sig_bin, sizeof(sig_bin), sig_hex_out, sig_cap);
  return true;
#else
  /* Host build: the canonical + nonce gate are tested directly; the full
   * sign/verify round-trip is exercised by the Flasher's Rust verifier. */
  sig_hex_out[0] = '\0';
  return false;
#endif
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

/* ──────────────────────────────────────────────────────────────────
 * HTTP enrollment endpoints. ARDUINO-only — host tests don't link
 * esp_http_server. Both handlers serve PUBLIC data (device_id, pubkey,
 * fingerprint) so they are intentionally unauthenticated. They read
 * the cached identity that device_signature::init copied at boot, so
 * there's no race with the rest of the radio coming up.
 */
#ifdef ARDUINO
#include <esp_http_server.h>
/* esp_timer, not Arduino's millis(): this translation unit deliberately
 * stays off Arduino.h (it pulls Ed25519 from the Crypto library and
 * mbedtls directly), and the throttle below only needs a monotonic
 * clock. esp_timer_get_time() is microseconds since boot. */
#include <esp_timer.h>

namespace device_identity_api {

namespace {
/* Render the enrollment JSON body into out. Returns bytes written
 * (excluding NUL), or 0 on truncation / snprintf error. Shared by
 * both the JSON endpoint and the HTML one's "Raw payload" panel.
 * All fields come from device_signature's public accessors so this
 * handler doesn't reach into private state.
 *
 * Contract matches the canonical builders earlier in this file: a
 * 0 return means "buffer too small, output cleared" — never a
 * partial JSON the caller would mistake for a complete payload. */
size_t render_enroll_json(char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  const char* fp  = device_signature::fingerprint_hex();
  const char* pk  = device_signature::pubkey_hex();
  const char* did = device_signature::device_id();
  const int n = snprintf(out, cap,
    "{"
      "\"device_id\":\"%s\","
      "\"pubkey_hex\":\"%s\","
      "\"fingerprint_hex\":\"%s\","
      "\"alg\":\"%s\","
      "\"v\":%d"
    "}",
    did ? did : "",
    pk  ? pk  : "",
    fp  ? fp  : "",
    device_signature::ALG_NAME,
    device_signature::SCHEMA_V);
  if (n <= 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n;
}
}  /* namespace */

esp_err_t handle_enroll_json(httpd_req_t* req) {
  /* Optional ?nonce=<16-64 lowercase hex>: the identity card gains a
   * PROOF — an Ed25519 signature over the whoami canonical binding this
   * device's key to the caller's fresh nonce. This is what turns "an
   * mDNS answer claimed to be canary_X" into "canary_X answered": the
   * card's static fields are copyable by anyone on the LAN, the
   * signature is not. Unauthenticated on purpose, like the card itself
   * — the proof only ever discloses PUBLIC identity, and the strict
   * nonce gate (whoami_nonce_ok) plus the fixed canonical prefix mean
   * the key never signs attacker-shaped bytes. */
  char nonce[80] = {0};
  bool with_proof = false;
  {
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "nonce", nonce, sizeof(nonce)) == ESP_OK &&
        nonce[0] != '\0') {
      if (!device_signature::whoami_nonce_ok(nonce)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req,
          "{\"error\":\"bad_nonce\",\"hint\":\"16-64 lowercase hex chars\"}");
        return ESP_OK;
      }
      /* A signing endpoint with no auth deserves its own throttle, even
       * a cheap one: at most ~4 proofs a second, self-contained so this
       * file stays free of the api_auth stack. Discovery flows ask once
       * per device per session; only a hammering client ever sees 429. */
      static uint32_t s_last_proof_ms = 0;
      const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
      if (s_last_proof_ms != 0 && (now - s_last_proof_ms) < 250) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_sendstr(req, "{\"error\":\"slow_down\"}");
        return ESP_OK;
      }
      s_last_proof_ms = now;
      with_proof = true;
    }
  }

  char body[608];
  size_t n = render_enroll_json(body, sizeof(body));
  if (n == 0 || n >= sizeof(body)) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  if (with_proof) {
    char sig_hex[device_signature::SIG_HEX_CAP];
    if (!device_signature::sign_whoami(nonce, sig_hex, sizeof(sig_hex))) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    /* Splice the proof fields in before the closing brace — the base
     * render stays byte-identical for proof-less callers (HA's TOFU
     * flow), and a truncation here clears the body like every other
     * builder in this file. */
    const int m = snprintf(body + (n - 1), sizeof(body) - (n - 1),
                           ",\"nonce\":\"%s\",\"sig_hex\":\"%s\"}",
                           nonce, sig_hex);
    if (m <= 0 || (size_t)m >= sizeof(body) - (n - 1)) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    n = (n - 1) + (size_t)m;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body, (ssize_t)n);
}

esp_err_t handle_enroll_html(httpd_req_t* req) {
  char json_body[320];
  /* render_enroll_json already returns 0 on truncation (post-fix), so
   * a single == 0 check covers both empty + truncated. Pre-fix this
   * was an unguarded cast that could feed a >= cap value into the
   * outer snprintf as part of %s and produce a half-rendered <pre>
   * panel — flagged by Gemini Code Review on PR #447. */
  const size_t jn = render_enroll_json(json_body, sizeof(json_body));
  if (jn == 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  /* Page deliberately uses inline CSS — no external assets so the
   * captive-portal flow doesn't break behind a router that intercepts
   * other domains. The fingerprint is rendered in big monospace text
   * so an installer can read it off a phone screen and type it into
   * HA's config flow on a different device. */
  char page[2048];
  const int n = snprintf(page, sizeof(page),
    "<!doctype html><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>SecuraCV Canary — Enroll</title>"
    "<style>"
      "body{font-family:system-ui,-apple-system,sans-serif;max-width:560px;margin:32px auto;padding:0 16px;color:#1a1a1a;}"
      "h1{font-size:20px;margin-bottom:4px;}"
      "p{color:#555;line-height:1.5;}"
      ".fp{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:28px;letter-spacing:2px;background:#f4f4f5;padding:16px 20px;border-radius:8px;text-align:center;margin:20px 0;word-break:break-all;}"
      ".pk{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;color:#777;word-break:break-all;}"
      "details{margin-top:24px;}"
      "summary{cursor:pointer;color:#0066cc;}"
      "pre{background:#0d1117;color:#c9d1d9;padding:12px;border-radius:6px;overflow:auto;font-size:12px;}"
    "</style>"
    "<h1>Device fingerprint</h1>"
    "<p>Read this fingerprint into Home Assistant when adding this Canary. "
    "Pinning the fingerprint protects you from a hostile MQTT broker spoofing the device.</p>"
    "<div class=\"fp\">%s</div>"
    "<p>Full Ed25519 public key:</p>"
    "<p class=\"pk\">%s</p>"
    "<details><summary>Raw enrollment payload (for scripts)</summary>"
    "<pre>%s</pre></details>",
    device_signature::fingerprint_hex(),
    device_signature::pubkey_hex(),
    json_body);
  if (n <= 0 || (size_t)n >= sizeof(page)) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, page, (ssize_t)n);
}

}  /* namespace device_identity_api */
#endif  /* ARDUINO */
