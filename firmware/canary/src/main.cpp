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
#include <sys/time.h>
#include <cstdlib>
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

#if FEATURE_OTA_PULL || FEATURE_OTA_UPDATE
#include "securacv_ota.h"
#include "ota_release_key.h"
#endif
#if FEATURE_OTA_PULL
#include <WiFi.h>
#if __has_include(<esp_random.h>)
#include <esp_random.h>
#endif
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

// Power-event resilience: boot-lineage classification + the durable outage log,
// feeding the shared host-tested core (firmware/common/power/power_events.h).
// Unconditional — the "when did the power go out" record isn't gated on the
// battery monitor. See docs/design/power_events.md.
#include "canary_power_events.h"

#if FEATURE_SETUP_WIZARD
#include "securacv_setup.h"
#include <WiFi.h>
#endif

#if FEATURE_DIAGNOSTICS
#include "securacv_diagnostics.h"
#endif

#include "securacv_thermal.h"

#if FEATURE_THERMAL_WATCHDOG
#include "securacv_thermal_watchdog.h"
#include <math.h>   /* lroundf for MQTT whole-degree rounding */
#endif

#if FEATURE_DATA_MGMT
#include "securacv_data_mgmt.h"
#endif

#if FEATURE_BLE_STATUS
#include "securacv_ble_status.h"
#endif

#if FEATURE_USB_ONBOARD
#include "securacv_usb_onboard.h"
#endif

// Pure, board-agnostic test-console policy + BLE bring-up ladder (no deps).
// Used by the read-only 't' run-all command below.
#include "health/test_console.h"

// Pure, host-tested device self-manifest builder: the machine-readable JSON the
// 'j' command emits (public-only) so a browser can read this unit over WebSerial
// — draw the same randomart from the pubkey, and show exactly the tools it has.
#include "attest/self_manifest.h"

#if FEATURE_CONSOLE_THEME
// Pure, host-tested console scene engine: the 'l' identity banner (key
// fingerprint as randomart). ASCII-safe by default; see docs/design/serial_console_theming.md.
#include "ui/randomart.h"
#include "ui/console_theme.h"
#include "ui/console_scenes.h"
#include "ui/console_wake.h"
static void print_boot_welcome();   // the friendly char-box hello on connect
#endif

// The fleet view: the 'n' nearby-fleet card (needs the console engine) AND the
// pure roster->peer helpers (fleet_peer_from_entry / fleet_fmt_flags) that the
// 'j' manifest's fleet[] reuses. Pull it in whenever EITHER the console card
// (FEATURE_CONSOLE_THEME) or the manifest fleet[] (FEATURE_BLE_SCAN) needs it;
// fleet_view.h is self-contained (it includes the console engine it depends on).
#if FEATURE_CONSOLE_THEME || (defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN)
#include "ui/fleet_view.h"
#endif
#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
#include "fleet_roster_feed.h"        // the live roster the 'n' card + fleet[] read
#endif

// Optional ESP-IDF provenance APIs for the 'f' fingerprint command. Guarded by
// __has_include so a toolchain without them still builds — the fields just show
// "unknown" (the same defensive pattern main.cpp uses for <esp_random.h>).
#if __has_include(<esp_ota_ops.h>)
#include <esp_ota_ops.h>
#define HAVE_OTA_PARTITION 1
#endif
#if __has_include(<esp_secure_boot.h>)
#include <esp_secure_boot.h>
#define HAVE_SECURE_BOOT 1
#endif
#if __has_include(<esp_flash_encrypt.h>)
#include <esp_flash_encrypt.h>
#define HAVE_FLASH_ENCRYPT 1
#endif
// esp_random() for the 'c' attestation's device-generated nonce fallback. Only
// conditionally pulled in above (under FEATURE_OTA_PULL), so include it here too
// whenever it exists — the header guard makes the double-include a no-op.
#if __has_include(<esp_random.h>)
#include <esp_random.h>
#define HAVE_ESP_RANDOM 1
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

// ════════════════════════════════════════════════════════════════════════════
// GPS-DERIVED SYSTEM CLOCK
// ════════════════════════════════════════════════════════════════════════════
//
// This project has no SNTP path — no WiFi-based time sync exists anywhere in
// canary/canary-wap firmware. Without it the system clock never leaves its
// post-boot state, so wall-clock seconds since 1970 stay meaninglessly small
// (well under WALL_CLOCK_FLOOR) for the device's entire life. GPS is the one
// clock source these devices actually have that isn't derived from an
// untrusted network peer: the L76K reports UTC directly from the satellite
// constellation. syncClockFromGps() seeds the system clock from it once a
// fix carries a validated RMC date/time, and re-checks periodically to correct
// crystal drift over long uptimes. GpsUtcTime.valid latches true on the first
// good RMC and is never cleared by a later void/stale sentence (other
// diagnostics consumers rely on that latch), so this checks last_seen_ms
// itself — a fix isn't "available" for a resync once GPS has been lost, or
// this would keep replaying a stale epoch every 10 minutes and freeze/rewind
// wall time after ordinary GNSS loss.
static const time_t WALL_CLOCK_FLOOR = 1700000000;  // ~2023-11-14; below this, "unset"
static const uint32_t CLOCK_RESYNC_INTERVAL_MS = 10UL * 60UL * 1000UL;  // drift correction
static const uint32_t GPS_FIX_STALE_MS = 30UL * 1000UL;  // RMC arrives ~1 Hz

static void syncClockFromGps() {
  static uint32_t s_last_sync_attempt_ms = 0;
  uint32_t now_ms = millis();

  time_t sys_now = time(nullptr);
  bool clock_set = (sys_now >= WALL_CLOCK_FLOOR);

  // The first believable clock is also the first chance this device has ever
  // had to know its own birthday — the key was generated long before any clock
  // existed. Offered here rather than in the loop because this is the one
  // function that knows the clock is real; the recorder stamps once for the
  // life of the key and costs a comparison on every call after that.
  if (clock_set) witness_note_wall_clock((uint32_t)sys_now);

  if (clock_set && (now_ms - s_last_sync_attempt_ms) < CLOCK_RESYNC_INTERVAL_MS) {
    return;  // already trustworthy and not due for a drift-correction check
  }

  const GpsUtcTime& utc = s_gps.getUtcTime();
  if (!utc.valid) return;
  if ((now_ms - utc.last_seen_ms) > GPS_FIX_STALE_MS) return;  // GPS lost

  time_t gps_epoch;
  if (!gps_utc_to_epoch(utc, &gps_epoch)) return;
  if (gps_epoch < WALL_CLOCK_FLOOR) return;  // receiver clock itself looks unset/wrong

  s_last_sync_attempt_ms = now_ms;

  // First sync, or periodic drift correction: only step the clock when GPS
  // disagrees by more than a second, so this isn't calling settimeofday()
  // on every 10-minute tick once the two clocks agree.
  if (clock_set && llabs((long long)(gps_epoch - sys_now)) < 2) return;

  struct timeval tv = { .tv_sec = gps_epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  Serial.printf("[CLOCK] system clock %s from GPS (epoch=%lld)\n",
                clock_set ? "corrected" : "set", (long long)gps_epoch);

  // The clock only just became real on the "set" path — offer it now rather
  // than waiting for the next call, so a device that gets one fix and then
  // loses the sky still records the day it was dated.
  witness_note_wall_clock((uint32_t)gps_epoch);
}

#if FEATURE_HA_MQTT
static uint32_t g_last_mqtt_status_ms = 0;
static uint32_t g_last_mqtt_health_ms = 0;
static uint32_t g_last_mqtt_sensing_ms = 0;
#endif

#if FEATURE_OTA_PULL
/* Set by the OTA progress callback (which runs on the OTA task and must
 * stay non-blocking); the main loop turns it into an MQTT publish. */
static volatile bool g_ota_publish_pending = false;
static securacv_ota_state_t g_ota_last_seen_state = SECURACV_OTA_IDLE;
static uint32_t g_ota_next_check_ms = 0;
#endif

#if FEATURE_HA_MQTT && FEATURE_SENSING_WITNESS
/* Pending tamper alert for the dedicated securacv/{id}/tamper topic.
 * Set from the sensing witness callback (must stay non-blocking, same
 * rule as the OTA flag above); the main loop turns it into an MQTT
 * publish that the host's mqtt_sensor adapter routes into the sealed
 * witness log as a TamperDetected event. Single-slot: if a second
 * tamper fires before the loop drains the first, the newest wins —
 * the host dedups per 10-minute bucket anyway. */
static volatile bool g_tamper_publish_pending = false;
static volatile uint8_t g_tamper_pending_kind = 0;       /* sensing_witness_kind_t */
static volatile uint8_t g_tamper_pending_confidence = 0; /* 0..100 */
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
#if FEATURE_OTA_PULL
static void mqtt_publish_ota_update_state();
#endif
#endif

// ════════════════════════════════════════════════════════════════════════════
// PULL-OTA INTEGRATION (signed firmware updates)
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_OTA_PULL || FEATURE_OTA_UPDATE

/* Sign an update lifecycle event into the witness chain. The chain is the
 * audit trail: a verifier can later prove when the firmware changed and
 * whether a rollback happened. Compiled whenever ANY install channel
 * exists — the boot-outcome witness in setup() uses it too. */
static void ota_witness_event(const char* type, const char* version) {
  uint8_t payload[96];
  CborWriter cbor(payload, sizeof(payload));
  cbor.write_map(2);
  cbor.write_text("type"); cbor.write_text(type);
  cbor.write_text("ver");  cbor.write_text(version);
  WitnessRecord rec;
  if (!witness_create_record(payload, cbor.size(), RECORD_STATE_CHANGE, &rec)) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_WITNESS, "OTA witness record failed", type);
  }
}

