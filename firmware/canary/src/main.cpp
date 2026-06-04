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
#include "gnss/gps_privacy.h"  // gps_coarsen_deg() — operator-facing GPS coarsening (Invariant III)

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

#if FEATURE_MESH_NETWORK
/* Mesh layer headers — independent of FEATURE_CSI. Building with
 * FEATURE_MESH_NETWORK=1 and FEATURE_CSI=0 is a supported combination
 * (e.g. a Hub-only role without CSI sensing) so these must NOT live
 * inside the CSI gate. */
#include "mesh_transport.h"
#include "mesh_session.h"
#include "mesh_state.h"
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

#if FEATURE_VISION_DETECT
#include "securacv_vision.h"
#endif

#if FEATURE_POWER_MONITOR
#include "securacv_power.h"
#endif

#if FEATURE_POWER_POLICY
#include "securacv_power_policy.h"
#endif

#if FEATURE_SETUP_WIZARD
#include "securacv_setup.h"
#include <WiFi.h>
#endif

#if FEATURE_DIAGNOSTICS
#include "securacv_diagnostics.h"
#endif

#if FEATURE_DATA_MGMT
#include "securacv_data_mgmt.h"
#endif

#if FEATURE_BLE_STATUS
#include "securacv_ble_status.h"
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

#include <esp_system.h>

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

#if FEATURE_MESH_NETWORK
/* Mesh PairedCallback. Fires from mesh_session::process() on the main
 * loop when the pairing state machine reaches PAIRED. On the JOINER
 * side, `secret` carries the freshly-distributed opera_secret. The
 * INITIATOR side passes `secret == nullptr` because the initiator
 * already had the secret when start_pairing_initiator() was called
 * (its persistence is the integration layer's responsibility before
 * pairing kicks off). */
static void persist_replay_counters() {
#if FEATURE_MESH_NETWORK
  uint8_t fps[mesh_session::MAX_TRUSTED_PEERS][mesh_crypto::FINGERPRINT_LEN];
  uint64_t ctrs[mesh_session::MAX_TRUSTED_PEERS];
  const size_t n = mesh_session::get_replay_counters(fps, ctrs,
                                                     mesh_session::MAX_TRUSTED_PEERS);
  if (n == 0) return;
  const size_t save_count = (n > mesh_state::MAX_REPLAY_ENTRIES)
                          ? mesh_state::MAX_REPLAY_ENTRIES : n;
  mesh_state::ReplayEntry entries[mesh_state::MAX_REPLAY_ENTRIES];
  for (size_t i = 0; i < save_count; ++i) {
    memcpy(entries[i].fingerprint, fps[i], mesh_crypto::FINGERPRINT_LEN);
    entries[i].last_counter = ctrs[i];
  }
  mesh_state::save_replay_counters(entries, save_count);
#endif
}

static void on_pairing_succeeded(const uint8_t* secret, uint32_t code) {
  if (secret == nullptr) {
    Serial.printf("[OK] Paired as initiator (code=%06u) — opera_secret "
                  "already persisted before start_pairing\n", code);
    return;
  }
  /* Joiner side: persist FIRST so a power cut between save and set
   * leaves the right NVS state on next boot. THEN set in-memory
   * UNCONDITIONALLY so the active session works even when persistence
   * fails — typical on FE-disabled dev hardware where save refuses
   * by design. Persistence failure should degrade reboot survivability
   * (user has to re-pair after reboot), not disable the current
   * session that just successfully paired. */
  const bool save_ok = mesh_state::save_opera_secret(secret);
  const bool set_ok  = mesh_session::set_opera_secret(secret);

  if (save_ok && set_ok) {
    Serial.printf("[OK] Paired as joiner (code=%06u) — opera_secret "
                  "persisted + active\n", code);
  } else if (set_ok && !save_ok) {
    /* Active session works; reboot won't survive. Most likely FE off. */
    Serial.printf("[WARN] Paired as joiner (code=%06u) — active for this "
                  "boot but NVS persist failed (flash encryption off? see "
                  "mesh_state::save_opera_secret); device will need to "
                  "re-pair after reboot\n", code);
  } else {
    /* set_opera_secret rejection means mesh_session isn't initialized
     * — shouldn't happen here because main.cpp init'd it before
     * registering this callback, but the contract permits a failure. */
    Serial.println("[ERR] Paired but mesh_session rejected opera_secret");
  }

  /* Register the just-paired peer's pubkey so this boot's receive
   * path accepts their BEACON_EVENT frames, AND persist it so a
   * future reboot also accepts them. The same "save first, set
   * unconditionally" posture as opera_secret: a save failure
   * (typically FE off) degrades reboot survivability, not the
   * active session. */
  uint8_t peer_pub[mesh_crypto::PUBKEY_LEN];
  if (mesh_session::get_paired_peer_pubkey(peer_pub)) {
    const bool peer_save_ok = mesh_state::save_trusted_peer(peer_pub);
    const bool peer_set_ok  = mesh_session::register_trusted_peer(peer_pub);
    if (peer_save_ok && peer_set_ok) {
      Serial.println("[OK] Peer pubkey persisted + registered for RX");
    } else if (peer_set_ok && !peer_save_ok) {
      Serial.println("[WARN] Peer registered for this boot but NVS persist "
                     "failed — receive will need to re-pair after reboot");
    } else if (!peer_set_ok && peer_save_ok) {
      /* register_trusted_peer returns false in two distinct cases:
       *   • table full (MAX_TRUSTED_PEERS=8 reached with a new pubkey)
       *   • duplicate registration (pubkey already in the in-memory
       *     table — typical on the post-first-paired-callback path
       *     because save_trusted_peer + the boot-time
       *     load_trusted_peers chain may already have registered it). */
      Serial.println("[WARN] Peer pubkey persisted but mesh_session register "
                     "failed (table full or already registered)");
    } else {
      Serial.println("[ERR] Failed to persist OR register peer pubkey");
    }
  } else {
    Serial.println("[WARN] Paired but mesh_session has no peer pubkey "
                   "available — receive from this peer won't work");
  }
}
#endif

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
// Drops every case variant of the ambiguous glyphs (0/O/o, 1/I/i/l/L) so a
// user can read the password off the serial monitor or sticker without
// guessing. Must stay byte-identical to UNAMBIGUOUS_ALPHABET in
// securacv_crypto.cpp — both are the project's single no-confusion alphabet.
static const char UNAMBIGUOUS_ALPHABET[] =
  "23456789ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz";
