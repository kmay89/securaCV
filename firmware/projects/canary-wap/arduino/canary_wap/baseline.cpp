/*
 * SecuraCV Canary — Adaptive baseline + anomaly scoring — Implementation
 *
 * See baseline.h for the design note. This file carries:
 *   • Welford-ish running sums per bucket × feature (int64 sum_sq to stay
 *     overflow-safe through BUCKET_MAX_COUNT samples).
 *   • Training-elapsed accounting across reboots (NVS-persisted every hour
 *     via the tick() entry point).
 *   • Integer z-score calculation: |x - mean| / sqrt(variance), scaled ×10.
 *   • A deterministic self-test.
 */

#include "baseline.h"
#include "dp.h"
#include "nvs_store.h"
#include "health_log.h"

#include <string.h>

namespace baseline {

// ────────────────────────────────────────────────────────────────────────────
// SECURITY + ARITHMETIC PRIMITIVES
// ────────────────────────────────────────────────────────────────────────────

static void secure_wipe(void* ptr, size_t len) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
  while (len--) { *p++ = 0; }
  asm volatile("" ::: "memory");
}

static inline uint32_t elapsed_ms(uint32_t start, uint32_t now) {
  return now - start;  // unsigned sub → wrap-safe
}

// Integer square root (bit-by-bit, same as csi_features). ~16 iters max.
static uint32_t isqrt_u32(uint32_t n) {
  uint32_t root = 0;
  uint32_t bit = (uint32_t)1 << 30;
  while (bit > n) bit >>= 2;
  while (bit) {
    const uint32_t trial = root + bit;
    if (n >= trial) { n -= trial; root = (root >> 1) + bit; }
    else            { root >>= 1; }
    bit >>= 2;
  }
  return root;
}

// ────────────────────────────────────────────────────────────────────────────
// PERSISTENT STATE
// ────────────────────────────────────────────────────────────────────────────

// Packed bucket: sum + sum_of_squares per feature, plus count.
//   mean     = sum / count
//   variance = (sum_sq - sum² / count) / (count - 1)
// Using int64 sum_sq because an int32 can barely hold 10000²×8000 for
// our RSSI-squared worst case, and we want headroom.
struct Bucket {
  uint16_t count;
  int32_t  sum   [FEATURE_COUNT];   // Σ xi
  int64_t  sum_sq[FEATURE_COUNT];   // Σ xi²
};

static const char* NVS_KEY_BUCKETS   = "bl_buckets";    // Bucket[BUCKET_COUNT]
static const char* NVS_KEY_TRAINED   = "bl_trained_ms"; // uint32, accumulated
static const char* NVS_KEY_VERSION   = "bl_version";    // migration sentinel

// Bump on any struct-layout change so NVS loads from a mismatched schema
// are rejected instead of silently corrupting a bucket.
static const uint32_t BASELINE_SCHEMA_VERSION = 1;

// ────────────────────────────────────────────────────────────────────────────
// RUNTIME STATE
// ────────────────────────────────────────────────────────────────────────────

static bool     s_initialized = false;
static Bucket   s_buckets[BUCKET_COUNT];

// Accumulated training uptime (persisted). Cleared on restart_training().
static uint32_t s_trained_ms = 0;

// millis() at the moment of init (or last persist). tick() computes
// delta since this and adds to s_trained_ms at each persist interval.
static uint32_t s_boot_anchor_ms = 0;
static uint32_t s_last_persist_ms = 0;

// Stats (in-RAM only; not persisted).
static uint32_t s_total_observations    = 0;
static uint32_t s_total_anomaly_queries = 0;
static uint32_t s_total_anomaly_hits    = 0;

// ────────────────────────────────────────────────────────────────────────────
// TRAINING PROGRESS
// ────────────────────────────────────────────────────────────────────────────

static uint32_t training_elapsed_now() {
  const uint32_t delta = elapsed_ms(s_boot_anchor_ms, millis());
  // Guard: if s_trained_ms + delta overflows, saturate at completion.
  if (s_trained_ms >= TRAINING_PERIOD_MS) return TRAINING_PERIOD_MS;
  if (delta > (TRAINING_PERIOD_MS - s_trained_ms)) return TRAINING_PERIOD_MS;
  return s_trained_ms + delta;
}

