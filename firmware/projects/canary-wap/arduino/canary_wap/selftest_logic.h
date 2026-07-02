/*
 * SecuraCV Canary WAP — pure self-test decisions (host-testable)
 *
 * Arduino-free: stdint/bool only. The pre-flight health check reported
 * "almost everything failing" on real hardware for reasons that are pure
 * status logic, not measurement:
 *   - the Bluetooth probe hard-FAILed "NimBLE init failed" during the boot
 *     window before init ran, and PERMANENTLY in safe mode (where every
 *     radio init is skipped by design) — a wrong verdict that also flipped
 *     all_passed and gated the wizard;
 *   - it keyed on the discovery flag alone, so a DEV build with a live
 *     pairing channel reported "Not built into this firmware";
 *   - the summary line counted only pass/fail and dropped SKIP rows, so a
 *     device with several intentionally-inactive optional peripherals read
 *     as "5 of 5" when ten probes ran.
 *
 * The branchy decisions live here so a host g++ run pins them.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_SELFTEST_LOGIC_H
#define SECURACV_SELFTEST_LOGIC_H

#include <stdint.h>

namespace selftest_logic {

// Mirrors selftest::Status; kept separate so this header stays Arduino-free.
enum class Status : uint8_t { UNKNOWN = 0, PASS = 1, FAIL = 2, SKIP = 3, ABSENT = 4 };

// Bluetooth verdict.
//   compiled_in       — the radio is in this build (FEATURE_BLE || FEATURE_BLUETOOTH)
//   init_attempted    — the BLE init function actually ran (false in the
//                       boot window before it, and in safe mode)
//   available         — the stack reports itself up
//   safe_mode         — rapid-reboot recovery skipped every radio init
//   any_feature_active— at least one BLE feature (advertise/scan/chirp/
//                       pairing channel) is live
//
// Never FAIL for a state that isn't a real fault: not-built → ABSENT,
// safe mode / not-yet-initialized → SKIP. FAIL only when init ran and the
// stack genuinely did not come up.
inline Status bluetooth_status(bool compiled_in, bool init_attempted,
                               bool available, bool safe_mode,
                               bool any_feature_active) {
  if (!compiled_in) return Status::ABSENT;
  if (safe_mode) return Status::SKIP;
  if (!init_attempted) return Status::SKIP;
  if (!available) return Status::FAIL;
  return any_feature_active ? Status::PASS : Status::SKIP;
}

// Wi-Fi radio state, so the detail line stops saying "radio off" when the
// radio is on but the home link merely dropped.
enum class WifiKind : uint8_t { JOINED = 0, HOTSPOT = 1, LINK_DOWN = 2, RADIO_OFF = 3 };

inline WifiKind wifi_kind(bool sta_connected, bool ap_up, bool radio_off) {
  if (sta_connected) return WifiKind::JOINED;
  if (ap_up) return WifiKind::HOTSPOT;
  if (radio_off) return WifiKind::RADIO_OFF;
  return WifiKind::LINK_DOWN;
}

inline Status wifi_status(WifiKind k) {
  switch (k) {
    case WifiKind::JOINED:  return Status::PASS;
    case WifiKind::HOTSPOT: return Status::SKIP;   // wizard step 1-3
    default:                return Status::FAIL;   // link down / radio off
  }
}

// Only FAIL gates setup. SKIP and ABSENT are informational rows.
inline bool all_passed(uint8_t fail_count) { return fail_count == 0; }

}  // namespace selftest_logic

#endif  // SECURACV_SELFTEST_LOGIC_H
