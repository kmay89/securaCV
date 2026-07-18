// firmware/common/provision_qr/provision_qr.h — the SecuraCV provisioning
// QR grammar, shared by the display (mints codes) and the canaries (scan
// them). Header-only, no Arduino/ESP dependencies: the same bytes compile
// on-device and in host tests.
//
// Two accepted payloads, tried in order:
//
//   Primary — SCV1 compact pipe-KV (canonical):
//     SCV1|s=MyWiFi|p=hunter2|h=192.168.4.20|o=1884|t=<22ch>|x=1789700000|n=Porch
//   keys: s SSID (required), p passphrase, h hub host, o hub port (absent =
//   1883), t single-use provisioning token (base64url, 22 chars), x unix
//   expiry, n suggested name. `\|` and `\\` escape inside values; unknown
//   keys are skipped (g= is reserved for a future fleet signing key).
//
//   Secondary — the universal phone format:
//     WIFI:T:WPA;S:<ssid>;P:<pass>;;
//   (ZXing escaping of \ ; , : " — accepted with or without escapes, since
//   Android and iOS generators disagree.) Carries Wi-Fi only; a canary that
//   joins this way finds the hub by asking the fleet and waits for a human
//   blessing on the display.
//
// SECURITY POSTURE (the Wyze lesson — CVE'd shell injection via the SSID
// field of a provisioning QR): every field here is HOSTILE INPUT. This
// parser length-caps every field, validates numerics and the token charset,
// and copies bytes — it never interprets them. Callers must never pass a
// parsed field to a shell, a format string, or an allocator size.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace securacv::qr {

struct Provision {
  char ssid[33];        // ≤32 bytes, required
  char pass[65];        // ≤64 bytes ('' = open network)
  char host[64];        // hub host, '' = discover via the fleet
  uint16_t port;        // 1883 when absent
  char token[24];       // base64url fast-track token, '' = none
  int64_t expires_at;   // unix epoch, 0 = none
  char name[33];        // suggested witness name, '' = none
  bool wifi_only;       // true = plain WIFI: payload (no hub, no token)
};

enum class Parse {
  Ok,
  NotProvision,   // not ours — keep scanning (someone's wallpaper)
  Malformed,      // ours but broken/over-cap — surface the garbled state
};

namespace detail {

inline void pv_clear(Provision& p) {
  memset(&p, 0, sizeof(p));
  p.port = 1883;
}

// Copy src[0..n) into a cap-limited field. False = over cap (hostile).
inline bool pv_copy(char* dst, size_t dstcap, const char* src, size_t n) {
  if (n >= dstcap) return false;
  memcpy(dst, src, n);
  dst[n] = '\0';
  return true;
}

inline bool pv_all_digits(const char* s, size_t n) {
  if (n == 0) return false;
  for (size_t i = 0; i < n; i++)
    if (s[i] < '0' || s[i] > '9') return false;
  return true;
}

inline bool pv_token_charset(const char* s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const char c = s[i];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

// Unescape one segment (backslash escapes the next byte) into buf.
// Returns unescaped length, or SIZE_MAX on truncation/trailing backslash.
inline size_t pv_unescape(const char* s, size_t n, char* buf, size_t cap) {
  if (cap == 0) return SIZE_MAX;  // review catch: cap 0 would write buf[0]
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '\\') {
      if (++i >= n) return SIZE_MAX;  // trailing escape = malformed
      c = s[i];
    }
    if (o + 1 >= cap) return SIZE_MAX;
    buf[o++] = c;
  }
  buf[o] = '\0';
  return o;
}

