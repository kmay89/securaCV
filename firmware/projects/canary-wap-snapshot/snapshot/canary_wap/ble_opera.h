/*
 * SecuraCV Canary — BLE Opera (Server/Advertising Mode)
 *
 * The Canary "performs" its presence. Other Canary devices and authorized
 * phones can discover it via BLE advertising and read status characteristics.
 *
 * Features:
 * - Custom BLE service with SecuraCV UUIDs
 * - Privacy-safe device identifier (derived from Ed25519 pubkey hash)
 * - Read-only characteristics: device info, witness status
 * - Writable command characteristic for remote control
 * - NimBLE for lower memory footprint
 *
 * Security:
 * - No MAC address in cleartext advertisements
 * - Device name uses truncated pubkey hash, not hardware ID
 * - Connection events logged to witness chain
 */

#ifndef BLE_OPERA_H
#define BLE_OPERA_H

#include "ble_config.h"

#if FEATURE_BLE && FEATURE_BLE_OPERA

#include <NimBLEDevice.h>
#include "log_level.h"

// Forward declarations for witness chain integration
// These are provided by the main .ino or ble_manager
extern void log_health(LogLevel level, LogCategory category, const char* message, const char* detail);

namespace ble_opera {

// ════════════════════════════════════════════════════════════════
// STATE
// ════════════════════════════════════════════════════════════════

static NimBLEServer* g_pServer = nullptr;
static NimBLEService* g_pService = nullptr;
static NimBLECharacteristic* g_pDeviceInfoChar = nullptr;
static NimBLECharacteristic* g_pWitnessChar = nullptr;
static NimBLECharacteristic* g_pCommandChar = nullptr;
static NimBLEAdvertising* g_pAdvertising = nullptr;

static bool g_advertising = false;
static uint32_t g_connectionsTotal = 0;
static uint32_t g_connectedNow = 0;
static char g_deviceName[16] = {0};  // "SCV-XXXX"

// External state references (set by ble_manager during init)
static const char* g_deviceIdHash = nullptr;
static const char* g_firmwareVersion = nullptr;
static uint32_t* g_chainHeight = nullptr;
static uint8_t* g_chainHead = nullptr;

// Last received command (for API visibility)
static char g_lastCommand[32] = {0};
static unsigned long g_lastCommandMs = 0;

// ════════════════════════════════════════════════════════════════
// COMMAND HANDLER CALLBACK TYPE
// ════════════════════════════════════════════════════════════════

typedef void (*CommandHandler)(const char* command);
static CommandHandler g_commandHandler = nullptr;

// ════════════════════════════════════════════════════════════════
// SERVER CALLBACKS
// ════════════════════════════════════════════════════════════════

class OperaServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        g_connectionsTotal++;
        g_connectedNow++;
        Serial.printf("[BLE] Client connected (total: %u, now: %u)\n",
                      g_connectionsTotal, g_connectedNow);

        // Resume advertising to allow additional connections
        if (g_pAdvertising) {
            g_pAdvertising->start();
        }
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        if (g_connectedNow > 0) g_connectedNow--;
        Serial.printf("[BLE] Client disconnected (reason: %d, now: %u)\n",
                      reason, g_connectedNow);

        // Resume advertising
        if (g_pAdvertising && g_advertising) {
            g_pAdvertising->start();
        }
    }
};

// ════════════════════════════════════════════════════════════════
// CHARACTERISTIC CALLBACKS
// ════════════════════════════════════════════════════════════════

class CommandCharCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        String value = pCharacteristic->getValue();
        if (value.length() > 0 && value.length() < sizeof(g_lastCommand)) {
            value.toCharArray(g_lastCommand, sizeof(g_lastCommand));
            g_lastCommandMs = millis();

            Serial.printf("[BLE] Command received: %s\n", g_lastCommand);

            if (g_commandHandler) {
                g_commandHandler(g_lastCommand);
            }
        }
    }
};

static OperaServerCallbacks g_serverCallbacks;
static CommandCharCallbacks g_commandCallbacks;

// ════════════════════════════════════════════════════════════════
// UPDATE CHARACTERISTICS (call periodically)
// ════════════════════════════════════════════════════════════════

static void updateDeviceInfoCharacteristic() {
    if (!g_pDeviceInfoChar) return;

    // Build compact JSON device info
    char buf[192];
    uint32_t height = g_chainHeight ? *g_chainHeight : 0;
    uint32_t uptimeSec = millis() / 1000;
    snprintf(buf, sizeof(buf),
        "{\"id\":\"%s\",\"fw\":\"%s\",\"type\":\"canary\",\"uptime\":%u,\"chain_height\":%u}",
        g_deviceIdHash ? g_deviceIdHash : "unknown",
        g_firmwareVersion ? g_firmwareVersion : "0.0.0",
        uptimeSec,
        height
    );
    g_pDeviceInfoChar->setValue(buf);
}

