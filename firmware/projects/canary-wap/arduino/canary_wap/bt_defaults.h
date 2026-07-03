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

// The BLE controller's largest single internal allocation at init is ~30 KB
// (0x7800 — the size in the "BLE_INIT: Malloc failed / emi.c" panic). On a
// board with PSRAM DISABLED the internal heap is ~127 KB and, once the WiFi AP
// and HTTP server are up, no contiguous block that large remains — the malloc
// fails, the controller asserts, the interrupt watchdog fires, and the device
// boot-loops (and, because it aborts below the app, even safe mode can't
// recover). So require a comfortable contiguous headroom before bringing the
// stack up; below it, skip BLE and leave the radio off rather than brick.
// Enabling PSRAM (Arduino IDE: Tools > PSRAM > "OPI PSRAM") is the real fix —
// this only keeps a mis-configured build usable as an AP + dashboard.
constexpr unsigned long MIN_INIT_FREE_BLOCK = 48UL * 1024UL;

// The contiguous block is necessary but NOT sufficient. The full stack
// (controller + NimBLE host + the pairing channel's six GATT services +
// discovery subsystems) costs ~55-65 KB of internal RAM in total, and the
// rest of the system needs real operating margin AFTER that — the field
// lesson: a boot where BLE init succeeded but left the heap near-empty took
// down the HTTP server (socket ENOBUFS) and even the SoftAP's WPA2
// handshake, which is strictly worse than "no Bluetooth". Require enough
// TOTAL free internal memory that post-init steady state keeps a healthy
// floor: ~65 KB stack cost + ~30 KB margin.
constexpr unsigned long MIN_INIT_TOTAL_FREE = 96UL * 1024UL;

// True when there is enough internal memory to bring the BLE stack up
// without OOM-panicking (contiguous block for the controller) AND without
// starving everything that comes after (total free). Callers pass the
// largest free internal DMA-capable block and the total free internal
// memory; when false they MUST skip NimBLEDevice::init().
inline bool init_has_headroom(unsigned long largest_free_internal_block,
                              unsigned long total_free_internal) {
  return largest_free_internal_block >= MIN_INIT_FREE_BLOCK &&
         total_free_internal >= MIN_INIT_TOTAL_FREE;
}

}  // namespace bt_defaults

#endif  // SECURACV_BT_DEFAULTS_H
