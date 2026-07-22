/* Host tests for firmware/common/health/boot_policy.h — the crash-loop /
 * safe-mode boot decision (docs/design/self_star_roadmap.md, TODO 2).
 *
 * These PROVE the recovery behaviour that keeps a bad firmware image from
 * bricking trust, so it can be locked in CI rather than trusted to a review of
 * boot-path glue:
 *   · a genuinely good image never trips safe mode (it reaches healthy and
 *     resets the counter before the threshold);
 *   · an unconfirmed image NEVER enters safe mode — A/B rollback owns that
 *     recovery while an image is still PENDING_VERIFY;
 *   · a confirmed image that cannot come up degrades to safe mode after exactly
 *     N consecutive unhealthy boots, then stays there (stable fixed point);
 *   · safe mode is always escapable — any reset returns the next boot to
 *     Normal, so it is never a permanent brick;
 *   · the counter saturates instead of overflowing.
 *
 * Build & run (CI: "Build + run firmware/common host tests"):
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/common \
 *       firmware/tests_host/test_boot_policy.cpp \
 *       -o /tmp/test_boot_policy && /tmp/test_boot_policy
 */

#include <cstdio>
#include <cstdint>

#include "health/boot_policy.h"

using namespace bootpolicy;

static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

// ── First-ever boot (or the boot right after any reset) ────────────────────
static void test_first_boot() {
  // prev == 0 always proceeds normally and counts this boot, confirmed or not.
  Decision c = decide(0, /*image_confirmed=*/true);
  CHECK(c.persist_count == 1);
  CHECK(c.mode == BootMode::Normal);

  Decision u = decide(0, /*image_confirmed=*/false);
  CHECK(u.persist_count == 1);
  CHECK(u.mode == BootMode::Normal);
}

// ── A good image never accumulates toward the threshold ────────────────────
static void test_good_image_steady_state() {
  // Each boot increments to 1, the app reaches healthy and resets to 0, so the
  // count never climbs. Repeat many cycles; it must stay Normal at count 1.
  uint16_t persisted = 0;
  for (int i = 0; i < 1000; i++) {
    Decision d = decide(persisted, /*image_confirmed=*/true);
    CHECK(d.persist_count == 1);
    CHECK(d.mode == BootMode::Normal);
    persisted = kHealthyReset;  // app reached healthy -> reset
  }
  CHECK(persisted == 0);
}

// ── A confirmed crash-loop trips at exactly the threshold ──────────────────
static void test_confirmed_crashloop_trips_at_threshold() {
  // Never reaching healthy, the persisted count is fed straight back in.
  // Default threshold is 4: boots at counts 1,2,3 stay Normal; the 4th trips.
  const uint16_t T = kDefaultSafeModeThreshold;
  uint16_t persisted = 0;
  for (uint16_t boot = 1; boot < T; boot++) {
    Decision d = decide(persisted, /*image_confirmed=*/true);
    CHECK(d.persist_count == boot);
    CHECK(d.mode == BootMode::Normal);
    persisted = d.persist_count;
  }
  // The boot that reaches count == T is the one that falls back.
  Decision trip = decide(persisted, /*image_confirmed=*/true);
  CHECK(trip.persist_count == T);
  CHECK(trip.mode == BootMode::SafeMode);
}

// ── Threshold boundary holds for arbitrary thresholds ──────────────────────
static void test_threshold_boundary_generic() {
  const uint16_t thresholds[] = {3, 4, 8, 16};
  for (uint16_t T : thresholds) {
    // One below the threshold: still Normal.
    Decision below = decide(static_cast<uint16_t>(T - 2), true, T);
    CHECK(below.persist_count == T - 1);
    CHECK(below.mode == BootMode::Normal);
    // The boot that lands exactly on the threshold: SafeMode.
    Decision at = decide(static_cast<uint16_t>(T - 1), true, T);
    CHECK(at.persist_count == T);
    CHECK(at.mode == BootMode::SafeMode);
  }
}

// ── An unconfirmed image NEVER enters safe mode (A/B rollback owns it) ──────
static void test_unconfirmed_never_safe_mode() {
  const uint16_t T = kDefaultSafeModeThreshold;
  const uint16_t counts[] = {0, 1, static_cast<uint16_t>(T - 1), T,
                             static_cast<uint16_t>(T + 5), 100, kBootAttemptCap};
  for (uint16_t prev : counts) {
    Decision d = decide(prev, /*image_confirmed=*/false, T);
    CHECK(d.mode == BootMode::Normal);            // never safe while unconfirmed
    CHECK(d.persist_count == saturating_inc(prev)); // but the boot is still counted
    CHECK(!in_safe_mode(prev, /*image_confirmed=*/false, T));
  }
}

// ── The counter saturates instead of overflowing ───────────────────────────
static void test_saturation() {
  CHECK(saturating_inc(0) == 1);
  CHECK(saturating_inc(kBootAttemptCap - 1) == kBootAttemptCap);
  CHECK(saturating_inc(kBootAttemptCap) == kBootAttemptCap);
  CHECK(saturating_inc(65535) == kBootAttemptCap);  // never wraps

  Decision at_cap = decide(kBootAttemptCap, true);
  CHECK(at_cap.persist_count == kBootAttemptCap);   // pinned, no overflow
  CHECK(at_cap.mode == BootMode::SafeMode);          // cap > threshold, so safe
}