uint32_t training_progress_bps() {
  const uint32_t e = training_elapsed_now();
  if (e >= TRAINING_PERIOD_MS) return 10000;
  // bps = e * 10000 / PERIOD. e ≤ 72h = 2.59e8 ms; × 10000 = 2.59e12 > uint32.
  // Use uint64.
  return (uint32_t)(((uint64_t)e * 10000ULL) / TRAINING_PERIOD_MS);
}

bool training_complete() {
  return training_elapsed_now() >= TRAINING_PERIOD_MS;
}

uint8_t current_k_x10() {
  const uint32_t bps = training_progress_bps();
  // k(t) = K_START - (K_START - K_END) × bps/10000
  // All small, fits easily in 32-bit.
  const uint32_t delta = ((uint32_t)(K_TRAIN_START_X10 - K_TRAIN_END_X10) * bps) / 10000U;
  const uint8_t k = (uint8_t)((uint32_t)K_TRAIN_START_X10 - delta);
  return k;
}

// ────────────────────────────────────────────────────────────────────────────
// PERSISTENCE
// ────────────────────────────────────────────────────────────────────────────

static void persist_trained_ms() {
  s_trained_ms = training_elapsed_now();
  s_boot_anchor_ms = millis();
  nvs_store::set_u32(NVS_KEY_TRAINED, s_trained_ms);
}

static void persist_buckets() {
  nvs_store::set_blob(NVS_KEY_BUCKETS, s_buckets, sizeof(s_buckets));
  nvs_store::set_u32(NVS_KEY_VERSION, BASELINE_SCHEMA_VERSION);
}

static void persist_all() {
  persist_trained_ms();
  persist_buckets();
  s_last_persist_ms = millis();
}

// ────────────────────────────────────────────────────────────────────────────
// LIFECYCLE
// ────────────────────────────────────────────────────────────────────────────

bool init() {
  if (s_initialized) return true;

  memset(s_buckets, 0, sizeof(s_buckets));

  // Version-checked blob load: reject NVS data with a mismatched schema.
  // On mismatch we also discard the trained_ms counter — otherwise we'd
  // boot with empty buckets but a fully-tightened k=2.0 threshold and
  // over-fire anomalies until the new buckets refilled (codex review #314).
  const uint32_t stored_version = nvs_store::get_u32(NVS_KEY_VERSION, 0);
  bool force_trained_reset = false;
  if (stored_version == BASELINE_SCHEMA_VERSION) {
    nvs_store::get_blob(NVS_KEY_BUCKETS, s_buckets, sizeof(s_buckets));
  } else if (stored_version != 0) {
    health_logging::logf(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
      "Baseline: NVS schema mismatch (stored=%u, expected=%u); full reset",
      (unsigned)stored_version, (unsigned)BASELINE_SCHEMA_VERSION);
    force_trained_reset = true;
  }

  if (force_trained_reset) {
    s_trained_ms = 0;
    // Write back a consistent state so the next init() sees the new
    // schema version with matching (empty) buckets and zero progress.
    nvs_store::set_u32(NVS_KEY_TRAINED, 0);
    nvs_store::set_u32(NVS_KEY_VERSION, BASELINE_SCHEMA_VERSION);
    nvs_store::set_blob(NVS_KEY_BUCKETS, s_buckets, sizeof(s_buckets));
  } else {
    s_trained_ms = nvs_store::get_u32(NVS_KEY_TRAINED, 0);
    if (s_trained_ms > TRAINING_PERIOD_MS) s_trained_ms = TRAINING_PERIOD_MS;
  }

  s_boot_anchor_ms = millis();
  s_last_persist_ms = millis();
  s_total_observations = 0;
  s_total_anomaly_queries = 0;
  s_total_anomaly_hits = 0;

  s_initialized = true;

  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Baseline: init (trained=%ums, k=%.1f, complete=%d)",
    (unsigned)s_trained_ms, (float)current_k_x10() / 10.0f,
    (int)training_complete());
  return true;
}

