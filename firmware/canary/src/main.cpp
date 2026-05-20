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

#if FEATURE_CSI
#include "securacv_csi.h"
#include "csi_modules_integration.h"
/* Bridge the canary HAL's csi_features_t into the common module
 * pipeline. Both structs share the same int8 vector + telemetry layout;
 * the assertion below is the wire-protocol guarantee that a refactor of
 * either side breaks the build instead of scrambling features at runtime. */
static_assert(sizeof(csi_features_t) == 36,
              "canary HAL csi_features_t must be 32 (vector) + 2 (frames) + 1 (bucket) + 1 (caps) bytes");
#endif

#if FEATURE_ACOUSTIC_EVENTS
#include "securacv_audio.h"
#include <Preferences.h>   /* mic mute persistence */
#endif

#if FEATURE_TOUCH
#include "securacv_touch.h"
#endif

#if FEATURE_IR_RMT
#include "securacv_ir.h"
#endif

#if FEATURE_TEMP_TAMPER
#include "securacv_envsens.h"
#endif

/* All five sensing sources feed a single aggregator. The header is
 * include-guarded, so one unconditional include is the right shape;
 * sensing_init() is also idempotent so each feature block can call it
 * without a guard. */
#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
#include "securacv_sensing.h"
#endif

/* The lowpower HAL is always compiled in once Phase 3 lands — it's
 * how main.cpp learns the wake reason on boot. Whether deep-sleep is
 * actually entered is gated separately by FEATURE_DEEP_SLEEP. */
#include "securacv_lowpower.h"

// ════════════════════════════════════════════════════════════════════════════
// GLOBALS
// ════════════════════════════════════════════════════════════════════════════

static GpsManager s_gps;
static uint32_t g_last_record_ms = 0;

