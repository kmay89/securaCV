/*
  SecuraCV Canary Vision — Optical Witness Sensor Firmware
  --------------------------------------------------------
  (c) 2026 Errer Labs / SecuraCV
  errerlabs.com | securacv.com
  GitHub: https://github.com/kmay89/securaCV

  License: Apache-2.0 (use repository license unless otherwise specified).
*/

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <esp_random.h>  // esp_random() for reconnect jitter

#include "canary/config.h"
#include "canary/version.h"
#include "canary/log.h"
#include "canary/topics.h"
#include "canary/types.h"
#include "boot/boot_banner.h"
#include "attest/self_manifest.h"
#include "identity/device_signature.h"
#include "identity/device_pseudonym.h"  // salted, MAC-free device handle (Invariant III)

#include "canary/runtime_config.h"
#include "canary/witness.h"  // Ed25519 identity + canonical hash chain
#include "canary/detect_config.h"
#include "canary/diagnostics.h"
#include "pins.h"  // board identity (BOARD_NAME) from firmware/boards/<id>/pins
#include "core/feature_sanity.h"  // FEATURE_* vs HAS_* compile-time cross-check
#include "canary/net/wifi_mgr.h"
#include "canary/net/mdns_mgr.h"
#include "canary/net/mqtt_mgr.h"
#include "canary/net/ota_mgr.h"
// What the beacon SAYS — always compiled, never behind a band's flag, so
// turning one carrier off for flash budget can't silence the others.
#include "canary/net/fleet_beacon_payload.h"
#include <fleet_beacon.h>                     // FLEET_BEACON_DETECT_* class tokens
#if defined(FEATURE_FLEET_BEACON) && FEATURE_FLEET_BEACON
#include "canary/net/fleet_beacon_adv.h"      // carrier: BLE advert
#endif
#if defined(FEATURE_FLEET_UDP) && FEATURE_FLEET_UDP
#include "canary/net/fleet_udp.h"             // carrier: LAN multicast
#include "canary/net/fleet_espnow.h"          // carrier: ESP-NOW (router-free)
#endif
#if defined(FEATURE_FLEET_ROSTER) && FEATURE_FLEET_ROSTER
#include "canary/net/fleet_roster_scan.h" // RX twin: track the other Canaries
#endif
#include "canary/vision/vision_mgr.h"
#include "canary/vision/optical_features.h"  // coarse posture/proximity/occupancy names
#include "canary/state/presence_fsm.h"

static Topics TOPICS;
static canary::state::PresenceFSM fsm;

static char last_event_name[48] = "boot";
static uint32_t last_invoke_ms = 0;
static uint32_t last_heartbeat_ms = 0;
static uint32_t g_boot_count = 0;

// Broker reconnect schedule (canary-sense parity): a broker outage must
// never pin the loop — each bounded connect attempt happens at most once
// per backoff window (2 s → 4 s → 8 s → 16 s → 30 s cap).
static uint32_t g_mqtt_next_attempt_ms = 0;
static uint32_t g_mqtt_attempts = 0;

// Aim assist (bench/aiming): boxes-only live channel, toggled from HA.
// Auto-off after AIM_AUTO_OFF_MS so a forgotten switch can't stream box
// telemetry indefinitely. Never pixels — coordinates and scores only.
static bool     g_aim_on = false;
static uint32_t g_aim_started_ms = 0;
static uint32_t g_aim_last_pub_ms = 0;

static void aim_set(bool on, uint32_t now_ms) {
  if (g_aim_on == on) {
    canary::net::publish_aim_state_retained(TOPICS, g_aim_on);
    return;
  }
  g_aim_on = on;
  g_aim_started_ms = now_ms;
  g_aim_last_pub_ms = 0;
  canary::net::publish_aim_state_retained(TOPICS, g_aim_on);
  canary::log_line("AIM", on ? "Aim assist ON (auto-off in 10 min)."
                             : "Aim assist off.");
}

// Publish one aim frame, throttled: detection frames at AIM_PUBLISH_MS,
// empty frames at AIM_IDLE_PUBLISH_MS (so the card clears its box without
// spamming the broker while the room is empty).
static void aim_publish(const VisionSample& vs, uint32_t now_ms) {
  const uint32_t period = vs.person_now ? AIM_PUBLISH_MS : AIM_IDLE_PUBLISH_MS;
  if ((now_ms - g_aim_last_pub_ms) < period) return;
  g_aim_last_pub_ms = now_ms;

  char msg[256];
  const int n = snprintf(msg, sizeof(msg),
           "{"
           "\"present\":%s,"
           "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
           "\"score\":%d,"
           "\"vr\":%d,\"vc\":%d,\"rows\":%u,\"cols\":%u,"
           "\"fw\":%d,\"fh\":%d"
           "}",
           vs.person_now ? "true" : "false",
           vs.bbox.x, vs.bbox.y, vs.bbox.w, vs.bbox.h,
           vs.bbox.score,
           vs.voxel.r, vs.voxel.c,
           (unsigned)VOXEL_ROWS, (unsigned)VOXEL_COLS,
           FRAME_W, FRAME_H);
  if (n <= 0 || (size_t)n >= sizeof(msg)) return;
  canary::net::publish_aim(TOPICS, msg);
}

