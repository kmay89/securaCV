/* test_dp_budget.cpp — the differential-privacy budget is enforced and fails
 * closed.
 *
 * Compiles the REAL dp.cpp (over stubs/dp: an Arduino.h with the fixed-width
 * types and an esp_fill_random on a seeded host PRNG) and pins dp.h's BUDGET
 * contract:
 *
 *   - a Release reserves draws x ε up front, all or nothing: a release the
 *     session budget cannot cover spends nothing and draws nothing;
 *   - a refused draw returns 0 whatever the input (the output carries no
 *     information about the counter) and marks the release incomplete —
 *     over-drawing a reservation, a refused release, sensitivity 0, ε 0;
 *   - the budget runs out exactly at DEFAULT_BUDGET_X1000 and reset_budget()
 *     (session rotation) restores it;
 *   - concurrent releases can never together overspend (the reservation is
 *     a compare-and-swap);
 *   - an honored draw is the counter plus noise of the calibrated scale.
 *
 * What it does not pin: that every exporter uses a Release (dp.h no longer
 * offers any other way to draw noise, so an exporter that skipped it would
 * not compile), and the on-device RNG. */

#include "dp.h"
#include "health_log.h"

#include <atomic>
#include <math.h>
#include <stdio.h>
#include <thread>
#include <vector>

// health_log.h's two sinks, which the sketch defines in canary_wap.ino.
void health_log(LogLevel, LogCategory, const char*) {}
void log_health(LogLevel, LogCategory, const char*, const char*) {}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); g_fail++; } \
  else { printf("ok   %s\n", msg); } } while (0)

int main() {
  const uint32_t B = dp::DEFAULT_BUDGET_X1000;
  const uint16_t E = dp::DEFAULT_EPSILON_X1000;
  CHECK(B == 4000 && E == 1000, "shipped parameters: 4 ε a session, 1 ε a draw");

  dp::reset_budget();
  CHECK(dp::remaining_budget_x1000() == B, "a fresh session holds the whole budget");

  // ── A release bigger than the budget: all or nothing. ────────────────────
  {
    const uint32_t withheld0 = dp::withheld_releases();
    dp::Release rel(9);   // notify's export: 9 draws
    CHECK(!rel.ok() && !rel.complete(), "a 9 ε release is refused under a 4 ε budget");
    CHECK(dp::consumed_budget_x1000() == 0, "a refused release spends nothing");
    CHECK(rel.u32(123456, 1) == 0 && rel.u32(0, 1) == 0 && rel.u32(UINT32_MAX, 1) == 0,
          "a refused release's draws are 0 whatever the counter");
    CHECK(rel.i32(-5000, 1) == 0, "signed draws too");
    CHECK(dp::withheld_releases() == withheld0 + 1, "the refusal is counted once");
  }

  // ── Releases that fit, until the budget is spent. ────────────────────────
  {
    dp::Release rel(3);
    CHECK(rel.ok(), "a 3 ε release fits");
    CHECK(dp::consumed_budget_x1000() == 3 * E, "and is charged 3 ε up front");
    const uint32_t a = rel.u32(1000, 1);
    const uint32_t b = rel.u32(1000, 1);
    const int32_t  c = rel.i32(-1000, 1);
    CHECK(rel.complete(), "three paid draws complete the release");
    // σ ≈ 5 at ε = 1, δ = 1e-5; the Irwin-Hall sum is bounded at ±6σ.
    CHECK(a >= 1000 - 30 && a <= 1000 + 30 && b >= 1000 - 30 && b <= 1000 + 30,
          "an honored draw is the counter plus bounded noise");
    CHECK(c >= -1030 && c <= -970, "a signed honored draw likewise");
  }
  {
    dp::Release rel(2);
    CHECK(!rel.ok(), "2 ε more does not fit in the 1 ε left");
    CHECK(dp::consumed_budget_x1000() == 3 * E, "and spends nothing");
  }
  {
    dp::Release rel(1);
    CHECK(rel.ok(), "the last 1 ε fits exactly");
    (void)rel.u32(7, 1);
    CHECK(rel.complete(), "and draws");
  }
  CHECK(dp::budget_exhausted() && dp::remaining_budget_x1000() == 0, "the budget is spent");
  {
    dp::Release rel(1);
    CHECK(!rel.ok() && rel.u32(42, 1) == 0, "an exhausted session releases nothing");
  }

  // ── Rotation. ────────────────────────────────────────────────────────────
  dp::reset_budget();
  CHECK(dp::remaining_budget_x1000() == B, "session rotation restores the budget");

  // ── Draws the reservation did not pay for. ──────────────────────────────
  {
    dp::Release rel(1);
    (void)rel.u32(10, 1);
    CHECK(rel.complete(), "one paid draw");
    CHECK(rel.u32(999999, 1) == 0, "a second, unpaid draw returns 0");
    CHECK(!rel.complete(), "and marks the release incomplete, so the export is withheld");
  }
  {
    dp::Release rel(1);
    CHECK(rel.ok(), "sensitivity 0: the reservation itself is fine");
    CHECK(rel.u32(31337, 0) == 0 && !rel.complete(),
          "but a zero-sensitivity draw (the raw value with no noise) is refused");
  }
  {
    const uint32_t before = dp::consumed_budget_x1000();
    dp::Release rel(1, /*epsilon_x1000=*/0);
    CHECK(!rel.ok() && dp::consumed_budget_x1000() == before, "ε = 0 is refused, not spent");
    dp::Release none(0);
    CHECK(!none.ok() && dp::consumed_budget_x1000() == before, "a zero-draw release is refused");
  }

  // ── Concurrency: releases racing on one budget never overspend. ─────────
  dp::reset_budget();
  {
    std::atomic<int> granted{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; t++) {
      ts.emplace_back([&granted]() {
        for (int i = 0; i < 500; i++) {
          dp::Release rel(1);
          if (rel.ok()) granted++;
        }
      });
    }
    for (auto& t : ts) t.join();
    CHECK(granted.load() == (int)(B / E), "8 racing threads get exactly budget / ε releases");
    CHECK(dp::consumed_budget_x1000() == B, "and the ledger ends exactly at the budget");
  }

  // ── Noise scale: the calibrated σ, not a token amount. ───────────────────
  {
    const uint32_t sigma_x1000 = dp::compute_sigma_x1000(1, E, dp::DEFAULT_DELTA_INV);
    CHECK(sigma_x1000 == 4846, "σ at sensitivity 1, ε 1, δ 1e-5 is 4.846");
    double sum = 0, sum_sq = 0;
    const int N = 4000;
    for (int i = 0; i < N; i++) {
      dp::reset_budget();
      dp::Release rel(1);
      const double d = (double)rel.i32(0, 1);
      sum += d;
      sum_sq += d * d;
    }
    const double mean = sum / N;
    const double sd = sqrt(sum_sq / N - mean * mean);
    printf("     noise mean=%.3f sd=%.3f (target sd 5)\n", mean, sd);
    CHECK(fabs(mean) < 0.5 && sd > 4.0 && sd < 6.0, "draw noise is centered at the calibrated scale");
  }

  if (g_fail == 0) {
    printf("ALL dp budget tests PASSED\n");
    return 0;
  }
  printf("%d FAILED\n", g_fail);
  return 1;
}
