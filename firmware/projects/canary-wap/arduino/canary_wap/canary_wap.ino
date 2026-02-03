/**
 * SecuraCV Canary WAP - Arduino IDE Entry Point
 *
 * This sketch wraps the modular firmware for Arduino IDE compatibility.
 * For PlatformIO builds, use the main src/main.cpp directly.
 *
 * SETUP INSTRUCTIONS:
 * ==================
 *
 * 1. Board Setup:
 *    - Open Arduino IDE
 *    - Go to File -> Preferences
 *    - Add to "Additional Boards Manager URLs":
 *      https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
 *    - Go to Tools -> Board -> Boards Manager
 *    - Search for "esp32" and install "esp32 by Espressif Systems"
 *    - Select: Tools -> Board -> ESP32S3 Dev Module (or XIAO ESP32S3)
 *
 * 2. Board Settings:
 *    - USB CDC On Boot: Enabled
 *    - Flash Size: 8MB
 *    - Partition Scheme: Default 4MB with SPIFFS
 *    - PSRAM: OPI PSRAM
 *
 * 3. Library Dependencies (install via Library Manager):
 *    - ArduinoJson by Benoit Blanchon (7.x)
 *    - Crypto by Rhys Weatherley
 *    - NimBLE-Arduino by h2zero
 *
 * 4. Open this sketch and click Upload
 *
 * For more details, see the README.md in the project root.
 */

// ============================================================================
// ARDUINO IDE CONFIGURATION
// ============================================================================

// Feature flags - match config.h
#define CONFIG_CANARY_WAP       1
#define CONFIG_DEFAULT          1

// Include paths are set up by the build system
// When building with Arduino IDE, copy common/ headers to src/

// ============================================================================
// BOARD AND CONFIG INCLUDES
// ============================================================================

// These paths work when the sketch is in the correct location
// relative to the firmware directory structure

// For Arduino IDE: headers are expected in the sketch folder or libraries
#if __has_include("pins.h")
#include "pins.h"
#else
#include "../../../boards/xiao-esp32s3-sense/pins/pins.h"
#endif

#if __has_include("config.h")
#include "config.h"
#else
#include "../../../configs/canary-wap/default/config.h"
#endif

// ============================================================================
// CORE INCLUDES
// ============================================================================

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>

#include "esp_system.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "mbedtls/sha256.h"

#include <Crypto.h>
#include <Ed25519.h>

#if FEATURE_CAMERA_PEEK
#include "esp_camera.h"
#endif

// ============================================================================
// VERSION
// ============================================================================

static const char* FW_VERSION = "2.1.0";
static const char* DEVICE_TYPE = "canary_wap";
static const char* BOARD_NAME_STR = "XIAO ESP32-S3 Sense";

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

void app_init_hardware();
void app_init_storage();
void app_init_network();
void app_init_witness();
void app_process_loop();

// ============================================================================
// GLOBALS
// ============================================================================

static bool g_initialized = false;
static uint32_t g_uptime_start_ms = 0;

// Device identity
static char g_device_id[32] = {0};
static uint8_t g_privkey[32];
static uint8_t g_pubkey[32];
static uint32_t g_sequence = 0;
static uint32_t g_boot_count = 0;
static uint8_t g_chain_head[32] = {0};

// System health
static struct {
    uint32_t records_created;
    uint32_t records_verified;
    uint32_t uptime_sec;
    uint32_t free_heap;
    uint32_t min_heap;
    bool gps_healthy;
    bool sd_healthy;
    bool wifi_active;
    bool crypto_healthy;
} g_health = {0};

// SD card
static SPIClass g_sd_spi(FSPI);
static bool g_sd_mounted = false;

// HTTP server
static httpd_handle_t g_http_server = nullptr;

// NVS namespace
static const char* NVS_NAMESPACE = "securacv";
static Preferences g_prefs;

// Timing
static uint32_t g_last_record_ms = 0;
static uint32_t g_last_health_log_ms = 0;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

static void generate_device_id(char* out, size_t cap) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "canary-s3-%02X%02X", mac[4], mac[5]);
}

