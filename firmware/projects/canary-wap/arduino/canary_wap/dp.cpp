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
// CALIBRATED COUNTER NOISE
// ────────────────────────────────────────────────────────────────────────────

static int32_t calibrated_noise(uint32_t sensitivity,
                                uint16_t epsilon_x1000,
                                uint32_t delta_inv) {
  const uint32_t sigma_x1000 = compute_sigma_x1000(sensitivity, epsilon_x1000, delta_inv);
  // gaussian_sample takes sigma in the SAME units as the output — so for
  // a counter (units of 1), sigma is sigma_x1000 / 1000. Round-to-nearest.
  const uint32_t sigma_units = (sigma_x1000 + 500) / 1000;
  if (sigma_units == 0) return 0;
  consume_budget(epsilon_x1000);
  return gaussian_sample(sigma_units);
}

uint32_t noisy_u32(uint32_t value, uint32_t sensitivity,
                   uint16_t epsilon_x1000, uint32_t delta_inv) {
  const int32_t noise = calibrated_noise(sensitivity, epsilon_x1000, delta_inv);
  // Clamp to [0, UINT32_MAX] preserving counter semantics.
  if (noise < 0 && (uint32_t)(-noise) > value) return 0;
  const int64_t sum = (int64_t)value + (int64_t)noise;
  if (sum < 0) return 0;
  if (sum > (int64_t)UINT32_MAX) return UINT32_MAX;
  return (uint32_t)sum;
}

uint16_t noisy_u16(uint16_t value, uint16_t sensitivity,
                   uint16_t epsilon_x1000, uint32_t delta_inv) {
  const uint32_t n = noisy_u32(value, sensitivity, epsilon_x1000, delta_inv);
  return n > UINT16_MAX ? UINT16_MAX : (uint16_t)n;
}

uint8_t noisy_u8(uint8_t value, uint8_t sensitivity,
                 uint16_t epsilon_x1000, uint32_t delta_inv) {
  const uint32_t n = noisy_u32(value, sensitivity, epsilon_x1000, delta_inv);
  return n > UINT8_MAX ? UINT8_MAX : (uint8_t)n;
}

int32_t noisy_i32(int32_t value, uint32_t sensitivity,
                  uint16_t epsilon_x1000, uint32_t delta_inv) {
  const int32_t noise = calibrated_noise(sensitivity, epsilon_x1000, delta_inv);
  const int64_t sum = (int64_t)value + (int64_t)noise;
  if (sum >  INT32_MAX) return INT32_MAX;
  if (sum <  INT32_MIN) return INT32_MIN;
  return (int32_t)sum;
}

// ────────────────────────────────────────────────────────────────────────────
// BUDGET TRACKING (advisory)
// ────────────────────────────────────────────────────────────────────────────

static uint32_t s_consumed_budget_x1000 = 0;

void consume_budget(uint16_t epsilon_x1000) {
  // Saturate at UINT32_MAX — this counter is informational, not enforced.
  if (s_consumed_budget_x1000 > UINT32_MAX - epsilon_x1000) {
    s_consumed_budget_x1000 = UINT32_MAX;
  } else {
    s_consumed_budget_x1000 += epsilon_x1000;
  }
}

uint32_t remaining_budget_x1000() {
  if (s_consumed_budget_x1000 >= DEFAULT_BUDGET_X1000) return 0;
  return DEFAULT_BUDGET_X1000 - s_consumed_budget_x1000;
}

uint32_t consumed_budget_x1000() { return s_consumed_budget_x1000; }

void reset_budget() {
  s_consumed_budget_x1000 = 0;
  health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "DP: per-session budget reset");
}

bool budget_exhausted() {
  return remaining_budget_x1000() == 0;
}

// ────────────────────────────────────────────────────────────────────────────
// CONFORMANCE
// ────────────────────────────────────────────────────────────────────────────

bool conformance_self_test() {
  // Save budget so the test doesn't pollute real telemetry.
  const uint32_t saved_budget = s_consumed_budget_x1000;

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

  // Restore budget.
  s_consumed_budget_x1000 = saved_budget;
  return ok;
}

}  // namespace dp