static const size_t UNAMBIGUOUS_LEN = sizeof(UNAMBIGUOUS_ALPHABET) - 1;  // 54

static void derive_ap_password(const uint8_t fingerprint[8], char* password, size_t len) {
  char encoded[6];
  size_t chars_produced = 0;
  for (size_t i = 0; chars_produced < 5 && i < 8; i++) {
    uint8_t b = fingerprint[i];
    if (b < 216) { // 216 = 54 * 4, rejection sampling to avoid bias
      encoded[chars_produced++] = UNAMBIGUOUS_ALPHABET[b % UNAMBIGUOUS_LEN];
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

  // Track reset reason for diagnostics and battery health inference
  {
    esp_reset_reason_t rst = esp_reset_reason();
    const char* rst_name = "unknown";
    switch (rst) {
      case ESP_RST_POWERON:  rst_name = "power_on"; break;
      case ESP_RST_SW:       rst_name = "software"; break;
      case ESP_RST_PANIC:    rst_name = "panic"; break;
      case ESP_RST_INT_WDT:  rst_name = "int_wdt"; break;
      case ESP_RST_TASK_WDT: rst_name = "task_wdt"; break;
      case ESP_RST_WDT:      rst_name = "wdt"; break;
      case ESP_RST_DEEPSLEEP: rst_name = "deep_sleep"; break;
      case ESP_RST_BROWNOUT: rst_name = "brownout"; break;
      case ESP_RST_SDIO:     rst_name = "sdio"; break;
      default: break;
    }
    Serial.printf("[..] Reset reason: %s\n", rst_name);
    if (rst == ESP_RST_BROWNOUT || rst == ESP_RST_PANIC ||
        rst == ESP_RST_INT_WDT || rst == ESP_RST_TASK_WDT || rst == ESP_RST_WDT) {
      log_health(LOG_LEVEL_WARNING, LOG_CAT_SYSTEM,
                 "Abnormal reset detected", rst_name);
    }
  }

  // Initialize battery/power monitoring (ADC on GPIO 1 or software inference)
#if FEATURE_POWER_MONITOR
  {
    power_config_t pwr_cfg = POWER_CONFIG_DEFAULT;
    if (power_init(&pwr_cfg)) {
      power_set_event_callback([](const power_event_t* evt) {
        if (evt->event_type == POWER_EVENT_CRITICAL_BATTERY) {
          power_graceful_shutdown();
        }
      });
      if (power_start()) {
        Serial.printf("[OK] Power monitor: %s mode, %u mAh capacity\n",
                      power_is_charging() ? "USB" : "battery",
                      pwr_cfg.capacity_mah);
      } else {
        Serial.println("[WARN] Power monitor start failed");
      }
    } else {
      Serial.println("[WARN] Power monitor init failed");
    }
  }
#endif

  // Initialize power policy engine (auto-adjusts features by battery level)
#if FEATURE_POWER_POLICY
  {
    policy_config_t pol_cfg = POLICY_CONFIG_DEFAULT;
    if (policy_init(&pol_cfg)) {
      policy_set_mode_change_callback([](power_mode_t prev, power_mode_t next) {
        uint8_t payload[64];
        CborWriter cbor(payload, sizeof(payload));
        cbor.write_map(3);
        cbor.write_text("type"); cbor.write_text("power_mode_change");
        cbor.write_text("from"); cbor.write_uint(prev);
        cbor.write_text("to");   cbor.write_uint(next);
        WitnessRecord rec;
        witness_create_record(payload, cbor.size(), RECORD_STATE_CHANGE, &rec);
      });
      Serial.printf("[OK] Power policy engine: %s mode\n",
                    policy_mode_name(policy_get_mode()));
    } else {
      Serial.println("[WARN] Power policy init failed");
    }
  }
#endif

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

  // Initialize first-time setup detection and device naming
#if FEATURE_SETUP_WIZARD
  setup_init();
  {
    char dev_name[SETUP_DEVICE_NAME_MAX + 1];
    if (setup_get_device_name(dev_name, sizeof(dev_name))) {
      Serial.printf("[OK] Device name: %s\n", dev_name);
    }
  }
#endif

  // Start WiFi Access Point and HTTP server
#if FEATURE_WIFI_AP
  {
#if FEATURE_SETUP_WIZARD
    const char* ap_ssid = device.ap_ssid;
    char setup_ssid[32];
    if (setup_is_first_boot()) {
      size_t id_len = strlen(device.device_id);
      const char* suffix = (id_len >= 4) ? (device.device_id + id_len - 4) : device.device_id;
      if (snprintf(setup_ssid, sizeof(setup_ssid), "SecuraCV-%s", suffix) >= (int)sizeof(setup_ssid)) {
        // Defense-in-depth: a longer device_id must never yield a truncated
        // (and therefore ambiguous) SSID. Fall back to a fixed, valid name.
        strncpy(setup_ssid, "SecuraCV-Setup", sizeof(setup_ssid) - 1);
        setup_ssid[sizeof(setup_ssid) - 1] = '\0';
      }
      ap_ssid = setup_ssid;
      Serial.printf("[..] SETUP MODE: AP SSID = %s\n", ap_ssid);
    }
#else
    const char* ap_ssid = device.ap_ssid;
#endif
    Serial.println("[..] Starting WiFi Access Point...");
    ScvNetworkManager& net = network_get_instance();
    if (net.begin(ap_ssid, g_ap_password, device.device_id)) {
      Serial.println("[OK] WiFi AP active");
#if FEATURE_HTTP_SERVER
      Serial.println("[..] Starting HTTP server...");
      net.startHttpServer();
#endif
#if FEATURE_SETUP_WIZARD
      if (setup_is_active()) {
        setup_start_captive_portal();
        Serial.println("[OK] Captive portal active — connect to configure");
      }
#endif
    } else {
      Serial.println("[WARN] WiFi AP failed to start");
    }
  }
#endif

#if FEATURE_MESH_NETWORK
  /* Mesh layer bring-up — must happen AFTER WiFi (ESP-NOW binds the
   * shared radio). mesh_transport handles peer table + ESP-NOW recv
   * ring; mesh_session bridges pairing + opera-authenticated traffic
   * (sign/verify for BEACON_EVENT, HEARTBEAT, etc.).
   *
   * No pairing is triggered here — that's the integration layer's
   * job once the user kicks off "pair another Canary" from the UI.
   * Until pairing succeeds + opera_secret is loaded, mesh_session's
   * send_beacon_event short-circuits (PR 5c-3 contract) and the
   * receive dispatch silently drops unknown senders (PR 5c-4). The
   * SPSC outbound queue in csi_modules_integration.cpp drains
   * harmlessly during that warm-up.
   *
   * Logging only — no health-chain rows for boot-time mesh init;
   * the chokepoint sees nothing if no events are emitted. */
  Serial.println("[..] Starting mesh layer...");
  if (mesh_transport::init(mesh_transport::Config::defaults()) &&
      mesh_transport::start() &&
      mesh_session::init(device.pubkey, device.privkey) &&
      mesh_session::start()) {
    Serial.println("[OK] Mesh layer active (mesh_transport + mesh_session)");

    /* Load the persisted opera_secret (if any) and feed it to
     * mesh_session so this boot can immediately send/receive
     * BEACON_EVENT frames without re-pairing. The local buffer is
     * wiped after the call — mesh_session derives + caches the
     * 16-byte opera_id and 8-byte sender_fp and does NOT retain
     * the 32-byte secret. mesh_state::load_opera_secret returns
     * false on first-boot / factory-reset / FE-disabled devices —
     * those flows leave the firmware in an unpaired state and the
     * Scout broadcast queue drains harmlessly. */
    uint8_t opera_secret_buf[mesh_crypto::OPERA_SECRET_LEN];
    if (mesh_state::load_opera_secret(opera_secret_buf)) {
      if (mesh_session::set_opera_secret(opera_secret_buf)) {
        Serial.println("[OK] Opera secret loaded — mesh broadcast enabled");
      } else {
        Serial.println("[WARN] mesh_session rejected loaded opera_secret");
      }
    }
    /* Wipe UNCONDITIONALLY — even when load_opera_secret returns false,
     * a partial-read on the NVS-failure path could leave sensitive
     * bytes in the buffer. The volatile write loop + memory barrier
     * prevents the compiler from eliminating the wipe via dead-store
     * elimination. */
    {
      volatile uint8_t* p = (volatile uint8_t*)opera_secret_buf;
      for (size_t i = 0; i < sizeof(opera_secret_buf); ++i) p[i] = 0;
#if defined(__GNUC__) || defined(__clang__)
      asm volatile("" ::: "memory");
#endif
    }

    /* Load persisted trusted peers (#480) and register each so this
     * boot's receive path can verify inbound BEACON_EVENT frames
     * from peers paired in previous sessions. Empty-list (first
     * boot / factory reset) is fine — mesh_session::on_opera_frame
     * silently drops unknown senders, and the dashboard / UI will
     * trigger pairing to populate. */
    {
      uint8_t peers_buf[mesh_state::MAX_TRUSTED_PEERS
                        * mesh_crypto::PUBKEY_LEN];
      size_t peers_count = 0;
      if (mesh_state::load_trusted_peers(peers_buf, sizeof(peers_buf),
                                         &peers_count)) {
        size_t registered = 0;
        for (size_t i = 0; i < peers_count; ++i) {
          if (mesh_session::register_trusted_peer(
                  peers_buf + i * mesh_crypto::PUBKEY_LEN)) {
            ++registered;
          }
        }
        if (peers_count > 0) {
          Serial.printf("[OK] Registered %u/%u trusted peer pubkeys from NVS\n",
                        (unsigned)registered, (unsigned)peers_count);
        }
      }
      /* Wipe the local buffer — pubkeys aren't secret per se but a
       * "no useful data on the stack after init" posture matches
       * the opera_secret wipe just above. */
      volatile uint8_t* pp = (volatile uint8_t*)peers_buf;
      for (size_t i = 0; i < sizeof(peers_buf); ++i) pp[i] = 0;
#if defined(__GNUC__) || defined(__clang__)
      asm volatile("" ::: "memory");
#endif
    }

    /* Restore per-peer replay counters from NVS so replay defense
     * survives reboots. Must run AFTER register_trusted_peer so the
     * peer table is populated for restore_replay_counter to look up. */
    {
      mesh_state::ReplayEntry entries[mesh_state::MAX_REPLAY_ENTRIES];
      size_t count = 0;
      if (mesh_state::load_replay_counters(entries,
                                           mesh_state::MAX_REPLAY_ENTRIES,
                                           &count)) {
        size_t restored = 0;
        for (size_t i = 0; i < count; ++i) {
          if (mesh_session::restore_replay_counter(
                  entries[i].fingerprint, entries[i].last_counter)) {
            ++restored;
          }
        }
        if (count > 0) {
          Serial.printf("[OK] Restored %u/%u replay counters from NVS\n",
                        (unsigned)restored, (unsigned)count);
        }
      }
    }

    /* Wire the PairedCallback so a successful pairing flow persists
     * the opera_secret + peer pubkey to NVS and immediately seeds
     * mesh_session with them. Only the joiner-side callback receives
     * a non-null secret (the initiator already had it before starting
     * pairing; its persistence is the integration layer's
     * responsibility before calling start_pairing_initiator). */
    mesh_session::set_paired_callback(&on_pairing_succeeded);
  } else {
    Serial.println("[WARN] Mesh layer init failed — broadcast disabled");
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
    (FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_TEMP_TAMPER || FEATURE_VISION_DETECT)
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
         we->kind == SENSING_WITNESS_TEMP_DRIFT  ||
         we->kind == SENSING_WITNESS_VISION_TAMPER)
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

  // Bring up the NimBLE stack BEFORE the CSI pipeline below. The RX-only BLE
  // Scout is registered inside securacv_csi_modules_init() and only ATTACHES to
  // an already-running stack; the BLE GATT status service is the single init
  // owner (it owns the GAP device name + TX power). Initializing it here — ahead
  // of the Scout — guarantees the device advertises under its configured name
  // rather than the Scout's generic "securacv-scout". (Full GATT service
  // creation + advertising still happens later in ble_status_init(); this only
  // owns the stack bring-up.)
#if FEATURE_BLE_STATUS
  if (!ble_status_stack_begin()) {
    Serial.println("[--] NimBLE stack init failed — BLE status/scout unavailable");
  }
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

  // Initialize vision detection pipeline
#if FEATURE_VISION_DETECT
  Serial.println("[..] Initializing vision detection...");
  sensing_init();
  vision_config_t vision_cfg = VISION_CONFIG_DEFAULT;
  if (vision_load_config_from_nvs(&vision_cfg)) {
    Serial.println("[OK] Vision config loaded from NVS");
  }
  if (vision_init(&vision_cfg)) {
    vision_set_event_callback([](const vision_event_t* evt) {
      sensing_feed_vision_event(evt->event_type, evt->confidence,
                                evt->zone, evt->time_bucket);
    });
    if (vision_start()) {
      Serial.println("[OK] Vision detection armed (3-layer cascade)");
    } else {
      Serial.println("[WARN] Vision detection start failed");
    }
  } else {
    Serial.println("[WARN] Vision detection init failed");
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

  // Initialize diagnostics and run boot self-test
#if FEATURE_DIAGNOSTICS
  diag_init();
  {
    uint8_t score = diag_run_selftest();
    Serial.printf("[OK] Self-test: %u%% health score\n", score);
    if (score < 70) {
      log_health(LOG_LEVEL_WARNING, LOG_CAT_SYSTEM,
                 "Low self-test score", nullptr);
    }
  }
#endif

  // Initialize data management (log rotation, chain backup, integrity)
#if FEATURE_DATA_MGMT
  if (datamgmt_init()) {
    datamgmt_stats_t dm_stats;
    if (datamgmt_get_stats(&dm_stats)) {
      Serial.printf("[OK] Data mgmt: %u witness, %u health files, backup=%s\n",
                    (unsigned)dm_stats.witness_files,
                    (unsigned)dm_stats.health_files,
                    dm_stats.backup_exists ? "yes" : "no");
    }
  } else {
    Serial.println("[WARN] Data management init failed");
  }
#endif

  // Initialize BLE GATT status service (battery, health, chain over BLE)
#if FEATURE_BLE_STATUS
  Serial.println("[..] Initializing BLE status service...");
  if (ble_status_init()) {
    Serial.println("[OK] BLE GATT status service active");
  } else {
    Serial.println("[WARN] BLE status service init failed");
  }
#endif

  // Enable WiFi modem sleep when running on battery to save ~20 mA
#if FEATURE_WIFI_AP && FEATURE_POWER_MONITOR
  {
    power_state_t pwr_ps;
    if (power_get_state(&pwr_ps) &&
        pwr_ps.power_source == POWER_SOURCE_BATTERY) {
      network_set_wifi_power_save(true);
      Serial.println("[OK] WiFi modem sleep enabled (battery power)");
    }
  }
#endif

  g_last_record_ms = millis();

  // Print ready banner
  Serial.println();
  Serial.println("╔══════════════════════════════════════════════════════════════╗");
  Serial.println("║               WITNESS DEVICE READY                           ║");
  Serial.println("╠══════════════════════════════════════════════════════════════╣");
  Serial.printf("║  Device ID  : %-45s  ║\n", device.device_id);
#if FEATURE_SETUP_WIZARD
  {
    char banner_name[SETUP_DEVICE_NAME_MAX + 1];
    if (setup_get_device_name(banner_name, sizeof(banner_name))) {
      Serial.printf("║  Name       : %-45s  ║\n", banner_name);
    }
  }
#endif
#if FEATURE_WIFI_AP
  ScvNetworkManager& network = network_get_instance();
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
#if FEATURE_POWER_MONITOR
  {
    power_state_t pwr;
    if (power_get_state(&pwr) && pwr.divider_detected) {
      char batt_str[48];
      snprintf(batt_str, sizeof(batt_str), "%u mV (%u%%) %s",
               pwr.voltage_mv, pwr.soc_pct,
               pwr.charge_state == 1 ? "charging" :
               pwr.charge_state == 2 ? "full" : "on battery");
      Serial.printf("║  Battery  : %-45s  ║\n", batt_str);
    } else {
      Serial.printf("║  Battery  : %-45s  ║\n", "monitoring (SW inference)");
    }
  }
#endif
  Serial.println("╠══════════════════════════════════════════════════════════════╣");
  Serial.println("║  Commands: h=help, i=identity, s=status, g=gps, r=data       ║");
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

#if FEATURE_MESH_NETWORK
  /* Drive the mesh stack from the main loop:
   *   mesh_transport::process() drains the ESP-NOW recv ring and ages
   *     peer state. Calling this is what surfaces inbound frames to
   *     mesh_session::on_transport_recv → on_opera_frame (PR 5c-4).
   *   mesh_session::process(now_ms) drives mesh_pairing::tick (5-min
   *     pairing timeout + initiator NOTIFY_PAIRED) and dispatches any
   *     pending actions.
   * Both are no-ops until init() succeeds. */
  mesh_transport::process();
  mesh_session::process((uint32_t)millis());

  {
    static uint32_t s_last_replay_save_ms = 0;
    const uint32_t now = millis();
    if ((int32_t)(now - s_last_replay_save_ms) >= 300000) {
      s_last_replay_save_ms = now;
      persist_replay_counters();
    }
  }
#endif

  // Handle serial commands
  handle_serial_commands();

  // Handle boot button (info print, factory reset)
  handle_boot_button();

#if FEATURE_SETUP_WIZARD
  if (setup_is_active()) {
    setup_dns_process();
    setup_check_timeout();
    if (WiFi.status() == WL_CONNECTED) {
      setup_mark_complete();
      Serial.println("[OK] Setup complete — WiFi connected, rebooting...");
      delay(1000);
      ESP.restart();
    }
  }
#endif

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

  // Runtime policy gates — the policy engine decides which sensors run
  // based on battery state. If policy isn't compiled in, all features
  // run unconditionally (same as PMODE_PLUGGED_IN).
  // Heap degradation provides a second gate: under memory pressure,
  // memory-hungry features are shed even if the policy allows them.
#if FEATURE_POWER_POLICY
  const policy_features_t* pf = policy_get_features();
#endif
#if FEATURE_DIAGNOSTICS
  degrade_level_t dl = diag_get_degrade_level();
#endif

  // Memory-hungry features: shed at CRITICAL or above.
#if FEATURE_CSI
  #if FEATURE_POWER_POLICY
  if (pf->csi)
  #endif
  #if FEATURE_DIAGNOSTICS
  if (dl < DEGRADE_CRITICAL)
  #endif
  csi::process();
#endif

#if FEATURE_ACOUSTIC_EVENTS
  #if FEATURE_POWER_POLICY
  if (pf->acoustic)
  #endif
  #if FEATURE_DIAGNOSTICS
  if (dl < DEGRADE_CRITICAL)
  #endif
  audio_process();
#endif

#if FEATURE_VISION_DETECT
  #if FEATURE_POWER_POLICY
  if (pf->vision)
  #endif
  #if FEATURE_DIAGNOSTICS
  if (dl < DEGRADE_CRITICAL)
  #endif
  vision_process();
#endif

  // Lightweight features: shed only at EMERGENCY.
#if FEATURE_TOUCH
  #if FEATURE_POWER_POLICY
  if (pf->touch)
  #endif
  #if FEATURE_DIAGNOSTICS
  if (dl < DEGRADE_EMERGENCY)
  #endif
  touch_process();
#endif

#if FEATURE_IR_RMT
  #if FEATURE_POWER_POLICY
  if (pf->ir_rmt)
  #endif
  #if FEATURE_DIAGNOSTICS
  if (dl < DEGRADE_EMERGENCY)
  #endif
  ir_process();
#endif

#if FEATURE_TEMP_TAMPER
  #if FEATURE_POWER_POLICY
  if (pf->temp_tamper)
  #endif
  #if FEATURE_DIAGNOSTICS
  if (dl < DEGRADE_EMERGENCY)
  #endif
  envsens_process();
#endif

#if FEATURE_POWER_MONITOR
  power_process();
  {
    power_state_t pwr;
    if (power_get_state(&pwr)) {
      health.battery_mv    = pwr.voltage_mv;
      health.battery_soc   = pwr.soc_pct;
      health.charge_state  = pwr.charge_state;
      health.power_source  = pwr.power_source;
      health.battery_trend = pwr.trend_mv_per_min;
    }
  }
  /* Persist battery health history every 10 minutes. */
  {
    static uint32_t s_last_batt_hist_ms = 0;
    if ((int32_t)(now - s_last_batt_hist_ms) >= 600000) {
      s_last_batt_hist_ms = now;
      power_persist_history();
    }
  }
#endif

#if FEATURE_POWER_POLICY
  policy_process();

  if (policy_should_deep_sleep() && !power_is_charging()) {
    uint32_t sleep_sec = policy_get_sleep_duration_sec();
    {
      power_state_t pwr;
      power_get_state(&pwr);
      uint8_t sl_payload[64];
      CborWriter sl_cbor(sl_payload, sizeof(sl_payload));
      sl_cbor.write_map(3);
      sl_cbor.write_text("type"); sl_cbor.write_text("deep_sleep_entry");
      sl_cbor.write_text("soc");  sl_cbor.write_uint(pwr.soc_pct);
      sl_cbor.write_text("dur");  sl_cbor.write_uint(sleep_sec);
      WitnessRecord sl_rec;
      witness_create_record(sl_payload, sl_cbor.size(),
                            RECORD_STATE_CHANGE, &sl_rec);
    }
    witness_persist_chain_state();
    lowpower_arm_wake_timer((uint64_t)sleep_sec * 1000000ULL);
    lowpower_arm_wake_touch();
    policy_ack_deep_sleep();
    lowpower_enter_deep_sleep();
  } else if (policy_should_deep_sleep()) {
    policy_ack_deep_sleep();
  }
#endif

#if FEATURE_DIAGNOSTICS
  diag_process();
#endif

#if FEATURE_DATA_MGMT
  datamgmt_process();
#endif

#if FEATURE_BLE_STATUS
  ble_status_update();
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
  {
#if FEATURE_POWER_POLICY
    uint32_t rec_interval = policy_get_features()->record_interval_ms;
#else
    uint32_t rec_interval = RECORD_INTERVAL_MS;
#endif
  if (now - g_last_record_ms >= rec_interval) {
    g_last_record_ms = now;

    // Build witness event payload
    uint8_t payload[256];
    CborWriter cbor(payload, sizeof(payload));

    FixState state = witness_get_state();

    cbor.write_map(7);
    cbor.write_text("state"); cbor.write_text(state_name_short(state));
    cbor.write_text("fix"); cbor.write_bool(fix.valid);
    cbor.write_text("lat"); cbor.write_float(gps_coarsen_deg(fix.lat));
    cbor.write_text("lon"); cbor.write_float(gps_coarsen_deg(fix.lon));
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
  } /* rec_interval scope */
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
    doc["lat"] = gps_coarsen_deg(fix.lat);
    doc["lon"] = gps_coarsen_deg(fix.lon);
    doc["satellites"] = fix.satellites;
  }
  doc["sd_healthy"] = health.sd_healthy;
  doc["chain_valid"] = (health.verify_failures == 0);
  doc["firmware"] = FIRMWARE_VERSION;
#if FEATURE_POWER_MONITOR
  doc["battery_mv"] = health.battery_mv;
  doc["battery_soc"] = health.battery_soc;
  {
    const char* cs = "unknown";
    switch (health.charge_state) {
      case 1: cs = "charging"; break;
      case 2: cs = "full"; break;
      case 3: cs = "discharging"; break;
      case 4: cs = "low"; break;
      case 5: cs = "critical"; break;
      case 6: cs = "no_battery"; break;
    }
    doc["charge_state"] = cs;
  }
#endif

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
#if FEATURE_POWER_MONITOR
  doc["battery_mv"] = health.battery_mv;
  doc["battery_soc"] = health.battery_soc;
  doc["battery_trend"] = health.battery_trend;
  doc["charge_cycles"] = 0;
  {
    power_state_t pwr;
    if (power_get_state(&pwr)) {
      doc["charge_cycles"] = pwr.charge_cycles;
    }
  }
#endif

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

#if FEATURE_POWER_POLICY
  doc["power_mode"] = policy_mode_name(policy_get_mode());
#endif

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
#if FEATURE_DATA_MGMT
      Serial.println("  r - Data management (rotation, backup, files)");
#endif
#if FEATURE_POWER_MONITOR
      Serial.println("  b - Battery status");
#endif
#if FEATURE_POWER_POLICY
      Serial.println("  p - Power policy status");
#endif
#if FEATURE_DIAGNOSTICS
      Serial.println("  d - Diagnostics (heap, SD, self-test)");
#endif
#if FEATURE_HA_MQTT
      Serial.println("  m - MQTT status");
#endif
      Serial.println("  x - Reboot");
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
        Serial.printf("  Lat: %.3f\n", gps_coarsen_deg(fix.lat));
        Serial.printf("  Lon: %.3f\n", gps_coarsen_deg(fix.lon));
        Serial.printf("  Alt: %.1f m\n", fix.altitude_m);
        Serial.printf("  Speed: %.1f km/h\n", fix.speed_kmh);
        Serial.printf("  Sats: %u\n", fix.satellites);
      }
      Serial.printf("  Sentences: %u (errors: %u)\n",
                    s_gps.getSentenceCount(), s_gps.getChecksumErrors());
      Serial.println();
      break;
    }

#if FEATURE_POWER_MONITOR
    case 'b':
    case 'B': {
      power_state_t pwr;
      Serial.println("\n=== Battery ===");
      if (power_get_state(&pwr)) {
        Serial.printf("  Mode: %s\n", pwr.divider_detected ? "HW ADC" : "SW inference");
        Serial.printf("  Voltage: %u mV\n", pwr.voltage_mv);
        Serial.printf("  SoC: %u%%\n", pwr.soc_pct);
        const char* cs = "unknown";
        switch (pwr.charge_state) {
          case 1: cs = "charging"; break;
          case 2: cs = "full"; break;
          case 3: cs = "discharging"; break;
          case 4: cs = "low"; break;
          case 5: cs = "critical"; break;
      case 6: cs = "no_battery"; break;
        }
        Serial.printf("  State: %s\n", cs);
        Serial.printf("  Trend: %+d mV/min\n", pwr.trend_mv_per_min);
        Serial.printf("  Capacity: %u mAh\n", pwr.capacity_mah);
        Serial.printf("  Cycles: %u\n", pwr.charge_cycles);
        Serial.printf("  Extremes: %u–%u mV\n",
                      pwr.min_voltage_mv == 0xFFFF ? 0 : pwr.min_voltage_mv,
                      pwr.max_voltage_mv);
        Serial.printf("  Samples: %u\n", pwr.samples_taken);
      } else {
        Serial.println("  Not initialized");
      }
      Serial.println();
      break;
    }
#endif

#if FEATURE_POWER_POLICY
    case 'p':
    case 'P': {
      policy_state_t pol;
      Serial.println("\n=== Power Policy ===");
      if (policy_get_state(&pol)) {
        Serial.printf("  Mode: %s\n", policy_mode_name((power_mode_t)pol.mode));
        Serial.printf("  Previous: %s\n", policy_mode_name((power_mode_t)pol.prev_mode));
        Serial.printf("  Auto: %s\n", pol.auto_mode ? "yes" : "no (manual override)");
        Serial.printf("  Transitions: %u\n", pol.transitions);
        Serial.printf("  Record interval: %u ms\n", pol.features.record_interval_ms);
        Serial.printf("  CPU freq: %u MHz\n", pol.features.cpu_freq_mhz);
        Serial.printf("  WiFi PS: %u\n", pol.features.wifi_ps_mode);
        Serial.println("  Features:");
        Serial.printf("    WiFi=%d CSI=%d Audio=%d Touch=%d IR=%d\n",
                      pol.features.wifi_ap, pol.features.csi,
                      pol.features.acoustic, pol.features.touch,
                      pol.features.ir_rmt);
        Serial.printf("    Camera=%d Vision=%d GPS=%d MQTT=%d Mesh=%d\n",
                      pol.features.camera_peek, pol.features.vision,
                      pol.features.gnss, pol.features.mqtt,
                      pol.features.mesh);
      } else {
        Serial.println("  Not initialized");
      }
      Serial.println();
      break;
    }
#endif

#if FEATURE_DIAGNOSTICS
    case 'd':
    case 'D': {
      diag_snapshot_t snap;
      Serial.println("\n=== Diagnostics ===");
      if (diag_get_snapshot(&snap)) {
        Serial.printf("  Heap: %u free, %u min, %u largest block\n",
                      snap.heap.free_heap, snap.heap.min_heap,
                      snap.heap.largest_block);
        Serial.printf("  PSRAM: %u / %u free\n",
                      snap.heap.psram_free, snap.heap.psram_total);
        Serial.printf("  Fragmentation: %u%%\n", snap.heap.fragmentation_pct);
        Serial.printf("  Stack HWM: %u words\n", snap.heap.stack_hwm_main);
        const char* dlabel = "none";
        switch (snap.heap.degrade_level) {
          case DEGRADE_WARN:      dlabel = "warn"; break;
          case DEGRADE_CRITICAL:  dlabel = "critical"; break;
          case DEGRADE_EMERGENCY: dlabel = "emergency"; break;
        }
        Serial.printf("  Degradation: %s\n", dlabel);
        Serial.printf("  SD: %s (%u%% used, %u writes, %u errors)\n",
                      snap.sd.mounted ? "mounted" : "not mounted",
                      snap.sd.usage_pct, snap.sd.total_writes,
                      snap.sd.write_errors);
        if (snap.selftest.has_run) {
          Serial.printf("  Self-test: %u/%u passed (%u%% health)\n",
                        snap.selftest.passed_count, snap.selftest.total_count,
                        snap.selftest.health_score);
        }
        Serial.printf("  Reset reason: %u\n", snap.reset_reason);
      }
      Serial.println();
      break;
    }
#endif

#if FEATURE_HA_MQTT
    case 'm':
    case 'M':
      Serial.println("\n=== MQTT ===");
      Serial.printf("  Connected: %s\n", mqtt_connected() ? "Yes" : "No");
      Serial.println();
      break;
#endif

#if FEATURE_DATA_MGMT
    case 'r':
    case 'R': {
      datamgmt_stats_t dm;
      Serial.println("\n=== Data Management ===");
      if (datamgmt_get_stats(&dm)) {
        Serial.printf("  Witness files: %u (max %u)\n",
                      (unsigned)dm.witness_files, DATAMGMT_MAX_WITNESS_FILES);
        Serial.printf("  Health files:  %u (max %u)\n",
                      (unsigned)dm.health_files, DATAMGMT_MAX_HEALTH_FILES);
        Serial.printf("  Files rotated: %u total\n",
                      (unsigned)dm.files_rotated_total);
        if (dm.last_rotation_ms > 0) {
          uint32_t ago = (millis() - dm.last_rotation_ms) / 1000;
          Serial.printf("  Last rotation: %us ago\n", (unsigned)ago);
        } else {
          Serial.println("  Last rotation: none");
        }
        Serial.printf("  Chain backup:  %s\n",
                      dm.backup_exists ? "yes" : "no");
        if (dm.last_backup_ms > 0) {
          uint32_t ago = (millis() - dm.last_backup_ms) / 1000;
          Serial.printf("  Last backup:   %us ago\n", (unsigned)ago);
        }
      } else {
        Serial.println("  Not initialized");
      }
      Serial.println();
      break;
    }
#endif

    case 'x':
    case 'X':
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
    Serial.printf(" (%.3f, %.3f, %u sats)", gps_coarsen_deg(fix.lat), gps_coarsen_deg(fix.lon), fix.satellites);
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

#if FEATURE_POWER_POLICY
  Serial.printf("  Power mode: %s\n", policy_mode_name(policy_get_mode()));
#endif

#if FEATURE_POWER_MONITOR
  {
    power_state_t pwr;
    if (power_get_state(&pwr)) {
      const char* cs = "unknown";
      switch (pwr.charge_state) {
        case 1: cs = "charging"; break;
        case 2: cs = "full"; break;
        case 3: cs = "discharging"; break;
        case 4: cs = "low"; break;
        case 5: cs = "critical"; break;
      case 6: cs = "no_battery"; break;
      }
      Serial.printf("  Battery: %u mV (%u%%) [%s]",
                    pwr.voltage_mv, pwr.soc_pct, cs);
      if (pwr.divider_detected) {
        Serial.print(" (HW ADC)");
      } else {
        Serial.print(" (SW inference)");
      }
      Serial.println();
      Serial.printf("  Trend: %+d mV/min, cycles: %u\n",
                    pwr.trend_mv_per_min, pwr.charge_cycles);
    }
  }
#endif

  Serial.println();
}