static void hex_to_str(char* out, const uint8_t* d, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        out[i*2] = hex[d[i] >> 4];
        out[i*2+1] = hex[d[i] & 0x0F];
    }
    out[n*2] = 0;
}

static void sha256_raw(const uint8_t* data, size_t n, uint8_t out[32]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, n);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

static void format_uptime(char* out, size_t cap, uint32_t secs) {
    uint32_t h = secs / 3600;
    uint32_t m = (secs % 3600) / 60;
    uint32_t s = secs % 60;
    snprintf(out, cap, "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
}

// ============================================================================
// NVS PERSISTENCE
// ============================================================================

static bool nvs_load_key(uint8_t priv[32]) {
    if (!g_prefs.begin(NVS_NAMESPACE, true)) return false;
    size_t n = g_prefs.getBytesLength("privkey");
    if (n != 32) { g_prefs.end(); return false; }
    g_prefs.getBytes("privkey", priv, 32);
    g_prefs.end();
    return true;
}

static bool nvs_store_key(const uint8_t priv[32]) {
    if (!g_prefs.begin(NVS_NAMESPACE, false)) return false;
    g_prefs.putBytes("privkey", priv, 32);
    g_prefs.end();
    return true;
}

static uint32_t nvs_load_u32(const char* key, uint32_t def = 0) {
    if (!g_prefs.begin(NVS_NAMESPACE, true)) return def;
    uint32_t v = g_prefs.getUInt(key, def);
    g_prefs.end();
    return v;
}

static bool nvs_store_u32(const char* key, uint32_t val) {
    if (!g_prefs.begin(NVS_NAMESPACE, false)) return false;
    g_prefs.putUInt(key, val);
    g_prefs.end();
    return true;
}

// ============================================================================
// CRYPTO
// ============================================================================

static bool generate_keypair(uint8_t priv[32], uint8_t pub[32]) {
    esp_fill_random(priv, 32);
    Ed25519::derivePublicKey(pub, priv);
    return true;
}

static void sign_message(const uint8_t priv[32], const uint8_t pub[32],
                         const uint8_t* msg, size_t len, uint8_t sig[64]) {
    Ed25519::sign(sig, priv, pub, msg, len);
}

static bool verify_signature(const uint8_t pub[32], const uint8_t* msg,
                             size_t len, const uint8_t sig[64]) {
    return Ed25519::verify(sig, pub, msg, len);
}

// ============================================================================
// HTTP SERVER
// ============================================================================

static esp_err_t http_status_handler(httpd_req_t* req) {
    char buf[512];
    char uptime_str[16];
    format_uptime(uptime_str, sizeof(uptime_str), g_health.uptime_sec);

    snprintf(buf, sizeof(buf),
        "{\"device_id\":\"%s\",\"firmware\":\"%s\",\"board\":\"%s\","
        "\"uptime_sec\":%u,\"uptime_str\":\"%s\",\"free_heap\":%u,"
        "\"sequence\":%u,\"boot_count\":%u,"
        "\"sd_healthy\":%s,\"wifi_active\":%s,\"crypto_healthy\":%s}",
        g_device_id, FW_VERSION, BOARD_NAME_STR,
        g_health.uptime_sec, uptime_str, g_health.free_heap,
        g_sequence, g_boot_count,
        g_health.sd_healthy ? "true" : "false",
        g_health.wifi_active ? "true" : "false",
        g_health.crypto_healthy ? "true" : "false"
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, strlen(buf));
}

static esp_err_t http_health_handler(httpd_req_t* req) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"records_created\":%u,\"records_verified\":%u,"
        "\"free_heap\":%u,\"min_heap\":%u,"
        "\"gps_healthy\":%s,\"sd_healthy\":%s}",
        g_health.records_created, g_health.records_verified,
        g_health.free_heap, g_health.min_heap,
        g_health.gps_healthy ? "true" : "false",
        g_health.sd_healthy ? "true" : "false"
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, strlen(buf));
}

