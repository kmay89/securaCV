/*
 * SecuraCV Canary — API Authentication
 *
 * Bearer-token authentication primitives for the REST API:
 *   - Constant-time token comparison (timing-attack safe)
 *   - Exponential-backoff lockout on repeated failures
 *   - Token redaction for safe logging
 *   - Lifetime auth counters for the health endpoint
 *
 * This library is the canary/ PlatformIO port of
 * firmware/projects/canary-wap/arduino/canary_wap/api_auth.h (Phase 1 of
 * the consolidation plan in firmware/canary/CONSOLIDATION.md). It is
 * stateful behind a class instead of file-scope static globals so the
 * module is unit-testable and safe under RTOS task interleaving.
 *
 * This library is now wired into the HTTP handlers: securacv_network.cpp's
 * auth_gate() calls auth_check() against the device-provisioned bearer token
 * on the REST endpoints (the SPA at "/" carries the token but is itself
 * ungated by design). The standalone class remains unit-testable in isolation.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_AUTH_H
#define SECURACV_AUTH_H

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

// ════════════════════════════════════════════════════════════════════════════
// CONFIGURATION (override via build_flags if needed)
// ════════════════════════════════════════════════════════════════════════════

#ifndef SECURACV_AUTH_MAX_FAILURES
#define SECURACV_AUTH_MAX_FAILURES       5        // failures before max lockout
#endif
#ifndef SECURACV_AUTH_LOCKOUT_BASE_MS
#define SECURACV_AUTH_LOCKOUT_BASE_MS    2000     // base lockout: 2 seconds
#endif
#ifndef SECURACV_AUTH_LOCKOUT_MAX_MS
#define SECURACV_AUTH_LOCKOUT_MAX_MS     300000   // max lockout: 5 minutes
#endif
#ifndef SECURACV_AUTH_FAILURE_WINDOW_MS
#define SECURACV_AUTH_FAILURE_WINDOW_MS  60000    // reset counter after 1 min clean
#endif
#ifndef SECURACV_AUTH_HEADER_BUF
#define SECURACV_AUTH_HEADER_BUF         128      // Authorization header max bytes
#endif

// ════════════════════════════════════════════════════════════════════════════
// AUTH MANAGER
// ════════════════════════════════════════════════════════════════════════════

struct AuthStats {
  uint32_t total_successes;
  uint32_t total_failures;
  uint8_t  consecutive_failures;
  bool     locked_out;
  uint32_t lockout_remaining_ms;
};

class AuthManager {
 public:
  AuthManager();

  // Check Bearer token on an esp_http_server request.
  //   - Returns true on success.
  //   - On failure, sends the appropriate error response (401/403/429),
  //     records the failure, and returns false. The caller should return
  //     ESP_OK immediately after a false return.
  bool check(httpd_req_t* req, const char* expected_token);

  // Same as check() but does NOT send an error response on failure.
  // Used by endpoints that allow both authenticated and unauthenticated
  // access (e.g., provisioning-receipt gated by BOOT button).
  bool checkOptional(httpd_req_t* req, const char* expected_token);

  // Redact a token for logging: "cv_7kX9mN2p..." → "cv_7kX9****".
  // Always null-terminates output when out_len >= 1.
  static void redact(const char* token, char* out, size_t out_len);

  // Snapshot of current auth counters (for /api/status or health log).
  AuthStats stats() const;

  // Serialize stats() as compact JSON into buf.
  // Always null-terminates when buf_len >= 1.
  void statsJson(char* buf, size_t buf_len) const;

  // Testing / factory reset hook.
  void reset();

  // Constant-time memory comparison. Always visits every byte regardless
  // of early mismatches to prevent timing side-channels.
  static bool constantTimeCompare(const void* a, const void* b, size_t len);

 private:
  bool isLockedOut() const;
  void recordFailure();
  void recordSuccess();

  uint8_t  m_consecutive_failures;
  uint32_t m_last_failure_ms;
  uint32_t m_lockout_until_ms;
  uint32_t m_total_failures;
  uint32_t m_total_successes;
};

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL INSTANCE + CONVENIENCE
// ════════════════════════════════════════════════════════════════════════════

AuthManager& auth_get_instance();

// Convenience wrappers over the global instance. These match the function
// surface of the Arduino-IDE api_auth.h so Phase-2 wiring is a 1:1 call.
bool auth_check(httpd_req_t* req, const char* expected_token);
bool auth_check_optional(httpd_req_t* req, const char* expected_token);
void auth_redact_token(const char* token, char* out, size_t out_len);
void auth_stats_json(char* buf, size_t buf_len);

// ════════════════════════════════════════════════════════════════════════════
// BEARER CREDENTIAL OWNERSHIP
// ════════════════════════════════════════════════════════════════════════════
// The bearer credential is derived from the device's signing key but lives
// here, not in the witness module — that boundary is enforced by
// regression_check.sh so the credential cannot be pulled into the chain
// payload by accident.

// Load the bearer credential from NVS, or derive it via HKDF and persist
// it on first boot. Returns true if the credential is ready to use.
bool auth_load_or_derive(const uint8_t privkey[32]);

// Read the current bearer credential. Returns an empty string if
// auth_load_or_derive() has not yet succeeded on this boot.
const char* auth_get_token();

#endif  // SECURACV_AUTH_H
