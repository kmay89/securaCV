/*
 * SecuraCV Canary — Federated mesh aggregation — Implementation
 *
 * See federated.h for the design + privacy invariants.
 */

#include "federated.h"
#include "baseline.h"
#include "familiar.h"
#include "dp.h"
#include "health_log.h"

#include <string.h>

namespace federated {

// ────────────────────────────────────────────────────────────────────────────
// STATE
// ────────────────────────────────────────────────────────────────────────────

static bool s_initialized = false;

// Per-share-type throttle timestamps. Decoupled so building a baseline
// share doesn't starve familiar-share emission (and vice versa) for
// SHARE_BUILD_MIN_INTERVAL_MS — codex P2 on #317.
static uint32_t s_last_baseline_build_ms = 0;
static uint32_t s_last_familiar_build_ms = 0;

static uint32_t s_total_baseline_built = 0;
static uint32_t s_total_familiar_built = 0;
static uint32_t s_total_baseline_merged = 0;
static uint32_t s_total_familiar_merged = 0;
static uint32_t s_total_rejected_version = 0;
static uint32_t s_total_rejected_magic = 0;
static uint32_t s_total_rejected_size = 0;

// ────────────────────────────────────────────────────────────────────────────
// HELPERS
// ────────────────────────────────────────────────────────────────────────────

static inline uint32_t elapsed_ms(uint32_t start, uint32_t now) {
  return now - start;  // unsigned sub → wrap-safe
}

static bool throttle_allows_build(uint32_t last_build_ms, bool force) {
  if (force) return true;
  if (last_build_ms == 0) return true;  // never built before
  return elapsed_ms(last_build_ms, millis()) >= SHARE_BUILD_MIN_INTERVAL_MS;
}

// Draws per bucket: count, then sum and sum_sq for each feature.
static constexpr uint16_t DRAWS_PER_BUCKET = 1 + 2 * baseline::FEATURE_COUNT;
// Draws per share. Charged by sequential composition (dp.h BUDGET): at the
// default ε = 1 that is 24 x 9 = 216 ε against a 4 ε window budget, so the
// share is always withheld today. Charging the disjoint hour buckets by
// parallel composition (9 ε) would still exceed 4 ε. Unblocking federated
// sharing is a budget / accounting decision, not a code fix.
static constexpr uint16_t SHARE_DRAWS =
    (uint16_t)(baseline::BUCKET_COUNT * DRAWS_PER_BUCKET);

// Apply DP noise to a single bucket share in-place, drawing from `rel`.
// count gets noise with sensitivity 1; sums get noise scaled by feature
// sensitivity.
static void apply_dp_noise_to_bucket(BaselineShareBucket* b, dp::Release& rel) {
  // Count: sensitivity = 1 (one event affects count by 1).
  const uint32_t noisy_count = rel.u32((uint32_t)b->count, 1);
  b->count = noisy_count > UINT16_MAX ? UINT16_MAX : (uint16_t)noisy_count;

  for (uint8_t i = 0; i < baseline::FEATURE_COUNT; i++) {
    // sum: sensitivity ~= max feature value; one event shifts sum by up
    // to that. Signed draw to allow negative noise (sums can be neg).
    b->sum[i] = rel.i32(b->sum[i], SUM_SENSITIVITY[i]);

    // sum_sq: sensitivity = max_feature² (one event shifts sum_sq by
    // up to that). There is no i64 draw, so we draw one signed noise
    // term with the sensitivity clamped to UINT32_MAX and add it to the
    // int64 accumulator with saturation guards. The clamp is acceptable
    // because SUM_SENSITIVITY²  (≤ 65025 for our worst-case feature)
    // stays well under UINT32_MAX.
    const uint64_t sens_sq = (uint64_t)SUM_SENSITIVITY[i] * SUM_SENSITIVITY[i];
    const uint32_t sens_clamped =
        sens_sq > UINT32_MAX ? UINT32_MAX : (uint32_t)sens_sq;
    const int32_t noise = rel.i32(0, sens_clamped);
    if      (noise > 0 && b->sum_sq[i] > INT64_MAX - noise) b->sum_sq[i] = INT64_MAX;
    else if (noise < 0 && b->sum_sq[i] < INT64_MIN - noise) b->sum_sq[i] = INT64_MIN;
    else                                                    b->sum_sq[i] += noise;
  }
}

// ────────────────────────────────────────────────────────────────────────────
// LIFECYCLE
// ────────────────────────────────────────────────────────────────────────────

bool init() {
  if (s_initialized) return true;
  s_last_baseline_build_ms = 0;
  s_last_familiar_build_ms = 0;
  s_total_baseline_built = 0;
  s_total_familiar_built = 0;
  s_total_baseline_merged = 0;
  s_total_familiar_merged = 0;
  s_total_rejected_version = 0;
  s_total_rejected_magic = 0;
  s_total_rejected_size = 0;
  s_initialized = true;
  health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Federated: init");
  return true;
}

void deinit() {
  if (!s_initialized) return;
  // No sensitive in-RAM buffers persist beyond a single build/merge call,
  // so this is mostly a flag flip.
  s_initialized = false;
}

void tick(uint32_t /*now_ms*/) {
  // No-op in v1. Reserved so a future internal scheduler can drive
  // build emission without a mesh-layer timer.
}

// ────────────────────────────────────────────────────────────────────────────
// BUILD (outbound)
// ────────────────────────────────────────────────────────────────────────────

bool build_baseline_share(BaselineShare* out, bool force) {
  if (!s_initialized || !out) return false;
  if (!throttle_allows_build(s_last_baseline_build_ms, force)) return false;

  memset(out, 0, sizeof(*out));

  // Pay for every draw before any raw bucket is copied in (dp.h BUDGET).
  // Refused: nothing is built, and the throttle clock and built counter
  // are left alone so the next session can try again.
  dp::Release rel(SHARE_DRAWS);
  if (!rel.ok()) return false;

  out->magic         = MAGIC_BASELINE;
  out->wire_version  = WIRE_VERSION_BASELINE;
  out->bucket_count  = baseline::BUCKET_COUNT;
  out->feature_count = baseline::FEATURE_COUNT;

  for (uint8_t i = 0; i < baseline::BUCKET_COUNT; i++) {
    baseline::RemoteBucketShare snap;
    if (!baseline::snapshot_bucket(i, &snap)) {
      // Should not happen; defensive. Never hand back a half-built share.
      memset(out, 0, sizeof(*out));
      return false;
    }
    BaselineShareBucket* b = &out->buckets[i];
    b->count = snap.count;
    for (uint8_t f = 0; f < baseline::FEATURE_COUNT; f++) {
      b->sum[f]    = snap.sum[f];
      b->sum_sq[f] = snap.sum_sq[f];
    }
    apply_dp_noise_to_bucket(b, rel);
  }
  if (!rel.complete()) {
    // A refused draw left a raw value in place: the share never leaves.
    memset(out, 0, sizeof(*out));
    return false;
  }

  s_last_baseline_build_ms = millis();
  s_total_baseline_built++;
  return true;
}

bool build_familiar_share(FamiliarShare* out, bool force) {
  if (!s_initialized || !out) return false;
  if (!throttle_allows_build(s_last_familiar_build_ms, force)) return false;

  memset(out, 0, sizeof(*out));
  out->magic        = MAGIC_FAMILIAR;
  out->wire_version = WIRE_VERSION_FAMILIAR;

  if (!familiar::snapshot_yesterday(out->yesterday, sizeof(out->yesterday))) {
    // Yesterday isn't valid yet (cold first day). Don't pollute the
    // throttle clock — let the next session try again.
    return false;
  }

  // Note: familiar::rotate_now already applied DP bit-flip noise to
  // yesterday on the most recent rotation, so we forward as-is.
  s_last_familiar_build_ms = millis();
  s_total_familiar_built++;
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// HANDLE (inbound)
// ────────────────────────────────────────────────────────────────────────────

bool handle_baseline_share(const BaselineShare* in, size_t bytes) {
  if (!s_initialized || !in) return false;

  if (bytes != sizeof(BaselineShare)) {
    s_total_rejected_size++;
    health_logging::logf(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
      "Federated: baseline share size %u != expected %u",
      (unsigned)bytes, (unsigned)sizeof(BaselineShare));
    return false;
  }
  if (in->magic != MAGIC_BASELINE) {
    s_total_rejected_magic++;
    return false;
  }
  if (in->wire_version != WIRE_VERSION_BASELINE) {
    s_total_rejected_version++;
    health_logging::logf(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
      "Federated: baseline share wire_version %u != expected %u",
      (unsigned)in->wire_version, (unsigned)WIRE_VERSION_BASELINE);
    return false;
  }
  if (in->bucket_count != baseline::BUCKET_COUNT
      || in->feature_count != baseline::FEATURE_COUNT) {
    // Shape mismatch — likely a peer running a different build target.
    // Bump the version counter (same semantic family: incompatible
    // structure) but log specifically so operators can tell them apart.
    s_total_rejected_version++;
    health_logging::logf(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
      "Federated: baseline share layout (buckets %u!=%u, features %u!=%u)",
      (unsigned)in->bucket_count, (unsigned)baseline::BUCKET_COUNT,
      (unsigned)in->feature_count, (unsigned)baseline::FEATURE_COUNT);
    return false;
  }

  uint16_t merged = 0;
  for (uint8_t i = 0; i < baseline::BUCKET_COUNT; i++) {
    baseline::RemoteBucketShare share;
    share.count = in->buckets[i].count;
    for (uint8_t f = 0; f < baseline::FEATURE_COUNT; f++) {
      share.sum[f]    = in->buckets[i].sum[f];
      share.sum_sq[f] = in->buckets[i].sum_sq[f];
    }
    if (baseline::merge_remote_bucket(i, share)) merged++;
  }

  s_total_baseline_merged++;
  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Federated: baseline share merged into %u/%u buckets",
    (unsigned)merged, (unsigned)baseline::BUCKET_COUNT);
  return true;
}

bool handle_familiar_share(const FamiliarShare* in, size_t bytes) {
  if (!s_initialized || !in) return false;

  if (bytes != sizeof(FamiliarShare)) {
    s_total_rejected_size++;
    health_logging::logf(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
      "Federated: familiar share size %u != expected %u",
      (unsigned)bytes, (unsigned)sizeof(FamiliarShare));
    return false;
  }
  if (in->magic != MAGIC_FAMILIAR) {
    s_total_rejected_magic++;
    health_logging::log(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
      "Federated: familiar share magic mismatch");
    return false;
  }
  if (in->wire_version != WIRE_VERSION_FAMILIAR) {
    s_total_rejected_version++;
    health_logging::logf(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
      "Federated: familiar share wire_version %u != expected %u",
      (unsigned)in->wire_version, (unsigned)WIRE_VERSION_FAMILIAR);
    return false;
  }

  if (!familiar::merge_remote_yesterday(in->yesterday, sizeof(in->yesterday))) {
    return false;
  }

  s_total_familiar_merged++;
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// INTROSPECTION
// ────────────────────────────────────────────────────────────────────────────

bool get_stats(Stats* out) {
  if (!out) return false;
  out->total_baseline_built     = s_total_baseline_built;
  out->total_familiar_built     = s_total_familiar_built;
  out->total_baseline_merged    = s_total_baseline_merged;
  out->total_familiar_merged    = s_total_familiar_merged;
  out->total_rejected_version   = s_total_rejected_version;
  out->total_rejected_magic     = s_total_rejected_magic;
  out->total_rejected_size      = s_total_rejected_size;
  // Report the age of the MORE-RECENT of the two share builds — most
  // useful operationally ("when did I last federate anything?").
  const uint32_t most_recent_build_ms =
      (s_last_familiar_build_ms > s_last_baseline_build_ms)
      ? s_last_familiar_build_ms : s_last_baseline_build_ms;
  out->last_build_age_ms = most_recent_build_ms == 0
                           ? 0
                           : elapsed_ms(most_recent_build_ms, millis());
  return true;
}

bool get_stats_for_export(Stats* out) {
  if (!get_stats(out)) return false;
  // One release, 7 draws, paid for up front (dp.h BUDGET). Refused or
  // cut short: the export is withheld and `out` holds nothing, raw or noised.
  dp::Release rel(7);
  if (!rel.ok()) { memset(out, 0, sizeof(*out)); return false; }
  out->total_baseline_built   = rel.u32(out->total_baseline_built,   1);
  out->total_familiar_built   = rel.u32(out->total_familiar_built,   1);
  out->total_baseline_merged  = rel.u32(out->total_baseline_merged,  1);
  out->total_familiar_merged  = rel.u32(out->total_familiar_merged,  1);
  out->total_rejected_version = rel.u32(out->total_rejected_version, 1);
  out->total_rejected_magic   = rel.u32(out->total_rejected_magic,   1);
  out->total_rejected_size    = rel.u32(out->total_rejected_size,    1);
  // last_build_age_ms is informational; not noised.
  if (!rel.complete()) { memset(out, 0, sizeof(*out)); return false; }
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// CONFORMANCE
// ────────────────────────────────────────────────────────────────────────────

bool conformance_self_test() {
  if (!s_initialized) return false;

  // Save stats and throttle so the test doesn't pollute them.
  const uint32_t saved_last_build_bl = s_last_baseline_build_ms;
  const uint32_t saved_last_build_fm = s_last_familiar_build_ms;
  const uint32_t saved_bb = s_total_baseline_built;
  const uint32_t saved_fb = s_total_familiar_built;
  const uint32_t saved_bm = s_total_baseline_merged;
  const uint32_t saved_fm = s_total_familiar_merged;
  const uint32_t saved_rv = s_total_rejected_version;
  const uint32_t saved_rmag = s_total_rejected_magic;
  const uint32_t saved_rsz = s_total_rejected_size;

  // 1. Build a baseline share with force=true (bypass throttle). The DP
  //    budget is enforced (dp.h): when this session cannot pay for the
  //    whole share, the build must be refused, spend nothing, and hand
  //    back no bucket data; when it can, the header must be right. At the
  //    shipped 4 ε budget a share (SHARE_DRAWS x ε) is always refused.
  static BaselineShare bsh;  // 'static' to keep stack pressure low (~1.2 KB)
  const bool affordable =
      (uint32_t)SHARE_DRAWS * dp::DEFAULT_EPSILON_X1000 <= dp::remaining_budget_x1000();
  const uint32_t spent_before = dp::consumed_budget_x1000();
  const bool built = build_baseline_share(&bsh, /*force*/true);
  bool b_ok;
  if (affordable) {
    b_ok = built;
  } else {
    bool empty = true;
    const uint8_t* bytes = (const uint8_t*)&bsh;
    for (size_t k = 0; k < sizeof(bsh); k++) {
      if (bytes[k] != 0) { empty = false; break; }
    }
    b_ok = !built && empty && dp::consumed_budget_x1000() == spent_before;
    // Steps 2-3 need a well-formed header to corrupt; build one by hand.
    bsh.magic         = MAGIC_BASELINE;
    bsh.wire_version  = WIRE_VERSION_BASELINE;
    bsh.bucket_count  = baseline::BUCKET_COUNT;
    bsh.feature_count = baseline::FEATURE_COUNT;
  }
  const bool b_hdr =
      bsh.magic == MAGIC_BASELINE &&
      bsh.wire_version == WIRE_VERSION_BASELINE &&
      bsh.bucket_count == baseline::BUCKET_COUNT &&
      bsh.feature_count == baseline::FEATURE_COUNT;

  // 2. handle_baseline_share with corrupted magic returns false and
  //    bumps reject_magic.
  BaselineShare bad = bsh;
  bad.magic = 0xDEADBEEFUL;
  const uint32_t before_mag = s_total_rejected_magic;
  const bool b_mag_rej = !handle_baseline_share(&bad, sizeof(bad));
  const bool b_mag_cnt = s_total_rejected_magic == before_mag + 1;

  // 3. wrong size → reject_size.
  const uint32_t before_sz = s_total_rejected_size;
  const bool b_sz_rej = !handle_baseline_share(&bsh, sizeof(bsh) - 1);
  const bool b_sz_cnt = s_total_rejected_size == before_sz + 1;

  // 4. Familiar share build may legitimately fail if no rotation has
  //    happened yet (yesterday cold). Treat both outcomes as "ok" but
  //    if it succeeded, exercise the round-trip.
  static FamiliarShare fsh;
  const bool fbuilt = build_familiar_share(&fsh, /*force*/true);
  bool f_round_trip = true;
  if (fbuilt) {
    f_round_trip = handle_familiar_share(&fsh, sizeof(fsh));
  }

  // Restore stats + throttle so the test doesn't pollute production
  // telemetry. This also zeroes the intentional-reject increments the
  // test made, keeping the counter meaning "rejects observed in prod".
  s_last_baseline_build_ms = saved_last_build_bl;
  s_last_familiar_build_ms = saved_last_build_fm;
  s_total_baseline_built  = saved_bb;
  s_total_familiar_built  = saved_fb;
  s_total_baseline_merged = saved_bm;
  s_total_familiar_merged = saved_fm;
  s_total_rejected_version = saved_rv;
  s_total_rejected_magic   = saved_rmag;
  s_total_rejected_size    = saved_rsz;

  const bool ok = b_ok && b_hdr && b_mag_rej && b_mag_cnt
                  && b_sz_rej && b_sz_cnt && f_round_trip;
  if (!ok) {
    health_logging::logf(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "Federated self-test FAIL: build=%d hdr=%d magrej=%d magcnt=%d szrej=%d szcnt=%d frt=%d",
      (int)b_ok, (int)b_hdr, (int)b_mag_rej, (int)b_mag_cnt,
      (int)b_sz_rej, (int)b_sz_cnt, (int)f_round_trip);
  } else {
    health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
      "Federated self-test: OK");
  }
  return ok;
}

}  // namespace federated
