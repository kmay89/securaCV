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
// PubSubClient's one buffer holds a whole outgoing packet: fixed header
// (5) + topic length (2) + topic + payload. It refuses anything larger,
// silently. The health payload is the largest periodic publish: ~970 B
// with realistic values, including the 64-hex public_key Home Assistant
// pins. With every field at its type's widest it reaches ~1170 B, a
// 1236 B packet on a topic at its 63-char cap. The old 1024 B buffer did
// not hold that worst case even before the key. 1280 does, and
// custom_components/securacv/tests/test_canary_health_trust.py holds
// every health key to it.
#ifndef MQTT_BUFFER_SIZE
  #define MQTT_BUFFER_SIZE        1280
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
// and whether a CA is present AND readable. `ca_set` is true only when the
// stored CA fits the transport's buffer (kCaBufBytes) — what load() will
// actually hand mbedTLS; a key that exists but does not fit (a third-party
// NVS writer; both flashers and the API cap at 3071) sets `ca_unreadable`
// instead, so the API can refuse a CA-verified mode over it the way the
// connect would, and say so (409 ca_unreadable: DELETE and upload again).
// Never the CA itself.
struct MqttTlsCurrent {
  uint8_t mode_byte;
  bool    fp_set;
  bool    ca_set;
  bool    ca_unreadable;
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

// What the credential row may carry over from what NVS already holds,
// exactly as mqtt_tls_fields::credential_carry decided (host-tested): a
// stored username / password the body omitted STANDS when `keep_*` is true
// (the same endpoint) and is REMOVED when false (a new host or port — a
// stored broker password never follows the link to an address it was not
// given for). A body that supplies a field always writes it.
struct MqttCredentialCarry {
  bool keep_user;
  bool keep_pass;
};

// One request, one NVS session, one reload. Writes the TLS keys FIRST (the
// pin, then the mode byte — mqtt_tls_fields::write_order, host-tested) and
// the credentials LAST, stops at the first failed write, closes the session
// and only then raises the main loop's reload. So the main task — which
// re-reads NVS and reconnects the moment it sees the flag, and shares this
// NVS handle — can never observe the new credentials next to the old (plain)
// mode byte, nor close the handle under a write still in flight. `tls` may
// be nullptr (credentials only); `carry` may be nullptr (keep everything the
// body omitted — the pre-sweep semantics, right only for the SAME endpoint;
// the API always passes the rule's answer). Returns false when NVS could not
// be opened or a write (a removal included) failed; a reload is raised
// either way, so the link follows what NVS actually holds.
bool mqtt_save_config(const MqttCredentials* creds, const MqttTlsWrite* tls,
                      const MqttCredentialCarry* carry);

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
  const char* mode;        // provisioned: "plain" / "tls-ca" / "tls-fingerprint" / "tls-insecure-lab", or "unknown" for a byte outside the table
  uint8_t     mode_byte;   // the byte as stored (not the table's fallback), so "unknown" comes with the value that was refused
  const char* transport;   // what the socket does: "plain" / "tls-ca" / "tls-fingerprint" / "tls-insecure" / "refused"
  bool        allowed;     // false = refused; nothing connects until reprovisioned
  bool        warn_insecure;
  const char* reason;      // the refusal, "" when allowed
};
void mqtt_transport_status(MqttTransportStatus* out);

// True once init() ran with a configured, enabled broker — the gate for
// callers deciding whether to build a payload at all. Unlike
// mqtt_connected() it stays true through an outage: events and tamper
// alerts are accepted (buffered) while the link is down.
bool mqtt_accepting();

// ── Publishing functions ────────────────────────────────────────────────
// Two publish classes. DISCRETE messages (events, tamper) return true when
// sent OR buffered: a broker outage queues them in a bounded FIFO
// (oldest-out on overflow) and the reconnected link replays them in order,
// so false means the message is truly not going anywhere (MQTT
// unconfigured, or the queue refused it) and the caller keeps its own
// re-arm. PERIODIC snapshots (status, health, sensing, chain, transport)
// are never queued — the next tick republishes fresher truth — and simply
// return false while disconnected (device continues without MQTT).

// Status: device state, GPS, chain sequence (QoS 0, every 30s)
bool mqtt_publish_status(const char* json_payload);

// Events: discrete event record (QoS 0, buffered across broker outages)
bool mqtt_publish_event(const char* json_payload);

// Events, live link only (the SD event log's backfill and its live path,
// csi_event_egress / common/csi/src/csi_event_backfill.h): true when the
// link took the body. Never buffers — false while the link is down, while
// the offline queue still holds records from an outage (those go first, in
// order: a queued tamper alert is never overtaken), or when the send
// failed. The caller's copy stays on the card and is retried.
bool mqtt_publish_event_live(const char* json_payload);

// Bumped by every reprovision that changes the broker a record would be
// delivered to (host, port or user changed, or the broker removed) — the
// same test that flushes the offline queue. A caller holding undelivered
// records of its own (the SD event log's backfill) drops them on a change:
// what waited for one broker is not the next one's to see.
uint32_t mqtt_destination_epoch();

// Health: system metrics (QoS 0, every 60s)
bool mqtt_publish_health(const char* json_payload);

// Chain: hash chain state (QoS 0, on demand)
bool mqtt_publish_chain(const char* json_payload);

// Tamper: tamper events (QoS 0, retained, immediate; buffered across
// broker outages so every alert in the window reaches HA, not just the
// newest)
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