inline Parse parse_scv1(const char* d, size_t len, Provision& out) {
  size_t i = 5;  // past "SCV1|"
  bool saw_ssid = false;
  while (i < len) {
    // Find the segment end: the next unescaped '|'.
    size_t j = i;
    while (j < len) {
      if (d[j] == '\\') {
        j += 2;
        continue;
      }
      if (d[j] == '|') break;
      j++;
    }
    if (j > len) return Parse::Malformed;  // trailing backslash ran off
    const size_t seg_len = j - i;
    if (seg_len < 2 || d[i + 1] != '=') return Parse::Malformed;
    const char key = d[i];
    const char* val = d + i + 2;
    const size_t vraw = seg_len - 2;

    // Unknown keys skip WITHOUT unescaping (review catch): forward
    // compatibility means a future long field — g= will be a signature —
    // must never trip a buffer sized for today's fields.
    const bool known = key == 's' || key == 'p' || key == 'h' ||
                       key == 'o' || key == 't' || key == 'x' || key == 'n';
    if (!known) {
      i = (j < len) ? j + 1 : len;
      continue;
    }

    // Unescape into a stack buffer that covers every KNOWN field's cap.
    char buf[96];
    const size_t vn = pv_unescape(val, vraw, buf, sizeof(buf));
    if (vn == SIZE_MAX) return Parse::Malformed;

    switch (key) {
      case 's':
        if (vn == 0 || !pv_copy(out.ssid, sizeof(out.ssid), buf, vn))
          return Parse::Malformed;
        saw_ssid = true;
        break;
      case 'p':
        if (!pv_copy(out.pass, sizeof(out.pass), buf, vn))
          return Parse::Malformed;
        break;
      case 'h':
        if (!pv_copy(out.host, sizeof(out.host), buf, vn))
          return Parse::Malformed;
        break;
      case 'o': {
        if (!pv_all_digits(buf, vn) || vn > 5) return Parse::Malformed;
        uint32_t v = 0;
        for (size_t k = 0; k < vn; k++) v = v * 10 + (uint32_t)(buf[k] - '0');
        if (v < 1 || v > 65535) return Parse::Malformed;
        out.port = (uint16_t)v;
        break;
      }
      case 't':
        if (vn == 0 || vn >= sizeof(out.token) ||
            !pv_token_charset(buf, vn) ||
            !pv_copy(out.token, sizeof(out.token), buf, vn))
          return Parse::Malformed;
        break;
      case 'x': {
        if (!pv_all_digits(buf, vn) || vn > 12) return Parse::Malformed;
        int64_t v = 0;
        for (size_t k = 0; k < vn; k++) v = v * 10 + (int64_t)(buf[k] - '0');
        out.expires_at = v;
        break;
      }
      case 'n':
        if (!pv_copy(out.name, sizeof(out.name), buf, vn))
          return Parse::Malformed;
        break;
      default:
        break;  // unknown key: skip (forward compatibility; g= reserved)
    }
    i = (j < len) ? j + 1 : len;
  }
  if (!saw_ssid) return Parse::Malformed;
  out.wifi_only = false;
  return Parse::Ok;
}

inline Parse parse_wifi(const char* d, size_t len, Provision& out) {
  size_t i = 5;  // past "WIFI:"
  bool saw_ssid = false;
  while (i < len) {
    if (d[i] == ';') {  // empty segment / the ";;" terminator
      i++;
      continue;
    }
    if (i + 1 >= len || d[i + 1] != ':') return Parse::Malformed;
    const char key = d[i];
    size_t j = i + 2;
    while (j < len) {  // value ends at the next unescaped ';'
      if (d[j] == '\\') {
        j += 2;
        continue;
      }
      if (d[j] == ';') break;
      j++;
    }
    if (j > len) return Parse::Malformed;
    // Only S and P matter to joining; other keys (T/H/E and whatever a
    // generator dreams up) skip raw so their length can never trip a
    // buffer sized for ours (review catch).
    if (key == 'S' || key == 'P') {
      char buf[96];
      const size_t vn = pv_unescape(d + i + 2, j - (i + 2), buf, sizeof(buf));
      if (vn == SIZE_MAX) return Parse::Malformed;
      if (key == 'S') {
        if (vn == 0 || !pv_copy(out.ssid, sizeof(out.ssid), buf, vn))
          return Parse::Malformed;
        saw_ssid = true;
      } else {
        if (!pv_copy(out.pass, sizeof(out.pass), buf, vn))
          return Parse::Malformed;
      }
    }
    i = (j < len) ? j + 1 : len;
  }
  if (!saw_ssid) return Parse::Malformed;
  out.wifi_only = true;
  return Parse::Ok;
}

