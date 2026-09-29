/*
 * SecuraCV Canary — Differential-privacy utilities — Implementation
 *
 * Integer-arithmetic implementation of the Gaussian mechanism.
 *
 * Gaussian mechanism: to release a function f with (ε, δ)-DP, add
 * noise drawn from N(0, σ²) where
 *     σ = sensitivity · √(2 · ln(1.25 / δ)) / ε
 *
 * We pre-compute √(2 · ln(1.25 / δ)) in a small lookup table for the
 * common δ values, so we don't need libm's log on the ESP32. The common
 * default δ = 1e-5 gives √(2 · ln(125000)) ≈ 4.845, stored as 4845
 * (Q12.0 with x1000 implicit scaling).
 *
 * For ε = 1.0 and sensitivity = 1, σ ≈ 4.845. A 95% confidence band
 * is roughly ±2σ = ±10, which is enough to mask per-event granularity
 * in a counter that typically registers tens or hundreds per day but
 * small enough not to drown the signal.
 */

#include "dp.h"
#include "health_log.h"

#include <string.h>
#include <atomic>
#include <esp_system.h>  // esp_fill_random

namespace dp {

// ────────────────────────────────────────────────────────────────────────────
// PRE-COMPUTED SQRT(2 · LN(1.25/δ)) · 1000 TABLE
// ────────────────────────────────────────────────────────────────────────────
//
// Rather than computing log at runtime, we use a small table for the
// common δ values. Any δ that doesn't match falls back to the δ=1e-5
// row (the default); an alternative path does piecewise-linear
// interpolation on the δ scale but the noise insensitivity to δ
// (it enters through the slow √ln function) makes that unnecessary.
//
// sqrt(2 · ln(1.25 / δ)) values, scaled × 1000:
//   δ = 1e-3: sqrt(2 · ln(1250))     ≈ sqrt(14.26) ≈ 3.776  → 3776
//   δ = 1e-4: sqrt(2 · ln(12500))    ≈ sqrt(18.86) ≈ 4.342  → 4342
//   δ = 1e-5: sqrt(2 · ln(125000))   ≈ sqrt(23.48) ≈ 4.846  → 4846
//   δ = 1e-6: sqrt(2 · ln(1250000))  ≈ sqrt(28.09) ≈ 5.300  → 5300
//   δ = 1e-7: sqrt(2 · ln(12500000)) ≈ sqrt(32.69) ≈ 5.717  → 5717
static uint32_t sqrt_2ln_factor_x1000(uint32_t delta_inv) {
  if (delta_inv >= 10000000UL) return 5717;  // δ ≤ 1e-7
  if (delta_inv >= 1000000UL)  return 5300;  // δ ≤ 1e-6
  if (delta_inv >= 100000UL)   return 4846;  // δ ≤ 1e-5 (DEFAULT)
  if (delta_inv >= 10000UL)    return 4342;  // δ ≤ 1e-4
  return 3776;                                // δ ≤ 1e-3 (and weaker)
}

// ────────────────────────────────────────────────────────────────────────────
// SIGMA COMPUTATION
// ────────────────────────────────────────────────────────────────────────────

uint32_t compute_sigma_x1000(uint32_t sensitivity,
                             uint16_t epsilon_x1000,
                             uint32_t delta_inv) {
  if (epsilon_x1000 == 0) return UINT32_MAX;  // undefined; infinite noise
  const uint32_t factor_x1000 = sqrt_2ln_factor_x1000(delta_inv);

  // σ × 1000 = sensitivity × factor_x1000 × 1000 / epsilon_x1000
  // rearrange for 64-bit safety.
  const uint64_t num = (uint64_t)sensitivity * (uint64_t)factor_x1000 * 1000ULL;
  const uint64_t den = (uint64_t)epsilon_x1000;
  const uint64_t result = num / den;
  return result > UINT32_MAX ? UINT32_MAX : (uint32_t)result;
}

// ────────────────────────────────────────────────────────────────────────────
// NOISE GENERATION — Irwin–Hall approximation
// ────────────────────────────────────────────────────────────────────────────
//
// The sum of 12 independent Uniform(0, 1) draws has mean 6 and variance 1.
// Subtracting 6 gives an approximate N(0, 1). Adequate for DP: we don't
// need cryptographic indistinguishability between Irwin–Hall and true
// Gaussian, only calibrated noise scale.

int32_t gaussian_sample(uint32_t sigma_units) {
  if (sigma_units == 0) return 0;

  // Fill a small buffer from the hardware RNG.
  uint32_t raw[12];
  esp_fill_random(raw, sizeof(raw));

  // Each raw[i] → uniform in [0, 1001) → sum ∈ [0, 12012].
  // Approximate mean = 6006, variance ≈ 12 · (1001²/12) = 1001² ≈ 1e6.
  // Centered sum has sigma ≈ 1001 ≈ 1000.
  int64_t sum = 0;
  for (int i = 0; i < 12; i++) {
    sum += (int64_t)(raw[i] % 1001U);
  }
  const int64_t centered = sum - 6000;  // ≈ N(0, 1000²)

  // Scale to sigma_units: out = centered × sigma_units / 1000.
  const int64_t scaled = (centered * (int64_t)sigma_units) / 1000;
  if (scaled >  INT32_MAX) return INT32_MAX;
  if (scaled < -INT32_MAX) return -INT32_MAX;
  return (int32_t)scaled;
}

// ────────────────────────────────────────────────────────────────────────────
// BUDGET (enforced)
// ────────────────────────────────────────────────────────────────────────────
//
// One session ledger, spent by Release reservations only. The reservation is
// a compare-and-swap loop, so two tasks exporting at once can never together
// spend past the budget: one of them is refused instead.

static std::atomic<uint32_t> s_consumed_budget_x1000{0};
static std::atomic<uint32_t> s_withheld_releases{0};

// All or nothing: spend `cost` if the session budget covers all of it.
static bool try_spend(uint32_t cost_x1000) {
  if (cost_x1000 == 0) return false;
  uint32_t cur = s_consumed_budget_x1000.load();
  for (;;) {
    if (cur > DEFAULT_BUDGET_X1000 ||
        cost_x1000 > DEFAULT_BUDGET_X1000 - cur) {
      return false;
    }
    if (s_consumed_budget_x1000.compare_exchange_weak(cur, cur + cost_x1000)) {
      return true;
    }
    // cur was reloaded by the failed exchange; re-check against it.
  }
}

uint32_t remaining_budget_x1000() {
  const uint32_t c = s_consumed_budget_x1000.load();
  return c >= DEFAULT_BUDGET_X1000 ? 0 : DEFAULT_BUDGET_X1000 - c;
}

uint32_t consumed_budget_x1000() { return s_consumed_budget_x1000.load(); }

void reset_budget() {
  s_consumed_budget_x1000.store(0);
  health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "DP: per-session budget reset");
}

