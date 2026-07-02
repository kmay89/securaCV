/*
 * SecuraCV Canary WAP — Bluetooth channel factory defaults (host-testable)
 *
 * Arduino-free constants. The BLE pairing channel carries the offline
 * console, WiFi-over-BLE provisioning, OTA, and log/witness export — the
 * features a user expects to reach right after flashing. Shipping the
 * radio DISABLED by default meant "the Bluetooth panel does nothing until
 * you first toggle it on", which read as broken at the demo. So the radio
 * is on out of the box.
 *
 * On is not open: pairing still requires the user to allow it AND confirm
 * a numeric-comparison PIN (require_pin), so an advertised-but-unpaired
 * device exposes nothing without an explicit on-device human step. The
 * GATT characteristics themselves keep READ_ENC/READ_AUTHEN. Enabling the
 * radio by default does not weaken the pairing gate.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_BT_DEFAULTS_H
#define SECURACV_BT_DEFAULTS_H

namespace bt_defaults {

// The radio advertises out of the box (was false — the "dead panel" cause).
constexpr bool ENABLED = true;
// Advertise automatically once enabled.
constexpr bool AUTO_ADVERTISE = true;
// A peer may REQUEST pairing…
constexpr bool ALLOW_PAIRING = true;
// …but must clear a numeric-comparison PIN — the actual access gate, kept
// on so "enabled by default" never means "pairs silently".
constexpr bool REQUIRE_PIN = true;
// Long-range (coded PHY) is an explicit opt-in.
constexpr bool LONG_RANGE = false;

}  // namespace bt_defaults

#endif  // SECURACV_BT_DEFAULTS_H
