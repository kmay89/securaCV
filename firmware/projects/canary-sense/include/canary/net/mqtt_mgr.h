#pragma once
#include <Arduino.h>
#include "canary/types.h"
#include "canary/topics.h"

namespace canary::net {

  void mqtt_init(const Topics& topics);
  void mqtt_loop();
  bool mqtt_connected();

  // ONE bounded connect attempt (TCP connect + MQTT CONNECT). On success it
  // publishes the retained online status, HA discovery, and reconciles the
  // update-entity subscriptions/state, then returns true. On failure it
  // returns false immediately — the caller owns the retry schedule, so a
  // broker outage can never pin the main loop and stop the radar witness
  // from sensing (unlike a spin-until-connected loop).
  bool mqtt_connect_attempt();

  // Publishing
  void publish_status_retained(const Topics& topics, const char* status);   // online/offline
  void publish_heartbeat(const Topics& topics, const SenseSnapshot& s);     // online + radar health
  void publish_state_retained(const Topics& topics, const SenseSnapshot& s);
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

} // namespace
