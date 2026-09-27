#pragma once
#include <Arduino.h>
#include "canary/types.h"
#include "canary/topics.h"

// canary-sense's MQTT manager with the sentinel's snapshot in place of the
// radar's and the radar reflex dials / identify button left out (Phase 1a
// wires neither). The transport, the trust surface (status / health / chain)
// and the OTA command glue are canary-sense's function for function —
// firmware/scripts/check_sentinel_net_sync.sh pins each one.

namespace canary::net {

  void mqtt_init(const Topics& topics);
  void mqtt_loop();
  bool mqtt_connected();

  // ONE bounded connect attempt (TCP connect + MQTT CONNECT). On success it
  // publishes the retained online status, HA discovery, and reconciles the
  // update-entity subscriptions/state, then returns true. On failure it
  // returns false immediately — the caller owns the retry schedule, so a
  // broker outage can never pin the main loop and stop the fusion engine
  // from sensing (unlike a spin-until-connected loop).
  bool mqtt_connect_attempt();

  // Publishing
  void publish_status_retained(const Topics& topics, const char* status);      // online/offline
  void publish_heartbeat(const Topics& topics, const SentinelSnapshot& s);     // online + fusion health
  void publish_state_retained(const Topics& topics, const SentinelSnapshot& s);
  void publish_event(const Topics& topics, const char* json_payload);          // non-retained

  // Witness trust surface (canary-wap wire schema):
  //   health — retained; carries public_key so HA TOFU-pins the device.
  //   chain  — retained; signed head+length, verified by HA's verify_chain.
  // Radar-link health for the retained health payload's `radar` object —
  // the wire contract Home Assistant's radar-link diagnostic sensor reads
  // (custom_components/securacv/sensor.py, SecuraCVCanaryRadarLinkSensor:
  // link_ok / last_frame_age_ms / frame_errors). A product with no radar,
  // or one whose UART has not been sampled yet, passes nullptr and the
  // object is left out, which HA reads as "unknown" rather than "down".
  struct RadarLinkHealth {
    bool     link_ok           = false;  // a presence frame arrived inside the stall window
    uint32_t last_frame_age_ms = 0;      // ms since the last presence frame
    uint32_t frame_errors      = 0;      // checksum/oversize drops (monotonic)
  };

  void publish_health_retained(const Topics& topics,
                               const RadarLinkHealth* radar = nullptr);
  void publish_chain_retained(const Topics& topics);

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
