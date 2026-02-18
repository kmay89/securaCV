/*
 * SecuraCV Canary — BLE Chirp (Broadcast Alerts)
 *
 * Short, connectionless broadcast alerts between Canary devices.
 * Uses BLE manufacturer-specific advertising data (no connection required).
 *
 * Features:
 * - Sends manufacturer-specific advertising packets with chirp payload
 * - Rate limited: max 1 chirp per 10 seconds
 * - Automatic heartbeat every 5 minutes
 * - Chirp types: ALERT, HEARTBEAT, TAMPER, WITNESS, BOOT
 * - Non-blocking: chirp starts advertising, update() restores Opera after 2s
 *
 * Privacy:
 * - Uses truncated witness chain hash (proves integrity, not content)
 * - Device ID prefix from pubkey hash, not hardware MAC
 * - Time coarsened to hour buckets
 */

#ifndef BLE_CHIRP_H
#define BLE_CHIRP_H

#include "ble_config.h"

#if FEATURE_BLE && FEATURE_BLE_CHIRP

#include <NimBLEDevice.h>

namespace ble_chirp {

// ════════════════════════════════════════════════════════════════
// CHIRP BROADCAST DURATION
// ════════════════════════════════════════════════════════════════
#define CHIRP_BROADCAST_DURATION_MS  2000  // Broadcast chirp for 2 seconds

// ════════════════════════════════════════════════════════════════
// STATE
// ════════════════════════════════════════════════════════════════

// Non-blocking chirp state machine
enum ChirpState : uint8_t {
    CHIRP_IDLE        = 0,  // No chirp in progress
    CHIRP_BROADCASTING = 1, // Chirp advertising active, waiting for duration
};

static ChirpState g_chirpState = CHIRP_IDLE;
static uint32_t g_chirpsSent = 0;
static unsigned long g_lastChirpMs = 0;
static unsigned long g_lastHeartbeatMs = 0;
static unsigned long g_chirpStartMs = 0;  // When current chirp broadcast started
static ChirpType g_lastChirpType = CHIRP_BOOT;

// External references (set during init)
static uint8_t* g_chainHead = nullptr;     // Current chain head hash (32 bytes)
static const char* g_deviceIdHash = nullptr;  // Hex string of pubkey fingerprint
static uint8_t g_deviceIdPrefixBytes[2] = {0};  // 2-byte device ID for chirp payload

// ════════════════════════════════════════════════════════════════
// PAYLOAD BUILDER
// ════════════════════════════════════════════════════════════════

// Build the chirp manufacturer data payload
// Layout (after company ID, which NimBLE adds):
//   [0]     Chirp type (uint8_t)
//   [1-4]   Coarse timestamp (uint32_t, epoch/3600 = hour bucket)
//   [5-12]  Chain hash prefix (8 bytes)
//   [13-14] Device ID prefix (2 bytes)
static void buildChirpPayload(ChirpType type, uint8_t* payload, size_t* len) {
    // Chirp type
    payload[0] = (uint8_t)type;

    // Coarse timestamp: epoch / 3600 (hour bucket)
    // On ESP32 without NTP, use millis()/3600000 as relative hour bucket
    uint32_t hourBucket = millis() / 3600000UL;
    payload[1] = (hourBucket >> 24) & 0xFF;
    payload[2] = (hourBucket >> 16) & 0xFF;
    payload[3] = (hourBucket >> 8) & 0xFF;
    payload[4] = hourBucket & 0xFF;

    // Chain hash prefix (8 bytes)
    if (g_chainHead) {
        memcpy(&payload[5], g_chainHead, 8);
    } else {
        memset(&payload[5], 0, 8);
    }

    // Device ID prefix (2 bytes)
    payload[13] = g_deviceIdPrefixBytes[0];
    payload[14] = g_deviceIdPrefixBytes[1];

    *len = CHIRP_PAYLOAD_SIZE;
}

// ════════════════════════════════════════════════════════════════
// RESTORE OPERA ADVERTISING (internal)
// ════════════════════════════════════════════════════════════════

static void restoreOperaAdvertising() {
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    if (!pAdvertising) return;

    pAdvertising->stop();

    // Restore Opera service advertising
    NimBLEAdvertisementData restoreData;
    restoreData.setCompleteServices(NimBLEUUID(SCV_SERVICE_UUID));
    pAdvertising->setAdvertisementData(restoreData);
    pAdvertising->setName(NimBLEDevice::getAddress().toString().c_str());
    pAdvertising->start();
}

// ════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════

static bool init(const char* deviceIdHash, uint8_t* chainHead) {
    g_deviceIdHash = deviceIdHash;
    g_chainHead = chainHead;
    g_lastHeartbeatMs = millis();

    // Parse last 4 hex chars of device ID hash into 2 bytes for chirp prefix
    size_t hashLen = strlen(deviceIdHash);
    if (hashLen >= 4) {
        const char* suffix = deviceIdHash + hashLen - 4;
        char hex[3] = {0};

        hex[0] = suffix[0]; hex[1] = suffix[1];
        g_deviceIdPrefixBytes[0] = (uint8_t)strtol(hex, nullptr, 16);

        hex[0] = suffix[2]; hex[1] = suffix[3];
        g_deviceIdPrefixBytes[1] = (uint8_t)strtol(hex, nullptr, 16);
    }

    Serial.printf("[BLE] Chirp initialized — device prefix: %02x%02x\n",
                  g_deviceIdPrefixBytes[0], g_deviceIdPrefixBytes[1]);
    return true;
}

// Start a chirp broadcast (non-blocking)
// Returns true if chirp was started, false if rate-limited or already in progress
static bool sendChirp(ChirpType type) {
    unsigned long now = millis();

    // Rate limit check
    if ((now - g_lastChirpMs) < CHIRP_MIN_INTERVAL_MS && g_chirpsSent > 0) {
        Serial.printf("[BLE] Chirp rate limited (type=%s)\n", chirpTypeName(type));
        return false;
    }

    // Prevent overlapping chirps
    if (g_chirpState != CHIRP_IDLE) return false;

    // Build payload
    uint8_t payload[CHIRP_PAYLOAD_SIZE];
    size_t payloadLen;
    buildChirpPayload(type, payload, &payloadLen);

    // Build full manufacturer data (company ID + payload)
    // NimBLE expects: company ID (2 bytes LE) + data
    uint8_t mfgData[CHIRP_PAYLOAD_SIZE + 2];
    mfgData[0] = CHIRP_COMPANY_ID & 0xFF;
    mfgData[1] = (CHIRP_COMPANY_ID >> 8) & 0xFF;
    memcpy(&mfgData[2], payload, payloadLen);

    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    if (!pAdvertising) return false;

    // Stop current advertising
    pAdvertising->stop();

    // Set manufacturer data for chirp
    NimBLEAdvertisementData advData;
    advData.setManufacturerData(std::string((char*)mfgData, payloadLen + 2));
    advData.setName(NimBLEDevice::getAddress().toString().c_str());
    pAdvertising->setAdvertisementData(advData);

    // Start chirp broadcast (non-blocking — update() will stop it after duration)
    pAdvertising->start();

    g_chirpState = CHIRP_BROADCASTING;
    g_chirpStartMs = now;
    g_chirpsSent++;
    g_lastChirpMs = now;
    g_lastChirpType = type;

    Serial.printf("[BLE] Chirp started: %s (#%u)\n", chirpTypeName(type), g_chirpsSent);
    return true;
}

// Non-blocking update — call from loop() to manage chirp state transitions
// Restores Opera advertising after chirp broadcast duration expires
static void update() {
    if (g_chirpState == CHIRP_BROADCASTING) {
        if ((millis() - g_chirpStartMs) >= CHIRP_BROADCAST_DURATION_MS) {
            restoreOperaAdvertising();
            g_chirpState = CHIRP_IDLE;
            Serial.println("[BLE] Chirp broadcast complete, Opera restored");
        }
    }
}

// Check if heartbeat chirp is due — call from loop()
// Lightweight: just a millis() comparison
static void heartbeatCheck() {
    unsigned long now = millis();
    if ((now - g_lastHeartbeatMs) >= CHIRP_HEARTBEAT_INTERVAL_MS) {
        g_lastHeartbeatMs = now;
        sendChirp(CHIRP_HEARTBEAT);
    }
}

static uint32_t getChirpsSent() {
    return g_chirpsSent;
}

static ChirpType getLastChirpType() {
    return g_lastChirpType;
}

static bool isRateLimited() {
    return (millis() - g_lastChirpMs) < CHIRP_MIN_INTERVAL_MS;
}

static bool isBroadcasting() {
    return g_chirpState == CHIRP_BROADCASTING;
}

} // namespace ble_chirp

#else // !FEATURE_BLE || !FEATURE_BLE_CHIRP

namespace ble_chirp {
    static inline bool init(const char*, uint8_t*) { return false; }
    static inline bool sendChirp(uint8_t) { return false; }
    static inline void update() {}
    static inline void heartbeatCheck() {}
    static inline uint32_t getChirpsSent() { return 0; }
    static inline uint8_t getLastChirpType() { return 0; }
    static inline bool isRateLimited() { return false; }
    static inline bool isBroadcasting() { return false; }
}

#endif // FEATURE_BLE && FEATURE_BLE_CHIRP

#endif // BLE_CHIRP_H
