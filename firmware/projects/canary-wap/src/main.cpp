/**
 * @file main.cpp
 * @brief SecuraCV Canary WAP - Main Application Entry Point
 *
 * This is the main application file for the Canary WAP witness device.
 * It initializes all subsystems and runs the main event loop.
 *
 * Architecture:
 *   - Board pins from: boards/xiao-esp32s3-sense/pins/
 *   - Configuration from: configs/canary-wap/default/
 *   - Common modules from: common/
 */

#include <Arduino.h>

// Board-specific pin definitions
#include "pins.h"

// Configuration
#include "config.h"

// Core modules
#include "core/types.h"
#include "core/log.h"
#include "core/version.h"

// HAL interfaces
#include "hal/hal.h"

// Feature modules (conditionally compiled)
#if FEATURE_GNSS
#include "gnss/gnss_parser.h"
#endif

#if FEATURE_SD_STORAGE
#include "storage/storage.h"
#endif

#if FEATURE_WIFI_AP || FEATURE_WIFI_STA
#include "hal/hal_wifi.h"
#endif

#if FEATURE_HTTP_SERVER
#include "web/http_server.h"
#include "web/web_ui.h"
#endif

#if FEATURE_MESH_NETWORK
#include "network/mesh_network.h"
#endif

#if FEATURE_BLUETOOTH
#include "bluetooth/bluetooth_mgr.h"
#endif

#if FEATURE_RF_PRESENCE
#include "rf_presence/rf_presence.h"
#endif

#if FEATURE_CHIRP
#include "chirp/chirp_channel.h"
#endif

#include "witness/witness_chain.h"

// ============================================================================
// APPLICATION STATE
// ============================================================================

static const char* LOG_TAG = "APP";

// System state
static witness_chain_t g_witness_chain;
static system_health_t g_health;
static bool g_initialized = false;

#if FEATURE_GNSS
static gnss_parser_t g_gnss_parser;
#endif

// Timing
static uint32_t g_last_record_ms = 0;
static uint32_t g_last_verify_ms = 0;
static uint32_t g_last_health_log_ms = 0;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void app_init_hardware(void);
static void app_init_storage(void);
static void app_init_network(void);
static void app_init_witness(void);
static void app_process_gnss(void);
static void app_process_records(void);
static void app_process_health(void);

// ============================================================================
// SETUP
// ============================================================================

void setup() {
    // Initialize serial for debugging
    Serial.begin(CONFIG_SERIAL_BAUD);

    // Wait for serial connection (development only)
    #if CORE_DEBUG_LEVEL > 0
    uint32_t wait_start = millis();
    while (!Serial && (millis() - wait_start) < CONFIG_SERIAL_CDC_WAIT_MS) {
        delay(10);
    }
    #endif

    LOG_I("SecuraCV Canary WAP v%s starting...", FW_VERSION_STRING);
    LOG_I("Board: %s", BOARD_NAME);

    // Initialize HAL
    if (hal_init() != HAL_OK) {
        LOG_E("HAL initialization failed!");
        return;
    }

    // Initialize subsystems
    app_init_hardware();
    app_init_storage();
    app_init_witness();
    app_init_network();

    g_initialized = true;
    g_health.uptime_sec = 0;

    LOG_I("Initialization complete. Device ID: %s",
          witness_chain_device_id(&g_witness_chain));
}

// ============================================================================
// MAIN LOOP
// ============================================================================

