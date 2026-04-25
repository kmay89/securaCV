/*
 * SecuraCV Canary — Federated mesh aggregation (Phase 9)
 * Version 0.1.0
 *
 * A multi-canary household converges faster and shares ambient-neighbor
 * fingerprints when devices on the Opera mesh exchange aggregate state.
 * This module defines the wire format and the safe build / merge paths.
 *
 * WHAT IS SHARED
 * ==============
 *   • Per-bucket baseline contributions: (count, sum[F], sum_sq[F])
 *     — sufficient statistics for the Welford Gaussian fit. A peer's
 *     contribution merges into ours by addition; the joint Gaussian is
 *     computed from the joined sums.
 *   • Familiar "yesterday" Bloom filter: a 256-byte snapshot. Peer's
 *     filter merges into ours via bitwise OR (set union of "ambient
 *     fingerprints recently seen").
 *
 * WHAT IS NEVER SHARED
 * ====================
 *   • Raw CSI samples (already not stored anywhere).
 *   • Household IRKs — those are intentionally per-device. The user
 *     enrolls each phone on each canary that should silence it.
 *   • The "always ignore" filter — that's user-personal.
 *   • Session tokens, MACs, fingerprints (only the BUCKETED Bloom
 *     image of fingerprints is shared).
 *   • Wall-clock timestamps. Shares carry no time information beyond
 *     the rotation epoch they apply to.
 *
 * PRIVACY MECHANISMS
 * ==================
 *   • Every share is built through dp::* so per-bucket counts and sums
 *     carry calibrated Gaussian noise BEFORE leaving the device.
 *   • Merge bounds: each peer can contribute at most
 *     baseline::REMOTE_MERGE_MAX_COUNT samples per bucket per merge,
 *     so a malicious peer cannot dominate our local distribution.
 *   • Cadence: shares are emitted at most once per rf_presence session
 *     rotation (4 h). The Phase 7 DP budget resets in lock-step, so
 *     each share consumes a bounded ε per epoch.
 *
 * TRANSPORT
 * =========
 * This module does NOT touch the mesh radio. It exposes:
 *   • build_baseline_share(out)    — produce a noised local snapshot
 *   • build_familiar_share(out)    — produce a noised yesterday Bloom
 *   • handle_baseline_share(in)    — merge a peer's baseline share
 *   • handle_familiar_share(in)    — merge a peer's familiar share
 * The mesh layer (Opera / canary-wap mesh_network) wires the bytes;
 * we are toolkit, not transport.
 */

#ifndef SECURACV_FEDERATED_H
#define SECURACV_FEDERATED_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

#include "baseline.h"   // baseline::BUCKET_COUNT, FEATURE_COUNT, RemoteBucketShare
#include "familiar.h"   // familiar::BLOOM_BYTES

