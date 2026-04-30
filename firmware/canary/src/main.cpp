/*
 * SecuraCV Canary — Main Entry Point
 *
 * Production witness device firmware for ESP32-S3.
 * Uses modular library components for faster incremental builds.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include <Arduino.h>
#include "canary_config.h"
#include "log_level.h"

// Library components
#include "securacv_crypto.h"
#include "securacv_witness.h"
#include "securacv_gps.h"

#if FEATURE_SD_STORAGE
#include "securacv_storage.h"
#endif

#if FEATURE_WIFI_AP
#include "securacv_network.h"
#endif

#if FEATURE_CAMERA_PEEK
#include "securacv_camera.h"
#endif

#if FEATURE_WATCHDOG
#include "esp_task_wdt.h"
#include "esp_idf_version.h"
#endif

#if FEATURE_HA_MQTT
#include "securacv_mqtt.h"
#include <ArduinoJson.h>
#endif

// ════════════════════════════════════════════════════════════════════════════
// GLOBALS
// ════════════════════════════════════════════════════════════════════════════

static GpsManager s_gps;
static uint32_t g_last_record_ms = 0;

#if FEATURE_HA_MQTT
static uint32_t g_last_mqtt_status_ms = 0;
static uint32_t g_last_mqtt_health_ms = 0;
#endif

// Device-unique AP password (derived from pubkey fingerprint)
static char g_ap_password[16];

// Serial command helpers
static void handle_serial_commands();
static void print_banner();
static void print_status();
static void handle_boot_button();
static void derive_ap_password(const uint8_t fingerprint[8], char* password, size_t len);

#if FEATURE_HA_MQTT
static void mqtt_publish_status_update();
static void mqtt_publish_health_update();
#endif

// ════════════════════════════════════════════════════════════════════════════
// UTILITY FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

static void serial_wait_for_cdc(uint32_t timeout_ms) {
#if defined(USB_CDC_ON_BOOT) && USB_CDC_ON_BOOT
  uint32_t start = millis();
  while (!Serial && (millis() - start < timeout_ms)) {
    delay(10);
  }
#else
  (void)timeout_ms;
#endif
}

// Derive device-unique AP password from public key fingerprint
// Format: "cv-XXXXX" (8 chars, unique per device)
static const char BASE62[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void derive_ap_password(const uint8_t fingerprint[8], char* password, size_t len) {
  char encoded[6];
  size_t chars_produced = 0;
  for (size_t i = 0; chars_produced < 5 && i < 8; i++) {
    uint8_t b = fingerprint[i];
    if (b < 248) { // Rejection sampling for unbiased BASE62
      encoded[chars_produced++] = BASE62[b % 62];
    }
  }
  while (chars_produced < 5) {
    encoded[chars_produced++] = '0';
  }
  encoded[5] = '\0';
  snprintf(password, len, "cv-%s", encoded);
}

// Factory reset: erase NVS and reboot
static void factory_reset() {
  Serial.println("\n[!!] FACTORY RESET — Erasing all stored data...");
  log_health(LOG_LEVEL_ALERT, LOG_CAT_SYSTEM, "Factory reset initiated", nullptr);

  // Persist final chain state before clearing
  witness_persist_chain_state();

  // Clear NVS
  NvsManager& nvs = NvsManager::instance();
  if (nvs.beginReadWrite()) {
    nvs.clear();
    nvs.end();
    Serial.println("[OK] NVS erased");
  }

  Serial.println("[..] Rebooting in 2 seconds...");
  delay(2000);
  ESP.restart();
}

// ════════════════════════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  serial_wait_for_cdc(SERIAL_CDC_WAIT_MS);

  print_banner();

  // Check for factory reset: hold BOOT button during startup
  pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);
  delay(100); // Debounce
  if (digitalRead(BOOT_BUTTON_GPIO) == LOW) {
    Serial.println("[!!] BOOT button held at startup — checking for factory reset...");
    uint32_t held_start = millis();
    while (digitalRead(BOOT_BUTTON_GPIO) == LOW) {
      if (millis() - held_start >= BOOT_LONG_PRESS_MS) {
        factory_reset(); // Does not return
      }
      delay(10);
    }
    Serial.println("[OK] BOOT button released before factory reset threshold");
  }

  // Provision device identity (keys, chain state)
  if (!witness_provision_device()) {
    Serial.println("[!!] Device provisioning failed - HALTING");
    while (true) { delay(1000); }
  }

  DeviceIdentity& device = witness_get_device();
  Serial.printf("[OK] Device ID: %s\n", device.device_id);

  // Derive device-unique AP password from pubkey fingerprint
  derive_ap_password(device.pubkey_fp, g_ap_password, sizeof(g_ap_password));
  Serial.printf("[OK] AP password derived (device-unique)\n");

  // Setup watchdog
#if FEATURE_WATCHDOG
  Serial.printf("[..] Watchdog timer: %us timeout\n", WATCHDOG_TIMEOUT_SEC);
  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    // ESP-IDF 5.x: struct-based API
    esp_task_wdt_config_t wdt_config = {
      .timeout_ms = WATCHDOG_TIMEOUT_SEC * 1000,
      .idle_core_mask = (1 << 0) | (1 << 1),
      .trigger_panic = true
    };
    esp_err_t wdt_err = esp_task_wdt_reconfigure(&wdt_config);
    if (wdt_err == ESP_ERR_INVALID_STATE) {
      esp_task_wdt_init(&wdt_config);
    }
  #else
    // ESP-IDF 4.x: simple parameters
    esp_task_wdt_init(WATCHDOG_TIMEOUT_SEC, true);
  #endif
  esp_task_wdt_add(NULL);
  Serial.println("[OK] Watchdog configured");
#endif

  // Initialize SD card storage
#if FEATURE_SD_STORAGE
  Serial.println("[..] Initializing SD card storage...");
  if (storage_init(nullptr)) {
    Serial.println("[OK] SD card ready for witness records");
    witness_get_health().sd_healthy = true;
  } else {
    Serial.println("[WARN] SD card not available - records will not persist");
    witness_get_health().sd_healthy = false;
  }
#endif

  // Start WiFi Access Point and HTTP server
#if FEATURE_WIFI_AP
  Serial.println("[..] Starting WiFi Access Point...");
  NetworkManager& net = network_get_instance();
  // Pass device_id as the mDNS hostname so each Canary on a shared home
  // network is reachable at its own canary-<id>.local — fixes hostname
  // collisions when a homeowner has more than one Canary.
  if (net.begin(device.ap_ssid, g_ap_password, device.device_id)) {
    Serial.println("[OK] WiFi AP active");
#if FEATURE_HTTP_SERVER
    Serial.println("[..] Starting HTTP server...");
    net.startHttpServer();
#endif
  } else {
    Serial.println("[WARN] WiFi AP failed to start");
  }
#endif

  // Initialize camera for peek/preview
#if FEATURE_CAMERA_PEEK
  Serial.println("[..] Initializing camera for peek/preview...");
  if (camera_init()) {
    Serial.println("[OK] Camera ready for peek");
  } else {
    Serial.println("[WARN] Camera init failed - peek disabled");
  }
#endif

  // Initialize GPS
  Serial.println();
  Serial.printf("[..] GNSS: %u baud, RX=GPIO%d, TX=GPIO%d\n", GPS_BAUD, GPS_RX_PIN, GPS_TX_PIN);
  s_gps.begin(Serial1, GPS_BAUD, GPS_RX_PIN, GPS_TX_PIN);

  // Initialize MQTT (optional — device works without it)
#if FEATURE_HA_MQTT
  Serial.println("[..] Initializing MQTT...");
  mqtt_init(device.device_id, FIRMWARE_VERSION);
#endif

  // Create boot attestation record
  Serial.println("[..] Creating boot attestation record...");
  uint8_t boot_payload[64];
  CborWriter cbor(boot_payload, sizeof(boot_payload));
  cbor.write_map(3);
  cbor.write_text("type"); cbor.write_text("boot");
  cbor.write_text("boot"); cbor.write_uint(device.boot_count);
  cbor.write_text("ver"); cbor.write_text(FIRMWARE_VERSION);

  WitnessRecord boot_rec;
  if (witness_create_record(boot_payload, cbor.size(), RECORD_BOOT_ATTESTATION, &boot_rec)) {
    Serial.printf("[OK] Boot attestation: seq=%u\n", boot_rec.seq);
  }

  // Log boot event
  log_health(LOG_LEVEL_INFO, LOG_CAT_SYSTEM, "Device boot complete", FIRMWARE_VERSION);

  g_last_record_ms = millis();

  // Print ready banner
  Serial.println();
  Serial.println("╔══════════════════════════════════════════════════════════════╗");
  Serial.println("║               WITNESS DEVICE READY                           ║");
  Serial.println("╠══════════════════════════════════════════════════════════════╣");
  Serial.printf("║  Device ID  : %-45s  ║\n", device.device_id);
#if FEATURE_WIFI_AP
  NetworkManager& network = network_get_instance();
  Serial.printf("║  WiFi AP    : %-45s  ║\n", device.ap_ssid);
  Serial.printf("║  Password   : %-45s  ║\n", g_ap_password);
  Serial.printf("║  Dashboard  : http://%-39s  ║\n", network.getStatus().ap_ip);
  {
    const char* host = network.getMdnsHostname();
    char mdns_url[64];
    snprintf(mdns_url, sizeof(mdns_url), "http://%s.local",
             (host && host[0]) ? host : "canary");
    Serial.printf("║  mDNS       : %-45s  ║\n", mdns_url);
  }
#endif
  Serial.println("╠══════════════════════════════════════════════════════════════╣");
  Serial.println("║  Commands: h=help, i=identity, s=status, g=gps               ║");
  Serial.println("║  BOOT: short=info, 5s hold=factory reset                     ║");
  Serial.println("╚══════════════════════════════════════════════════════════════╝");
  Serial.println();
}

// ════════════════════════════════════════════════════════════════════════════
// LOOP
// ════════════════════════════════════════════════════════════════════════════

void loop() {
#if FEATURE_WATCHDOG
  esp_task_wdt_reset();
#endif

  // Handle serial commands
  handle_serial_commands();

  // Handle boot button (info print, factory reset)
  handle_boot_button();

  // Update GPS
  s_gps.update();

  // Update state machine
  const GnssFix& fix = s_gps.getFix();
  witness_update_state(fix.valid, fix.last_update_ms, s_gps.getSpeedMps());

  // Update health metrics
  SystemHealth& health = witness_get_health();
  uint32_t now = millis();
  health.uptime_sec = now / 1000;
  health.free_heap = ESP.getFreeHeap();
  if (health.free_heap < health.min_heap || health.min_heap == 0) {
    health.min_heap = health.free_heap;
  }
  health.gps_healthy = fix.valid;

  // Sync GPS stats to health
  health.gps_sentences = s_gps.getSentenceCount();
  health.gga_count = s_gps.getGgaCount();
  health.rmc_count = s_gps.getRmcCount();
  health.gsa_count = s_gps.getGsaCount();
  health.gsv_count = s_gps.getGsvCount();
  health.vtg_count = s_gps.getVtgCount();
  if (s_gps.getFirstFixMs() > 0 && health.gps_lock_ms == 0) {
    health.gps_lock_ms = s_gps.getFirstFixMs();
  }

#if FEATURE_WIFI_AP
  // Check WiFi connection periodically
  network_get_instance().checkConnection();
#endif

#if FEATURE_HA_MQTT
  // MQTT loop — handles reconnect and keepalive
  mqtt_loop();

  // Publish status periodically
  if (mqtt_connected() && now - g_last_mqtt_status_ms >= MQTT_STATUS_INTERVAL_MS) {
    g_last_mqtt_status_ms = now;
    mqtt_publish_status_update();
  }

  // Publish health periodically
  if (mqtt_connected() && now - g_last_mqtt_health_ms >= MQTT_HEALTH_INTERVAL_MS) {
    g_last_mqtt_health_ms = now;
    mqtt_publish_health_update();
  }
#endif

  // Create witness records at interval
  if (now - g_last_record_ms >= RECORD_INTERVAL_MS) {
    g_last_record_ms = now;

    // Build witness event payload
    uint8_t payload[256];
    CborWriter cbor(payload, sizeof(payload));

    FixState state = witness_get_state();

    cbor.write_map(7);
    cbor.write_text("state"); cbor.write_text(state_name_short(state));
    cbor.write_text("fix"); cbor.write_bool(fix.valid);
    cbor.write_text("lat"); cbor.write_float(fix.lat);
    cbor.write_text("lon"); cbor.write_float(fix.lon);
    cbor.write_text("alt"); cbor.write_float(fix.altitude_m);
    cbor.write_text("spd"); cbor.write_float(fix.speed_kmh);
    cbor.write_text("sats"); cbor.write_uint(fix.satellites);

    WitnessRecord rec;
    if (witness_create_record(payload, cbor.size(), RECORD_WITNESS_EVENT, &rec)) {
      health.records_created++;

      // Print status every 20 records
      if (health.records_created % 20 == 0) {
        print_status();
      }
    } else {
      log_health(LOG_LEVEL_ERROR, LOG_CAT_WITNESS, "Record creation failed", nullptr);
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
// BOOT BUTTON HANDLER
// ════════════════════════════════════════════════════════════════════════════

static void handle_boot_button() {
  static uint32_t boot_btn_start = 0;
  static bool boot_btn_was_pressed = false;

  bool pressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);

  if (pressed && !boot_btn_was_pressed) {
    // Button just pressed
    boot_btn_start = millis();
    boot_btn_was_pressed = true;
  } else if (pressed && boot_btn_was_pressed) {
    // Still held — check for factory reset threshold
    uint32_t duration = millis() - boot_btn_start;
    if (duration >= BOOT_LONG_PRESS_MS) {
      factory_reset(); // Does not return
    }
  } else if (!pressed && boot_btn_was_pressed) {
    // Button released
    uint32_t duration = millis() - boot_btn_start;
    boot_btn_was_pressed = false;

    if (duration >= BOOT_MEDIUM_PRESS_MS) {
      // Medium hold: print device info
      print_status();
    }
    // Short press: reserved for future use (provisioning gate)
  }
}

// ════════════════════════════════════════════════════════════════════════════
// MQTT PUBLISHING
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_HA_MQTT

static void mqtt_publish_status_update() {
  DeviceIdentity& device = witness_get_device();
  SystemHealth& health = witness_get_health();
  const GnssFix& fix = s_gps.getFix();

  JsonDocument doc;
  doc["uptime"] = health.uptime_sec;
  doc["free_heap"] = health.free_heap;
  doc["records_created"] = health.records_created;
  doc["seq"] = device.seq;
  doc["state"] = state_name_short(witness_get_state());
  doc["gps_fix"] = fix.valid;
  if (fix.valid) {
    doc["lat"] = fix.lat;
    doc["lon"] = fix.lon;
    doc["satellites"] = fix.satellites;
  }
  doc["sd_healthy"] = health.sd_healthy;
  doc["chain_valid"] = (health.verify_failures == 0);
  doc["firmware"] = FIRMWARE_VERSION;

  String payload;
  serializeJson(doc, payload);
  mqtt_publish_status(payload.c_str());
}

static void mqtt_publish_health_update() {
  SystemHealth& health = witness_get_health();
  DeviceIdentity& device = witness_get_device();

  JsonDocument doc;
  doc["uptime"] = health.uptime_sec;
  doc["free_heap"] = health.free_heap;
  doc["min_heap"] = health.min_heap;
  doc["records_created"] = health.records_created;
  doc["records_verified"] = health.records_verified;
  doc["verify_failures"] = health.verify_failures;
  doc["chain_persists"] = health.chain_persists;
  doc["gps_healthy"] = health.gps_healthy;
  doc["crypto_healthy"] = health.crypto_healthy;
  doc["sd_healthy"] = health.sd_healthy;
  doc["wifi_active"] = health.wifi_active;
  doc["http_requests"] = health.http_requests;
  doc["sd_writes"] = health.sd_writes;
  doc["sd_errors"] = health.sd_errors;
  doc["boot_count"] = device.boot_count;
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["tamper_detected"] = device.tamper_active;

  String payload;
  serializeJson(doc, payload);
  mqtt_publish_health(payload.c_str());
}

#endif // FEATURE_HA_MQTT

// ════════════════════════════════════════════════════════════════════════════
// SERIAL COMMANDS
// ════════════════════════════════════════════════════════════════════════════

static void handle_serial_commands() {
  if (!Serial.available()) return;

  char cmd = Serial.read();
  switch (cmd) {
    case 'h':
    case 'H':
    case '?':
      Serial.println("\n=== Commands ===");
      Serial.println("  h - This help");
      Serial.println("  i - Device identity");
      Serial.println("  s - Status");
      Serial.println("  g - GPS info");
      Serial.println("  r - Reboot");
#if FEATURE_HA_MQTT
      Serial.println("  m - MQTT status");
#endif
      Serial.println();
      break;

    case 'i':
    case 'I': {
      DeviceIdentity& device = witness_get_device();
      Serial.println("\n=== Identity ===");
      Serial.printf("  Device ID: %s\n", device.device_id);
      Serial.print("  Public Key: ");
      for (int i = 0; i < 32; i++) Serial.printf("%02x", device.pubkey[i]);
      Serial.println("\n");
      break;
    }

    case 's':
    case 'S':
      print_status();
      break;

    case 'g':
    case 'G': {
      const GnssFix& fix = s_gps.getFix();
      Serial.println("\n=== GPS ===");
      Serial.printf("  Fix: %s\n", fix.valid ? "Yes" : "No");
      if (fix.valid) {
        Serial.printf("  Lat: %.6f\n", fix.lat);
        Serial.printf("  Lon: %.6f\n", fix.lon);
        Serial.printf("  Alt: %.1f m\n", fix.altitude_m);
        Serial.printf("  Speed: %.1f km/h\n", fix.speed_kmh);
        Serial.printf("  Sats: %u\n", fix.satellites);
      }
      Serial.printf("  Sentences: %u (errors: %u)\n",
                    s_gps.getSentenceCount(), s_gps.getChecksumErrors());
      Serial.println();
      break;
    }

#if FEATURE_HA_MQTT
    case 'm':
    case 'M':
      Serial.println("\n=== MQTT ===");
      Serial.printf("  Connected: %s\n", mqtt_connected() ? "Yes" : "No");
      Serial.println();
      break;
#endif

    case 'r':
    case 'R':
      Serial.println("\nRebooting...");
      witness_persist_chain_state();
#if FEATURE_HA_MQTT
      mqtt_disconnect();
#endif
      delay(500);
      ESP.restart();
      break;

    default:
      break;
  }

  // Flush remaining input
  while (Serial.available()) Serial.read();
}

static void print_banner() {
  Serial.println();
  Serial.println("╔══════════════════════════════════════════════════════════════╗");
  Serial.println("║     SecuraCV Canary — Production Witness Device              ║");
  Serial.println("║     Privacy Witness Kernel (PWK) Compatible                  ║");
  Serial.printf("║     Version %-48s  ║\n", FIRMWARE_VERSION);
  Serial.println("╚══════════════════════════════════════════════════════════════╝");
}

static void print_status() {
  SystemHealth& health = witness_get_health();
  DeviceIdentity& device = witness_get_device();
  const GnssFix& fix = s_gps.getFix();

  Serial.println("\n=== Status ===");
  Serial.printf("  Uptime: %us\n", health.uptime_sec);
  Serial.printf("  Free heap: %u bytes\n", health.free_heap);
  Serial.printf("  Min heap: %u bytes\n", health.min_heap);
  Serial.printf("  Records: %u (seq: %u)\n", health.records_created, device.seq);

  FixState state = witness_get_state();
  Serial.printf("  State: %s\n", state_name(state));

  Serial.printf("  GPS: %s", health.gps_healthy ? "OK" : "No fix");
  if (health.gps_healthy) {
    Serial.printf(" (%.4f, %.4f, %u sats)", fix.lat, fix.lon, fix.satellites);
  }
  Serial.println();

#if FEATURE_SD_STORAGE
  Serial.printf("  SD: %s\n", health.sd_healthy ? "OK" : "Not mounted");
#endif

#if FEATURE_WIFI_AP
  Serial.printf("  WiFi: %s\n", health.wifi_active ? "OK" : "Down");
#endif

#if FEATURE_HA_MQTT
  Serial.printf("  MQTT: %s\n", mqtt_connected() ? "Connected" : "Disconnected");
#endif

  Serial.println();
}
