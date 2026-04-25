/*
 * SecuraCV Canary — Test harness + red-team scenarios (Phase 11)
 * Version 0.1.0
 *
 * Two purposes:
 *
 * 1. UNIFIED CONFORMANCE
 *    Each Phase 4-10 module exposes a conformance_self_test() that
 *    exercises its own invariants. This harness calls them all and
 *    aggregates the results so a single boot-time call (or HTTP
 *    poke) can verify the whole stack.
 *
 * 2. RED-TEAM SCENARIOS
 *    Self-tests prove a module behaves correctly on intended inputs.
 *    Red-team tests prove the privacy / security invariants hold
 *    against ADVERSARIAL inputs:
 *
 *    a) MAC randomization replay — stream 1024 distinct random MACs
 *       through rf_presence::feed_ble_scan; verify the token map
 *       stays bounded (≤ SESSION_TOKEN_MAP_SIZE), no MAC bytes leak
 *       into observation aggregates, and rotation wipes everything.
 *       Models the "unique-MAC-per-advertisement" attack vector
 *       documented in the 2024 ACM allowlist-side-channel paper.
 *
 *    b) Cloned RPA — synthesize a Resolvable Private Address that
 *       does NOT match any enrolled IRK, verify household::resolve_rpa
 *       correctly rejects it (otherwise an attacker could forge the
 *       neighbor's RPA and walk into our home unchallenged).
 *
 *    c) Federated baseline poisoning — submit a peer share with
 *       count = 65535 and absurdly large sum_sq, verify
 *       baseline::merge_remote_bucket clamps to REMOTE_MERGE_MAX_COUNT
 *       and uses the int64 pre-clamp so neither overflow nor distribution
 *       takeover succeeds.
 *
 * The scenarios run in <100 ms total on ESP32-S3, fast enough to
 * include in the boot self-test alongside the existing conformance
 * suite. They can also be triggered on demand by a future HTTP
 * /api/test/run endpoint (deferred to follow-up wiring).
 *
 * WHAT THIS PR DOES NOT DO
 * ========================
 *   • Bench rig: a CSI ground-truth dataset (pedestrian motion,
 *     stillness, breathing, empty room) is operational, not firmware.
 *   • Field corpus: a 2-week real-home recording for neighbor-
 *     suppression validation is operational, not firmware.
 *   • RPA-spoof attack with a real BLE radio: requires an external
 *     attacker device. The clone here is a synthetic-MAC test that
 *     proves the resolve_rpa logic is correct; the radio-level test
 *     belongs in lab follow-up.
 */

#ifndef SECURACV_TESTS_H
#define SECURACV_TESTS_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

namespace tests {

// ════════════════════════════════════════════════════════════════════════════
// RESULT TYPES
// ════════════════════════════════════════════════════════════════════════════

// A single named result. Names are short fixed strings so the aggregate
// fits in ~256 bytes for HTTP / log emission.
struct NamedResult {
  const char* name;
  bool        passed;
};

// Aggregate report from run_all(). The arrays are sized for the current
// module count; bump if you add a module.
static const size_t MAX_MODULE_RESULTS  = 8;   // household, familiar, baseline,
                                                //  notify, federated, wizard, dp, csi_hal
static const size_t MAX_REDTEAM_RESULTS = 4;   // mac-replay, cloned-rpa, federated-poison
                                                //  + headroom

struct Report {
  NamedResult conformance[MAX_MODULE_RESULTS];
  uint8_t     conformance_count;
  uint8_t     conformance_passed;

  NamedResult red_team[MAX_REDTEAM_RESULTS];
  uint8_t     red_team_count;
  uint8_t     red_team_passed;

  uint32_t    total_ms;        // wall-time of the full run
  bool        all_passed;      // both groups all-passed
};

// ════════════════════════════════════════════════════════════════════════════
// RUN
// ════════════════════════════════════════════════════════════════════════════

// Run every module's conformance_self_test(). Pure; restores state.
bool run_all_conformance(Report* out);

// Run all red-team scenarios. Each scenario is responsible for its own
// state save / restore; they should not pollute production telemetry.
bool run_all_red_team(Report* out);

// Convenience: run conformance + red-team and fill the same report.
bool run_all(Report* out);

// ════════════════════════════════════════════════════════════════════════════
// INDIVIDUAL RED-TEAM SCENARIOS
// ════════════════════════════════════════════════════════════════════════════

// Stream N random MACs through rf_presence::feed_ble_scan and verify
// the privacy invariants hold (token map bounded, observation buffer
// has no MAC-shaped byte runs after rotation).
bool red_team_mac_randomization_replay();

// Generate an RPA from a fresh random IRK that is NOT enrolled in
// household, verify household::resolve_rpa returns false. Run with the
// fresh IRK enrolled, verify true. Cleans up the test slot afterward.
bool red_team_cloned_rpa();

// Submit a poisoned BaselineShareBucket with maximum count and out-of-
// range sums, verify baseline::merge_remote_bucket clamps. Verifies
// the int64-pre-clamp guard against compiler-undefined overflow.
bool red_team_federated_poisoning();

}  // namespace tests

#endif  // SECURACV_TESTS_H
