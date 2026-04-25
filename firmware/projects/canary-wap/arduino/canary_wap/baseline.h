/*
 * SecuraCV Canary — Adaptive baseline + anomaly scoring (Phase 6)
 * Version 0.1.0
 *
 * Learns a per-hour "what's normal" profile for the RF-sensing feature
 * vector and returns a z-score-like anomaly strength on new observations.
 *
 * HOW IT WORKS
 * ============
 * Every confirmed rf_presence_started event supplies four features:
 *     csi_motion, ble_count, rssi_mean, rssi_spread
 * We maintain one Gaussian fit per hour-of-day bucket (24 buckets, mean +
 * variance per feature) using Welford-style sum + sum-of-squares.
 *
 * Three deliberate simplifications for v1:
 *   • Hour-of-day is derived from device uptime (millis()), not wall
 *     clock. A device that runs continuously sees stable periodicity;
 *     reboots reset the buckets' phase. Sufficient for a week-long
 *     residential baseline; a wall-clock follow-up is straightforward.
 *   • No weekend/weekday split yet. Most residential traffic patterns
 *     are stable enough that a single daily profile is usable.
 *   • Gaussian per bucket, not an autoencoder. The plan aspires to a
 *     learned int8 autoencoder eventually; this v1 gets the adaptive
 *     thresholding, persistence, and Phase 8's API correct, at a
 *     fraction of the implementation risk.
 *
 * TRAINING WINDOW
 * ===============
 * First 72 h of accumulated uptime = training. During training, the
 * anomaly threshold k (in units of sigma) shrinks linearly from
 * K_TRAIN_START (=4.0) to K_TRAIN_END (=2.0). Training elapsed time
 * survives reboots via NVS persistence (updated hourly).
 *
 * PRIVACY INVARIANTS
 * ==================
 *   • Inputs are the same aggregate feature scalars rf_presence already
 *     computes — no MAC, no token, no raw RSSI, no per-second timestamp.
 *   • Bucket statistics export a single bit ("is anomaly") via the Phase
 *     8 consumer. Per-feature z-scores are available only inside the
 *     module for debugging; they never leave the firmware.
 *   • On deinit, all bucket accumulators + training counters are
 *     secure-wiped from RAM.
 */

#ifndef SECURACV_BASELINE_H
#define SECURACV_BASELINE_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

