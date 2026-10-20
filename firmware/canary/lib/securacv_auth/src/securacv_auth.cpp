/*
 * SecuraCV Canary — API Authentication Implementation
 *
 * See securacv_auth.h for the module contract.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_auth.h"
#include "securacv_crypto.h"
#include "canary_config.h"

#include <string.h>

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL INSTANCE
// ════════════════════════════════════════════════════════════════════════════

static AuthManager s_auth;

AuthManager& auth_get_instance() {
  return s_auth;
}

// ════════════════════════════════════════════════════════════════════════════
// AuthManager
// ════════════════════════════════════════════════════════════════════════════

AuthManager::AuthManager() : m_table{} {}

void AuthManager::reset() {
  m_table = canary::net::auth_backoff::Table{};
}

bool AuthManager::constantTimeCompare(const void* a, const void* b, size_t len) {
  // Always visit every byte. Cast through volatile so the optimizer cannot
  // short-circuit once a mismatch is detected.
  const volatile uint8_t* pa = static_cast<const volatile uint8_t*>(a);
  const volatile uint8_t* pb = static_cast<const volatile uint8_t*>(b);
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) {
    diff |= pa[i] ^ pb[i];
  }
  return diff == 0;
}

void AuthManager::handedToken(uint32_t source) {
  canary::net::auth_backoff::record_success(m_table, source);
}

void AuthManager::redact(const char* token, char* out, size_t out_len) {
  if (!out || out_len == 0) return;
  if (!token || strlen(token) < 8 || out_len < 13) {
    const char* fallback = "cv_****";
    size_t n = strlen(fallback);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, fallback, n);
    out[n] = '\0';
    return;
  }
  snprintf(out, out_len, "%.8s****", token);
}

// ════════════════════════════════════════════════════════════════════════════
// AUTH CHECKS (esp_http_server)
// ════════════════════════════════════════════════════════════════════════════

static void send_json_error(httpd_req_t* req, const char* status_str,
                            const char* body) {
  httpd_resp_set_status(req, status_str);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, body);
}

// The lockout's 429 for `source`: Retry-After is the seconds left of its
// backoff, rounded up past the end (remaining / 1000 + 1), so at least 1 and,
// with auth_backoff.h's 60 s cap, never more than 61. The strings live until
// the send, which copies them.
void AuthManager::sendLockout(httpd_req_t* req, uint32_t source) {
  const uint32_t remaining_ms =
      canary::net::auth_backoff::remaining_ms(m_table, source, millis());
  const uint32_t retry_s = (remaining_ms / 1000) + 1;
  char retry_str[12];
  snprintf(retry_str, sizeof(retry_str), "%lu", (unsigned long)retry_s);

  httpd_resp_set_status(req, "429 Too Many Requests");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Retry-After", retry_str);
  char body[128];
  snprintf(body, sizeof(body),
           "{\"error\":\"too_many_requests\",\"retry_after_s\":%s}",
           retry_str);
  httpd_resp_sendstr(req, body);
}

AuthManager::Presented AuthManager::compare(httpd_req_t* req, const char* expected_token) {
  size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (auth_len >= SECURACV_AUTH_HEADER_BUF) return Presented::TOO_LONG;

  char auth_buf[SECURACV_AUTH_HEADER_BUF];
  if (httpd_req_get_hdr_value_str(req, "Authorization", auth_buf,
                                  sizeof(auth_buf)) != ESP_OK) {
    return Presented::UNREADABLE;
  }

  // ── Validate Bearer prefix ──
  if (strncmp(auth_buf, "Bearer ", 7) != 0) return Presented::NOT_BEARER;

  // ── Trim whitespace ──
  const char* token = auth_buf + 7;
  while (*token == ' ') token++;
  size_t token_len = strlen(token);
  while (token_len > 0 && token[token_len - 1] == ' ') token_len--;

  size_t expected_len = strlen(expected_token);

  // An empty expected token matches nothing: a zero-length constant-time
  // compare is equal, so "Bearer " would otherwise pass. The callers refuse
  // an unprovisioned credential before they get here (auth_gate's 503,
  // bearer_present_and_valid's guard); this is the library failing closed
  // on its own.
  if (expected_len == 0) return Presented::WRONG;

  // Length mismatch is a hard no — comparison below would be incorrect, but
  // we still want constant-time behavior on the matching-length path. A
  // length mismatch leaks only that the token is wrong-length, which is
  // acceptable and matches industry norms.
  if (token_len != expected_len) return Presented::WRONG;

  return constantTimeCompare(token, expected_token, expected_len) ? Presented::RIGHT
                                                                   : Presented::WRONG;
}

AuthManager::BearerResult AuthManager::grant(httpd_req_t* req, const char* expected_token,
                                             uint32_t source) {
  if (!req || !expected_token) return BearerResult::NONE;
  const bool presented = httpd_req_get_hdr_value_len(req, "Authorization") != 0;
  return canary::net::auth_backoff::grant(
      m_table, source, millis(), presented,
      [&]() { return compare(req, expected_token) == Presented::RIGHT; });
}

bool AuthManager::check(httpd_req_t* req, const char* expected_token, uint32_t source) {
  if (!req || !expected_token) return false;

  // ── Lockout check ── first, so while `source`'s backoff runs even a
  // request with no header is the 429 (the grant below would answer NONE).
  const uint32_t now = millis();
  if (canary::net::auth_backoff::locked(m_table, source, now)) {
    sendLockout(req, source);
    return false;
  }

  // ── The accounted grant ── one strike for a malformed or wrong
  // credential, `source` freed for the right one. `why` is what the one
  // compare saw, for the refusal's body.
  Presented why = Presented::WRONG;
  const bool presented = httpd_req_get_hdr_value_len(req, "Authorization") != 0;
  const BearerResult result = canary::net::auth_backoff::grant(
      m_table, source, now, presented, [&]() {
        why = compare(req, expected_token);
        return why == Presented::RIGHT;
      });

  switch (result) {
    case BearerResult::OK:
      return true;
    case BearerResult::HELD:
      // Unreachable: the check above, at the same `now`, found `source`
      // free. Answered as the lockout, never served.
      sendLockout(req, source);
      return false;
    case BearerResult::NONE:
      // No strike: a request with no credential carries no guess (the WAP's
      // rule, canary-wap's auth_logic.h). Counting it let the dashboard's
      // own token-less tab, polling /api/status every 2 s, hold every bearer
      // caller at 429 (sweep F340). A malformed or wrong one below still is.
      httpd_resp_set_status(req, "401 Unauthorized");
      httpd_resp_set_type(req, "application/json");
      httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"securacv\"");
      httpd_resp_sendstr(req,
          "{\"error\":\"unauthorized\","
          "\"hint\":\"Include 'Authorization: Bearer cv_...' header\"}");
      return false;
    case BearerResult::WRONG:
      break;
  }

  switch (why) {
    case Presented::TOO_LONG:
      send_json_error(req, "401 Unauthorized",
                      "{\"error\":\"unauthorized\","
                      "\"hint\":\"Authorization header too long\"}");
      break;
    case Presented::UNREADABLE:
      send_json_error(req, "401 Unauthorized", "{\"error\":\"unauthorized\"}");
      break;
    case Presented::NOT_BEARER:
      send_json_error(req, "401 Unauthorized",
                      "{\"error\":\"unauthorized\","
                      "\"hint\":\"Use 'Bearer <token>' format\"}");
      break;
    case Presented::RIGHT:
    case Presented::WRONG:
      send_json_error(req, "403 Forbidden", "{\"error\":\"forbidden\"}");
      break;
  }
  return false;
}

// ════════════════════════════════════════════════════════════════════════════
// CONVENIENCE WRAPPERS
// ════════════════════════════════════════════════════════════════════════════

bool auth_check(httpd_req_t* req, const char* expected_token, uint32_t source) {
  return auth_get_instance().check(req, expected_token, source);
}

AuthManager::BearerResult auth_bearer_grant(httpd_req_t* req, const char* expected_token,
                                            uint32_t source) {
  return auth_get_instance().grant(req, expected_token, source);
}

void auth_handed_token(uint32_t source) {
  auth_get_instance().handedToken(source);
}

void auth_redact_token(const char* token, char* out, size_t out_len) {
  AuthManager::redact(token, out, out_len);
}

// ════════════════════════════════════════════════════════════════════════════
// BEARER CREDENTIAL OWNERSHIP
// ════════════════════════════════════════════════════════════════════════════

static char s_bearer[36];  // "cv_" + 32 base62 chars + NUL

bool auth_load_or_derive(const uint8_t privkey[32]) {
  if (!privkey) return false;

  NvsManager& nvs = NvsManager::instance();
  bool loaded = false;

  if (nvs.beginReadOnly()) {
    size_t tlen = nvs.getBytesLength(NVS_KEY_TOKEN);
    if (tlen > 0 && tlen < sizeof(s_bearer)) {
      size_t got = nvs.getBytes(NVS_KEY_TOKEN, s_bearer, tlen);
      if (got == tlen) {
        s_bearer[tlen] = '\0';
        loaded = (strncmp(s_bearer, "cv_", 3) == 0 && strlen(s_bearer) >= 35);
      } else {
        // Partial read — treat as absent so we re-derive rather than
        // trusting whatever partial bytes landed in s_bearer.
        s_bearer[0] = '\0';
      }
    }
    nvs.end();
  }

  if (!loaded) {
    if (!derive_api_token(privkey, s_bearer, sizeof(s_bearer))) {
      s_bearer[0] = '\0';
      return false;
    }
    if (nvs.beginReadWrite()) {
      size_t wrote = nvs.putBytes(NVS_KEY_TOKEN, s_bearer, strlen(s_bearer));
      nvs.end();
      if (wrote == 0) {
        // Persistence failed. Token derivation is deterministic in the
        // signing key + MAC, so the same credential will be re-derived
        // next boot — client-side cached tokens remain valid.
        Serial.println("[!!] Bearer credential NVS persist failed "
                       "(will re-derive on next boot)");
      }
    }
  }

  return s_bearer[0] != '\0';
}

const char* auth_get_token() {
  return s_bearer;
}
