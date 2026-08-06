/*
 * SecuraCV Canary — BLE Nearby Discovery (Scanner/Client Mode)
 *
 * Scans for nearby BLE devices, identifies other Canaries via SecuraCV
 * service UUID, tracks RSSI for proximity estimation.
 *
 * Privacy Constraints:
 * - Non-Canary devices are COUNTED only, never individually logged
 * - No MAC addresses stored to SD or exposed via API
 * - Only Canary-to-Canary identification (via SecuraCV service UUID)
 *
 * Threading:
 * - Scan runs on a dedicated FreeRTOS task (non-blocking)
 * - nearbyCanaries[] protected by mutex for thread-safe HTTP access
 */

#ifndef BLE_NEARBY_H
#define BLE_NEARBY_H

#include "ble_config.h"

#if FEATURE_BLE && FEATURE_BLE_NEARBY

#include <NimBLEDevice.h>
#include "fleet_beacon.h"   // canonical fleet-link presence beacon wire format

namespace ble_nearby {

// ════════════════════════════════════════════════════════════════
// STATE
// ════════════════════════════════════════════════════════════════

static NearbyCanary g_nearbyCanaries[NEARBY_MAX_CANARIES];
static uint16_t g_nonCanaryCount = 0;
static SemaphoreHandle_t g_nearbyMutex = nullptr;
static TaskHandle_t g_scanTaskHandle = nullptr;
static NimBLEScan* g_pBLEScan = nullptr;
static bool g_scanActive = false;
static unsigned long g_lastScanMs = 0;
static uint32_t g_lastScanDurationMs = 0;
static uint32_t g_chirpsReceived = 0;
static uint32_t g_beaconsReceived = 0;
static uint32_t g_canariesDiscovered = 0;

// ════════════════════════════════════════════════════════════════
// HELPERS
// ════════════════════════════════════════════════════════════════

static int findCanarySlot(const char* prefix) {
    for (int i = 0; i < NEARBY_MAX_CANARIES; i++) {
        if (g_nearbyCanaries[i].active &&
            strncmp(g_nearbyCanaries[i].deviceIdPrefix, prefix, 4) == 0) {
            return i;
        }
    }
    return -1;
}

static int findFreeSlot() {
    // Find inactive slot
    for (int i = 0; i < NEARBY_MAX_CANARIES; i++) {
        if (!g_nearbyCanaries[i].active) return i;
    }
    // No free slot — evict oldest (LRU)
    int oldest = 0;
    unsigned long oldestTime = ULONG_MAX;
    for (int i = 0; i < NEARBY_MAX_CANARIES; i++) {
        if (g_nearbyCanaries[i].lastSeenMs < oldestTime) {
            oldestTime = g_nearbyCanaries[i].lastSeenMs;
            oldest = i;
        }
    }
    return oldest;
}

static void addRssiReading(NearbyCanary* c, int8_t rssi) {
    c->rssiHistory[c->rssiIndex] = rssi;
    c->rssiIndex = (c->rssiIndex + 1) % NEARBY_RSSI_HISTORY;
    if (c->rssiCount < NEARBY_RSSI_HISTORY) c->rssiCount++;
}

static int8_t avgRssi(const NearbyCanary* c) {
    if (c->rssiCount == 0) return -100;
    int32_t sum = 0;
    for (uint8_t i = 0; i < c->rssiCount; i++) {
        sum += c->rssiHistory[i];
    }
    return (int8_t)(sum / c->rssiCount);
}

static int8_t currentRssi(const NearbyCanary* c) {
    if (c->rssiCount == 0) return -100;
    uint8_t idx = (c->rssiIndex == 0) ? NEARBY_RSSI_HISTORY - 1 : c->rssiIndex - 1;
    return c->rssiHistory[idx];
}

// Parse chirp manufacturer data from a Canary device
// Returns true if valid chirp data found
static bool parseChirpData(const uint8_t* data, size_t len, NearbyCanary* canary) {
    // Manufacturer data: company ID (2 bytes, stripped by NimBLE) + payload
    // We expect at least CHIRP_PAYLOAD_SIZE bytes after company ID
    if (len < CHIRP_PAYLOAD_SIZE + 2) return false;

    // Skip company ID (first 2 bytes)
    const uint8_t* payload = data + 2;

    // Validate company ID
    uint16_t companyId = data[0] | (data[1] << 8);
    if (companyId != CHIRP_COMPANY_ID) return false;

    canary->lastChirpType = (ChirpType)payload[0];
    memcpy(canary->chainHashPrefix, &payload[5], 8);

    // Device ID prefix: last 2 bytes → 4 hex chars
    snprintf(canary->deviceIdPrefix, 5, "%02x%02x", payload[13], payload[14]);

    return true;
}

// Parse the fleet-link presence beacon (type 0x10, 11-byte manufacturer blob)
// via the canonical shared parser (fleet_beacon.h). This is the direct,
// broker-free status channel: unlike the chirp it carries battery %, health %,
// liveness flags, and the low 16 bits of the chain height. Returns true and
// fills the peer's status on a well-formed beacon. The fp2 suffix it carries is
// the SAME 2 bytes the chirp puts at [15-16], so a chirp and a beacon from the
// one device resolve to the same roster slot.
static bool parseBeaconData(const uint8_t* data, size_t len, NearbyCanary* canary) {
    FleetBeaconFields f;
    if (!fleet_beacon_parse(data, len, &f)) return false;

    snprintf(canary->deviceIdPrefix, 5, "%02x%02x", f.fp_b0, f.fp_b1);
    canary->hasBeaconStatus = true;
    canary->batteryPct    = (int16_t)f.battery_pct;   // -1 when unknown
    canary->healthPct     = (int16_t)f.health_pct;    // -1 when unknown
    canary->beaconFlags   = f.flags;
    canary->chainHeightLo = f.chain_lo16;
    return true;
}

// ════════════════════════════════════════════════════════════════
// SCAN CALLBACKS
// ════════════════════════════════════════════════════════════════

class NearbyScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        bool isCanary = false;