// Escaped append for minting: escapes '\' and '|'. Returns false on overflow.
inline bool pv_append_escaped(char* out, size_t cap, size_t& o,
                              const char* v) {
  for (const char* p = v; *p; p++) {
    if (*p == '\\' || *p == '|') {
      if (o + 2 >= cap) return false;
      out[o++] = '\\';
    }
    if (o + 1 >= cap) return false;
    out[o++] = *p;
  }
  return true;
}

inline bool pv_append_raw(char* out, size_t cap, size_t& o, const char* v) {
  for (const char* p = *v ? v : ""; *p; p++) {
    if (o + 1 >= cap) return false;
    out[o++] = *p;
  }
  return true;
}

}  // namespace detail

// Parse a scanned payload. `out` is fully reset first.
inline Parse parse(const char* data, size_t len, Provision& out) {
  detail::pv_clear(out);
  if (!data) return Parse::NotProvision;
  if (len > 512) return Parse::NotProvision;  // no legit payload is this big
  if (len >= 5 && memcmp(data, "SCV1|", 5) == 0)
    return detail::parse_scv1(data, len, out);
  if (len >= 5 && memcmp(data, "WIFI:", 5) == 0)
    return detail::parse_wifi(data, len, out);
  return Parse::NotProvision;
}

// Mint a canonical SCV1 payload. Empty/zero optional fields are omitted;
// port 1883 (the default) is omitted. Returns the payload length, or 0 if
// it did not fit `cap`.
inline size_t mint_scv1(char* out, size_t cap, const char* ssid,
                        const char* pass, const char* host, uint16_t port,
                        const char* token, int64_t expires_at,
                        const char* name) {
  using namespace detail;
  if (!out || cap == 0 || !ssid || !ssid[0]) return 0;
  size_t o = 0;
  if (!pv_append_raw(out, cap, o, "SCV1|s=")) return 0;
  if (!pv_append_escaped(out, cap, o, ssid)) return 0;
  if (pass && pass[0]) {
    if (!pv_append_raw(out, cap, o, "|p=")) return 0;
    if (!pv_append_escaped(out, cap, o, pass)) return 0;
  }
  if (host && host[0]) {
    if (!pv_append_raw(out, cap, o, "|h=")) return 0;
    if (!pv_append_escaped(out, cap, o, host)) return 0;
  }
  if (port != 1883 && port != 0) {
    char num[8];
    int n = 0;
    uint32_t v = port;
    char rev[8];
    while (v) {
      rev[n++] = (char)('0' + v % 10);
      v /= 10;
    }
    for (int k = 0; k < n; k++) num[k] = rev[n - 1 - k];
    num[n] = '\0';
    if (!pv_append_raw(out, cap, o, "|o=")) return 0;
    if (!pv_append_raw(out, cap, o, num)) return 0;
  }
  if (token && token[0]) {
    if (!pv_append_raw(out, cap, o, "|t=")) return 0;
    if (!pv_append_raw(out, cap, o, token)) return 0;  // charset is ours
  }
  if (expires_at > 0) {
    char num[16];
    int n = 0;
    int64_t v = expires_at;
    char rev[16];
    while (v && n < 14) {
      rev[n++] = (char)('0' + (int)(v % 10));
      v /= 10;
    }
    for (int k = 0; k < n; k++) num[k] = rev[n - 1 - k];
    num[n] = '\0';
    if (!pv_append_raw(out, cap, o, "|x=")) return 0;
    if (!pv_append_raw(out, cap, o, num)) return 0;
  }
  if (name && name[0]) {
    if (!pv_append_raw(out, cap, o, "|n=")) return 0;
    if (!pv_append_escaped(out, cap, o, name)) return 0;
  }
  if (o >= cap) return 0;
  out[o] = '\0';
  return o;
}

}  // namespace securacv::qr