// Identify (the wizard's "which device is which" moment): a 10 s LED
// flash driven from HA's identify button or the companion app. Boards
// without a user LED (XIAO ESP32-C3) still publish the MQTT echo so the
// device card can pulse on screen instead.
static uint32_t g_identify_until_ms = 0;  // 0 = idle

#if defined(LED_BUILTIN) && (!defined(LED_BUILTIN_AVAILABLE) || LED_BUILTIN_AVAILABLE)
#define IDENTIFY_HAS_LED 1
#else
#define IDENTIFY_HAS_LED 0
#endif

static void identify_led(bool on) {
#if IDENTIFY_HAS_LED
#if defined(LED_ACTIVE_LOW) && LED_ACTIVE_LOW
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
#else
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
#endif
#else
  (void)on;
#endif
}

static void identify_start(uint32_t now_ms) {
  g_identify_until_ms = now_ms + 10000UL;
  if (g_identify_until_ms == 0) g_identify_until_ms = 1;  // wrap guard
#if IDENTIFY_HAS_LED
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  canary::net::publish_identify_echo(TOPICS, true);
  canary::log_line("IDFY", IDENTIFY_HAS_LED
                               ? "Identify: flashing LED for 10 s."
                               : "Identify: no user LED on this board — MQTT echo only.");
}

static void identify_tick(uint32_t now_ms) {
  if (!g_identify_until_ms) return;
  if ((int32_t)(now_ms - g_identify_until_ms) >= 0) {
    g_identify_until_ms = 0;
    identify_led(false);
    canary::net::publish_identify_echo(TOPICS, false);
    return;
  }
  // 2 Hz flash — unmistakable against a steady status LED.
  identify_led(((now_ms / 250) & 1) == 0);
}

// Trigger blink (the First Light pair demo's camera-side cue): one short
// solid LED pulse the instant the presence FSM flips. Standing next to the
// pair, the eye compares this flash against the glass lighting up — the
// trigger timing made visible with no bench gear. Identify owns the LED
// while its 10 s window runs; the pulse yields rather than fight it. On
// hosts without a user LED (the XIAO ESP32-C3 — see IDENTIFY_HAS_LED) this
// is a silent no-op and the serial PAIR line is the camera-side cue; the
// runbook says so rather than promising a light the board can't make.
static uint32_t g_pair_blink_until_ms = 0;  // 0 = idle
static uint32_t g_last_invoke_us = 0;       // last SSCMA invoke round-trip

static void pair_blink_start(uint32_t now_ms) {
  if (g_identify_until_ms) return;  // identify session owns the LED
#if IDENTIFY_HAS_LED
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  g_pair_blink_until_ms = now_ms + 150UL;
  if (g_pair_blink_until_ms == 0) g_pair_blink_until_ms = 1;  // wrap guard
}

static void pair_blink_tick(uint32_t now_ms) {
  if (!g_pair_blink_until_ms) return;
  if (g_identify_until_ms) {  // identify started mid-pulse: stand down
    g_pair_blink_until_ms = 0;
    return;
  }
  if ((int32_t)(now_ms - g_pair_blink_until_ms) >= 0) {
    g_pair_blink_until_ms = 0;
    identify_led(false);
    return;
  }
  identify_led(true);
}

static void set_last_event(const char* e) {
  strncpy(last_event_name, e ? e : "boot", sizeof(last_event_name) - 1);
  last_event_name[sizeof(last_event_name) - 1] = '\0';
}

static void publish_state_now(uint32_t now_ms) {
  const auto snap = fsm.snapshot(now_ms, last_event_name);
  canary::net::publish_state_retained(TOPICS, snap);
}

static void publish_heartbeat_now(uint32_t now_ms) {
  const auto snap = fsm.snapshot(now_ms, last_event_name);
  canary::net::publish_heartbeat(TOPICS, snap);
}

