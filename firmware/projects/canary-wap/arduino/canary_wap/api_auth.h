/*
 * SecuraCV Canary — API Authentication Module
 *
 * Provides Bearer token authentication for the REST API with:
 * - Constant-time token comparison (prevents timing side-channel attacks)
 * - Exponential backoff on failed attempts (brute-force mitigation), one
 *   backoff per source address (sweep F343): 8 tracked addresses plus one
 *   shared overflow, 2 s doubling, capped at 60 s (auth_backoff.h)
 * - Token redaction for safe logging
 *
 * Usage with esp_http_server (ESP-IDF native):
 *   In any handler:
 *     if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;
 *
 * The check function sends the appropriate error response (401/403/429)
 * and returns false if auth fails. The handler should return immediately.
 */

#ifndef SECURACV_API_AUTH_H
#define SECURACV_API_AUTH_H

#include <Arduino.h>
#include "esp_http_server.h"

#include "auth_logic.h"
#include "auth_backoff.h"   // the per-address lockout table (staged from
                            // firmware/common/network; check_improv_sync.sh)

// Optional bridge supplied by csi_integration.cpp. A valid HttpOnly
// cv_session cookie is an API credential for browser UI calls, so users
// who already opened the paired dashboard do not have to reveal or retype
// the long-lived API token when they click Settings.
//
// It is NOT a credential for the routes that hand out a credential (sweep
// F289). The dashboard's landing page mints that session for whoever loads
// it on the home network (deliberate LAN posture, the 2026-07 audit's
// "Trust-on-LAN"), so a session is proof of being on the LAN, not of holding
// the token or of a hand on the BOOT button. GET /api/provisioning-receipt
// (the bearer token, the SoftAP password, the TLS pin) and GET
// /api/pairing-qr (the token and the pin) therefore take the bearer-only
// checks below — api_auth_bearer_grant() and api_auth_check_bearer(), both
// under every admin route's per-address lockout — and never this bridge;
// their other grants (the BOOT tap, the setup-door claim) are the handlers'
// own.
extern bool cv_session_validate(httpd_req_t* req) __attribute__((weak));

// The source a request's wrong bearers are counted against (sweep F343): the
// host-order IPv4 of the request's socket peer, never a request header, or 0
// for a source that cannot be named (a real IPv6 peer, an unreadable socket),
// the one bucket every such source shares. Defined once, in canary_wap.ino
// (auth_request_source(), over request_peer_key()). Not weak: a build that
// compiles the lockout without it fails to link, where a weak hook would
// count every caller in one bucket without a word.
uint32_t auth_request_source(httpd_req_t* req);

// ═══════════════════════════════════════════════════════════════════
// STATE
// ═══════════════════════════════════════════════════════════════════

// ONE table for every file that includes this header: canary_wap.ino,
// csi_integration.cpp, csi_mqtt.cpp and the module headers they include. A
// C++17 inline variable has one definition across translation units. The
// `static AuthState g_auth` it replaces was a separate copy in each file
// (a `static` in a header is one per .cpp), so a strike one file's gate
// recorded never held another file's. Touched only from the httpd task that
// serves the API (auth_backoff.h takes no lock).
inline canary::net::auth_backoff::Table g_auth_table = {};

// ═══════════════════════════════════════════════════════════════════
// CONSTANT-TIME COMPARISON
// ═══════════════════════════════════════════════════════════════════

// Prevents timing side-channel attacks. The comparison always takes
// the same amount of time regardless of where the mismatch occurs.
// This is critical — ESP32 is fast enough for timing attacks to be
// practical over WiFi.

static bool constant_time_compare(const char* a, const char* b, size_t len) {
  volatile uint8_t result = 0;
  for (size_t i = 0; i < len; i++) {
    result |= ((volatile uint8_t)a[i]) ^ ((volatile uint8_t)b[i]);
  }
  return result == 0;
}

// ═══════════════════════════════════════════════════════════════════
// EXPONENTIAL BACKOFF — one per source address (sweep F343)
// ═══════════════════════════════════════════════════════════════════

// Each wrong bearer is a strike against the request's source address alone
// (auth_request_source()), and only that address is held. The table
// (auth_backoff.h) tracks 8 addresses, each with its own backoff; an address
// that finds no free slot strikes one shared overflow slot, which holds every
// untracked address while it runs. Per address, the backoff doubles:
//   strike 1: 2 s
//   strike 2: 4 s
//   strike 3: 8 s
//   strike 4: 16 s
//   strike 5: 32 s
//   strike 6 and on: 60 s, the cap, and nothing waits longer
// An address clean for 60 s is free again (its count cleared). The cap is
// that window: the 5-minute cap this replaced never ran, because the window
// cleared the count first (and the sixth strike announced 65 s for a hold
// that ended at 60 s). A success clears only its own address's slot, never
// the overflow.

