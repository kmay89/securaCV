/**
 * @file securacv_protocol.h
 * @brief SecuraCV BLE debug beacon protocol parser for Flipper Zero
 *
 * Matches the wire format from firmware/common/bluetooth/ble_debug_beacon.h
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// ============================================================================
// PROTOCOL CONSTANTS
// ============================================================================

#define SCV_MAGIC           0x5C
#define SCV_VERSION         0x01
#define SCV_COMPANY_ID      0xFFFF
#define SCV_PAYLOAD_LEN     20

#define SCV_NAME_PREFIX_DBG "SCV-DBG-"
#define SCV_NAME_PREFIX_STD "SecuraCV"
#define SCV_NAME_PREFIX_SCV "SCV-"

// Subsystem flag bits
#define SCV_FLAG_WIFI       (1 << 0)
#define SCV_FLAG_BLE        (1 << 1)
#define SCV_FLAG_MESH       (1 << 2)
#define SCV_FLAG_CHIRP      (1 << 3)
#define SCV_FLAG_GPS        (1 << 4)
#define SCV_FLAG_SD         (1 << 5)
#define SCV_FLAG_CRYPTO     (1 << 6)
#define SCV_FLAG_TAMPER     (1 << 7)

// ============================================================================
// TYPES
// ============================================================================

typedef struct {
    uint8_t  subsystem_flags;
    uint8_t  mesh_peers;
    uint8_t  rf_device_count;
    uint16_t free_heap_kb;
    uint32_t uptime_sec;
    uint16_t chain_height;
    uint8_t  chain_verify;      // 0=ok, 1=fail, 0xFF=pending
    uint8_t  error_code;
    bool     valid;             // parsing succeeded
} scv_debug_beacon_t;

#define SCV_RSSI_HISTORY_LEN 8

typedef struct {
    char     name[32];
    int8_t   rssi;
    bool     is_debug_mode;
    bool     has_debug_data;
    scv_debug_beacon_t debug;
    uint32_t last_seen_ms;

    // Signal analysis
    int8_t   rssi_history[SCV_RSSI_HISTORY_LEN];
    uint8_t  rssi_history_idx;
    uint8_t  rssi_sample_count;
    int16_t  rssi_avg;          // x10 for one decimal place
    int8_t   rssi_min;
    int8_t   rssi_max;
} scv_device_t;

// ============================================================================
// ERROR CODE NAMES
// ============================================================================

static inline const char* scv_error_name(uint8_t code) {
    switch(code) {
        case 0x00: return "OK";
        case 0x01: return "CRYPTO";
        case 0x02: return "SD CARD";
        case 0x03: return "MESH";
        case 0x04: return "WIFI";
        case 0x05: return "LOW HEAP";
        case 0x06: return "CHAIN";
        case 0x07: return "WATCHDOG";
        default:   return "UNKNOWN";
    }
}

static inline const char* scv_verify_name(uint8_t status) {
    switch(status) {
        case 0x00: return "OK";
        case 0x01: return "FAIL";
        case 0xFF: return "PENDING";
        default:   return "?";
    }
}

// ============================================================================
// PARSING
// ============================================================================

static inline uint16_t scv_get_be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t scv_get_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/**
 * @brief Check if a device name belongs to a SecuraCV device
 */
static inline bool scv_is_securacv_device(const char* name) {
    if(!name) return false;
    if(strncmp(name, SCV_NAME_PREFIX_DBG, strlen(SCV_NAME_PREFIX_DBG)) == 0) return true;
    if(strncmp(name, SCV_NAME_PREFIX_STD, strlen(SCV_NAME_PREFIX_STD)) == 0) return true;
    if(strncmp(name, SCV_NAME_PREFIX_SCV, strlen(SCV_NAME_PREFIX_SCV)) == 0) return true;
    return false;
}

/**
 * @brief Check if a device is in debug mode (by name prefix)
 */
static inline bool scv_is_debug_mode(const char* name) {
    if(!name) return false;
    return strncmp(name, SCV_NAME_PREFIX_DBG, strlen(SCV_NAME_PREFIX_DBG)) == 0;
}

/**
 * @brief Parse manufacturer-specific data into debug beacon struct
 * @param data Raw manufacturer data (including company ID)
 * @param len Data length
 * @param out Parsed result
 * @return true if valid SecuraCV debug beacon
 */
