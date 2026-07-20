/*
 * SecuraCV Canary — USB onboarding (firmware glue).
 *
 * The "plug me in" experience (docs/design/usb_onboard.md). On a USB-OTG build
 * (ARDUINO_USB_MODE=0) the Canary enumerates as a composite device:
 *
 *   ├─ CDC : the serial console (unchanged)
 *   ├─ HID : a keyboard whose ONLY job is to open the owner's help page
 *   │        (https://securacv.com/canary) after a physical confirmation
 *   └─ MSC : the SD card, READ-ONLY, so the witness files are just… files
 *
 * The trust model — why a self-typing keyboard here is not a BadUSB — lives in
 * the pure logic (common/usb/usb_onboard_logic.h) and is enforced by this glue:
 * enumerating types nothing; the keyboard emits only after the owner asks on
 * the console (which ANNOUNCES the exact URL) and then presses the physical
 * BOOT button; the payload is a compile-time-fixed, run-time allow-listed help
 * URL and nothing else; it is one-shot and defaults OFF.
 *
 * Requires FEATURE_USB_ONBOARD=1 AND a TinyUSB (USB-OTG) build. The stock
 * canary profile builds hwcdc (ARDUINO_USB_MODE=1), so this ships as the
 * separate opt-in [env:usb-onboard]. On every other build the functions below
 * still link but report "not in this build".
 *
 * PHASE 2 (hardware validation) PENDING: the HID/MSC glue compiles only in the
 * flagged build and has not yet enumerated on real hosts. Do not enable in a
 * release profile before the on-device checklist in the design doc runs. Mirror
 * of the sibling usb_evidence_drive precedent.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_USB_ONBOARD_H
#define SECURACV_USB_ONBOARD_H

#include <stddef.h>
#include <stdint.h>

#include "usb/usb_onboard_logic.h"

namespace usb_onboard {

struct Config {
  const char* help_url_base;  // SECURACV_HELP_URL_BASE, e.g. https://securacv.com/canary
  const char* device_id;      // witness device id, appended as ?d=
  bool        expose_msc;     // also present the SD read-only over USB (navigate files)
};

// One-time init. Brings up the composite USB device (HID always; MSC when
// cfg.expose_msc && SD is mounted). Safe to call on a non-OTG build — it just
// records that onboarding is unavailable and returns false.
bool begin(const Config& cfg);

// Main-loop pump. Raises TIMEOUT when the arming window elapses and services
// deferred USB (mount/unmount) events. Cheap when idle. No-op if !begin().
void poll();

// Console 'u': ask to open the help page. Prints the ANNOUNCEMENT (the exact
// URL + "press BOOT to open") and arms the confirm window. Never types here.
void request_launch();

// Physical BOOT-button confirmation (short press). The ONLY call that can make
// the keyboard type, and only while ARMED. Emits the keystroke plan once.
void confirm();

// Owner backed out (any other console key while armed) — re-lock quietly.
void cancel();

// Pick how the keyboard opens the browser (console). MANUAL (default) types the
// URL as text and lets the person press Enter; the OS methods add a launcher
// hotkey. Persisted only for this session.
void set_method(LaunchMethod method);
LaunchMethod method();

// Notify of a USB detach (cable removed) so state re-locks.
void on_unplug();

// Current consent state + a one-line human status for the console banner.
State state();
void print_status();

// Guided walkthroughs printed to the serial console. These SURFACE existing
// firmware/tooling behaviour (they do not perform destructive actions):
//   recovery — the witness-chain "SD-wins" reconciliation (witness_recover_from_sd)
//              and how to verify the log off-device.
//   unseal   — how sealed evidence is opened on a computer with the operator's
//              private key (tools/unseal_snapshot.py); the device cannot decrypt.
void print_recovery_guide();
void print_unseal_guide();

}  // namespace usb_onboard

#endif  // SECURACV_USB_ONBOARD_H