        // Check if device advertises SecuraCV service UUID
        if (advertisedDevice->haveServiceUUID()) {
            if (advertisedDevice->isAdvertisingService(NimBLEUUID(SCV_SERVICE_UUID))) {
                isCanary = true;
            }
        }

        // Check manufacturer data for a fleet-link presence beacon (11-byte,
        // type 0x10 — rich status) or a chirp (17-byte — presence + chain).
        bool hasChirpData = false;
        bool hasBeaconData = false;
        NearbyCanary tempCanary;
        memset(&tempCanary, 0, sizeof(tempCanary));
        tempCanary.batteryPct = -1;   // unknown until a beacon says otherwise
        tempCanary.healthPct  = -1;

        if (advertisedDevice->haveManufacturerData()) {
            // Read the RAW bytes — a std::string preserves embedded NULs, which
            // an Arduino String(c_str()) would truncate at (the beacon's flags/
            // battery/version bytes are frequently 0x00).
            const std::string mfg = advertisedDevice->getManufacturerData();
            const uint8_t* mfgBytes = reinterpret_cast<const uint8_t*>(mfg.data());
            const size_t mfgLen = mfg.size();

            if (mfgLen == FLEET_BEACON_MFG_LEN ||
                mfgLen == FLEET_BEACON_MFG_V2_LEN) {
                hasBeaconData = parseBeaconData(mfgBytes, mfgLen, &tempCanary);
                if (hasBeaconData) isCanary = true;
            } else if (mfgLen >= (size_t)(CHIRP_PAYLOAD_SIZE + 2)) {
                hasChirpData = parseChirpData(mfgBytes, mfgLen, &tempCanary);
                if (hasChirpData) isCanary = true;
            }
        }

