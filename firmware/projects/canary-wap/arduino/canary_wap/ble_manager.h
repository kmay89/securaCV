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
#include <ArduinoJson.h>
#include "ble_opera.h"
#include "ble_chirp.h"
#include "ble_nearby.h"
#include "ble_heap_guard.h"

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

    // Single NimBLE init owner. When the bluetooth_channel (pairing/console)
    // feature is compiled in, it runs first in setup() and already brought the
    // stack up — owning the GAP device name, TX power, MTU and security. NimBLE
    // 2.x init() is idempotent, so calling it again here was a no-op that
    // SILENTLY DROPPED our intended name (the stack keeps the first name set,
    // i.e. "SecuraCV-Canary"), and the unconditional setPower(9) clobbered
    // bluetooth_channel's NVS-configured power. So: only bring the stack up
    // ourselves when nobody else has, and propagate a real failure instead of
    // assuming success.
    //
    // NimBLEDevice::init() can fail if BLE hardware is unavailable. Without the
    // external antenna on the IPEX connector, init may succeed but advertising
    // won't reach far. XIAO ESP32-C3 REQUIRES the antenna; XIAO ESP32-S3 has an
    // onboard antenna (external improves range).
    if (!NimBLEDevice::isInitialized()) {
        // Fail closed on low memory: the controller malloc failure inside
        // init() panics (it doesn't return false), so on a no-PSRAM build the
        // check below never runs — the device boot-loops. Skip the stack and
        // operate without BLE discovery instead.
        size_t largest = 0;
        size_t total   = 0;
        if (!ble_heap_guard::can_init(&largest, &total)) {
            Serial.printf("[BLE] Discovery skipped: insufficient heap "
                          "(largest internal block %u B, total free %u B)\n",
                          (unsigned)largest, (unsigned)total);
            g_ble_available = false;
            return false;
        }
        if (!NimBLEDevice::init(bleName)) {
            Serial.println("[BLE] NimBLE init failed — BLE Discovery unavailable");
            g_ble_available = false;
            return false;
        }
        // Sole owner of the stack — set TX power here. NimBLE 2.x setPower takes
        // the dBm value directly (int8_t); +9 dBm is the max valid on S3/C3.
        // DO NOT pass ESP_PWR_LVL_* — those are indexes, not dBm.
        NimBLEDevice::setPower(9);
    }

    g_ble_available = NimBLEDevice::isInitialized();
    if (!g_ble_available) {
        Serial.println("[BLE] NimBLE stack not initialized — BLE Discovery unavailable");
        return false;
    }

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

    #if FEATURE_BLE_CHIRP
    // Process non-blocking chirp state machine (restores Opera after broadcast)
    if (g_chirp_active) {
        ble_chirp::update();
    }
    #endif

    #if FEATURE_BLE_OPERA
    // Periodically refresh characteristic values AND the live presence beacon
    // (battery/health/chain/flags). Skip while a chirp burst owns the advert —
    // ble_chirp::update() above restores the beacon when the burst ends, so
    // clobbering it mid-broadcast would both cut the chirp short and race the
    // restore. isBroadcasting() is a cheap millis compare.
    static unsigned long lastOperaUpdate = 0;
    if (millis() - lastOperaUpdate > 5000) {
        #if FEATURE_BLE_CHIRP
        const bool chirp_owns_advert = ble_chirp::isBroadcasting();
        #else
        const bool chirp_owns_advert = false;
        #endif
        if (!chirp_owns_advert) {
            ble_opera::update();
            lastOperaUpdate = millis();
        }
    }
    #endif
}

// ════════════════════════════════════════════════════════════════
// STATUS JSON
// ════════════════════════════════════════════════════════════════

// Build /api/ble/status JSON
static String statusJson() {
    JsonDocument doc;
    doc["ble_enabled"] = true;
    doc["ble_available"] = g_ble_available;

    // Opera status
    JsonObject opera = doc["opera"].to<JsonObject>();
    opera["active"] = g_opera_active;
    opera["connections_total"] = ble_opera::getConnectionsTotal();
    opera["connected_now"] = ble_opera::getConnectedNow();
    opera["device_name"] = ble_opera::getDeviceName();

    // Chirp status
    JsonObject chirp = doc["chirp"].to<JsonObject>();
    chirp["active"] = g_chirp_active;
    chirp["sent"] = ble_chirp::getChirpsSent();
    chirp["received"] = ble_nearby::getChirpsReceived();
    chirp["last_sent_type"] = chirpTypeName(ble_chirp::getLastChirpType());

    // Nearby status
    JsonObject nearby = doc["nearby"].to<JsonObject>();
    nearby["active"] = g_nearby_active;
    nearby["canary_count"] = ble_nearby::activeCanaryCount();
    nearby["non_canary_count"] = ble_nearby::getNonCanaryCount();
    nearby["scan_active"] = ble_nearby::isScanActive();
    nearby["beacons_received"] = ble_nearby::getBeaconsReceived();

    String json;
    serializeJson(doc, json);
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
