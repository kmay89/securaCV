/*
 * SecuraCV — pure SD mount-recovery decisions (host-testable)
 *
 * Arduino-free: stdint only. The blocking SD work (SD.begin / SD.end / the
 * mount worker task) lives in the consuming tree; every branchy DECISION
 * around it lives here so a host g++ run (tests_host/test_sd_mount_policy.cpp)
 * can pin it. Wrap-safe time math throughout (unsigned subtraction).
 *
 * Two lessons this table encodes, both paid for on real hardware:
 *
 * 1. SD.begin() can block for many seconds (absent or wedged card — the SPI
 *    driver's waits retry internally with no overall deadline). Run it on the
 *    watchdog-subscribed loop task and a bad card blows the task watchdog and
 *    crash-loops the device. So the policy never asks for a mount while one
 *    is already in flight, and consumers run the blocking call on a dedicated
 *    idle-priority worker (see canary-wap's sd_mount_logic.h, where this
 *    lesson was first written down — that sketch keeps its own local table
 *    for now; unifying it onto this header is a follow-up, not done here).
 *
 * 2. SD.end() frees driver state. USB MSC exposes the card to a host by raw
 *    sector reads from the TinyUSB task (securacv_usb_onboard msc_read_cb),
 *    so tearing the driver down while MSC holds the card is a use-after-free
 *    handed to whatever the USB host is reading. While MSC holds the card the
 *    policy refuses every teardown and every remount (a remount would hand
 *    the host a filesystem that changed identity mid-session).
 *
 * Consumed by firmware/canary's securacv_storage (the loop-task-only storage
 * manager). This file replaced an unbuilt scaffold header (storage.h) that
 * declared a remount nothing implemented — the seventh of its kind after the
 * six the 2026-09 audit removed.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_SD_MOUNT_POLICY_H
#define SECURACV_SD_MOUNT_POLICY_H

#include <stdint.h>

namespace sd_mount_policy {

// What should the loop's periodic SD check do right now?
enum class PeriodicAction : uint8_t {
  NONE    = 0,  // nothing (interval not elapsed / attempt in flight / MSC holds the card)
  VERIFY  = 1,  // card is mounted — cheap presence check
  REMOUNT = 2,  // card absent — ask the mount worker for a bounded attempt
};

// Decision table for the loop()'s periodic SD check:
// - Never act before the recheck interval has elapsed (wrap-safe).
// - Never act while a previous mount attempt is still in flight — the worker
//   may be inside a blocking SD.begin(); piling on requests is useless and
//   the driver state is unknown until it returns.
// - Never REMOUNT while USB MSC holds the card: SD.begin() would rebuild
//   driver state under the host's raw-sector session. A mounted card may
//   still be VERIFYed under MSC (verify is a read), but its failure path is
//   gated by may_teardown() below.
inline PeriodicAction periodic_action(bool mounted, bool mount_in_flight,
                                      bool msc_holds_card,
                                      uint32_t now_ms, uint32_t last_check_ms,
                                      uint32_t interval_ms) {
  if ((uint32_t)(now_ms - last_check_ms) < interval_ms) return PeriodicAction::NONE;
  if (mount_in_flight) return PeriodicAction::NONE;
  if (!mounted && msc_holds_card) return PeriodicAction::NONE;
  return mounted ? PeriodicAction::VERIFY : PeriodicAction::REMOUNT;
}

// May the loop task call SD.end() right now? Never under a running mount
// attempt (the worker owns the driver state) and never while USB MSC holds
// the card (the USB host is reading raw sectors from that state).
inline bool may_teardown(bool mount_in_flight, bool msc_holds_card) {
  return !mount_in_flight && !msc_holds_card;
}

// Has a run of consecutive write failures crossed the give-up threshold?
// At the threshold the consumer stops attempting writes (marks the card
// lost) and lets the periodic check tear down and remount. A threshold of
// zero never declares the card lost — refusing to let a config typo turn
// one bad sector into a permanent unmount.
inline bool declare_lost(uint32_t consecutive_errors, uint32_t threshold) {
  return threshold > 0 && consecutive_errors >= threshold;
}

// Has the caller's bounded wait for the mount worker expired? The caller
// polls the worker in short slices, feeding the task watchdog each slice;
// past the budget it reports the card absent and moves on (the worker keeps
// running until the blocking call returns; the result is adopted by a later
// periodic pass instead of being discarded). Wrap-safe.
inline bool mount_wait_expired(uint32_t now_ms, uint32_t started_ms,
                               uint32_t budget_ms) {
  return (uint32_t)(now_ms - started_ms) >= budget_ms;
}

}  // namespace sd_mount_policy

#endif  // SECURACV_SD_MOUNT_POLICY_H
