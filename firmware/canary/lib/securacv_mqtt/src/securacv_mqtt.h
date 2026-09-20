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

// Credential management (stored in NVS). A save or clear takes effect on
// the main loop's next pass — the live link is dropped and re-made with the
// new settings, no reboot needed. mqtt_save_credentials is the
// credentials-only form of mqtt_save_config (below); a request that also
// carries TLS fields MUST go through mqtt_save_config so its writes land in
// one NVS session with one reload.
bool mqtt_load_credentials(MqttCredentials* creds);
bool mqtt_save_credentials(const MqttCredentials* creds);
bool mqtt_clear_credentials();

// ── Broker transport (TLS) ──────────────────────────────────────────────
// The socket PubSubClient rides is decided fleet-wide by
// firmware/common/network/mqtt_transport_logic.h from three NVS keys that
// sit next to the credentials in the same "securacv" namespace:
//   mqtt_tls  u8      0 plain (default) / 1 CA-verified / 2 SHA-256 pin / 3 lab
//   mqtt_ca   string  the CA certificate, PEM
//   mqtt_fp   string  the broker certificate's SHA-256 pin
// Plain unless provisioned. An incomplete TLS setup (mode 1 with no CA,
// mode 2 with no pin, a malformed value, an unknown byte) is REFUSED at
// connect with the reason on the serial log, in the health log and on
// /api/mqtt/status — never downgraded to plain or to an unverified socket.
// The functions below are this product's writer and reporter for those
// keys; validation of what to write lives in mqtt_tls_fields.h (pure,
// host-tested) so the API refuses exactly what the connect would.

// What NVS holds now, for the API's save-time judgment and for status:
// the mode byte (0 when absent), the stored pin as-is ("" when absent;
// "?" for a set-but-unreadable key, as the transport itself reports it),
// and whether a CA is present. Never the CA itself.
struct MqttTlsCurrent {
  uint8_t mode_byte;
  bool    fp_set;
  bool    ca_set;
  char    fp[128];
};
bool mqtt_tls_read_current(MqttTlsCurrent* out);

// The TLS half of one POST /api/mqtt/config, exactly as a validated plan
// says (mqtt_tls_fields::plan): `fp_canonical` is the 95-char "AA:BB:..."
// form, `clear_fp` forgets the stored pin.
struct MqttTlsWrite {
  bool        set_mode;
  uint8_t     mode;
  bool        set_fp;
  const char* fp_canonical;
  bool        clear_fp;
};

// One request, one NVS session, one reload. Writes the TLS keys FIRST (the
// pin, then the mode byte — mqtt_tls_fields::write_order, host-tested) and
// the credentials LAST, stops at the first failed write, closes the session
// and only then raises the main loop's reload. So the main task — which
// re-reads NVS and reconnects the moment it sees the flag, and shares this
// NVS handle — can never observe the new credentials next to the old (plain)
// mode byte, nor close the handle under a write still in flight. `tls` may
// be nullptr (credentials only). Returns false when NVS could not be opened
// or a write failed; a reload is raised either way, so the link follows what
// NVS actually holds.
bool mqtt_save_config(const MqttCredentials* creds, const MqttTlsWrite* tls);

// Store / forget the broker CA. `pem` has already passed
// mqtt_tls_fields::check_ca and ends in '\n' (the caller adds it, as the
// flashers do). Reloads the transport on the main loop's next pass.
bool mqtt_tls_save_ca(const char* pem);
bool mqtt_tls_clear_ca();

// The transport as decided at init or the last reprovision. Every string is
// a constant from the shared header — safe for any channel; never the CA,
// the pin or a credential.
struct MqttTransportStatus {
  bool        loaded;      // false only before mqtt_init() has read NVS (the HTTP server starts first)
  const char* mode;        // provisioned: "plain" / "tls-ca" / "tls-fingerprint" / "tls-insecure-lab"
  uint8_t     mode_byte;
  const char* transport;   // what the socket does: "plain" / "tls-ca" / "tls-fingerprint" / "tls-insecure" / "refused"
  bool        allowed;     // false = refused; nothing connects until reprovisioned
  bool        warn_insecure;
  const char* reason;      // the refusal, "" when allowed
};
void mqtt_transport_status(MqttTransportStatus* out);

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
