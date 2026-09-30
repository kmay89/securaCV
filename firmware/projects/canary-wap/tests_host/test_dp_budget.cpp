/* test_dp_budget.cpp — the differential-privacy budget is enforced and fails
 * closed.
 *
 * Compiles the REAL dp.cpp (over stubs/dp: an Arduino.h with the fixed-width
 * types and an esp_fill_random on a seeded host PRNG) and pins dp.h's BUDGET
 * contract:
 *
 *   - a Release reserves draws x ε up front, all or nothing: a release the
 *     window's budget cannot cover spends nothing and draws nothing;
 *   - a refused draw returns 0 whatever the input (the output carries no
 *     information about the counter) and marks the release incomplete —
 *     over-drawing a reservation, a refused release, sensitivity 0, ε 0;
 *   - the budget runs out exactly at DEFAULT_BUDGET_X1000, and only
 *     refill_if_due() restores it, and only once BUDGET_WINDOW_MS of uptime
 *     has passed since the last refill or boot (wrap-safe), never before a
 *     restore;
 *   - nothing a caller can trigger refills it: a manual session rotation
 *     (the epoch moves, dp is not called) followed by a reboot carries the
 *     spend over, and a reboot restarts the window rather than refilling;
 *     every reservation and refill is persisted before a draw is honored,
 *     a failed write refuses the release, and a ledger that cannot be read
 *     (or names a later epoch, or more than the budget) reads as spent; so
 *     does a budget nobody restored;
 *   - rf_presence.cpp and rf_presence_api.h are wired that way (source
 *     pins): rotate_session() never touches dp, update() is the only
 *     refill, init() restores, and GET /api/rf/conformance does not rotate
 *     unless asked to;
 *   - a refill cannot be raced: a release reserved before a refill can
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
#include "beacon_source_scan.h"   // generic source-pin helpers (read, strip, body)

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

#ifndef RF_PRESENCE_CPP
#define RF_PRESENCE_CPP "../arduino/canary_wap/rf_presence.cpp"
#endif
#ifndef RF_PRESENCE_API_H
#define RF_PRESENCE_API_H "../arduino/canary_wap/rf_presence_api.h"
#endif

// The device's uptime clock, as rf_presence::update() would pass it.
static uint32_t g_now = 5000;

// One budget window later: the clock-driven refill, the only one there is.
static bool next_window(uint32_t epoch) {
  g_now += dp::BUDGET_WINDOW_MS;
  return dp::refill_if_due(epoch, g_now);
}

int main() {
  const uint32_t B = dp::DEFAULT_BUDGET_X1000;
  const uint16_t E = dp::DEFAULT_EPSILON_X1000;
  CHECK(B == 4000 && E == 1000, "shipped parameters: 4 ε a window, 1 ε a draw");
  CHECK(dp::BUDGET_WINDOW_MS == 4UL * 60 * 60 * 1000, "the window is 4 h of uptime");

  // ── Before the ledger is restored, nothing is released or refilled. ─────
  {
    CHECK(dp::budget_exhausted(), "a budget nobody restored reads as spent");
    dp::Release rel(1);
    CHECK(!rel.ok() && rel.u32(42, 1) == 0, "and releases nothing");
    CHECK(!next_window(1) && !next_window(1) && dp::budget_exhausted(),
          "and never refills, however long the device runs (safe mode)");
  }
  CHECK(dp::restore_budget(1, g_now), "a first boot (no stored ledger) restores");
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

  // ── A reboot does not refill it. ─────────────────────────────────────────
  CHECK(g_store_present && g_store_epoch == 1 && g_store_consumed == B,
        "every granted reservation was persisted with its epoch");
  CHECK(dp::restore_budget(1, g_now) && dp::budget_exhausted(),
        "a reboot into the same epoch restores the spend, not a fresh budget");
  {
    dp::Release rel(1);
    CHECK(!rel.ok(), "so the rebooted device releases nothing more");
  }

  // ── Nor does a manual rotation (POST /api/rf/rotate, the opt-in
  //    conformance check). rotate_session() bumps the epoch and never calls
  //    dp (pinned below), so the only way a rotation reaches the budget is
  //    through the next boot's restore, with the new epoch. ───────────────
  CHECK(dp::restore_budget(2, g_now) && dp::budget_exhausted(),
        "a boot after a manual rotation (a later epoch) carries the spend over");
  CHECK(dp::restore_budget(50, g_now) && dp::budget_exhausted(),
        "so does a boot after fifty of them");
  {
    dp::Release rel(1);
    CHECK(!rel.ok(), "rotating and rebooting on demand buys no ε");
  }

  // ── The clock refills it: once a window, never early. ───────────────────
  {
    const uint32_t writes = g_store_writes;
    CHECK(!dp::refill_if_due(50, g_now + dp::BUDGET_WINDOW_MS - 1) && dp::budget_exhausted(),
          "one ms short of a window since the restore: no refill");
    CHECK(g_store_writes == writes, "and nothing written");
    CHECK(next_window(50) && dp::remaining_budget_x1000() == B,
          "a whole window after the restore: a fresh budget");
    CHECK(g_store_writes == writes + 1 && g_store_epoch == 50 && g_store_consumed == 0,
          "and the refill is persisted, so a reboot restores the fresh window");
    CHECK(!dp::refill_if_due(51, g_now) && !dp::refill_if_due(51, g_now + 1000),
          "a second refill inside the same window is refused");
  }
  {
    dp::Release rel(2);
    CHECK(rel.ok() && g_store_epoch == 50 && g_store_consumed == 2 * E,
          "the window's first reservation is persisted against its epoch");
  }
  CHECK(dp::restore_budget(51, g_now + 10) && dp::consumed_budget_x1000() == 2 * E,
        "a partial spend survives a reboot (after a rotation) exactly");
  CHECK(!dp::refill_if_due(51, g_now + 10 + dp::BUDGET_WINDOW_MS - 1) &&
        dp::consumed_budget_x1000() == 2 * E,
        "a reboot restarts the window: it postpones the refill, never brings it forward");
  g_now += 10;

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
  CHECK(!dp::restore_budget(51, g_now) && dp::budget_exhausted(), "an unreadable ledger reads as spent");
  g_store_read_fails = false;
  g_store_epoch = 99;
  CHECK(!dp::restore_budget(51, g_now) && dp::budget_exhausted(),
        "a ledger naming a later epoch than the session reads as spent");
  g_store_epoch = 51;
  g_store_consumed = B + 1;
  CHECK(!dp::restore_budget(51, g_now) && dp::budget_exhausted(),
        "a ledger holding more than the budget reads as spent");
  g_store_consumed = 2 * E;
  CHECK(next_window(52) && dp::remaining_budget_x1000() == B,
        "an unreadable ledger is spent only until the next window");
  {
    const uint32_t writes = g_store_writes;
    {
      dp::Release spend(3);
      CHECK(spend.ok(), "3 ε spent in the refilled window");
    }
    g_store_write_fails = true;
    CHECK(next_window(53) && dp::remaining_budget_x1000() == B,
          "a refill whose write fails still refills in RAM ...");
    g_store_write_fails = false;
    CHECK(g_store_writes == writes + 1 && g_store_consumed == 3 * E,
          "... and the stored ledger keeps the last window's (larger) spend");
    CHECK(dp::restore_budget(53, g_now) && dp::consumed_budget_x1000() == 3 * E,
          "so a reboot then errs toward spent, not fresh");
  }

  // ── Millis wrap (49.7 days): the window is an unsigned difference. ──────
  {
    g_now = UINT32_MAX - 1000;
    CHECK(dp::restore_budget(53, g_now), "a restore just before the clock wraps");
    CHECK(!dp::refill_if_due(53, 2000) && dp::consumed_budget_x1000() == 3 * E,
          "3 s later, past the wrap: not a window yet");
    g_now = g_now + dp::BUDGET_WINDOW_MS;   // wraps
    CHECK(dp::refill_if_due(53, g_now) && dp::remaining_budget_x1000() == B,
          "a whole window later, across the wrap: refilled");
  }

  // ── A release cannot cross a refill. ─────────────────────────────────────
  {
    dp::Release old_rel(2);
    CHECK(old_rel.ok(), "a release reserved in one window");
    (void)old_rel.u32(5, 1);
    CHECK(next_window(54), "the window turns (another task's update())");
    CHECK(old_rel.u32(123456, 1) == 0, "cannot draw after the refill");
    CHECK(!old_rel.complete(), "and is not complete, so its export is withheld");
    dp::Release fresh(4);
    CHECK(fresh.ok() && dp::remaining_budget_x1000() == 0,
          "the new window's budget is its own, and exactly the budget");
  }
  {
    CHECK(next_window(55), "next window");
    dp::Release rel(1);
    (void)rel.u32(5, 1);
    CHECK(rel.complete(), "a release whose draws all landed is complete ...");
    CHECK(next_window(56), "next window");
    CHECK(!rel.complete(), "... until the window it was paid from is refilled");
  }
  {
    CHECK(next_window(57), "next window");
    dp::Release rel(1);
    CHECK(dp::restore_budget(57, g_now) && !rel.complete() && rel.u32(1, 1) == 0,
          "a restore is a new generation too");
  }
  CHECK(next_window(58), "next window");

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
  CHECK(next_window(59), "next window");
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
    CHECK(g_store_epoch == 59 && g_store_consumed == B, "and the persisted ledger says so");
  }

  // ── Noise scale: the calibrated σ, not a token amount. ───────────────────
  {
    const uint32_t sigma_x1000 = dp::compute_sigma_x1000(1, E, dp::DEFAULT_DELTA_INV);
    CHECK(sigma_x1000 == 4846, "σ at sensitivity 1, ε 1, δ 1e-5 is 4.846");
    double sum = 0, sum_sq = 0;
    const int N = 4000;
    for (int i = 0; i < N; i++) {
      (void)next_window(60);
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

  // ── The firmware's wiring (source pins; rf_presence.cpp needs NVS, BLE
  //    and mbedTLS, so no host test links it). ────────────────────────────
  {
    using namespace beacon_source_scan;
    bool ok = false;
    const std::string rf = strip_comments(read_source(RF_PRESENCE_CPP, &ok));
    CHECK(ok && !rf.empty(), "rf_presence.cpp read (the pins fail closed without it)");
    const std::string rotate = squeeze(function_body(rf, "rotate_session"));
    CHECK(!rotate.empty() && rotate.find("dp::") == std::string::npos,
          "rotate_session() (the manual rotation too) never touches the DP budget");
    CHECK(count(squeeze(rf), "dp::refill_if_due(") == 1 &&
          squeeze(function_body(rf, "update")).find("dp::refill_if_due(s_session_epoch,now);")
              != std::string::npos,
          "the one refill is update()'s, on the uptime clock");
    CHECK(count(squeeze(rf), "dp::restore_budget(") == 1 &&
          squeeze(function_body(rf, "init")).find("dp::restore_budget(s_session_epoch,s_session_start_ms);")
              != std::string::npos,
          "init() restores the ledger and starts the window at boot");
    const std::string conf = squeeze(function_body(rf, "conformance_check_token_rotation"));
    CHECK(!conf.empty() && conf.find("dp::") == std::string::npos,
          "the conformance rotation check does not touch the budget either");

    const std::string api = strip_comments(read_source(RF_PRESENCE_API_H, &ok));
    CHECK(ok && !api.empty(), "rf_presence_api.h read");
    const std::string get = squeeze(function_body(api, "handle_rf_conformance"));
    CHECK(get.find("boolskip_rotation=true;") != std::string::npos,
          "GET /api/rf/conformance skips the rotating check by default");
    CHECK(get.find("skip_rotation=!(strcmp(param_val,\"false\")==0||strcmp(param_val,\"0\")==0);")
              != std::string::npos,
          "and runs it only on an explicit skip_rotation=false / 0");
  }

  if (g_fail == 0) {
    printf("ALL dp budget tests PASSED\n");
    return 0;
  }
  printf("%d FAILED\n", g_fail);
  return 1;
}