static void publish_event_json(
  const char* event_name,
  const char* reason,
  uint32_t now_ms,
  const VisionSample& vs
) {
  (void)vs;
  static uint32_t fallback_seq = 0;

  const auto snap = fsm.snapshot(now_ms, last_event_name);

  // Witness fields (LOCKED sense canonical — HA verify_sense_event):
  // presence/occupants come from the detector, range is honestly
  // "unknown" (an optical witness has no radar range bands), the time
  // bucket is the same 10-minute uptime coarsening canary-sense uses.
  const char* presence  = snap.presence ? "present" : "clear";
  const char* occupants = snap.presence ? "1" : "0";
  const char* range     = "unknown";
  const uint32_t bucket_uptime_s = (now_ms / 1000UL / 600UL) * 600UL;
  const uint32_t seq = canary::witness::ready()
                           ? canary::witness::chain_length() + 1
                           : ++fallback_seq;

  // Chain first: the witness record exists regardless of connectivity.
  canary::witness::chain_advance(seq, event_name, presence, occupants,
                                 range, bucket_uptime_s);

  // Envelope max is 142 bytes incl. NUL; the headroom keeps a future
  // constant bump from silently publishing unsigned events through the
  // fail-closed truncation path.
  char sig_env[160] = "";
  const bool signed_ok = canary::witness::sign_event_envelope(
      seq, event_name, presence, occupants, range, bucket_uptime_s,
      sig_env, sizeof(sig_env));

  char msg[1024];

  if (reason) {
    snprintf(msg, sizeof(msg),
      "{"
        "\"device_id\":\"%s\","
        "\"device_type\":\"%s\","
        "\"profile\":\"%s\","
        "\"event\":\"%s\","
        "\"reason\":\"%s\","
        "\"seq\":%lu,"
        "\"bucket_uptime_s\":%lu,"
        "\"presence\":\"%s\","
        "\"occupants\":\"%s\","
        "\"range\":\"%s\","
        "\"signed\":%s,"
        "\"ts_ms\":%lu,"
        "\"presence_ms\":%lu,"
        "\"dwell_ms\":%lu,"
        "\"visit_ms\":%lu,"
        "\"confidence\":%d,"
        "\"voxel\":{\"rows\":%u,\"cols\":%u,\"r\":%d,\"c\":%d},"
        "\"bbox\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},"
        "\"occupancy\":\"%s\","
        "\"posture\":\"%s\","
        "\"proximity\":\"%s\","
        "\"occ_mask\":%u"
        "%s"
      "}",
      canary::cfg::get().device_id, DEVICE_TYPE,
      canary::cfg::watch_profile(canary::cfg::detect().profile).key,
      event_name, reason,
      (unsigned long)seq,
      (unsigned long)bucket_uptime_s,
      presence, occupants, range,
      signed_ok ? "true" : "false",
      (unsigned long)now_ms,
      (unsigned long)snap.presence_ms,
      (unsigned long)snap.dwell_ms,
      (unsigned long)snap.visit_ms,
      snap.confidence,
      snap.voxel.rows, snap.voxel.cols, snap.voxel.r, snap.voxel.c,
      snap.bbox.x, snap.bbox.y, snap.bbox.w, snap.bbox.h,
      canary::vision::optical::occupancy_name(snap.person_count),
      canary::vision::optical::posture_name(snap.posture),
      canary::vision::optical::proximity_name(snap.proximity),
      (unsigned)snap.voxel_mask,
      sig_env
    );
  } else {
    snprintf(msg, sizeof(msg),
      "{"
        "\"device_id\":\"%s\","
        "\"device_type\":\"%s\","
        "\"profile\":\"%s\","
        "\"event\":\"%s\","
        "\"seq\":%lu,"
        "\"bucket_uptime_s\":%lu,"
        "\"presence\":\"%s\","
        "\"occupants\":\"%s\","
        "\"range\":\"%s\","
        "\"signed\":%s,"
        "\"ts_ms\":%lu,"
        "\"presence_ms\":%lu,"
        "\"dwell_ms\":%lu,"
        "\"visit_ms\":%lu,"
        "\"confidence\":%d,"
        "\"voxel\":{\"rows\":%u,\"cols\":%u,\"r\":%d,\"c\":%d},"
        "\"bbox\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},"
        "\"occupancy\":\"%s\","
        "\"posture\":\"%s\","
        "\"proximity\":\"%s\","
        "\"occ_mask\":%u"
        "%s"
      "}",
      canary::cfg::get().device_id, DEVICE_TYPE,
      canary::cfg::watch_profile(canary::cfg::detect().profile).key,
      event_name,
      (unsigned long)seq,
      (unsigned long)bucket_uptime_s,
      presence, occupants, range,
      signed_ok ? "true" : "false",
      (unsigned long)now_ms,
      (unsigned long)snap.presence_ms,
      (unsigned long)snap.dwell_ms,
      (unsigned long)snap.visit_ms,
      snap.confidence,
      snap.voxel.rows, snap.voxel.cols, snap.voxel.r, snap.voxel.c,
      snap.bbox.x, snap.bbox.y, snap.bbox.w, snap.bbox.h,
      canary::vision::optical::occupancy_name(snap.person_count),
      canary::vision::optical::posture_name(snap.posture),
      canary::vision::optical::proximity_name(snap.proximity),
      (unsigned)snap.voxel_mask,
      sig_env
    );
  }

  canary::net::publish_event(TOPICS, msg);
  // The event advanced the chain — refresh the retained signed head
  // (same cadence as canary-sense: events are rare, one small publish).
  canary::net::publish_chain_retained(TOPICS);
}