void deinit() {
  if (!s_initialized) return;
  secure_wipe(s_buckets, sizeof(s_buckets));
  s_trained_ms = 0;
  s_boot_anchor_ms = 0;
  s_last_persist_ms = 0;
  s_total_observations = 0;
  s_total_anomaly_queries = 0;
  s_total_anomaly_hits = 0;
  s_initialized = false;
}

// ────────────────────────────────────────────────────────────────────────────
// OBSERVE
// ────────────────────────────────────────────────────────────────────────────

uint8_t bucket_from_time_bucket(uint8_t time_bucket_10min) {
  // time_bucket is 10-min units in [0, 143]. Hourly = /6.
  const uint8_t h = (uint8_t)(time_bucket_10min / 6);
  return h >= BUCKET_COUNT ? (uint8_t)(BUCKET_COUNT - 1) : h;
}

void observe(uint8_t bucket, const Features& f) {
  if (!s_initialized) return;
  if (bucket >= BUCKET_COUNT) return;

  Bucket& b = s_buckets[bucket];
  if (b.count >= BUCKET_MAX_COUNT) {
    // Saturated: stop accumulating so sum/sum_sq don't overflow and the
    // Gaussian stays stable. Future enhancement: exponential decay.
    s_total_observations++;
    return;
  }

  const int32_t v[FEATURE_COUNT] = {
    (int32_t)f.csi_motion,
    (int32_t)f.ble_count,
    (int32_t)f.rssi_mean,
    (int32_t)f.rssi_spread,
  };
  for (uint8_t i = 0; i < FEATURE_COUNT; i++) {
    b.sum[i]    += v[i];
    b.sum_sq[i] += (int64_t)v[i] * v[i];
  }
  b.count++;
  s_total_observations++;
}

void tick(uint32_t now_ms) {
  if (!s_initialized) return;
  if (elapsed_ms(s_last_persist_ms, now_ms) >= PERSIST_INTERVAL_MS) {
    persist_all();
  }
}

// ────────────────────────────────────────────────────────────────────────────
// SCORE / QUERY
// ────────────────────────────────────────────────────────────────────────────

// Given a bucket and feature index, compute |z| × 10 as an unsigned int.
//
// The precision-preserving formulation from gemini review #314: compute
//    z² × 100 = (x·n − Σx)² · (n−1) · 100 / (n · (n·Σx² − (Σx)²))
// in int64 space in one go, then take a single sqrt. This avoids the
// compounding truncation of the naive
//    mean = Σx / n
//    σ²   = (n·Σx² − (Σx)²) / (n·(n−1))
//    z    = (x − mean) · 10 / √σ²
// where each integer-division step rounded toward zero and killed
// precision for features with small ranges (ble_count, stable RSSI).
//
// Overflow: worst case is RSSI with n=8000, x=-127, Σx=-127·n = -1.02M.
//   xn_minus_sum  ≤ 2·10⁶   (absolute)
//   (xn_minus_sum)² ≤ 4·10¹²
//   × (n−1) · 100  ≤ 3.2·10¹⁸     < int64 max (9.2·10¹⁸) ✓
//
// Returns INT16_MAX only for under-populated buckets. A zero-variance
// bucket with x == mean returns 0; with x ≠ mean it returns INT16_MAX
// (so is_anomaly treats it as "unknown", not "definitely anomalous" —
// a stable bucket with one outlier is ambiguous without more samples).
static int16_t z_score_x10(const Bucket& b, uint8_t feat, int32_t x) {
  if (b.count < 2) return INT16_MAX;

  const int64_t n      = (int64_t)b.count;
  const int64_t sum    = (int64_t)b.sum[feat];
  const int64_t sum_sq = b.sum_sq[feat];

  // num = n·Σx² − (Σx)² = n · (n−1) · sample_variance
  const int64_t num = n * sum_sq - sum * sum;

  if (num <= 0) {
    // Bucket has no variance. If x matches the (integer) mean we report
    // z = 0; otherwise we cannot score it meaningfully and return
    // INT16_MAX so is_anomaly treats this feature as "unknown".
    return (x * n == sum) ? 0 : INT16_MAX;
  }

  const int64_t xn_minus_sum = (int64_t)x * n - sum;
  // z²·100 = (x·n − Σx)² · (n−1) · 100 / (n · num)
  const int64_t z2_x100_num = xn_minus_sum * xn_minus_sum * (n - 1) * 100LL;
  const int64_t z2_x100_den = n * num;
  if (z2_x100_den <= 0) return INT16_MAX;

  const uint64_t z2_x100 = (uint64_t)(z2_x100_num / z2_x100_den);
  if (z2_x100 == 0) return 0;

  // sqrt, then cap. isqrt_u32 takes uint32_t; clamp first to avoid
  // truncation of very large z² values (they saturate to INT16_MAX anyway).
  const uint32_t z2_capped = (z2_x100 > UINT32_MAX) ? UINT32_MAX : (uint32_t)z2_x100;
  const uint32_t z10 = isqrt_u32(z2_capped);
  return z10 > (uint32_t)INT16_MAX ? INT16_MAX : (int16_t)z10;
}