        if (xSemaphoreTake(g_nearbyMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (isCanary) {
                // Extract device ID prefix from name if available
                char prefix[5] = {0};
                if (hasChirpData || hasBeaconData) {
                    strncpy(prefix, tempCanary.deviceIdPrefix, 4);
                } else {
                    // Try to extract from BLE name "SCV-XXXX"
                    String name = advertisedDevice->getName().c_str();
                    if (name.startsWith(BLE_DEVICE_NAME_PREFIX) && name.length() >= 8) {
                        name.substring(4, 8).toCharArray(prefix, 5);
                    } else {
                        // No valid device ID prefix available — treat as non-Canary
                        // (We never use MAC addresses as identifiers for privacy)
                        g_nonCanaryCount++;
                        xSemaphoreGive(g_nearbyMutex);
                        return;
                    }
                }
                prefix[4] = '\0';

                int slot = findCanarySlot(prefix);
                bool isNew = (slot < 0);
                if (slot < 0) {
                    slot = findFreeSlot();
                    memset(&g_nearbyCanaries[slot], 0, sizeof(NearbyCanary));
                    strncpy(g_nearbyCanaries[slot].deviceIdPrefix, prefix, 4);
                    g_nearbyCanaries[slot].deviceIdPrefix[4] = '\0';
                    // Unknown until a beacon carries status — never show a false 0%.
                    g_nearbyCanaries[slot].batteryPct = -1;
                    g_nearbyCanaries[slot].healthPct  = -1;
                    g_nearbyCanaries[slot].active = true;
                    g_canariesDiscovered++;
                }

                addRssiReading(&g_nearbyCanaries[slot], (int8_t)advertisedDevice->getRSSI());
                g_nearbyCanaries[slot].lastSeenMs = millis();

                if (hasChirpData) {
                    g_nearbyCanaries[slot].lastChirpType = tempCanary.lastChirpType;
                    memcpy(g_nearbyCanaries[slot].chainHashPrefix, tempCanary.chainHashPrefix, 8);
                    g_chirpsReceived++;
                }

                if (hasBeaconData) {
                    // Rich status from the direct beacon. battery/health carry a
                    // -1 sentinel when the beacon marked them unknown — keep the
                    // last known value in that case (a status-less refresh must
                    // not wipe a good reading), mirroring the shared roster rule.
                    g_nearbyCanaries[slot].hasBeaconStatus = true;
                    if (tempCanary.batteryPct >= 0) g_nearbyCanaries[slot].batteryPct = tempCanary.batteryPct;
                    if (tempCanary.healthPct  >= 0) g_nearbyCanaries[slot].healthPct  = tempCanary.healthPct;
                    g_nearbyCanaries[slot].beaconFlags   = tempCanary.beaconFlags;
                    g_nearbyCanaries[slot].chainHeightLo = tempCanary.chainHeightLo;
                    g_beaconsReceived++;
                }

                if (isNew) {
                    Serial.printf("[BLE] Canary discovered: %s RSSI=%d\n", prefix, advertisedDevice->getRSSI());
                }
            } else {
                g_nonCanaryCount++;
            }
            xSemaphoreGive(g_nearbyMutex);
        }
    }

    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        g_scanActive = false;
    }
};

static NearbyScanCallbacks g_scanCallbacks;

// ════════════════════════════════════════════════════════════════
// EXPIRE OLD ENTRIES
// ════════════════════════════════════════════════════════════════

static void expireNearbyCanaries() {
    unsigned long now = millis();
    if (xSemaphoreTake(g_nearbyMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < NEARBY_MAX_CANARIES; i++) {
            if (g_nearbyCanaries[i].active) {
                if ((now - g_nearbyCanaries[i].lastSeenMs) > (NEARBY_EXPIRY_SEC * 1000UL)) {
                    Serial.printf("[BLE] Canary lost: %s\n", g_nearbyCanaries[i].deviceIdPrefix);
                    g_nearbyCanaries[i].active = false;
                }
            }
        }
        xSemaphoreGive(g_nearbyMutex);
    }
}

// ════════════════════════════════════════════════════════════════
// SCAN TASK (FreeRTOS)
// ════════════════════════════════════════════════════════════════

