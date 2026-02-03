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
#endif

// ════════════════════════════════════════════════════════════════════════════
// GLOBALS
// ════════════════════════════════════════════════════════════════════════════

static DeviceState g_device_state = STATE_NO_FIX;
static uint32_t g_state_entered_ms = 0;
static uint32_t g_last_record_ms = 0;

// Serial command helpers
static void handle_serial_commands();
static void print_banner();
static void print_status();

// ════════════════════════════════════════════════════════════════════════════
// UTILITY FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

static void serial_wait_for_cdc(uint32_t timeout_ms) {
#if USB_CDC_ON_BOOT
  uint32_t start = millis();
  while (!Serial && (millis() - start < timeout_ms)) {
    delay(10);
  }
#else
  (void)timeout_ms;
#endif
}

// ════════════════════════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  serial_wait_for_cdc(SERIAL_CDC_WAIT_MS);

  print_banner();

  // Initialize crypto and device identity
  NvsManager& nvs = nvs_get_instance();
  if (!nvs.begin()) {
    Serial.println("[!!] NVS initialization failed - HALTING");
    while (true) { delay(1000); }
  }

  if (!nvs.provisionDevice()) {
    Serial.println("[!!] Device provisioning failed - HALTING");
    while (true) { delay(1000); }
  }

  Serial.printf("[OK] Device ID: %s\n", nvs.getDeviceId());

  pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);

  // Setup watchdog
#if FEATURE_WATCHDOG
  Serial.printf("[..] Watchdog timer: %us timeout\n", WATCHDOG_TIMEOUT_SEC);
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WATCHDOG_TIMEOUT_SEC * 1000,
    .idle_core_mask = (1 << 0) | (1 << 1),
    .trigger_panic = true
  };
  esp_err_t wdt_err = esp_task_wdt_reconfigure(&wdt_config);
  if (wdt_err == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_init(&wdt_config);
  }
  esp_task_wdt_add(NULL);
  Serial.println("[OK] Watchdog configured");
#endif

  // Initialize SD card storage
#if FEATURE_SD_STORAGE
  Serial.println("[..] Initializing SD card storage...");
  if (storage_init(nullptr)) {
    Serial.println("[OK] SD card ready for witness records");
  } else {
    Serial.println("[WARN] SD card not available - records will not persist");
  }
#endif

  // Start WiFi Access Point and HTTP server
#if FEATURE_WIFI_AP
  Serial.println("[..] Starting WiFi Access Point...");
  NetworkManager& net = network_get_instance();
  if (net.beginAP()) {
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
  Serial.printf("[..] GNSS: %u baud, RX=GPIO%d, TX=GPIO%d\n", GPS_BAUD, GPS_RX_GPIO, GPS_TX_GPIO);
  GpsManager& gps = gps_get_instance();
  gps.begin(GPS_BAUD, GPS_RX_GPIO, GPS_TX_GPIO);

  // Initialize witness chain
  WitnessChain& chain = witness_get_chain();
  chain.begin(nvs.getPrivateKey(), nvs.getPublicKey());

  // Create boot attestation record
  Serial.println("[..] Creating boot attestation record...");
  if (chain.createBootAttestation()) {
    Serial.printf("[OK] Boot attestation: seq=%u\n", chain.getSequence());
  }

  // Log boot event
  witness_log_health(LOG_LEVEL_INFO, LOG_CAT_SYSTEM, "Device boot complete", FIRMWARE_VERSION);

  g_state_entered_ms = millis();
  g_last_record_ms = millis();

  // Print ready banner
  Serial.println();
  Serial.println("╔══════════════════════════════════════════════════════════════╗");
  Serial.println("║               WITNESS DEVICE READY                           ║");
  Serial.println("╠══════════════════════════════════════════════════════════════╣");
  Serial.printf("║  Device ID  : %-45s  ║\n", nvs.getDeviceId());
#if FEATURE_WIFI_AP
  NetworkManager& network = network_get_instance();
  Serial.printf("║  WiFi AP    : %-45s  ║\n", network.getSSID());
  Serial.printf("║  Password   : %-45s  ║\n", AP_PASSWORD_DEFAULT);
  Serial.printf("║  Dashboard  : http://%-39s  ║\n", network.getIP().toString().c_str());
  Serial.println("║  mDNS       : http://canary.local                             ║");
#endif
  Serial.println("╠══════════════════════════════════════════════════════════════╣");
  Serial.println("║  Commands: h=help, i=identity, s=status, g=gps               ║");
  Serial.println("║  Hold BOOT button 1.2s to print all info                     ║");
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

  // Check boot button for info reprint
  static uint32_t boot_btn_start = 0;
  bool pressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);
  if (pressed) {
    if (boot_btn_start == 0) {
      boot_btn_start = millis();
    } else if (millis() - boot_btn_start >= BOOT_BUTTON_HOLD_MS) {
      print_status();
      boot_btn_start = 0;
      delay(300);
    }
  } else {
    boot_btn_start = 0;
  }

  // Update GPS
  GpsManager& gps = gps_get_instance();
  gps.update();

  // Update device state based on GPS fix
  DeviceState new_state = g_device_state;
  if (gps.hasFix()) {
    if (gps.isMoving(MOVING_SPEED_THRESHOLD)) {
      new_state = STATE_MOVING;
    } else {
      new_state = STATE_STATIONARY;
    }
  } else {
    new_state = STATE_NO_FIX;
  }

  if (new_state != g_device_state) {
    g_device_state = new_state;
    g_state_entered_ms = millis();
  }

  // Update health metrics
  HealthMetrics& health = witness_get_health();
  uint32_t now = millis();
  health.uptime_sec = now / 1000;
  health.free_heap = ESP.getFreeHeap();
  if (health.free_heap < health.min_heap) {
    health.min_heap = health.free_heap;
  }
  health.gps_healthy = gps.hasFix();