// Embedded Web UI (minimal dashboard)
static const char WEB_UI_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SecuraCV Canary</title>
<style>
:root{--bg:#1a1a2e;--fg:#eef;--accent:#4a9eff;--card:#16213e}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui;background:var(--bg);color:var(--fg);padding:1rem}
.container{max-width:800px;margin:0 auto}
h1{font-size:1.5rem;margin-bottom:1rem}
.card{background:var(--card);border-radius:0.5rem;padding:1rem;margin-bottom:1rem}
.card h2{color:var(--accent);font-size:1rem;margin-bottom:0.5rem}
.metric{display:flex;justify-content:space-between;padding:0.5rem 0;border-bottom:1px solid #333}
.metric:last-child{border:none}
.value{color:var(--accent);font-weight:600}
.status{display:inline-block;padding:0.25rem 0.5rem;border-radius:9999px;font-size:0.875rem}
.online{background:#4ade80;color:#000}
.offline{background:#f87171;color:#fff}
button{background:var(--accent);color:#fff;border:none;padding:0.5rem 1rem;border-radius:0.25rem;cursor:pointer;margin:0.25rem}
button:hover{opacity:0.8}
</style>
</head>
<body>
<div class="container">
<h1>SecuraCV Canary <span id="status" class="status offline">Connecting...</span></h1>
<div class="card">
<h2>Device Info</h2>
<div class="metric"><span>Device ID</span><span id="device_id" class="value">-</span></div>
<div class="metric"><span>Firmware</span><span id="firmware" class="value">-</span></div>
<div class="metric"><span>Uptime</span><span id="uptime" class="value">-</span></div>
<div class="metric"><span>Free Heap</span><span id="heap" class="value">-</span></div>
</div>
<div class="card">
<h2>Witness Chain</h2>
<div class="metric"><span>Sequence</span><span id="sequence" class="value">-</span></div>
<div class="metric"><span>Boot Count</span><span id="boots" class="value">-</span></div>
<div class="metric"><span>Records</span><span id="records" class="value">-</span></div>
</div>
<div class="card">
<h2>Actions</h2>
<button onclick="location.href='/api/witness/export'">Export Records</button>
<button onclick="refresh()">Refresh</button>
</div>
</div>
<script>
async function refresh(){
try{
const r=await fetch('/api/status');
const d=await r.json();
document.getElementById('device_id').textContent=d.device_id||'-';
document.getElementById('firmware').textContent=d.firmware||'-';
document.getElementById('uptime').textContent=d.uptime_str||'-';
document.getElementById('heap').textContent=(d.free_heap/1024).toFixed(1)+' KB';
document.getElementById('sequence').textContent=d.sequence||'-';
document.getElementById('boots').textContent=d.boot_count||'-';
document.getElementById('status').textContent='Online';
document.getElementById('status').className='status online';
}catch(e){
document.getElementById('status').textContent='Offline';
document.getElementById('status').className='status offline';
}
}
refresh();
setInterval(refresh,5000);
</script>
</body>
</html>
)rawliteral";

static esp_err_t http_root_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, WEB_UI_HTML, strlen(WEB_UI_HTML));
}

static void start_http_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&g_http_server, &config) != ESP_OK) {
        Serial.println("[HTTP] Server start failed");
        return;
    }

    // Register handlers
    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = http_root_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(g_http_server, &uri_root);

    httpd_uri_t uri_status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = http_status_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(g_http_server, &uri_status);

    httpd_uri_t uri_health = {
        .uri = "/api/health",
        .method = HTTP_GET,
        .handler = http_health_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(g_http_server, &uri_health);

    Serial.println("[HTTP] Server started on port 80");
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void app_init_hardware() {
    Serial.println("[INIT] Hardware...");

#if FEATURE_WATCHDOG
    esp_task_wdt_init(CONFIG_WATCHDOG_TIMEOUT_SEC, true);
    esp_task_wdt_add(nullptr);
    Serial.printf("[INIT] Watchdog enabled (%ds)\n", CONFIG_WATCHDOG_TIMEOUT_SEC);
#endif
}

void app_init_storage() {
    Serial.println("[INIT] Storage...");

#if FEATURE_SD_STORAGE
    g_sd_spi.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
    if (SD.begin(SD_PIN_CS, g_sd_spi, SD_SPI_FREQ_FAST)) {
        g_sd_mounted = true;
        g_health.sd_healthy = true;
        Serial.println("[INIT] SD card mounted");
    } else {
        Serial.println("[INIT] SD card not available");
    }
#endif
}

void app_init_witness() {
    Serial.println("[INIT] Witness chain...");

    // Generate device ID
    generate_device_id(g_device_id, sizeof(g_device_id));

    // Load or generate keypair
    if (!nvs_load_key(g_privkey)) {
        Serial.println("[INIT] Generating new keypair...");
        generate_keypair(g_privkey, g_pubkey);
        nvs_store_key(g_privkey);
    } else {
        Ed25519::derivePublicKey(g_pubkey, g_privkey);
    }

    // Load chain state
    g_sequence = nvs_load_u32("seq", 0);
    g_boot_count = nvs_load_u32("boots", 0) + 1;
    nvs_store_u32("boots", g_boot_count);

    // Verify crypto
    uint8_t test_msg[] = "test";
    uint8_t test_sig[64];
    sign_message(g_privkey, g_pubkey, test_msg, 4, test_sig);
    g_health.crypto_healthy = verify_signature(g_pubkey, test_msg, 4, test_sig);

    char pubkey_hex[17];
    hex_to_str(pubkey_hex, g_pubkey, 8);

    Serial.printf("[INIT] Device: %s\n", g_device_id);
    Serial.printf("[INIT] Pubkey: %s...\n", pubkey_hex);
    Serial.printf("[INIT] Sequence: %u, Boots: %u\n", g_sequence, g_boot_count);
}

void app_init_network() {
    Serial.println("[INIT] Network...");

#if FEATURE_WIFI_AP
    // Generate AP SSID
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "SecuraCV-%s", g_device_id + 10);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid, CONFIG_AP_PASSWORD_DEFAULT, CONFIG_AP_CHANNEL, 0, CONFIG_AP_MAX_CLIENTS);

    Serial.printf("[INIT] WiFi AP: %s\n", ap_ssid);
    Serial.printf("[INIT] IP: %s\n", WiFi.softAPIP().toString().c_str());
    g_health.wifi_active = true;
