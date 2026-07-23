// Host test for the pure mode registry (include/canary/mode/mode_registry.h).
//
// Builds standalone with g++ — no Arduino, no board. Run in CI by the
// "mode registry host test" step in .github/workflows/firmware.yml. Prints
// "ALL MODE REGISTRY TESTS PASSED" on success (the CI grep makes a silent
// pass impossible to fake). Build (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//   firmware/projects/canary-display/tests_host/test_mode_registry.cpp -o t && ./t

#include "canary/mode/mode_registry.h"

#include <cstdio>

using canary::mode::BuildCaps;
using canary::mode::Mode;
using canary::mode::mode_allowed;
using canary::mode::mode_from_token;
using canary::mode::mode_policy;
using canary::mode::mode_token;
using canary::mode::resolve_boot_mode;

static int g_fail = 0;

#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                      \
    }                                                                \
  } while (0)

static const Mode ALL[] = {Mode::Fleet, Mode::Bench, Mode::Demo, Mode::Debug,
                           Mode::Arcade};

// ── Token contract ──────────────────────────────────────────────────────────
static void test_token_roundtrip() {
  for (Mode m : ALL) {
    CHECK(mode_from_token(mode_token(m)) == m, "token round-trips");
  }
}

static void test_unknown_token_is_fleet() {
  CHECK(mode_from_token(nullptr) == Mode::Fleet, "null -> Fleet");
  CHECK(mode_from_token("") == Mode::Fleet, "empty -> Fleet");
  CHECK(mode_from_token("kiosk") == Mode::Fleet, "from-the-future -> Fleet");
  CHECK(mode_from_token("BENCH") == Mode::Fleet, "case-mismatch -> Fleet");
  CHECK(mode_from_token("bench ") == Mode::Fleet, "trailing junk -> Fleet");
}

// ── Capability gate ─────────────────────────────────────────────────────────
static void test_fleet_always_allowed() {
  BuildCaps none;  // a build carrying nothing extra
  CHECK(mode_allowed(none, Mode::Fleet), "Fleet allowed on the barest build");
  CHECK(!mode_allowed(none, Mode::Bench), "no bench without a bench flag");
  CHECK(!mode_allowed(none, Mode::Demo), "no demo without FEATURE_DEMO_MODE");
  CHECK(!mode_allowed(none, Mode::Debug), "no debug without FEATURE_DEBUG_MODE");
  CHECK(!mode_allowed(none, Mode::Arcade), "no arcade without FEATURE_ARCADE");
}

static void test_bench_via_either_flag() {
  BuildCaps bench_env;
  bench_env.dedicated_bench = true;
  CHECK(mode_allowed(bench_env, Mode::Bench), "dedicated bench env allows Bench");
  BuildCaps devmode;
  devmode.has_devmode = true;
  CHECK(mode_allowed(devmode, Mode::Bench), "FEATURE_DEVMODE allows Bench");
}

// ── Boot resolution ─────────────────────────────────────────────────────────
static void test_dedicated_bench_ignores_latches() {
  BuildCaps caps;
  caps.dedicated_bench = true;
  CHECK(resolve_boot_mode(caps, "demo", false) == Mode::Bench,
        "bench env: stored token ignored");
  CHECK(resolve_boot_mode(caps, nullptr, false) == Mode::Bench,
        "bench env: no latch needed");
  CHECK(resolve_boot_mode(caps, "fleet", true) == Mode::Bench,
        "bench env: legacy bool ignored too");
}

static void test_stored_token_wins_when_carried() {
  BuildCaps caps;
  caps.has_devmode = true;
  caps.has_demo = true;
  CHECK(resolve_boot_mode(caps, "demo", false) == Mode::Demo,
        "stored demo boots demo");
  CHECK(resolve_boot_mode(caps, "bench", false) == Mode::Bench,
        "stored bench boots the bench");
  CHECK(resolve_boot_mode(caps, nullptr, false) == Mode::Fleet,
        "no latch: the product face");
}