static void vision_serial_write(const char* str) {
  canary::dbg_serial().print(str);
}

static uint32_t increment_boot_count() {
  Preferences prefs;
  if (!prefs.begin("securacv", /*readOnly=*/false)) return 0;
  uint32_t boots = prefs.getULong("boots", 0);
  if (boots != UINT32_MAX) ++boots;
  prefs.putULong("boots", boots);
  prefs.end();
  return boots;
}

static void hex32(const uint8_t* bytes, char out[65]) {
  static const char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; ++i) {
    out[i * 2] = HEX_DIGITS[(bytes[i] >> 4) & 0x0f];
    out[i * 2 + 1] = HEX_DIGITS[bytes[i] & 0x0f];
  }
  out[64] = '\0';
}

// `j` is the app-facing print receipt. It is public-only and remains available
// when Wi-Fi/MQTT are absent, which is exactly when first-boot diagnosis needs
// it most.
static void emit_self_manifest() {
  const char* features[] = {
    "vision", "mqtt", "home_assistant", "ota", "diagnostics",
#if defined(FEATURE_FLEET_BEACON) && FEATURE_FLEET_BEACON
    "fleet_beacon",
#endif
#if defined(FEATURE_FLEET_ROSTER) && FEATURE_FLEET_ROSTER
    "fleet_roster",
#endif
  };
  const manifest::Cmd commands[] = {
    {'h', "help"},
    {'j', "self_manifest"},
  };
  char chain_head[65];
  hex32(canary::witness::chain_head(), chain_head);
  const int health = (canary::vision::ready() ? 70 : 0) +
                     (canary::witness::ready() ? 30 : 0);

  manifest::Facts facts{};
  facts.board = DEVICE_TYPE;
  facts.firmware = CANARY_FW_VERSION;
#ifdef FIRMWARE_GIT_HASH
  facts.git = FIRMWARE_GIT_HASH;
#else
  facts.git = "not-embedded";
#endif
  facts.protocol = "pwk:v0.3.0";
  facts.device_id = canary::cfg::get().device_id;
  facts.pubkey_hex = device_signature::pubkey_hex();
  facts.pubkey_fp_hex = device_signature::fingerprint_hex();
  facts.chain_head_hex = chain_head;
  facts.seq = canary::witness::chain_length();
  facts.boots = g_boot_count;
  facts.health = health;
  facts.tamper = false;
  facts.features = features;
  facts.feature_count = sizeof(features) / sizeof(features[0]);
  facts.commands = commands;
  facts.command_count = sizeof(commands) / sizeof(commands[0]);
  facts.fleet = nullptr;
  facts.fleet_count = 0;
  facts.help_url = "https://securacv.com/canary";

  char output[1600];
  const size_t n = manifest::build(facts, output, sizeof(output));
  canary::dbg_serial().println();
  if (n) canary::dbg_serial().println(output);
  else canary::dbg_serial().println("{\"error\":\"manifest overflow\"}");

  // A separate additive record keeps the shared manifest schema stable while
  // giving the native flasher a hard, live gate for the two-board Vision unit.
  canary::dbg_serial().printf(
      "{\"schema\":\"securacv.vision.proof/v1\","
      "\"module_id\":%d,\"i2c_ready\":%s,\"witness_ready\":%s,"
      "\"wifi_configured\":%s,\"wifi_connected\":%s,\"mqtt_connected\":%s}\n",
      canary::vision::module_id(),
      canary::vision::ready() ? "true" : "false",
      canary::witness::ready() ? "true" : "false",
      canary::net::wifi_configured() ? "true" : "false",
      canary::net::wifi_connected() ? "true" : "false",
      canary::net::mqtt_connected() ? "true" : "false");
}

static void handle_serial_commands() {
  while (canary::dbg_serial().available()) {
    const char command = (char)canary::dbg_serial().read();
    if (command == 'j' || command == 'J') {
      emit_self_manifest();
    } else if (command == 'h' || command == 'H' || command == '?') {
      canary::dbg_serial().println("\n=== Commands ===");
      canary::dbg_serial().println("  j - print machine-readable install receipt");
      canary::dbg_serial().println("  h - this help");
    }
  }
}

