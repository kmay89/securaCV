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
static void app_print_scene_banner(void);
static void app_print_scene_hardware(void);
static void app_print_scene_features(void);
static void app_print_scene_protocol(void);
static void app_print_scene_network(const char* ssid);
static void app_print_scene_chain(void);
static void app_print_scene_ready(void);
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

    app_print_scene_banner();

    // Initialize HAL
    if (hal_init() != HAL_OK) {
        LOG_E("HAL initialization failed!");
        return;
    }

    app_print_scene_hardware();
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
    snprintf(ssid_buf, sizeof(ssid_buf), "%s%s",
             CONFIG_AP_SSID_PREFIX,
             witness_chain_device_id(&g_witness_chain) + strlen(CONFIG_DEVICE_ID_PREFIX));

    app_print_scene_network(ssid_buf);

    // Initialize debug beacon after BLE is running
    #if FEATURE_BLUETOOTH
    if (g_ble_debug_active) {
        char fp_hex[5];
        if (g_health.crypto_healthy) {
            snprintf(fp_hex, sizeof(fp_hex), "%02x%02x",
                     g_witness_chain.pubkey_fingerprint[6],
                     g_witness_chain.pubkey_fingerprint[7]);
        } else {
            strcpy(fp_hex, "0000");
        }
        ble_debug_beacon_init(fp_hex);
    }
    #endif

    g_initialized = true;
    g_health.uptime_sec = 0;

    app_print_scene_ready();
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
// SERIAL BOOT SEQUENCE
//
// The canary guides the user through each boot stage, explaining
// what the device is doing in plain language. Each scene uses a
// small bird illustration that visually matches the current step.
// ============================================================================

static void app_print_scene_banner() {
    Serial.println();
    Serial.println();
    Serial.println(F("              ,_,          Waking up..."));
    Serial.println(F("             (o.o)"));
    Serial.println(F("             /| |\\         SecuraCV Canary WAP"));
    Serial.printf( "              d b          v%s\n", FW_VERSION_STRING);
    Serial.println();
    Serial.println(F("    This is your privacy witness device."));
    Serial.println(F("    It creates tamper-proof records of what it"));
    Serial.println(F("    sees, so nobody can change the story later."));
    Serial.println();
    Serial.printf( "    Type        %s\n", CONFIG_DEVICE_TYPE);
    Serial.printf( "    Model       %s\n", CONFIG_MODEL);
    Serial.printf( "    Built       %s %s\n", FW_BUILD_DATE, FW_BUILD_TIME);
    Serial.printf( "    MAC         %s\n", WiFi.macAddress().c_str());
    Serial.println(F("    ------------------------------------------------"));
    Serial.println();
}

