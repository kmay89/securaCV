/*
 * SecuraCV Canary — Familiar-device recognizer (Phase 5)
 * Version 0.1.0
 *
 * Solves the "neighbor problem". Household devices get suppressed by the
 * IRK recognizer (see household.h). But recurring *non-paired* devices —
 * the neighbor's phone that walks by at 6 pm every weekday, the kid's
 * bike computer on the bus route — shouldn't alert either, and we can't
 * bond with them. Instead we recognize their BEHAVIOR.
 *
 * HOW IT WORKS
 * ============
 * An rf_presence event carries a tiny 11-bit "fingerprint" derived from
 * three deliberately-coarse observables:
 *     time-of-day × rssi-envelope × advertising-density
 * Many different physical devices share any given fingerprint bucket
 * (plausible deniability), but the fingerprint of a specific recurring
 * device is reasonably stable day-to-day.
 *
 * Storage is two rotating Bloom filters (256 B each = 2048 bits, 8 hash
 * functions):
 *   today      — being populated as events happen
 *   yesterday  — queried by Phase 8's notification policy to decide
 *                whether an arrival is "ambient" (seen before) and
 *                therefore not alert-worthy
 * At each 24-hour mark: yesterday ← today + DP noise, today ← empty.
 *
 * A third, persistent 64-byte filter holds user-chosen "always ignore
 * this pattern" fingerprints. It survives rotation and lives in NVS.
 *
 * PRIVACY INVARIANTS
 * ==================
 *   • We never store a MAC, a token, a timestamp, or a raw RSSI sample.
 *     Only 11-bit bucketed fingerprints enter the filter via hashed bits.
 *   • Hashes are salted with a per-device secret kept in NVS; flash
 *     dumps alone do not reveal which fingerprints were recorded.
 *   • Differential-privacy noise is mixed into yesterday's filter at
 *     every rotation so that flash dumps + the secret give only
 *     probabilistic answers, not certainties.
 *   • Yesterday's filter is destroyed when tomorrow's rotation occurs:
 *     the memory of the neighbor's 6 pm walk-by is ~48 h at most.
 *   • The "always ignore" filter contains only user-confirmed patterns
 *     and is never exported or shared.
 */

#ifndef SECURACV_FAMILIAR_H
#define SECURACV_FAMILIAR_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