// Apply runtime detection settings written by HA's number entities. The
// MQTT callback only latches the parsed values; this drains them on the
// main task, persists via detect_config, and mirrors the retained state.
static void drain_detect_cfg_commands() {
  bool changed = false;
  bool any_cmd = false;
  long v;

  // Profile first: selecting one applies its preset to the four settings
  // below, so a same-loop individual tweak still wins over the preset.
  if ((v = canary::net::take_pending_cfg_profile()) >= 0) {
    any_cmd = true;
    changed |= canary::cfg::detect_set_profile((uint8_t)v);
  }
  if ((v = canary::net::take_pending_cfg_target()) >= 0) {
    any_cmd = true;
    changed |= canary::cfg::detect_set_person_target((uint8_t)v);
  }
  if ((v = canary::net::take_pending_cfg_score()) >= 0) {
    any_cmd = true;
    changed |= canary::cfg::detect_set_score_min((uint8_t)v);
  }
  if ((v = canary::net::take_pending_cfg_lost()) >= 0) {
    any_cmd = true;
    changed |= canary::cfg::detect_set_lost_timeout_ms((uint32_t)v);
  }
  if ((v = canary::net::take_pending_cfg_dwell()) >= 0) {
    any_cmd = true;
    changed |= canary::cfg::detect_set_dwell_start_ms((uint32_t)v);
  }

  if (changed) {
    const auto& det = canary::cfg::detect();
    canary::log_header("CFG");
    canary::dbg_serial().printf(
        "Detection settings: profile=%s target=%u score>=%u lost=%lums dwell=%lums\n",
        canary::cfg::watch_profile(det.profile).key,
        (unsigned)det.person_target, (unsigned)det.score_min,
        (unsigned long)det.lost_timeout_ms, (unsigned long)det.dwell_start_ms);
  }
  // Republish on every inbound command, even a clamped-to-same one: HA's
  // number entity optimistically shows what the user typed until the
  // retained state corrects it.
  if (any_cmd) canary::net::publish_detect_cfg_retained(TOPICS);
}

