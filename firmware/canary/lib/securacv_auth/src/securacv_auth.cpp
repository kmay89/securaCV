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

AuthManager::AuthManager()
  : m_consecutive_failures(0),
    m_last_failure_ms(0),
    m_lockout_until_ms(0),
    m_total_failures(0),
    m_total_successes(0) {}

void AuthManager::reset() {
  m_consecutive_failures = 0;
  m_last_failure_ms = 0;
  m_lockout_until_ms = 0;
  m_total_failures = 0;
  m_total_successes = 0;
}

bool AuthManager::constantTimeCompare(const void* a, const void* b, size_t len) {
  // Always visit every byte. Cast through volatile so the optimiser cannot
  // short-circuit once a mismatch is detected.
  const volatile uint8_t* pa = static_cast<const volatile uint8_t*>(a);
  const volatile uint8_t* pb = static_cast<const volatile uint8_t*>(b);
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) {
    diff |= pa[i] ^ pb[i];
  }
  return diff == 0;
}

bool AuthManager::isLockedOut() const {
  if (m_consecutive_failures == 0) return false;

  uint32_t now = millis();

  // If the window has elapsed since the last failure with no new failure,
  // treat the counter as reset on the next recordFailure()/check().
  if (now - m_last_failure_ms > SECURACV_AUTH_FAILURE_WINDOW_MS) {
    return false;
  }

  return now < m_lockout_until_ms;
}

void AuthManager::recordFailure() {
  uint32_t now = millis();

  // Clean-window reset: if the last failure was long enough ago, start fresh.
  if (m_consecutive_failures > 0 &&
      now - m_last_failure_ms > SECURACV_AUTH_FAILURE_WINDOW_MS) {
    m_consecutive_failures = 0;
  }

  m_consecutive_failures++;
  m_total_failures++;
  m_last_failure_ms = now;

  // Exponential backoff: 2^(failures-1) * base, capped at MAX.
  uint32_t lockout_ms = SECURACV_AUTH_LOCKOUT_BASE_MS;
  for (uint8_t i = 1; i < m_consecutive_failures && i < 10; i++) {
    lockout_ms *= 2;
    if (lockout_ms > SECURACV_AUTH_LOCKOUT_MAX_MS) {
      lockout_ms = SECURACV_AUTH_LOCKOUT_MAX_MS;
      break;
    }
  }
  m_lockout_until_ms = now + lockout_ms;
}

void AuthManager::recordSuccess() {
  m_consecutive_failures = 0;
  m_total_successes++;
}

AuthStats AuthManager::stats() const {
  AuthStats s;
  s.total_successes = m_total_successes;
  s.total_failures = m_total_failures;
  s.consecutive_failures = m_consecutive_failures;

  bool locked = isLockedOut();
  s.locked_out = locked;
  s.lockout_remaining_ms = 0;
  if (locked) {
    uint32_t now = millis();
    s.lockout_remaining_ms = (m_lockout_until_ms > now)
                                 ? (m_lockout_until_ms - now)
                                 : 0;
  }
  return s;
}