void loop() {
    if (!g_initialized) {
        delay(1000);
        return;
    }

    uint32_t now = millis();

    // Update uptime
    g_health.uptime_sec = now / 1000;
    g_health.free_heap = hal_free_heap();
    if (g_health.free_heap < g_health.min_heap || g_health.min_heap == 0) {
        g_health.min_heap = g_health.free_heap;
    }

    // Process GNSS data
    #if FEATURE_GNSS
    app_process_gnss();
    #endif

    // Process witness records
    app_process_records();

    // Process health logging
    app_process_health();

    // Process mesh network
    #if FEATURE_MESH_NETWORK
    mesh_process();
    #endif

    // Process BLE
    #if FEATURE_BLUETOOTH
    ble_mgr_process();
    #endif

    // Process RF presence
    #if FEATURE_RF_PRESENCE
    rf_presence_process();
    #endif

    // Process chirp channel
    #if FEATURE_CHIRP
    chirp_process();
    #endif

    // Feed watchdog
    #if FEATURE_WATCHDOG
    hal_watchdog_feed();
    #endif

    // Small delay to prevent tight loop
    delay(10);
}

// ============================================================================
// INITIALIZATION FUNCTIONS
// ============================================================================

static void app_init_hardware() {
    LOG_I("Initializing hardware...");

    // Initialize watchdog
    #if FEATURE_WATCHDOG
    if (hal_watchdog_init(CONFIG_WATCHDOG_TIMEOUT_SEC) == HAL_OK) {
        LOG_I("Watchdog enabled (%d sec timeout)", CONFIG_WATCHDOG_TIMEOUT_SEC);
    }
    #endif

    // Initialize GNSS UART
    #if FEATURE_GNSS
    uart_config_t gnss_uart_cfg = UART_CONFIG_DEFAULT;
    gnss_uart_cfg.baud = CONFIG_GNSS_BAUD;
    gnss_uart_cfg.tx_pin = GNSS_PIN_TX;
    gnss_uart_cfg.rx_pin = GNSS_PIN_RX;
    if (hal_uart_init(1, &gnss_uart_cfg) == 0) {
        LOG_I("GNSS UART initialized at %d baud", CONFIG_GNSS_BAUD);
        gnss_parser_init(&g_gnss_parser);
    }
    #endif

    LOG_I("Hardware initialization complete");
}

static void app_init_storage() {
    LOG_I("Initializing storage...");

    // Initialize NVS
    if (nvs_storage_init() == RESULT_OK) {
        LOG_I("NVS storage initialized");
    }

    // Initialize SD card
    #if FEATURE_SD_STORAGE
    sd_storage_config_t sd_cfg = {
        .cs_pin = SD_PIN_CS,
        .sck_pin = SD_PIN_SCK,
        .miso_pin = SD_PIN_MISO,
        .mosi_pin = SD_PIN_MOSI,
        .freq_hz = CONFIG_SD_SPI_FREQ_FAST,
    };

    if (sd_storage_init(&sd_cfg) == RESULT_OK) {
        LOG_I("SD card mounted");
        g_health.sd_healthy = true;

        // Initialize witness storage
        witness_storage_init();
        log_storage_init();
    } else {
        LOG_W("SD card not available");
        g_health.sd_healthy = false;
    }
    #endif

    LOG_I("Storage initialization complete");
}

static void app_init_witness() {
    LOG_I("Initializing witness chain...");

    witness_chain_config_t cfg = WITNESS_CHAIN_CONFIG_DEFAULT;
    cfg.device_id_prefix = CONFIG_DEVICE_ID_PREFIX;
    cfg.time_bucket_ms = CONFIG_TIME_BUCKET_MS;
    cfg.persist_interval = CONFIG_SD_PERSIST_INTERVAL;

    if (witness_chain_init(&g_witness_chain, &cfg) == RESULT_OK) {
        LOG_I("Witness chain initialized");
        LOG_I("  Device ID: %s", witness_chain_device_id(&g_witness_chain));
        LOG_I("  Sequence: %u", witness_chain_sequence(&g_witness_chain));
        LOG_I("  Boot count: %u", witness_chain_boot_count(&g_witness_chain));

        // Create boot attestation record
        witness_record_t boot_record;
        if (witness_chain_create_boot_attestation(&g_witness_chain, &boot_record) == RESULT_OK) {
            LOG_I("Boot attestation created (seq=%u)", boot_record.sequence);
            g_health.records_created++;
        }

        g_health.crypto_healthy = true;
    } else {
        LOG_E("Witness chain initialization failed!");
        g_health.crypto_healthy = false;
    }
}