namespace familiar {

// ════════════════════════════════════════════════════════════════════════════
// CONSTANTS
// ════════════════════════════════════════════════════════════════════════════

// Fingerprint: 11 bits total. Layout (LSB to MSB):
//   bits [0..4]  time-of-day bucket  (5 bits → 32 buckets of 45 min each)
//   bits [5..7]  rssi mean class     (3 bits → 8 levels across -90..0 dBm)
//   bits [8..9]  adv density class   (2 bits → 4 levels: quiet/low/med/high)
//   bit  [10]    rssi stability flag (1 bit  → 0=steady, 1=fluctuating)
static const size_t FINGERPRINT_BITS = 11;
static const uint16_t FINGERPRINT_MASK = (1u << FINGERPRINT_BITS) - 1;

// Rotating Bloom filter (today / yesterday).
// 2048 bits, 8 hash functions — supports ~100 recent entries at <0.5% FPR.
static const size_t  BLOOM_BITS    = 2048;
static const size_t  BLOOM_BYTES   = BLOOM_BITS / 8;
static const uint8_t BLOOM_HASHES  = 8;

// Persistent "always ignore" filter.
// 512 bits, 6 hash functions — supports ~32 entries at <1% FPR.
static const size_t  IGNORE_BITS   = 512;
static const size_t  IGNORE_BYTES  = IGNORE_BITS / 8;
static const uint8_t IGNORE_HASHES = 6;

// Rotation period. One Earth day, measured in device uptime (no wall clock).
static const uint32_t ROTATION_PERIOD_MS = 24UL * 60UL * 60UL * 1000UL;

// Differential-privacy noise rate applied during rotation. 1% bit flip
// rate in the "yesterday" snapshot — enough to add plausible deniability,
// small enough not to wreck recall for real recurring fingerprints.
static const uint16_t DP_NOISE_BASIS_POINTS = 100;  // = 1.00%

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

struct FingerprintInputs {
  uint8_t  time_of_day_bucket;  // 0..143 (10-minute buckets); will be re-bucketed
  int8_t   rssi_mean_dbm;       // -127..0
  uint8_t  adv_per_minute;      // observed advertising density
  uint8_t  rssi_spread_dbm;     // max - min over the observation window
};

struct Stats {
  uint32_t total_notes;                 // cumulative count of note_fingerprint()
  uint32_t total_rotations;             // cumulative 24 h rotations
  uint32_t total_ambient_queries;       // cumulative is_ambient() calls
  uint32_t total_ambient_matches;       // how many of those returned true
  uint32_t total_always_ignored_queries;
  uint32_t total_always_ignored_matches;
  uint32_t today_bits_set;              // approx load of today's filter
  uint32_t yesterday_bits_set;          // approx load of yesterday's filter
  uint32_t always_ignored_bits_set;
  uint32_t ms_until_next_rotation;
};

// ════════════════════════════════════════════════════════════════════════════
// LIFECYCLE
// ════════════════════════════════════════════════════════════════════════════

// Load persistent state from NVS (always-ignore filter + salt). Generates
// a fresh per-device salt on first boot. Safe before WiFi/BLE.
bool init();

// Secure-wipe in-memory Bloom filters + salt. Does NOT erase NVS.
void deinit();

// ════════════════════════════════════════════════════════════════════════════
// FINGERPRINT COMPUTATION
// ════════════════════════════════════════════════════════════════════════════

// Deterministic mapping from rf_presence observables → 11-bit fingerprint.
uint16_t compute_fingerprint(const FingerprintInputs& inputs);

// ════════════════════════════════════════════════════════════════════════════
// ROTATING FILTER (daily "familiar" recognition)
// ════════════════════════════════════════════════════════════════════════════

// Record a fingerprint in today's filter.
void note_fingerprint(uint16_t fp);

// Is this fingerprint present in YESTERDAY's filter?
// (Used by Phase 8's notification policy to mark an event as ambient.)
// Returns false if no rotation has happened yet (cold boot, first day).
bool is_ambient(uint16_t fp);

// Call periodically from the main loop. Performs a rotation if
// ROTATION_PERIOD_MS has elapsed since the last one.
void tick(uint32_t now_ms);

// Force an immediate rotation (for tests and manual resets).
void rotate_now();

// ════════════════════════════════════════════════════════════════════════════
// "ALWAYS IGNORE" FILTER (persistent, user-initiated)
// ════════════════════════════════════════════════════════════════════════════

// User pressed "Always ignore this pattern" on an alert; persist the
// fingerprint to NVS. Subsequent events matching this fp will be marked
// always-ignored by Phase 8.
bool always_ignore(uint16_t fp);

// Query the always-ignore filter.
bool is_always_ignored(uint16_t fp);

// User-initiated reset — forget all always-ignore entries.
bool forget_always_ignored();

// ════════════════════════════════════════════════════════════════════════════
// FEDERATED MERGE (Phase 9)
//
// A peer's "yesterday" Bloom filter snapshot can be OR-merged into ours
// to expand the ambient set with fingerprints the peer has seen but we
// haven't. This is safe because a Bloom filter answers "have we seen
// this?" — a OR is just a union with the peer's positive set.
// ════════════════════════════════════════════════════════════════════════════

// Merge `peer_yesterday` (BLOOM_BYTES bytes) into our local yesterday
// filter via bitwise OR. After merge, our `is_ambient(fp)` returns true
// for any fingerprint either we OR the peer has seen recently.
//
// Safe to call even if our yesterday is "cold" (first day) — merging
// with an empty side is a no-op for that side. After merge, yesterday
// is marked valid. NVS is updated so the merged state survives reboot.
bool merge_remote_yesterday(const uint8_t* peer_yesterday, size_t len);

// Copy our local "yesterday" filter into `out` (must be at least
// BLOOM_BYTES). Returns false if yesterday is not yet valid (first
// day before any rotation has occurred) or out_len < BLOOM_BYTES.
// NO DP noise is applied here — the rotation already added it. The
// federated module is responsible for any additional epsilon-budget
// accounting on transmission.
bool snapshot_yesterday(uint8_t* out, size_t out_len);

// ════════════════════════════════════════════════════════════════════════════
// INTROSPECTION
// ════════════════════════════════════════════════════════════════════════════

bool get_stats(Stats* out);

// Same as get_stats(), with differential-privacy Gaussian noise applied
// to the monotonic counter fields. Use for HTTP / MQTT export; local
// decision paths (is_ambient, is_always_ignored) are intentionally
// noise-free. Bit-set counts are noised too — they leak the load of
// the Bloom filter which is a proxy for event frequency.
bool get_stats_for_export(Stats* out);

// ════════════════════════════════════════════════════════════════════════════
// CONFORMANCE
// ════════════════════════════════════════════════════════════════════════════

// Self-test: insert a known fp into today's filter, query it (should be
// present), force rotation, query via is_ambient (should be present),
// force second rotation, query again (should be false — it aged out).
bool conformance_self_test();

}  // namespace familiar

#endif  // SECURACV_FAMILIAR_H
