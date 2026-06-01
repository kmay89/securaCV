/*
 * SecuraCV Canary — BLE GATT Status Service
 *
 * Exposes device status over BLE GATT so a companion phone can read
 * battery, health, chain info, and degradation state without WiFi.
 *
 * Two GATT services are advertised:
 *
 *   1. Standard Battery Service (0x180F)
 *      - Battery Level characteristic (0x2A19, read/notify)
 *
 *   2. Custom SecuraCV Status Service (128-bit UUID)
 *      - Device name (read-only, UTF-8)
 *      - Firmware version (read-only, UTF-8)
 *      - Chain sequence number (read-only, uint32)
 *      - Health score (read-only, uint8, 0-100%)
 *      - Degradation level (read-only, uint8, enum)
 *      - Uptime seconds (read-only, uint32)
 *      - SD usage percent (read-only, uint8)
 *
 * Characteristic values are refreshed by ble_status_update() which
 * pulls from the witness, diagnostics, and power modules. Rate-limited
 * to once every 5 seconds to avoid starving the main loop.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_BLE_STATUS_H
#define SECURACV_BLE_STATUS_H

#include "canary_config.h"

#if FEATURE_BLE_STATUS

#include <stdint.h>
#include <stdbool.h>

/* ────────────────────────────────────────────────────────────────────────── */
/*  BLE SERVICE UUIDs                                                        */
/* ────────────────────────────────────────────────────────────────────────── */

/* Standard Battery Service */
#define BLE_BATTERY_SERVICE_UUID        "180F"
#define BLE_BATTERY_LEVEL_CHAR_UUID     "2A19"

/* Custom SecuraCV Status Service
 * Generated: 5e63a1b0-7c3d-4f2e-8a91-0d1b2c3e4f5a
 * Characteristic UUIDs are offset from the service base. */
#define BLE_SCV_SERVICE_UUID            "5e63a1b0-7c3d-4f2e-8a91-0d1b2c3e4f5a"
#define BLE_SCV_DEVICE_NAME_UUID        "5e63a1b1-7c3d-4f2e-8a91-0d1b2c3e4f5a"
#define BLE_SCV_FW_VERSION_UUID         "5e63a1b2-7c3d-4f2e-8a91-0d1b2c3e4f5a"
#define BLE_SCV_CHAIN_SEQ_UUID          "5e63a1b3-7c3d-4f2e-8a91-0d1b2c3e4f5a"
#define BLE_SCV_HEALTH_SCORE_UUID       "5e63a1b4-7c3d-4f2e-8a91-0d1b2c3e4f5a"
#define BLE_SCV_DEGRADE_LEVEL_UUID      "5e63a1b5-7c3d-4f2e-8a91-0d1b2c3e4f5a"
#define BLE_SCV_UPTIME_UUID             "5e63a1b6-7c3d-4f2e-8a91-0d1b2c3e4f5a"
#define BLE_SCV_SD_USAGE_UUID           "5e63a1b7-7c3d-4f2e-8a91-0d1b2c3e4f5a"

/* ────────────────────────────────────────────────────────────────────────── */
/*  TIMING                                                                   */
/* ────────────────────────────────────────────────────────────────────────── */

#define BLE_STATUS_UPDATE_INTERVAL_MS   5000

/* ────────────────────────────────────────────────────────────────────────── */
/*  C API                                                                    */
/* ────────────────────────────────────────────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the NimBLE stack under this device's configured name + TX power,
 * if it isn't already initialized. This service is the SINGLE NimBLE init
 * owner for the modular canary build: it owns the GAP device name and TX
 * power. Call this EARLY in setup() — before any other BLE consumer (notably
 * the RX-only BLE Scout, which is registered inside securacv_csi_modules_init()
 * and attaches to whatever stack is already running) — so the device always
 * advertises under its configured name rather than the Scout's generic
 * "securacv-scout". Idempotent; ble_status_init() also calls it. Returns true
 * if the stack is up (NimBLE 2.x init() can fail when the radio/host stack
 * can't come up). */
bool ble_status_stack_begin(void);

/* Initialize BLE stack, create GATT services and characteristics,
 * begin advertising. Call once from setup() after witness and
 * diagnostics are initialized. Returns true on success. */
bool ble_status_init(void);

/* Refresh characteristic values from witness, diagnostics, and power
 * modules. Call from loop(). Internally rate-limited — cheap to call
 * every iteration. */
void ble_status_update(void);

/* Returns true if a BLE central (phone) is currently connected. */
bool ble_status_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* FEATURE_BLE_STATUS */

#endif /* SECURACV_BLE_STATUS_H */
