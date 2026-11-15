/*
 * api_seal.h — the sealed bearer (canary-wap local API).
 *
 * WHAT IT IS. The local API's bearer token normally rides a plain
 * `Authorization: Bearer` header over plain HTTP (no stock build stores a
 * TLS certificate, sweep F15). A client that holds the device's pinned
 * identity fingerprint can instead agree a per-session key with THIS
 * device and send the token sealed under it:
 *
 *   1. GET /api/device/seal?nonce=<16-64 lowercase hex>&client_pub=<64 hex>
 *      — the client's fresh X25519 public key and its freshness.
 *   2. The device makes an ephemeral X25519 keypair, derives
 *        shared     = X25519(device_priv, client_pub)   (all-zero refused)
 *        canonical  = securacv-canary-sig|v1|seal|<device_id>|<nonce>|
 *                     <client_pub_hex>|<device_pub_hex>   (device_signature.h)
 *        transcript = SHA-256(canonical)
 *        key        = HKDF-SHA256(salt = transcript, ikm = shared,
 *                                 info = "securacv:api-seal:v1", 32 bytes)
 *        sid        = transcript[0..8]
 *      keeps {sid, key, last_seq = 0} in a small table, and answers the
 *      enroll card plus "seal": {sid, device_pub_hex, sig_hex, ttl_s}, where
 *      sig_hex is the identity key's Ed25519 signature over the canonical.
 *   3. The client verifies the signature against the fingerprint it pinned
 *      at flash time, derives the same key, and from then on sends
 *        Authorization: Sealed <sid_hex>.<seq>.<hex(ciphertext || tag)>
 *      with ciphertext = ChaCha20-Poly1305(key, iv = 0x00*8 || seq as 4
 *      big-endian bytes, aad = "<METHOD> <path>", plaintext = the token).
 *      seq is a strictly increasing counter per session.
 *   4. api_auth.h opens the header through api_seal_open() (seal_api.h),
 *      and the token inside is compared exactly as a plain bearer is, under
 *      the same per-address lockout.
 *
 * WHAT IT GIVES. The signature covers both public keys, so the key is
 * agreed with the holder of the identity key and nothing else; the sealed
 * header carries the token only under that key, bound to one method and
 * path and one counter value. A host that answers to this device's name,
 * or relays to the real device, can forward one sealed request but cannot
 * read the token, replay the request, or point it at another route.
 *
 * WHAT IT DOES NOT GIVE, stated so nobody over-reads it: the request body
 * and the answer are not sealed (a relay can still read an answer or change
 * a body such as identify's blink duration), and a plain `Bearer` header is
 * still accepted from every other client (the phone app, the dashboard,
 * Home Assistant), so this is a client's choice to never send the token in
 * the clear, not a device rule. The flashers make that choice.
 *
 * Pure: Curve25519 and ChaChaPoly from the Crypto library the sketch already
 * uses, mbedTLS HKDF and SHA-256, and device_signature.h's canonical. Host
 * tested over real X25519 / HKDF / ChaCha20-Poly1305 (OpenSSL behind the
 * stubs) in tests_host/test_api_seal.cpp, against reference vectors the
 * Flasher's Rust client (desktop/src-tauri/src/seal.rs) pins too.
 */

#ifndef CANARY_WAP_API_SEAL_H
#define CANARY_WAP_API_SEAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ChaChaPoly.h>
#include <Curve25519.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include "device_signature.h"
#include "identity_json.h"   /* build(): measured, then written (F212) */
#include "wap_json_writer.h"

namespace api_seal {

constexpr int      VERSION   = 1;
constexpr size_t   KEY_LEN   = 32;
constexpr size_t   PUB_LEN   = 32;
constexpr size_t   SID_LEN   = 8;
constexpr size_t   TAG_LEN   = 16;
constexpr size_t   IV_LEN    = 12;
constexpr size_t   SLOTS     = 4;                  /* sessions held at once */
constexpr uint32_t TTL_MS    = 15u * 60u * 1000u;  /* a session's life */
constexpr const char INFO[]  = "securacv:api-seal:v1";
constexpr const char SCHEME[] = "Sealed ";          /* the Authorization scheme */
constexpr size_t   SCHEME_LEN = sizeof(SCHEME) - 1;
/* The longest plaintext a seal carries (the token is 35 bytes). */
constexpr size_t   TOKEN_CAP = 64;
/* "<16 hex>.<up to 10 digits>.<hex of TOKEN_CAP + TAG_LEN bytes>" and NUL. */
constexpr size_t   VALUE_CAP = 2 * SID_LEN + 1 + 10 + 1 + 2 * (TOKEN_CAP + TAG_LEN) + 1;
/* The canonical: prefix 19 + "|v1|seal|" 9 + device_id 48 + 3 separators +
 * nonce 64 + two keys of 64 + NUL = 272 at most. */
constexpr size_t   CANONICAL_CAP = 320;

using FillRandomFn = void (*)(void* buf, size_t len);

struct Session {
  uint8_t  sid[SID_LEN];
  uint8_t  key[KEY_LEN];
  uint32_t last_seq;
  uint32_t opened_ms;
  bool     live;
};

struct Table {
  Session slot[SLOTS];
};

/* ONE table for every file that includes this header (a C++17 inline
 * variable), as api_auth.h's g_auth_table. Touched only from the httpd task. */
inline Table g_table = {};

inline void wipe(void* p, size_t n) {
  volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) v[i] = 0;
}