void setup() {
  canary::dbg_serial().begin(115200);
  delay(600);

  g_boot_count = increment_boot_count();

  boot_set_output(vision_serial_write);

  // Privacy (Invariant III): never surface the raw MAC. The stable device handle
  // is the salted, MAC-free pseudonym shown as "Hardware ID" below; mac_address is
  // left null so the boot banner skips the MAC line.
  boot_info_t bi = {};
  bi.product_name  = "SecuraCV Canary Vision";
  bi.fw_version    = CANARY_FW_VERSION;
  bi.build_date    = __DATE__;
  bi.build_time    = __TIME__;
  bi.device_type   = DEVICE_TYPE;
  bi.model         = MODEL;
  bi.board_name    = BOARD_NAME;  // from the board's pins.h
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

  boot_scene_banner(&bi);
  boot_scene_hardware(&bi);

  // Vision-specific config
  boot_line("              ,_,");
  boot_line("             (^.^)         What can I see?");
  boot_line("              |#|");
  boot_line("             [###]");
  boot_separator();
  boot_kv("Sensor",  "Grove Vision AI V2 (SSCMA)");
  // NVS-backed runtime settings (adjustable from HA; compiled values seed
  // the first boot only — see canary/detect_config.h).
  const auto& det = canary::cfg::detect();
  const auto& prof = canary::cfg::watch_profile(det.profile);
  boot_kvf("Profile", "%s  (watching for a %s)", prof.key, prof.subject);
  boot_kvf("Target",  "class %u  (the model's %s class)", (unsigned)det.person_target, prof.subject);
  boot_kvf("Score",   ">= %u%%  (confidence threshold)", (unsigned)det.score_min);
  boot_kvf("Lost",    "%lu ms  (timeout before '%s left')", (unsigned long)det.lost_timeout_ms, prof.subject);
  boot_kvf("Dwell",   "%lu ms  (lingering detection)", (unsigned long)det.dwell_start_ms);
  boot_kvf("Voxel",   "%ux%u grid (%dx%d frame)", VOXEL_COLS, VOXEL_ROWS, FRAME_W, FRAME_H);
  boot_kvf("Rate",    "every %lu ms", (unsigned long)INVOKE_PERIOD_MS);
  boot_blank();

  TOPICS = build_topics(canary::cfg::get().device_id);

  fsm.reset();

  canary::net::wifi_init_or_reboot();

  // Fleet LAN presence: the _securacv._tcp advert (device_id/name/host/
  // fw/model/dt/role TXT) that lets the companion app, canary-display,
  // and sibling Canaries find and label this witness. Failure is
  // non-fatal — MQTT/HA never depends on it.
  canary::net::mdns_init();

  // Seed the heap-health snapshot so the first status publish carries real
  // numbers instead of zeros.
  canary::diag::loop(canary::ms_now());

  // Confirm (or roll back) a freshly installed image now — before anything
  // that can block on external services. See ota_mgr.h.
  canary::net::ota_boot_validate();

  canary::net::mqtt_init(TOPICS);
  canary::vision::init();

  // MQTT connection
  boot_line("              ,_,  ))");
  boot_line("             (o.o)  ))     Connecting to MQTT...");
  boot_line("              | |");
  boot_separator();
  boot_kv("Device ID", canary::cfg::get().device_id);
  char devid_hex[device_pseudonym::HEX_LEN + 1] = {0};
  if (device_pseudonym::device_id_hex(devid_hex, sizeof(devid_hex))) {
    boot_kv("Hardware ID", devid_hex);  // salted pseudonym, not the raw MAC
  }
  boot_kvf("Heartbeat", "every %lu ms", (unsigned long)HEARTBEAT_MS);
  boot_kv("HA prefix", HA_DISCOVERY_PREFIX);
  boot_blank();

  // Witness identity + canonical chain: keypair from NVS (generated on
  // first boot), shared signer init. A failure means events publish
  // unsigned — never blocks the optical pipeline.
  if (canary::witness::init()) {
    boot_kv("Witness", "Ed25519 identity ready (events signed)");
  } else {
    boot_kv("Witness", "signing unavailable (events publish unsigned)");
  }

#if defined(FEATURE_FLEET_BEACON) && FEATURE_FLEET_BEACON
  // Fleet-link BLE presence beacon: WiFi STA is up (they coexist on the C3/C6
  // shared radio) and the witness fingerprint is available, so a canary-display
  // can find this witness directly over BLE — broker-free and WiFi-free.
  // Fail-safe: a stack that can't come up degrades to a no-op.
  canary::net::fleet_beacon_begin(canary::ms_now());
#endif

#if defined(FEATURE_FLEET_UDP) && FEATURE_FLEET_UDP
  // The LAN band for the same beacon. Nothing here needs WiFi to be up yet —
  // the socket is opened by the first tick that finds an address, and reopened
  // if that address ever changes, so first join and rejoin are one code path.
  canary::net::fleet_udp_begin(canary::ms_now());
#endif

#if defined(FEATURE_FLEET_ESPNOW) && FEATURE_FLEET_ESPNOW
  // The router-free band for the same beacon. The WiFi driver is up in STA
  // mode even on an unprovisioned unit (wifi_init_or_reboot), which is all
  // ESP-NOW needs — this is the carrier a factory-fresh pair meets on.
  canary::net::fleet_espnow_begin(canary::ms_now());
#endif

  // Bounded boot attempt: if the broker is down at boot the device still
  // finishes setup — the loop's backoff supervisor brings the link (and
  // every retained surface, discovery included) up when the broker returns.
  if (canary::net::wifi_connected() &&
      canary::cfg::mqtt_credentials_configured() &&
      canary::net::mqtt_connect_attempt()) {
    canary::net::ha_discovery_publish_once(TOPICS);

    // Broker link is up — gossip it on the fleet advert so a display or a
    // sibling Canary that lost its broker can self-heal from ours.
    canary::net::mdns_advertise_broker(canary::cfg::get().mqtt_host,
                                       canary::cfg::get().mqtt_port);

    canary::net::publish_health_retained(TOPICS);   // carries public_key (TOFU)
    canary::net::publish_chain_retained(TOPICS);    // retained signed head
  } else {
    if (!canary::cfg::mqtt_credentials_configured()) {
      canary::log_line("MQTT", "Provisioning required — broker settings are placeholders; continuing offline.");
    } else if (!canary::net::wifi_connected()) {
      canary::log_line("MQTT", "WiFi is offline — broker connect deferred to the reconnect supervisor.");
    } else {
      canary::log_line("MQTT",
                       "Broker unreachable at boot — continuing; the loop's "
                       "backoff supervisor owns the retry.");
    }
  }

  // Signed pull-OTA: arm the engine (validation already ran right after
  // WiFi). Daily jittered checks; HA's Install button and auto-update
  // switch are drained in loop().
  canary::net::ota_init(TOPICS);

  set_last_event("boot");
  publish_state_now(canary::ms_now());
  delay(250);
  publish_state_now(canary::ms_now());

  last_invoke_ms = canary::ms_now();
  last_heartbeat_ms = canary::ms_now();

#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
  // Task watchdog — same IDF5 config as canary-sense/wap, armed LAST so the
  // blocking boot phases above (WiFi connect, one bounded broker attempt)
  // can't trip it. config.h has claimed FEATURE_WATCHDOG since day one but
  // nothing ever armed it (docs/strategy/12, finding F1): a hung loop was
  // a permanent freeze. Timeout must exceed loop()'s worst bounded block
  // (one MQTT connect attempt against a dead broker).
  // Same dual-API guard as firmware/canary: this project's PlatformIO env
  // pins an IDF 4.x-era core where the struct API doesn't exist yet.
  {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    // ESP-IDF 5.x: struct-based API
    esp_task_wdt_config_t wdt_config = {
      .timeout_ms = CV_WATCHDOG_TIMEOUT_SEC * 1000,
#if CONFIG_FREERTOS_UNICORE
      .idle_core_mask = (1 << 0),               // XIAO ESP32-C3: single core
#else
      .idle_core_mask = (1 << 0) | (1 << 1),    // ESP32-S3 boards: both cores
#endif
      .trigger_panic = true
    };
    esp_err_t wdt_err = esp_task_wdt_reconfigure(&wdt_config);
    if (wdt_err == ESP_ERR_INVALID_STATE) {
      esp_task_wdt_init(&wdt_config);
    }
#else
    // ESP-IDF 4.x: simple parameters (panics on timeout)
    esp_task_wdt_init(CV_WATCHDOG_TIMEOUT_SEC, true);
#endif
    esp_task_wdt_add(NULL);
    boot_kvf("Watchdog", "%lu s timeout", (unsigned long)CV_WATCHDOG_TIMEOUT_SEC);
  }
#endif

  boot_scene_ready(
      "It will publish presence events via MQTT",
      "to Home Assistant for real-time monitoring.",
      NULL
  );
}

