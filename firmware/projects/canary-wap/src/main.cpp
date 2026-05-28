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
#include "boot/boot_banner.h"

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

#if FEATURE_BLUETOOTH
#include "bluetooth/ble_debug_beacon.h"
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
#if FEATURE_BLUETOOTH
static uint32_t g_last_debug_beacon_ms = 0;
static bool g_ble_debug_active = false;
#endif

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

// Boot sequence serial output
static boot_info_t app_collect_boot_info(void);
static void app_print_scene_features(void);
static void app_print_scene_protocol(void);
static void app_print_scene_network(const char* ssid);
static void app_print_scene_chain(void);
static void app_print_scene_guide(const char* ssid);

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

    boot_info_t bi = app_collect_boot_info();
    boot_scene_banner(&bi);

    // Initialize HAL
    if (hal_init() != HAL_OK) {
        LOG_E("HAL initialization failed!");
        return;
    }

    // Refresh hardware stats now that HAL is up
    bi.psram_found    = psramFound();
    bi.psram_total_kb = (uint32_t)(ESP.getPsramSize() / 1024);
    bi.psram_free_kb  = (uint32_t)(ESP.getFreePsram() / 1024);
    bi.heap_free_kb   = (uint32_t)(ESP.getFreeHeap() / 1024);

    boot_scene_hardware(&bi);
    app_print_scene_features();
    app_print_scene_protocol();

    // Initialize subsystems
    app_init_hardware();
    app_init_storage();
    app_init_witness();

    app_print_scene_chain();

    // Detect debug beacon activation BEFORE network init so BLE starts
    // with the correct device name (SCV-DBG-XXXX vs SecuraCV-Canary)
    #if FEATURE_BLUETOOTH
    {
        #if FEATURE_BLE_DEBUG
        g_ble_debug_active = true;
        LOG_I("BLE debug beacon enabled (compile-time)");
        #else
        // Runtime activation: hold BOOT button (GPIO 0) during startup
        pinMode(0, INPUT_PULLUP);
        if (digitalRead(0) == LOW) {
            uint32_t hold_start = millis();
            while (digitalRead(0) == LOW &&
                   (millis() - hold_start) < CONFIG_BOOT_BUTTON_HOLD_MS) {
                delay(10);
            }
            if ((millis() - hold_start) >= CONFIG_BOOT_BUTTON_HOLD_MS) {
                g_ble_debug_active = true;
                LOG_I("BLE debug beacon enabled (button hold)");
            }
        }
        #endif
    }
    #endif

    app_init_network();

    // Build the SSID for display in serial scenes
    char ssid_buf[64];
    const char* dev_id = witness_chain_device_id(&g_witness_chain);
    size_t prefix_len = strlen(CONFIG_DEVICE_ID_PREFIX);
    const char* suffix = (strncmp(dev_id, CONFIG_DEVICE_ID_PREFIX, prefix_len) == 0)
                         ? dev_id + prefix_len : dev_id;
    snprintf(ssid_buf, sizeof(ssid_buf), "%s%s", CONFIG_AP_SSID_PREFIX, suffix);

    if (g_health.wifi_active) {
        app_print_scene_network(ssid_buf);
    }

    // Initialize debug beacon after BLE is running
    #if FEATURE_BLUETOOTH
    if (g_ble_debug_active) {
        char fp_hex[5];
        if (g_health.crypto_healthy) {
            snprintf(fp_hex, sizeof(fp_hex), "%02x%02x",
                     g_witness_chain.fingerprint[6],
                     g_witness_chain.fingerprint[7]);
        } else {
            strcpy(fp_hex, "0000");
        }
        ble_debug_beacon_init(fp_hex);
    }
    #endif

    g_initialized = true;
    g_health.uptime_sec = 0;

    boot_scene_ready(
        "It will now create a signed witness record",
        "every second and store it to the SD card.",
        "Nobody can alter these records after the fact."
    );
    app_print_scene_guide(ssid_buf);
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

    // Update BLE debug beacon
    #if FEATURE_BLUETOOTH
    if (g_ble_debug_active && (now - g_last_debug_beacon_ms >= CONFIG_BLE_DEBUG_UPDATE_MS)) {
        g_last_debug_beacon_ms = now;
        ble_debug_beacon_update(&g_health);
    }
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

