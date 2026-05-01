/*
 * SecuraCV Canary — API Authentication Module
 *
 * Provides Bearer token authentication for the REST API with:
 * - Constant-time token comparison (prevents timing side-channel attacks)
 * - Exponential backoff on failed attempts (brute-force mitigation)
 * - Token redaction for safe logging
 * - Auth statistics for health monitoring
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

// ═══════════════════════════════════════════════════════════════════
// CONFIGURATION
// ═══════════════════════════════════════════════════════════════════

#define AUTH_MAX_FAILURES       5       // failures before max lockout
#define AUTH_LOCKOUT_BASE_MS    2000    // base lockout: 2 seconds
#define AUTH_LOCKOUT_MAX_MS     300000  // max lockout: 5 minutes
#define AUTH_FAILURE_WINDOW_MS  60000   // reset failure count after 1 min clean

// ═══════════════════════════════════════════════════════════════════
// STATE
// ═══════════════════════════════════════════════════════════════════

struct AuthState {
  uint8_t  consecutive_failures;
  uint32_t last_failure_ms;
  uint32_t lockout_until_ms;
  uint32_t total_failures;      // lifetime counter (for health log)
  uint32_t total_successes;
};

static AuthState g_auth = {0, 0, 0, 0, 0};

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
// EXPONENTIAL BACKOFF
// ═══════════════════════════════════════════════════════════════════

// After each failed attempt, lockout duration doubles:
//   Attempt 1: 2s
//   Attempt 2: 4s
//   Attempt 3: 8s
//   Attempt 4: 16s
//   Attempt 5: 32s → then capped at 5 minutes
//
// The failure counter resets after 1 minute without a failure.

static bool auth_is_locked_out() {
  if (g_auth.consecutive_failures == 0) return false;

  // Reset failures if clean for AUTH_FAILURE_WINDOW_MS
  if (millis() - g_auth.last_failure_ms > AUTH_FAILURE_WINDOW_MS) {
    g_auth.consecutive_failures = 0;
    return false;
  }

  if (millis() < g_auth.lockout_until_ms) {
    return true;
  }

  return false;
}

static void auth_record_failure() {
  g_auth.consecutive_failures++;
  g_auth.total_failures++;
  g_auth.last_failure_ms = millis();

  // Exponential backoff: 2^(failures) * base, capped
  uint32_t lockout_ms = AUTH_LOCKOUT_BASE_MS;
  for (int i = 1; i < g_auth.consecutive_failures && i < 10; i++) {
    lockout_ms *= 2;
    if (lockout_ms > AUTH_LOCKOUT_MAX_MS) {
      lockout_ms = AUTH_LOCKOUT_MAX_MS;
      break;
    }
  }
  g_auth.lockout_until_ms = millis() + lockout_ms;

  Serial.printf("[AUTH] Failed attempt #%d. Lockout: %lu ms\n",
                g_auth.consecutive_failures, (unsigned long)lockout_ms);
}

static void auth_record_success() {
  g_auth.consecutive_failures = 0;
  g_auth.total_successes++;
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

// Returns true if authenticated, false if response already sent (401/403/429).
// The caller should return ESP_OK immediately if this returns false.

static bool api_auth_check(httpd_req_t* req, const char* expected_token) {
  // ── Check lockout ──
  if (auth_is_locked_out()) {
    uint32_t remaining = g_auth.lockout_until_ms - millis();
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
    return false;
  }

  // ── Extract Authorization header ──
  size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (auth_len == 0) {
    auth_record_failure();
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"securacv\"");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\",\"hint\":\"Include 'Authorization: Bearer cv_...' header\"}");
    return false;
  }

  char auth_buf[128];
  if (auth_len >= sizeof(auth_buf)) {
    auth_record_failure();
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\",\"hint\":\"Authorization header too long\"}");
    return false;
  }

  httpd_req_get_hdr_value_str(req, "Authorization", auth_buf, sizeof(auth_buf));

  // ── Validate Bearer prefix ──
  if (strncmp(auth_buf, "Bearer ", 7) != 0) {
    auth_record_failure();
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

  size_t expected_len = strlen(expected_token);
  if (token_len != expected_len) {
    auth_record_failure();
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"forbidden\"}");
    return false;
  }

  if (!constant_time_compare(token, expected_token, expected_len)) {
    auth_record_failure();
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"forbidden\"}");
    return false;
  }

  // ── Success ──
  auth_record_success();
  return true;
}

// ═══════════════════════════════════════════════════════════════════
// OPTIONAL AUTH CHECK — Returns true if valid token present
// ═══════════════════════════════════════════════════════════════════

// Like api_auth_check but does NOT send error response on failure.
// Used for endpoints that allow both authenticated and unauthenticated access
// (e.g., provisioning-receipt).

static bool api_auth_check_optional(httpd_req_t* req, const char* expected_token) {
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

// ═══════════════════════════════════════════════════════════════════
// AUTH STATS (for health endpoint)
// ═══════════════════════════════════════════════════════════════════

static void auth_stats_json(char* buf, size_t buf_len) {
  bool locked = auth_is_locked_out();
  if (locked) {
    uint32_t remaining = (g_auth.lockout_until_ms - millis()) / 1000;
    snprintf(buf, buf_len,
      "{\"total_successes\":%lu,\"total_failures\":%lu,"
      "\"consecutive_failures\":%u,\"locked_out\":true,"
      "\"lockout_remaining_s\":%lu}",
      (unsigned long)g_auth.total_successes,
      (unsigned long)g_auth.total_failures,
      g_auth.consecutive_failures,
      (unsigned long)remaining);
  } else {
    snprintf(buf, buf_len,
      "{\"total_successes\":%lu,\"total_failures\":%lu,"
      "\"consecutive_failures\":%u,\"locked_out\":false}",
      (unsigned long)g_auth.total_successes,
      (unsigned long)g_auth.total_failures,
      g_auth.consecutive_failures);
  }
}


static bool api_auth_check_or_query(httpd_req_t* req, const char* expected_token, const char* query_key = "token") {
  if (api_auth_check_optional(req, expected_token)) {
    auth_record_success();
    return true;
  }

  char qs[192];
  esp_err_t qerr = httpd_req_get_url_query_str(req, qs, sizeof(qs));
  if (qerr == ESP_OK) {
    char token[128];
    if (httpd_query_key_value(qs, query_key, token, sizeof(token)) == ESP_OK) {
      size_t token_len = strlen(token);
      size_t expected_len = strlen(expected_token);
      if (token_len == expected_len && constant_time_compare(token, expected_token, expected_len)) {
        auth_record_success();
        return true;
      }
    }
  }

  auth_record_failure();
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"securacv\"");
  httpd_resp_sendstr(req, "{\"error\":\"unauthorized\",\"hint\":\"Use Authorization header or token query param\"}");
  return false;
}
#endif // SECURACV_API_AUTH_H