static void bleNearbyTask(void* param) {
    for (;;) {
        if (g_pBLEScan != nullptr) {
            g_scanActive = true;
            g_nonCanaryCount = 0;  // Reset non-canary count per scan cycle

            unsigned long scanStart = millis();
            // Start scan (blocking for duration, results handled in callback)
            g_pBLEScan->getResults(BLE_SCAN_DURATION_SEC * 1000, false);
            g_lastScanDurationMs = millis() - scanStart;
            g_lastScanMs = millis();

            // Clean up scan results to free memory
            g_pBLEScan->clearResults();

            // Expire old entries
            expireNearbyCanaries();

            g_scanActive = false;
        }
        vTaskDelay(pdMS_TO_TICKS(BLE_SCAN_PERIOD_SEC * 1000));
    }
}

// ════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════

static bool init() {
    g_nearbyMutex = xSemaphoreCreateMutex();
    if (!g_nearbyMutex) {
        Serial.println("[BLE] Failed to create nearby mutex");
        return false;
    }

    memset(g_nearbyCanaries, 0, sizeof(g_nearbyCanaries));

    g_pBLEScan = NimBLEDevice::getScan();
    if (!g_pBLEScan) {
        Serial.println("[BLE] Failed to get BLE scan instance");
        return false;
    }

    g_pBLEScan->setScanCallbacks(&g_scanCallbacks, false);
    g_pBLEScan->setActiveScan(BLE_ACTIVE_SCAN);
    g_pBLEScan->setInterval(BLE_SCAN_INTERVAL);
    g_pBLEScan->setWindow(BLE_SCAN_WINDOW);

    return true;
}

static bool startTask() {
    // On dual-core (ESP32-S3): pin to core 0 (WiFi/BLE core) to keep core 1 free
    // On single-core (ESP32-C3): use xTaskCreate (no core pinning needed)
    #if HW_CPU_CORES > 1
    BaseType_t result = xTaskCreatePinnedToCore(
        bleNearbyTask,
        "ble_nearby",
        NEARBY_SCAN_TASK_STACK,
        nullptr,
        NEARBY_SCAN_TASK_PRIORITY,
        &g_scanTaskHandle,
        0  // Run on core 0 (WiFi/BLE core)
    );
    #else
    BaseType_t result = xTaskCreate(
        bleNearbyTask,
        "ble_nearby",
        NEARBY_SCAN_TASK_STACK,
        nullptr,
        NEARBY_SCAN_TASK_PRIORITY,
        &g_scanTaskHandle
    );
    #endif

    if (result != pdPASS) {
        Serial.println("[BLE] Failed to create nearby scan task");
        return false;
    }

    Serial.println("[BLE] Nearby scan task started");
    return true;
}

static void stop() {
    if (g_scanTaskHandle) {
        vTaskDelete(g_scanTaskHandle);
        g_scanTaskHandle = nullptr;
    }
    if (g_pBLEScan) {
        g_pBLEScan->stop();
    }
    g_scanActive = false;
}

static int activeCanaryCount() {
    int count = 0;
    if (xSemaphoreTake(g_nearbyMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < NEARBY_MAX_CANARIES; i++) {
            if (g_nearbyCanaries[i].active) count++;
        }
        xSemaphoreGive(g_nearbyMutex);
    }
    return count;
}

static uint16_t getNonCanaryCount() {
    return g_nonCanaryCount;
}

static bool isScanActive() {
    return g_scanActive;
}

static uint32_t getChirpsReceived() {
    return g_chirpsReceived;
}

static uint32_t getCanariesDiscovered() {
    return g_canariesDiscovered;
}

static uint32_t getBeaconsReceived() {
    return g_beaconsReceived;
}