// ============================================================================
// BOOT INFO COLLECTION
// ============================================================================

static String s_mac_cache;

static boot_info_t app_collect_boot_info() {
    s_mac_cache = WiFi.macAddress();

    boot_info_t bi = {};
    bi.product_name  = "SecuraCV Canary WAP";
    bi.fw_version    = FW_VERSION_STRING;
    bi.build_date    = FW_BUILD_DATE;
    bi.build_time    = FW_BUILD_TIME;
    bi.device_type   = CONFIG_DEVICE_TYPE;
    bi.model         = CONFIG_MODEL;
    bi.mac_address   = s_mac_cache.c_str();
    bi.board_name    = BOARD_NAME;
    bi.chip_model    = ESP.getChipModel();
    bi.chip_revision = (uint8_t)ESP.getChipRevision();
    bi.cpu_freq_mhz  = (uint16_t)ESP.getCpuFreqMHz();
    bi.cpu_cores     = (uint8_t)ESP.getChipCores();
    bi.flash_mb      = (uint32_t)(ESP.getFlashChipSize() / (1024 * 1024));
    bi.psram_found   = psramFound();
    bi.psram_total_kb = (uint32_t)(ESP.getPsramSize() / 1024);
    bi.psram_free_kb  = (uint32_t)(ESP.getFreePsram() / 1024);
    bi.heap_free_kb   = (uint32_t)(ESP.getFreeHeap() / 1024);
    bi.sdk_version    = ESP.getSdkVersion();
    return bi;
}

static void app_print_scene_features() {
    boot_line("                  ,_,");
    boot_line("            ___  (o.o)     What can I do?");
    boot_line("           | . |/ /");
    boot_line("           | . |          Each + is a feature that's");
    boot_line("           |___|          turned on in this build.");
    boot_separator();

    #if FEATURE_GNSS
    boot_feature(true,  "GPS/GNSS",    "knows where it is on Earth");
    #else
    boot_feature(false, "GPS/GNSS",    NULL);
    #endif
    #if FEATURE_WIFI_AP
    boot_feature(true,  "WiFi AP/STA", "creates its own WiFi + joins yours");
    #else
    boot_feature(false, "WiFi AP/STA", NULL);
    #endif
    #if FEATURE_SD_STORAGE
    boot_feature(true,  "SD Storage",  "saves records to a memory card");
    #else
    boot_feature(false, "SD Storage",  NULL);
    #endif
    #if FEATURE_HTTP_SERVER
    boot_feature(true,  "HTTP Server", "runs a web dashboard you can visit");
    #else
    boot_feature(false, "HTTP Server", NULL);
    #endif
    #if FEATURE_CAMERA_PEEK
    boot_feature(true,  "Camera",      "live preview for aiming the device");
    #else
    boot_feature(false, "Camera",      NULL);
    #endif
    #if FEATURE_MESH_NETWORK
    boot_feature(true,  "Opera Mesh",  "talks to other canaries nearby");
    #else
    boot_feature(false, "Opera Mesh",  NULL);
    #endif
    #if FEATURE_BLUETOOTH
    boot_feature(true,  "Bluetooth",   "pairs with your phone for setup");
    #else
    boot_feature(false, "Bluetooth",   NULL);
    #endif
    #if FEATURE_RF_PRESENCE
    boot_feature(true,  "RF Presence", "detects people nearby using radio");
    #else
    boot_feature(false, "RF Presence", NULL);
    #endif
    #if FEATURE_CHIRP
    boot_feature(true,  "Chirp",       "relays alerts from the community");
    #else
    boot_feature(false, "Chirp",       NULL);
    #endif
    #if FEATURE_WATCHDOG
    boot_kvf("+ Watchdog", "%ds auto-restarts if something freezes", CONFIG_WATCHDOG_TIMEOUT_SEC);
    #else
    boot_feature(false, "Watchdog",    NULL);
    #endif

    boot_blank();
    boot_line("    Timing: how often the canary creates records");
    boot_separator();
    boot_kvf("Record rate", "every %u ms", CONFIG_RECORD_INTERVAL_MS);
    boot_note("(creates 1 signed witness record per second)");
    boot_kvf("Time bucket", "%u ms", CONFIG_TIME_BUCKET_MS);
    boot_note("(rounds timestamps so exact times stay private)");
    boot_kvf("Self-verify", "every %u seconds", CONFIG_VERIFY_INTERVAL_SEC);
    boot_note("(the canary checks its own math is correct)");
    boot_kvf("SD persist",  "every %u records", CONFIG_SD_PERSIST_INTERVAL);
    boot_note("(saves a batch to the SD card at once)");
    boot_blank();
}

