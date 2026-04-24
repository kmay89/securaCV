/*
 * SecuraCV Canary — Differential-privacy utilities (Phase 7)
 * Version 0.1.0
 *
 * WHY THIS EXISTS
 * ===============
 * rf_presence, household, familiar, and baseline each expose counters
 * over the device's HTTP / MQTT surface (how many BLE advertisements
 * were dropped, how many RPAs resolved, how many anomaly events fired
 * in the last hour). Those raw counters leak information about a
 * household: counter(9 am) >> counter(10 am) tells a passive observer
 * that 9 am is when people come and go.
 *
 * The Gaussian mechanism of differential privacy masks each exported
 * counter with calibrated noise:
 *     noisy(x) = clamp(x + N(0, σ²))
 *     σ = sensitivity · √(2 · ln(1.25 / δ)) / ε
 * so that a single additional event (sensitivity = 1) changes the
 * observable by O(σ), which is information-theoretically bounded by
 * (ε, δ). The local firmware ALWAYS uses the raw counter — only values
 * crossing the export boundary pick up noise.
 *
 * HOW TO USE
 * ==========
 *   // Inside a get_stats_for_export():
 *   uint32_t exported = dp::noisy_u32(s_total_events, /*sensitivity*/1);
 *
 *   // Default is ε=1.0, δ=1e-5. Override for tighter/looser queries:
 *   uint32_t v = dp::noisy_u32(x, sensitivity, /*eps_x1000*/500, /*d_inv*/1000000);
 *
 * Do NOT call these on values used for local decisions (is_anomaly,
 * resolve_rpa, is_ambient). Those must stay noise-free so the firmware
 * doesn't silently quiet or fire alerts based on random drift.
 *
 * BUDGET TRACKING (advisory in v1)
 * ================================
 * Differential privacy composes: every noisy query spends some ε of
 * the total budget. v1 exposes consume_budget() / remaining_budget()
 * as advisory hooks; future work can reject further exports once the
 * budget is exhausted. The budget resets on rf_presence session
 * rotation so an attacker observing over a 4 h window doesn't get
 * arbitrarily precise aggregation.
 */

#ifndef SECURACV_DP_H
#define SECURACV_DP_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

namespace dp {

// ════════════════════════════════════════════════════════════════════════════
// CONSTANTS
// ════════════════════════════════════════════════════════════════════════════

// Default privacy parameters.
// ε = 1.0 (strong but usable); expressed as 1000 in millis for integer math.
static const uint16_t DEFAULT_EPSILON_X1000 = 1000;
// δ = 1e-5 (per-query); stored as 1/δ for the integer log computation.
static const uint32_t DEFAULT_DELTA_INV     = 100000;

// Per-session budget (millis of ε). 4.0 = 4.0 × 1000.
// Chosen to allow ~4 noisy queries per rotation at the default ε=1.0,
// which matches the typical polling cadence of a user-facing UI.
static const uint32_t DEFAULT_BUDGET_X1000  = 4000;

// ════════════════════════════════════════════════════════════════════════════
// NOISE GENERATION
// ════════════════════════════════════════════════════════════════════════════

// Draw a zero-mean Gaussian with sigma = sigma_units (integer units).
// Uses the Irwin–Hall sum-of-12-uniforms approximation. Good to ~3σ;
// beyond that the tails are slightly thinner than true Gaussian. Fine
// for counter noise — we aren't doing cryptographic simulation here.
int32_t gaussian_sample(uint32_t sigma_units);

// ════════════════════════════════════════════════════════════════════════════
// CALIBRATED COUNTER NOISE
// ════════════════════════════════════════════════════════════════════════════

// Apply Gaussian-mechanism noise calibrated for (ε, δ). Clamps to
// the output range (unsigned counters cannot go negative).
uint32_t noisy_u32(uint32_t value, uint32_t sensitivity,
                   uint16_t epsilon_x1000 = DEFAULT_EPSILON_X1000,
                   uint32_t delta_inv     = DEFAULT_DELTA_INV);

uint16_t noisy_u16(uint16_t value, uint16_t sensitivity,
                   uint16_t epsilon_x1000 = DEFAULT_EPSILON_X1000,
                   uint32_t delta_inv     = DEFAULT_DELTA_INV);

uint8_t  noisy_u8 (uint8_t  value, uint8_t  sensitivity,
                   uint16_t epsilon_x1000 = DEFAULT_EPSILON_X1000,
                   uint32_t delta_inv     = DEFAULT_DELTA_INV);

int32_t  noisy_i32(int32_t  value, uint32_t sensitivity,
                   uint16_t epsilon_x1000 = DEFAULT_EPSILON_X1000,
                   uint32_t delta_inv     = DEFAULT_DELTA_INV);

// ════════════════════════════════════════════════════════════════════════════
// BUDGET (advisory)
// ════════════════════════════════════════════════════════════════════════════

// Record that a query consumed epsilon_x1000 of budget. In v1 this is
// purely diagnostic — no enforcement.
void consume_budget(uint16_t epsilon_x1000);

// Remaining budget in millis of ε.
uint32_t remaining_budget_x1000();

// Total consumed since last reset, for diagnostics.
uint32_t consumed_budget_x1000();

// Reset the per-session budget. Call from rf_presence::rotate_session().
void reset_budget();

// Convenience: has the per-session budget been exceeded?
bool budget_exhausted();

// ════════════════════════════════════════════════════════════════════════════
// INTROSPECTION
// ════════════════════════════════════════════════════════════════════════════

// Returns the Gaussian-mechanism sigma (in integer units × 1000) for
// the given (sensitivity, ε, δ). Exposed for diagnostics / the setup
// wizard's "privacy level" display.
uint32_t compute_sigma_x1000(uint32_t sensitivity,
                             uint16_t epsilon_x1000,
                             uint32_t delta_inv);

// ════════════════════════════════════════════════════════════════════════════
// CONFORMANCE
// ════════════════════════════════════════════════════════════════════════════

// Draw N samples of gaussian_sample(sigma), verify:
//   |sample mean| < sigma/10           (centering)
//   |sample stddev - sigma| < sigma/5   (scale)
// Uses N=1024 for reasonable statistical stability at compile-time-
// fixed cost (~32 KB of esp_fill_random draws).
bool conformance_self_test();

}  // namespace dp

#endif  // SECURACV_DP_H