static bool auth_is_locked_out(httpd_req_t* req) {
  return canary::net::auth_backoff::locked(g_auth_table, auth_request_source(req), millis());
}

// The console line for a strike: that address's count (the overflow's, for
// an address without a slot) and the backoff it armed. It names no address
// (auth_backoff.h: addresses stay in RAM).
static void auth_log_strike(uint32_t source, uint32_t now) {
  const canary::net::auth_backoff::Slot* s = canary::net::auth_backoff::tracked(g_auth_table, source);
  const canary::net::auth_backoff::Slot& r = s ? *s : g_auth_table.overflow;
  Serial.printf("[AUTH] Failed attempt #%u%s. Lockout: %lu ms\n", (unsigned)r.failures,
                s ? "" : " (shared overflow)",
                (unsigned long)canary::net::auth_backoff::remaining_ms(g_auth_table, source, now));
}

static void auth_record_failure(httpd_req_t* req) {
  const uint32_t source = auth_request_source(req);
  const uint32_t now = millis();
  canary::net::auth_backoff::record_failure(g_auth_table, source, now);
  auth_log_strike(source, now);
}

static void auth_record_success(httpd_req_t* req) {
  canary::net::auth_backoff::record_success(g_auth_table, auth_request_source(req));
}

// The milliseconds the request's source's backoff has left, 0 once it has
// ended (the same reading auth_is_locked_out() makes). The two answers that
// report it read it here: a caller checks the lockout first and sends later,
// and a backoff that ends in between must read 0, not an unsigned
// subtraction that wraps to about 4.29 million seconds.

static uint32_t auth_lockout_remaining_ms(httpd_req_t* req) {
  return canary::net::auth_backoff::remaining_ms(g_auth_table, auth_request_source(req), millis());
}

// ═══════════════════════════════════════════════════════════════════
// TOKEN REDACTION
// ═══════════════════════════════════════════════════════════════════

// For logging: never log the full token. Show only prefix.
// "cv_7kX9mN2p..." → "cv_7kX9****"

static void auth_redact_token(const char* token, char* out, size_t out_len) {
  if (strlen(token) < 8 || out_len < 13) {
    strncpy(out, "cv_****", out_len);
    return;
  }
  snprintf(out, out_len, "%.8s****", token);
}

// ═══════════════════════════════════════════════════════════════════
// AUTH CHECK — For use with esp_http_server (ESP-IDF)
// ═══════════════════════════════════════════════════════════════════

// The lockout's answer: 429 with Retry-After, the seconds left of this
// request's source's backoff (at least 1, also when the backoff ended after
// the caller's check; at most 61, the 60 s cap's). Sent by
// api_auth_check_bearer() for any request while the backoff runs, and by GET
// /api/provisioning-receipt for a bearer the backoff kept from being compared
// when neither the BOOT tap nor the setup-door claim served it.

static void api_auth_send_lockout(httpd_req_t* req) {
  const uint32_t remaining = auth_lockout_remaining_ms(req);
  char retry_str[8];
  snprintf(retry_str, sizeof(retry_str), "%lu", (unsigned long)(remaining / 1000 + 1));

  httpd_resp_set_status(req, "429 Too Many Requests");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Retry-After", retry_str);

  char body[128];
  snprintf(body, sizeof(body),
    "{\"error\":\"too_many_requests\",\"retry_after_s\":%s}",
    retry_str);
  httpd_resp_sendstr(req, body);
}

// Returns true if authenticated, false if response already sent (401/403/429).
// The caller should return ESP_OK immediately if this returns false.
//
// Two entry points over one body. api_auth_check() takes a valid cv_session
// cookie OR a Bearer token: every admin route. api_auth_check_bearer() takes
// ONLY the Bearer token, with the same lockout, the same failure accounting
// and the same 401/403/429 answers: the routes whose answer carries a
// credential (sweep F289 — the pairing QR), where a session the landing page
// mints for any LAN host must not stand in for the token.