static void app_print_scene_protocol() {
    boot_line("              ,_,");
    boot_line("             (o.o)         Setting up the locks...");
    boot_line("              |#|");
    boot_line("             [###]         Every record gets signed");
    boot_line("              | |          so it can't be forged.");
    boot_separator();
    boot_kv("Witness",   PWK_PROTOCOL_VERSION);
    boot_note("(the Privacy Witness Kernel protocol version)");
    boot_kv("Chain",     CHAIN_ALGORITHM);
    boot_note("(how records are linked — like a chain of");
    boot_note(" paper clips where removing one breaks all)");
    boot_kv("Signature", SIGNATURE_ALGORITHM);
    boot_note("(the math that proves a record is genuine,");
    boot_note(" like a wax seal on a letter)");
    boot_kv("Ruleset",   RULESET_ID);
    boot_note("(the set of rules this device follows)");
    boot_blank();
}

static void app_print_scene_chain() {
    boot_line("              ,_,");
    boot_line("             (o.o)         Resuming the witness chain...");
    boot_line("              |=|");
    boot_line("             [===]         Each record links to the last,");
    boot_line("              |=|          like pages in a sealed book.");
    boot_separator();
    boot_kv("Device", witness_chain_device_id(&g_witness_chain));
    boot_kvf("Sequence", "%u  (next record number)", witness_chain_sequence(&g_witness_chain));
    boot_kvf("Boot",     "#%u  (times this device has started)", witness_chain_boot_count(&g_witness_chain));
    boot_kv("Integrity", g_health.crypto_healthy ? "verified" : "check required");
    if (g_health.crypto_healthy) {
        boot_kvf("Key", "%02x%02x%02x%02x...  (Ed25519 public key prefix)",
                 g_witness_chain.fingerprint[0],
                 g_witness_chain.fingerprint[1],
                 g_witness_chain.fingerprint[2],
                 g_witness_chain.fingerprint[3]);
    }
    boot_blank();
}

static void app_print_scene_network(const char* ssid) {
    boot_line("              ,_,  ))");
    boot_line("             (o.o)  ))     Broadcasting...");
    boot_line("              | |");
    boot_line("              | |          Your canary is now a WiFi");
    boot_line("              d b          hotspot you can connect to.");
    boot_separator();
    boot_kv("WiFi name", ssid);
    boot_kv("Password",  CONFIG_AP_PASSWORD_DEFAULT);
    boot_kvf("Channel",  "%d  (WiFi radio channel, up to %d devices at once)",
             CONFIG_AP_CHANNEL, CONFIG_AP_MAX_CLIENTS);
    boot_kv("Dashboard", "http://canary.local");
    boot_note("(type this in your browser to see the dashboard)");
    boot_kvf("Direct IP", "http://%s", WiFi.softAPIP().toString().c_str());
    boot_note("(use this if canary.local doesn't work)");
    boot_kvf("HTTP port", "%d", CONFIG_HTTP_PORT);
    boot_kv("mDNS",       "canary.local  (service: _securacv._tcp)");
    boot_note("(mDNS lets your device find the canary by name");
    boot_note(" instead of remembering a number like 192.168.4.1)");
    #if FEATURE_BLUETOOTH
    boot_kv("BLE name", CONFIG_BLE_DEVICE_NAME);
    boot_note("(how the canary appears on Bluetooth scans)");
    #endif
    #if FEATURE_GNSS
    boot_kvf("GNSS baud", "%u  (speed the GPS module talks at)", CONFIG_GNSS_BAUD);
    #endif
    boot_blank();
}

