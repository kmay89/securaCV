/*
 * cv_session_store.h — the dashboard's cv_session store, with where each
 * session came from (sweep F289).
 *
 * A session is a 32-byte random token the browser holds in an HttpOnly,
 * SameSite=Strict cookie (`cv_session=<64 hex>`), Secure on a unit that
 * serves its dashboard over TLS. Two places mint one:
 *
 *   LANDING — GET /?cv_pair=<token> (handle_ui). The landing page at `/`
 *             mints a pair token for ANY caller that can reach the device,
 *             on the SoftAP and, once the WAP has joined the home Wi-Fi,
 *             on the home network too: the 2026-07 audit's "Trust-on-LAN"
 *             posture. Such a session runs the dashboard.
 *   OWNER   — beside a served provisioning receipt (handle_provisioning_
 *             receipt), after a BOOT tap or a setup-door claim: the same
 *             response already handed the browser the bearer token.
 *
 * The origin is stored with the token when the session is minted and is
 * never read from anything the client sends: the cookie carries only the
 * random token, and the slot it matches says where it came from. A route
 * that answers a credential (the bearer token, the SoftAP password) takes
 * only an OWNER session in place of the bearer (api_auth.h,
 * api_auth_check_credential*); every other route takes either, as before.
 *
 * Pure: no Arduino, no IDF. The caller supplies the clock and the random
 * bytes (csi_integration.cpp: session_now_ms() and esp_fill_random()), so
 * the host suite (tests_host/test_receipt_session.cpp) runs this exact store.
 *
 * The clock is milliseconds since boot in 64 bits (esp_timer_get_time() /
 * 1000), never millis(): millis() is 32 bits on both Arduino-ESP32 cores and
 * wraps every 2^32 ms (49.7 days), and a slot's age taken across the wrap
 * reads young again, so a session past its 24 h would answer once more, for
 * a day, every 49.7 days of uptime (sweep F289's review).
 *
 * Threading: the store is the httpd worker's (handlers are serialized
 * behind one ESP-IDF httpd task), as it always was.
 */

#ifndef SECURACV_CV_SESSION_STORE_H
#define SECURACV_CV_SESSION_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace cv_session {

/* 8 slots is enough for a household worth of phones / laptops / tablets
 * signed in at once. 24 h TTL matches typical "remember me" UX and caps
 * the post-compromise window without forcing daily re-pairing. */
constexpr size_t   SLOTS       = 8;
constexpr uint64_t TTL_MS      = 24ULL * 60ULL * 60ULL * 1000ULL;  /* 24 h */
constexpr size_t   TOK_BYTES   = 32;                           /* 256-bit entropy */
constexpr size_t   TOK_HEX_LEN = TOK_BYTES * 2;                /* 64 hex chars */

/* Where a session came from. Set at mint, from the code path that minted
 * it; never from the request that presents it. The zero value is NONE, so
 * a slot nothing ever minted (the store is zero-initialized) reads as no
 * origin at all, never as the owner's: only an explicit OWNER mint answers
 * a credential. */
enum class Origin : uint8_t {
  NONE    = 0,   /* never minted: what a zeroed slot holds */
  LANDING = 1,   /* the landing page's pair token: any caller on the network */
  OWNER   = 2,   /* minted with a served receipt: a BOOT tap or a setup-door claim */
};

struct Slot {
  bool     active;
  uint64_t issued_ms;   /* the 64-bit clock at mint */
  Origin   origin;
  uint8_t  token[TOK_BYTES];
};

struct Store {
  Slot slots[SLOTS];
};

/* Constant-time compare so a timing oracle can't tell which byte differs. */
inline bool token_eq(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; ++i) d |= a[i] ^ b[i];
  return d == 0;
}

/* The slot a new session goes into: an empty or expired one, else the
 * oldest. A determined caller can churn issuances and bump a live session
 * (the owner signs in again); that trade-off is unchanged by F289. */
inline Slot* pick_for_issuance(Store& st, uint64_t now) {
  for (size_t i = 0; i < SLOTS; ++i) {
    Slot& s = st.slots[i];
    if (!s.active || (now - s.issued_ms) >= TTL_MS) return &s;
  }
  size_t oldest = 0;
  for (size_t i = 1; i < SLOTS; ++i) {
    if ((now - st.slots[i].issued_ms) > (now - st.slots[oldest].issued_ms)) {
      oldest = i;
    }
  }
  return &st.slots[oldest];
}

/* Store `fresh` (the caller's random bytes) as a session minted now, with
 * the origin of the code path that minted it. */
inline void issue(Store& st, uint64_t now, const uint8_t fresh[TOK_BYTES], Origin origin) {
  Slot* s = pick_for_issuance(st, now);
  memcpy(s->token, fresh, TOK_BYTES);
  s->issued_ms = now;
  s->origin    = origin;
  s->active    = true;
}

/* Zero `n` bytes at `p` in a way the compiler keeps: a memset on a local
 * that is dead after it may be dropped (the session bytes a mint leaves on
 * the stack). */
inline void wipe(void* p, size_t n) {
  volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) v[i] = 0;
}

/* The live session `token` names, or nullptr (none, or expired). */
inline const Slot* find(const Store& st, uint64_t now, const uint8_t token[TOK_BYTES]) {
  for (size_t i = 0; i < SLOTS; ++i) {
    const Slot& s = st.slots[i];
    if (!s.active) continue;
    if ((now - s.issued_ms) >= TTL_MS) continue;
    if (token_eq(s.token, token, TOK_BYTES)) return &s;
  }
  return nullptr;
}

/* The cv_session cookie's value out of a Cookie header ("name=value;
 * name=value; ..."): exactly TOK_HEX_LEN hex characters after the first
 * cookie NAMED cv_session, ended by ';', ' ' or the end of the header.
 * Anything else is a malformed cookie, and false. The name is the whole
 * name: it starts the header or follows ';' (spaces or tabs between), so
 * another cookie whose name ends in it ("xcv_session=") or whose value
 * holds it ("foo=cv_session=") is not read as the session (sweep F289's
 * second review). */
inline bool cookie_hex(const char* cookie_header, char hex_out[TOK_HEX_LEN + 1]) {
  if (!cookie_header || !hex_out) return false;
  static const char kName[] = "cv_session=";
  const char* k = nullptr;
  for (const char* p = strstr(cookie_header, kName); p; p = strstr(p + 1, kName)) {
    const char* q = p;
    while (q > cookie_header && (q[-1] == ' ' || q[-1] == '\t')) --q;
    if (q == cookie_header || q[-1] == ';') {
      k = p + (sizeof(kName) - 1);
      break;
    }
  }
  if (!k) return false;
  for (size_t i = 0; i < TOK_HEX_LEN; ++i) {
    const char c = k[i];
    const bool is_hex = (c >= '0' && c <= '9') ||
                        (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!is_hex) return false;
    hex_out[i] = c;
  }
  hex_out[TOK_HEX_LEN] = '\0';
  const char tail = k[TOK_HEX_LEN];
  return (tail == '\0' || tail == ';' || tail == ' ');
}

}  // namespace cv_session

#endif  // SECURACV_CV_SESSION_STORE_H
