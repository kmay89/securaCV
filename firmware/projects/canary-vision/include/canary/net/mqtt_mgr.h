#pragma once
#include <Arduino.h>
#include "canary/types.h"
#include "canary/topics.h"

namespace canary::net {

  void mqtt_init(const Topics& topics);
  void mqtt_loop();
  bool mqtt_connected();

  // ONE bounded connect attempt (TCP connect + MQTT CONNECT). On success it
  // republishes retained surfaces and re-subscribes command topics; on
  // failure it returns so the caller's backoff owns the retry cadence.
  // Never loops: a dead broker must not pin the witness (canary-sense parity).
  bool mqtt_connect_attempt();

  // Publishing
  void publish_status_retained(const Topics& topics, const char* status);   // online/offline
  void publish_heartbeat(const Topics& topics, const StateSnapshot& s);     // online + booleans
  void publish_state_retained(const Topics& topics, const StateSnapshot& s);
  void publish_event(const Topics& topics, const char* json_payload);       // non-retained

  // Witness surfaces (same envelopes as canary-sense): health carries
  // public_key so HA TOFU-pins the device on first sight; chain is the
  // retained, Ed25519-signed head HA's verify_chain checks.
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

  // ── Aim assist (bench/aiming) ──────────────────────────────────────────
  // Boxes-only live channel for the Lovelace aim card. publish_aim is
  // QUIET (no per-publish serial log — it runs at ~5 Hz) and non-retained;
  // the switch state is retained so HA renders the toggle correctly after
  // a broker restart. The inbound switch command uses the same
  // latch-and-drain pattern as the other HA commands.
  bool publish_aim(const Topics& topics, const char* json_payload);
  bool publish_aim_state_retained(const Topics& topics, bool enabled);
  int take_pending_aim();       // -1 none; 0/1 = switch set off/on

  // ── Identify (which-device-is-which) ──────────────────────────────────
  // HA's identify button / the companion app writes identify/set; the
  // callback latches it and main.cpp owns the 10 s blink window, echoing
  // on/off (non-retained) so dashboards can pulse the card in sync.
  bool take_pending_identify();  // true exactly once per inbound request
  bool publish_identify_echo(const Topics& topics, bool active);

  // ── Runtime detection settings ─────────────────────────────────────────
  // Same latch-and-drain pattern as the update commands: the callback only
  // parses and latches inbound numbers; main.cpp drains them, applies via
  // canary::cfg::detect_set_*(), and republishes the retained cfg state.
  bool publish_detect_cfg_retained(const Topics& topics);
  long take_pending_cfg_target();  // -1 none; else 0..255
  long take_pending_cfg_score();   // -1 none; else 0..100
  long take_pending_cfg_lost();    // -1 none; else ms
  long take_pending_cfg_dwell();   // -1 none; else ms
  long take_pending_cfg_profile(); // -1 none; else watch profile id

} // namespace
