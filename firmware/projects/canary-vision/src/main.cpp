/*
  SecuraCV Canary Vision — Optical Witness Sensor Firmware
  --------------------------------------------------------
  (c) 2026 Errer Labs / SecuraCV
  errerlabs.com | securacv.com
  GitHub: https://github.com/kmay89/securaCV

  License: Apache-2.0 (use repository license unless otherwise specified).
*/

#include <Arduino.h>
#include <WiFi.h>

#include "canary/config.h"
#include "canary/version.h"
#include "canary/log.h"
#include "canary/topics.h"
#include "canary/types.h"
#include "boot/boot_banner.h"
#include "identity/device_pseudonym.h"  // salted, MAC-free device handle (Invariant III)

#include "canary/runtime_config.h"
#include "canary/witness.h"  // Ed25519 identity + canonical hash chain
#include "canary/detect_config.h"
#include "canary/diagnostics.h"
#include "pins.h"  // board identity (BOARD_NAME) from firmware/boards/<id>/pins
#include "canary/net/wifi_mgr.h"
#include "canary/net/mqtt_mgr.h"
#include "canary/net/ota_mgr.h"
#include "canary/vision/vision_mgr.h"
#include "canary/state/presence_fsm.h"

static Topics TOPICS;
static canary::state::PresenceFSM fsm;

static char last_event_name[48] = "boot";
static uint32_t last_invoke_ms = 0;
static uint32_t last_heartbeat_ms = 0;

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

  char sig_env[144] = "";
  const bool signed_ok = canary::witness::sign_event_envelope(
      seq, event_name, presence, occupants, range, bucket_uptime_s,
      sig_env, sizeof(sig_env));

  char msg[1024];

  if (reason) {
    snprintf(msg, sizeof(msg),
      "{"
        "\"device_id\":\"%s\","
        "\"device_type\":\"%s\","
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
        "\"confidence\":%d,"
        "\"voxel\":{\"rows\":%u,\"cols\":%u,\"r\":%d,\"c\":%d},"
        "\"bbox\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}"
        "%s"
      "}",
      canary::cfg::get().device_id, DEVICE_TYPE,
      event_name, reason,
      (unsigned long)seq,
      (unsigned long)bucket_uptime_s,
      presence, occupants, range,
      signed_ok ? "true" : "false",
      (unsigned long)now_ms,
      (unsigned long)snap.presence_ms,
      (unsigned long)snap.dwell_ms,
      snap.confidence,
      snap.voxel.rows, snap.voxel.cols, snap.voxel.r, snap.voxel.c,
      snap.bbox.x, snap.bbox.y, snap.bbox.w, snap.bbox.h,
      sig_env
    );
  } else {
    snprintf(msg, sizeof(msg),
      "{"
        "\"device_id\":\"%s\","
        "\"device_type\":\"%s\","
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
        "\"confidence\":%d,"
        "\"voxel\":{\"rows\":%u,\"cols\":%u,\"r\":%d,\"c\":%d},"
        "\"bbox\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}"
        "%s"
      "}",
      canary::cfg::get().device_id, DEVICE_TYPE,
      event_name,
      (unsigned long)seq,
      (unsigned long)bucket_uptime_s,
      presence, occupants, range,
      signed_ok ? "true" : "false",
      (unsigned long)now_ms,
      (unsigned long)snap.presence_ms,
      (unsigned long)snap.dwell_ms,
      snap.confidence,
      snap.voxel.rows, snap.voxel.cols, snap.voxel.r, snap.voxel.c,
      snap.bbox.x, snap.bbox.y, snap.bbox.w, snap.bbox.h,
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

// Apply runtime detection settings written by HA's number entities. The
// MQTT callback only latches the parsed values; this drains them on the
// main task, persists via detect_config, and mirrors the retained state.
static void drain_detect_cfg_commands() {
  bool changed = false;
  bool any_cmd = false;
  long v;

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
        "Detection settings: target=%u score>=%u lost=%lums dwell=%lums\n",
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
  boot_kvf("Target",  "class %u  (person detection)", (unsigned)det.person_target);
  boot_kvf("Score",   ">= %u%%  (confidence threshold)", (unsigned)det.score_min);
  boot_kvf("Lost",    "%lu ms  (timeout before 'person left')", (unsigned long)det.lost_timeout_ms);
  boot_kvf("Dwell",   "%lu ms  (lingering detection)", (unsigned long)det.dwell_start_ms);
  boot_kvf("Voxel",   "%ux%u grid (%dx%d frame)", VOXEL_COLS, VOXEL_ROWS, FRAME_W, FRAME_H);
  boot_kvf("Rate",    "every %lu ms", (unsigned long)INVOKE_PERIOD_MS);
  boot_blank();

  TOPICS = build_topics(canary::cfg::get().device_id);

  fsm.reset();

  canary::net::wifi_init_or_reboot();

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

  canary::net::mqtt_reconnect_blocking();
  canary::net::ha_discovery_publish_once(TOPICS);

  canary::net::publish_status_retained(TOPICS, "online");
  canary::net::publish_health_retained(TOPICS);   // carries public_key (TOFU)
  canary::net::publish_chain_retained(TOPICS);    // retained signed head

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

  boot_scene_ready(
      "It will publish presence events via MQTT",
      "to Home Assistant for real-time monitoring.",
      NULL
  );
}

void loop() {
  // STA supervision first: backoff reconnects, outage reboot (S3 parity).
  canary::net::wifi_loop(canary::ms_now());
  canary::diag::loop(canary::ms_now());

  if (!canary::net::mqtt_connected()) {
    if (!canary::net::wifi_connected()) {
      // No link, no broker — let wifi_loop() drive recovery.
      delay(50);
      return;
    }
    canary::log_line("MQTT", "Disconnected. Reconnecting...");
    canary::net::mqtt_reconnect_blocking();
    if (!canary::net::mqtt_connected()) return;  // WiFi dropped mid-attempt
    canary::net::publish_status_retained(TOPICS, "online");
    canary::net::publish_health_retained(TOPICS);
    canary::net::publish_chain_retained(TOPICS);
    publish_state_now(canary::ms_now());
    delay(250);
    publish_state_now(canary::ms_now());
  }

  canary::net::mqtt_loop();

  const uint32_t now_ms = canary::ms_now();

  // Pull-OTA: scheduler + HA command drain + update-entity publishing.
  // Must run before the vision-rate early return below.
  canary::net::ota_loop(now_ms);

  // Runtime detection settings written from HA. Also before the early
  // return so slider changes apply at MQTT speed, not vision-tick speed.
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

  if ((now_ms - last_heartbeat_ms) > HEARTBEAT_MS) {
    last_heartbeat_ms = now_ms;
    publish_heartbeat_now(now_ms);
    publish_state_now(now_ms);
  }

  // Under heap pressure the diagnostics ladder stretches the vision cadence
  // (2x at critical, 5x at emergency) so inference never OOMs the stack.
  const uint32_t invoke_period_ms =
      INVOKE_PERIOD_MS * canary::diag::period_scale();
  if ((now_ms - last_invoke_ms) < invoke_period_ms) {
    delay(5);
    return;
  }
  last_invoke_ms = now_ms;

  VisionSample vs{};
  if (!canary::vision::sample(vs)) return;

  if (g_aim_on) aim_publish(vs, now_ms);

  EventMsg ev{};
  const bool emitted = fsm.tick(vs, now_ms, ev);

  if (emitted && ev.event_name) {
    set_last_event(ev.event_name);
    publish_event_json(ev.event_name, ev.reason, now_ms, vs);
    publish_state_now(now_ms);
  }
}