Scores score(uint8_t bucket, const Features& f) {
  Scores out;
  for (uint8_t i = 0; i < FEATURE_COUNT; i++) out.z10[i] = INT16_MAX;

  if (!s_initialized || bucket >= BUCKET_COUNT) return out;

  const Bucket& b = s_buckets[bucket];
  const int32_t v[FEATURE_COUNT] = {
    (int32_t)f.csi_motion,
    (int32_t)f.ble_count,
    (int32_t)f.rssi_mean,
    (int32_t)f.rssi_spread,
  };
  for (uint8_t i = 0; i < FEATURE_COUNT; i++) {
    out.z10[i] = z_score_x10(b, i, v[i]);
  }
  return out;
}

bool is_anomaly(uint8_t bucket, const Features& f) {
  s_total_anomaly_queries++;
  const Scores s = score(bucket, f);
  const int16_t k = (int16_t)current_k_x10();

  for (uint8_t i = 0; i < FEATURE_COUNT; i++) {
    if (s.z10[i] == INT16_MAX) continue;  // under-populated feature
    if (s.z10[i] > k) {
      s_total_anomaly_hits++;
      return true;
    }
  }
  return false;
}

// ────────────────────────────────────────────────────────────────────────────
// INTROSPECTION + RESET
// ────────────────────────────────────────────────────────────────────────────

bool get_stats(Stats* out) {
  if (!out) return false;
  out->trained_ms             = training_elapsed_now();
  out->training_complete      = training_complete();
  out->current_k_x10          = current_k_x10();
  out->total_observations     = s_total_observations;
  out->total_anomaly_queries  = s_total_anomaly_queries;
  out->total_anomaly_hits     = s_total_anomaly_hits;

  uint16_t populated = 0;
  for (uint8_t i = 0; i < BUCKET_COUNT; i++) {
    if (s_buckets[i].count >= 2) populated++;
  }
  out->populated_buckets = populated;
  return true;
}

bool get_stats_for_export(Stats* out) {
  if (!get_stats(out)) return false;
  out->total_observations    = dp::noisy_u32(out->total_observations,    1);
  out->total_anomaly_queries = dp::noisy_u32(out->total_anomaly_queries, 1);
  out->total_anomaly_hits    = dp::noisy_u32(out->total_anomaly_hits,    1);
  return true;
}

