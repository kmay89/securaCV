/*
  SecuraCV Canary Vision — Optical Witness Sensor Firmware
  --------------------------------------------------------
  (c) 2026 Errer Labs / SecuraCV
  errerlabs.com | securacv.com
  GitHub: https://github.com/kmay89/securaCV

  License: Apache-2.0 (use repository license unless otherwise specified).
*/

#include <Arduino.h>

#include "canary/config.h"
#include "canary/version.h"
#include "canary/log.h"
#include "canary/topics.h"
#include "canary/types.h"

#include "canary/net/wifi_mgr.h"
#include "canary/net/mqtt_mgr.h"
#include "canary/vision/vision_mgr.h"
#include "canary/state/presence_fsm.h"

static Topics TOPICS;
static canary::state::PresenceFSM fsm;

static char last_event_name[48] = "boot";
static uint32_t last_invoke_ms = 0;
static uint32_t last_heartbeat_ms = 0;

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
  static uint32_t seq = 0;

  const auto snap = fsm.snapshot(now_ms, last_event_name);
  char msg[768];

  if (reason) {
    snprintf(msg, sizeof(msg),
      "{"
        "\"device_id\":\"%s\","
        "\"device_type\":\"%s\","
        "\"event\":\"%s\","
        "\"reason\":\"%s\","
        "\"seq\":%lu,"
        "\"ts_ms\":%lu,"
        "\"presence_ms\":%lu,"
        "\"dwell_ms\":%lu,"
        "\"confidence\":%d,"
        "\"voxel\":{\"rows\":%u,\"cols\":%u,\"r\":%d,\"c\":%d},"
        "\"bbox\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}"
      "}",
      DEVICE_ID, DEVICE_TYPE,
      event_name, reason,
      (unsigned long)(++seq),
      (unsigned long)now_ms,
      (unsigned long)snap.presence_ms,
      (unsigned long)snap.dwell_ms,
      snap.confidence,
      snap.voxel.rows, snap.voxel.cols, snap.voxel.r, snap.voxel.c,
      snap.bbox.x, snap.bbox.y, snap.bbox.w, snap.bbox.h
    );
  } else {
    snprintf(msg, sizeof(msg),
      "{"
        "\"device_id\":\"%s\","
        "\"device_type\":\"%s\","
        "\"event\":\"%s\","
        "\"seq\":%lu,"
        "\"ts_ms\":%lu,"
        "\"presence_ms\":%lu,"
        "\"dwell_ms\":%lu,"
        "\"confidence\":%d,"
        "\"voxel\":{\"rows\":%u,\"cols\":%u,\"r\":%d,\"c\":%d},"
        "\"bbox\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}"
      "}",
      DEVICE_ID, DEVICE_TYPE,
      event_name,
      (unsigned long)(++seq),
      (unsigned long)now_ms,
      (unsigned long)snap.presence_ms,
      (unsigned long)snap.dwell_ms,
      snap.confidence,
      snap.voxel.rows, snap.voxel.cols, snap.voxel.r, snap.voxel.c,
      snap.bbox.x, snap.bbox.y, snap.bbox.w, snap.bbox.h
    );
  }

  canary::net::publish_event(TOPICS, msg);
}

void setup() {
  canary::dbg_serial().begin(115200);
  delay(600);

  TOPICS = build_topics();

  canary::log_line("BOOT", "SecuraCV Canary Vision starting...");
  canary::log_header("BOOT");
  canary::dbg_serial().printf(
    "Device=%s Type=%s FW=%s\n",
    DEVICE_ID,
    DEVICE_TYPE,
    CANARY_FW_VERSION
  );

  fsm.reset();

  canary::net::wifi_init_or_reboot();
  canary::net::mqtt_init(TOPICS);
  canary::vision::init();

  canary::net::mqtt_reconnect_blocking();
  canary::net::ha_discovery_publish_once(TOPICS);

  canary::net::publish_status_retained(TOPICS, "online");

  set_last_event("boot");
  publish_state_now(canary::ms_now());
  delay(250);
  publish_state_now(canary::ms_now());

  last_invoke_ms = canary::ms_now();
  last_heartbeat_ms = canary::ms_now();

  canary::log_line("RUN", "Loop started.");
}

void loop() {
  if (!canary::net::mqtt_connected()) {
    canary::log_line("MQTT", "Disconnected. Reconnecting...");
    canary::net::mqtt_reconnect_blocking();
    canary::net::publish_status_retained(TOPICS, "online");
    publish_state_now(canary::ms_now());
    delay(250);
    publish_state_now(canary::ms_now());
  }

  canary::net::mqtt_loop();

  const uint32_t now_ms = canary::ms_now();

  if ((now_ms - last_heartbeat_ms) > HEARTBEAT_MS) {
    last_heartbeat_ms = now_ms;
    publish_heartbeat_now(now_ms);
    publish_state_now(now_ms);
  }

  if ((now_ms - last_invoke_ms) < INVOKE_PERIOD_MS) {
    delay(5);
    return;
  }
  last_invoke_ms = now_ms;

  VisionSample vs{};
  if (!canary::vision::sample(vs)) return;

  EventMsg ev{};
  const bool emitted = fsm.tick(vs, now_ms, ev);

  if (emitted && ev.event_name) {
    set_last_event(ev.event_name);
    publish_event_json(ev.event_name, ev.reason, now_ms, vs);
    publish_state_now(now_ms);
  }
}