// Build JSON for /api/nearby endpoint
// Caller must hold or acquire mutex
static String buildNearbyJson() {
    String json = "{\"canaries\":[";
    bool first = true;
    unsigned long now = millis();

    if (xSemaphoreTake(g_nearbyMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < NEARBY_MAX_CANARIES; i++) {
            if (!g_nearbyCanaries[i].active) continue;

            if (!first) json += ",";
            first = false;

            int8_t cur = currentRssi(&g_nearbyCanaries[i]);
            int8_t avg = avgRssi(&g_nearbyCanaries[i]);
            uint32_t secsAgo = (now - g_nearbyCanaries[i].lastSeenMs) / 1000;

            // Chain hash prefix as hex
            char hashHex[17];
            for (int j = 0; j < 8; j++) {
                snprintf(hashHex + j * 2, 3, "%02x", g_nearbyCanaries[i].chainHashPrefix[j]);
            }
            hashHex[16] = '\0';

            json += "{\"device_id_prefix\":\"";
            json += g_nearbyCanaries[i].deviceIdPrefix;
            json += "\",\"rssi_current\":";
            json += String(cur);
            json += ",\"rssi_avg\":";
            json += String(avg);
            json += ",\"rssi_quality\":\"";
            json += rssiQuality(cur);
            json += "\",\"last_seen_sec_ago\":";
            json += String(secsAgo);
            json += ",\"last_chirp_type\":\"";
            json += chirpTypeName(g_nearbyCanaries[i].lastChirpType);
            json += "\",\"chain_hash_prefix\":\"";
            json += hashHex;
            json += "\"";

            // Direct fleet-link beacon status (present only once a type-0x10
            // beacon has been decoded for this peer). battery/health are null
            // when the beacon marked them unknown (0xFF on the wire).
            if (g_nearbyCanaries[i].hasBeaconStatus) {
                const uint8_t fl = g_nearbyCanaries[i].beaconFlags;
                json += ",\"beacon\":{\"battery_pct\":";
                json += (g_nearbyCanaries[i].batteryPct >= 0) ? String(g_nearbyCanaries[i].batteryPct) : String("null");
                json += ",\"health_pct\":";
                json += (g_nearbyCanaries[i].healthPct >= 0) ? String(g_nearbyCanaries[i].healthPct) : String("null");
                json += ",\"chain_height_lo\":";
                json += String(g_nearbyCanaries[i].chainHeightLo);
                json += ",\"tamper\":";      json += (fl & FLEET_BEACON_FLAG_TAMPER)      ? "true" : "false";
                json += ",\"mic_muted\":";   json += (fl & FLEET_BEACON_FLAG_MIC_MUTED)   ? "true" : "false";
                json += ",\"degraded\":";    json += (fl & FLEET_BEACON_FLAG_DEGRADED)    ? "true" : "false";
                json += ",\"on_wifi\":";     json += (fl & FLEET_BEACON_FLAG_ON_WIFI_STA) ? "true" : "false";
                json += ",\"alert\":";       json += (fl & FLEET_BEACON_FLAG_ALERT)       ? "true" : "false";
                json += "}";
            }

            json += "}";
        }
        xSemaphoreGive(g_nearbyMutex);
    }

    json += "],\"non_canary_device_count\":";
    json += String(g_nonCanaryCount);
    json += ",\"scan_active\":";
    json += g_scanActive ? "true" : "false";
    json += ",\"last_scan_ms\":";
    json += String(g_lastScanDurationMs);
    json += ",\"beacons_received\":";
    json += String(g_beaconsReceived);
    json += "}";

    return json;
}

} // namespace ble_nearby

#else // !FEATURE_BLE || !FEATURE_BLE_NEARBY

namespace ble_nearby {
    static inline bool init() { return false; }
    static inline bool startTask() { return false; }
    static inline void stop() {}
    static inline int activeCanaryCount() { return 0; }
    static inline uint16_t getNonCanaryCount() { return 0; }
    static inline bool isScanActive() { return false; }
    static inline uint32_t getChirpsReceived() { return 0; }
    static inline uint32_t getBeaconsReceived() { return 0; }
    static inline uint32_t getCanariesDiscovered() { return 0; }
    static inline String buildNearbyJson() { return "{\"canaries\":[],\"non_canary_device_count\":0,\"scan_active\":false,\"last_scan_ms\":0}"; }
}

#endif // FEATURE_BLE && FEATURE_BLE_NEARBY

#endif // BLE_NEARBY_H