bool restart_training() {
  if (!s_initialized) return false;
  // secure_wipe (not memset) per the header's privacy invariant: a
  // user-initiated restart should leave no trace of the previous
  // baseline in RAM between the wipe and the next persist.
  secure_wipe(s_buckets, sizeof(s_buckets));
  s_trained_ms = 0;
  s_boot_anchor_ms = millis();
  s_last_persist_ms = millis();
  persist_all();
  health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Baseline: training restarted (all buckets secure-wiped)");
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// FEDERATED MERGE (Phase 9)
// ────────────────────────────────────────────────────────────────────────────

bool snapshot_bucket(uint8_t bucket, RemoteBucketShare* out) {
  if (!s_initialized || !out) return false;
  if (bucket >= BUCKET_COUNT) return false;
  const Bucket& b = s_buckets[bucket];
  out->count = b.count;
  for (uint8_t i = 0; i < FEATURE_COUNT; i++) {
    out->sum[i]    = b.sum[i];
    out->sum_sq[i] = b.sum_sq[i];
  }
  return true;
}

// Test-only inverse of snapshot_bucket. See header for the warning.
bool overwrite_bucket_for_tests(uint8_t bucket, const RemoteBucketShare& share) {
  if (!s_initialized) return false;
  if (bucket >= BUCKET_COUNT) return false;
  Bucket& b = s_buckets[bucket];
  // share.count is uint16; clamp at BUCKET_MAX_COUNT defensively in case
  // a future caller passes an arbitrary share.
  b.count = share.count > BUCKET_MAX_COUNT ? BUCKET_MAX_COUNT : share.count;
  for (uint8_t i = 0; i < FEATURE_COUNT; i++) {
    b.sum[i]    = share.sum[i];
    b.sum_sq[i] = share.sum_sq[i];
  }
  return true;
}

bool merge_remote_bucket(uint8_t bucket, const RemoteBucketShare& share) {
  if (!s_initialized) return false;
  if (bucket >= BUCKET_COUNT) return false;

  Bucket& b = s_buckets[bucket];
  if (b.count >= BUCKET_MAX_COUNT) return false;  // local saturated

  // Cap the peer's contribution. Even if a peer reports count=10000, we
  // only credit them with REMOTE_MERGE_MAX_COUNT; this bounds influence.
  uint16_t add_count = share.count;
  if (add_count > REMOTE_MERGE_MAX_COUNT) add_count = REMOTE_MERGE_MAX_COUNT;

  // Don't let merge push count past BUCKET_MAX_COUNT. Scale sums
  // proportionally if we have to clamp count.
  const uint16_t headroom = (uint16_t)(BUCKET_MAX_COUNT - b.count);
  if (add_count > headroom) add_count = headroom;
  if (add_count == 0) return false;

  // Scale factor for sums: if we accepted only a fraction of the share,
  // we must accept the same fraction of sum / sum_sq, otherwise the
  // mean and variance will be biased.
  //
  // Overflow safety without __int128 (not reliable on xtensa-esp-elf-gcc):
  // before multiplying by `add_count`, clamp the operand to a magnitude
  // that keeps the product in int64. A crafted peer share with
  // |share.sum_sq[i]| > INT64_MAX / add_count would have overflowed a
  // plain int64 multiply; clamping biases only adversarial inputs toward
  // smaller magnitudes while leaving honest shares (always well below
  // the threshold — BUCKET_MAX_COUNT × max_feature² ≈ 1.3×10⁸) untouched.
  int64_t scaled_sum   [FEATURE_COUNT] = {0};
  int64_t scaled_sum_sq[FEATURE_COUNT] = {0};
  if (add_count < share.count) {
    // share.count is non-zero because add_count > 0 and add_count ≤ share.count.
    const int64_t ac = (int64_t)add_count;
    const int64_t sc = (int64_t)share.count;
    // Largest per-operand magnitude that can be multiplied by ac without
    // overflowing int64. `ac ≥ 1` so this is well-defined.
    const int64_t max_safe = INT64_MAX / ac;
    for (uint8_t i = 0; i < FEATURE_COUNT; i++) {
      int64_t s  = (int64_t)share.sum[i];
      int64_t ss = share.sum_sq[i];
      if (s  >  max_safe)  s  =  max_safe;
      if (s  < -max_safe)  s  = -max_safe;
      if (ss >  max_safe)  ss =  max_safe;
      if (ss < -max_safe)  ss = -max_safe;
      scaled_sum[i]    = (s  * ac) / sc;
      scaled_sum_sq[i] = (ss * ac) / sc;
    }
  } else {
    for (uint8_t i = 0; i < FEATURE_COUNT; i++) {
      scaled_sum[i]    = share.sum[i];
      scaled_sum_sq[i] = share.sum_sq[i];
    }
  }

  // Apply, with overflow saturation guards on int32 sum.
  for (uint8_t i = 0; i < FEATURE_COUNT; i++) {
    const int64_t new_sum = (int64_t)b.sum[i] + scaled_sum[i];
    if      (new_sum >  INT32_MAX) b.sum[i] =  INT32_MAX;
    else if (new_sum <  INT32_MIN) b.sum[i] =  INT32_MIN;
    else                           b.sum[i] = (int32_t)new_sum;
    // sum_sq is already int64; saturate at INT64_MAX.
    if (scaled_sum_sq[i] > 0 && b.sum_sq[i] > INT64_MAX - scaled_sum_sq[i]) {
      b.sum_sq[i] = INT64_MAX;
    } else {
      b.sum_sq[i] += scaled_sum_sq[i];
    }
  }
  b.count = (uint16_t)(b.count + add_count);
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// CONFORMANCE
// ────────────────────────────────────────────────────────────────────────────

bool conformance_self_test() {
  // Save state so the test doesn't corrupt real data (stats included —
  // see familiar.cpp for the same pattern).
  Bucket   saved_buckets[BUCKET_COUNT];
  memcpy(saved_buckets, s_buckets, sizeof(s_buckets));
  const uint32_t saved_trained = s_trained_ms;
  const uint32_t saved_anchor  = s_boot_anchor_ms;
  const uint32_t saved_persist = s_last_persist_ms;
  const uint32_t saved_obs = s_total_observations;
  const uint32_t saved_aq  = s_total_anomaly_queries;
  const uint32_t saved_ah  = s_total_anomaly_hits;

  // Clean slate: single bucket, tight distribution around (50, 5, -55, 10).
  memset(s_buckets, 0, sizeof(s_buckets));
  s_trained_ms = TRAINING_PERIOD_MS;  // pretend training done → tight k
  const uint8_t B = 5;
  const Features nominal  = { 50, 5, -55, 10 };
  const Features outlier  = { 95, 5, -55, 10 };    // csi_motion way high

  // Seed 50 samples of a tight distribution with tiny jitter.
  for (int i = 0; i < 50; i++) {
    const int16_t jitter = (int16_t)((i * 7) % 5 - 2);   // -2..+2
    const Features noisy = { (int16_t)(nominal.csi_motion + jitter),
                              nominal.ble_count,
                              (int16_t)(nominal.rssi_mean + jitter),
                              nominal.rssi_spread };
    observe(B, noisy);
  }

  // In-distribution sample: z-scores should all be small.
  const Scores z_ok = score(B, nominal);
  const bool step1 =
      z_ok.z10[0] != INT16_MAX && z_ok.z10[0] <= 30 &&
      z_ok.z10[1] != INT16_MAX && z_ok.z10[2] != INT16_MAX;

  // Out-of-distribution: csi_motion z-score should be large.
  const Scores z_bad = score(B, outlier);
  const bool step2 = z_bad.z10[0] > 30;

  // is_anomaly should fire for the outlier (k = 2.0 after training complete).
  const bool step3 = is_anomaly(B, outlier);
  const bool step4 = !is_anomaly(B, nominal);

  // Restore state (including stats).
  memcpy(s_buckets, saved_buckets, sizeof(s_buckets));
  s_trained_ms = saved_trained;
  s_boot_anchor_ms = saved_anchor;
  s_last_persist_ms = saved_persist;
  s_total_observations    = saved_obs;
  s_total_anomaly_queries = saved_aq;
  s_total_anomaly_hits    = saved_ah;

  const bool ok = step1 && step2 && step3 && step4;
  if (!ok) {
    health_logging::logf(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "Baseline self-test FAIL: s1=%d s2=%d s3=%d s4=%d (z_ok=%d z_bad=%d)",
      step1, step2, step3, step4,
      (int)z_ok.z10[0], (int)z_bad.z10[0]);
  } else {
    health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
      "Baseline self-test: OK");
  }
  return ok;
}

}  // namespace baseline