// The optical pipeline: one rate-limited inference, the presence FSM, the
// witness chain, and the BLE beacon. Deliberately BROKER-INDEPENDENT — see
// the call site in loop(). Returns true when this pass actually sampled.
//
// Every MQTT publish reachable from here is already connection-gated
// (publish_checked/publish_aim no-op when the client is down), so an off-grid
// pass does exactly the broker-free work and nothing else: it advances the
// signed chain — "the witness record exists regardless of connectivity", the
// rule publish_event_json has always stated — and it refreshes the beacon.
static bool vision_tick(uint32_t now_ms) {
  // Under heap pressure the diagnostics ladder stretches the vision cadence
  // (2x at critical, 5x at emergency) so inference never OOMs the stack.
  const uint32_t invoke_period_ms =
      INVOKE_PERIOD_MS * canary::diag::period_scale();
  if ((now_ms - last_invoke_ms) < invoke_period_ms) return false;
  last_invoke_ms = now_ms;

  VisionSample vs{};
  const uint32_t invoke_t0_us = micros();
  if (!canary::vision::sample(vs)) return false;
  g_last_invoke_us = micros() - invoke_t0_us;

  if (g_aim_on) aim_publish(vs, now_ms);

  EventMsg ev{};
  const bool emitted = fsm.tick(vs, now_ms, ev);

  // Mirror the FSM's debounced presence onto the fleet beacon (v2: ALERT flag
  // + class + confidence) so a canary-display alerts DIRECTLY — no broker, no
  // hub. Debounced presence, not the raw per-frame hit, so the beacon doesn't
  // flap on a single dropped frame. The class token comes from the active
  // watch profile (person for room_presence, animal for litter_box) — always
  // within the ObjectClass vocabulary, never past it.
  //
  // Ungated on purpose: this sets what the beacon SAYS, and every carrier
  // reads it. Gating it on any one band would mean turning off BLE for flash
  // budget also silenced the detection on the LAN.
  {
    const auto snap = fsm.snapshot(now_ms, last_event_name);
    const uint32_t gen_before = canary::net::fleet_beacon_payload_generation();
    canary::net::fleet_beacon_note_detection(
        snap.presence,
        canary::cfg::watch_profile(canary::cfg::detect().profile).beacon_class,
        snap.confidence, now_ms);
    if (canary::net::fleet_beacon_payload_generation() != gen_before) {
      // A beacon-visible edge: make the trigger timing observable on the
      // bench. The LED pulse is the camera-side cue the pair demo's glass
      // is compared against; the invoke number is the NPU round-trip this
      // pass actually measured (each side reports only its own clock — the
      // wire stays timestamp-free by contract).
      pair_blink_start(now_ms);
      canary::log_header("PAIR");
      canary::dbg_serial().printf(
          "detection edge (%s) - invoke took %lu us this pass\n",
          snap.presence ? "presence on" : "presence off",
          (unsigned long)g_last_invoke_us);
    }
  }

  if (emitted && ev.event_name) {
    set_last_event(ev.event_name);
    publish_event_json(ev.event_name, ev.reason, now_ms, vs);
    publish_state_now(now_ms);
  }
  return true;
}