static void test_uncarried_mode_fails_safe_to_fleet() {
  BuildCaps caps;  // nothing extra compiled in
  CHECK(resolve_boot_mode(caps, "demo", false) == Mode::Fleet,
        "downgrade: stored demo on a demo-less build -> Fleet");
  CHECK(resolve_boot_mode(caps, "garbage#", false) == Mode::Fleet,
        "corrupt token -> Fleet");
  BuildCaps demo_only;
  demo_only.has_demo = true;
  CHECK(resolve_boot_mode(demo_only, "arcade", false) == Mode::Fleet,
        "mode from a richer sibling build -> Fleet here");
}

static void test_legacy_devmode_migration() {
  BuildCaps caps;
  caps.has_devmode = true;
  caps.has_demo = true;
  CHECK(resolve_boot_mode(caps, nullptr, true) == Mode::Bench,
        "legacy devmode=true, no token -> Bench (the unit asked for the bench)");
  CHECK(resolve_boot_mode(caps, "", true) == Mode::Bench,
        "legacy devmode=true, empty token -> Bench");
  CHECK(resolve_boot_mode(caps, "demo", true) == Mode::Demo,
        "a written token is the newer intent: it outranks the legacy bool");
  CHECK(resolve_boot_mode(caps, "fleet", true) == Mode::Fleet,
        "explicit fleet token parks the legacy bool");
  BuildCaps no_bench;  // devmode flag absent: the legacy latch has no bench to enter
  CHECK(resolve_boot_mode(no_bench, nullptr, true) == Mode::Fleet,
        "legacy bool without the bench compiled in -> Fleet (fail-safe)");
}

// ── Policy invariants ───────────────────────────────────────────────────────
static void test_only_fleet_gets_ota() {
  for (Mode m : ALL) {
    const auto p = mode_policy(m);
    if (m == Mode::Fleet) {
      CHECK(p.ota && p.network, "Fleet: full stack");
    } else {
      CHECK(!p.ota, "non-fleet gear never takes an update");
    }
  }
}

static void test_every_nonfleet_mode_has_exit_and_banner() {
  for (Mode m : ALL) {
    const auto p = mode_policy(m);
    if (m == Mode::Fleet) {
      CHECK(!p.touch_exit && !p.banner, "Fleet is home: no exit, no banner");
    } else {
      CHECK(p.touch_exit, "non-fleet gear exits by long-press (no roach motel)");
      CHECK(p.banner, "non-fleet gear wears its banner (no impersonation)");
    }
  }
}

static void test_network_policy() {
  CHECK(!mode_policy(Mode::Bench).network, "bench is network-silent");
  CHECK(!mode_policy(Mode::Demo).network, "demo is network-silent");
  CHECK(!mode_policy(Mode::Arcade).network, "arcade is network-silent");
  CHECK(mode_policy(Mode::Debug).network,
        "debug keeps the network up — the link is the patient");
  for (Mode m : ALL) {
    const auto p = mode_policy(m);
    CHECK(p.watchdog == (m == Mode::Fleet),
          "watchdog arms only under the product face");
  }
}

int main() {
  test_token_roundtrip();
  test_unknown_token_is_fleet();
  test_fleet_always_allowed();
  test_bench_via_either_flag();
  test_dedicated_bench_ignores_latches();
  test_stored_token_wins_when_carried();
  test_uncarried_mode_fails_safe_to_fleet();
  test_legacy_devmode_migration();
  test_only_fleet_gets_ota();
  test_every_nonfleet_mode_has_exit_and_banner();
  test_network_policy();

  if (g_fail == 0) {
    std::printf("ALL MODE REGISTRY TESTS PASSED\n");
    return 0;
  }
  std::printf("%d MODE REGISTRY TEST(S) FAILED\n", g_fail);
  return 1;
}
