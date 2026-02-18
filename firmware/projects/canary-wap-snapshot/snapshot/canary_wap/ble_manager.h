/*
 * SecuraCV Canary — BLE Manager (Orchestrator)
 *
 * Initializes and coordinates the three BLE subsystems:
 * - Opera: BLE server/advertising (presence)
 * - Chirp: BLE broadcast alerts (connectionless)
 * - Nearby: BLE scanner/discovery (client)
 *
 * Handles:
 * - NimBLE device initialization (single point of init)
 * - Graceful degradation if BLE hardware unavailable
 * - Mode switching between Opera and Chirp advertising
 * - JSON status generation for HTTP API
 * - Clean shutdown
 */

#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include "ble_config.h"

#if FEATURE_BLE

#include <NimBLEDevice.h>
#include "ble_opera.h"
#include "ble_chirp.h"
#include "ble_nearby.h"

namespace ble_manager {

// ════════════════════════════════════════════════════════════════
// STATE
// ════════════════════════════════════════════════════════════════

static bool g_ble_available = false;
static bool g_opera_active = false;
static bool g_chirp_active = false;
static bool g_nearby_active = false;

// Device ID hash as hex string (persistent, set once during init)
static char g_deviceIdHex[20] = {0};

// ════════════════════════════════════════════════════════════════
// COMMAND HANDLER
// ════════════════════════════════════════════════════════════════

// Handle commands received via Opera BLE characteristic
static void handleBleCommand(const char* command) {
    if (strcmp(command, "STATUS") == 0) {
        // Trigger a status chirp
        #if FEATURE_BLE_CHIRP
        ble_chirp::sendChirp(CHIRP_HEARTBEAT);
        #endif
        Serial.println("[BLE] STATUS command → sent heartbeat chirp");
    } else if (strcmp(command, "EXPORT") == 0) {
        Serial.println("[BLE] EXPORT command received (future: trigger witness export)");
    } else {
        Serial.printf("[BLE] Unknown command: %s\n", command);
    }
}

// ════════════════════════════════════════════════════════════════
// INITIALIZATION
// ════════════════════════════════════════════════════════════════

// Initialize the BLE subsystem
// deviceIdHash: hex string of pubkey fingerprint (e.g., "A3F7B2C1...")
// fwVersion: firmware version string
// chainHeight: pointer to chain sequence counter
// chainHead: pointer to 32-byte chain head hash
// Returns true if BLE initialized successfully
static bool init(const char* deviceIdHash, const char* fwVersion,
                 uint32_t* chainHeight, uint8_t* chainHead) {
    // Store device ID hash
    strncpy(g_deviceIdHex, deviceIdHash, sizeof(g_deviceIdHex) - 1);

    Serial.println("[BLE] Initializing NimBLE stack...");

    // Initialize NimBLE with the device name
    // Build name: "SCV-" + last 4 chars of hash
    char bleName[16];
    size_t hashLen = strlen(deviceIdHash);
    if (hashLen >= 4) {
        snprintf(bleName, sizeof(bleName), "%s%s",
                 BLE_DEVICE_NAME_PREFIX, deviceIdHash + hashLen - 4);
    } else {
        snprintf(bleName, sizeof(bleName), "%s0000", BLE_DEVICE_NAME_PREFIX);
    }

    // NimBLEDevice::init() can fail if BLE hardware is unavailable
    // On ESP32-S3 without antenna, this may still succeed but advertising won't reach
    NimBLEDevice::init(bleName);

    // Set transmit power
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // Max power for range

    g_ble_available = true;

    // Initialize Opera (server/advertising)
    #if FEATURE_BLE_OPERA
    if (ble_opera::init(deviceIdHash, fwVersion, chainHeight, chainHead)) {
        ble_opera::setCommandHandler(handleBleCommand);
        Serial.println("[BLE] Opera subsystem ready");
    } else {
        Serial.println("[BLE] Opera init failed");
    }
    #endif

    // Initialize Chirp (broadcast alerts)
    #if FEATURE_BLE_CHIRP
    if (ble_chirp::init(deviceIdHash, chainHead)) {
        g_chirp_active = true;
        Serial.println("[BLE] Chirp subsystem ready");
    } else {
        Serial.println("[BLE] Chirp init failed");
    }
    #endif

    // Initialize Nearby (scanner/discovery)
    #if FEATURE_BLE_NEARBY
    if (ble_nearby::init()) {
        Serial.println("[BLE] Nearby subsystem ready");
    } else {
        Serial.println("[BLE] Nearby init failed");
    }
    #endif

    return true;
}

// ════════════════════════════════════════════════════════════════
// START / STOP SUBSYSTEMS
// ════════════════════════════════════════════════════════════════

static void operaStart() {
    #if FEATURE_BLE_OPERA
    if (g_ble_available) {
        ble_opera::startAdvertising();
        g_opera_active = true;
    }
    #endif
}

static void operaStop() {
    #if FEATURE_BLE_OPERA
    ble_opera::stopAdvertising();
    g_opera_active = false;
    #endif
}

static void nearbyStart() {
    #if FEATURE_BLE_NEARBY
    if (g_ble_available) {
        if (ble_nearby::startTask()) {
            g_nearby_active = true;
        }
    }
    #endif
}

static void nearbyStop() {
    #if FEATURE_BLE_NEARBY
    ble_nearby::stop();
    g_nearby_active = false;
    #endif
}

// ════════════════════════════════════════════════════════════════
// SEND CHIRP (public wrapper)
// ════════════════════════════════════════════════════════════════

static bool sendChirp(ChirpType type) {
    #if FEATURE_BLE_CHIRP
    if (!g_ble_available || !g_chirp_active) return false;
    return ble_chirp::sendChirp(type);
    #else
    return false;
    #endif
}

// Check heartbeat timer — call from loop()
static void chirpHeartbeatCheck() {
    #if FEATURE_BLE_CHIRP
    if (g_ble_available && g_chirp_active) {
        ble_chirp::heartbeatCheck();
    }
    #endif
}

// ════════════════════════════════════════════════════════════════
// UPDATE (call from loop)
// ════════════════════════════════════════════════════════════════

static void update() {
    if (!g_ble_available) return;

    #if FEATURE_BLE_OPERA
    // Periodically update characteristic values
    static unsigned long lastOperaUpdate = 0;
    if (millis() - lastOperaUpdate > 5000) {
        ble_opera::update();
        lastOperaUpdate = millis();
    }
    #endif
}

// ════════════════════════════════════════════════════════════════
// STATUS JSON
// ════════════════════════════════════════════════════════════════

// Build /api/ble/status JSON
static String statusJson() {
    String json = "{";
    json += "\"ble_enabled\":true,";
    json += "\"ble_available\":";
    json += g_ble_available ? "true" : "false";

    // Opera status
    json += ",\"opera\":{\"active\":";
    json += g_opera_active ? "true" : "false";
    json += ",\"connections_total\":";
    json += String(ble_opera::getConnectionsTotal());
    json += ",\"connected_now\":";
    json += String(ble_opera::getConnectedNow());
    json += ",\"device_name\":\"";
    json += ble_opera::getDeviceName();
    json += "\"}";

    // Chirp status
    json += ",\"chirp\":{\"active\":";
    json += g_chirp_active ? "true" : "false";
    json += ",\"sent\":";
    json += String(ble_chirp::getChirpsSent());
    json += ",\"received\":";
    json += String(ble_nearby::getChirpsReceived());
    json += ",\"last_sent_type\":\"";
    json += chirpTypeName(ble_chirp::getLastChirpType());
    json += "\"}";

    // Nearby status
    json += ",\"nearby\":{\"active\":";
    json += g_nearby_active ? "true" : "false";
    json += ",\"canary_count\":";
    json += String(ble_nearby::activeCanaryCount());
    json += ",\"non_canary_count\":";
    json += String(ble_nearby::getNonCanaryCount());
    json += ",\"scan_active\":";
    json += ble_nearby::isScanActive() ? "true" : "false";
    json += "}";

    json += "}";
    return json;
}

// Build /api/nearby JSON
static String nearbyJson() {
    return ble_nearby::buildNearbyJson();
}

// ════════════════════════════════════════════════════════════════
// GETTERS
// ════════════════════════════════════════════════════════════════

static bool isAvailable() {
    return g_ble_available;
}

static bool isOperaActive() {
    return g_opera_active;
}

static bool isChirpActive() {
    return g_chirp_active;
}

static bool isNearbyActive() {
    return g_nearby_active;
}

// ════════════════════════════════════════════════════════════════
// SHUTDOWN
// ════════════════════════════════════════════════════════════════

static void shutdown() {
    nearbyStop();
    operaStop();
    NimBLEDevice::deinit(true);
    g_ble_available = false;
    Serial.println("[BLE] Shutdown complete");
}

} // namespace ble_manager

#else // !FEATURE_BLE

// No-op stubs when BLE is disabled at compile time
namespace ble_manager {
    static inline bool init(const char*, const char*, uint32_t*, uint8_t*) { return false; }
    static inline void operaStart() {}
    static inline void operaStop() {}
    static inline void nearbyStart() {}
    static inline void nearbyStop() {}
    static inline bool sendChirp(uint8_t) { return false; }
    static inline void chirpHeartbeatCheck() {}
    static inline void update() {}
    static inline String statusJson() { return "{\"ble_enabled\":false}"; }
    static inline String nearbyJson() { return "{\"canaries\":[],\"non_canary_device_count\":0,\"scan_active\":false,\"last_scan_ms\":0}"; }
    static inline bool isAvailable() { return false; }
    static inline bool isOperaActive() { return false; }
    static inline bool isChirpActive() { return false; }
    static inline bool isNearbyActive() { return false; }
    static inline void shutdown() {}
}

#endif // FEATURE_BLE
#endif // BLE_MANAGER_H
