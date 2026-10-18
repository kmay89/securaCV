/**
 * @file hub_handoff.h
 * @brief The hub hand-off over the Bluetooth setup door — the pure half.
 *
 * A Sense or Vision that takes its Wi-Fi from a phone over the Improv door
 * (improv_core.h / improv_ble.cpp) still has no hub: its broker address and
 * login come from NVS seeded at flash time, or from the compiled secrets,
 * and neither family browses for a broker. The phone, meanwhile, holds both
 * — it found Home Assistant over Bonjour and it minted the broker login —
 * so right after the join it writes them to one characteristic on the
 * SecuraCV companion service, over the same encrypted link. This header is
 * the JSON that crosses, the rules it is held to, and the status byte the
 * phone reads back; it knows nothing of NimBLE, NVS or MQTT, so a g++ test
 * reaches every rule.
 *
 * The wire (docs/design/magic_pairing.md §3.6, docs/ble_protocol.md):
 *
 *   service        8fc1cf00-b162-4401-9607-c8ac21383e90   (the WAP's companion
 *                  service, the same UUID; the WAP's CLAIM is 8fc1cf01, which
 *                  Sense and Vision do not register)
 *   HUB            8fc1cf02-b162-4401-9607-c8ac21383e90
 *                  WRITE | WRITE_ENC | READ | READ_ENC
 *
 *   WRITE: UTF-8 JSON, at most MAX_PAYLOAD (240) bytes:
 *     {"host":"homeassistant.local","port":1883,"user":"canary","pass":"<secret>","tls":false}
 *     host  required: 1..63 bytes of letters, digits, dots and hyphens in
 *           DNS labels (no leading, trailing or doubled dot; no label that
 *           starts or ends with a hyphen; a label at most 63 bytes), or an
 *           IPv4 literal (four decimal octets 0..255, no leading zeros). A
 *           name made only of digits and dots must be an IPv4 literal.
 *     port  optional, default 1883: an integer 1..65535 as a JSON number
 *           (no sign, fraction, exponent or leading zero).
 *     user, pass  optional, both present or both absent: strings of at most
 *           64 bytes each after unescaping; the user is never empty, the
 *           pass may be (a broker that asks only for a name).
 *     tls   optional, default false: a JSON boolean.
 *   READ: one byte, the Status below — never the document, never a secret.
 *
 * The reader is deliberately small and strict, as improv_core.h's packet
 * parser is: exactly these five keys and no others, each at most once (a
 * duplicate is Malformed, not "last wins"), the three JSON escapes a host
 * or a password can need (\" \\ \/) and no other (\n, \t and \uXXXX are
 * refused), no control bytes inside a string, no value of another type,
 * nothing after the closing brace but whitespace. A document that fails any
 * of that is refused whole and nothing in it is believed; the caller's
 * HubHandoff is wiped on every verdict but Ok.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace canary {
namespace net {
namespace hub_handoff {

// ── The wire ───────────────────────────────────────────────────────────────

/// The SecuraCV companion service, beside the Improv one and outside it,
/// so a standard Improv client never meets it. Same UUID the WAP's sketch
/// registers for its CLAIM characteristic (8fc1cf01).
inline constexpr const char* SERVICE_UUID = "8fc1cf00-b162-4401-9607-c8ac21383e90";
/// HUB: WRITE the broker document, READ one status byte.
inline constexpr const char* HUB_UUID     = "8fc1cf02-b162-4401-9607-c8ac21383e90";

inline constexpr size_t   MAX_PAYLOAD    = 240;   ///< the longest document accepted, in bytes
inline constexpr size_t   HOST_MAX       = 63;    ///< bytes, the DNS name bound
inline constexpr size_t   CREDENTIAL_MAX = 64;    ///< bytes each, user and pass, after unescaping
inline constexpr uint16_t DEFAULT_PORT   = 1883;

/// What a READ of HUB answers: the device's verdict on the last write this
/// join, and nothing else.
enum class Status : uint8_t {
  None         = 0,  ///< nothing stored through the door this session
  Stored       = 1,  ///< stored (durably, readback-checked) and applied
  Rejected     = 2,  ///< malformed, too long, an unencrypted link, or no join this session
  LoginRefused = 3,  ///< stored, but the broker refused the login on the first attempt
                     ///< (emitted only by a glue whose MQTT manager can report it cheaply)
};

/// The reader's verdict on one document.
enum class Verdict : uint8_t {
  Ok,
  Malformed,       ///< not the JSON this header describes (shape, type, escape, duplicate, unknown key)
  TooLong,         ///< more than MAX_PAYLOAD bytes; nothing was read
  BadHost,         ///< host missing, empty, over 63 bytes, or not a DNS name / IPv4 literal
  BadPort,         ///< port present but not an integer 1..65535
  BadCredentials,  ///< user without pass or pass without user; an empty user; either over 64 bytes
};

/// A parsed document. Fixed buffers so the password never touches the heap;
/// wipe() it when done.
struct HubHandoff {
  char     host[HOST_MAX + 1];
  uint16_t port;
  bool     has_credentials;   ///< user and pass are both present
  char     user[CREDENTIAL_MAX + 1];
  char     pass[CREDENTIAL_MAX + 1];
  bool     tls;
};

/// Zero a buffer in a way the optimizer keeps (improv_core.h's rule).
inline void wipe(void* p, size_t n) {
  volatile uint8_t* b = (volatile uint8_t*)p;
  while (n--) *b++ = 0;
}

/// Zero the whole record, password included.
inline void wipe(HubHandoff& h) { wipe(&h, sizeof(h)); }

inline const char* verdict_name(Verdict v) {
  switch (v) {
    case Verdict::Ok:             return "ok";
    case Verdict::Malformed:      return "malformed";
    case Verdict::TooLong:        return "too long";
    case Verdict::BadHost:        return "bad host";
    case Verdict::BadPort:        return "bad port";
    case Verdict::BadCredentials: return "bad credentials";
    default:                      return "?";
  }
}

// ── The host rule ──────────────────────────────────────────────────────────

namespace detail {

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/// Four decimal octets 0..255 separated by single dots, no leading zeros
/// ("01" is refused; "0" is fine), nothing else.
inline bool valid_ipv4(const char* s, size_t n) {
  size_t i = 0;
  for (int octet = 0; octet < 4; ++octet) {
    if (octet) {
      if (i >= n || s[i] != '.') return false;
      ++i;
    }
    size_t digits = 0;
    unsigned value = 0;
    while (i < n && is_digit(s[i])) {
      if (digits == 0 && s[i] == '0' && i + 1 < n && is_digit(s[i + 1])) return false;  // leading zero
      value = value * 10 + (unsigned)(s[i] - '0');
      if (++digits > 3) return false;
      ++i;
    }
    if (digits == 0 || value > 255) return false;
  }
  return i == n;
}

/// A DNS name as the header comment states it, or an IPv4 literal.
inline bool valid_host(const char* s, size_t n) {
  if (n == 0 || n > HOST_MAX) return false;
  bool numeric = true;
  for (size_t i = 0; i < n; ++i) {
    if (!is_digit(s[i]) && s[i] != '.') { numeric = false; break; }
  }
  if (numeric) return valid_ipv4(s, n);
  size_t label = 0;
  char prev = '.';
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    if (c == '.') {
      if (label == 0 || prev == '-') return false;   // a leading or doubled dot; a label ending in '-'
      label = 0;
    } else if (is_alpha(c) || is_digit(c) || c == '-') {
      if (label == 0 && c == '-') return false;      // a label starting with '-'
      if (++label > 63) return false;
    } else {
      return false;
    }
    prev = c;
  }
  return label != 0 && prev != '-';                  // a trailing dot; a name ending in '-'
}

// ── The reader ─────────────────────────────────────────────────────────────

struct Cursor {
  const uint8_t* p;
  const uint8_t* end;
};

inline void skip_ws(Cursor& c) {
  while (c.p < c.end && (*c.p == ' ' || *c.p == '\t' || *c.p == '\n' || *c.p == '\r')) ++c.p;
}

inline bool take(Cursor& c, char ch) {
  skip_ws(c);
  if (c.p < c.end && *c.p == (uint8_t)ch) { ++c.p; return true; }
  return false;
}

/// One JSON string into `out` (cap includes the NUL). False when it is not a
/// string this reader accepts: unterminated, a control byte, an escape other
/// than \" \\ \/. A string longer than the buffer is consumed whole and
/// reported through `overflow` (the caller names the field); `len` is the
/// unescaped length either way.
inline bool read_string(Cursor& c, char* out, size_t cap, size_t& len, bool& overflow) {
  skip_ws(c);
  if (c.p >= c.end || *c.p != '"') return false;
  ++c.p;
  size_t n = 0;
  overflow = false;
  for (;;) {
    if (c.p >= c.end) return false;        // unterminated
    uint8_t ch = *c.p++;
    if (ch == '"') break;
    if (ch < 0x20) return false;           // a control byte (NUL included) is never literal in JSON
    if (ch == '\\') {
      if (c.p >= c.end) return false;
      const uint8_t e = *c.p++;
      if (e == '"' || e == '\\' || e == '/') ch = e;
      else return false;                   // \n \t \b \f \r \uXXXX: refused
    }
    if (n + 1 < cap) out[n] = (char)ch;
    else overflow = true;
    ++n;
  }
  out[n + 1 < cap ? n : cap - 1] = '\0';
  len = n;
  return true;
}

/// One JSON number token. False when there is no number here at all (the
/// caller answers Malformed). `bad` is set for a number that is not an
/// unsigned integer spelled plainly — a sign, a fraction, an exponent, a
/// leading zero — or one past 65535; the token is consumed either way.
inline bool read_uint(Cursor& c, uint32_t& out, bool& bad) {
  skip_ws(c);
  bad = false;
  if (c.p >= c.end) return false;
  if (*c.p == '-') { bad = true; ++c.p; }
  if (c.p >= c.end || !is_digit((char)*c.p)) return false;
  const bool leading_zero = *c.p == '0';
  uint32_t v = 0;
  size_t digits = 0;
  while (c.p < c.end && is_digit((char)*c.p)) {
    if (v <= 65535) v = v * 10 + (uint32_t)(*c.p - '0');   // saturates past the port range
    ++c.p;
    ++digits;
  }
  if (leading_zero && digits > 1) bad = true;
  if (c.p < c.end && (*c.p == '.' || *c.p == 'e' || *c.p == 'E')) {
    bad = true;
    while (c.p < c.end && (is_digit((char)*c.p) || *c.p == '.' || *c.p == 'e' || *c.p == 'E' ||
                           *c.p == '+' || *c.p == '-')) {
      ++c.p;
    }
  }
  if (v > 65535) bad = true;
  out = v;
  return true;
}

/// The literals true / false, nothing else (null is not a boolean).
inline bool read_bool(Cursor& c, bool& out) {
  skip_ws(c);
  const size_t left = (size_t)(c.end - c.p);
  if (left >= 4 && memcmp(c.p, "true", 4) == 0) { c.p += 4; out = true; return true; }
  if (left >= 5 && memcmp(c.p, "false", 5) == 0) { c.p += 5; out = false; return true; }
  return false;
}

inline bool key_is(const char* key, size_t klen, const char* lit) {
  return klen == strlen(lit) && memcmp(key, lit, klen) == 0;
}

inline Verdict refuse(HubHandoff& out, Verdict v) {
  wipe(out);
  return v;
}

}  // namespace detail

/**
 * @brief Parse one HUB write.
 *
 * `out` is wiped first and again on every verdict but Ok, so a refused
 * document leaves no password behind. The field verdicts (BadHost, BadPort,
 * BadCredentials) are answered only for a document that is otherwise
 * well-formed JSON of this shape — a malformed document is Malformed
 * whatever its fields said — and in that order when more than one applies.
 */
