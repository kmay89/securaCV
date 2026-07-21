/* Host tests for firmware/common/health/test_console.h — the security policy of
 * the serial test console and the BLE bring-up ladder (see
 * docs/design/test_console.md).
 *
 * These PROVE the properties that make a test-command interface over a physical
 * serial port safe: no command leaks secrets, production images expose only
 * read-only diagnostics, and anything that mutates state requires a physical
 * confirm. CI runs this so the guarantees can't silently regress.
 *
 * Build & run (CI: firmware host tests):
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/common \
 *       firmware/tests_host/test_test_console.cpp \
 *       -o /tmp/test_test_console && /tmp/test_test_console
 */

#include <cstdio>

#include "health/test_console.h"

using namespace testcon;

static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

// A representative command table shaped like the real console.
static const Command TABLE[] = {
  //  key  name              tier          mutates leak   confirm
  { 's', "status",          Tier::Diag,   false,  false, false },
  { 't', "run all tests",   Tier::Diag,   false,  false, false },
  { 'i', "identity (pub)",  Tier::Diag,   false,  false, false },
  { 'B', "BLE advertise",   Tier::Demo,   false,  false, false },
  { 'P', "camera peek",     Tier::Demo,   false,  false, false },
  { 'M', "mic beep",        Tier::Demo,   false,  false, false },
  { 'F', "factory reset",   Tier::Mutate, true,   false, true  },
  { 'W', "set wifi",        Tier::Mutate, true,   false, true  },
};
static const size_t N = sizeof(TABLE) / sizeof(TABLE[0]);

static void test_table_is_safe() {
  CHECK(table_is_safe(TABLE, N));
  // A table with a secret-leaking command is NOT safe.
  Command bad = { 'z', "dump privkey", Tier::Diag, false, true, false };
  CHECK(!table_is_safe(&bad, 1));
  // A mutating command with no confirm is NOT safe.
  Command bad2 = { 'z', "reset", Tier::Mutate, true, false, false };
  CHECK(!table_is_safe(&bad2, 1));
  // A "diagnostic" that mutates is NOT safe.
  Command bad3 = { 'z', "wipe", Tier::Diag, true, false, true };
  CHECK(!table_is_safe(&bad3, 1));
}

static void test_production_exposes_only_readonly_diag() {
  for (size_t i = 0; i < N; ++i) {
    bool prodAllowed = command_allowed(TABLE[i], /*production*/ true, /*confirmed*/ true);
    if (TABLE[i].tier == Tier::Diag) CHECK(prodAllowed);          // read-only diag survives in prod
    else CHECK(!prodAllowed);                                     // demo/mutate are NOT in production
    CHECK(available_in_production(TABLE[i]) == (TABLE[i].tier == Tier::Diag));
  }
}

static void test_mutating_needs_physical_confirm() {
  Command reset = { 'F', "factory reset", Tier::Mutate, true, false, true };
  // Dev build, but no confirm → refused.
  CHECK(!command_allowed(reset, /*production*/ false, /*confirmed*/ false));
  // Dev build + confirm → allowed.
  CHECK(command_allowed(reset, /*production*/ false, /*confirmed*/ true));
  // Even confirmed, it's never allowed in a production image.
  CHECK(!command_allowed(reset, /*production*/ true, /*confirmed*/ true));
}

static void test_secret_leak_never_allowed() {
  Command leaky = { 'z', "print privkey", Tier::Diag, false, true, false };
  CHECK(!command_allowed(leaky, false, true));   // not in dev
  CHECK(!command_allowed(leaky, true, true));     // not in prod
}

static void test_demo_dev_only() {
  Command peek = { 'P', "camera peek", Tier::Demo, false, false, false };
  CHECK(command_allowed(peek, /*production*/ false, false));   // dev: fine
  CHECK(!command_allowed(peek, /*production*/ true, true));    // prod: compiled out / refused
}

// ── BLE ladder ──────────────────────────────────────────────────────────────

static BleObs obs(bool built, bool stack, bool svc, bool adv, bool conn, bool exch) {
  BleObs o; o.compiled_in = built; o.stack_up = stack; o.service_up = svc;
  o.advertising = adv; o.connected = conn; o.exchanged = exch; return o;
}

static void test_ble_ladder_stages() {
  CHECK(ble_stage(obs(false, false, false, false, false, false)) == BleStage::NotBuilt);
  CHECK(ble_stage(obs(true,  false, false, false, false, false)) == BleStage::StackDown);
  CHECK(ble_stage(obs(true,  true,  false, false, false, false)) == BleStage::NoService);
  CHECK(ble_stage(obs(true,  true,  true,  true,  false, false)) == BleStage::Advertising);
  CHECK(ble_stage(obs(true,  true,  true,  true,  true,  false)) == BleStage::Connected);
  CHECK(ble_stage(obs(true,  true,  true,  true,  true,  true )) == BleStage::Exchanged);
  // Monotonic robustness: stack+service up but 'advertising' flag missing still
  // reads as at-least-advertising (the ladder trusts the lower rungs).
  CHECK(ble_stage(obs(true, true, true, false, false, false)) == BleStage::Advertising);
}

static void test_ble_hints_and_ok() {
  // Every stage yields a non-empty, actionable hint.
  BleStage stages[] = { BleStage::NotBuilt, BleStage::StackDown, BleStage::NoService,
                        BleStage::Advertising, BleStage::Connected, BleStage::Exchanged };
  for (BleStage s : stages) {
    CHECK(ble_hint(s)[0] != '\0');
    CHECK(ble_stage_label(s)[0] != '\0');
  }
  // The honest flash reason is surfaced for the "not built" case.
  const char* h = ble_hint(BleStage::NotBuilt);
  bool mentions_full = false;
  for (const char* p = h; *p; ++p) if (p[0] == 'f' && p[1] == 'u' && p[2] == 'l' && p[3] == 'l') mentions_full = true;
  CHECK(mentions_full);
  // "healthy resting" states: absent, or up-and-advertising/connected/verified.
  CHECK(ble_stage_ok(BleStage::NotBuilt));
  CHECK(ble_stage_ok(BleStage::Advertising));
  CHECK(!ble_stage_ok(BleStage::StackDown));
  CHECK(!ble_stage_ok(BleStage::NoService));
}

int main() {
  test_table_is_safe();
  test_production_exposes_only_readonly_diag();
  test_mutating_needs_physical_confirm();
  test_secret_leak_never_allowed();
  test_demo_dev_only();
  test_ble_ladder_stages();
  test_ble_hints_and_ok();

  if (g_failures == 0) { std::printf("PASS test_test_console (all assertions)\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
