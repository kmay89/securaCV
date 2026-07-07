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

  // Household ack-sync (trailblazer spec §2): announce a local long-press
  // ack on the shared retained topic. Caller supplies epoch seconds (only
  // call when SNTP-synced — no guessed clocks on the wire).
  void publish_fleet_ack(uint32_t epoch_s);

  // ── Broker endpoint (flock-discovery rebind) ──────────────────────────
  // The broker address starts from runtime config but is rebindable at
  // runtime: mDNS flock discovery adopts a referral when the compiled/NVS
  // endpoint is a placeholder or has gone dark (broker moved DHCP lease).
  // Adopting persists to the same NVS keys runtime_config reads, so the
  // discovered endpoint survives reboots. Drops any current connection;
  // the caller's supervision loop reconnects on its normal backoff.
  void mqtt_set_broker(const char* host, uint16_t port);
  const char* mqtt_broker_host();
  uint16_t mqtt_broker_port();
  bool mqtt_broker_is_placeholder();   // unconfigured (CI stub / blank)

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