/* RFC 7748 §5. */
inline void clamp_scalar(uint8_t k[KEY_LEN]) {
  k[0]  &= 0xF8;
  k[31]  = (uint8_t)((k[31] & 0x7F) | 0x40);
}

inline int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

/* Exactly `n` lowercase hex chars (the whole string). */
inline bool hex_is(const char* s, size_t n) {
  if (!s) return false;
  for (size_t i = 0; i < n; ++i) {
    if (hex_nibble(s[i]) < 0) return false;
  }
  return s[n] == '\0';
}

/* 2*n hex chars from `s` into out[n]; false on any other byte. A string
 * that ends early is not read past its NUL. */
inline bool hex_decode(const char* s, size_t n, uint8_t* out) {
  for (size_t i = 0; i < n; ++i) {
    const int hi = hex_nibble(s[2 * i]);
    if (hi < 0) return false;
    const int lo = hex_nibble(s[2 * i + 1]);
    if (lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

/* Lowercase hex of in[n] into out[2n + 1]. */
inline void hex_encode(const uint8_t* in, size_t n, char* out) {
  static const char H[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out[2 * i]     = H[in[i] >> 4];
    out[2 * i + 1] = H[in[i] & 0xF];
  }
  out[2 * n] = '\0';
}

inline bool all_zero(const uint8_t* p, size_t n) {
  uint8_t acc = 0;
  for (size_t i = 0; i < n; ++i) acc |= p[i];
  return acc == 0;
}

inline bool expired(const Session& s, uint32_t now_ms) {
  return (uint32_t)(now_ms - s.opened_ms) > TTL_MS;
}

/* A slot for a new session: a free one, else an expired one, else the
 * oldest. There is always one. */
inline Session& take_slot(Table& t, uint32_t now_ms) {
  for (size_t i = 0; i < SLOTS; ++i) {
    if (!t.slot[i].live) return t.slot[i];
  }
  for (size_t i = 0; i < SLOTS; ++i) {
    if (expired(t.slot[i], now_ms)) return t.slot[i];
  }
  size_t oldest = 0;
  for (size_t i = 1; i < SLOTS; ++i) {
    if ((uint32_t)(now_ms - t.slot[i].opened_ms) > (uint32_t)(now_ms - t.slot[oldest].opened_ms)) {
      oldest = i;
    }
  }
  return t.slot[oldest];
}

inline Session* find(Table& t, const uint8_t sid[SID_LEN]) {
  for (size_t i = 0; i < SLOTS; ++i) {
    if (t.slot[i].live && memcmp(t.slot[i].sid, sid, SID_LEN) == 0) return &t.slot[i];
  }
  return nullptr;
}

/* The handshake's product: what goes on the wire, and the canonical the
 * identity key signs (the handler hands it to device_signature::sign_seal
 * through its three fields). */
struct Opened {
  char device_pub_hex[2 * PUB_LEN + 1];
  char sid_hex[2 * SID_LEN + 1];
  char canonical[CANONICAL_CAP];
};

enum class Open : uint8_t { OK, BAD_NONCE, BAD_CLIENT_PUB, NO_RANDOM, NO_SHARED, NO_KEY };

/* Step 2 of the protocol: an ephemeral keypair, the shared secret, the
 * canonical, the session key; the session stored in `t`. The private
 * scalar and the shared secret are wiped before return, on every path. */
inline Open open_session(Table& t, const char* device_id, const char* nonce,
                         const char* client_pub_hex, FillRandomFn fill,
                         uint32_t now_ms, Opened& out) {
  out.device_pub_hex[0] = '\0';
  out.sid_hex[0] = '\0';
  out.canonical[0] = '\0';
  if (!device_signature::whoami_nonce_ok(nonce)) return Open::BAD_NONCE;
  if (!device_signature::seal_pub_hex_ok(client_pub_hex)) return Open::BAD_CLIENT_PUB;
  uint8_t client_pub[PUB_LEN];
  if (!hex_decode(client_pub_hex, PUB_LEN, client_pub)) return Open::BAD_CLIENT_PUB;
  if (fill == nullptr) return Open::NO_RANDOM;

  uint8_t priv[KEY_LEN];
  uint8_t pub[PUB_LEN];
  bool made = false;
  for (int attempt = 0; attempt < 4 && !made; ++attempt) {
    fill(priv, KEY_LEN);
    clamp_scalar(priv);
    made = Curve25519::eval(pub, priv, nullptr);
  }
  if (!made) {
    wipe(priv, sizeof(priv));
    return Open::NO_RANDOM;
  }
  uint8_t shared[KEY_LEN];
  const bool agreed = Curve25519::eval(shared, priv, client_pub) && !all_zero(shared, KEY_LEN);
  wipe(priv, sizeof(priv));
  if (!agreed) {
    wipe(shared, sizeof(shared));
    return Open::NO_SHARED;
  }

  hex_encode(pub, PUB_LEN, out.device_pub_hex);
  const size_t n = device_signature::build_seal_canonical(
      nonce, client_pub_hex, out.device_pub_hex, device_id, out.canonical, sizeof(out.canonical));
  if (n == 0) {
    wipe(shared, sizeof(shared));
    out.device_pub_hex[0] = '\0';
    return Open::NO_KEY;
  }
  uint8_t transcript[32];
  if (mbedtls_sha256((const unsigned char*)out.canonical, n, transcript, 0) != 0) {
    wipe(shared, sizeof(shared));
    out.device_pub_hex[0] = '\0';
    out.canonical[0] = '\0';
    return Open::NO_KEY;
  }
  Session& s = take_slot(t, now_ms);
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  const int ret = mbedtls_hkdf(md, transcript, sizeof(transcript), shared, sizeof(shared),
                               (const unsigned char*)INFO, strlen(INFO), s.key, KEY_LEN);
  wipe(shared, sizeof(shared));
  if (ret != 0) {
    wipe(s.key, sizeof(s.key));
    s.live = false;
    out.device_pub_hex[0] = '\0';
    out.canonical[0] = '\0';
    return Open::NO_KEY;
  }
  memcpy(s.sid, transcript, SID_LEN);
  s.last_seq = 0;
  s.opened_ms = now_ms;
  s.live = true;
  hex_encode(s.sid, SID_LEN, out.sid_hex);
  return Open::OK;
}

enum class Unseal : uint8_t { OK, BAD_FORMAT, NO_SESSION, EXPIRED, REPLAY, BAD_TAG };

/* The word the 401 carries, so a client tells "open a new session" from
 * "the token inside was wrong" (which is api_auth.h's own 403). */
inline const char* reason(Unseal u) {
  switch (u) {
    case Unseal::OK:         return "ok";
    case Unseal::BAD_FORMAT: return "bad_format";
    case Unseal::NO_SESSION: return "no_session";
    case Unseal::EXPIRED:    return "expired";
    case Unseal::REPLAY:     return "replay";
    case Unseal::BAD_TAG:    return "bad_tag";
  }
  return "bad_format";
}

/* The associated data: "<METHOD> <path>", the path being the URI up to its
 * query. False when it does not fit. */
inline bool aad_for(const char* method, const char* uri, char* out, size_t cap) {
  if (!method || !uri || !out || cap == 0) return false;
  const char* q = strchr(uri, '?');
  const size_t path_len = q ? (size_t)(q - uri) : strlen(uri);
  const size_t m = strlen(method);
  if (m + 1 + path_len + 1 > cap) {
    out[0] = '\0';
    return false;
  }
  memcpy(out, method, m);
  out[m] = ' ';
  memcpy(out + m + 1, uri, path_len);
  out[m + 1 + path_len] = '\0';
  return true;
}

/* The header's value after "Sealed ": "<sid>.<seq>.<hex>", seq 1..2^32-1
 * in decimal with no leading zero. */
inline bool parse_value(const char* value, uint8_t sid[SID_LEN], uint32_t* seq,
                        const char** hex, size_t* hex_len) {
  if (!value) return false;
  if (!hex_decode(value, SID_LEN, sid)) return false;
  if (value[2 * SID_LEN] != '.') return false;
  const char* p = value + 2 * SID_LEN + 1;
  if (*p < '1' || *p > '9') return false;
  uint64_t n = 0;
  size_t digits = 0;
  while (*p >= '0' && *p <= '9') {
    n = n * 10 + (uint64_t)(*p - '0');
    if (++digits > 10 || n > 0xFFFFFFFFull) return false;
    ++p;
  }
  if (*p != '.') return false;
  ++p;
  const size_t len = strlen(p);
  if (len < 2 * TAG_LEN || (len & 1) != 0 || len > 2 * (TOKEN_CAP + TAG_LEN)) return false;
  for (size_t i = 0; i < len; ++i) {
    if (hex_nibble(p[i]) < 0) return false;
  }
  *seq = (uint32_t)n;
  *hex = p;
  *hex_len = len;
  return true;
}

/* Step 4: open one sealed header for the request it rides on. On OK the
 * token is in token_out (NUL-terminated) and the session's counter has
 * moved; on anything else token_out is empty. An expired session is
 * dropped the first time it is named. */
inline Unseal unseal(Table& t, const char* method, const char* uri, const char* value,
                     uint32_t now_ms, char* token_out, size_t token_cap) {
  if (!token_out || token_cap == 0) return Unseal::BAD_FORMAT;
  token_out[0] = '\0';
  uint8_t sid[SID_LEN];
  uint32_t seq = 0;
  const char* hex = nullptr;
  size_t hex_len = 0;
  if (!parse_value(value, sid, &seq, &hex, &hex_len)) return Unseal::BAD_FORMAT;
  const size_t ct_len = hex_len / 2 - TAG_LEN;
  if (ct_len + 1 > token_cap) return Unseal::BAD_FORMAT;
  char aad[96];
  if (!aad_for(method, uri, aad, sizeof(aad))) return Unseal::BAD_FORMAT;

  Session* s = find(t, sid);
  if (s == nullptr) return Unseal::NO_SESSION;
  if (expired(*s, now_ms)) {
    wipe(s->key, sizeof(s->key));
    s->live = false;
    return Unseal::EXPIRED;
  }
  if (seq <= s->last_seq) return Unseal::REPLAY;

  uint8_t buf[TOKEN_CAP + TAG_LEN];
  if (!hex_decode(hex, hex_len / 2, buf)) return Unseal::BAD_FORMAT;
  uint8_t iv[IV_LEN] = {0};
  iv[8]  = (uint8_t)(seq >> 24);
  iv[9]  = (uint8_t)(seq >> 16);
  iv[10] = (uint8_t)(seq >> 8);
  iv[11] = (uint8_t)(seq);

  ChaChaPoly aead;
  if (!aead.setKey(s->key, KEY_LEN) || !aead.setIV(iv, IV_LEN)) {
    aead.clear();
    return Unseal::BAD_TAG;
  }
  aead.addAuthData(aad, strlen(aad));
  aead.decrypt((uint8_t*)token_out, buf, ct_len);
  const bool ok = aead.checkTag(buf + ct_len, TAG_LEN);
  aead.clear();
  wipe(buf, sizeof(buf));
  if (!ok) {
    wipe(token_out, ct_len);
    token_out[0] = '\0';
    return Unseal::BAD_TAG;
  }
  token_out[ct_len] = '\0';
  s->last_seq = seq;
  return Unseal::OK;
}

/* ── the handshake's answer, measured and escaped (F212) ────────────────── */

struct Answer {
  const char* device_id;        /* device_signature::device_id() */
  const char* pubkey_hex;       /* device_signature::pubkey_hex() */
  const char* fingerprint_hex;  /* device_signature::fingerprint_hex() */
  const char* nonce;            /* the caller's, as validated */
  const char* sid_hex;          /* Opened::sid_hex */
  const char* device_pub_hex;   /* Opened::device_pub_hex */
  const char* sig_hex;          /* device_signature::sign_seal() */
  uint32_t    ttl_s;            /* TTL_MS / 1000 */
};

/* The enroll card's fields first, by the same names (a client that reads
 * the card reads this too), then the seal. */
inline void write_answer(wap_json::Writer& w, const Answer& a) {
  wap_json::raw(w, "{\"device_id\":");
  wap_json::str(w, a.device_id);
  wap_json::raw(w, ",\"pubkey_hex\":");
  wap_json::str(w, a.pubkey_hex);
  wap_json::raw(w, ",\"fingerprint_hex\":");
  wap_json::str(w, a.fingerprint_hex);
  wap_json::raw(w, ",\"alg\":\"ed25519\",\"v\":1,\"nonce\":");
  wap_json::str(w, a.nonce);
  wap_json::raw(w, ",\"seal\":{\"v\":1,\"sid\":");
  wap_json::str(w, a.sid_hex);
  wap_json::raw(w, ",\"device_pub_hex\":");
  wap_json::str(w, a.device_pub_hex);
  wap_json::raw(w, ",\"sig_hex\":");
  wap_json::str(w, a.sig_hex);
  wap_json::raw(w, ",\"ttl_s\":");
  wap_json::number(w, a.ttl_s);
  wap_json::raw(w, "}}");
}

/* The answer into out[cap]; identity_json::build()'s contract (NULL out
 * measures; a buffer too small writes nothing and returns the need). */
inline size_t answer(const Answer& a, char* out, size_t cap) {
  return identity_json::build(write_answer, a, out, cap);
}

}  /* namespace api_seal */

#endif  /* CANARY_WAP_API_SEAL_H */