static inline bool scv_parse_debug_beacon(const uint8_t* data, size_t len,
                                           scv_debug_beacon_t* out) {
    if(!out) return false;
    memset(out, 0, sizeof(*out));
    out->valid = false;

    if(!data || len < SCV_PAYLOAD_LEN) return false;

    // Check company ID (little-endian 0xFFFF)
    if(data[0] != 0xFF || data[1] != 0xFF) return false;

    // Check magic and version
    if(data[2] != SCV_MAGIC) return false;
    if(data[3] != SCV_VERSION) return false;

    // Verify checksum (XOR of bytes 2-18)
    uint8_t xor_val = 0;
    for(int i = 2; i < 19; i++) {
        xor_val ^= data[i];
    }
    if(xor_val != data[19]) return false;

    // Parse fields
    out->subsystem_flags = data[4];
    out->mesh_peers      = data[5];
    out->rf_device_count = data[6];
    out->free_heap_kb    = scv_get_be16(&data[7]);
    out->uptime_sec      = scv_get_be32(&data[9]);
    out->chain_height    = scv_get_be16(&data[13]);
    out->chain_verify    = data[15];
    out->error_code      = data[18];
    out->valid           = true;

    return true;
}

/**
 * @brief Format uptime into human-readable string
 * @param sec Uptime in seconds
 * @param buf Output buffer (at least 16 chars)
 * @param buf_len Buffer size
 */
static inline void scv_format_uptime(uint32_t sec, char* buf, size_t buf_len) {
    uint32_t d = sec / 86400;
    uint32_t h = (sec % 86400) / 3600;
    uint32_t m = (sec % 3600) / 60;
    uint32_t s = sec % 60;

    if(d > 0) {
        snprintf(buf, buf_len, "%lud %02lu:%02lu:%02lu", (unsigned long)d,
                 (unsigned long)h, (unsigned long)m, (unsigned long)s);
    } else {
        snprintf(buf, buf_len, "%02lu:%02lu:%02lu",
                 (unsigned long)h, (unsigned long)m, (unsigned long)s);
    }
}

// ============================================================================
// SIGNAL ANALYSIS
// ============================================================================

static inline void scv_rssi_push(scv_device_t* dev, int8_t rssi) {
    dev->rssi_history[dev->rssi_history_idx] = rssi;
    dev->rssi_history_idx = (dev->rssi_history_idx + 1) % SCV_RSSI_HISTORY_LEN;
    if(dev->rssi_sample_count < SCV_RSSI_HISTORY_LEN) {
        dev->rssi_sample_count++;
    }

    if(dev->rssi_sample_count == 1) {
        dev->rssi_min = rssi;
        dev->rssi_max = rssi;
    } else {
        if(rssi < dev->rssi_min) dev->rssi_min = rssi;
        if(rssi > dev->rssi_max) dev->rssi_max = rssi;
    }

    int32_t sum = 0;
    for(uint8_t i = 0; i < dev->rssi_sample_count; i++) {
        sum += dev->rssi_history[i];
    }
    dev->rssi_avg = (int16_t)((sum * 10) / dev->rssi_sample_count);
}

static inline const char* scv_signal_quality(int16_t rssi_avg_x10) {
    int16_t avg = rssi_avg_x10 / 10;
    if(avg > -50) return "Excellent";
    if(avg > -65) return "Good";
    if(avg > -80) return "Fair";
    if(avg > -95) return "Weak";
    return "Very Weak";
}

/**
 * @brief Estimate distance from RSSI using log-distance path loss model
 *
 * Uses measured power of -59 dBm at 1 meter (typical BLE) and
 * path loss exponent of 2.0 (free space). Returns distance in
 * decimeters (tenths of a meter) to avoid floating point.
 */
static inline uint16_t scv_estimate_distance_dm(int16_t rssi_avg_x10) {
    int16_t avg = rssi_avg_x10 / 10;
    int16_t measured_power = -59;
    int16_t diff = measured_power - avg;

    if(diff <= 0) return 1; // < 1m, clamp to 0.1m

    // 10^(diff/20) approximation using integer math:
    // 2^(diff * 3.32 / 10) ~ 2^(diff/3)
    // This gives rough distance in decimeters
    uint16_t dm;
    if(diff < 10) dm = 10;
    else if(diff < 15) dm = 18;
    else if(diff < 20) dm = 32;
    else if(diff < 25) dm = 56;
    else if(diff < 30) dm = 100;
    else if(diff < 35) dm = 180;
    else if(diff < 40) dm = 320;
    else dm = 999;

    return dm;
}

static inline void scv_format_distance(uint16_t dm, char* buf, size_t buf_len) {
    if(dm >= 10) {
        snprintf(buf, buf_len, "%d.%dm", dm / 10, dm % 10);
    } else {
        snprintf(buf, buf_len, "0.%dm", dm);
    }
}