bool budget_exhausted() {
  return remaining_budget_x1000() == 0;
}

uint32_t withheld_releases() { return s_withheld_releases.load(); }

// ────────────────────────────────────────────────────────────────────────────
// RELEASE — the only way to draw calibrated noise
// ────────────────────────────────────────────────────────────────────────────

Release::Release(uint16_t draws, uint16_t epsilon_x1000, uint32_t delta_inv)
    : draws_left_(draws), epsilon_x1000_(epsilon_x1000), delta_inv_(delta_inv),
      ok_(false), short_(false) {
  // ε = 0 is infinite noise by definition and a caller bug in practice;
  // zero draws is nothing to pay for. Both refuse rather than spend.
  if (draws == 0 || epsilon_x1000 == 0) {
    s_withheld_releases.fetch_add(1);
    return;
  }
  const uint32_t cost = (uint32_t)draws * (uint32_t)epsilon_x1000;  // ≤ 65535²  < 2^32
  ok_ = try_spend(cost);
  if (!ok_) {
    s_withheld_releases.fetch_add(1);
    health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
      "DP: release of %u draw(s) withheld (needs %u, %u of %u left)",
      (unsigned)draws, (unsigned)cost, (unsigned)remaining_budget_x1000(),
      (unsigned)DEFAULT_BUDGET_X1000);
  }
}

