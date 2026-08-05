/*
 * SecuraCV Canary — MQTT Publisher with HA Discovery
 *
 * Publishes witness events, health, chain state, and tamper alerts
 * to a local MQTT broker. Sends HA MQTT Discovery messages on connect.
 *
 * MQTT is an OPTIONAL convenience transport — the device must function
 * fully without it. No raw pixel data is ever sent over MQTT.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_MQTT_H
#define SECURACV_MQTT_H

#include <Arduino.h>
#include "canary_config.h"

#if FEATURE_HA_MQTT

// ════════════════════════════════════════════════════════════════════════════
// MQTT CONFIGURATION
// ════════════════════════════════════════════════════════════════════════════

#ifndef MQTT_KEEPALIVE_SEC
  #define MQTT_KEEPALIVE_SEC      60
#endif
#ifndef MQTT_BUFFER_SIZE
  #define MQTT_BUFFER_SIZE        1024
#endif
#ifndef MQTT_RECONNECT_MIN_MS
  #define MQTT_RECONNECT_MIN_MS   1000
#endif
#ifndef MQTT_RECONNECT_MAX_MS
  #define MQTT_RECONNECT_MAX_MS   30000
#endif
#ifndef MQTT_STATUS_INTERVAL_MS
  #define MQTT_STATUS_INTERVAL_MS 30000
#endif
#ifndef MQTT_HEALTH_INTERVAL_MS
  #define MQTT_HEALTH_INTERVAL_MS 60000
#endif

// ════════════════════════════════════════════════════════════════════════════
// MQTT CREDENTIALS (stored in NVS)
// ════════════════════════════════════════════════════════════════════════════

struct MqttCredentials {
  char host[64];
  uint16_t port;
  char username[32];
  char password[64];
  bool enabled;
  bool configured;
};

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════════════════

// Initialize MQTT client. Call once in setup().
// Loads credentials from NVS if available.
bool mqtt_init(const char* device_id, const char* firmware_version);

// Call in loop() — handles reconnect, keepalive, incoming messages
void mqtt_loop();

// Connection state
bool mqtt_connected();
void mqtt_disconnect();

// Credential management (stored in NVS)
bool mqtt_load_credentials(MqttCredentials* creds);
bool mqtt_save_credentials(const MqttCredentials* creds);
bool mqtt_clear_credentials();

// ── Publishing functions ────────────────────────────────────────────────
// All publish functions return true if message was sent (or buffered).
// They are no-ops if MQTT is not connected (device continues without MQTT).

// Status: device state, GPS, chain sequence (QoS 0, every 30s)
bool mqtt_publish_status(const char* json_payload);

// Events: witness record created (QoS 0, debounced to max 1/sec)
bool mqtt_publish_event(const char* json_payload);

// Health: system metrics (QoS 0, every 60s)
bool mqtt_publish_health(const char* json_payload);

// Chain: hash chain state (QoS 0, on demand)
bool mqtt_publish_chain(const char* json_payload);

// Tamper: tamper events (QoS 0, retained, immediate)
bool mqtt_publish_tamper(const char* json_payload, bool retained = true);

// Transport: transport status (QoS 0, on change)
bool mqtt_publish_transport(const char* json_payload);

// Sensing: full snapshot of the Phase-1..5 sensing state (CSI motion /
// breathing scores, acoustic last-event, touch last-event, IR last-
// activity, temp drift, lowpower wake reason). One retained JSON blob
// per device — HA value_template extracts each entity's field. Called
// every MQTT_STATUS_INTERVAL_MS from the main loop.
bool mqtt_publish_sensing(const char* json_payload);

// ── HA MQTT Discovery ───────────────────────────────────────────────────
// Send Home Assistant MQTT Discovery config messages.
// Called automatically on first connect and reconnect.
#if FEATURE_HA_DISCOVERY
bool mqtt_send_ha_discovery(const char* device_id, const char* firmware_version);
#endif

// ── Inbound commands ────────────────────────────────────────────────────
// Home Assistant publishes to a per-device command topic to flip the
// matching `switch` entity. The MQTT lib subscribes and parses; the
// application registers a callback to act on the command without the
// MQTT lib having to depend on the audio HAL. Pass nullptr to clear.
typedef void (*mqtt_mic_mute_cmd_cb_t)(bool muted);
void mqtt_set_mic_mute_cmd_callback(mqtt_mic_mute_cmd_cb_t cb);

// HA's `button` entity for the audio self-test publishes "start" to a
// dedicated topic. Same wiring as mic mute: the MQTT lib parses and
// fires this callback, which main.cpp wires to audio_selftest_start().
typedef void (*mqtt_audio_test_cmd_cb_t)(void);
void mqtt_set_audio_test_cmd_callback(mqtt_audio_test_cmd_cb_t cb);

// Publish the current mute state to the dedicated retained state topic
// (separate from the main /sensing topic so HA's `switch` entity reflects
// changes instantly rather than waiting for the next /sensing tick).
bool mqtt_publish_mic_mute_state(bool muted);

// ── Firmware update entity (pull-OTA) ───────────────────────────────────
// HA's `update` entity shows installed vs latest version with release
// notes and an Install button; the `switch` entity is the per-device
// auto-update opt-in. Same wiring pattern as the mic mute switch: the
// MQTT lib parses inbound commands and fires app-registered callbacks.
#if FEATURE_OTA_PULL

// HA pressed Install on the update entity.
typedef void (*mqtt_ota_install_cmd_cb_t)(void);
void mqtt_set_ota_install_cmd_callback(mqtt_ota_install_cmd_cb_t cb);

// HA toggled the auto-update switch.
typedef void (*mqtt_ota_auto_cmd_cb_t)(bool enabled);
void mqtt_set_ota_auto_cmd_callback(mqtt_ota_auto_cmd_cb_t cb);

// Publish the update entity's JSON state (installed_version,
// latest_version, in_progress, update_percentage, release_summary,
// release_url). Retained + republished on reconnect.
bool mqtt_publish_update_state(const char* json_payload);

// Publish the auto-update switch state. Retained + republished on reconnect.
bool mqtt_publish_update_auto_state(bool enabled);

#endif // FEATURE_OTA_PULL

#endif // FEATURE_HA_MQTT

#endif // SECURACV_MQTT_H
