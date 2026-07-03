/*
 * SecuraCV Canary WAP — pure SD-mount decisions (host-testable)
 *
 * Arduino-free: stdint only. The blocking SD work itself lives in
 * hardware_state.h (sd_mount_safe + the mount worker task); every branchy
 * DECISION around it lives here so a host g++ run (test_sd_mount_logic.cpp)
 * can pin it. Wrap-safe time math throughout (unsigned subtraction).
 *
 * Why this exists: SD.begin() can block for many seconds (absent or
 * unresponsive card — the SPI driver retries internally). It used to run
 * directly on the watchdog-subscribed loop task with only a millis() check
 * BETWEEN attempts, so the "mount timeout" never interrupted anything: a
 * slow/no card blew the 8 s task watchdog and crash-looped the device. The
 * loop()'s periodic remount repeated the same blocking call EVEN IN SAFE
 * MODE, so safe mode crash-looped too and could never stabilize.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_SD_MOUNT_LOGIC_H
#define SECURACV_SD_MOUNT_LOGIC_H

#include <stdint.h>

namespace sd_mount_logic {

// What should the periodic hardware check do about the SD card right now?
enum class PeriodicAction : uint8_t {
  NONE    = 0,  // nothing (interval not elapsed / safe mode / attempt in flight)
  VERIFY  = 1,  // card is mounted — cheap presence check + space-cache refresh
  REMOUNT = 2,  // card absent — ask the mount worker for a bounded attempt
};

// Decision table for the loop()'s periodic SD check:
// - Never act before the recheck interval has elapsed (wrap-safe).
// - Never act while a previous mount attempt is still in flight — the worker
//   may be stuck inside a blocking SD.begin(); piling on requests is useless
//   and the driver state is unknown until it returns.
// - Never attempt SD work in SAFE MODE: boot skipped SD init on purpose
//   ("optional peripherals disabled"), and the loop path must honor the same
//   contract. Before this gate, the safe-mode remount attempt re-ran the
//   blocking mount and crash-looped the one mode that exists to be stable.
inline PeriodicAction periodic_action(bool safe_mode, bool mounted,
                                      bool mount_in_flight,
                                      uint32_t now_ms, uint32_t last_check_ms,
                                      uint32_t interval_ms) {
  if ((uint32_t)(now_ms - last_check_ms) < interval_ms) return PeriodicAction::NONE;
  if (mount_in_flight) return PeriodicAction::NONE;
  if (safe_mode) return PeriodicAction::NONE;
  return mounted ? PeriodicAction::VERIFY : PeriodicAction::REMOUNT;
}

// Has the caller's bounded wait for the mount worker expired? The caller
// polls the worker in short slices, feeding the task watchdog each slice;
// past the budget it reports the card absent and moves on (the worker keeps
// running detached until the blocking call returns). Wrap-safe.
inline bool mount_wait_expired(uint32_t now_ms, uint32_t started_ms,
                               uint32_t budget_ms) {
  return (uint32_t)(now_ms - started_ms) >= budget_ms;
}

}  // namespace sd_mount_logic

#endif  // SECURACV_SD_MOUNT_LOGIC_H