void loop() {
#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
  esp_task_wdt_reset();
#endif

  // Always service USB proof before any network-dependent early return.
  handle_serial_commands();

  // STA supervision first: backoff reconnects, outage reboot (S3 parity).
  canary::net::wifi_loop(canary::ms_now());
  canary::net::mdns_loop(canary::ms_now());  // drain deferred re-announce
  canary::diag::loop(canary::ms_now());

#if defined(FEATURE_FLEET_BEACON) && FEATURE_FLEET_BEACON
  // Refresh the BLE presence beacon every pass (internally rate-limited to
  // ~5 s). Placed before the broker/WiFi early-returns below so it keeps
  // advertising through an MQTT outage — that broker-free reach is the point.
  canary::net::fleet_beacon_tick(canary::ms_now());
#endif

#if defined(FEATURE_FLEET_UDP) && FEATURE_FLEET_UDP
  // The same beacon on the LAN band. Also before the broker early-returns: an
  // MQTT outage is exactly when this matters, and the carrier's own STA check
  // is what decides whether it can send — it never blocks or retries in line,
  // so a down link costs this call nothing (the C3 lesson: a fleet transport
  // must never be the reason a rejoin stalls).
  canary::net::fleet_udp_tick(canary::ms_now());
#endif

#if defined(FEATURE_FLEET_ESPNOW) && FEATURE_FLEET_ESPNOW
  // The router-free band. Same placement, same reasoning as the two ticks
  // above: an edge must reach a nearby display whether or not any network
  // or broker exists — that broker-free reach is the whole band.
  canary::net::fleet_espnow_tick(canary::ms_now());
#endif

#if defined(FEATURE_FLEET_ROSTER) && FEATURE_FLEET_ROSTER
  // Low-duty passive scan that hears the OTHER Canaries and keeps this
  // witness's own fleet roster (last-heartbeat + status). Broker-independent
  // like the beacon; also before the early-returns so it keeps tracking peers
  // through an MQTT/WiFi outage (continuous scan when fully off-grid).
  canary::net::fleet_roster_scan_tick(canary::ms_now(),
                                      canary::net::wifi_connected(),
                                      canary::net::wifi_configured());
#endif

  // Optical pipeline BEFORE the broker/WiFi early-returns below — the same
  // placement, and the same reason, as the beacon and roster ticks above.
  // Seeing is not a broker-dependent act: a Vision booted off-grid, or one
  // that loses WiFi or just the broker, must keep running inference, keep
  // advancing its signed chain, and keep telling nearby displays what it
  // currently sees. Left below the gate it would freeze — and, worse, the
  // beacon tick above would go on re-advertising the last detection, so a
  // display would show a person who had already left. (review catch)
  const bool sampled = vision_tick(canary::ms_now());

  if (!canary::net::mqtt_connected()) {
    if (!canary::net::wifi_connected()) {
      // No link, no broker — let wifi_loop() drive recovery.
      delay(50);
      return;
    }
    // Bounded single attempt on an exponential backoff (canary-sense
    // parity). The old code blocked here in an unbounded retry loop — on a
    // live-WiFi/dead-broker network the witness froze until power-cycle,
    // and delay(1000) yielding to IDLE meant not even the idle watchdog
    // fired (docs/strategy/12, finding F1).
    const uint32_t mqtt_now = canary::ms_now();
    if ((int32_t)(mqtt_now - g_mqtt_next_attempt_ms) >= 0) {
      // Ground-truth-only gossip: stop referring the fleet to a broker we
      // can't reach ourselves.
      canary::net::mdns_clear_broker();
      if (canary::net::mqtt_connect_attempt()) {
        g_mqtt_attempts = 0;
        // Covers the broker-was-down-at-boot case; internally once-guarded.
        canary::net::ha_discovery_publish_once(TOPICS);
        canary::net::mdns_advertise_broker(canary::cfg::get().mqtt_host,
                                           canary::cfg::get().mqtt_port);
        canary::net::publish_health_retained(TOPICS);
        canary::net::publish_chain_retained(TOPICS);
        publish_state_now(canary::ms_now());
      } else {
        uint32_t attempt = g_mqtt_attempts;
        if (attempt > 4) attempt = 4;
        uint32_t backoff_ms = 2000UL << attempt;
        if (backoff_ms > 30000UL) backoff_ms = 30000UL;
        // Fleet reconnect jitter: up to a fraction of the backoff so brokers
        // don't see a herd.
        backoff_ms += esp_random() % (backoff_ms / 4 + 1);
        g_mqtt_attempts++;
        g_mqtt_next_attempt_ms = mqtt_now + backoff_ms;
      }
    }
    // Still no broker: keep the loop's cadence (WDT fed, wifi/mdns/diag
    // supervised, and the optical pipeline above already ran) instead of
    // running the publish-dependent tail.
    if (!canary::net::mqtt_connected()) {
      delay(50);
      return;
    }
  }

  canary::net::mqtt_loop();

  const uint32_t now_ms = canary::ms_now();

  // Pull-OTA: scheduler + HA command drain + update-entity publishing.
  canary::net::ota_loop(now_ms);

  // Runtime detection settings written from HA. Drained every connected pass,
  // so slider changes land at MQTT speed and the next vision_tick — which
  // runs at the top of the following pass — samples under the new config.
  drain_detect_cfg_commands();

  // Aim-assist switch: drain the HA command and enforce the auto-off.
  {
    const int aim_cmd = canary::net::take_pending_aim();
    if (aim_cmd >= 0) aim_set(aim_cmd == 1, now_ms);
    if (g_aim_on &&
        (now_ms - g_aim_started_ms) > AIM_AUTO_OFF_MS) {
      canary::log_line("AIM", "Auto-off (10 min elapsed).");
      aim_set(false, now_ms);
    }
  }

  // Identify: drain the button press and drive the blink window.
  if (canary::net::take_pending_identify()) identify_start(now_ms);
  identify_tick(now_ms);
  pair_blink_tick(now_ms);

  if ((now_ms - last_heartbeat_ms) > HEARTBEAT_MS) {
    last_heartbeat_ms = now_ms;
    publish_heartbeat_now(now_ms);
    publish_state_now(now_ms);
  }

  if (!sampled) delay(5);
}