static void app_init_network() {
    LOG_I("Initializing network...");

    // Initialize WiFi
    #if FEATURE_WIFI_AP
    if (hal_wifi_init(WIFI_MODE_APSTA) == 0) {
        // Start access point
        wifi_config_t ap_cfg = {
            .channel = CONFIG_AP_CHANNEL,
            .hidden = CONFIG_AP_HIDDEN,
            .max_connections = CONFIG_AP_MAX_CLIENTS,
            .auth = WIFI_AUTH_WPA2_PSK,
        };
        snprintf(ap_cfg.ssid, sizeof(ap_cfg.ssid), "%s%s",
                 CONFIG_AP_SSID_PREFIX,
                 witness_chain_device_id(&g_witness_chain) + strlen(CONFIG_DEVICE_ID_PREFIX));
        strncpy(ap_cfg.password, CONFIG_AP_PASSWORD_DEFAULT, sizeof(ap_cfg.password));

        if (hal_wifi_start_ap(&ap_cfg) == 0) {
            LOG_I("WiFi AP started: %s", ap_cfg.ssid);
            g_health.wifi_active = true;
        }
    }
    #endif

    // Initialize HTTP server
    #if FEATURE_HTTP_SERVER
    http_server_config_t http_cfg = HTTP_SERVER_CONFIG_DEFAULT;
    http_cfg.port = CONFIG_HTTP_PORT;
    http_cfg.max_connections = CONFIG_HTTP_MAX_CONNECTIONS;

    if (http_server_init(&http_cfg) == RESULT_OK) {
        http_register_standard_api();
        web_ui_register_routes();

        if (http_server_start() == RESULT_OK) {
            LOG_I("HTTP server started on port %d", CONFIG_HTTP_PORT);
        }
    }
    #endif

    // Initialize mesh network
    #if FEATURE_MESH_NETWORK
    mesh_config_t mesh_cfg = {
        .device_id = witness_chain_device_id(&g_witness_chain),
        .private_key = g_witness_chain.private_key,
        .public_key = g_witness_chain.public_key,
        .opera_id = NULL,  // Will be loaded from NVS
        .channel = CONFIG_MESH_CHANNEL,
        .auto_connect = CONFIG_MESH_AUTO_CONNECT,
        .discoverable = CONFIG_MESH_DISCOVERABLE,
        .heartbeat_interval_ms = CONFIG_MESH_HEARTBEAT_MS,
        .discovery_interval_ms = CONFIG_MESH_DISCOVERY_MS,
        .msg_callback = NULL,
        .peer_callback = NULL,
        .user_data = NULL,
    };

    if (mesh_init(&mesh_cfg) == RESULT_OK) {
        mesh_start();
        LOG_I("Mesh network started");
        g_health.mesh_active = true;
    }
    #endif

    // Initialize Bluetooth
    #if FEATURE_BLUETOOTH
    ble_config_t ble_cfg = BLE_CONFIG_DEFAULT;
    ble_cfg.device_name = CONFIG_BLE_DEVICE_NAME;
    ble_cfg.device_id = witness_chain_device_id(&g_witness_chain);
    ble_cfg.public_key = g_witness_chain.public_key;
    ble_cfg.tx_power = CONFIG_BLE_TX_POWER;
    ble_cfg.pairable = CONFIG_BLE_PAIRABLE;

    if (ble_mgr_init(&ble_cfg) == RESULT_OK) {
        ble_mgr_start_advertising();
        LOG_I("Bluetooth started");
        g_health.ble_active = true;
    }
    #endif

    // Initialize RF presence
    #if FEATURE_RF_PRESENCE
    rf_presence_config_t rf_cfg = RF_PRESENCE_CONFIG_DEFAULT;
    rf_cfg.mode = (rf_detection_mode_t)CONFIG_RF_MODE;
    rf_cfg.sample_interval_ms = CONFIG_RF_SAMPLE_INTERVAL_MS;
    rf_cfg.presence_threshold = CONFIG_RF_PRESENCE_THRESHOLD;
    rf_cfg.crowd_threshold = CONFIG_RF_CROWD_THRESHOLD;

    if (rf_presence_init(&rf_cfg) == RESULT_OK) {
        rf_presence_start();
        LOG_I("RF presence detection started");
    }
    #endif

    // Initialize chirp channel
    #if FEATURE_CHIRP
    chirp_config_t chirp_cfg = CHIRP_CONFIG_DEFAULT;
    chirp_cfg.auto_relay = CONFIG_CHIRP_AUTO_RELAY;
    chirp_cfg.min_urgency = (chirp_urgency_t)CONFIG_CHIRP_MIN_URGENCY;

    if (chirp_init(&chirp_cfg) == RESULT_OK) {
        LOG_I("Chirp channel initialized");
        g_health.chirp_active = true;
    }
    #endif

    LOG_I("Network initialization complete");
}