#endif // FEATURE_OTA_PULL || FEATURE_OTA_UPDATE

#if FEATURE_OTA_PULL

/* Runs on the OTA task — only flips a flag; the main loop publishes. */
static void ota_on_progress(securacv_ota_state_t state, uint8_t percent,
                            securacv_ota_error_t error, void* user_data) {
  (void)state; (void)percent; (void)error; (void)user_data;
  g_ota_publish_pending = true;
}

/* Install-permission gate: never start (or finish) an update the device
 * might not survive. Checked before the download and again before the
 * reboot. */
static bool ota_can_install(char* reason, size_t reason_len) {
#if FEATURE_POWER_MONITOR
  power_state_t pwr;
  if (power_get_state(&pwr) &&
      pwr.power_source == POWER_SOURCE_BATTERY && pwr.soc_pct < 30) {
    snprintf(reason, reason_len, "battery too low (%u%%)", pwr.soc_pct);
    return false;
  }
#else
  (void)reason; (void)reason_len;
#endif
  return true;
}

/* Mirror the manual-reboot path before the post-install restart: persist
 * the chain head so the witness chain continues seamlessly on the new
 * firmware. (The engine records the install target itself the moment the
 * boot partition flips, so deferred or indirect reboots are covered too.) */
static void ota_before_reboot() {
  witness_persist_chain_state();
}

static void ota_schedule_next_check(uint32_t delay_ms, uint32_t jitter_ms) {
  uint32_t jitter = (jitter_ms > 0) ? (esp_random() % jitter_ms) : 0;
  g_ota_next_check_ms = millis() + delay_ms + jitter;
}

/* Daily jittered update check. The jitter spreads a fleet's checks over
 * an hour so a release never sees a thundering herd, and the first check
 * lands a couple of minutes after boot so a fresh install learns about
 * updates right away. */
static void ota_scheduler_process(uint32_t now) {
  if (g_ota_next_check_ms == 0) {
    ota_schedule_next_check(120000UL, 60000UL);
    return;
  }
  if ((int32_t)(now - g_ota_next_check_ms) < 0) return;

  if (securacv_ota_get_state() != SECURACV_OTA_IDLE || !WiFi.isConnected()) {
    // Busy or no route to the update server yet — try again in 15 min.
    ota_schedule_next_check(15UL * 60 * 1000, 60000UL);
    return;
  }

  ota_schedule_next_check(24UL * 60 * 60 * 1000, 3600000UL);
  if (securacv_ota_get_auto_update()) {
    securacv_ota_check_and_install();
  } else {
    securacv_ota_check();
  }
}

#endif // FEATURE_OTA_PULL

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

  // Power-event lineage: classify how the last power session ended (brownout /
  // clean reboot / restored outage / fault) and append it to the durable log —
  // the honest "when did the power go out" record. Runs here, before risky
  // init, so an outage is recorded even if a later stage faults.
  canary_pe::on_boot();

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
    // Reconcile the chain head with the durable SD log BEFORE the first
    // record of this boot is created: NVS persists only every
    // SD_PERSIST_INTERVAL records, so after a power cut the cached head
    // can be behind the last record actually signed — resuming from it
    // would fork the append-only chain.
    if (witness_recover_from_sd()) {
      Serial.printf("[OK] Witness chain head recovered from SD (seq %u)\n",
                    (unsigned)witness_get_device().seq);
    }
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

  // USB "plug me in" onboarding (opt-in USB-OTG build). Brings up the HID
  // keyboard (idle — types nothing until a BOOT-button confirm) and, when the
  // SD is mounted, exposes it read-only so the witness files are browsable.
