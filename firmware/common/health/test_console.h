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

// ════════════════════════════════════════════════════════════════════════════
// 3. Chain attestation ('c') — a signing command that ISN'T a signing oracle
// ════════════════════════════════════════════════════════════════════════════
//
// The 'c' command answers "prove to me, standing at the port, that this device's
// witness log hasn't been rewritten": it signs a challenger-supplied nonce
// together with the current chain head under the DEVICE key, so anyone can
// verify the result offline against the pinned public key. That is genuinely
// powerful — and the reason a naive version would be dangerous: a raw
// "sign these bytes" command is a signing oracle an attacker could use to forge
// a chain entry or an OTA approval.
//
// The safety comes entirely from DOMAIN SEPARATION. This message is prefixed
// with a tag that appears in NO other signing context in the firmware (the
// witness chain signs compute_chain_hash() output under "securacv:fw:chain:v1";
// pairing/OTA use their own contexts). A signature produced here therefore can
// never be replayed as anything else. The prefix + layout live in this pure,
// host-tested builder so the separation can't silently regress in review.

static const char ATTEST_DOMAIN[] = "SECURACV-ATTEST-v1";

// Bytes attest_build_message() needs for a given nonce length:
//   ATTEST_DOMAIN || nonce || chain_head[32] || seq(LE32) || boot_count(LE32)
inline size_t attest_message_size(size_t nonce_len) {
  return (sizeof(ATTEST_DOMAIN) - 1) + nonce_len + 32 + 4 + 4;
}

// Build the exact byte string the device signs for a 'c' attestation. Returns
// the length written, or 0 if it doesn't fit in `cap` (never truncates — a
// short buffer must fail closed, not sign a partial/ambiguous message).
inline size_t attest_build_message(uint8_t* dst, size_t cap,
                                   const uint8_t* nonce, size_t nonce_len,
                                   const uint8_t chain_head[32],
                                   uint32_t seq, uint32_t boot_count) {
  const size_t need = attest_message_size(nonce_len);
  if (!dst || need > cap) return 0;
  if (nonce_len && !nonce) return 0;
  size_t o = 0;
  const size_t dlen = sizeof(ATTEST_DOMAIN) - 1;
  for (size_t i = 0; i < dlen; ++i)       dst[o++] = (uint8_t)ATTEST_DOMAIN[i];
  for (size_t i = 0; i < nonce_len; ++i)  dst[o++] = nonce[i];
  for (size_t i = 0; i < 32; ++i)         dst[o++] = chain_head[i];
  dst[o++] = (uint8_t)(seq         & 0xFF); dst[o++] = (uint8_t)((seq >> 8)  & 0xFF);
  dst[o++] = (uint8_t)((seq >> 16) & 0xFF); dst[o++] = (uint8_t)((seq >> 24) & 0xFF);
  dst[o++] = (uint8_t)(boot_count         & 0xFF); dst[o++] = (uint8_t)((boot_count >> 8)  & 0xFF);
  dst[o++] = (uint8_t)((boot_count >> 16) & 0xFF); dst[o++] = (uint8_t)((boot_count >> 24) & 0xFF);
  return o;
}

// ════════════════════════════════════════════════════════════════════════════
// 4. Boot / provenance labels (shared, host-tested)
// ════════════════════════════════════════════════════════════════════════════

// Firmware-internal reset cause. main.cpp maps ESP_RST_* onto this so the
// labelling is host-testable without pulling in esp_system.h.
enum class ResetReason : uint8_t {
  Unknown = 0, PowerOn, Software, Panic, IntWdt, TaskWdt, BrownOut, DeepSleep,
  Watchdog, Sdio, Other,
};

inline const char* reset_reason_label(ResetReason r) {
  switch (r) {
    case ResetReason::PowerOn:   return "power-on (cold boot)";
    case ResetReason::Software:  return "software restart";
    case ResetReason::Panic:     return "PANIC — firmware crash";
    case ResetReason::IntWdt:    return "interrupt watchdog";
    case ResetReason::TaskWdt:   return "task watchdog";
    case ResetReason::BrownOut:  return "BROWNOUT — supply dipped";
    case ResetReason::DeepSleep: return "woke from deep sleep";
    case ResetReason::Watchdog:  return "watchdog";
    case ResetReason::Sdio:      return "SDIO host reset";
    case ResetReason::Other:     return "other";
    case ResetReason::Unknown:   return "unknown";
  }
  return "unknown";
}

// True for reset causes that indicate an abnormal/fault reboot worth surfacing.
inline bool reset_reason_is_fault(ResetReason r) {
  return r == ResetReason::Panic || r == ResetReason::IntWdt ||
         r == ResetReason::TaskWdt || r == ResetReason::BrownOut ||
         r == ResetReason::Watchdog;
}

// Label for diagnostics degrade_level_t (0=none..3=emergency).
inline const char* degrade_level_label(uint8_t lvl) {
  switch (lvl) {
    case 0: return "none";
    case 1: return "warning";
    case 2: return "critical";
    case 3: return "emergency";
    default: return "?";
  }
}

}  // namespace testcon

#endif  // SECURACV_TEST_CONSOLE_H