static bool api_auth_check_bearer(httpd_req_t* req, const char* expected_token) {
  // ── Check this source's lockout ──
  if (auth_is_locked_out(req)) {
    api_auth_send_lockout(req);
    return false;
  }

  // ── Extract Authorization header ──
  size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (auth_len == 0) {
    // No credential presented at all — a plain 401, NOT a lockout strike.
    // Counting these let any credential-less client (a dashboard tab whose
    // session cookie died, captive-portal probes) arm the backoff and 429
    // the operator's correct token (see auth_logic.h).
    if (auth_logic::counts_toward_lockout(auth_logic::Attempt::NO_CREDENTIAL)) {
      auth_record_failure(req);
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"securacv\"");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\",\"hint\":\"Include 'Authorization: Bearer cv_...' header\"}");
    return false;
  }

  char auth_buf[128];
  if (auth_len >= sizeof(auth_buf)) {
    auth_record_failure(req);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\",\"hint\":\"Authorization header too long\"}");
    return false;
  }

  httpd_req_get_hdr_value_str(req, "Authorization", auth_buf, sizeof(auth_buf));

  // ── Validate Bearer prefix ──
  if (strncmp(auth_buf, "Bearer ", 7) != 0) {
    auth_record_failure(req);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\",\"hint\":\"Use 'Bearer <token>' format\"}");
    return false;
  }

  // ── Extract token (skip "Bearer ") ──
  const char* token = auth_buf + 7;

  // Trim leading/trailing whitespace
  while (*token == ' ') token++;
  size_t token_len = strlen(token);
  while (token_len > 0 && token[token_len - 1] == ' ') token_len--;

  // An empty device token is no token: no header matches it (a zero-length
  // compare would take a bare "Bearer ").
  size_t expected_len = strlen(expected_token);
  if (expected_len == 0 || token_len != expected_len) {
    auth_record_failure(req);
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"forbidden\"}");
    return false;
  }

  if (!constant_time_compare(token, expected_token, expected_len)) {
    auth_record_failure(req);
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"forbidden\"}");
    return false;
  }

  // ── Success ──
  auth_record_success(req);
  return true;
}

static bool api_auth_check(httpd_req_t* req, const char* expected_token) {
  // A valid session cookie authenticates the request, but it must NOT
  // reset the Bearer-token failure lockout. Otherwise an attacker who
  // happens to hold (or can mint) a session cookie could clear its
  // address's backoff between token guesses, neutralizing the brute-force
  // protection. Only a successful Bearer token comparison calls
  // auth_record_success() (in api_auth_check_bearer).
  if (cv_session_validate && cv_session_validate(req)) {
    return true;
  }
  return api_auth_check_bearer(req, expected_token);
}

// ═══════════════════════════════════════════════════════════════════
// OPTIONAL AUTH CHECK — Returns true if valid token present
// ═══════════════════════════════════════════════════════════════════

// Like api_auth_check but does NOT send error response on failure, and
// records nothing against the lockout. Used for endpoints that allow both
// authenticated and unauthenticated access.
//
// api_auth_bearer_valid() is the Bearer half alone: a real
// `Authorization: Bearer <token>` header, constant-time compared, true or
// false, nothing sent and nothing recorded. A valid cv_session cookie is NOT
// accepted here (sweep F289): the compare under GET
// /api/provisioning-receipt's first grant (api_auth_bearer_grant(), below,
// which adds the lockout), whose answer is the bearer token itself and the
// SoftAP password, so the landing page's 24 h session must not be what buys
// it. api_auth_check_optional() is the cookie OR this.

static bool api_auth_bearer_valid(httpd_req_t* req, const char* expected_token) {
  // An empty device token is no token (a zero-length compare would take a
  // bare "Bearer "); this also covers api_auth_check_optional() and
  // api_auth_bearer_grant(), which compare through here.
  if (expected_token == nullptr || expected_token[0] == '\0') return false;
  size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (auth_len == 0) return false;

  char auth_buf[128];
  if (auth_len >= sizeof(auth_buf)) return false;
  httpd_req_get_hdr_value_str(req, "Authorization", auth_buf, sizeof(auth_buf));

  if (strncmp(auth_buf, "Bearer ", 7) != 0) return false;

  const char* token = auth_buf + 7;
  while (*token == ' ') token++;
  size_t token_len = strlen(token);
  while (token_len > 0 && token[token_len - 1] == ' ') token_len--;

  size_t expected_len = strlen(expected_token);
  if (token_len != expected_len) return false;

  return constant_time_compare(token, expected_token, expected_len);
}

static bool api_auth_check_optional(httpd_req_t* req, const char* expected_token) {
  if (cv_session_validate && cv_session_validate(req)) return true;
  return api_auth_bearer_valid(req, expected_token);
}