#if FEATURE_WIFI_AP
  // Check WiFi connection periodically
  network_get_instance().checkConnection();
#endif

  // Create witness records at interval
  if (now - g_last_record_ms >= RECORD_INTERVAL_MS) {
    g_last_record_ms = now;

    WitnessChain& chain = witness_get_chain();
    const GpsFix& fix = gps.getFix();

    if (chain.createWitnessEvent(fix, g_device_state)) {
      // Record created successfully
      health.records_created++;

      // Print status every 20 records
      if (health.records_created % 20 == 0) {
        print_status();
      }
    } else {
      witness_log_health(LOG_LEVEL_ERROR, LOG_CAT_WITNESS, "Record creation failed", nullptr);
    }
  }
}

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
      Serial.println();
      break;

    case 'i':
    case 'I': {
      NvsManager& nvs = nvs_get_instance();
      Serial.println("\n=== Identity ===");
      Serial.printf("  Device ID: %s\n", nvs.getDeviceId());
      Serial.print("  Public Key: ");
      const uint8_t* pk = nvs.getPublicKey();
      for (int i = 0; i < 32; i++) Serial.printf("%02x", pk[i]);
      Serial.println("\n");
      break;
    }

    case 's':
    case 'S':
      print_status();
      break;

    case 'g':
    case 'G': {
      GpsManager& gps = gps_get_instance();
      const GpsFix& fix = gps.getFix();
      Serial.println("\n=== GPS ===");
      Serial.printf("  Fix: %s\n", fix.valid ? "Yes" : "No");
      if (fix.valid) {
        Serial.printf("  Lat: %.6f\n", fix.lat);
        Serial.printf("  Lon: %.6f\n", fix.lon);
        Serial.printf("  Alt: %.1f m\n", fix.alt_m);
        Serial.printf("  Speed: %.1f km/h\n", fix.speed_kmh);
        Serial.printf("  Sats: %u\n", fix.sats);
      }
      Serial.println();
      break;
    }

    case 'r':
    case 'R':
      Serial.println("\nRebooting...");
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
  HealthMetrics& health = witness_get_health();
  GpsManager& gps = gps_get_instance();
  WitnessChain& chain = witness_get_chain();

  Serial.println("\n=== Status ===");
  Serial.printf("  Uptime: %us\n", health.uptime_sec);
  Serial.printf("  Free heap: %u bytes\n", health.free_heap);
  Serial.printf("  Min heap: %u bytes\n", health.min_heap);
  Serial.printf("  Records: %u (seq: %u)\n", health.records_created, chain.getSequence());

  const char* state_names[] = {"NO_FIX", "STATIONARY", "MOVING"};
  Serial.printf("  State: %s\n", state_names[g_device_state]);

  Serial.printf("  GPS: %s", health.gps_healthy ? "OK" : "No fix");
  if (health.gps_healthy) {
    const GpsFix& fix = gps.getFix();
    Serial.printf(" (%.4f, %.4f, %u sats)", fix.lat, fix.lon, fix.sats);
  }
  Serial.println();

#if FEATURE_SD_STORAGE
  Serial.printf("  SD: %s\n", health.sd_healthy ? "OK" : "Not mounted");
#endif

#if FEATURE_WIFI_AP
  Serial.printf("  WiFi: %s\n", health.wifi_healthy ? "OK" : "Down");
#endif

  Serial.println();
}
