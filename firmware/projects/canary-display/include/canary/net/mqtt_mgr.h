#pragma once
#include <Arduino.h>
#include "canary/topics.h"

namespace canary::net {

  void mqtt_init(const Topics& topics);
  void mqtt_loop();
  bool mqtt_connected();

  // ONE bounded connect attempt (TCP connect + MQTT CONNECT). On success it
  // publishes the retained online status, subscribes the fleet wildcards +
  // update-entity commands, and returns true. On failure it returns false
  // immediately — the caller owns the retry schedule, so a broker outage
  // can never pin the main loop and freeze the display (which keeps
  // rendering its last-known fleet state, clearly marked stale).
  bool mqtt_connect_attempt();

  // Publishing (the display's own trust-neutral liveness surface)
  void publish_status_retained(const Topics& topics, const char* status);   // online/offline
  void publish_health_retained(const Topics& topics);

  // ── Firmware update entity (signed pull-OTA) ──────────────────────────
  // Retained state for HA's update entity + the auto-update switch; cached
  // and republished on every reconnect. Inbound commands are latched by the
  // MQTT callback and drained from the main loop by ota_mgr. Same wire
  // schema as every other Canary variant.
  bool publish_update_state_retained(const Topics& topics, const char* json_payload);
  bool publish_update_auto_retained(const Topics& topics, bool enabled);
  bool take_pending_install();   // true exactly once after HA pressed Install
  int take_pending_auto();       // -1 none; 0/1 = switch set off/on

} // namespace canary::net
