/**
 * @file ble_debug_beacon.cpp
 * @brief BLE debug beacon implementation
 */

#include "ble_debug_beacon.h"
#include "bluetooth_mgr.h"
#include "../core/log.h"
#include <string.h>
#include <stdio.h>

static const char* LOG_TAG = "BLE_DBG";

// ============================================================================
// STATE
// ============================================================================

static bool s_active = false;
static char s_debug_name[32];
static uint8_t s_mesh_peers = 0;
static uint8_t s_rf_devices = 0;
static uint16_t s_chain_height = 0;
static uint8_t s_chain_verify = 0xFF;  // pending
static ble_debug_error_t s_error = BLE_DEBUG_ERR_NONE;

// ============================================================================
// HELPERS
// ============================================================================

static uint8_t compute_checksum(const ble_debug_payload_t* p) {
    const uint8_t* bytes = (const uint8_t*)p;
    uint8_t xor_val = 0;
    // XOR bytes 2 through 18 (skip company_id[0-1], skip checksum[19])
    for (int i = 2; i < 19; i++) {
        xor_val ^= bytes[i];
    }
    return xor_val;
}

static void put_be16(uint8_t* dst, uint16_t val) {
    dst[0] = (uint8_t)(val >> 8);
    dst[1] = (uint8_t)(val & 0xFF);
}

static void put_be32(uint8_t* dst, uint32_t val) {
    dst[0] = (uint8_t)(val >> 24);
    dst[1] = (uint8_t)(val >> 16);
    dst[2] = (uint8_t)(val >> 8);
    dst[3] = (uint8_t)(val & 0xFF);
}

// ============================================================================
// API
// ============================================================================

result_t ble_debug_beacon_init(const char* fingerprint_hex) {
    if (s_active) {
        return RESULT_OK;
    }

    snprintf(s_debug_name, sizeof(s_debug_name), "SCV-DBG-%s",
             fingerprint_hex ? fingerprint_hex : "0000");

    // Update the advertised device name so scanners see SCV-DBG-XXXX
    ble_mgr_set_device_name(s_debug_name);

    s_active = true;
    s_error = BLE_DEBUG_ERR_NONE;
    s_chain_verify = 0xFF;

    LOG_I("Debug beacon active: %s", s_debug_name);
    return RESULT_OK;
}

result_t ble_debug_beacon_deinit(void) {
    if (!s_active) {
        return RESULT_OK;
    }

    s_active = false;
    LOG_I("Debug beacon deactivated");
    return RESULT_OK;
}

void ble_debug_beacon_build_payload(ble_debug_payload_t* out,
                                     const system_health_t* health) {
    if (!out || !health) {
        return;
    }
    memset(out, 0, sizeof(*out));

    // Company ID (little-endian 0xFFFF)
    out->company_id[0] = 0xFF;
    out->company_id[1] = 0xFF;

    out->magic = BLE_DEBUG_MAGIC;
    out->version = BLE_DEBUG_VERSION;

    // Subsystem flags
    uint8_t flags = 0;
    if (health->wifi_active)    flags |= BLE_DEBUG_FLAG_WIFI;
    if (health->ble_active)     flags |= BLE_DEBUG_FLAG_BLE;
    if (health->mesh_active)    flags |= BLE_DEBUG_FLAG_MESH;
    if (health->chirp_active)   flags |= BLE_DEBUG_FLAG_CHIRP;
    if (health->gps_healthy)    flags |= BLE_DEBUG_FLAG_GPS;
    if (health->sd_healthy)     flags |= BLE_DEBUG_FLAG_SD;
    if (health->crypto_healthy) flags |= BLE_DEBUG_FLAG_CRYPTO;
    out->subsystem_flags = flags;

    out->mesh_peers = s_mesh_peers;
    out->rf_device_count = s_rf_devices;

    uint16_t heap_kb = (uint16_t)(health->free_heap / 1024);
    put_be16(out->free_heap_kb, heap_kb);

    put_be32(out->uptime_sec, health->uptime_sec);

    put_be16(out->chain_height, s_chain_height);
    out->chain_verify = s_chain_verify;

    out->reserved[0] = 0;
    out->reserved[1] = 0;
    out->error_code = (uint8_t)s_error;

    out->checksum = compute_checksum(out);
}

result_t ble_debug_beacon_update(const system_health_t* health) {
    if (!s_active) {
        return RESULT_NOT_INITIALIZED;
    }

    // Update chain height from witness records
    s_chain_height = (uint16_t)(health->records_created & 0xFFFF);

    // Detect error conditions
    if (health->verify_failures > 0) {
        s_error = BLE_DEBUG_ERR_CHAIN_VERIFY;
        s_chain_verify = 1;
    } else if (!health->crypto_healthy) {
        s_error = BLE_DEBUG_ERR_CRYPTO_FAIL;
    } else if (!health->sd_healthy) {
        s_error = BLE_DEBUG_ERR_SD_FAIL;
    } else if (health->free_heap < 8192) {
        s_error = BLE_DEBUG_ERR_HEAP_LOW;
    } else {
        s_error = BLE_DEBUG_ERR_NONE;
        if (health->records_verified > 0) {
            s_chain_verify = 0;
        }
    }

    ble_debug_payload_t payload;
    ble_debug_beacon_build_payload(&payload, health);

    // Update manufacturer-specific advertising data visible to passive scanners
    ble_mgr_set_manufacturer_data((const uint8_t*)&payload, sizeof(payload));

    return RESULT_OK;
}

void ble_debug_beacon_set_error(ble_debug_error_t err) {
    s_error = err;
}

void ble_debug_beacon_set_mesh_peers(uint8_t count) {
    s_mesh_peers = count;
}

void ble_debug_beacon_set_rf_devices(uint8_t count) {
    s_rf_devices = count;
}

void ble_debug_beacon_set_chain_height(uint16_t height) {
    s_chain_height = height;
}

void ble_debug_beacon_set_chain_verify(uint8_t status) {
    s_chain_verify = status;
}

bool ble_debug_beacon_is_active(void) {
    return s_active;
}