void AuthManager::statsJson(char* buf, size_t buf_len) const {
  if (!buf || buf_len == 0) return;
  AuthStats s = stats();
  if (s.locked_out) {
    snprintf(buf, buf_len,
             "{\"total_successes\":%lu,\"total_failures\":%lu,"
             "\"consecutive_failures\":%u,\"locked_out\":true,"
             "\"lockout_remaining_s\":%lu}",
             (unsigned long)s.total_successes,
             (unsigned long)s.total_failures,
             s.consecutive_failures,
             (unsigned long)((s.lockout_remaining_ms + 999) / 1000));
  } else {
    snprintf(buf, buf_len,
             "{\"total_successes\":%lu,\"total_failures\":%lu,"
             "\"consecutive_failures\":%u,\"locked_out\":false}",
             (unsigned long)s.total_successes,
             (unsigned long)s.total_failures,
             s.consecutive_failures);
  }
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

bool AuthManager::check(httpd_req_t* req, const char* expected_token) {
  if (!req || !expected_token) return false;

  // ── Lockout check ──
  if (isLockedOut()) {
    uint32_t now = millis();
    uint32_t remaining_ms = (m_lockout_until_ms > now)
                                ? (m_lockout_until_ms - now)
                                : 0;
    uint32_t retry_s = (remaining_ms / 1000) + 1;
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
    return false;
  }

  // ── Extract Authorization header ──
  size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (auth_len == 0) {
    recordFailure();
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"securacv\"");
    httpd_resp_sendstr(req,
        "{\"error\":\"unauthorized\","
        "\"hint\":\"Include 'Authorization: Bearer cv_...' header\"}");
    return false;
  }

  if (auth_len >= SECURACV_AUTH_HEADER_BUF) {
    recordFailure();
    send_json_error(req, "401 Unauthorized",
                    "{\"error\":\"unauthorized\","
                    "\"hint\":\"Authorization header too long\"}");
    return false;
  }

  char auth_buf[SECURACV_AUTH_HEADER_BUF];
  if (httpd_req_get_hdr_value_str(req, "Authorization", auth_buf,
                                  sizeof(auth_buf)) != ESP_OK) {
    recordFailure();
    send_json_error(req, "401 Unauthorized",
                    "{\"error\":\"unauthorized\"}");
    return false;
  }

  // ── Validate Bearer prefix ──
  if (strncmp(auth_buf, "Bearer ", 7) != 0) {
    recordFailure();
    send_json_error(req, "401 Unauthorized",
                    "{\"error\":\"unauthorized\","
                    "\"hint\":\"Use 'Bearer <token>' format\"}");
    return false;
  }

  // ── Trim whitespace ──
  const char* token = auth_buf + 7;
  while (*token == ' ') token++;
  size_t token_len = strlen(token);
  while (token_len > 0 && token[token_len - 1] == ' ') token_len--;

  size_t expected_len = strlen(expected_token);

  // Length mismatch is a hard no — comparison below would be incorrect, but
  // we still want constant-time behavior on the matching-length path. A
  // length mismatch leaks only that the token is wrong-length, which is
  // acceptable and matches industry norms.
  if (token_len != expected_len) {
    recordFailure();
    send_json_error(req, "403 Forbidden", "{\"error\":\"forbidden\"}");
    return false;
  }

  if (!constantTimeCompare(token, expected_token, expected_len)) {
    recordFailure();
    send_json_error(req, "403 Forbidden", "{\"error\":\"forbidden\"}");
    return false;
  }

  recordSuccess();
  return true;
}

bool AuthManager::checkOptional(httpd_req_t* req, const char* expected_token) {
  if (!req || !expected_token) return false;

  size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (auth_len == 0 || auth_len >= SECURACV_AUTH_HEADER_BUF) return false;

  char auth_buf[SECURACV_AUTH_HEADER_BUF];
  if (httpd_req_get_hdr_value_str(req, "Authorization", auth_buf,
                                  sizeof(auth_buf)) != ESP_OK) {
    return false;
  }

  if (strncmp(auth_buf, "Bearer ", 7) != 0) return false;

  const char* token = auth_buf + 7;
  while (*token == ' ') token++;
  size_t token_len = strlen(token);
  while (token_len > 0 && token[token_len - 1] == ' ') token_len--;

  size_t expected_len = strlen(expected_token);
  if (token_len != expected_len) return false;

  return constantTimeCompare(token, expected_token, expected_len);
}

// ════════════════════════════════════════════════════════════════════════════
// CONVENIENCE WRAPPERS
// ════════════════════════════════════════════════════════════════════════════

bool auth_check(httpd_req_t* req, const char* expected_token) {
  return auth_get_instance().check(req, expected_token);
}

bool auth_check_optional(httpd_req_t* req, const char* expected_token) {
  return auth_get_instance().checkOptional(req, expected_token);
}

void auth_redact_token(const char* token, char* out, size_t out_len) {
  AuthManager::redact(token, out, out_len);
}

void auth_stats_json(char* buf, size_t buf_len) {
  auth_get_instance().statsJson(buf, buf_len);
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
