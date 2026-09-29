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
 *   - a reboot inside the session does not: every reservation is persisted
 *     with its epoch before a draw is honored, restore_budget() picks the
 *     spend back up, a failed write refuses the release, and a ledger that
 *     cannot be read (or names a later epoch, or more than the budget)
 *     reads as spent; so does a budget nobody restored;
 *   - a rotation cannot be raced: a release reserved before a reset can
 *     neither draw nor complete after it;
 *   - concurrent releases can never together overspend (the reservation is
 *     a compare-and-swap);
 *   - an honored draw is the counter plus noise of the calibrated scale.
 *
 * What it does not pin: that every exporter uses a Release (dp.h no longer
 * offers any other way to draw noise, so an exporter that skipped it would
 * not compile), the on-device RNG, and the NVS ledger_store in
 * rf_presence.cpp (a fake stands in for it here). */

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

// A fake ledger_store: the NVS record a reboot would find.
static bool     g_store_present = false;
static uint32_t g_store_epoch = 0;
static uint32_t g_store_consumed = 0;
static bool     g_store_read_fails = false;
static bool     g_store_write_fails = false;
static uint32_t g_store_writes = 0;

namespace dp {
namespace ledger_store {
Read read(uint32_t* epoch, uint32_t* consumed_x1000) {
  if (g_store_read_fails) return READ_FAILED;
  if (!g_store_present) return READ_ABSENT;
  *epoch = g_store_epoch;
  *consumed_x1000 = g_store_consumed;
  return READ_OK;
}
bool write(uint32_t epoch, uint32_t consumed_x1000) {
  if (g_store_write_fails) return false;
  g_store_present = true;
  g_store_epoch = epoch;
  g_store_consumed = consumed_x1000;
  g_store_writes++;
  return true;
}
}  // namespace ledger_store
}  // namespace dp

static int g_fail = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); g_fail++; } \
  else { printf("ok   %s\n", msg); } } while (0)

int main() {
  const uint32_t B = dp::DEFAULT_BUDGET_X1000;
  const uint16_t E = dp::DEFAULT_EPSILON_X1000;
  CHECK(B == 4000 && E == 1000, "shipped parameters: 4 ε a session, 1 ε a draw");

  // ── Before the ledger is restored, nothing is released. ─────────────────
  {
    CHECK(dp::budget_exhausted(), "a budget nobody restored reads as spent");
    dp::Release rel(1);
    CHECK(!rel.ok() && rel.u32(42, 1) == 0, "and releases nothing");
  }
  CHECK(dp::restore_budget(1), "a first boot (no stored ledger) restores");
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

  // ── A reboot inside the session does not refill it. ──────────────────────
  CHECK(g_store_present && g_store_epoch == 1 && g_store_consumed == B,
        "every granted reservation was persisted with its epoch");
  CHECK(dp::restore_budget(1) && dp::budget_exhausted(),
        "a reboot into the same epoch restores the spend, not a fresh budget");
  {
    dp::Release rel(1);
    CHECK(!rel.ok(), "so the rebooted session releases nothing more");
  }
  CHECK(dp::restore_budget(2) && dp::remaining_budget_x1000() == B,
        "a boot into a later epoch (an earlier session's ledger) is a fresh budget");
  {
    dp::Release rel(2);
    CHECK(rel.ok() && g_store_epoch == 2 && g_store_consumed == 2 * E,
          "and its first reservation is persisted against the new epoch");
  }
  CHECK(dp::restore_budget(2) && dp::consumed_budget_x1000() == 2 * E,
        "a partial spend survives a reboot exactly");

  // ── Failures of the store fail closed. ──────────────────────────────────
  {
    const uint32_t writes = g_store_writes;
    g_store_write_fails = true;
    dp::Release rel(1);
    CHECK(!rel.ok() && rel.u32(99, 1) == 0, "a reservation that cannot be persisted is refused");
    CHECK(dp::consumed_budget_x1000() == 2 * E && g_store_writes == writes,
          "and leaves the ledger as it was");
    g_store_write_fails = false;
  }
  g_store_read_fails = true;
  CHECK(!dp::restore_budget(2) && dp::budget_exhausted(), "an unreadable ledger reads as spent");
  g_store_read_fails = false;
  g_store_epoch = 9;
  CHECK(!dp::restore_budget(2) && dp::budget_exhausted(),
        "a ledger naming a later epoch than the session reads as spent");
  g_store_epoch = 2;
  g_store_consumed = B + 1;
  CHECK(!dp::restore_budget(2) && dp::budget_exhausted(),
        "a ledger holding more than the budget reads as spent");
  g_store_consumed = 2 * E;

  // ── Rotation. ────────────────────────────────────────────────────────────
  dp::reset_budget(3);
  CHECK(dp::remaining_budget_x1000() == B, "session rotation restores the budget");

  // ── A release cannot cross a rotation. ───────────────────────────────────
  {
    dp::Release old_rel(2);
    CHECK(old_rel.ok(), "a release reserved in session 3");
    (void)old_rel.u32(5, 1);
    dp::reset_budget(4);          // another task rotates the session
    CHECK(old_rel.u32(123456, 1) == 0, "cannot draw after the rotation");
    CHECK(!old_rel.complete(), "and is not complete, so its export is withheld");
    dp::Release fresh(4);
    CHECK(fresh.ok() && dp::remaining_budget_x1000() == 0,
          "the new session's budget is its own, and exactly the budget");
  }
  {
    dp::reset_budget(5);
    dp::Release rel(1);
    (void)rel.u32(5, 1);
    CHECK(rel.complete(), "a release whose draws all landed is complete ...");
    dp::reset_budget(6);
    CHECK(!rel.complete(), "... until the session it was paid from is reset");
  }
  {
    dp::reset_budget(7);
    dp::Release rel(1);
    CHECK(dp::restore_budget(7) && !rel.complete() && rel.u32(1, 1) == 0,
          "a restore is a new generation too");
  }
  dp::reset_budget(8);

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
  dp::reset_budget(9);
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
    CHECK(g_store_epoch == 9 && g_store_consumed == B, "and the persisted ledger says so");
  }

  // ── Noise scale: the calibrated σ, not a token amount. ───────────────────
  {
    const uint32_t sigma_x1000 = dp::compute_sigma_x1000(1, E, dp::DEFAULT_DELTA_INV);
    CHECK(sigma_x1000 == 4846, "σ at sensitivity 1, ε 1, δ 1e-5 is 4.846");
    double sum = 0, sum_sq = 0;
    const int N = 4000;
    for (int i = 0; i < N; i++) {
      dp::reset_budget(10);
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