static void updateWitnessCharacteristic() {
    if (!g_pWitnessChar) return;

    // Build compact witness status
    char buf[128];
    uint32_t height = g_chainHeight ? *g_chainHeight : 0;

    // Chain head prefix (first 8 bytes as hex)
    char hashHex[17] = {0};
    if (g_chainHead) {
        for (int i = 0; i < 8; i++) {
            snprintf(hashHex + i * 2, 3, "%02x", g_chainHead[i]);
        }
    }

    snprintf(buf, sizeof(buf),
        "{\"chain_height\":%u,\"chain_head\":\"%s\",\"verified\":true}",
        height, hashHex
    );
    g_pWitnessChar->setValue(buf);
}

// ════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════

static bool init(const char* deviceIdHash, const char* fwVersion,
                 uint32_t* chainHeight, uint8_t* chainHead) {
    g_deviceIdHash = deviceIdHash;
    g_firmwareVersion = fwVersion;
    g_chainHeight = chainHeight;
    g_chainHead = chainHead;

    // Build device name: "SCV-" + last 4 chars of device ID hash
    size_t hashLen = strlen(deviceIdHash);
    if (hashLen >= 4) {
        snprintf(g_deviceName, sizeof(g_deviceName), "%s%s",
                 BLE_DEVICE_NAME_PREFIX, deviceIdHash + hashLen - 4);
    } else {
        snprintf(g_deviceName, sizeof(g_deviceName), "%s%s",
                 BLE_DEVICE_NAME_PREFIX, deviceIdHash);
    }

    // Create server
    g_pServer = NimBLEDevice::createServer();
    if (!g_pServer) {
        Serial.println("[BLE] Failed to create BLE server");
        return false;
    }
    g_pServer->setCallbacks(&g_serverCallbacks);

    // Create service
    g_pService = g_pServer->createService(SCV_SERVICE_UUID);
    if (!g_pService) {
        Serial.println("[BLE] Failed to create BLE service");
        return false;
    }

    // Create characteristics
    g_pDeviceInfoChar = g_pService->createCharacteristic(
        SCV_CHAR_DEVICE_INFO_UUID,
        NIMBLE_PROPERTY::READ
    );

    g_pWitnessChar = g_pService->createCharacteristic(
        SCV_CHAR_WITNESS_UUID,
        NIMBLE_PROPERTY::READ
    );

    g_pCommandChar = g_pService->createCharacteristic(
        SCV_CHAR_COMMAND_UUID,
        NIMBLE_PROPERTY::WRITE
    );
    g_pCommandChar->setCallbacks(&g_commandCallbacks);

    // Set initial values
    updateDeviceInfoCharacteristic();
    updateWitnessCharacteristic();

    // Start service
    g_pService->start();

    // Configure advertising
    g_pAdvertising = NimBLEDevice::getAdvertising();
    g_pAdvertising->addServiceUUID(SCV_SERVICE_UUID);
    g_pAdvertising->setName(g_deviceName);
    g_pAdvertising->enableScanResponse(true);

    Serial.printf("[BLE] Opera initialized — device name: %s\n", g_deviceName);
    return true;
}

static void startAdvertising() {
    if (g_pAdvertising) {
        g_pAdvertising->start();
        g_advertising = true;
        Serial.println("[BLE] Opera advertising started");
    }
}

static void stopAdvertising() {
    if (g_pAdvertising) {
        g_pAdvertising->stop();
        g_advertising = false;
        Serial.println("[BLE] Opera advertising stopped");
    }
}

static bool isAdvertising() {
    return g_advertising;
}

static uint32_t getConnectionsTotal() {
    return g_connectionsTotal;
}

static uint32_t getConnectedNow() {
    return g_connectedNow;
}

static const char* getDeviceName() {
    return g_deviceName;
}

static void setCommandHandler(CommandHandler handler) {
    g_commandHandler = handler;
}

// Call periodically to refresh characteristic values
static void update() {
    updateDeviceInfoCharacteristic();
    updateWitnessCharacteristic();
}

// Temporarily stop advertising (for chirp mode switching)
static NimBLEAdvertising* getAdvertising() {
    return g_pAdvertising;
}

} // namespace ble_opera

#else // !FEATURE_BLE || !FEATURE_BLE_OPERA

namespace ble_opera {
    static inline bool init(const char*, const char*, uint32_t*, uint8_t*) { return false; }
    static inline void startAdvertising() {}
    static inline void stopAdvertising() {}
    static inline bool isAdvertising() { return false; }
    static inline uint32_t getConnectionsTotal() { return 0; }
    static inline uint32_t getConnectedNow() { return 0; }
    static inline const char* getDeviceName() { return ""; }
    static inline void setCommandHandler(void (*)(const char*)) {}
    static inline void update() {}
    static inline void* getAdvertising() { return nullptr; }
}

#endif // FEATURE_BLE && FEATURE_BLE_OPERA

#endif // BLE_OPERA_H
