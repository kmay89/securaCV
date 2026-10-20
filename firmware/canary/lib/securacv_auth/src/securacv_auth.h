/*
 * SecuraCV Canary — API Authentication
 *
 * Bearer-token authentication primitives for the REST API:
 *   - Constant-time token comparison (timing-attack safe)
 *   - Exponential-backoff lockout on repeated failures, one per source
 *     address (firmware/common/network/auth_backoff.h, the WAP's table)
 *   - Token redaction for safe logging
 *
 * This library is the canary/ PlatformIO port of
 * firmware/projects/canary-wap/arduino/canary_wap/api_auth.h (Phase 1 of
 * the consolidation plan in firmware/canary/CONSOLIDATION.md). It is
 * stateful behind a class instead of file-scope static globals so the
 * module is unit-testable (firmware/tests_host/test_securacv_auth.cpp
 * compiles this library whole).
 *
 * This library is now wired into the HTTP handlers: securacv_network.cpp's
 * auth_gate() calls auth_check() against the device-provisioned bearer token
 * on the REST endpoints, and its bearer_present_and_valid() (the provisioning
 * receipt, the page-token decision) calls auth_bearer_grant(). Both pass the
 * request's source address, request_peer_ipv4(): its socket peer, read with
 * getpeername, never a header.
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
#include "network/auth_backoff.h"

// ════════════════════════════════════════════════════════════════════════════
// CONFIGURATION
// ════════════════════════════════════════════════════════════════════════════

// The backoff's numbers are firmware/common/network/auth_backoff.h's, the one
// table both firmware trees run (sweep F340): per source address, 2 s after
// the first strike and doubling, never more than 60 s, and cleared once that
// address sends no wrong token for a minute. The old per-device knobs are
// gone. A build that still sets one meant to change a number this library no
// longer reads, so it stops here instead of building with the knob ignored.
#if defined(SECURACV_AUTH_MAX_FAILURES) || defined(SECURACV_AUTH_LOCKOUT_BASE_MS) || \
    defined(SECURACV_AUTH_LOCKOUT_MAX_MS) || defined(SECURACV_AUTH_FAILURE_WINDOW_MS)
#error "SECURACV_AUTH_MAX_FAILURES, _LOCKOUT_BASE_MS, _LOCKOUT_MAX_MS and _FAILURE_WINDOW_MS are gone (sweep F340): the bearer backoff is firmware/common/network/auth_backoff.h, per source address. Remove the -D from build_flags."
#endif
#ifndef SECURACV_AUTH_HEADER_BUF
#define SECURACV_AUTH_HEADER_BUF         128      // Authorization header max bytes
#endif

// ════════════════════════════════════════════════════════════════════════════
// AUTH MANAGER
// ════════════════════════════════════════════════════════════════════════════

// One instance per device (auth_get_instance()), holding the device's one
// backoff table. One httpd task touches it: the primary server's. The port-80
// redirect server that runs beside an HTTPS one answers its captive probes
// with the TLS hint and never reaches a bearer (handle_captive_probe), so the
// table takes no lock (auth_backoff.h's contract).
class AuthManager {
 public:
  using BearerResult = canary::net::auth_backoff::BearerResult;

  AuthManager();

  // Check the Bearer token of a request from `source` (its socket peer's
  // IPv4, request_peer_ipv4() in securacv_network.cpp; 0 for every source
  // the socket cannot name) and answer every refusal.
  //   - Returns true on success, and frees `source`'s backoff.
  //   - On failure, sends the appropriate error response (401/403/429)
  //     and returns false. The caller should return ESP_OK immediately
  //     after a false return.
  //   - While `source`'s backoff runs, every request from it is the 429
  //     with Retry-After (at least 1, never more than 61), and nothing is
  //     compared or recorded. Another address is not held by it.
  //   - A malformed or wrong credential is one strike against `source`.
  //     A request with no Authorization header is a 401 and no strike: it
  //     carries no guess (sweep F340; the WAP's auth_logic.h rule).
  bool check(httpd_req_t* req, const char* expected_token, uint32_t source);

  // The accounted grant, for a route that has its own answer for a missing
  // or refused bearer (the provisioning receipt and the page-token decision,
  // through bearer_present_and_valid()). Sends nothing.
  //   NONE  — no Authorization header; nothing recorded.
  //   OK    — the right bearer; `source`'s backoff is freed.
  //   WRONG — a malformed or wrong one; one strike against `source`.
  //   HELD  — `source`'s backoff runs; nothing compared or recorded.
  BearerResult grant(httpd_req_t* req, const char* expected_token, uint32_t source);

  // For a path that hands `source` the token without a bearer (the BOOT
  // tap; sweep F341 wires those calls): its backoff ends as a right
  // bearer's would. Frees only its own tracked slot, never the shared
  // overflow (auth_backoff.h).
  void handedToken(uint32_t source);

  // Redact a token for logging: "cv_7kX9mN2p..." → "cv_7kX9****".
  // Always null-terminates output when out_len >= 1.
  static void redact(const char* token, char* out, size_t out_len);

  // Testing / factory reset hook: every backoff freed.
  void reset();

  // Constant-time memory comparison. Always visits every byte regardless
  // of early mismatches to prevent timing side-channels.
  static bool constantTimeCompare(const void* a, const void* b, size_t len);

 private:
  // What a present Authorization header holds. Only RIGHT is a match; each
  // other value is one strike, answered by check() with its own body.
  enum class Presented : uint8_t { RIGHT, TOO_LONG, UNREADABLE, NOT_BEARER, WRONG };

  // The one compare: the header against expected_token, constant time on
  // the matching-length path. An empty expected token matches nothing.
  Presented compare(httpd_req_t* req, const char* expected_token);
  void sendLockout(httpd_req_t* req, uint32_t source);

  canary::net::auth_backoff::Table m_table;
};

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL INSTANCE + CONVENIENCE
// ════════════════════════════════════════════════════════════════════════════

AuthManager& auth_get_instance();

// Convenience wrappers over the global instance. `source` is the request's
// socket peer (request_peer_ipv4() in securacv_network.cpp), passed in so
// the library reads no socket itself.
bool auth_check(httpd_req_t* req, const char* expected_token, uint32_t source);
AuthManager::BearerResult auth_bearer_grant(httpd_req_t* req, const char* expected_token,
                                            uint32_t source);
void auth_handed_token(uint32_t source);
void auth_redact_token(const char* token, char* out, size_t out_len);

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
