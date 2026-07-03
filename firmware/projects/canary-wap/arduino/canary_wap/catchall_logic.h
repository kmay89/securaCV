/*
 * SecuraCV Canary WAP — pure canary.local catch-all decisions (host-testable)
 *
 * Arduino-free: stdint only. The mDNS calls (probe/claim/withdraw) live in
 * canary_wap.ino; every branchy DECISION around WHO should own the shared
 * "canary.local" catch-all lives here so a host g++ run
 * (test_catchall_logic.cpp) can pin it.
 *
 * Why this exists: the catch-all used to be claimed with a single 600 ms
 * first-wins probe at STA join. Two Canaries powering up together (power
 * restored after an outage — the common multi-device case) each probed while
 * the other wasn't answering yet, so BOTH claimed canary.local. Which device
 * the browser reached was then arbitrary and could flip between requests,
 * invalidating the session cookie mid-use (the field symptom: dashboards
 * randomly "logged out", Fleet actions failing with a bare "Failed").
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_CATCHALL_LOGIC_H
#define SECURACV_CATCHALL_LOGIC_H

#include <stdint.h>

namespace catchall_logic {

// De-synchronize the initial claim probes of devices that boot in the same
// instant: each device waits a fingerprint-derived offset before its first
// probe, so one of them claims first and the other's probe then sees it.
// Bounds: [500, 3499] ms — small enough to not delay single-device homes
// noticeably, wide enough that two probes (600 ms window each) rarely overlap.
inline uint32_t claim_stagger_ms(uint8_t fp0, uint8_t fp1) {
  return 500u + (uint32_t)(((uint16_t)fp0 << 8) | fp1) % 3000u;
}

// Deterministic tie-break when a conflict IS detected (both devices ended up
// claiming): each side compares its own IP with the responder's and keeps the
// claim only if it wins. Both devices evaluate the same unordered pair from
// opposite sides, so exactly one keeps it and one withdraws — no negotiation
// protocol needed. Lower numeric IP wins (any antisymmetric total order works;
// this one is stable across reboots on DHCP leases that persist).
inline bool keep_claim_on_conflict(uint32_t my_ip, uint32_t other_ip) {
  return my_ip < other_ip;
}

// Is a probe answer a *conflict*? Zero means nobody answered; an answer that
// matches one of our own interface addresses is our own (delegated) record
// echoed back, not a peer.
inline bool probe_is_conflict(uint32_t answer_ip, uint32_t my_ap_ip,
                              uint32_t my_sta_ip) {
  if (answer_ip == 0) return false;
  if (answer_ip == my_ap_ip && my_ap_ip != 0) return false;
  if (answer_ip == my_sta_ip && my_sta_ip != 0) return false;
  return true;
}

// Wrap-safe "is this periodic/deferred action due" — shared by the deferred
// initial claim and the recurring conflict check.
inline bool due(uint32_t now_ms, uint32_t scheduled_ms) {
  return (int32_t)(now_ms - scheduled_ms) >= 0;
}

}  // namespace catchall_logic

#endif  // SECURACV_CATCHALL_LOGIC_H