namespace baseline {

// ════════════════════════════════════════════════════════════════════════════
// CONSTANTS
// ════════════════════════════════════════════════════════════════════════════

// Number of hour-of-day buckets (24 = one per hour).
static const uint8_t BUCKET_COUNT = 24;

// Number of features tracked per bucket. Keep in sync with the Features
// struct below and the rf_presence call site.
static const uint8_t FEATURE_COUNT = 4;

// Per-bucket sample cap. Past this, bucket enters a "saturated" mode that
// keeps the Gaussian fit stable (welford-style fixed mean with exponential
// decay of M2). Prevents integer overflow on long-running devices.
static const uint16_t BUCKET_MAX_COUNT = 8000;

// Training window: 72 hours total.
static const uint32_t TRAINING_PERIOD_MS = 72UL * 60UL * 60UL * 1000UL;

// Adaptive threshold (k × sigma) during and after training. Stored as
// fixed-point / 10 (so 40 = k=4.0, 20 = k=2.0).
static const uint8_t K_TRAIN_START_X10 = 40;   // generous — few false positives
static const uint8_t K_TRAIN_END_X10   = 20;   // tight — catches real anomalies

// Interval between NVS progress persistences. Flash wear is modest at
// this rate (~24 writes/day).
static const uint32_t PERSIST_INTERVAL_MS = 60UL * 60UL * 1000UL;  // 1 hour

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

// The feature vector we track. Values are expected in their natural
// ranges: csi_motion 0..100, ble_count 0..255, rssi_mean -127..0,
// rssi_spread 0..127. The module does no further clamping — callers
// are responsible for bounded inputs.
struct Features {
  int16_t csi_motion;
  int16_t ble_count;
  int16_t rssi_mean;
  int16_t rssi_spread;
};

// Per-feature anomaly strength, expressed as z-score × 10 (fixed-point).
// Set to INT16_MAX for a feature whose bucket has fewer than 2 samples
// (variance is undefined; caller should treat as "unknown, don't fire").
struct Scores {
  int16_t z10[FEATURE_COUNT];
};

struct Stats {
  uint32_t trained_ms;                 // accumulated training uptime
  bool     training_complete;
  uint8_t  current_k_x10;              // current threshold
  uint32_t total_observations;         // cumulative observe() calls
  uint32_t total_anomaly_queries;      // cumulative is_anomaly() calls
  uint32_t total_anomaly_hits;         // of those, how many fired
  uint16_t populated_buckets;          // how many of 24 buckets have ≥ 2 samples
};

// ════════════════════════════════════════════════════════════════════════════
// LIFECYCLE
// ════════════════════════════════════════════════════════════════════════════

// Restore bucket stats + training progress from NVS. Safe before WiFi/BLE.
bool init();

// Secure-wipe all bucket accumulators + counters in RAM. Does NOT erase NVS.
void deinit();

// ════════════════════════════════════════════════════════════════════════════
// OBSERVE (hot path — called from rf_presence on each presence_started)
// ════════════════════════════════════════════════════════════════════════════

// Map a 10-minute time bucket (0..143, from rf_presence::get_time_bucket())
// to an hourly bucket (0..23).
uint8_t bucket_from_time_bucket(uint8_t time_bucket_10min);

// Accumulate one observation into the given bucket's Gaussian fit.
// Idempotent-safe for count; clamps on overflow. Also advances the
// training-progress counter and triggers periodic NVS persist.
void observe(uint8_t bucket, const Features& f);

// Main-loop tick: updates training-elapsed counter, persists progress
// when due. Safe to call at any cadence; the elapsed calculation is
// wrap-safe and uses monotonic device uptime.
void tick(uint32_t now_ms);

// ════════════════════════════════════════════════════════════════════════════
// SCORE / QUERY
// ════════════════════════════════════════════════════════════════════════════

// Compute per-feature z-scores (× 10) for `f` against the bucket's
// current Gaussian fit. Returns all INT16_MAX if bucket has < 2 samples.
Scores score(uint8_t bucket, const Features& f);

// Convenience: returns true if any feature's |z-score| exceeds the
// current adaptive threshold k(t). Returns false if bucket is under-
// populated (< 2 samples) — "unknown" isn't "anomalous".
bool is_anomaly(uint8_t bucket, const Features& f);

// The current k threshold (× 10) for anomaly decisions. Shrinks linearly
// from K_TRAIN_START_X10 → K_TRAIN_END_X10 across training, then stays
// at K_TRAIN_END_X10.
uint8_t current_k_x10();

// Training progress in basis points (0..10000). 10000 = training complete.
uint32_t training_progress_bps();

bool training_complete();

// ════════════════════════════════════════════════════════════════════════════
// INTROSPECTION + RESET
// ════════════════════════════════════════════════════════════════════════════

bool get_stats(Stats* out);

// Same as get_stats(), with differential-privacy Gaussian noise applied
// to the monotonic counter fields (observations, anomaly queries/hits).
// trained_ms, training_complete, current_k_x10, and populated_buckets
// are deterministic state observable from device behavior anyway —
// they're not noised. Use this variant whenever Stats crosses the
// HTTP / MQTT boundary.
bool get_stats_for_export(Stats* out);

// User-initiated reset: wipe all bucket stats + restart the 72 h training
// window. Used when the user moves the device to a new location, or after
// a big lifestyle change (new baby, new roommate, etc.).
bool restart_training();

// ════════════════════════════════════════════════════════════════════════════
// FEDERATED MERGE (Phase 9)
//
// Per-bucket share format used by federated mesh aggregation. A peer
// contributes (count_delta, sum[F], sum_sq[F]) for each bucket; we merge
// those into our local Welford accumulators with bounded effect.
// ════════════════════════════════════════════════════════════════════════════

struct RemoteBucketShare {
  uint16_t count;                 // peer's contribution to count
  int32_t  sum    [FEATURE_COUNT];
  int64_t  sum_sq [FEATURE_COUNT];
};

// Maximum count contribution accepted from a single peer in one merge.
// Caps the influence of any one (potentially malicious) peer so they
// cannot dominate our local baseline by submitting a large count.
static const uint16_t REMOTE_MERGE_MAX_COUNT = 64;

// Merge one peer's bucket share into our local bucket. Returns true on
// success, false if the bucket index is invalid or the local bucket is
// already saturated. Caps remote.count at REMOTE_MERGE_MAX_COUNT and
// will not push our local count past BUCKET_MAX_COUNT.
bool merge_remote_bucket(uint8_t bucket, const RemoteBucketShare& share);

// Snapshot one of our local buckets into a RemoteBucketShare (caller-
// owned). NO DP noise is applied here — the federated module is
// responsible for noising before transmission. Returns false on invalid
// bucket index. Used by federated::build_baseline_share().
bool snapshot_bucket(uint8_t bucket, RemoteBucketShare* out);

// ════════════════════════════════════════════════════════════════════════════
// CONFORMANCE
// ════════════════════════════════════════════════════════════════════════════

// Self-test: populate a bucket with a fixed distribution, verify that
// an in-distribution sample scores low and an out-of-distribution sample
// scores high. Restores all state (including stats) on exit.
bool conformance_self_test();

}  // namespace baseline

#endif  // SECURACV_BASELINE_H
