// Host tests for common/storage/sd_mount_policy.h — the pure decision table
// around the blocking SD mount/teardown work. Pins the two hardware-paid
// lessons: never pile onto an in-flight mount (watchdog crash-loop), and
// never rebuild or free driver state while USB MSC holds the card
// (use-after-free handed to the USB host). Wrap-safe time math is asserted
// at the uint32_t boundary.
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cassert>
#include <cstdio>

#include "../common/storage/sd_mount_policy.h"

using sd_mount_policy::PeriodicAction;
using sd_mount_policy::declare_lost;
using sd_mount_policy::may_teardown;
using sd_mount_policy::mount_wait_expired;
using sd_mount_policy::periodic_action;
using sd_mount_policy::sd_state_for_tamper;

static int g_checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                         \
    }                                                                   \
    ++g_checks;                                                         \
  } while (0)

int main() {
  const uint32_t kInterval = 30000;

  // Interval gate: nothing happens before the recheck interval elapses,
  // whatever the card state.
  CHECK(periodic_action(false, false, false, 1000, 0, kInterval) ==
        PeriodicAction::NONE);
  CHECK(periodic_action(true, false, false, 29999, 0, kInterval) ==
        PeriodicAction::NONE);

  // At and past the interval: mounted → VERIFY, absent → REMOUNT.
  CHECK(periodic_action(true, false, false, 30000, 0, kInterval) ==
        PeriodicAction::VERIFY);
  CHECK(periodic_action(false, false, false, 30001, 0, kInterval) ==
        PeriodicAction::REMOUNT);

  // In-flight mount blocks everything, even long past the interval.
  CHECK(periodic_action(false, true, false, 500000, 0, kInterval) ==
        PeriodicAction::NONE);
  CHECK(periodic_action(true, true, false, 500000, 0, kInterval) ==
        PeriodicAction::NONE);

  // MSC holds the card: no REMOUNT ever (driver state must not be rebuilt
  // under the USB host), but a mounted card may still be verified.
  CHECK(periodic_action(false, false, true, 500000, 0, kInterval) ==
        PeriodicAction::NONE);
  CHECK(periodic_action(true, false, true, 500000, 0, kInterval) ==
        PeriodicAction::VERIFY);

  // Wrap-safe interval math: last_check near UINT32_MAX, now wrapped.
  const uint32_t near_wrap = 0xFFFFF000u;
  CHECK(periodic_action(false, false, false, near_wrap + 100, near_wrap,
                        kInterval) == PeriodicAction::NONE);
  CHECK(periodic_action(false, false, false, near_wrap + kInterval,
                        near_wrap, kInterval) == PeriodicAction::REMOUNT);

  // Teardown guard: only when no mount is in flight AND MSC does not hold
  // the card.
  CHECK(may_teardown(false, false));
  CHECK(!may_teardown(true, false));
  CHECK(!may_teardown(false, true));
  CHECK(!may_teardown(true, true));

  // Consecutive-error threshold: crossing declares the card lost; a zero
  // threshold never does (config-typo guard).
  CHECK(!declare_lost(0, 2));
  CHECK(!declare_lost(1, 2));
  CHECK(declare_lost(2, 2));
  CHECK(declare_lost(7, 2));
  CHECK(!declare_lost(1000, 0));

  // Bounded wait expiry, wrap-safe.
  CHECK(!mount_wait_expired(1000, 0, 4000));
  CHECK(mount_wait_expired(4000, 0, 4000));
  CHECK(!mount_wait_expired(near_wrap + 100, near_wrap, 4000));
  CHECK(mount_wait_expired(near_wrap + 4000, near_wrap, 4000));

  // The tamper watcher's sd_state. The numbers are pinned: they are
  // tamper_events_module.cpp's ABSENT/MOUNTED/ERROR (and the canary-wap
  // SdState), so a renumbering here would turn a pulled card into a
  // failing one on the wire.
  CHECK(sd_mount_policy::SD_TAMPER_ABSENT == 0);
  CHECK(sd_mount_policy::SD_TAMPER_MOUNTED == 1);
  CHECK(sd_mount_policy::SD_TAMPER_ERROR == 2);
  // Mounted wins, whatever the latch says (a successful remount clears it;
  // a stale latch must never narrate a working card as failing).
  CHECK(sd_state_for_tamper(true, false) == sd_mount_policy::SD_TAMPER_MOUNTED);
  CHECK(sd_state_for_tamper(true, true) == sd_mount_policy::SD_TAMPER_MOUNTED);
  // Given up on after consecutive write failures = ERROR (sd_error).
  CHECK(sd_state_for_tamper(false, true) == sd_mount_policy::SD_TAMPER_ERROR);
  // Probe failed / never mounted / remount pending = ABSENT (sd_remove).
  CHECK(sd_state_for_tamper(false, false) == sd_mount_policy::SD_TAMPER_ABSENT);

  std::printf("test_sd_mount_policy: %d checks passed\n", g_checks);
  return 0;
}