#if FEATURE_HA_MQTT
static uint32_t g_last_mqtt_status_ms = 0;
static uint32_t g_last_mqtt_health_ms = 0;
static uint32_t g_last_mqtt_sensing_ms = 0;
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
#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
static void mqtt_publish_sensing_update();
#endif
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
// Drops visually ambiguous glyphs (0/O, 1/I/l) so users can read the
// password off the serial monitor or sticker without guessing.
static const char UNAMBIGUOUS_ALPHABET[] =
  "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static void derive_ap_password(const uint8_t fingerprint[8], char* password, size_t len) {
  char encoded[6];
  size_t chars_produced = 0;
  for (size_t i = 0; chars_produced < 5 && i < 8; i++) {
    uint8_t b = fingerprint[i];
    if (b < 228) { // 228 = 57 * 4, rejection sampling to avoid bias
      encoded[chars_produced++] = UNAMBIGUOUS_ALPHABET[b % 57];
    }
  }
  while (chars_produced < 5) {
    encoded[chars_produced++] = '2';
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

// 10-minute daily time bucket (0..143), matched across audio, sensing,
// CSI, and witness payloads so a verifier comparing two events from
// different sensors sees consistent bucket values. Constant name is
// prefixed BUCKET_ to avoid colliding with the 5-second TIME_BUCKET_MS
// macro that canary_config.h defines for witness-chain coarsening.
static constexpr uint32_t BUCKET_10MIN_MS    = 10UL * 60UL * 1000UL;
static constexpr uint8_t  TIME_BUCKETS_PER_DAY = 144;
static inline uint8_t time_bucket_now() {
  return (uint8_t)((millis() / BUCKET_10MIN_MS) % TIME_BUCKETS_PER_DAY);
}

// ════════════════════════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  serial_wait_for_cdc(SERIAL_CDC_WAIT_MS);

  print_banner();

  // Capture wake reason from the previous deep-sleep cycle BEFORE any
  // peripheral init touches RTC state. On a normal cold boot this
  // returns "cold_boot"; on a touch-pad wake (panic / tamper) we log
  // the firing pad here so a forensics trail exists even if the device
  // was woken by an attacker tampering with the enclosure.
  lowpower_init();
  {
    const uint8_t wr = lowpower_get_wake_reason();
    if (wr != LOWPOWER_WAKE_UNDEFINED) {
      Serial.printf("[..] Wake reason: %s", lowpower_wake_reason_name(wr));
      if (wr == LOWPOWER_WAKE_TOUCH) {
        const int pad = lowpower_get_wake_touch_pad();
        if (pad >= 0) Serial.printf(" (pad=%d)", pad);
      }
      Serial.println();
    }
  }

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
  // The device always advertises as `canary.local`; device_id goes into
  // the _securacv._tcp TXT record so the SPA wizard can distinguish
  // multiple Canaries during its service browse. RFC 6762 §9 handles
  // the rare two-on-one-network hostname collision by auto-renaming
  // the second device to canary-2.local etc.
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

  #if FEATURE_ACOUSTIC_EVENTS
  // HA can flip the mic mute via the discovered switch entity. The
  // callback runs in the MQTT loop task (which IS the main task; the
  // PubSubClient pumps from mqtt_loop()), so it can safely call into
  // audio_mute() — that already defers the I2S teardown to
  // audio_process() via atomic pending flags.
  mqtt_set_mic_mute_cmd_callback([](bool muted) {
    audio_mute(muted, AUDIO_MUTE_SOURCE_MQTT);
    /* Persist intent so a reboot honors the HA-set state, via the
     * shared helper used by every other control path. */
    audio_save_mute_intent(muted);
  });

  /* HA "Run Audio Self-Test" button: 30-second window, same gate as
   * the dashboard's /api/audio/test/start. The selftest matcher ignores
   * the event callback while active so a test press DOESN'T flow into
   * smoke / CO automations — see audio.h. */
  mqtt_set_audio_test_cmd_callback([](){
    if (!audio_is_muted() && audio_is_running()) {
      audio_selftest_start(30000);
    }
  });
  #endif
#endif

  // Wire emergency / security sensing events into the Ed25519 witness
  // chain. Only T3/T4 alarm cadences, silent panic, enclosure tamper,
  // and temp drift get signed — high-rate informational events (CSI
  // windows, IR button presses) are deliberately NOT witnessed.
#if FEATURE_SENSING_WITNESS && \
    (FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_TEMP_TAMPER)
  sensing_init();  /* idempotent — make sure the aggregator is up */
  sensing_set_witness_callback([](const sensing_witness_event_t* we) {
    /* Build a small CBOR payload — same shape across all five kinds
     * so verifiers can ingest with one schema. The IR / CSI events
     * deliberately don't reach this callback, so the schema doesn't
     * need to carry their fields. */
    uint8_t payload[64];
    CborWriter cbor(payload, sizeof(payload));
    cbor.write_map(4);
    cbor.write_text("kind"); cbor.write_uint(we->kind);
    cbor.write_text("conf"); cbor.write_uint(we->confidence);
    cbor.write_text("bkt");  cbor.write_uint(we->time_bucket);
    cbor.write_text("cat");  cbor.write_uint(we->category);

    /* Tamper kinds (touch enclosure tamper, temp drift) get the
     * RECORD_TAMPER_ALERT type so downstream filters can split them
     * out. Everything else uses RECORD_WITNESS_EVENT. */
    const RecordType rt =
        (we->kind == SENSING_WITNESS_TOUCH_TAMPER ||
         we->kind == SENSING_WITNESS_TEMP_DRIFT)
            ? RECORD_TAMPER_ALERT
            : RECORD_WITNESS_EVENT;

    WitnessRecord rec;
    /* witness_create_record() already increments records_created on
     * success internally (securacv_witness.cpp); we only log on the
     * failure path here. */
    if (!witness_create_record(payload, cbor.size(), rt, &rec)) {
      log_health(LOG_LEVEL_ERROR, LOG_CAT_WITNESS,
                 "Sensing witness record failed", nullptr);
    }
  });
  Serial.println("[OK] Sensing witness chain bridge armed");
#endif

  // Initialize CSI sensing (motion / breathing / micro-activity)
#if FEATURE_CSI
  Serial.println("[..] Initializing CSI environmental sensing...");
  sensing_init();
  csi_config_t csi_cfg = CSI_CONFIG_DEFAULT;
  if (csi::init(csi_cfg)) {
    /* Register the v1 module pipeline (presence, breathing, activity
     * ribbon, daily summary, anomaly baseline) BEFORE arming the
     * features callback. The HAL won't deliver windows until start()
     * succeeds, but registering early means a deferred-start retry
     * doesn't race the first feature window. */
    securacv_csi_modules_init();

    csi::set_features_callback([](const csi_features_t* f) {
      sensing_feed_csi(f);
      /* Forward the same window into the common module pipeline.
       * Pass as void* so this TU stays the only place where both
       * csi_features_t typedefs are visible (canary HAL and common
       * library); the size_t static_assert at the top of this file
       * guards against future struct drift. */
      securacv_csi_modules_feed(static_cast<const void*>(f));
    });
    /* WiFi may not yet be running; start() defers itself if so and the
     * deferred-start retry runs from process() until WiFi comes up. */
    if (csi::start()) {
      Serial.println("[OK] CSI sensing armed (deferred-start safe, modules registered)");
    } else {
      Serial.println("[WARN] CSI sensing start failed");
    }
  } else {
    Serial.println("[WARN] CSI sensing init failed");
  }
#endif

  // Initialize PDM acoustic event detection (T3 smoke / T4 CO cadences)
#if FEATURE_ACOUSTIC_EVENTS
  Serial.println("[..] Initializing PDM acoustic event detection...");
  sensing_init();  /* idempotent */
  audio_config_t audio_cfg = AUDIO_CONFIG_DEFAULT;
  if (audio_init(&audio_cfg)) {
    audio_set_event_callback([](const audio_event_t* evt) {
      sensing_feed_audio_event(evt->event_type, evt->confidence,
                               evt->cycle_count, evt->time_bucket);
    });
    /* Sign every mute / unmute toggle into the witness chain. This lets
     * a later operator verify when the mic was turned off and by what
     * source (boot / dashboard / Home Assistant) — important if anyone
     * ever asks "was the device listening at the time of the incident?". */
    audio_set_mute_callback([](bool muted, uint8_t source) {
      sensing_feed_mic_mute_event(muted, source, time_bucket_now());
      /* Tell Home Assistant immediately so the switch entity reflects
       * the new state without waiting for the next /sensing publish.
       * No-op if MQTT isn't connected. */
      #if FEATURE_HA_MQTT
      mqtt_publish_mic_mute_state(muted);
      #endif
    });
    /* Honor user-persisted mute state. We're still single-task here —
     * the HTTP server has not started yet — so we can safely use the
     * synchronous boot helper that opens / skips I2S directly. After
     * the HTTP server comes up, runtime mute calls go through the
     * deferred audio_mute() path instead. */
    Preferences mic_prefs;
    bool persisted_mute = false;
    if (mic_prefs.begin("securacv", true /* read-only */)) {
      persisted_mute = mic_prefs.getBool("mic_muted", false);
      mic_prefs.end();
    }
    const bool boot_ok = audio_mute_sync_at_boot(persisted_mute);
    if (persisted_mute) {
      Serial.println("[OK] Acoustic detector held MUTED by user (NVS)");
      /* Emit a boot-state mute record so the witness chain shows the
       * device booted into the muted state — investigators can later
       * tell "muted before the incident" from "muted in response to
       * it". We only emit on the persisted-muted path; the unmuted
       * default would flood the chain with one record per reboot. */
      sensing_feed_mic_mute_event(true, AUDIO_MUTE_SOURCE_BOOT, time_bucket_now());
    } else if (boot_ok && audio_is_running()) {
      Serial.println("[OK] Acoustic detector armed (T3 smoke / T4 CO)");
    } else {
      Serial.println("[WARN] Acoustic detector start failed");
    }
    /* Seed the MQTT-side cache with the current state. The publish
     * itself is a no-op until the broker connection is up, but the
     * cached value will be sent on the first successful connect. */
    #if FEATURE_HA_MQTT
    mqtt_publish_mic_mute_state(audio_is_muted());
    #endif
  } else {
    Serial.println("[WARN] Acoustic detector init failed");
  }
#endif

  // Initialize capacitive-touch sensor (silent panic / enclosure tamper)
#if FEATURE_TOUCH
  Serial.println("[..] Initializing capacitive-touch sensor...");
  sensing_init();  /* idempotent */
  touch_config_t touch_cfg = TOUCH_CONFIG_DEFAULT;
  if (touch_init(&touch_cfg)) {
    touch_set_event_callback([](const touch_event_t* evt) {
      sensing_feed_touch_event(evt->event_type, evt->confidence,
                               evt->pad_channel, evt->time_bucket);
    });
    if (touch_start()) {
      Serial.printf("[OK] Touch sensor armed on pad %d (panic+tamper)\n",
                    touch_cfg.channel);
    } else {
      Serial.println("[WARN] Touch sensor start failed");
    }
  } else {
    Serial.println("[WARN] Touch sensor init failed");
  }
#endif

  // Initialize IR remote-control activity detection (RMT RX)
#if FEATURE_IR_RMT
  Serial.println("[..] Initializing IR remote-control detection...");
  sensing_init();  /* idempotent */
  ir_config_t ir_cfg = IR_CONFIG_DEFAULT;
  if (ir_init(&ir_cfg)) {
    ir_set_event_callback([](const ir_event_t* evt) {
      sensing_feed_ir_event(evt->category, evt->hash_bucket,
                            evt->confidence, evt->time_bucket);
    });
    if (ir_start()) {
      Serial.printf("[OK] IR detector armed on GPIO %d\n", ir_cfg.gpio_num);
    } else {
      Serial.println("[WARN] IR detector start failed");
    }
  } else {
    Serial.println("[WARN] IR detector init failed");
  }
#endif

  // Initialize internal temperature-drift tamper detector
#if FEATURE_TEMP_TAMPER
  Serial.println("[..] Initializing temp-drift tamper detector...");
  sensing_init();  /* idempotent */
  envsens_config_t envsens_cfg = ENVSENS_CONFIG_DEFAULT;
  if (envsens_init(&envsens_cfg)) {
    envsens_set_event_callback([](const envsens_event_t* evt) {
      sensing_feed_temp_drift_event(evt->confidence, evt->time_bucket);
    });
    if (envsens_start()) {
      Serial.println("[OK] Temp-drift detector armed");
    } else {
      Serial.println("[WARN] Temp-drift detector start failed");
    }
  } else {
    Serial.println("[WARN] Temp-drift detector init failed");
  }
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

  // Advance the motion filter so the API reports a stable position when the
  // device is stationary. We hint with the witness state's notion of "at
  // rest" so the filter and state machine don't disagree.
  s_gps.updateMotion(witness_get_state() == STATE_STATIONARY);

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

#if FEATURE_CSI
  // Pump CSI: drain ring, finalize windows, fire feature callback into
  // sensing_feed_csi().
  csi::process();
#endif

#if FEATURE_ACOUSTIC_EVENTS
  // Pump audio: drain I2S DMA, compute RMS envelope, run cadence FSM,
  // fire event callback into sensing_feed_audio_event() on T3/T4 match.
  audio_process();
#endif

#if FEATURE_TOUCH
  // Pump touch: read filtered pad value at 20 Hz, run panic/tamper/
  // approach state machines, fire event callback on confirmed match.
  touch_process();
#endif

#if FEATURE_IR_RMT
  // Pump IR: drain RMT ring, decode NEC/RC5/Sony, hash payload to
  // privacy bucket, fire on confirmed match.
  ir_process();
#endif

#if FEATURE_TEMP_TAMPER
  // Pump envsens: sample internal die temp once per minute, run drift
  // detector. Cheap — at most one read per process() call.
  envsens_process();
#endif

  // Age out stale sensing events (TTL decay across all sources). One
  // call per loop is sufficient — sensing_tick() is idempotent and
  // cheap. Gated only on "any sensing source compiled in" so an
  // all-sensors-off build still links cleanly.
#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
  sensing_tick();
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

  // Publish sensing snapshot periodically — same cadence as status.
  // Only useful when at least one sensing source is compiled in;
  // otherwise the snapshot is all-defaults and the HA entities stay
  // at "Unknown".
#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
  if (mqtt_connected() && now - g_last_mqtt_sensing_ms >= MQTT_STATUS_INTERVAL_MS) {
    g_last_mqtt_sensing_ms = now;
    mqtt_publish_sensing_update();
  }
#endif
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

#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
static void mqtt_publish_sensing_update() {
  /* Single retained JSON snapshot of the sensing aggregator state.
   * Each HA entity's value_template extracts its field. Same shape
   * as /api/sensing — all the per-source TTL handling already lives
   * inside securacv_sensing, so a stale event naturally clears here. */
  sensing_state_t s;
  sensing_snapshot(&s);

  JsonDocument doc;

  /* CSI / activity headline scalars. */
  doc["label"]      = sensing_label_name(s.activity_label);
  doc["motion"]     = s.motion_score;
  doc["breathing"]  = s.breathing_score;
  doc["rssi_dbm"]   = (int)s.rssi_dbm;
  doc["frames_in_window"] = s.frames_in_window;
  doc["channel"]    = s.channel;
  doc["time_bucket"] = s.time_bucket;

  /* Acoustic last event (cleared by TTL after 30 s). HA value
   * templates compare against the string here to drive smoke / CO /
   * knock / doorbell / glass-break binary sensors. Use the actual
   * enum names from securacv_audio.h so the mapping survives any
   * future enum-value changes. */
  const char* ae = "none";
#if FEATURE_ACOUSTIC_EVENTS
  switch (s.last_audio_event_type) {
    case AUDIO_EVENT_T3_SMOKE_ALARM: ae = "smoke_alarm_t3"; break;
    case AUDIO_EVENT_T4_CO_ALARM:    ae = "co_alarm_t4";    break;
    case AUDIO_EVENT_KNOCK:          ae = "knock";          break;
    case AUDIO_EVENT_DOORBELL:       ae = "doorbell";       break;
    case AUDIO_EVENT_GLASS_BREAK:    ae = "glass_break";    break;
  }
#endif
  doc["acoustic_event"] = ae;
  doc["acoustic_conf"]  = s.last_audio_event_conf;
  /* DELIBERATELY NOT publishing acoustic_event_age_ms over MQTT —
   * that would leak sub-second event timing to every broker
   * subscriber and violate AGENTS.md Invariant III (only 10-min
   * coarse buckets cross the device boundary). HA's binary_sensors
   * therefore latch ON until the 30 s sensing-aggregator TTL clears
   * `acoustic_event` back to "none", which is plenty fast enough for
   * automations like "if smoke alarm cadence, push notification".
   * The local /api/sensing endpoint DOES expose age_ms — that's
   * localhost / LAN traffic, not a published feed. */
#if FEATURE_ACOUSTIC_EVENTS && FEATURE_ACOUSTIC_TRANSIENTS
  /* Diagnostic counters — drive the HA total_increasing sensors so an
   * operator can plot alarm frequency over time. Gated by
   * FEATURE_ACOUSTIC_TRANSIENTS so non-transient builds don't publish
   * unused zero counters into HA's device card. */
  audio_stats_t a_stats = {0};
  audio_get_stats(&a_stats);
  JsonObject ast = doc["acoustic_stats"].to<JsonObject>();
  ast["frames_processed"]     = a_stats.frames_processed;
  ast["on_transitions"]       = a_stats.on_transitions;
  ast["off_transitions"]      = a_stats.off_transitions;
  ast["t3_detected"]          = a_stats.t3_detected;
  ast["t4_detected"]          = a_stats.t4_detected;
  ast["i2s_read_errors"]      = a_stats.i2s_read_errors;
  ast["knock_detected"]       = a_stats.knock_detected;
  ast["doorbell_detected"]    = a_stats.doorbell_detected;
  ast["glass_break_detected"] = a_stats.glass_break_detected;
#endif

  /* Touch last event (cleared by TTL after 60 s). */
  const char* te = "none";
#if FEATURE_TOUCH
  switch (s.last_touch_event_type) {
    case TOUCH_EVENT_SILENT_PANIC:     te = "silent_panic";     break;
    case TOUCH_EVENT_ENCLOSURE_TAMPER: te = "enclosure_tamper"; break;
    case TOUCH_EVENT_APPROACH:         te = "approach";         break;
  }
#endif
  doc["touch_event"]   = te;
  doc["touch_conf"]    = s.last_touch_event_conf;
  doc["touch_pad"]     = s.last_touch_pad_channel;

  /* IR last activity (cleared by TTL after 10 s). */
  const char* ip = "none";
#if FEATURE_IR_RMT
  switch (s.last_ir_category) {
    case IR_PROTOCOL_NEC:  ip = "nec";  break;
    case IR_PROTOCOL_RC5:  ip = "rc5";  break;
    case IR_PROTOCOL_SONY: ip = "sony"; break;
  }
#endif
  doc["ir_protocol"] = ip;
  doc["ir_bucket"]   = s.last_ir_hash_bucket;
  doc["ir_conf"]     = s.last_ir_confidence;

  /* Temp drift active flag (cleared by TTL after 5 min). The HA
   * binary sensor for enclosure_tamper ORs this with touch_event
   * so a single tamper indicator covers both surfaces. */
  doc["temp_drift_active"] = (s.last_temp_drift_ms != 0);
  doc["temp_drift_conf"]   = s.last_temp_drift_conf;

  /* Lowpower wake reason — surfaces as a diagnostic sensor in HA so
   * an operator can see whether a remote canary booted from a touch
   * wake, timer, or cold boot. The lowpower HAL is unconditionally
   * compiled in (see top of file). */
  doc["wake_reason"] = lowpower_wake_reason_name(lowpower_get_wake_reason());

  String payload;
  serializeJson(doc, payload);
  mqtt_publish_sensing(payload.c_str());
}
#endif

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