static void app_print_scene_guide(const char* ssid) {
    boot_line("              ,_,");
    boot_line("             (o.o) !       How to connect:");
    boot_line("              |>|");
    boot_line("              | |");
    boot_separator();
    boot_blank();
    boot_line("    1. On your phone or laptop, join the WiFi");
    boot_linef("       network called \"%s\"", ssid);
    boot_linef("       and enter the password: %s", CONFIG_AP_PASSWORD_DEFAULT);
    boot_blank();
    boot_line("    2. Open a web browser and go to:");
    boot_line("       http://canary.local");
    boot_linef("       (or try http://%s)", WiFi.softAPIP().toString().c_str());
    boot_blank();
    boot_line("    3. From the dashboard you can:");
    boot_line("       Timeline  - see the witness record history");
    boot_line("       Peek      - aim the camera");
    boot_line("       Sensing   - see who's nearby (via RF)");
    boot_line("       Settings  - connect to your home WiFi");
    boot_blank();
    boot_line("    REST API \xe2\x80\x94 for developers, scripts, and tinkerers:");
    boot_line("    (These are web addresses you can visit or call");
    boot_line("     from code. Add them after http://canary.local)");
    boot_separator();
    boot_line("    GET  /api/status          is the device healthy?");
    boot_line("    GET  /api/chain           latest witness chain info");
    boot_line("    GET  /api/logs            recent event log");
    boot_line("    POST /api/export          download all signed records");
    boot_line("    GET  /api/wifi/scan       see nearby WiFi networks");
    boot_line("    POST /api/wifi/connect    join your home WiFi");
    boot_line("    GET  /api/peek/stream     live camera video feed");
    boot_line("    GET  /api/peek/snapshot   take one photo");
    boot_line("    GET  /api/sensing         who's nearby? (RF data)");
    boot_line("    GET  /api/diagnostics     everything about this device");
    boot_line("    GET  /api/selftest        run a hardware check");
    boot_line("    POST /api/reboot          restart the canary");
    boot_blank();
    boot_line("    Serial monitor \xe2\x80\x94 what you're reading right now:");
    boot_separator();
    boot_kvf("Baud rate", "%u  (the speed of this text connection)", CONFIG_SERIAL_BAUD);
    boot_line("    Health      a status line prints every 60 seconds");
    boot_note("so you know the canary is still alive");
    boot_line("    Debug mode  for much more detail, rebuild with:");
    boot_note("  pio run -e canary-wap-debug");
    boot_note("this turns on verbose logging for");
    boot_note("every subsystem (GPS, BLE, mesh, etc.)");
    boot_line("    BLE debug   hold the BOOT button during power-on");
    boot_kvf("",  "for %u seconds to activate a special", CONFIG_BOOT_BUTTON_HOLD_MS / 1000);
    boot_note("Bluetooth debug beacon");
    boot_blank();
    boot_separator();
    boot_blank();
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
        const char* dev_id_ap = witness_chain_device_id(&g_witness_chain);
        size_t ap_prefix_len = strlen(CONFIG_DEVICE_ID_PREFIX);
        const char* ap_suffix = (strncmp(dev_id_ap, CONFIG_DEVICE_ID_PREFIX, ap_prefix_len) == 0)
                                ? dev_id_ap + ap_prefix_len : dev_id_ap;
        snprintf(ap_cfg.ssid, sizeof(ap_cfg.ssid), "%s%s",
                 CONFIG_AP_SSID_PREFIX, ap_suffix);
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
    ble_cfg.device_name = g_ble_debug_active
        ? CONFIG_BLE_DEBUG_NAME_PREFIX
        : CONFIG_BLE_DEVICE_NAME;
    ble_cfg.device_id = witness_chain_device_id(&g_witness_chain);
    ble_cfg.public_key = g_witness_chain.public_key;
    ble_cfg.tx_power = CONFIG_BLE_TX_POWER;
    ble_cfg.pairable = CONFIG_BLE_PAIRABLE;

    if (ble_mgr_init(&ble_cfg) == RESULT_OK) {
        ble_mgr_start_advertising();
        LOG_I("Bluetooth started%s", g_ble_debug_active ? " (debug mode)" : "");
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

        witness_record_t record = {};
        uint8_t payload[128];
        size_t payload_len = 0;

        // Payload byte 0: time source flag
        #if FEATURE_GNSS
        bool gps_time_valid = g_gnss_parser.time.valid &&
            (now - g_gnss_parser.time.last_update_ms) < 30000;
        payload[payload_len++] = gps_time_valid ? 0x01 : 0x00;
        if (gps_time_valid) {
            // Bytes 1-7: GPS UTC (year_hi, year_lo, mon, day, hr, min, sec)
            payload[payload_len++] = (uint8_t)(g_gnss_parser.time.year >> 8);
            payload[payload_len++] = (uint8_t)(g_gnss_parser.time.year & 0xFF);
            payload[payload_len++] = g_gnss_parser.time.month;
            payload[payload_len++] = g_gnss_parser.time.day;
            payload[payload_len++] = g_gnss_parser.time.hour;
            payload[payload_len++] = g_gnss_parser.time.minute;
            payload[payload_len++] = g_gnss_parser.time.second;
            // Byte 8: fix quality, Byte 9: satellites
            payload[payload_len++] = (uint8_t)g_gnss_parser.fix.quality;
            payload[payload_len++] = g_gnss_parser.fix.satellites;
        }
        #else
        payload[payload_len++] = 0x00;
        #endif

        if (witness_chain_create_record(&g_witness_chain, RECORD_TYPE_WITNESS_EVENT,
                                        payload, payload_len, &record) == RESULT_OK) {
            #if FEATURE_GNSS
            if (gps_time_valid) {
                record.time_source = TIME_SOURCE_GPS_UTC;
                record.gps_time.available = true;
                record.gps_time.utc = g_gnss_parser.time;
                record.gps_time.fix_quality = g_gnss_parser.fix.quality;
                record.gps_time.satellites = g_gnss_parser.fix.satellites;
                record.gps_time.fix_age_ms = now - g_gnss_parser.time.last_update_ms;
            } else {
                record.time_source = TIME_SOURCE_DEVICE;
                memset(&record.gps_time, 0, sizeof(record.gps_time));
            }
            #endif
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

        uint32_t up = g_health.uptime_sec;
        uint32_t h = up / 3600;
        uint32_t m = (up % 3600) / 60;
        uint32_t s = up % 60;

        Serial.println();
        if (h > 0)
            Serial.printf("    ,_, Alive %uh %um %us", h, m, s);
        else
            Serial.printf("    ,_, Alive %um %us", m, s);

        Serial.printf(" | %u records created | %u self-checks passed",
                       g_health.records_created, g_health.records_verified);
        Serial.println();
        Serial.printf("        Memory %uK free (lowest: %uK)",
                       g_health.free_heap / 1024, g_health.min_heap / 1024);

        #if FEATURE_GNSS
        const gnss_fix_t* fix = gnss_parser_get_fix(&g_gnss_parser);
        if (fix->valid)
            Serial.printf(" | GPS locked (%u satellites)", fix->satellites);
        else
            Serial.print(" | GPS searching...");
        #endif

        #if FEATURE_SD_STORAGE
        Serial.printf(" | SD %u writes", g_health.sd_writes);
        #endif

        #if FEATURE_MESH_NETWORK
        if (g_health.mesh_messages_sent > 0)
            Serial.printf(" | Mesh %u msgs", g_health.mesh_messages_sent);
        #endif

        Serial.println();

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
