// Host-side unit tests for the three-tier health summary
// (arduino/canary_wap/status_tier_logic.h). Pure logic, no Arduino glue.
//
// What's pinned here:
//   - A healthy device reads "good"/"ok".
//   - Every single fault lands in its tier with its own reason code.
//   - Worst-first ranking: signing beats verify beats safe mode beats the
//     card beats a crash restart beats low memory beats unacked notes.
//   - A missing card is not a fault; an erroring one is.
//   - The low-memory row needs a floor and a real reading (0 = unknown).
//   - Every reason code the header can emit is one the dashboard's
//     COPY.tier table knows (the list below mirrors it).
//
// Build/run via tests_host/Makefile. Exits non-zero on any failure.

#include <cstdio>
#include <cstring>

#include "../arduino/canary_wap/status_tier_logic.h"

using namespace status_tier_logic;

static int g_failures = 0;

#define CHECK(cond, msg)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("FAIL %s:%d: %s\n", __func__, __LINE__, msg);       \
      ++g_failures;                                                   \
    }                                                                 \
  } while (0)

static Inputs healthy() {
  Inputs in;
  in.crypto_healthy = true;
  in.verify_failures = 0;
  in.safe_mode = false;
  in.sd_card_erroring = false;
  in.last_reset_crash = false;
  in.min_free_heap = 120000;
  in.low_heap_floor = 20000;
  in.logs_unacked = 0;
  return in;
}

static bool is(const Verdict& v, Tier t, const char* tier, const char* reason) {
  return v.tier == t && std::strcmp(v.tier_code, tier) == 0 &&
         std::strcmp(v.reason_code, reason) == 0;
}

static void healthy_is_good() {
  CHECK(is(evaluate(healthy()), Tier::GOOD, "good", "ok"), "healthy → good/ok");
}

static void each_fault_alone() {
  Inputs in = healthy(); in.crypto_healthy = false;
  CHECK(is(evaluate(in), Tier::ACTION_REQUIRED, "action_required", "signing"), "signing");
  in = healthy(); in.verify_failures = 1;
  CHECK(is(evaluate(in), Tier::ACTION_REQUIRED, "action_required", "verify"), "verify");
  in = healthy(); in.safe_mode = true;
  CHECK(is(evaluate(in), Tier::ACTION_REQUIRED, "action_required", "safe_mode"), "safe mode");
  in = healthy(); in.sd_card_erroring = true;
  CHECK(is(evaluate(in), Tier::NEEDS_ATTENTION, "needs_attention", "sd_card"), "card");
  in = healthy(); in.last_reset_crash = true;
  CHECK(is(evaluate(in), Tier::NEEDS_ATTENTION, "needs_attention", "restarted"), "crash restart");
  in = healthy(); in.min_free_heap = 19999;
  CHECK(is(evaluate(in), Tier::NEEDS_ATTENTION, "needs_attention", "low_memory"), "low memory");
  in = healthy(); in.logs_unacked = 3;
  CHECK(is(evaluate(in), Tier::NEEDS_ATTENTION, "needs_attention", "notes"), "notes");
}

static void worst_first_ranking() {
  Inputs in = healthy();
  in.crypto_healthy = false; in.verify_failures = 2; in.safe_mode = true;
  in.sd_card_erroring = true; in.last_reset_crash = true; in.min_free_heap = 1;
  in.logs_unacked = 9;
  CHECK(std::strcmp(evaluate(in).reason_code, "signing") == 0, "signing first");
  in.crypto_healthy = true;
  CHECK(std::strcmp(evaluate(in).reason_code, "verify") == 0, "then verify");
  in.verify_failures = 0;
  CHECK(std::strcmp(evaluate(in).reason_code, "safe_mode") == 0, "then safe mode");
  in.safe_mode = false;
  CHECK(std::strcmp(evaluate(in).reason_code, "sd_card") == 0, "then the card");
  in.sd_card_erroring = false;
  CHECK(std::strcmp(evaluate(in).reason_code, "restarted") == 0, "then a crash restart");
  in.last_reset_crash = false;
  CHECK(std::strcmp(evaluate(in).reason_code, "low_memory") == 0, "then low memory");
  in.min_free_heap = 120000;
  CHECK(std::strcmp(evaluate(in).reason_code, "notes") == 0, "then notes");
  in.logs_unacked = 0;
  CHECK(std::strcmp(evaluate(in).reason_code, "ok") == 0, "then ok");
}

static void low_memory_needs_a_floor_and_a_reading() {
  Inputs in = healthy();
  in.min_free_heap = 0;  // never sampled
  CHECK(evaluate(in).tier == Tier::GOOD, "an unknown reading is not low memory");
  in = healthy(); in.low_heap_floor = 0; in.min_free_heap = 1;
  CHECK(evaluate(in).tier == Tier::GOOD, "no floor configured → no low-memory row");
  in = healthy(); in.min_free_heap = in.low_heap_floor;
  CHECK(evaluate(in).tier == Tier::GOOD, "exactly at the floor is fine");
}

static void every_reason_is_one_the_dashboard_knows() {
  // Mirrors COPY.tier in csi_dashboard_html.h. A new reason here without a
  // COPY row would render as the tier label alone — add both together.
  static const char* const kKnown[] = {
    "ok", "signing", "verify", "safe_mode", "sd_card", "restarted", "low_memory", "notes",
  };
  Inputs cases[8];
  for (Inputs& c : cases) c = healthy();
  cases[1].crypto_healthy = false;
  cases[2].verify_failures = 1;
  cases[3].safe_mode = true;
  cases[4].sd_card_erroring = true;
  cases[5].last_reset_crash = true;
  cases[6].min_free_heap = 1;
  cases[7].logs_unacked = 1;
  for (const Inputs& c : cases) {
    const char* r = evaluate(c).reason_code;
    bool found = false;
    for (const char* k : kKnown) found = found || std::strcmp(k, r) == 0;
    CHECK(found, r);
  }
}

int main() {
  healthy_is_good();
  each_fault_alone();
  worst_first_ranking();
  low_memory_needs_a_floor_and_a_reading();
  every_reason_is_one_the_dashboard_knows();
  if (g_failures) {
    std::printf("test_status_tier_logic: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_status_tier_logic: all checks passed\n");
  return 0;
}