#if FEATURE_USB_ONBOARD
  {
    usb_onboard::Config oc;
    oc.help_url_base = SECURACV_HELP_URL_BASE;
    oc.device_id     = witness_get_device().device_id;
    oc.expose_msc    = storage_is_mounted();
    usb_onboard::begin(oc);
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
      // The captive DNS redirector runs for the AP's LIFETIME, not just first
      // boot (LESSONS_LEARNED, "captive DNS runs for the AP's lifetime"): the
      // OS probe domains only resolve to us while the hijack is live, and the
      // probe handlers answer correctly in every state — the setup wizard
      // while unprovisioned or offline, the platform success tokens once the
      // home Wi-Fi is up. Safe because the softAP has no upstream anyway, and
      // the device's own lookups use the STA resolver, never this listener.
      setup_start_captive_portal();
      if (setup_is_active()) {
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

  #if FEATURE_OTA_PULL
  /* HA update entity "Install" button + auto-update switch. Both
   * callbacks run on the main task (PubSubClient pumps from mqtt_loop),
   * so calling into the OTA engine is safe — it only spawns its task. */
  mqtt_set_ota_install_cmd_callback([]() {
    log_health(LOG_LEVEL_NOTICE, LOG_CAT_USER,
               "Firmware install requested from Home Assistant", nullptr);
    securacv_ota_check_and_install();
  });
  mqtt_set_ota_auto_cmd_callback([](bool enabled) {
    securacv_ota_set_auto_update(enabled);
    mqtt_publish_update_auto_state(enabled);
    log_health(LOG_LEVEL_INFO, LOG_CAT_USER,
               enabled ? "Auto-update turned on" : "Auto-update turned off",
               nullptr);
  });
  /* Seed the cached states so the first broker connect publishes them
   * (same pattern as the mic mute switch above). */
  mqtt_publish_update_auto_state(securacv_ota_get_auto_update());
  #endif
#endif

  // Initialize the signed pull-OTA engine (manifest checks run on the
  // daily scheduler in loop(); installs are user- or auto-triggered).
#if FEATURE_OTA_PULL
  {
    securacv_ota_config_t ota_cfg = SECURACV_OTA_CONFIG_DEFAULT;
    ota_cfg.product          = SECURACV_OTA_PRODUCT;
    ota_cfg.current_version  = FIRMWARE_VERSION;
    ota_cfg.manifest_url     = SECURACV_OTA_MANIFEST_URL;
    ota_cfg.release_pubkey   = SECURACV_OTA_RELEASE_PUBKEY;
    ota_cfg.on_progress      = ota_on_progress;
    ota_cfg.can_install      = ota_can_install;
    ota_cfg.on_before_reboot = ota_before_reboot;
    if (securacv_ota_init(&ota_cfg) == ESP_OK) {
      Serial.printf("[OK] Pull-OTA engine ready (product=%s, version=%s)\n",
                    SECURACV_OTA_PRODUCT, FIRMWARE_VERSION);
    } else {
      Serial.println("[WARN] Pull-OTA engine init failed");
    }
  }
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

#if FEATURE_HA_MQTT
    /* Tamper must ALSO reach the host kernel's sealed log, not just the
     * device-side chain: queue a publish on the dedicated tamper topic
     * (drained by the main loop; this callback must stay non-blocking). */
    if (rt == RECORD_TAMPER_ALERT) {
      g_tamper_pending_kind = we->kind;
      g_tamper_pending_confidence = we->confidence;
      g_tamper_publish_pending = true;
    }
#endif

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

  // Sign a witness record if THIS boot followed a real power incident (a
  // restored outage or a brownout), now that the device is provisioned.
  canary_pe::witness_incident();

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

  // Passive thermal observer: lifetime die-temp history, advisories.
  // Never actuates — the camera state machine stays the sole actuator.
#if FEATURE_THERMAL_WATCHDOG
  thermal_wd_init();
  Serial.println("[OK] Thermal watchdog observing");
#endif

  // Confirm — or roll back — a freshly applied OTA image. Reaching this
  // line at all means provisioning, storage, and network bring-up survived
  // the new firmware; the registered probes assert the parts that matter
  // for the device's job. If a required probe fails, the engine marks the
  // image invalid and reboots into the previous firmware (does not return).
  //
  // Guard covers BOTH install channels: the engine owns rollback
  // confirmation (verifyRollbackLater), so any build that can install an
  // image — pull OTA or the dev push endpoint — must also confirm it here,
  // or the bootloader reverts it on the second boot.
#if FEATURE_OTA_PULL || FEATURE_OTA_UPDATE
  {
    static const securacv_selftest_t k_ota_selftests[] = {
      { "device identity", [](const char*) -> bool {
          return witness_get_device().device_id[0] != '\0';
        }, true },
#if FEATURE_DIAGNOSTICS
      { "diagnostics suite", [](const char*) -> bool {
          return diag_run_selftest() >= 50;
        }, true },
#endif
    };
    for (size_t i = 0; i < sizeof(k_ota_selftests) / sizeof(k_ota_selftests[0]); i++) {
      securacv_ota_register_selftest(&k_ota_selftests[i]);
    }
    securacv_ota_boot_self_test();

    // Witness the update outcome. The engine recorded the install target
    // the moment the boot partition flipped; running the old version again
    // means the rollback fired.
    char target[SECURACV_OTA_VERSION_MAX];
    if (securacv_ota_take_pending_version(target, sizeof(target))) {
      if (strcmp(target, FIRMWARE_VERSION) == 0) {
        ota_witness_event("fw_update_applied", FIRMWARE_VERSION);
        log_health(LOG_LEVEL_NOTICE, LOG_CAT_SYSTEM,
                   "Firmware update applied", FIRMWARE_VERSION);
      } else {
        ota_witness_event("fw_update_rolled_back", target);
        log_health(LOG_LEVEL_WARNING, LOG_CAT_SYSTEM,
                   "Firmware update rolled back", target);
      }
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

  // Enable WiFi modem sleep when running on battery to save ~20 mA.
  //
  // Only when NEITHER the power policy engine NOR CSI is compiled in:
  //   • With FEATURE_POWER_POLICY, the policy engine OWNS wifi power-save and
  //     is now CSI-aware (it forces PS off while CSI is capturing), so a manual
  //     boot-time enable here would just fight it.
  //   • With FEATURE_CSI (and no policy), CSI needs every RX frame, so modem
  //     sleep must stay off regardless of battery state.
  // This removes the old unconditional boot-time MIN_MODEM that silently
  // collapsed CSI capture on battery.
#if FEATURE_WIFI_AP && FEATURE_POWER_MONITOR && !FEATURE_POWER_POLICY && !FEATURE_CSI
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
#if FEATURE_CONSOLE_THEME
  // The warm hello: the canary greets whoever just plugged in and points them
  // to the site that explains everything. Boot already succeeded (FR-8) — this
  // only prints; ASCII-safe, no terminal probe at boot.
  print_boot_welcome();
#endif
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

#if FEATURE_USB_ONBOARD
  // Re-lock the HID keyboard if an arming window elapsed with no confirm.
  usb_onboard::poll();
#endif

  // Handle boot button (info print, factory reset)
  handle_boot_button();

#if FEATURE_SETUP_WIZARD
  // Pump the portal DNS whenever the responder is up — NOT only while setup
  // is active. Setup completes the moment credentials are saved, but the
  // phone's captive-portal sheet stays open on the wizard's success screen
  // and keeps re-resolving the hijacked hostname; going deaf here would cut
  // that session off mid-sentence. (setup_dns_process is a no-op once the
  // responder is stopped, so this costs nothing in normal operation.)
  setup_dns_process();
  if (setup_is_active()) {
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

  // No SNTP path exists in this project — GPS is the only wall-clock source
  // available. Seed/correct the system clock from it once RMC has a
  // validated date/time (cheap: bails out immediately once synced and not
  // due for a drift-correction check).
  syncClockFromGps();

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

  // Tamper narration (system.integrity): feed the watcher the boot's reset
  // classification once per loop. The module owns every transition rule and
  // emits through the chokepoint (tamper_events_module, reached via the
  // csi_modules_integration bridge — including it here directly would
  // collide the two csi_features_t typedefs); this call is a data feed,
  // never a policy site. Deliberately OUTSIDE the CSI power/degrade gates
  // above: integrity events are the one story that must survive battery
  // saver. On builds where the CSI pipeline never initializes, the
  // module's bounded retry gives up quietly.
  //
  // sd_state: this lane has no SD state machine — storage mounts once at
  // boot (storage_init) and never re-probes, unmounts, or errors out at
  // runtime — so we feed the module's pinned ABSENT (0) constant rather
  // than inventing a detector: the watcher adopts it on the first call and
  // never emits an SD kind. The sd_error/sd_remove stories stay exclusive
  // to hosts with a real hot-swap state machine (canary-wap).
  {
    static const esp_reset_reason_t s_boot_rst = esp_reset_reason();
    // Same crash set as canary-wap's hardware_state.h reset_is_crash():
    // panic, the three watchdogs, brownout. A user toggling power, pressing
    // reset, or our own ESP.restart() is a clean boot with nothing to
    // confess.
    const bool rst_watchdog = (s_boot_rst == ESP_RST_INT_WDT ||
                               s_boot_rst == ESP_RST_TASK_WDT ||
                               s_boot_rst == ESP_RST_WDT);
    const bool rst_brownout = (s_boot_rst == ESP_RST_BROWNOUT);
    const bool rst_crash = (s_boot_rst == ESP_RST_PANIC) ||
                           rst_watchdog || rst_brownout;
    securacv_csi_modules_tamper_watch(rst_crash ? 1 : 0,
                                      rst_watchdog ? 1 : 0,
                                      rst_brownout ? 1 : 0,
                                      /*sd_state: pinned ABSENT*/ 0u);
  }

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
  #if FEATURE_CAMERA_PEEK
  /* Camera streaming heats the die 10–20 °C — by temperature alone
   * that's the heat-gun tamper signature. Tell the drift detector to
   * fast-track its baseline (and hold detection) while peek is active,
   * plus its internal cooldown after it stops. */
  envsens_set_high_load(camera_get_instance().isPeekActive());
  #endif
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

  /* Persist the power-event liveness heartbeat (bounds the next outage's
   * lower-bound duration). Cheap: skips the write unless the wall clock is set,
   * so a clock-less board never writes it. */
  canary_pe::heartbeat(now);

#if FEATURE_THERMAL_WATCHDOG
  thermal_wd_process();  /* rate-limits internally (30 s sample, 10 min persist) */
#endif

#if FEATURE_POWER_POLICY
  policy_process();

#if FEATURE_DEEP_SLEEP
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
#if FEATURE_THERMAL_WATCHDOG
    thermal_wd_persist();
#endif
    lowpower_arm_wake_timer((uint64_t)sleep_sec * 1000000ULL);
    lowpower_arm_wake_touch();
    policy_ack_deep_sleep();
    lowpower_enter_deep_sleep();
  } else if (policy_should_deep_sleep()) {
    policy_ack_deep_sleep();
  }
#else
  /* FEATURE_DEEP_SLEEP=0 — "compiled in but never sleeps" (canary_config.h).
   * Honor the policy's sleep bookkeeping so it does not spin re-requesting a
   * sleep that never happens, but never actually enter deep sleep. This is the
   * documented contract of the flag; the previous code gated this block on
   * FEATURE_POWER_POLICY alone, so a default (FEATURE_DEEP_SLEEP=0) build would
   * still deep-sleep on a LOW_POWER duty cycle. Critical-battery deep-sleep
   * protection likewise requires FEATURE_DEEP_SLEEP=1 (securacv_power.cpp). */
  if (policy_should_deep_sleep()) {
    policy_ack_deep_sleep();
  }
#endif  /* FEATURE_DEEP_SLEEP */
#endif  /* FEATURE_POWER_POLICY */

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

#if FEATURE_OTA_PULL
  // Daily jittered update check (auto-installs only when the user opted in).
  ota_scheduler_process(now);

  // Witness the moment a download actually starts — the chain should show
  // every install attempt, not just the outcomes.
  {
    const securacv_ota_state_t ota_st = securacv_ota_get_state();
    if (ota_st == SECURACV_OTA_DOWNLOADING &&
        g_ota_last_seen_state != SECURACV_OTA_DOWNLOADING) {
      const securacv_ota_manifest_t* m = securacv_ota_get_manifest();
      ota_witness_event("fw_update_started", (m != NULL) ? m->version : "?");
    }
    g_ota_last_seen_state = ota_st;
  }
#endif

#if FEATURE_HA_MQTT
  // MQTT loop — handles reconnect and keepalive
  mqtt_loop();

  // Publish status periodically
  if (mqtt_connected() && now - g_last_mqtt_status_ms >= MQTT_STATUS_INTERVAL_MS) {
    g_last_mqtt_status_ms = now;
    mqtt_publish_status_update();
#if FEATURE_OTA_PULL
    mqtt_publish_ota_update_state();
#endif
  }

#if FEATURE_OTA_PULL
  // Push update-entity changes promptly (progress %, state transitions)
  // — the flag is set from the OTA task callback, published here.
  if (g_ota_publish_pending && mqtt_connected()) {
    g_ota_publish_pending = false;
    mqtt_publish_ota_update_state();
  }
#endif

#if FEATURE_HA_MQTT && FEATURE_SENSING_WITNESS
  // Drain a pending tamper alert onto securacv/{id}/tamper. Payload shape
  // matches the host mqtt_sensor adapter contract ({state, confidence,
  // kind}); the adapter routes it into the sealed log as TamperDetected.
  // Confidence is rescaled 0..100 -> 0..1 for the kernel's bounds check.
  if (g_tamper_publish_pending && mqtt_connected()) {
    g_tamper_publish_pending = false;
    /* Copy BOTH volatile fields back-to-back before formatting: a tamper
     * callback firing mid-publish may overwrite them, and a torn read
     * would pair one event's kind with another's confidence. */
    const uint8_t kind = g_tamper_pending_kind;
    const uint8_t confidence = g_tamper_pending_confidence;
    const char* kind_str =
        (kind == SENSING_WITNESS_TOUCH_TAMPER)  ? "enclosure_tamper" :
        (kind == SENSING_WITNESS_TEMP_DRIFT)    ? "temp_drift"
                                                : "camera_tamper";
    char payload[96];
    snprintf(payload, sizeof(payload),
             "{\"state\":\"on\",\"confidence\":%.2f,\"kind\":\"%s\"}",
             (double)confidence / 100.0, kind_str);
    if (!mqtt_publish_tamper(payload)) {
      // Re-arm so the alert survives a transient broker drop; the
      // device-side chain already holds the signed record either way.
      g_tamper_publish_pending = true;
    }
  }
#endif

  // Publish the boot power lineage once per boot: a restored outage, a
  // brownout, or a fault reset becomes the {"type":"power_loss"} /
  // {"type":"unexpected_reboot"} tamper payload the HA integration's
  // per-type sensors parse (and the adapter's tamper route can seal). A
  // benign boot builds no payload and stays silent. Same re-arm rule as
  // the sensing tamper drain above.
  {
    static bool s_pe_tamper_pending = true;
    if (s_pe_tamper_pending && mqtt_connected()) {
      s_pe_tamper_pending = false;
      char pe_payload[224];
      if (canary_pe::ha_tamper_payload(pe_payload, sizeof(pe_payload)) &&
          !mqtt_publish_tamper(pe_payload, /*retained=*/false)) {
        s_pe_tamper_pending = true;
      }
    }
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
#if FEATURE_USB_ONBOARD
    else {
      // Short press: the physical confirmation for USB onboarding. This is the
      // trust keystone — the ONLY thing that lets the HID keyboard type, and
      // only while it is ARMED (a no-op otherwise).
      usb_onboard::confirm();
    }
#endif
    // (Short press is otherwise reserved for future use / provisioning gate.)
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

#if FEATURE_OTA_PULL
/* State payload for the HA MQTT `update` entity. Keys follow the
 * documented schema (installed_version / latest_version / in_progress /
 * update_percentage / release_summary / release_url). The retained topic
 * plus the reconnect republish in the MQTT lib keep HA in sync across
 * broker restarts. Kept well under the 1 KB PubSubClient buffer. */
static void mqtt_publish_ota_update_state() {
  JsonDocument doc;
  doc["installed_version"] = FIRMWARE_VERSION;

  const securacv_ota_manifest_t* m = securacv_ota_get_manifest();
  if (m != NULL && securacv_ota_update_available()) {
    doc["latest_version"] = m->version;
    if (m->release_url[0] != '\0') {
      doc["release_url"] = m->release_url;
    }
    if (m->release_notes[0] != '\0') {
      // Truncate to keep the whole payload comfortably inside the buffer.
      char summary[201];
      strncpy(summary, m->release_notes, sizeof(summary) - 1);
      summary[sizeof(summary) - 1] = '\0';
      doc["release_summary"] = summary;
    }
  } else {
    doc["latest_version"] = FIRMWARE_VERSION;
  }

  const securacv_ota_state_t st = securacv_ota_get_state();
  const bool in_progress = (st == SECURACV_OTA_DOWNLOADING ||
                            st == SECURACV_OTA_VERIFYING ||
                            st == SECURACV_OTA_FLASHING ||
                            st == SECURACV_OTA_REBOOTING);
  doc["in_progress"] = in_progress;
  if (in_progress) {
    doc["update_percentage"] = securacv_ota_get_progress();
  } else {
    doc["update_percentage"] = nullptr;  // resets HA's progress bar
  }

  String payload;
  serializeJson(doc, payload);
  mqtt_publish_update_state(payload.c_str());
}
#endif // FEATURE_OTA_PULL

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

  /* Power lineage flags, held for kIncidentHoldMs after boot: the tamper
   * topic's one-shot message is non-retained, so a hub that reboots slower
   * than the Canary (every whole-house outage) learns about the incident
   * from here instead. HA's health parse clears the sensors once the hold
   * lapses and the flags go false. */
  doc["power_loss_detected"] = canary_pe::health_power_flag(millis());
  doc["unexpected_reboot"] = canary_pe::health_fault_flag(millis());

  /* SD endurance metrics: lifetime write counters (NVS-persisted), wear
   * estimate against the configured TBW rating, and the replacement
   * recommendation latch. HA's SD Wear / SD Replacement sensors read
   * this object. */
#if FEATURE_DIAGNOSTICS
  {
    diag_sd_t sd;
    if (diag_get_sd(&sd)) {
      JsonObject sdo = doc["sd"].to<JsonObject>();
      sdo["mounted"] = sd.mounted;
      sdo["usage_pct"] = sd.usage_pct;
      sdo["writes"] = sd.total_writes;
      sdo["errors"] = sd.write_errors;
      sdo["lifetime_kb"] = (uint64_t)(sd.lifetime_bytes / 1024);
      sdo["wear_pct"] = sd.wear_pct_x10 / 10.0;
      sdo["replace_recommended"] = sd.replace_recommended;
    }
  }
#endif
  /* Die temperature via the shared provider (heat accelerates flash wear;
   * HA surfaces it alongside the SD metrics). */
  {
    float temp_c = 0.0f;
    if (thermal_read_die_c(&temp_c)) {
      doc["temp_c"] = temp_c;
    }
  }
#if FEATURE_POWER_MONITOR
  doc["battery_mv"] = health.battery_mv;
  doc["battery_soc"] = health.battery_soc;
  doc["battery_trend"] = health.battery_trend;
  doc["charge_cycles"] = 0;
  /* battery_present gates HA's battery-threshold derivation: without it
   * a USB-only device's battery_soc=0 would read as a critical battery.
   * The HA integration only trusts battery_soc when presence is
   * explicitly true. */
  doc["battery_present"] = false;
  {
    power_state_t pwr;
    if (power_get_state(&pwr)) {
      doc["charge_cycles"] = pwr.charge_cycles;
      doc["battery_present"] = pwr.battery_present;
      if (pwr.battery_present) {
        doc["charge_state"] = power_charge_state_name(pwr.charge_state);
        doc["battery_health_pct"] = power_health_pct();
      }
    }
  }
#endif

#if FEATURE_THERMAL_WATCHDOG
  /* Whole degrees only — same display rounding the dashboard uses. */
  {
    thermal_wd_state_t tw;
    thermal_wd_history_t th;
    if (thermal_wd_get_state(&tw) && thermal_wd_get_history(&th) &&
        tw.last_sample_ms != 0) {
      static const char* tnames[] = {"normal", "throttled", "paused"};
      doc["die_temp_c"]    = (int)lroundf(tw.die_temp_c);
      doc["thermal_state"] = tnames[tw.shadow_state <= 2 ? tw.shadow_state : 0];
      doc["thermal_sensor_ok"] = tw.sensor_ok;
      doc["thermal_advisory"]  = tw.advisories != 0;
      doc["thermal_throttled_min"] = th.throttled_min;
      doc["thermal_pause_events"]  = th.pause_events;
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

// The diagnostic-console catalog, declared as data so the security TIER of every
// command we expose is auditable by testcon::table_is_safe() (host-tested in CI)
// rather than implicit in the switch below. Everything here is Tier::Diag —
// read-only, mutates nothing, leaks no secret — so it is safe even on a
// production image. Mutating/demo commands live behind FEATURE_TEST_CONSOLE and
// a physical confirm (docs/design/test_console.md); legacy operational keys
// (reboot, onboarding launch) are not part of this diagnostic contract.
static const testcon::Command kConsoleCommands[] = {
  { 'i', "identity",     testcon::Tier::Diag, false, false, false },
  { 'j', "manifest",     testcon::Tier::Diag, false, false, false },
  { 's', "status",       testcon::Tier::Diag, false, false, false },
  { 'g', "gps",          testcon::Tier::Diag, false, false, false },
  { 'b', "battery",      testcon::Tier::Diag, false, false, false },
  { 'd', "diagnostics",  testcon::Tier::Diag, false, false, false },
  { 'm', "mqtt",         testcon::Tier::Diag, false, false, false },
  { 't', "run-tests",    testcon::Tier::Diag, false, false, false },
  { 'c', "attest",       testcon::Tier::Diag, false, false, false },
  { 'f', "fingerprint",  testcon::Tier::Diag, false, false, false },
  { 'e', "explain-boot", testcon::Tier::Diag, false, false, false },
  { 'w', "tamper-log",   testcon::Tier::Diag, false, false, false },
#if FEATURE_CONSOLE_THEME
  { 'l', "identity-banner", testcon::Tier::Diag, false, false, false },
#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
  { 'n', "nearby",          testcon::Tier::Diag, false, false, false },
#endif
#if FEATURE_DIAGNOSTICS
  { 'a', "wake-selftest",   testcon::Tier::Diag, false, false, false },
#endif
#endif
};
static const size_t kConsoleCommandCount =
    sizeof(kConsoleCommands) / sizeof(kConsoleCommands[0]);

// Map the ESP reset cause onto the host-testable testcon::ResetReason so the
// labeling logic lives in the pure header (and is proven in CI).
static testcon::ResetReason map_reset_reason(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return testcon::ResetReason::PowerOn;
    case ESP_RST_SW:        return testcon::ResetReason::Software;
    case ESP_RST_PANIC:     return testcon::ResetReason::Panic;
    case ESP_RST_INT_WDT:   return testcon::ResetReason::IntWdt;
    case ESP_RST_TASK_WDT:  return testcon::ResetReason::TaskWdt;
    case ESP_RST_WDT:       return testcon::ResetReason::Watchdog;
    case ESP_RST_BROWNOUT:  return testcon::ResetReason::BrownOut;
    case ESP_RST_DEEPSLEEP: return testcon::ResetReason::DeepSleep;
    case ESP_RST_SDIO:      return testcon::ResetReason::Sdio;
    case ESP_RST_UNKNOWN:   return testcon::ResetReason::Unknown;
    default:                return testcon::ResetReason::Other;
  }
}

static void print_hex(const uint8_t* b, size_t n) {
  for (size_t i = 0; i < n; ++i) Serial.printf("%02x", b[i]);
}

// Read the rest of the current serial line (after the command char) as an
// optional argument — used for the challenger nonce of the 'c' command. Trims
// surrounding whitespace/CR/LF. Returns the length copied into buf (NUL-term).
static size_t read_line_arg(char* buf, size_t cap) {
  size_t n = 0;
  uint32_t deadline = millis() + 60;   // brief settle for the line to arrive
  while (millis() < deadline && n + 1 < cap) {
    while (Serial.available() && n + 1 < cap) {
      char c = (char)Serial.read();
      if (c == '\n' || c == '\r') { deadline = 0; break; }   // end of line
      buf[n++] = c;
      deadline = millis() + 20;
    }
    if (deadline == 0) break;
  }
  // trim leading/trailing spaces
  size_t start = 0; while (start < n && buf[start] == ' ') start++;
  size_t end = n;   while (end > start && buf[end - 1] == ' ') end--;
  size_t len = end - start;
  if (start > 0) memmove(buf, buf + start, len);
  buf[len] = '\0';
  return len;
}

#if FEATURE_CONSOLE_THEME
// The scene engine composes text and pushes it here one chunk at a time.
static void theme_emit(void*, const char* s) { Serial.print(s); }

// The boot hello — the canary greeting whoever just plugged in, and the one URL
// that explains everything. ASCII-safe (no probe at boot; the terminal may not
// be attached yet). Forward-declared up top so setup() can call it.
static void print_boot_welcome() {
  scene::Renderer r{theme_emit, nullptr, scene::caps_ascii()};
  Serial.print("\r\n");
  scene::welcome_card(r, witness_get_device().device_id, SECURACV_HELP_URL_BASE);
}

// Probe the terminal ONCE before drawing: emit a cursor-position report request
// (ESC[6n) and see if it answers with ESC[row;colR. An answer ⇒ the terminal
// speaks ANSI ⇒ we light up color + Unicode. Silence (a dumb monitor — or our
// own flasher's garbage-averse parser) ⇒ we stay on the 7-bit ASCII floor, so
// the banner never turns into escape-code garbage. Bounded to ~200 ms.
static scene::Caps console_probe() {
  while (Serial.available()) Serial.read();          // clear pending input
  Serial.print("\x1b[6n");
  uint32_t deadline = millis() + 200;
  bool ansi = false;
  while (millis() < deadline) {
    while (Serial.available()) {
      if ((char)Serial.read() == 'R') { ansi = true; deadline = 0; break; }
    }
    if (deadline == 0) break;
  }
  return ansi ? scene::caps_full(80, 24) : scene::caps_ascii();
}

// 'l' — the identity banner: the device's verifiable trust card, with its
// public-key fingerprint drawn as drunken-bishop randomart. Read-only.
// Build the trust card from live device state and render it via `r` (whose caps
// were already chosen — by a fresh probe, or reused from the wake sequence).
static void render_trust_card(const scene::Renderer& r) {
  DeviceIdentity& dev = witness_get_device();
  char fp[17];
  for (int i = 0; i < 8; ++i) snprintf(fp + i * 2, 3, "%02x", dev.pubkey_fp[i]);
  char ch[9];
  for (int i = 0; i < 4; ++i) snprintf(ch + i * 2, 3, "%02x", dev.chain_head[i]);
  int health = -1;
#if FEATURE_DIAGNOSTICS
  selftest_report_t st;
  if (diag_get_selftest(&st)) health = (int)st.health_score;
#endif
  scene::TrustInfo t{};
  t.device_id = dev.device_id;
  t.firmware = FIRMWARE_VERSION;
  t.git = FIRMWARE_GIT_HASH;
  t.built = __DATE__;
  t.chain_head_hex = ch;
  t.key_fp_hex = fp;
  t.seq = dev.seq;
  t.boots = dev.boot_count;
  t.health = health;
  t.tamper = dev.tamper_active;
  t.key_bytes = dev.pubkey;
  t.key_len = 32;

  Serial.print("\r\n");
  scene::trust_card(r, t);
}

static void show_identity_banner() {
  scene::Renderer r{theme_emit, nullptr, console_probe()};
  render_trust_card(r);
}

#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
// 'n' — nearby: the fleet roster (which OTHER Canaries this one has heard over
// BLE), rendered as the width-aligned fleet card. Read-only. Presence is
// UNSIGNED — liveness, never a verified trust claim (the card says so). Snapshot
// the shared roster, project each entry to a view peer at `now`, then render.
static void show_fleet_nearby() {
  DeviceIdentity& dev = witness_get_device();
  char self_fp4[5];
  snprintf(self_fp4, sizeof self_fp4, "%02x%02x", dev.pubkey_fp[6], dev.pubkey_fp[7]);

  static FleetRosterEntry entries[FLEET_ROSTER_MAX];
  scene::FleetPeer peers[FLEET_ROSTER_MAX];
  const int nn = fleet_roster_feed::snapshot(entries, FLEET_ROSTER_MAX);
  const uint32_t now = millis();
  for (int i = 0; i < nn; ++i) peers[i] = scene::fleet_peer_from_entry(entries[i], now);

  scene::FleetView v{};
  v.self_id  = dev.device_id;
  v.self_fp4 = self_fp4;
  v.peers    = peers;
  v.count    = (size_t)(nn < 0 ? 0 : nn);
  v.capacity = FLEET_ROSTER_MAX;

  scene::Renderer r{theme_emit, nullptr, console_probe()};
  Serial.print("\r\n");
  scene::fleet_card(r, v);
}
#endif

#if FEATURE_DIAGNOSTICS
// 'a' — the animated wake: the 10 self-test probes light up [..] -> [OK]/[!!]
// as they report (real per-probe results), then it settles into the identity
// card. On a confirmed ANSI terminal the fixed-height block repaints in place;
// on the plain ASCII floor there's no cursor control, so it prints the finished
// checklist once. Press any key to skip the reveal. Read-only (Tier::Diag).
static void run_wake() {
  scene::Caps caps = console_probe();
  scene::Renderer r{theme_emit, nullptr, caps};
  Serial.print("\r\nWaking - running self-test...\r\n");
  diag_run_selftest();                 // the real run (~2-5s), fills the report
  selftest_report_t st;
  if (!diag_get_selftest(&st)) { render_trust_card(r); return; }

  int n = st.total_count;
  if (n > SELFTEST_COUNT) n = SELFTEST_COUNT;
  scene::WakeProbe probes[SELFTEST_COUNT];
  for (int i = 0; i < n; ++i) {
    probes[i].label = st.tests[i].name;
    probes[i].state = scene::ProbeState::Pending;
    probes[i].ms = st.tests[i].duration_ms;
  }

  if (!caps.ansi) {
    // ASCII floor: no cursor control — reveal all and print the block once.
    for (int i = 0; i < n; ++i)
      probes[i].state = st.tests[i].passed ? scene::ProbeState::Pass
                                           : scene::ProbeState::Fail;
    scene::wake_frame(r, scene::TRUST_INNER, probes, n, st.health_score, true);
  } else {
    scene::hide_cursor(r);
    scene::wake_frame(r, scene::TRUST_INNER, probes, n, st.health_score, false);
    bool skipped = false;
    for (int i = 0; i < n && !skipped; ++i) {
      probes[i].state = st.tests[i].passed ? scene::ProbeState::Pass
                                           : scene::ProbeState::Fail;
      scene::cursor_up(r, scene::wake_height(n));
      scene::wake_frame(r, scene::TRUST_INNER, probes, n, st.health_score, i == n - 1);
      uint32_t until = millis() + 180;
      while (millis() < until) {
        if (Serial.available()) { Serial.read(); skipped = true; break; }
      }
    }
    if (skipped) {
      for (int j = 0; j < n; ++j)
        probes[j].state = st.tests[j].passed ? scene::ProbeState::Pass
                                             : scene::ProbeState::Fail;
      scene::cursor_up(r, scene::wake_height(n));
      scene::wake_frame(r, scene::TRUST_INNER, probes, n, st.health_score, true);
    }
    scene::show_cursor(r);
  }
  render_trust_card(r);
}
#endif  // FEATURE_DIAGNOSTICS
#endif  // FEATURE_CONSOLE_THEME

// 'j' — the machine-readable self-manifest: a compact JSON line describing this
// unit to a *program* (public info only — the same facts as the trust card,
// plus the enabled feature set). A browser reads it over WebSerial to draw the
// matching randomart from the pubkey and show exactly the tools this device
// has. Read-only, leaks no secret (Tier::Diag). Always available.
static void emit_self_manifest() {
  DeviceIdentity& dev = witness_get_device();

  char pk[65], fp[17], ch[65];
  for (int i = 0; i < 32; ++i) snprintf(pk + i * 2, 3, "%02x", dev.pubkey[i]);
  for (int i = 0; i < 8;  ++i) snprintf(fp + i * 2, 3, "%02x", dev.pubkey_fp[i]);
  for (int i = 0; i < 32; ++i) snprintf(ch + i * 2, 3, "%02x", dev.chain_head[i]);

  // Enabled capabilities — the compile-time FEATURE_* flags ARE the source of
  // truth, so the manifest can never claim a feature the image doesn't carry.
  const char* feats[40];
  size_t nf = 0;
  #define CV_ADD_FEAT(name) do { \
      if (nf < sizeof(feats) / sizeof(feats[0])) feats[nf++] = (name); } while (0)
#if FEATURE_SD_STORAGE
  CV_ADD_FEAT("sd_storage");
#endif
#if FEATURE_WIFI_AP
  CV_ADD_FEAT("wifi_ap");
#endif
#if FEATURE_HTTP_SERVER
  CV_ADD_FEAT("http_server");
#endif
#if FEATURE_CAMERA_PEEK
  CV_ADD_FEAT("camera_peek");
#endif
#if FEATURE_GNSS
  CV_ADD_FEAT("gnss");
#endif
#if FEATURE_WATCHDOG
  CV_ADD_FEAT("watchdog");
#endif
#if FEATURE_OTA_UPDATE
  CV_ADD_FEAT("ota");
#endif
#if FEATURE_OTA_PULL
  CV_ADD_FEAT("ota_pull");
#endif
#if FEATURE_HA_MQTT
  CV_ADD_FEAT("mqtt");
#endif
#if FEATURE_MESH_NETWORK
  CV_ADD_FEAT("mesh");
#endif
#if FEATURE_BLE
  CV_ADD_FEAT("ble");
#endif
#if FEATURE_BLE_STATUS
  CV_ADD_FEAT("ble_status");
#endif
#if FEATURE_CSI
  CV_ADD_FEAT("csi");
#endif
#if FEATURE_VISION_DETECT
  CV_ADD_FEAT("vision");
#endif
#if FEATURE_POWER_MONITOR
  CV_ADD_FEAT("power_monitor");
#endif
#if FEATURE_POWER_POLICY
  CV_ADD_FEAT("power_policy");
#endif
#if FEATURE_THERMAL_WATCHDOG
  CV_ADD_FEAT("thermal_watchdog");
#endif
#if FEATURE_DIAGNOSTICS
  CV_ADD_FEAT("diagnostics");
#endif
#if FEATURE_DATA_MGMT
  CV_ADD_FEAT("data_mgmt");
#endif
#if FEATURE_SETUP_WIZARD
  CV_ADD_FEAT("setup_wizard");
#endif
#if FEATURE_USB_ONBOARD
  CV_ADD_FEAT("usb_onboard");
#endif
#if FEATURE_CONSOLE_THEME
  CV_ADD_FEAT("console_theme");
#endif
  #undef CV_ADD_FEAT

  int health = -1;
#if FEATURE_DIAGNOSTICS
  selftest_report_t st;
  if (diag_get_selftest(&st)) health = (int)st.health_score;   // last score; no fresh run
#endif

  // The interactive keys THIS image answers — straight from the one command
  // registry (kConsoleCommands), so the manifest can't list a key the device
  // doesn't actually handle.
  manifest::Cmd cmds[kConsoleCommandCount + 1];
  size_t nc = 0;
  for (size_t i = 0; i < kConsoleCommandCount; ++i) {
    cmds[nc].key = kConsoleCommands[i].key;
    cmds[nc].name = kConsoleCommands[i].name;
    nc++;
  }

  // The fleet roster THIS unit has heard over the air — the machine-readable
  // twin of the 'n' card, for the browser /fleet page. Public-only and UNSIGNED.
  // Empty unless FEATURE_BLE_SCAN feeds the roster; single-sourced from the same
  // snapshot + host-tested projection (fleet_peer_from_entry) the card uses, so
  // the page shows the live truth. (null + 0 → "fleet":[] on non-scan builds.)
  const manifest::Peer* fleet_ptr = nullptr;
  size_t nfleet = 0;
#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
  static manifest::Peer   fleetbuf[FLEET_ROSTER_MAX];
  static FleetRosterEntry rent[FLEET_ROSTER_MAX];
  static char             flagbufs[FLEET_ROSTER_MAX][40];
  const int rn = fleet_roster_feed::snapshot(rent, FLEET_ROSTER_MAX);
  const uint32_t rnow = millis();
  for (int i = 0; i < rn && nfleet < FLEET_ROSTER_MAX; ++i) {
    const scene::FleetPeer vp = scene::fleet_peer_from_entry(rent[i], rnow);
    scene::fleet_fmt_flags(vp.flags, flagbufs[nfleet], sizeof flagbufs[nfleet]);
    fleetbuf[nfleet].fp      = vp.fp4;
    fleetbuf[nfleet].age_s   = vp.age_s;
    fleetbuf[nfleet].health  = vp.health_pct;
    fleetbuf[nfleet].battery = vp.battery_pct;
    fleetbuf[nfleet].chain   = vp.chain_lo;
    // "" for a clean peer (fleet_fmt_flags renders that as "ok" for the console).
    fleetbuf[nfleet].flags   = (vp.flags == 0) ? "" : flagbufs[nfleet];
    nfleet++;
  }
  fleet_ptr = fleetbuf;
#endif

  manifest::Facts f{};
  f.board          = DEVICE_TYPE;
  f.firmware       = FIRMWARE_VERSION;
  f.git            = FIRMWARE_GIT_HASH;
  f.protocol       = PROTOCOL_VERSION;
  f.device_id      = dev.device_id;
  f.pubkey_hex     = pk;
  f.pubkey_fp_hex  = fp;
  f.chain_head_hex = ch;
  f.seq            = dev.seq;
  f.boots          = dev.boot_count;
  f.born_day       = dev.born_day;
  f.born_exact     = dev.born_exact;
  f.health         = health;
  {
    // Heat, from the shared thermal provider (never Arduino's temperatureRead()
    // — the envsens tamper detector owns the sensor; see securacv_thermal.h).
    float die_c = 0.0f;
    if (thermal_read_die_c(&die_c)) {
      f.temp_c = (int)(die_c + (die_c >= 0.0f ? 0.5f : -0.5f));
    }
  }
  f.tamper         = dev.tamper_active;
  f.features       = feats;
  f.feature_count  = nf;
  f.commands       = cmds;
  f.command_count  = nc;
  f.fleet          = fleet_ptr;
  f.fleet_count    = nfleet;
  f.help_url       = SECURACV_HELP_URL_BASE;

  // Only the scan build carries a fleet[]; size it for the WORST case so a full,
  // maximally-flagged roster never overflows to {"error":...} exactly when it's
  // most interesting. Worst peer — max age (uint32) + every status word —
  // serialises to ~119B; FLEET_ROSTER_MAX (16) of them ≈ 1.9KB, plus the base
  // manifest (full command set + features + 64-hex keys) ≈ 1.4KB → ~3.3KB. 4KB
  // clears that with headroom. Non-scan builds keep the small buffer (their
  // fleet[] is always empty), so RAM doesn't regress there.
#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
  static char buf[4096];
#else
  static char buf[1600];
#endif
  size_t n = manifest::build(f, buf, sizeof buf);
  Serial.println();
  if (n) Serial.println(buf);
  else   Serial.println("{\"error\":\"manifest overflow\"}");
}

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
      Serial.println("  j - Self-manifest (machine-readable JSON for the app)");
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
#if FEATURE_USB_ONBOARD
      Serial.println("  u - Open help page (announced preview; press BOOT to confirm)");
      Serial.println("      (or just tap BOOT any time, or open START-HERE on the drive)");
      Serial.println("  o - USB onboarding status / launch method");
      Serial.println("  v - Recovery guide");
      Serial.println("  k - Unseal guide");
#endif
      Serial.println("  t - Run all tests (self-test + feature health + Bluetooth)");
#if FEATURE_CONSOLE_THEME
      Serial.println("  l - Identity banner (key fingerprint as randomart)");
#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
      Serial.println("  n - Nearby Canaries (the fleet this one hears)");
#endif
#if FEATURE_DIAGNOSTICS
      Serial.println("  a - Animated wake (watch the self-test run live)");
#endif
#endif
      Serial.println("  c - Attest chain (sign a nonce + chain head: c <nonce>)");
      Serial.println("  f - Fingerprint / provenance (version, secure-boot, keys)");
      Serial.println("  e - Explain last boot (reset reason + recent faults)");
      Serial.println("  w - Tamper log (passive detections, read-only)");
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

    case 'j':
    case 'J':
      emit_self_manifest();
      break;

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
        Serial.printf("  Witness files: %u (append-only, never rotated)\n",
                      (unsigned)dm.witness_files);
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

#if FEATURE_USB_ONBOARD
    case 'u':
    case 'U':
      // Ask to open the help page. Announces the exact URL and arms the
      // window; the keyboard types nothing until a physical BOOT press.
      usb_onboard::request_launch();
      break;

    case 'o':
    case 'O': {
      // Cycle the launch method (MANUAL → macOS → Windows → Linux) and show
      // status. Cancels any in-flight arming first.
      usb_onboard::cancel();
      usb_onboard::LaunchMethod next;
      switch (usb_onboard::method()) {
        case usb_onboard::LaunchMethod::MANUAL:
          next = usb_onboard::LaunchMethod::MAC_SPOTLIGHT; break;
        case usb_onboard::LaunchMethod::MAC_SPOTLIGHT:
          next = usb_onboard::LaunchMethod::WIN_RUN; break;
        case usb_onboard::LaunchMethod::WIN_RUN:
          next = usb_onboard::LaunchMethod::GNOME_TERMINAL; break;
        default:
          next = usb_onboard::LaunchMethod::MANUAL; break;
      }
      usb_onboard::set_method(next);
      usb_onboard::print_status();
      break;
    }

    case 'v':
    case 'V':
      usb_onboard::print_recovery_guide();
      break;

    case 'k':
    case 'K':
      usb_onboard::print_unseal_guide();
      break;
#endif

    // 't' — "run all tests": read-only diagnostics only (testcon::Tier::Diag),
    // so it is safe even on a production image. It prints public status, never
    // secret material, and mutates nothing. The demo/mutating tiers live behind
    // FEATURE_TEST_CONSOLE + a physical confirm (see docs/design/test_console.md).
    case 't':
    case 'T': {
      Serial.println("\n=== Run all tests (read-only) ===");
#if FEATURE_DIAGNOSTICS
      diag_run_selftest();
      selftest_report_t st;
      if (diag_get_selftest(&st)) {
        Serial.printf("  Self-test : %u/%u probes · %u%% health\n",
                      st.passed_count, st.total_count, st.health_score);
      }
#else
      Serial.println("  Self-test : diagnostics not compiled in");
#endif
#if FEATURE_SD_STORAGE
      Serial.printf("  SD card   : %s\n", storage_is_mounted() ? "mounted" : "absent");
#else
      Serial.println("  SD card   : storage not compiled in");
#endif
#if FEATURE_GNSS
      Serial.printf("  GPS       : %s\n", s_gps.getFix().valid ? "fix" : "no fix");
#endif
#if FEATURE_POWER_MONITOR
      { power_state_t pw; Serial.printf("  Battery   : %s\n",
          power_get_state(&pw) ? "monitored" : "n/a"); }
#endif
#if FEATURE_HA_MQTT
      Serial.printf("  MQTT      : %s\n", mqtt_connected() ? "connected" : "offline");
#endif
      // Bluetooth ladder — say exactly which rung it reached (and why, if down).
      {
        testcon::BleObs o{};
#if FEATURE_BLE_STATUS
        o.compiled_in = true; o.stack_up = true; o.service_up = true;
        o.advertising = true; o.connected = ble_status_is_connected();
        o.exchanged = false;
#else
        o.compiled_in = false;   // dev/release images ship BLE off ([env:full] only)
#endif
        testcon::BleStage bs = testcon::ble_stage(o);
        Serial.printf("  Bluetooth : %s\n", testcon::ble_stage_label(bs));
        Serial.printf("              %s\n", testcon::ble_hint(bs));
      }
      Serial.println();
      break;
    }

    // 'c' — chain attestation. Signs "SECURACV-ATTEST-v1 || nonce || chain_head
    // || seq || boot_count" with the DEVICE key so anyone can verify offline
    // against the pinned public key that the witness log hasn't been rewritten.
    // It is Tier::Diag: read-only, mutates nothing, and — crucially — the fixed
    // domain prefix (testcon::attest_build_message) means the signature can
    // never be replayed as a chain entry or an OTA approval, so this is an
    // attestation channel, not a signing oracle. Never prints the private key.
    case 'c':
    case 'C': {
      DeviceIdentity& dev = witness_get_device();
      char arg[80];
      size_t got = read_line_arg(arg, sizeof(arg));
      uint8_t nonce[32];
      size_t nlen = 0;
      bool device_nonce = false;
      if (got > 0) {
        nlen = got > sizeof(nonce) ? sizeof(nonce) : got;
        memcpy(nonce, arg, nlen);
      } else {
        // No challenger nonce supplied. Generate one so the signature is still
        // fresh, but say so — a real remote challenge should pass its own.
#if HAVE_ESP_RANDOM
        for (size_t i = 0; i < 16; ++i) nonce[i] = (uint8_t)esp_random();
        nlen = 16;
        device_nonce = true;
#endif
      }
      uint8_t msg[128];
      size_t mlen = testcon::attest_build_message(msg, sizeof(msg), nonce, nlen,
                                                  dev.chain_head, dev.seq,
                                                  dev.boot_count);
      if (mlen == 0) { Serial.println("\nattest: message buffer too small"); break; }
      uint8_t sig[64];
      crypto_sign(dev.privkey, dev.pubkey, msg, mlen, sig);

      Serial.println("\n=== Chain attestation ===");
      Serial.printf("  Device    : %s\n", dev.device_id);
      Serial.print("  Pubkey    : ");   print_hex(dev.pubkey, 32);     Serial.println();
      Serial.print("  ChainHead : ");   print_hex(dev.chain_head, 32); Serial.println();
      Serial.printf("  Seq/Boot  : %lu / %lu\n",
                    (unsigned long)dev.seq, (unsigned long)dev.boot_count);
      Serial.printf("  Domain    : %s\n", testcon::ATTEST_DOMAIN);
      Serial.print("  Nonce     : ");    print_hex(nonce, nlen);
      Serial.printf("%s\n", device_nonce ? "  (device-generated — pass your own: c <nonce>)" : "");
      Serial.print("  Signature : ");    print_hex(sig, 64);           Serial.println();
      Serial.println("  Verify offline: ed25519(pubkey, DOMAIN||nonce||head||"
                     "seq_le32||boot_le32) — matches only this device + head.");
      Serial.println();
      break;
    }

    // 'f' — provenance / fingerprint card: "is this the device + build it claims?"
    case 'f':
    case 'F': {
      DeviceIdentity& dev = witness_get_device();
      Serial.println("\n=== Provenance / fingerprint ===");
      Serial.printf("  Firmware  : %s\n", FIRMWARE_VERSION);
      Serial.printf("  Git       : %s\n", FIRMWARE_GIT_HASH);
      Serial.printf("  Built     : %s %s\n", __DATE__, __TIME__);
      Serial.print("  Key FP    : ");   print_hex(dev.pubkey_fp, 8);   Serial.println();
      Serial.printf("  Boot/Seq  : %lu / %lu\n",
                    (unsigned long)dev.boot_count, (unsigned long)dev.seq);
#if HAVE_OTA_PARTITION
      { const esp_partition_t* run = esp_ota_get_running_partition();
        Serial.printf("  Partition : %s\n", run ? run->label : "unknown"); }
#endif
#if HAVE_SECURE_BOOT
      Serial.printf("  SecureBoot: %s\n", esp_secure_boot_enabled() ? "ENABLED" : "off");
#endif
#if HAVE_FLASH_ENCRYPT
      Serial.printf("  FlashEnc  : %s\n", esp_flash_encryption_enabled() ? "ENABLED" : "off");
#endif
      Serial.printf("  Console   : %u diag cmds · policy %s\n",
                    (unsigned)kConsoleCommandCount,
                    testcon::table_is_safe(kConsoleCommands, kConsoleCommandCount)
                        ? "SAFE" : "UNSAFE");
      Serial.println();
      break;
    }

    // 'e' — explain the last boot: why did it reset, and what has gone wrong
    // recently. Read-only introspection of the reset cause + health ring.
    case 'e':
    case 'E': {
      Serial.println("\n=== Last boot / recent faults ===");
      testcon::ResetReason rr = map_reset_reason(esp_reset_reason());
      Serial.printf("  Reset     : %s%s\n", testcon::reset_reason_label(rr),
                    testcon::reset_reason_is_fault(rr) ? "  ⚠" : "");
      Serial.printf("  Boot #    : %lu\n", (unsigned long)witness_get_device().boot_count);
#if FEATURE_DIAGNOSTICS
      Serial.printf("  Degrade   : %s\n",
                    testcon::degrade_level_label((uint8_t)diag_get_degrade_level()));
#endif
      // Most-recent-first walk of the health ring (mirrors the /api/logs order).
      const size_t RING = 100;   // HEALTH_LOG_RING_SIZE (securacv_witness.cpp)
      HealthLogRingEntry* ring = witness_get_health_log_ring();
      size_t count = witness_get_health_log_count();
      size_t head = witness_get_health_log_head();
      Serial.println("  Recent events:");
      size_t shown = 0;
      for (size_t i = 0; i < count && shown < 6; ++i) {
        size_t idx = (head + RING - 1 - i) % RING;
        HealthLogRingEntry& ev = ring[idx];
        Serial.printf("    [%s] %s%s%s\n", log_level_name(ev.level), ev.message,
                      ev.detail[0] ? " — " : "", ev.detail[0] ? ev.detail : "");
        shown++;
      }
      if (shown == 0) Serial.println("    (none logged this session)");
      Serial.println();
      break;
    }

    // 'w' — passive tamper log. Read-only: shows the device-level tamper counter
    // and the last-seen time of each passive detector. Never actuates anything.
    case 'w':
    case 'W': {
      DeviceIdentity& dev = witness_get_device();
      Serial.println("\n=== Tamper log (passive, read-only) ===");
      Serial.printf("  State     : %s\n", dev.tamper_active ? "ACTIVE ⚠" : "clear");
      Serial.printf("  Count     : %lu total\n", (unsigned long)dev.tamper_count);
#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
      { sensing_state_t s;
        sensing_snapshot(&s);
        uint32_t now = millis();
        auto ago = [now](uint32_t ms) { return ms ? (unsigned long)((now - ms) / 1000) : 0UL; };
        if (s.last_touch_event_ms)
          Serial.printf("  Enclosure : touch/tamper %lus ago (conf %u)\n",
                        ago(s.last_touch_event_ms), s.last_touch_event_conf);
        if (s.last_temp_drift_ms)
          Serial.printf("  Temp drift: %lus ago (conf %u)\n",
                        ago(s.last_temp_drift_ms), s.last_temp_drift_conf);
        if (s.last_vision_event_ms)
          Serial.printf("  Vision    : event %lus ago (conf %u)\n",
                        ago(s.last_vision_event_ms), s.last_vision_confidence);
        if (!s.last_touch_event_ms && !s.last_temp_drift_ms && !s.last_vision_event_ms)
          Serial.println("  Detectors : no passive events this session");
      }
#else
      Serial.println("  Detectors : none compiled in this build");
#endif
      Serial.println();
      break;
    }

#if FEATURE_CONSOLE_THEME
    // 'l' — the identity banner: verifiable trust card + key-fingerprint
    // randomart. Read-only; ASCII-safe unless the terminal answers the probe.
    case 'l':
    case 'L':
      show_identity_banner();
      break;
#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
    case 'n':
    case 'N':
      show_fleet_nearby();
      break;
#endif
#if FEATURE_DIAGNOSTICS
    case 'a':
    case 'A':
      run_wake();
      break;
#endif
#endif

    case 'x':
    case 'X':
      Serial.println("\nRebooting...");
      witness_persist_chain_state();
#if FEATURE_THERMAL_WATCHDOG
      thermal_wd_persist();
#endif
#if FEATURE_HA_MQTT
      mqtt_disconnect();
#endif
      delay(500);
      ESP.restart();
      break;

    default:
#if FEATURE_USB_ONBOARD
      // Any unrecognized key while the help launcher is armed cancels it, so
      // the arming window only ever resolves via a deliberate BOOT press.
      usb_onboard::cancel();
#endif
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
