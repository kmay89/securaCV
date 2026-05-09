/**
 * @file csi_mqtt.h
 * @brief Optional MQTT bridge: publishes the canary-wap firmware's CSI
 *        sensing events, health, counts, and chain state to a
 *        user-supplied MQTT broker so the Home Assistant integration in
 *        custom_components/securacv/ (already shipped) sees live data.
 *
 * Backed by ESP-IDF's native esp_mqtt client (mqtt_client.h). Bundled
 * with arduino-esp32 — no lib_deps addition. ESP-IDF runs the MQTT
 * task internally and handles auto-reconnect, so callers publish from
 * any context without thinking about threading.
 *
 * Topic schema (locked against custom_components/securacv/const.py +
 * docs/homeassistant_setup.md):
 *
 *   {prefix}/{device_id}/status   — JSON, retained, LWT "offline"
 *   {prefix}/{device_id}/health   — JSON, every 60 s
 *   {prefix}/{device_id}/events   — JSON, on each csi_event commit
 *   {prefix}/{device_id}/chain    — JSON, on hash-chain advance
 *   {prefix}/{device_id}/counts   — JSON, every 30 s
 *
 * Privacy contract: every publish wraps
 * csi_integration::add_outbound_bytes(payload_len) so the dashboard's
 * privacy-budget pill correctly reflects bytes leaving the device. A
 * follow-up commit flips handle_privacy_budget's wired:false to true
 * once this module ships.
 *
 * Boundaries: csi_mqtt is the MQTT plumbing. Caller owns the cadence
 * and the live metrics — canary_wap.ino's main loop builds the
 * health/status/counts JSON on its own gates and calls publish_*().
 * That keeps the module narrow and avoids a callback table that would
 * recreate the same coupling.
 */

#ifndef SECURACV_CSI_MQTT_H
#define SECURACV_CSI_MQTT_H

#include "esp_http_server.h"
#include <csi_event.h>
#include <stddef.h>
#include <stdint.h>

namespace csi_mqtt {

/* NVS keys (≤15 chars, "csi" namespace shared with the rest of the
 * dashboard's settings store). */
constexpr const char* NVS_KEY_ENABLED = "mqtt.en";
constexpr const char* NVS_KEY_HOST    = "mqtt.host";
constexpr const char* NVS_KEY_PORT    = "mqtt.port";
constexpr const char* NVS_KEY_USER    = "mqtt.user";
constexpr const char* NVS_KEY_PASS    = "mqtt.pass";
constexpr const char* NVS_KEY_PREFIX  = "mqtt.prefix";
constexpr const char* NVS_KEY_TLS     = "mqtt.tls";

constexpr size_t MAX_HOST_LEN   = 128;
constexpr size_t MAX_USER_LEN   = 64;
constexpr size_t MAX_PASS_LEN   = 128;
constexpr size_t MAX_PREFIX_LEN = 32;

/* Configuration mirror of the NVS row. password is loaded but
 * intentionally never returned by handle_config_get. */
struct Config {
  bool     enabled;
  char     host[MAX_HOST_LEN + 1];
  uint16_t port;
  char     user[MAX_USER_LEN + 1];
  char     pass[MAX_PASS_LEN + 1];
  char     prefix[MAX_PREFIX_LEN + 1];
  bool     tls;
};

bool config_load(Config* out);
bool config_save(const Config& cfg);

/**
 * Cold-boot init. Reads NVS, opens the esp_mqtt client if enabled, and
 * arms the LWT. Idempotent — a second call (e.g. after a config POST)
 * tears down the existing client and re-opens with the new credentials.
 * Safe to call before WiFi STA is up; the client stays disconnected
 * until TCP can establish.
 *
 *   device_id        the canary's device_id (g_device.device_id) — copied
 *   firmware_version the FIRMWARE_VERSION literal — copied
 *   public_key_hex   64-char hex-encoded device pubkey, or nullptr
 *
 * Returns true if the client started OK (or is intentionally disabled).
 */
bool init(const char* device_id,
          const char* firmware_version,
          const char* public_key_hex);

/**
 * Per-tick pump. Currently a no-op (esp_mqtt manages its own task and
 * supervises reconnection internally), but reserved as the place to
 * land any future main-loop synchronization (e.g. backfill events from
 * the SD ring once SD persistence lands).
 */
void loop();

/** True iff the underlying MQTT client is connected to the broker. */
bool connected();

/**
 * Push one CSI event to {prefix}/{device_id}/events. Called from the
 * csi_event_on_committed() strong override after the chokepoint has
 * cleared the event. No-op when MQTT is disabled or disconnected.
 */
void publish_event(const char*               module_id,
                   const char*               type_name,
                   csi_event_category_t      category,
                   csi_privacy_class_t       privacy,
                   const csi_event_values_t* values);

/**
 * Push the witness-chain head to {prefix}/{device_id}/chain.
 * latest_hash_32 is the binary 32-byte chain hash; we hex-encode for
 * the wire so HA can render / compare it without bytes vs string
 * confusion.
 */
void publish_chain(uint32_t length, const uint8_t* latest_hash_32);

/**
 * Push the canonical health snapshot to {prefix}/{device_id}/health.
 * Schema (matches custom_components/securacv/sensor.py:328-345):
 *   battery, memory_free, uptime, firmware_version, public_key
 * Battery is reserved for future hardware revisions; the canary-wap
 * reference board is mains-powered, so we publish 100. The HA sensor
 * derives "healthy/warning/critical" from battery + memory_free.
 */
void publish_health(uint32_t free_heap_bytes, uint32_t uptime_sec);

/**
 * Push the witness count to {prefix}/{device_id}/counts. Used by the
 * canary fleet view as the "how many records did this device write?"
 * indicator.
 */
void publish_counts(uint32_t total);

/**
 * Push the device's running status to {prefix}/{device_id}/status —
 * retained, so a freshly-connecting HA picks up the latest snapshot
 * even if it missed the moment of publish. Includes wifi/csi state
 * so HA can correlate "device online" with "CSI sensing actually
 * working" without a second fetch.
 */
void publish_status(bool csi_running,
                    bool wifi_connected,
                    int  rssi_dbm);

/**
 * Register the Bearer-token accessor used by the /api/mqtt/* HTTP
 * handlers. csi_mqtt has no direct access to g_device.api_token_str,
 * so csi_integration::init wires this up at boot — same string the
 * rest of the CSI surface authenticates against, no duplicate
 * secret to manage.
 */
void set_api_token_provider(const char* (*fn)());

/* HTTP handlers — registered by csi_integration::init alongside the
 * other CSI routes. Auth-gated by either the cv_session cookie or a
 * Bearer header carrying the api_token (the same dual-mode
 * CSI_AUTH_OR_RETURN uses). The /test handler forces a reconnect with
 * current NVS settings and reports the broker's reachability
 * synchronously, so the dashboard's "Test connection" button gives a
 * real signal rather than a spinner. */
esp_err_t handle_config_get(httpd_req_t* req);
esp_err_t handle_config_post(httpd_req_t* req);
esp_err_t handle_test(httpd_req_t* req);
esp_err_t handle_ui(httpd_req_t* req);

}  /* namespace csi_mqtt */

#endif  /* SECURACV_CSI_MQTT_H */
