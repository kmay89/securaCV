/*
 * SecuraCV Canary — why the STA join failed, in the fleet's words, and when
 * a failing saved network gives way to the next one.
 *
 * The canary's STA supervisor (securacv_network.cpp checkConnection) keeps
 * its own shape — a 30 s connect window and a 2 → 30 s doubling backoff —
 * so it is not a wifi_join_policy.h consumer the way the display and the
 * headless trio are (scripts/lint_wifi_join_policy.py lists those). It does
 * speak the policy's WORDS: the four JoinFailure verdicts and their labels
 * are what GET /api/wifi/status reports as `reason`, so a phone reads one
 * vocabulary off every Canary.
 *
 * The mapping is the sibling trees' three-line wl_status_t switch
 * (canary-display wifi_mgr.cpp, common/network/wifi_supervisor.cpp) PLUS the
 * raw disconnect reason, because on Arduino-ESP32 core 2.0.17 — the core the
 * dev/release envs build on — a wrong password usually ends in a 4-way
 * handshake timeout (reason 15) or a handshake timeout (204), which the core
 * files under WL_CONNECTION_LOST / WL_DISCONNECTED, not WL_CONNECT_FAILED.
 * Read off the status alone, the single most common setup mistake would
 * answer "Couldn't connect" instead of "Wrong password".
 *
 * Pure: no Arduino. The wl_status_t and wifi_err_reason_t values are spelled
 * here as integers so a host test can drive the classifier, and
 * securacv_network.cpp static_asserts each one against the real enum (the
 * ap_security_policy.h pattern). firmware/tests_host/test_canary_onboarding_glue.cpp.
 */

#pragma once

#include <stdint.h>

#include "network/wifi_join_policy.h"
#include "network/wifi_store.h"

namespace canary {
namespace net {
namespace join_reason {

// wl_status_t (WiFiType.h) — asserted against the enum in securacv_network.cpp.
inline constexpr int kWlIdleStatus    = 0;  ///< associated, no address yet
inline constexpr int kWlNoSsidAvail   = 1;
inline constexpr int kWlConnectFailed = 4;

// wifi_err_reason_t (esp_wifi_types.h) — asserted the same way.
inline constexpr int kReasonNone                 = 0;    ///< no disconnect seen this attempt
inline constexpr int kReasonAuthExpire           = 2;
inline constexpr int kReason4wayHandshakeTimeout = 15;
inline constexpr int kReasonNoApFound            = 201;
inline constexpr int kReasonAuthFail             = 202;
inline constexpr int kReasonHandshakeTimeout     = 204;

/**
 * @brief The verdict for a join that did not end in an address.
 *
 * @param wl_status               WiFi.status() when the attempt was given up
 * @param last_disconnect_reason  the reason of the most recent STA_DISCONNECTED
 *                                event during this attempt (kReasonNone if none)
 */
inline JoinFailure classify(int wl_status, int last_disconnect_reason) {
  switch (wl_status) {
    case kWlNoSsidAvail:   return JoinFailure::NotFound;
    case kWlConnectFailed: return JoinFailure::BadPassword;
    case kWlIdleStatus:    return JoinFailure::NoAddress;
    default:               break;
  }
  switch (last_disconnect_reason) {
    case kReasonNoApFound:            return JoinFailure::NotFound;
    case kReasonAuthExpire:
    case kReason4wayHandshakeTimeout:
    case kReasonAuthFail:
    case kReasonHandshakeTimeout:     return JoinFailure::BadPassword;
    default:                          break;
  }
  return JoinFailure::Unknown;
}

/// Consecutive NotFound verdicts on one saved network before the supervisor
/// moves to the next saved one (wifi_store::pick round-robin).
inline constexpr uint8_t kRotateAfterFails = 2;

/**
 * @brief Should the supervisor try the next saved network instead of this one?
 *
 * Only when there IS a next one, only for NotFound (a wrong password is
 * fixed by a human, not by another network; a silent DHCP server is the
 * router's), and only after kRotateAfterFails misses in a row — a slow AP
 * coming back from a power cut must not lose its place on the first miss.
 */
inline bool should_rotate(const wifi_store::Store& s, int idx, JoinFailure f) {
  if (s.count < 2) return false;
  if (f != JoinFailure::NotFound) return false;
  if (idx < 0 || idx >= (int)s.count) return false;
  return s.nets[idx].fails >= kRotateAfterFails;
}

}  // namespace join_reason
}  // namespace net
}  // namespace canary
