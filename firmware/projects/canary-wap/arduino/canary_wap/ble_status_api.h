/*
 * SecuraCV Canary — BLE GATT Status Service (WAP port)
 *
 * Header-only port of PIO's securacv_ble_status. Exposes device status
 * over BLE GATT so a companion phone can read battery, health, chain
 * info, and degradation state without WiFi.
 *
 * Two GATT services are registered on the shared NimBLE server:
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
 * Characteristic values are refreshed by ble_status::update() which
 * pulls from the witness, sys_monitor, and hardware_state modules.
 * Rate-limited to once every 5 seconds to avoid starving the main loop.
 *
 * IMPORTANT: WAP shares a single NimBLE server across all BLE services.
 * This module does NOT call NimBLEDevice::init() or createServer().
 * Instead, init() accepts the existing server from bluetooth_channel.
 *
 * UUIDs match PIO's securacv_ble_status for cross-platform compatibility.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_BLE_STATUS_API_H
#define SECURACV_BLE_STATUS_API_H

#include "build_config.h"

#if FEATURE_BLE_STATUS

#include <NimBLEDevice.h>
#include "sys_monitor.h"
#include "hardware_state.h"
#if FEATURE_POWER_MONITOR
#include "power_monitor.h"
#endif

// ════════════════════════════════════════════════════════════════════════════
// BLE SERVICE UUIDs — must match PIO's securacv_ble_status.h
// ════════════════════════════════════════════════════════════════════════════

// Standard Battery Service
#define BLE_STATUS_BATTERY_SERVICE_UUID    "180F"
#define BLE_STATUS_BATTERY_LEVEL_CHAR_UUID "2A19"

// Custom SecuraCV Status Service
// Generated: 5e63a1b0-7c3d-4f2e-8a91-0d1b2c3e4f5a
// Characteristic UUIDs are offset from the service base.
#define BLE_STATUS_SCV_SERVICE_UUID        "5e63a1b0-7c3d-4f2e-8a91-0d1b2c3e4f5a"
#define BLE_STATUS_SCV_DEVICE_NAME_UUID    "5e63a1b1-7c3d-4f2e-8a91-0d1b2c3e4f5a"
#define BLE_STATUS_SCV_FW_VERSION_UUID     "5e63a1b2-7c3d-4f2e-8a91-0d1b2c3e4f5a"
#define BLE_STATUS_SCV_CHAIN_SEQ_UUID      "5e63a1b3-7c3d-4f2e-8a91-0d1b2c3e4f5a"
#define BLE_STATUS_SCV_HEALTH_SCORE_UUID   "5e63a1b4-7c3d-4f2e-8a91-0d1b2c3e4f5a"
#define BLE_STATUS_SCV_DEGRADE_LEVEL_UUID  "5e63a1b5-7c3d-4f2e-8a91-0d1b2c3e4f5a"
#define BLE_STATUS_SCV_UPTIME_UUID         "5e63a1b6-7c3d-4f2e-8a91-0d1b2c3e4f5a"
#define BLE_STATUS_SCV_SD_USAGE_UUID       "5e63a1b7-7c3d-4f2e-8a91-0d1b2c3e4f5a"

// ════════════════════════════════════════════════════════════════════════════
// TIMING
// ════════════════════════════════════════════════════════════════════════════

#define BLE_STATUS_UPDATE_INTERVAL_MS 5000

namespace ble_status {

// ════════════════════════════════════════════════════════════════════════════
// STATE
// ════════════════════════════════════════════════════════════════════════════

static NimBLECharacteristic* g_batt_level_char = nullptr;
static NimBLECharacteristic* g_dev_name_char   = nullptr;
static NimBLECharacteristic* g_fw_ver_char     = nullptr;
static NimBLECharacteristic* g_chain_seq_char  = nullptr;
static NimBLECharacteristic* g_health_char     = nullptr;
static NimBLECharacteristic* g_degrade_char    = nullptr;
static NimBLECharacteristic* g_uptime_char     = nullptr;
static NimBLECharacteristic* g_sd_usage_char   = nullptr;

static bool     g_initialized    = false;
static bool     g_connected      = false;
static uint32_t g_last_update_ms = 0;

// External state references (set during init, owned by caller)
static const char*  g_device_name     = nullptr;
static const char*  g_fw_version      = nullptr;
static uint32_t*    g_chain_seq_ptr   = nullptr;
static uint8_t*     g_health_score_ptr = nullptr;   // optional, may be nullptr
static uint8_t*     g_degrade_lvl_ptr  = nullptr;   // optional, may be nullptr

// ════════════════════════════════════════════════════════════════════════════
// CONNECTION TRACKING
// ════════════════════════════════════════════════════════════════════════════
//
// We do NOT install our own NimBLEServerCallbacks — the bluetooth_channel
// layer already owns the server callbacks (NimBLE supports only one
// callbacks object per server). Instead, we track connection state by
// querying the server directly in is_connected().

static NimBLEServer* g_server = nullptr;

// ════════════════════════════════════════════════════════════════════════════
// INIT
// ════════════════════════════════════════════════════════════════════════════

// Initialize the BLE GATT status service on the shared NimBLE server.
//
// Parameters:
//   server       — existing NimBLE server (from bluetooth_channel)
//   device_name  — device display name (caller-owned, must outlive)
//   fw_version   — firmware version string (caller-owned, must outlive)
//   chain_seq    — pointer to chain sequence counter (read each update)
//   health_score — optional pointer to health score [0-100] (nullptr OK)
//   degrade_lvl  — optional pointer to degradation level (nullptr OK)
//
// Returns true on success.
static bool init(NimBLEServer* server,
                 const char* device_name,
                 const char* fw_version,
                 uint32_t* chain_seq,
                 uint8_t* health_score = nullptr,
                 uint8_t* degrade_lvl  = nullptr) {
  if (g_initialized) return true;
  if (!server) return false;

  g_server          = server;
  g_device_name     = device_name;
  g_fw_version      = fw_version;
  g_chain_seq_ptr   = chain_seq;
  g_health_score_ptr = health_score;
  g_degrade_lvl_ptr  = degrade_lvl;

  // ── Standard Battery Service (0x180F) ──────────────────────────────
  NimBLEService* batt_svc = server->createService(BLE_STATUS_BATTERY_SERVICE_UUID);
  if (!batt_svc) {
    Serial.println("[BLE-Status] Battery service creation failed");
    return false;
  }

  g_batt_level_char = batt_svc->createCharacteristic(
      BLE_STATUS_BATTERY_LEVEL_CHAR_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  {
    uint8_t initial_batt = 0;
    g_batt_level_char->setValue(&initial_batt, 1);
  }
  batt_svc->start();

  // ── Custom SecuraCV Status Service ─────────────────────────────────
  NimBLEService* scv_svc = server->createService(BLE_STATUS_SCV_SERVICE_UUID);
  if (!scv_svc) {
    Serial.println("[BLE-Status] SecuraCV status service creation failed");
    return false;
  }

  g_dev_name_char = scv_svc->createCharacteristic(
      BLE_STATUS_SCV_DEVICE_NAME_UUID, NIMBLE_PROPERTY::READ);
  g_dev_name_char->setValue(device_name ? device_name : "SecuraCV");

  g_fw_ver_char = scv_svc->createCharacteristic(
      BLE_STATUS_SCV_FW_VERSION_UUID, NIMBLE_PROPERTY::READ);
  g_fw_ver_char->setValue(fw_version ? fw_version : "0.0.0");

  g_chain_seq_char = scv_svc->createCharacteristic(
      BLE_STATUS_SCV_CHAIN_SEQ_UUID, NIMBLE_PROPERTY::READ);
  {
    uint32_t seq = chain_seq ? *chain_seq : 0;
    g_chain_seq_char->setValue(seq);
  }

  g_health_char = scv_svc->createCharacteristic(
      BLE_STATUS_SCV_HEALTH_SCORE_UUID, NIMBLE_PROPERTY::READ);
  {
    uint8_t score = health_score ? *health_score : 0;
    g_health_char->setValue(&score, 1);
  }

  g_degrade_char = scv_svc->createCharacteristic(
      BLE_STATUS_SCV_DEGRADE_LEVEL_UUID, NIMBLE_PROPERTY::READ);
  {
    uint8_t dl = degrade_lvl ? *degrade_lvl : 0;
    g_degrade_char->setValue(&dl, 1);
  }

  g_uptime_char = scv_svc->createCharacteristic(
      BLE_STATUS_SCV_UPTIME_UUID, NIMBLE_PROPERTY::READ);
  {
    uint32_t up = 0;
    g_uptime_char->setValue(up);
  }

  g_sd_usage_char = scv_svc->createCharacteristic(
      BLE_STATUS_SCV_SD_USAGE_UUID, NIMBLE_PROPERTY::READ);
  {
    uint8_t sd = 0;
    g_sd_usage_char->setValue(&sd, 1);
  }

  scv_svc->start();

  // NOTE: We do NOT touch advertising here. The bluetooth_channel /
  // ble_manager layer owns advertising configuration. Adding our service
  // UUIDs to the advertisement packet would exceed the 31-byte payload
  // limit and is unnecessary — phones discover services via GATT after
  // connecting.

  g_initialized = true;
  g_last_update_ms = millis();

  Serial.printf("[BLE-Status] GATT status service started (device: %s)\n",
                device_name ? device_name : "SecuraCV");
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// UPDATE
// ════════════════════════════════════════════════════════════════════════════

// Refresh characteristic values from sys_monitor, hardware_state, and
// caller-provided pointers. Call from loop(). Internally rate-limited
// to BLE_STATUS_UPDATE_INTERVAL_MS — cheap to call every iteration.
static void update() {
  if (!g_initialized) return;

  uint32_t now = millis();
  if ((now - g_last_update_ms) < BLE_STATUS_UPDATE_INTERVAL_MS) return;
  g_last_update_ms = now;

  bool connected = g_server && (g_server->getConnectedCount() > 0);
  g_connected = connected;

  // ── Battery level ──────────────────────────────────────────────────
#if FEATURE_POWER_MONITOR
  {
    PowerState pwr;
    if (power_monitor::get_state(&pwr)) {
      uint8_t soc = pwr.soc_pct;
      if (soc > 100) soc = 100;
      g_battery_char->setValue(&soc, 1);
      if (connected) g_battery_char->notify();
    }
  }
#endif

  // ── Chain sequence ─────────────────────────────────────────────────
  if (g_chain_seq_ptr) {
    uint32_t seq = *g_chain_seq_ptr;
    g_chain_seq_char->setValue(seq);
  }

  // ── Health score ───────────────────────────────────────────────────
  if (g_health_score_ptr) {
    uint8_t score = *g_health_score_ptr;
    if (score > 100) score = 100;
    g_health_char->setValue(&score, 1);
  }

  // ── Degradation level ──────────────────────────────────────────────
  if (g_degrade_lvl_ptr) {
    uint8_t dl = *g_degrade_lvl_ptr;
    g_degrade_char->setValue(&dl, 1);
  }

  // ── Uptime ─────────────────────────────────────────────────────────
  // Pull from sys_monitor's global metrics (uptime_sec is updated by
  // sys_monitor::update() which runs in the main loop).
  {
    uint32_t up = g_sys_metrics.uptime_sec;
    g_uptime_char->setValue(up);
  }

  // ── SD usage percent ───────────────────────────────────────────────
  // Derive from hardware_state's cached SD size/free values.
  {
    uint8_t sd_pct = 0;
    if (g_hw.sd_available && g_hw.sd_total_bytes > 0) {
      uint64_t used = g_hw.sd_total_bytes - g_hw.sd_free_bytes;
      sd_pct = (uint8_t)((used * 100ULL) / g_hw.sd_total_bytes);
      if (sd_pct > 100) sd_pct = 100;
    }
    g_sd_usage_char->setValue(&sd_pct, 1);
  }

  // ── Battery notify (only when connected) ───────────────────────────
  // If/when a battery sensor is integrated, notify here:
  // if (connected) { g_batt_level_char->notify(); }
}

// ════════════════════════════════════════════════════════════════════════════
// QUERY
// ════════════════════════════════════════════════════════════════════════════

// Returns true if a BLE central (phone) is currently connected.
// Queries the server directly rather than relying on callbacks (the
// bluetooth_channel layer owns the server callbacks).
static bool is_connected() {
  if (!g_initialized || !g_server) return false;
  return g_server->getConnectedCount() > 0;
}

} // namespace ble_status

#else // !FEATURE_BLE_STATUS

// No-op stubs when BLE status is disabled at compile time.
namespace ble_status {
  static inline bool init(void*, const char*, const char*, uint32_t*,
                          uint8_t* = nullptr, uint8_t* = nullptr) { return false; }
  static inline void update() {}
  static inline bool is_connected() { return false; }
}

#endif // FEATURE_BLE_STATUS

#endif // SECURACV_BLE_STATUS_API_H
