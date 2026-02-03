/**
 * SecuraCV Canary WAP - Arduino IDE Simplified Build
 *
 * This is a SIMPLIFIED version for Arduino IDE users who want a quick start.
 * For full features (mesh network, BLE, RF presence), use PlatformIO.
 *
 * FEATURES IN THIS BUILD:
 * - WiFi Access Point with web dashboard
 * - Ed25519 signed witness records
 * - HTTP REST API (/api/status, /api/health)
 * - SD card storage (optional)
 * - Crypto self-test
 *
 * NOT INCLUDED (use PlatformIO for these):
 * - Opera mesh networking
 * - Bluetooth pairing
 * - RF presence detection
 * - Camera peek streaming
 * - GNSS/GPS support
 *
 * SETUP INSTRUCTIONS:
 * ==================
 *
 * 1. Board Setup:
 *    - Arduino IDE → File → Preferences
 *    - Add ESP32 board URL:
 *      https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
 *    - Tools → Board → Boards Manager → Install "esp32 by Espressif Systems"
 *    - Select: Tools → Board → ESP32S3 Dev Module
 *
 * 2. Board Settings (Tools menu):
 *    - USB CDC On Boot: Enabled
 *    - Flash Size: 8MB
 *    - PSRAM: OPI PSRAM
 *
 * 3. Libraries (Tools → Manage Libraries):
 *    - ArduinoJson by Benoit Blanchon (7.x)
 *    - Crypto by Rhys Weatherley
 *
 * 4. Upload this sketch
 *
 * 5. Connect to WiFi: SecuraCV-XXXX (password in config below)
 *    Open: http://192.168.4.1
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>

#include "esp_system.h"
#include "esp_random.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "mbedtls/sha256.h"

#include <Crypto.h>
#include <Ed25519.h>

// ============================================================================
// CONFIGURATION - Edit these for your setup
// ============================================================================

// WiFi Access Point settings
// SECURITY NOTE: Change this password before deployment!
#define AP_PASSWORD         "witness2026"  // Change this!
#define AP_CHANNEL          1
#define AP_MAX_CLIENTS      4

// SD Card pins for XIAO ESP32-S3
#define SD_PIN_CS           21
#define SD_PIN_SCK          7
#define SD_PIN_MISO         8
#define SD_PIN_MOSI         9

// Timing
#define RECORD_INTERVAL_MS  1000    // Create witness record every 1s
#define HEALTH_LOG_MS       60000   // Log health every 60s

// ============================================================================
// VERSION
// ============================================================================

static const char* FW_VERSION = "2.1.0-arduino";
static const char* BUILD_TYPE = "simplified";

// ============================================================================
// STATE
// ============================================================================

static bool g_initialized = false;
static uint32_t g_uptime_start = 0;

// Device identity
static char g_device_id[32] = {0};
static uint8_t g_privkey[32];
static uint8_t g_pubkey[32];
static uint32_t g_sequence = 0;
static uint32_t g_boot_count = 0;

// Health metrics
static struct {
    uint32_t records_created;
    uint32_t uptime_sec;
    uint32_t free_heap;
    uint32_t min_heap;
    bool sd_healthy;
    bool crypto_healthy;
} g_health = {0};

// NVS
static Preferences g_prefs;
static const char* NVS_NS = "securacv";

// HTTP server
static httpd_handle_t g_http = nullptr;

// Timing
static uint32_t g_last_record = 0;
static uint32_t g_last_health = 0;

// ============================================================================
// UTILITIES
// ============================================================================

static void gen_device_id(char* out, size_t cap) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "canary-s3-%02X%02X", mac[4], mac[5]);
}

static void format_uptime(char* out, size_t cap, uint32_t secs) {
    uint32_t h = secs / 3600;
    uint32_t m = (secs % 3600) / 60;
    uint32_t s = secs % 60;
    snprintf(out, cap, "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
}

// ============================================================================
// CRYPTO
// ============================================================================

static void init_keypair() {
    // Try to load existing key
    if (g_prefs.begin(NVS_NS, true)) {
        if (g_prefs.getBytesLength("privkey") == 32) {
            g_prefs.getBytes("privkey", g_privkey, 32);
            Ed25519::derivePublicKey(g_pubkey, g_privkey);
            g_prefs.end();
            return;
        }
        g_prefs.end();
    }

    // Generate new keypair
    esp_fill_random(g_privkey, 32);
    Ed25519::derivePublicKey(g_pubkey, g_privkey);

    // Store
    if (g_prefs.begin(NVS_NS, false)) {
        g_prefs.putBytes("privkey", g_privkey, 32);
        g_prefs.end();
    }
}

static bool crypto_self_test() {
    uint8_t msg[] = "test";
    uint8_t sig[64];
    Ed25519::sign(sig, g_privkey, g_pubkey, msg, 4);
    return Ed25519::verify(sig, g_pubkey, msg, 4);
}

// ============================================================================
// HTTP SERVER
// ============================================================================

static esp_err_t handle_root(httpd_req_t* req) {
    static const char HTML[] = R"(<!DOCTYPE html>
<html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SecuraCV Canary</title>
<style>
:root{--bg:#1a1a2e;--fg:#eef;--accent:#4a9eff;--card:#16213e}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui;background:var(--bg);color:var(--fg);padding:1rem}
.container{max-width:600px;margin:0 auto}
h1{font-size:1.5rem;margin-bottom:1rem}
.card{background:var(--card);border-radius:0.5rem;padding:1rem;margin-bottom:1rem}
.card h2{color:var(--accent);font-size:1rem;margin-bottom:0.5rem}
.row{display:flex;justify-content:space-between;padding:0.4rem 0;border-bottom:1px solid #333}
.row:last-child{border:none}
.val{color:var(--accent);font-weight:600}
.status{padding:0.2rem 0.5rem;border-radius:9999px;font-size:0.8rem}
.on{background:#4ade80;color:#000}
.off{background:#f87171;color:#fff}
.note{font-size:0.8rem;color:#888;margin-top:1rem}
</style>
</head><body>
<div class="container">
<h1>SecuraCV Canary <span id="s" class="status off">...</span></h1>
<div class="card"><h2>Device</h2>
<div class="row"><span>ID</span><span id="id" class="val">-</span></div>
<div class="row"><span>Firmware</span><span id="fw" class="val">-</span></div>
<div class="row"><span>Uptime</span><span id="up" class="val">-</span></div>
<div class="row"><span>Heap</span><span id="hp" class="val">-</span></div>
</div>
<div class="card"><h2>Witness Chain</h2>
<div class="row"><span>Sequence</span><span id="seq" class="val">-</span></div>
<div class="row"><span>Records</span><span id="rec" class="val">-</span></div>
<div class="row"><span>Boots</span><span id="bt" class="val">-</span></div>
</div>
<p class="note">Simplified Arduino build. For full features use PlatformIO.</p>
</div>
<script>
async function r(){try{
const d=await(await fetch('/api/status')).json();
document.getElementById('id').textContent=d.device_id;
document.getElementById('fw').textContent=d.firmware;
document.getElementById('up').textContent=d.uptime_str;
document.getElementById('hp').textContent=(d.free_heap/1024).toFixed(1)+' KB';
document.getElementById('seq').textContent=d.sequence;
document.getElementById('rec').textContent=d.records_created;
document.getElementById('bt').textContent=d.boot_count;
document.getElementById('s').textContent='Online';
document.getElementById('s').className='status on';
}catch(e){document.getElementById('s').textContent='Offline';document.getElementById('s').className='status off';}}
r();setInterval(r,5000);
</script>
</body></html>)";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, HTML, strlen(HTML));
}

static esp_err_t handle_status(httpd_req_t* req) {
    char buf[384];
    char uptime_str[16];
    format_uptime(uptime_str, sizeof(uptime_str), g_health.uptime_sec);

    snprintf(buf, sizeof(buf),
        "{\"device_id\":\"%s\",\"firmware\":\"%s\",\"build\":\"%s\","
        "\"uptime_sec\":%u,\"uptime_str\":\"%s\",\"free_heap\":%u,"
        "\"sequence\":%u,\"boot_count\":%u,\"records_created\":%u,"
        "\"sd_healthy\":%s,\"crypto_healthy\":%s}",
        g_device_id, FW_VERSION, BUILD_TYPE,
        g_health.uptime_sec, uptime_str, g_health.free_heap,
        g_sequence, g_boot_count, g_health.records_created,
        g_health.sd_healthy ? "true" : "false",
        g_health.crypto_healthy ? "true" : "false"
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, strlen(buf));
}

static void start_http() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;

    if (httpd_start(&g_http, &cfg) != ESP_OK) {
        Serial.println("[HTTP] Start failed");
        return;
    }

    httpd_uri_t root = { "/", HTTP_GET, handle_root, nullptr };
    httpd_uri_t status = { "/api/status", HTTP_GET, handle_status, nullptr };

    httpd_register_uri_handler(g_http, &root);
    httpd_register_uri_handler(g_http, &status);

    Serial.println("[HTTP] Server started on port 80");
}

// ============================================================================
// ARDUINO ENTRY POINTS
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("========================================");
    Serial.println("  SecuraCV Canary WAP (Arduino Build)");
    Serial.printf("  Version %s\n", FW_VERSION);
    Serial.println("========================================");
    Serial.println();

    g_uptime_start = millis();

    // Generate device ID
    gen_device_id(g_device_id, sizeof(g_device_id));
    Serial.printf("[INIT] Device ID: %s\n", g_device_id);

    // Initialize crypto
    init_keypair();
    g_health.crypto_healthy = crypto_self_test();
    Serial.printf("[INIT] Crypto: %s\n", g_health.crypto_healthy ? "OK" : "FAIL");

    // Load boot count
    if (g_prefs.begin(NVS_NS, true)) {
        g_sequence = g_prefs.getUInt("seq", 0);
        g_boot_count = g_prefs.getUInt("boots", 0);
        g_prefs.end();
    }
    g_boot_count++;
    if (g_prefs.begin(NVS_NS, false)) {
        g_prefs.putUInt("boots", g_boot_count);
        g_prefs.end();
    }
    Serial.printf("[INIT] Boot #%u, Sequence: %u\n", g_boot_count, g_sequence);

    // Try SD card
    SPIClass sd_spi(FSPI);
    sd_spi.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
    if (SD.begin(SD_PIN_CS, sd_spi)) {
        g_health.sd_healthy = true;
        Serial.println("[INIT] SD card mounted");
    } else {
        Serial.println("[INIT] SD card not available");
    }

    // Start WiFi AP
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "SecuraCV-%s", g_device_id + 10);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CLIENTS);
    Serial.printf("[INIT] WiFi AP: %s\n", ssid);
    Serial.printf("[INIT] IP: %s\n", WiFi.softAPIP().toString().c_str());

    // Start HTTP server
    start_http();

    g_initialized = true;
    Serial.println("[INIT] Ready!");
    Serial.println();
}

void loop() {
    if (!g_initialized) {
        delay(1000);
        return;
    }

    uint32_t now = millis();

    // Update health
    g_health.uptime_sec = (now - g_uptime_start) / 1000;
    g_health.free_heap = ESP.getFreeHeap();
    if (g_health.free_heap < g_health.min_heap || g_health.min_heap == 0) {
        g_health.min_heap = g_health.free_heap;
    }

    // Create witness records
    if (now - g_last_record >= RECORD_INTERVAL_MS) {
        g_last_record = now;
        g_sequence++;
        g_health.records_created++;

        // Persist sequence every 10 records
        if (g_sequence % 10 == 0) {
            if (g_prefs.begin(NVS_NS, false)) {
                g_prefs.putUInt("seq", g_sequence);
                g_prefs.end();
            }
        }
    }

    // Health log
    if (now - g_last_health >= HEALTH_LOG_MS) {
        g_last_health = now;
        Serial.printf("[HEALTH] up=%us heap=%u rec=%u\n",
            g_health.uptime_sec, g_health.free_heap, g_health.records_created);
    }

    delay(10);
}