// ═══════════════════════════════════════════════════════════════════
// THE RECEIPT'S BEARER GRANT — the lockout kept, nothing sent
// ═══════════════════════════════════════════════════════════════════

// The bearer decision under the lockout, nothing sent (auth_backoff.h's
// grant() over this request's source): NONE with no Authorization header
// (nothing recorded: auth_logic.h, no credential carries no guess); HELD
// while the source's backoff runs (the header is not compared and nothing is
// recorded, so a guess sent during it learns nothing); OK for the right token
// (that source's slot cleared, as every admin route clears it); WRONG for a
// malformed header or a wrong token (one strike against that source, as on
// every admin route).

static canary::net::auth_backoff::BearerResult api_auth_bearer_result(httpd_req_t* req,
                                                                     const char* expected_token) {
  const uint32_t source = auth_request_source(req);
  const uint32_t now = millis();
  const canary::net::auth_backoff::BearerResult r = canary::net::auth_backoff::grant(
      g_auth_table, source, now, httpd_req_get_hdr_value_len(req, "Authorization") != 0,
      [&]() { return api_auth_bearer_valid(req, expected_token); });
  if (r == canary::net::auth_backoff::BearerResult::WRONG) auth_log_strike(source, now);
  return r;
}

// GET /api/provisioning-receipt's first grant (sweep F289's follow-up). The
// bearer header alone, as api_auth_bearer_valid() reads it, with
// api_auth_check_bearer()'s lockout and failure accounting but no answer
// sent, so the route's other grants (the setup-door claim, the BOOT tap)
// still decide what a refused bearer gets. True only for the right token
// outside its source's backoff (api_auth_bearer_result() is OK). Before
// F289's follow-up the receipt compared silently, so it was the one route
// where guessing the token cost nothing.
// The lockout never gates the claim or the tap: a page holding a stale token,
// or a backoff a guesser armed, is still served by the claim or a BOOT tap.
// (What the lockout does not decide: any host on the network can spend a
// pending claim with one wrong ?claim=, or take an open tap by asking first.)

static bool api_auth_bearer_grant(httpd_req_t* req, const char* expected_token) {
  return api_auth_bearer_result(req, expected_token) == canary::net::auth_backoff::BearerResult::OK;
}

// Whether `req` presents an Authorization header while its source's backoff
// runs: the bearer api_auth_bearer_grant() will not compare. The receipt
// reads it before its grant, so the guess that arms the backoff is refused
// like any wrong token (403) and only the ones sent during it get the 429.

static bool api_auth_bearer_held(httpd_req_t* req) {
  return httpd_req_get_hdr_value_len(req, "Authorization") != 0 && auth_is_locked_out(req);
}


static bool api_auth_check_or_query(httpd_req_t* req, const char* expected_token, const char* query_key = "token") {
  // Session-cookie auth grants access but must not reset the token
  // failure lockout (see api_auth_check). Check it first, separately
  // from the token paths, so it never clears a source's backoff.
  if (cv_session_validate && cv_session_validate(req)) {
    return true;
  }

  if (api_auth_check_optional(req, expected_token)) {
    auth_record_success(req);
    return true;
  }

  bool presented_credential =
      httpd_req_get_hdr_value_len(req, "Authorization") > 0;

  char qs[192];
  esp_err_t qerr = httpd_req_get_url_query_str(req, qs, sizeof(qs));
  if (qerr == ESP_OK) {
    char token[128];
    if (httpd_query_key_value(qs, query_key, token, sizeof(token)) == ESP_OK) {
      presented_credential = true;
      size_t token_len = strlen(token);
      size_t expected_len = strlen(expected_token);
      if (expected_len != 0 && token_len == expected_len &&
          constant_time_compare(token, expected_token, expected_len)) {
        auth_record_success(req);
        return true;
      }
    }
  }

  // Only an actual guess (header or query token presented) advances the
  // brute-force lockout; a bare request is a plain 401 (see auth_logic.h).
  if (auth_logic::counts_toward_lockout(
          presented_credential ? auth_logic::Attempt::WRONG_TOKEN
                               : auth_logic::Attempt::NO_CREDENTIAL)) {
    auth_record_failure(req);
  }
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"securacv\"");
  httpd_resp_sendstr(req, "{\"error\":\"unauthorized\",\"hint\":\"Use Authorization header or token query param\"}");
  return false;
}
#endif // SECURACV_API_AUTH_H
