/*
 * SecuraCV — test-console policy + BLE bring-up ladder (host-testable, no Arduino).
 *
 * Two pure pieces of the "run tests over the serial console" feature
 * (docs/design/test_console.md):
 *
 *   1. The SECURITY POLICY. A serial console = physical access, so the console
 *      must never let physical access alone break the device's guarantees:
 *      no command may leak secret material, forge/disable the witness chain, or
 *      silently mutate state. Commands are declared with a Tier + flags, and one
 *      pure function (command_allowed) decides what may run in which build. The
 *      whole table is auditable by table_is_safe() — host-tested, so CI proves
 *      the invariants rather than trusting review.
 *
 *   2. The BLE BRING-UP LADDER. "Bluetooth doesn't work" is really several
 *      distinct failures; ble_stage() collapses a handful of observations into
 *      the first rung that failed, and ble_hint() says what to do about it —
 *      including the honest flash/RAM reasons BLE is [env:full]-only.
 *
 * This file must compile hosted for tests_host/test_test_console.cpp with
 * -Wall -Wextra -Werror.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_TEST_CONSOLE_H
#define SECURACV_TEST_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

namespace testcon {

// ════════════════════════════════════════════════════════════════════════════
// 1. Security tiers + command policy
// ════════════════════════════════════════════════════════════════════════════

enum class Tier : uint8_t {
  Diag = 0,  // read-only diagnostics. Safe in production. Never mutates, never
             // prints secret material (public keys are fine).
  Demo,      // benign, non-destructive self-tests/demos (camera peek, mic beep,
             // BLE advertise-and-wait). Dev/test builds only; in-memory; auto-
             // revert; suppressed from Home Assistant so a test can't trip a
             // real automation.
  Mutate,    // changes device/security state (pairing, config, factory reset).
             // Dev builds only AND requires a deliberate physical confirm.
};

// A console command, declared so the policy is auditable rather than implicit.
struct Command {
  char        key;           // the serial key that triggers it
  const char* name;
  Tier        tier;
  bool        mutates;       // changes persistent or security-relevant state
  bool        leaks_secret;  // MUST be false for every command (asserted)
  bool        needs_confirm; // requires the physical BOOT-button confirm to act
};

// THE policy. May this command run, given whether this is a production image and
// whether a physical confirm was captured? This one function is the whole model.
inline bool command_allowed(const Command& c, bool production_build, bool confirmed) {
  if (c.leaks_secret) return false;                            // invariant: never, anywhere
  if (production_build && c.tier != Tier::Diag) return false;  // production = read-only diagnostics only
  if (c.mutates && !confirmed) return false;                   // mutating always needs the physical confirm
  return true;
}

// Audit the whole command table for the invariants a human reviewer would check.
// Returns true only if EVERY command is safe by construction. Host-tested.
inline bool table_is_safe(const Command* cmds, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const Command& c = cmds[i];
    if (c.leaks_secret) return false;                       // 1. nothing ever leaks secrets
    if (c.tier == Tier::Diag && c.mutates) return false;    // 2. read-only tier never mutates
    if (c.mutates && !c.needs_confirm) return false;        // 3. mutation implies a confirm gate
    if (c.tier == Tier::Mutate && !c.needs_confirm) return false;
  }
  return true;
}

// Convenience: is a command exposed at all in a shipping/production image?
inline bool available_in_production(const Command& c) {
  return c.tier == Tier::Diag && !c.leaks_secret && !c.mutates;
}

// ════════════════════════════════════════════════════════════════════════════
// 2. BLE bring-up ladder  ("why isn't Bluetooth working?")
// ════════════════════════════════════════════════════════════════════════════

enum class BleStage : uint8_t {
  NotBuilt = 0,  // FEATURE_BLE_STATUS off — BLE is in the [env:full] build only
  StackDown,     // compiled in, but the NimBLE stack didn't come up (RAM headroom?)
  NoService,     // stack up, but the GATT status service isn't registered
  Advertising,   // advertising, waiting for a phone/central to connect
  Connected,     // a central connected
  Exchanged,     // a characteristic was read/notified — end-to-end proven
};

// What the firmware glue observes from the BLE lib (all cheap booleans). The
// ladder is strictly ordered: a higher rung implies the ones below it.
struct BleObs {
  bool compiled_in;  // FEATURE_BLE_STATUS
  bool stack_up;     // NimBLEDevice::isInitialized()
  bool service_up;   // GATT service registered (ble_status_init succeeded)
  bool advertising;  // advertiser running
  bool connected;    // a central is connected (ble_status_is_connected)
  bool exchanged;    // a characteristic was read or notified during the test
};

// Collapse the observations into the FIRST rung that failed (or the highest
// reached). Robust to inconsistent inputs (treats the ladder as monotonic).
inline BleStage ble_stage(const BleObs& o) {
  if (!o.compiled_in) return BleStage::NotBuilt;
  if (!o.stack_up)    return BleStage::StackDown;
  if (!o.service_up)  return BleStage::NoService;
  if (o.exchanged)    return BleStage::Exchanged;
  if (o.connected)    return BleStage::Connected;
  return BleStage::Advertising;  // stack + service up ⇒ at least advertising
}

inline const char* ble_stage_label(BleStage s) {
  switch (s) {
    case BleStage::NotBuilt:    return "not compiled in";
    case BleStage::StackDown:   return "stack down";
    case BleStage::NoService:   return "no GATT service";
    case BleStage::Advertising: return "advertising";
    case BleStage::Connected:   return "connected";
    case BleStage::Exchanged:   return "verified";
  }
  return "?";
}

// The payoff for the "I'm struggling with Bluetooth" case: what to actually do.
inline const char* ble_hint(BleStage s) {
  switch (s) {
    case BleStage::NotBuilt:
      return "BLE is compiled out. It ships only in the [env:full] build — the "
             "NimBLE image (~2.7 MB) doesn't fit the dev/release OTA A/B slots.";
    case BleStage::StackDown:
      return "NimBLE init failed. The controller needs ~96 KB free internal RAM "
             "(a ~30 KB contiguous block PSRAM can't host). Free RAM before the "
             "BLE gate, or the build is too loaded to bring the radio up.";
    case BleStage::NoService:
      return "Stack up but the GATT status service didn't register — check "
             "ble_status_init().";
    case BleStage::Advertising:
      return "Advertising. Connect a phone (nRF Connect or any BLE scanner) to "
             "the device to confirm the link end-to-end.";
    case BleStage::Connected:
      return "A central connected. Read the health/battery characteristic to "
             "finish the test.";
    case BleStage::Exchanged:
      return "BLE verified: advertised, connected, and a characteristic was "
             "exchanged.";
  }
  return "";
}

// A stage is a "healthy resting state" if BLE is either intentionally absent or
// fully up and advertising (the normal idle) — used to decide pass/warn/fail.
inline bool ble_stage_ok(BleStage s) {
  return s == BleStage::NotBuilt || s == BleStage::Advertising ||
         s == BleStage::Connected || s == BleStage::Exchanged;
}

}  // namespace testcon

#endif  // SECURACV_TEST_CONSOLE_H