// ============================================================================
// PROCESSING FUNCTIONS
// ============================================================================

#if FEATURE_GNSS
static void app_process_gnss() {
    // Read available GNSS data
    uint8_t buf[128];
    int len = hal_uart_read(1, buf, sizeof(buf), 0);
    if (len > 0) {
        gnss_parser_process(&g_gnss_parser, buf, len);
        g_health.gps_sentences += len;
    }

    // Update GPS health
    const gnss_fix_t* fix = gnss_parser_get_fix(&g_gnss_parser);
    g_health.gps_healthy = fix->valid;
}
#endif

static void app_process_records() {
    uint32_t now = millis();

    // Create periodic witness records
    if (now - g_last_record_ms >= CONFIG_RECORD_INTERVAL_MS) {
        g_last_record_ms = now;

        // Build witness event payload
        // (In production, this would include GPS data, state, etc.)
        witness_record_t record;
        uint8_t payload[64];
        size_t payload_len = 0;

        // Add timestamp and state to payload
        // ... payload building code ...

        if (witness_chain_create_record(&g_witness_chain, RECORD_TYPE_WITNESS_EVENT,
                                        payload, payload_len, &record) == RESULT_OK) {
            g_health.records_created++;

            // Store to SD if available
            #if FEATURE_SD_STORAGE
            if (g_health.sd_healthy) {
                witness_storage_append(&record, payload, payload_len);
                g_health.sd_writes++;
            }
            #endif

            // Broadcast to mesh
            #if FEATURE_MESH_NETWORK
            if (g_health.mesh_active) {
                mesh_broadcast_witness(&record);
                g_health.mesh_messages_sent++;
            }
            #endif
        }
    }

    // Periodic self-verification
    if (now - g_last_verify_ms >= (CONFIG_VERIFY_INTERVAL_SEC * 1000)) {
        g_last_verify_ms = now;

        // Run crypto self-test
        if (hal_crypto_self_test() == 0) {
            g_health.records_verified++;
        } else {
            g_health.verify_failures++;
            LOG_E("Crypto self-test failed!");
        }
    }
}

static void app_process_health() {
    uint32_t now = millis();

    // Log health metrics periodically
    if (now - g_last_health_log_ms >= 60000) {  // Every minute
        g_last_health_log_ms = now;

        LOG_I("Health: uptime=%us heap=%u/%u records=%u verified=%u",
              g_health.uptime_sec, g_health.free_heap, g_health.min_heap,
              g_health.records_created, g_health.records_verified);

        #if FEATURE_SD_STORAGE
        if (g_health.sd_healthy) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Health: records=%u heap=%u",
                     g_health.records_created, g_health.free_heap);
            log_storage_append(LOG_LEVEL_INFO, "HEALTH", msg);
            g_health.logs_stored++;
        }
        #endif
    }
}