// ── Safe mode is a stable fixed point: once in, stay in, count pinned ───────
static void test_safe_mode_is_stable() {
  uint16_t persisted = 0;
  bool entered = false;
  // Confirmed image that never reaches healthy: drive many boots.
  for (int i = 0; i < 100; i++) {
    Decision d = decide(persisted, /*image_confirmed=*/true);
    persisted = d.persist_count;
    if (d.mode == BootMode::SafeMode) entered = true;
    if (entered) {
      // After entering, every subsequent boot stays SafeMode...
      CHECK(d.mode == BootMode::SafeMode);
    }
  }
  CHECK(entered);
  // ...and the count is pinned at the cap, not growing unbounded.
  CHECK(persisted == kBootAttemptCap);
  CHECK(decide(persisted, true).mode == BootMode::SafeMode);
}

// ── Safe mode is always escapable — never a permanent brick ────────────────
static void test_reset_escapes_safe_mode() {
  // All three reset aliases are the same "clean slate" value.
  CHECK(kHealthyReset == 0);
  CHECK(kFreshImageReset == 0);
  CHECK(kOperatorClearReset == 0);

  // Sitting deep in safe mode (count saturated, image confirmed)...
  const uint16_t stuck = kBootAttemptCap;
  CHECK(in_safe_mode(stuck, /*image_confirmed=*/true));

  // ...each recovery path resets the counter, and the next boot is Normal.
  const uint16_t resets[] = {kHealthyReset, kFreshImageReset, kOperatorClearReset};
  for (uint16_t r : resets) {
    Decision next = decide(r, /*image_confirmed=*/true);
    CHECK(next.persist_count == 1);
    CHECK(next.mode == BootMode::Normal);
  }
}

// ── decide() and in_safe_mode() agree across the whole input grid ──────────
static void test_decide_predicate_consistency() {
  const uint16_t thresholds[] = {3, 4, 8};
  for (uint16_t T : thresholds) {
    for (uint16_t prev = 0; prev <= kBootAttemptCap + 2; prev++) {
      for (int ci = 0; ci < 2; ci++) {
        const bool confirmed = (ci == 1);
        Decision d = decide(prev, confirmed, T);
        // The persisted (post-increment) count fed to in_safe_mode() must give
        // the same verdict decide() reached — the predicate is the mode read
        // back out of the counter without mutating it.
        const bool pred = in_safe_mode(d.persist_count, confirmed, T);
        CHECK((d.mode == BootMode::SafeMode) == pred);
      }
    }
  }
}

// ── The rollback-churn scenario: headroom keeps a good image from a false trip
static void test_rollback_churn_headroom() {
  // A confirmed good image is healthy (count 0). A new image is installed over
  // OTA -> the engine resets the counter for the newcomer.
  uint16_t persisted = kFreshImageReset;
  CHECK(persisted == 0);

  // The new (unconfirmed) image boots, is bad, and crashes before confirming.
  // It is counted but, being unconfirmed, cannot enter safe mode.
  Decision bad = decide(persisted, /*image_confirmed=*/false);
  CHECK(bad.mode == BootMode::Normal);
  persisted = bad.persist_count;  // now 1; bootloader will A/B-roll-back

  // The last-known-good CONFIRMED image boots next. At the decision point it has
  // not yet reached healthy, so it inherits the inflated count — but the default
  // threshold's headroom keeps it Normal, and then it reaches healthy and resets.
  Decision good = decide(persisted, /*image_confirmed=*/true);
  CHECK(good.persist_count == 2);
  CHECK(good.mode == BootMode::Normal);   // 2 < 4: no false safe-mode
  persisted = kHealthyReset;               // good image reached healthy
  CHECK(persisted == 0);

  // Why the >= 3 headroom matters: with a too-tight threshold of 2, that same
  // good confirmed image would have been sent to safe mode at count 2 before it
  // could prove itself. This is exactly what the static_assert in the header
  // forbids for the default, and what this test documents.
  CHECK(decide(1, /*image_confirmed=*/true, /*threshold=*/2).mode == BootMode::SafeMode);
}

// ── The shipped defaults are sane ──────────────────────────────────────────
static void test_default_constants() {
  CHECK(kDefaultSafeModeThreshold >= 3);
  CHECK(kBootAttemptCap > kDefaultSafeModeThreshold);
  CHECK(kHealthyReset == 0 && kFreshImageReset == 0 && kOperatorClearReset == 0);
}

int main() {
  test_first_boot();
  test_good_image_steady_state();
  test_confirmed_crashloop_trips_at_threshold();
  test_threshold_boundary_generic();
  test_unconfirmed_never_safe_mode();
  test_saturation();
  test_safe_mode_is_stable();
  test_reset_escapes_safe_mode();
  test_decide_predicate_consistency();
  test_rollback_churn_headroom();
  test_default_constants();

  if (g_failures) {
    std::printf("%d CHECK(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL boot-policy checks PASSED\n");
  return 0;
}