// Pay one draw out of the reservation, and compute its σ. False (and the
// release marked short) when the reservation is refused or used up, or when
// the noise would round to nothing — a zero-σ draw is the raw value.
bool Release::take_draw(uint32_t sensitivity, uint32_t* sigma_units) {
  if (!ok_ || draws_left_ == 0 || sensitivity == 0) {
    if (!short_ && ok_) s_withheld_releases.fetch_add(1);
    short_ = true;
    return false;
  }
  const uint32_t sigma_x1000 = compute_sigma_x1000(sensitivity, epsilon_x1000_, delta_inv_);
  // gaussian_sample takes sigma in the SAME units as the output — so for
  // a counter (units of 1), sigma is sigma_x1000 / 1000. Round-to-nearest.
  const uint32_t units = sigma_x1000 >= UINT32_MAX - 500 ? UINT32_MAX / 1000
                                                         : (sigma_x1000 + 500) / 1000;
  if (units == 0) {
    if (!short_) s_withheld_releases.fetch_add(1);
    short_ = true;
    return false;
  }
  draws_left_--;
  *sigma_units = units;
  return true;
}

uint32_t Release::u32(uint32_t value, uint32_t sensitivity) {
  uint32_t sigma = 0;
  if (!take_draw(sensitivity, &sigma)) return 0;
  const int64_t sum = (int64_t)value + (int64_t)gaussian_sample(sigma);
  // Clamp to [0, UINT32_MAX] preserving counter semantics.
  if (sum < 0) return 0;
  if (sum > (int64_t)UINT32_MAX) return UINT32_MAX;
  return (uint32_t)sum;
}

int32_t Release::i32(int32_t value, uint32_t sensitivity) {
  uint32_t sigma = 0;
  if (!take_draw(sensitivity, &sigma)) return 0;
  const int64_t sum = (int64_t)value + (int64_t)gaussian_sample(sigma);
  if (sum >  INT32_MAX) return INT32_MAX;
  if (sum <  INT32_MIN) return INT32_MIN;
  return (int32_t)sum;
}

// ────────────────────────────────────────────────────────────────────────────
// CONFORMANCE
// ────────────────────────────────────────────────────────────────────────────

bool conformance_self_test() {
  // Draws gaussian_sample directly: no Release, so no budget is spent.
  constexpr uint32_t N = 1024;
  constexpr uint32_t SIGMA = 100;  // test sigma; large enough for stable stats

  int64_t sum = 0;
  int64_t sum_sq = 0;
  for (uint32_t i = 0; i < N; i++) {
    const int32_t s = gaussian_sample(SIGMA);
    sum    += s;
    sum_sq += (int64_t)s * s;
  }

  const int64_t mean_x1000 = (sum * 1000) / (int64_t)N;
  const int64_t variance   = (sum_sq / (int64_t)N) - (sum / (int64_t)N) * (sum / (int64_t)N);

  // Compute sigma estimate via isqrt (reuse from existing idiom; simple Newton here).
  uint32_t est_sigma = 0;
  {
    int64_t v = variance > 0 ? variance : 0;
    uint64_t x = (uint64_t)v;
    uint64_t root = 0;
    uint64_t bit = (uint64_t)1 << 30;
    while (bit > x) bit >>= 2;
    while (bit) {
      if (x >= root + bit) { x -= root + bit; root = (root >> 1) + bit; }
      else                 { root >>= 1; }
      bit >>= 2;
    }
    est_sigma = (uint32_t)root;
  }

  // Expectations:
  //   |mean|  < SIGMA / 10       →  |mean_x1000| < 10000
  //   |est_sigma - SIGMA| < SIGMA / 5
  const bool mean_ok  = (mean_x1000 > -10000 && mean_x1000 < 10000);
  const int32_t sigma_err = (int32_t)est_sigma - (int32_t)SIGMA;
  const bool sigma_ok = (sigma_err > -(int32_t)(SIGMA / 5) &&
                         sigma_err <  (int32_t)(SIGMA / 5));

  const bool ok = mean_ok && sigma_ok;
  if (!ok) {
    health_logging::logf(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "DP self-test FAIL: mean×1000=%lld, est_sigma=%u (target %u)",
      (long long)mean_x1000, (unsigned)est_sigma, (unsigned)SIGMA);
  } else {
    health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
      "DP self-test OK (mean×1000=%lld, est_sigma=%u)",
      (long long)mean_x1000, (unsigned)est_sigma);
  }

  return ok;
}

}  // namespace dp