namespace federated {

// ════════════════════════════════════════════════════════════════════════════
// CONSTANTS
// ════════════════════════════════════════════════════════════════════════════

// Bumped on any wire-format change. Mismatched-version shares are rejected.
static const uint8_t  WIRE_VERSION_BASELINE = 1;
static const uint8_t  WIRE_VERSION_FAMILIAR = 1;

// Magic bytes prefix to disambiguate share types in a generic mesh frame.
static const uint32_t MAGIC_BASELINE = 0x46424553UL;  // 'FBES'  (Federated BasELine)
static const uint32_t MAGIC_FAMILIAR = 0x46464D52UL;  // 'FFMR'  (Federated FaMiliaR)

// Minimum interval between outbound share builds. Matches the rf_presence
// session rotation period so shares and DP budgets stay in lock-step.
static const uint32_t SHARE_BUILD_MIN_INTERVAL_MS = 4UL * 60UL * 60UL * 1000UL;

// Sensitivity bounds on per-feature sum noise. A single observation can
// shift sum by at most this much; we calibrate dp Gaussian noise at it.
// (csi_motion 0..100, ble_count 0..255, |rssi| ≤ 127, rssi_spread 0..127)
static const uint32_t SUM_SENSITIVITY[baseline::FEATURE_COUNT] = {
  100, 255, 127, 127
};

// ════════════════════════════════════════════════════════════════════════════
// WIRE TYPES
//
// These structs are used both in-RAM and on-the-wire. They are
// __attribute__((packed)) so the byte layout is stable across compilers
// and ESP32 variants. The mesh transport layer is responsible for any
// envelope authentication / integrity (e.g. Ed25519 over MAGIC || body).
// ════════════════════════════════════════════════════════════════════════════

struct __attribute__((packed)) BaselineShareBucket {
  uint16_t count;
  int32_t  sum    [baseline::FEATURE_COUNT];
  int64_t  sum_sq [baseline::FEATURE_COUNT];
};

struct __attribute__((packed)) BaselineShare {
  uint32_t magic;                                        // = MAGIC_BASELINE
  uint8_t  wire_version;                                 // = WIRE_VERSION_BASELINE
  uint8_t  bucket_count;                                 // = baseline::BUCKET_COUNT
  uint8_t  feature_count;                                // = baseline::FEATURE_COUNT
  uint8_t  reserved;
  BaselineShareBucket buckets[baseline::BUCKET_COUNT];
};

struct __attribute__((packed)) FamiliarShare {
  uint32_t magic;                                        // = MAGIC_FAMILIAR
  uint8_t  wire_version;                                 // = WIRE_VERSION_FAMILIAR
  uint8_t  reserved[3];
  uint8_t  yesterday[familiar::BLOOM_BYTES];
};

struct Stats {
  uint32_t total_baseline_built;
  uint32_t total_familiar_built;
  uint32_t total_baseline_merged;
  uint32_t total_familiar_merged;
  uint32_t total_rejected_version;
  uint32_t total_rejected_magic;
  uint32_t total_rejected_size;
  uint32_t last_build_age_ms;       // ms since most recent share build
};

// ════════════════════════════════════════════════════════════════════════════
// LIFECYCLE
// ════════════════════════════════════════════════════════════════════════════

bool init();
void deinit();

// Periodic tick. Currently a no-op (sharing is pull-driven via build_*
// callers); reserved so a future internal scheduler can drive emission.
void tick(uint32_t now_ms);

// ════════════════════════════════════════════════════════════════════════════
// BUILD (outbound)
//
// Snapshot LOCAL state into a share, applying DP noise per the Phase 7
// utility. After a successful build, the build-throttle clock advances
// so subsequent build_*_share calls within SHARE_BUILD_MIN_INTERVAL_MS
// return false (caller can retry later or use force=true to bypass the
// throttle for testing).
// ════════════════════════════════════════════════════════════════════════════

bool build_baseline_share(BaselineShare* out, bool force = false);
bool build_familiar_share(FamiliarShare* out, bool force = false);

// ════════════════════════════════════════════════════════════════════════════
// HANDLE (inbound)
//
// Validate magic + version + size, then merge into local state using
// baseline::merge_remote_bucket and familiar::merge_remote_yesterday
// (both bounded — see those headers for safety properties).
// Returns false on any validation failure or merge rejection; the
// reason is logged via health_logging.
// ════════════════════════════════════════════════════════════════════════════

bool handle_baseline_share(const BaselineShare* in, size_t bytes);
bool handle_familiar_share(const FamiliarShare* in, size_t bytes);

// ════════════════════════════════════════════════════════════════════════════
// INTROSPECTION
// ════════════════════════════════════════════════════════════════════════════

bool get_stats(Stats* out);
bool get_stats_for_export(Stats* out);   // DP-noised counters

// ════════════════════════════════════════════════════════════════════════════
// CONFORMANCE
// ════════════════════════════════════════════════════════════════════════════

// End-to-end self-test:
//   • Snapshot current state, build a baseline + familiar share, merge
//     them into a separately-wiped baseline + familiar (loopback), and
//     verify counters increased and Bloom popcount went up.
//   • Reject paths: corrupt magic, wrong version, wrong size — each
//     returns false and increments the matching reject counter.
//   • Restores all original state and stats on exit.
bool conformance_self_test();

}  // namespace federated

#endif  // SECURACV_FEDERATED_H