#endif

#if FEATURE_HTTP_SERVER
    start_http_server();
#endif
}

// ============================================================================
// MAIN LOOP
// ============================================================================

void app_process_loop() {
    uint32_t now = millis();

    // Update health
    g_health.uptime_sec = (now - g_uptime_start_ms) / 1000;
    g_health.free_heap = ESP.getFreeHeap();
    if (g_health.free_heap < g_health.min_heap || g_health.min_heap == 0) {
        g_health.min_heap = g_health.free_heap;
    }

    // Create periodic witness records
    if (now - g_last_record_ms >= CONFIG_RECORD_INTERVAL_MS) {
        g_last_record_ms = now;
        g_sequence++;
        g_health.records_created++;

        // Persist every N records
        if (g_sequence % CONFIG_SD_PERSIST_INTERVAL == 0) {
            nvs_store_u32("seq", g_sequence);
        }
    }

    // Periodic health log
    if (now - g_last_health_log_ms >= 60000) {
        g_last_health_log_ms = now;
        Serial.printf("[HEALTH] uptime=%us heap=%u/%u records=%u\n",
            g_health.uptime_sec, g_health.free_heap, g_health.min_heap,
            g_health.records_created);
    }

#if FEATURE_WATCHDOG
    esp_task_wdt_reset();
#endif
}

// ============================================================================
// ARDUINO ENTRY POINTS
// ============================================================================

void setup() {
    Serial.begin(115200);

    // Wait for serial (development)
    uint32_t wait_start = millis();
    while (!Serial && (millis() - wait_start) < 2500) {
        delay(10);
    }

    Serial.println();
    Serial.println("╔════════════════════════════════════════╗");
    Serial.println("║  SecuraCV Canary WAP                   ║");
    Serial.printf("║  Version %s                        ║\n", FW_VERSION);
    Serial.println("╚════════════════════════════════════════╝");
    Serial.println();

    g_uptime_start_ms = millis();

    app_init_hardware();
    app_init_storage();
    app_init_witness();
    app_init_network();

    g_initialized = true;
    Serial.println("[INIT] Complete!");
    Serial.println();
}

void loop() {
    if (!g_initialized) {
        delay(1000);
        return;
    }

    app_process_loop();
    delay(10);
}
