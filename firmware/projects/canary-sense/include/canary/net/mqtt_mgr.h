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

  // Witness trust surface (canary-wap wire schema):
  //   health — retained; carries public_key so HA TOFU-pins the device.
  //   chain  — retained; signed head+length, verified by HA's verify_chain.
  void publish_health_retained(const Topics& topics);
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

  // ── Identify (which-device-is-which) ──────────────────────────────────
  // HA's identify button / the companion app writes identify/set; the
  // callback latches it and main.cpp owns the 10 s blink window, echoing
  // on/off (non-retained) so dashboards can pulse the card in sync.
  bool take_pending_identify();  // true exactly once per inbound request
  bool publish_identify_echo(const Topics& topics, bool active);

  // ── Runtime radar reflexes (sense_config ↔ HA number entities) ────────
  // Retained snapshot on cfg/state; one command topic per knob (cfg/*/set).
  // The callback latches inbound values; main.cpp drains them, applies via
  // the clamping setters, reconfigures the FSMs, and republishes. Same
  // latch-and-drain contract as the OTA commands. -1 = nothing pending.
  bool publish_sense_cfg_retained(const Topics& topics);
  long take_pending_cfg_debounce();
  long take_pending_cfg_clear();
  long take_pending_cfg_stall();
  long take_pending_cfg_near();
  long take_pending_cfg_mid();
  long take_pending_cfg_vlock();
  long take_pending_cfg_vlost();
  long take_pending_cfg_bmin();
  long take_pending_cfg_bmax();
  long take_pending_cfg_hmin();
  long take_pending_cfg_hmax();

} // namespace