inline Verdict parse_hub_handoff(const uint8_t* data, size_t len, HubHandoff& out) {
  using namespace detail;
  wipe(out);
  if (len > MAX_PAYLOAD) return Verdict::TooLong;            // nothing read; the record stays wiped
  if (data == nullptr || len == 0) return Verdict::Malformed;
  out.port = DEFAULT_PORT;

  Cursor c{data, data + len};
  if (!take(c, '{')) return refuse(out, Verdict::Malformed);

  bool seen_host = false, seen_port = false, seen_user = false, seen_pass = false, seen_tls = false;
  bool host_bad = false, port_bad = false, cred_bad = false;

  skip_ws(c);
  if (c.p < c.end && *c.p == '}') {
    ++c.p;
  } else {
    for (;;) {
      char key[8];
      size_t klen = 0;
      bool kover = false;
      if (!read_string(c, key, sizeof(key), klen, kover)) return refuse(out, Verdict::Malformed);
      if (!take(c, ':')) return refuse(out, Verdict::Malformed);
      if (kover) return refuse(out, Verdict::Malformed);   // longer than any key this reader knows

      if (key_is(key, klen, "host")) {
        if (seen_host) return refuse(out, Verdict::Malformed);
        seen_host = true;
        size_t n = 0;
        bool over = false;
        if (!read_string(c, out.host, sizeof(out.host), n, over)) return refuse(out, Verdict::Malformed);
        if (over || !valid_host(out.host, n)) host_bad = true;
      } else if (key_is(key, klen, "port")) {
        if (seen_port) return refuse(out, Verdict::Malformed);
        seen_port = true;
        uint32_t v = 0;
        bool bad = false;
        if (!read_uint(c, v, bad)) return refuse(out, Verdict::Malformed);
        if (bad || v < 1 || v > 65535) port_bad = true;
        else out.port = (uint16_t)v;
      } else if (key_is(key, klen, "user")) {
        if (seen_user) return refuse(out, Verdict::Malformed);
        seen_user = true;
        size_t n = 0;
        bool over = false;
        if (!read_string(c, out.user, sizeof(out.user), n, over)) return refuse(out, Verdict::Malformed);
        if (over || n == 0) cred_bad = true;
      } else if (key_is(key, klen, "pass")) {
        if (seen_pass) return refuse(out, Verdict::Malformed);
        seen_pass = true;
        size_t n = 0;
        bool over = false;
        if (!read_string(c, out.pass, sizeof(out.pass), n, over)) return refuse(out, Verdict::Malformed);
        if (over) cred_bad = true;
      } else if (key_is(key, klen, "tls")) {
        if (seen_tls) return refuse(out, Verdict::Malformed);
        seen_tls = true;
        if (!read_bool(c, out.tls)) return refuse(out, Verdict::Malformed);
      } else {
        return refuse(out, Verdict::Malformed);           // a key this reader does not know
      }

      skip_ws(c);
      if (c.p >= c.end) return refuse(out, Verdict::Malformed);
      if (*c.p == ',') { ++c.p; continue; }
      if (*c.p == '}') { ++c.p; break; }
      return refuse(out, Verdict::Malformed);
    }
  }
  skip_ws(c);
  if (c.p != c.end) return refuse(out, Verdict::Malformed);   // bytes after the document

  if (!seen_host || host_bad) return refuse(out, Verdict::BadHost);
  if (port_bad) return refuse(out, Verdict::BadPort);
  if (seen_user != seen_pass || cred_bad) return refuse(out, Verdict::BadCredentials);
  out.has_credentials = seen_user;
  return Verdict::Ok;
}

}  // namespace hub_handoff
}  // namespace net
}  // namespace canary
