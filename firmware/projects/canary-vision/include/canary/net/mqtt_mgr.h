#pragma once
#include <Arduino.h>
#include "canary/types.h"
#include "canary/topics.h"

namespace canary::net {

  void mqtt_init(const Topics& topics);
  void mqtt_loop();
  bool mqtt_connected();
  void mqtt_reconnect_blocking();

  // Publishing
  void publish_status_retained(const Topics& topics, const char* status);   // online/offline
  void publish_heartbeat(const Topics& topics, const StateSnapshot& s);     // online + booleans
  void publish_state_retained(const Topics& topics, const StateSnapshot& s);
  void publish_event(const Topics& topics, const char* json_payload);       // non-retained

  // HA discovery (retained)
  void ha_discovery_publish_once(const Topics& topics);

  // ── Firmware update entity (signed pull-OTA) ──────────────────────────
  // Retained state for HA's update entity + the auto-update switch; cached
  // and republished on every reconnect. Inbound commands are latched by the
  // MQTT callback and drained from the main loop by ota_mgr.
  bool publish_update_state_retained(const Topics& topics, const char* json_payload);
  bool publish_update_auto_retained(const Topics& topics, bool enabled);
  bool take_pending_install();   // true exactly once after HA pressed Install
  int take_pending_auto();       // -1 none; 0/1 = switch set off/on

  // ── Runtime detection settings ─────────────────────────────────────────
  // Same latch-and-drain pattern as the update commands: the callback only
  // parses and latches inbound numbers; main.cpp drains them, applies via
  // canary::cfg::detect_set_*(), and republishes the retained cfg state.
  bool publish_detect_cfg_retained(const Topics& topics);
  long take_pending_cfg_target();  // -1 none; else 0..255
  long take_pending_cfg_score();   // -1 none; else 0..100
  long take_pending_cfg_lost();    // -1 none; else ms
  long take_pending_cfg_dwell();   // -1 none; else ms

} // namespace