static void app_print_scene_hardware() {
    Serial.println(F("              ,_,"));
    Serial.println(F("             (o.o) ?       Checking the hardware..."));
    Serial.println(F("             (  >)"));
    Serial.println(F("              \" \"          What am I running on?"));
    Serial.println(F("    ------------------------------------------------"));
    Serial.printf( "    Board       %s\n", BOARD_NAME);
    Serial.printf( "    Chip        %s rev %u\n", ESP.getChipModel(), (unsigned)ESP.getChipRevision());
    Serial.printf( "    CPU         %u MHz, %u core(s)\n", (unsigned)ESP.getCpuFreqMHz(), (unsigned)ESP.getChipCores());
    Serial.println(F("                (the brain — higher MHz = faster thinking)"));
    Serial.printf( "    Flash       %u MB\n", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
    Serial.println(F("                (permanent storage, like a hard drive)"));
    if (psramFound()) {
        Serial.printf( "    PSRAM       %u KB total, %u KB free\n",
                       (unsigned)(ESP.getPsramSize() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
        Serial.println(F("                (extra memory for big tasks like camera)"));
    } else {
        Serial.println(F("    PSRAM       not found"));
    }
    Serial.printf( "    Heap        %u KB free at boot\n", (unsigned)(ESP.getFreeHeap() / 1024));
    Serial.println(F("                (working memory — like a desk to work on)"));
    Serial.printf( "    SDK         %s\n", ESP.getSdkVersion());
    Serial.println(F("                (the software toolkit this firmware uses)"));
    Serial.println();
}

static void app_print_scene_features() {
    Serial.println(F("                  ,_,"));
    Serial.println(F("            ___  (o.o)     What can I do?"));
    Serial.println(F("           | . |/ /"));
    Serial.println(F("           | . |          Each + is a feature that's"));
    Serial.println(F("           |___|          turned on in this build."));
    Serial.println(F("    ------------------------------------------------"));

    #if FEATURE_GNSS
    Serial.println(F("    + GPS/GNSS       knows where it is on Earth"));
    #else
    Serial.println(F("    - GPS/GNSS"));
    #endif
    #if FEATURE_WIFI_AP
    Serial.println(F("    + WiFi AP/STA    creates its own WiFi + joins yours"));
    #else
    Serial.println(F("    - WiFi AP/STA"));
    #endif
    #if FEATURE_SD_STORAGE
    Serial.println(F("    + SD Storage     saves records to a memory card"));
    #else
    Serial.println(F("    - SD Storage"));
    #endif
    #if FEATURE_HTTP_SERVER
    Serial.println(F("    + HTTP Server    runs a web dashboard you can visit"));
    #else
    Serial.println(F("    - HTTP Server"));
    #endif
    #if FEATURE_CAMERA_PEEK
    Serial.println(F("    + Camera         live preview for aiming the device"));
    #else
    Serial.println(F("    - Camera"));
    #endif
    #if FEATURE_MESH_NETWORK
    Serial.println(F("    + Opera Mesh     talks to other canaries nearby"));
    #else
    Serial.println(F("    - Opera Mesh"));
    #endif
    #if FEATURE_BLUETOOTH
    Serial.println(F("    + Bluetooth      pairs with your phone for setup"));
    #else
    Serial.println(F("    - Bluetooth"));
    #endif
    #if FEATURE_RF_PRESENCE
    Serial.println(F("    + RF Presence    detects people nearby using radio"));
    #else
    Serial.println(F("    - RF Presence"));
    #endif
    #if FEATURE_CHIRP
    Serial.println(F("    + Chirp          relays alerts from the community"));
    #else
    Serial.println(F("    - Chirp"));
    #endif
    #if FEATURE_WATCHDOG
    Serial.printf("    + Watchdog %ds    auto-restarts if something freezes\n", CONFIG_WATCHDOG_TIMEOUT_SEC);
    #else
    Serial.println(F("    - Watchdog"));
    #endif

    Serial.println();
    Serial.println(F("    Timing: how often the canary creates records"));
    Serial.println(F("    ------------------------------------------------"));
    Serial.printf( "    Record rate   every %u ms\n", CONFIG_RECORD_INTERVAL_MS);
    Serial.println(F("                  (creates 1 signed witness record per second)"));
    Serial.printf( "    Time bucket   %u ms\n", CONFIG_TIME_BUCKET_MS);
    Serial.println(F("                  (rounds timestamps so exact times stay private)"));
    Serial.printf( "    Self-verify   every %u seconds\n", CONFIG_VERIFY_INTERVAL_SEC);
    Serial.println(F("                  (the canary checks its own math is correct)"));
    Serial.printf( "    SD persist    every %u records\n", CONFIG_SD_PERSIST_INTERVAL);
    Serial.println(F("                  (saves a batch to the SD card at once)"));
    Serial.println();
}

static void app_print_scene_protocol() {
    Serial.println(F("              ,_,"));
    Serial.println(F("             (o.o)         Setting up the locks..."));
    Serial.println(F("              |#|"));
    Serial.println(F("             [###]         Every record gets signed"));
    Serial.println(F("              | |          so it can't be forged."));
    Serial.println(F("    ------------------------------------------------"));
    Serial.printf( "    Witness     %s\n", PWK_PROTOCOL_VERSION);
    Serial.println(F("                (the Privacy Witness Kernel protocol version)"));
    Serial.printf( "    Chain       %s\n", CHAIN_ALGORITHM);
    Serial.println(F("                (how records are linked — like a chain of"));
    Serial.println(F("                 paper clips where removing one breaks all)"));
    Serial.printf( "    Signature   %s\n", SIGNATURE_ALGORITHM);
    Serial.println(F("                (the math that proves a record is genuine,"));
    Serial.println(F("                 like a wax seal on a letter)"));
    Serial.printf( "    Ruleset     %s\n", RULESET_ID);
    Serial.println(F("                (the set of rules this device follows)"));
    Serial.println();
}

static void app_print_scene_chain() {
    Serial.println(F("              ,_,"));
    Serial.println(F("             (o.o)         Resuming the witness chain..."));
    Serial.println(F("              |=|"));
    Serial.println(F("             [===]         Each record links to the last,"));
    Serial.println(F("              |=|          like pages in a sealed book."));
    Serial.println(F("    ------------------------------------------------"));
    Serial.printf( "    Device      %s\n", witness_chain_device_id(&g_witness_chain));
    Serial.printf( "    Sequence    %u  (next record number)\n", witness_chain_sequence(&g_witness_chain));
    Serial.printf( "    Boot        #%u  (times this device has started)\n", witness_chain_boot_count(&g_witness_chain));
    Serial.printf( "    Integrity   %s\n", g_health.crypto_healthy ? "verified" : "check required");
    if (g_health.crypto_healthy) {
        Serial.printf("    Key         %02x%02x%02x%02x...  (Ed25519 public key prefix)\n",
                       g_witness_chain.pubkey_fingerprint[0],
                       g_witness_chain.pubkey_fingerprint[1],
                       g_witness_chain.pubkey_fingerprint[2],
                       g_witness_chain.pubkey_fingerprint[3]);
    }
    Serial.println();
}

static void app_print_scene_network(const char* ssid) {
    Serial.println(F("              ,_,  ))"));
    Serial.println(F("             (o.o)  ))     Broadcasting..."));
    Serial.println(F("              | |"));
    Serial.println(F("              | |          Your canary is now a WiFi"));
    Serial.println(F("              d b          hotspot you can connect to."));
    Serial.println(F("    ------------------------------------------------"));
    Serial.printf( "    WiFi name   %s\n", ssid);
    Serial.printf( "    Password    %s\n", CONFIG_AP_PASSWORD_DEFAULT);
    Serial.printf( "    Channel     %d  (WiFi radio channel, up to %d devices at once)\n", CONFIG_AP_CHANNEL, CONFIG_AP_MAX_CLIENTS);
    Serial.println(F("    Dashboard   http://canary.local"));
    Serial.println(F("                (type this in your browser to see the dashboard)"));
    Serial.printf( "    Direct IP   http://%s\n", WiFi.softAPIP().toString().c_str());
    Serial.println(F("                (use this if canary.local doesn't work)"));
    Serial.printf( "    HTTP port   %d\n", CONFIG_HTTP_PORT);
    Serial.println(F("    mDNS        canary.local  (service: _securacv._tcp)"));
    Serial.println(F("                (mDNS lets your device find the canary by name"));
    Serial.println(F("                 instead of remembering a number like 192.168.4.1)"));
    #if FEATURE_BLUETOOTH
    Serial.printf( "    BLE name    %s\n", CONFIG_BLE_DEVICE_NAME);
    Serial.println(F("                (how the canary appears on Bluetooth scans)"));
    #endif
    #if FEATURE_GNSS
    Serial.printf( "    GNSS baud   %u  (speed the GPS module talks at)\n", CONFIG_GNSS_BAUD);
    #endif
    Serial.println();
}

static void app_print_scene_ready() {
    Serial.println();
    Serial.println(F("    ================================================"));
    Serial.println();
    Serial.println(F("                   ,_,"));
    Serial.println(F("                  (o.o)  ~~"));
    Serial.println(F("                 /(> <)\\ ~~~~"));
    Serial.println(F("                  d | b  ~~~~~~"));
    Serial.println();
    Serial.println(F("    The canary is singing. Everything is working."));
    Serial.println();
    Serial.println(F("    It will now create a signed witness record"));
    Serial.println(F("    every second and store it to the SD card."));
    Serial.println(F("    Nobody can alter these records after the fact."));
    Serial.println();
    Serial.println(F("    ================================================"));
    Serial.println();
}

static void app_print_scene_guide(const char* ssid) {
    Serial.println(F("              ,_,"));
    Serial.println(F("             (o.o) !       How to connect:"));
    Serial.println(F("              |>|"));
    Serial.println(F("              | |"));
    Serial.println(F("    ------------------------------------------------"));
    Serial.println();
    Serial.printf( "    1. On your phone or laptop, join the WiFi\n");
    Serial.printf( "       network called \"%s\"\n", ssid);
    Serial.printf( "       and enter the password: %s\n", CONFIG_AP_PASSWORD_DEFAULT);
    Serial.println();
    Serial.println(F("    2. Open a web browser and go to:"));
    Serial.println(F("       http://canary.local"));
    Serial.printf( "       (or try http://%s)\n", WiFi.softAPIP().toString().c_str());
    Serial.println();
    Serial.println(F("    3. From the dashboard you can:"));
    Serial.println(F("       Timeline  - see the witness record history"));
    Serial.println(F("       Peek      - aim the camera"));
    Serial.println(F("       Sensing   - see who's nearby (via RF)"));
    Serial.println(F("       Settings  - connect to your home WiFi"));
    Serial.println();
    Serial.println(F("    REST API — for developers, scripts, and tinkerers:"));
    Serial.println(F("    (These are web addresses you can visit or call"));
    Serial.println(F("     from code. Add them after http://canary.local)"));
    Serial.println(F("    ------------------------------------------------"));
    Serial.println(F("    GET  /api/status          is the device healthy?"));
    Serial.println(F("    GET  /api/chain           latest witness chain info"));
    Serial.println(F("    GET  /api/logs            recent event log"));
    Serial.println(F("    POST /api/export          download all signed records"));
    Serial.println(F("    GET  /api/wifi/scan       see nearby WiFi networks"));
    Serial.println(F("    POST /api/wifi/connect    join your home WiFi"));
    Serial.println(F("    GET  /api/peek/stream     live camera video feed"));
    Serial.println(F("    GET  /api/peek/snapshot   take one photo"));
    Serial.println(F("    GET  /api/sensing         who's nearby? (RF data)"));
    Serial.println(F("    GET  /api/diagnostics     everything about this device"));
    Serial.println(F("    GET  /api/selftest        run a hardware check"));
    Serial.println(F("    POST /api/reboot          restart the canary"));
    Serial.println();
    Serial.println(F("    Serial monitor — what you're reading right now:"));
    Serial.println(F("    ------------------------------------------------"));
    Serial.printf( "    Baud rate   %u  (the speed of this text connection)\n", CONFIG_SERIAL_BAUD);
    Serial.println(F("    Health      a status line prints every 60 seconds"));
    Serial.println(F("                so you know the canary is still alive"));
    Serial.println(F("    Debug mode  for much more detail, rebuild with:"));
    Serial.println(F("                  pio run -e canary-wap-debug"));
    Serial.println(F("                this turns on verbose logging for"));
    Serial.println(F("                every subsystem (GPS, BLE, mesh, etc.)"));
    Serial.println(F("    BLE debug   hold the BOOT button during power-on"));
    Serial.printf( "                for %u seconds to activate a special\n", CONFIG_BOOT_BUTTON_HOLD_MS / 1000);
    Serial.println(F("                Bluetooth debug beacon"));
    Serial.println();
    Serial.println(F("    ------------------------------------------------"));
    Serial.println();
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

        #if FEATURE_GNSS
        if (gps_time_valid) {
            record.time_source = TIME_SOURCE_GPS_UTC;
            record.gps_time.available = true;
            record.gps_time.utc = g_gnss_parser.time;
            record.gps_time.fix_quality = g_gnss_parser.fix.quality;
            record.gps_time.satellites = g_gnss_parser.fix.satellites;
            record.gps_time.fix_age_ms = now - g_gnss_parser.time.last_update_ms;
        }
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
