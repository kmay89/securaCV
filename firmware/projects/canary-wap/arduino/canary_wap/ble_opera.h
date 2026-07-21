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
#include "fleet_beacon.h"

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

// ── Fleet-link presence beacon (continuous manufacturer-data advert) ──
// Live status fed in by setBeaconStatus(); fp is fixed at init from the
// device id hash suffix, chain height is read live from *g_chainHeight.
// See fleet_beacon.h for the authoritative wire contract (11-byte blob,
// company id 0xFFFF, type 0x10 — disambiguated from the 17-byte chirp by
// BOTH size and type).
static uint8_t g_beaconFp[2] = {0, 0};
static uint8_t g_beaconFlags = 0;
static int     g_beaconBattery = -1;  // -1 = unknown -> 0xFF on the wire
static int     g_beaconHealth  = -1;  // -1 = unknown -> 0xFF on the wire

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
// FLEET-LINK PRESENCE BEACON ADVERT (single (re)build chokepoint)
// ════════════════════════════════════════════════════════════════

// Build the beacon manufacturer data from live values and set it as the
// PRIMARY advertisement, moving the 128-bit SCV service UUID + "SCV-XXXX"
// name into the SCAN RESPONSE (other WAPs active-scan, so they still resolve
// both — backward compatible). Both init and the chirp-restore path route
// through here so "restore after a chirp burst" re-applies the beacon, not
// the old UUID advert.
//
// Fail-safe: if the explicit adv-data path fails for any reason, fall back to
// today's addServiceUUID + setName so the device is NEVER left un-advertised.
// Returns true when the beacon path took, false when the fallback was used.
static bool applyBeaconAdvertising() {
    if (!g_pAdvertising) return false;

    // Payload bytes [2..10]; NimBLE prepends the 2 company-id bytes.
    uint8_t payload[FLEET_BEACON_PAYLOAD_LEN];
    uint32_t chain = g_chainHeight ? *g_chainHeight : 0;
    fleet_beacon_build(payload, g_beaconFlags, g_beaconBattery, g_beaconHealth,
                       chain, g_beaconFp[0], g_beaconFp[1]);

    // Full manufacturer blob = company id (LE) + payload.
    uint8_t mfg[FLEET_BEACON_PAYLOAD_LEN + 2];
    mfg[0] = FLEET_BEACON_COMPANY_ID & 0xFF;
    mfg[1] = (FLEET_BEACON_COMPANY_ID >> 8) & 0xFF;
    memcpy(&mfg[2], payload, FLEET_BEACON_PAYLOAD_LEN);

    NimBLEAdvertisementData advData;
    advData.setManufacturerData(std::string((char*)mfg, sizeof(mfg)));

    NimBLEAdvertisementData scanData;
    scanData.setName(g_deviceName);
    scanData.addServiceUUID(SCV_SERVICE_UUID);

    bool ok = g_pAdvertising->setAdvertisementData(advData);
    ok = g_pAdvertising->setScanResponseData(scanData) && ok;
    if (!ok) {
        // Fallback: legacy UUID + name advert (today's behavior).
        g_pAdvertising->addServiceUUID(SCV_SERVICE_UUID);
        g_pAdvertising->setName(g_deviceName);
        return false;
    }
    return true;
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

    // Fleet-link beacon fingerprint: last 2 bytes of the pubkey fingerprint,
    // parsed from the last 4 hex chars of the device id hash — the same 2
    // bytes the chirp carries at [15-16] and the "SCV-XXXX" name derives from.
    g_beaconFp[0] = 0;
    g_beaconFp[1] = 0;
    if (hashLen >= 4) {
        const char* suffix = deviceIdHash + hashLen - 4;
        char hx[3] = {0};
        hx[0] = suffix[0]; hx[1] = suffix[1];
        g_beaconFp[0] = (uint8_t)strtol(hx, nullptr, 16);
        hx[0] = suffix[2]; hx[1] = suffix[3];
        g_beaconFp[1] = (uint8_t)strtol(hx, nullptr, 16);
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

    // Configure advertising. Scan response must be enabled so the SCV service
    // UUID + name (moved off the primary advert to make room for the beacon
    // manufacturer data) are still discoverable by active-scanning WAPs.
    g_pAdvertising = NimBLEDevice::getAdvertising();
    g_pAdvertising->enableScanResponse(true);
    if (!applyBeaconAdvertising()) {
        // applyBeaconAdvertising() already installed the legacy UUID+name
        // advert as a fallback — the device is advertising, just without the
        // presence beacon. Log so the failure is visible in the field.
        Serial.println("[BLE] Opera beacon adv setup failed — legacy UUID advert active");
    }

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

// Feed live status into the presence beacon. Stored here and applied on the
// next refreshBeacon()/update(). chain height is read live from the pointer
// handed to init(); fp is fixed at init. Cheap — safe to call every loop.
static void setBeaconStatus(uint8_t flags, int battery_pct, int health_pct) {
    g_beaconFlags   = flags;
    g_beaconBattery = battery_pct;
    g_beaconHealth  = health_pct;
}

// Re-apply the beacon advert with current live values (used by the periodic
// refresh and by the chirp-restore path). The caller must ensure a chirp
// burst is not mid-broadcast — ble_manager gates on ble_chirp::isBroadcasting().
static bool refreshBeacon() {
    return applyBeaconAdvertising();
}

// Call periodically to refresh characteristic values AND the live beacon
// manufacturer data (battery/health/chain/flags change over time). The chirp
// state machine owns the advert while broadcasting, so ble_manager only calls
// this when a chirp is not in flight.
static void update() {
    updateDeviceInfoCharacteristic();
    updateWitnessCharacteristic();
    applyBeaconAdvertising();
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
    static inline void setBeaconStatus(uint8_t, int, int) {}
    static inline bool refreshBeacon() { return false; }
    static inline bool applyBeaconAdvertising() { return false; }
}

#endif // FEATURE_BLE && FEATURE_BLE_OPERA

#endif // BLE_OPERA_H
