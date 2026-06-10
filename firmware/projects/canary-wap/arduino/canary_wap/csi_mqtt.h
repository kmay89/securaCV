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
 *   {prefix}/{device_id}/status       — JSON, retained, LWT "offline"
 *   {prefix}/{device_id}/health       — JSON, every 60 s
 *   {prefix}/{device_id}/events       — JSON, on each csi_event commit
 *   {prefix}/{device_id}/chain        — JSON, on hash-chain advance
 *   {prefix}/{device_id}/counts       — JSON, on each new witness record
 *   {prefix}/{device_id}/mesh         — JSON, retained, every 30 s (mesh builds)
 *   {prefix}/{device_id}/chirp        — JSON, retained, with mesh snapshot
 *   {prefix}/{device_id}/beacon       — JSON, retained, every 30 s (beacon builds)
 *   {prefix}/{device_id}/update/state — JSON, retained (signed pull-OTA)
 *   {prefix}/{device_id}/update/auto  — "ON"/"OFF", retained; cmd on …/auto/cmd
 *   {prefix}/{device_id}/sensing      — JSON, retained: acoustic_event +
 *                                       detection counters + mic_muted
 *                                       (FEATURE_ACOUSTIC_EVENTS builds)
 *   {prefix}/{device_id}/mic/state    — "muted"/"live", retained; commands
 *                                       arrive on …/mic/cmd (mute/unmute/ON/OFF)
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

#include "build_config.h"
#include "esp_http_server.h"
#include <csi_event.h>
#include <stddef.h>
#include <stdint.h>

namespace csi_mqtt {

/* NVS keys (≤15 chars, "csi" namespace shared with the rest of the
 * dashboard's settings store). */
constexpr const char* NVS_KEY_ENABLED   = "mqtt.en";
constexpr const char* NVS_KEY_HOST      = "mqtt.host";
constexpr const char* NVS_KEY_PORT      = "mqtt.port";
constexpr const char* NVS_KEY_USER      = "mqtt.user";
constexpr const char* NVS_KEY_PASS      = "mqtt.pass";
constexpr const char* NVS_KEY_PREFIX    = "mqtt.prefix";
constexpr const char* NVS_KEY_TLS       = "mqtt.tls";
constexpr const char* NVS_KEY_DISCOVERY = "mqtt.disc";

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
  /* When true, publish HA MQTT auto-discovery payloads on
   * homeassistant/{component}/canary_<device_id>/{object_id}/config
   * the moment we connect to the broker. HA picks them up
   * automatically and creates entities under one device — users
   * with only the HA MQTT integration installed see the canary
   * without any manual sensor wiring. Defaults to true; non-HA
   * MQTT consumers can flip it off to suppress the discovery
   * payloads. Retained on the broker so a freshly-subscribing HA
   * sees them whenever it comes online. */
  bool     discovery;
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
 *
 * event_id is the same id csi_event allocated; tracked internally as
 * the high-water-mark of "events HA has seen" so a subsequent MQTT
 * reconnect knows where to start the backfill replay.
 */
void publish_event(uint32_t                  event_id,
                   const char*               module_id,
                   const char*               type_name,
                   csi_event_category_t      category,
                   csi_privacy_class_t       privacy,
                   const csi_event_values_t* values);

/**
 * Replay an on-disk event during MQTT-reconnect backfill. Same wire
 * format as publish_event but anchors the timestamp at the original
 * first_seen_ms (so HA's history places the event at the right
 * moment instead of "now") and uses the persisted bundled_count.
 * Called by the main-loop drain triggered when the MQTT bridge
 * reconnects after an outage.
 *
 * Returns true on successful enqueue so the backfill iterator can
 * stop mid-replay if a publish fails — letting later successes
 * advance the watermark past a failed record would permanently
 * skip it on subsequent reconnects.
 */
bool publish_event_record(const csi_event_record_t* rec);

/**
 * Publish HA MQTT auto-discovery payloads for the canary's full entity
 * set on `homeassistant/{component}/canary_<device_id>/{object_id}/config`.
 * Called from MQTT_EVENT_CONNECTED after the online-status publish so
 * HA sees the device as online before referencing it as an entity's
 * availability topic. Retained, so a freshly-subscribing HA picks
 * them up regardless of when it comes online. Internally also calls
 * publish_triggers so the device-automation set lands in the same
 * connect cycle. No-op when the Config.discovery toggle is false.
 */
void publish_discovery();

/**
 * Publish HA Device Trigger discovery payloads on
 * `homeassistant/device_automation/canary_<device_id>/{trigger_id}/config`.
 * Called from publish_discovery. Each trigger surfaces a single
 * type/subtype variant in HA's automation builder so users can
 * drag-and-drop "presence became active" instead of authoring a
 * value_template by hand.
 */
void publish_triggers();

/**
 * Publish empty retained payloads to every entity AND trigger config
 * topic so HA evicts our entries from its registry. Called from
 * MQTT_EVENT_CONNECTED when Config.discovery is FALSE — covers the
 * "user toggled discovery off post-install" case where the previously
 * retained payloads would otherwise linger on the broker forever and
 * leave HA showing the entities as "unavailable".
 *
 * Idempotent: empty publishes to topics with no retained message are
 * silently ignored by the broker, so we don't track "last published"
 * state across reboots.
 */
void remove_discovery();

/**
 * Push the witness-chain head to {prefix}/{device_id}/chain.
 * latest_hash_32 is the binary 32-byte chain hash; we hex-encode for
 * the wire so HA can render / compare it without bytes vs string
 * confusion.
 */
void publish_chain(uint32_t length, const uint8_t* latest_hash_32);

/**
 * Battery snapshot for the health publish. Filled from power_monitor
 * by the .ino. Pass nullptr when no battery is present (or the build
 * has no power monitor): the publish then carries the mains semantics
 * HA expects (battery=100, battery_present=false).
 */
struct MqttBatteryInfo {
  uint8_t     soc_pct;       // state of charge, 0-100
  uint8_t     health_pct;    // cycle-fade capacity estimate, 60-100
  uint16_t    battery_mv;    // cell voltage in millivolts
  const char* charge_state;  // power_monitor::charge_state_name()
};

/**
 * Push the canonical health snapshot to {prefix}/{device_id}/health.
 * Schema (matches custom_components/securacv/sensor.py health handler):
 *   battery, battery_present, memory_free, uptime, firmware_version,
 *   public_key — plus charge_state, battery_health_pct, battery_mv
 *   when a battery is present.
 * The HA sensor derives "healthy/warning/critical" from battery +
 * memory_free; charging devices and mains-powered devices (battery
 * nullptr → battery=100) never trip the battery thresholds.
 */
void publish_health(uint32_t free_heap_bytes, uint32_t uptime_sec,
                    const MqttBatteryInfo* battery = nullptr);

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
 * Push the mesh-coexistence snapshot to {prefix}/{device_id}/mesh —
 * retained, periodic (~30 s). Surfaces the airtime governor and the
 * mesh-channel-policy decision so installers can see, in Home
 * Assistant, that the multi-Canary mesh is following the home WiFi
 * channel and staying under its airtime cap.
 *
 * Caller fetches the inputs from airtime_governor::snapshot() and
 * mesh_channel_policy::current() so this module stays free of mesh
 * deps (csi_mqtt is the publish path, not the data source). All
 * fields are denormalized into the JSON so HA can build templates
 * without state across topics.
 *
 *   airtime_pct_x100  utilization × 100 (e.g. 215 = 2.15%)
 *   channel           current 2.4 GHz channel (1-13)
 *   locked_to_sta     true when the mesh is following an associated STA
 *   locked_to_ap      true when STA is off but AP is up
 *   fallback          true when neither STA nor AP is up (radio free)
 *   routine_allowed   lifetime count of routine sends permitted
 *   routine_denied    lifetime count of routine sends gated by the cap
 *   urgent_sends      lifetime count of tamper/power/OFFLINE_IMMINENT sends
 */
void publish_mesh(uint16_t airtime_pct_x100,
                  uint8_t  channel,
                  bool     locked_to_sta,
                  bool     locked_to_ap,
                  bool     fallback,
                  uint32_t routine_allowed,
                  uint32_t routine_denied,
                  uint32_t urgent_sends);

/**
 * Publish the Chirp channel's NFPA-72-style state as a string enum.
 * Surfaces under topic securacv/<prefix>/<device>/chirp with
 * `state` field consumed by sensor.canary_<id>_chirp_state.
 *
 * state_name: one of "Normal" | "Trouble" | "Alarm" | "Supervisory"
 *             (or the Chirp ChirpState string from chirp_channel::state_name).
 */
void publish_chirp_state(const char* state_name);

/**
 * Publish the Beacon channel's NFPA-72 state surface, audit-log size,
 * active alarm template, and beacon-only airtime utilization.
 *
 * state_name              one of "Normal" | "Trouble" | "Alarm" | "Supervisory"
 * beacon_airtime_pct_x100 rolling-window airtime utilization × 100
 * active_template         human-readable template text or "" when no active alarm
 * beacon_sends            lifetime count of Beacon-class TX
 * beacon_set_size         number of paired neighbor pubkeys
 * trouble_mask            bitfield of BeaconTroubleReason values
 */
void publish_beacon_state(const char* state_name,
                          uint16_t beacon_airtime_pct_x100,
                          const char* active_template,
                          uint32_t beacon_sends,
                          uint8_t beacon_set_size,
                          uint16_t trouble_mask);

#if FEATURE_ACOUSTIC_EVENTS
/**
 * ── Acoustic events + microphone mute (PDM mic) ──────────────────────
 *
 * publish_sensing pushes the acoustic snapshot to
 * {prefix}/{device_id}/sensing — retained, caller-built JSON carrying
 * `acoustic_event` (string enum: smoke_alarm_t3 | co_alarm_t4 | knock |
 * doorbell | glass_break | none), `mic_muted`, and the detection
 * counters. The HA integration's smoke/CO/knock/doorbell/glass binary
 * sensors template against `acoustic_event`, same contract as the
 * canary core's securacv_mqtt (custom_components/securacv). The caller
 * owns the 30 s "event then clear back to none" cadence.
 *
 * publish_mic_state mirrors the hard-mute switch the same way the
 * update/auto switch works: retained "muted"/"live" on
 * {prefix}/{device_id}/mic/state, cached for the reconnect republish.
 *
 * Inbound mute commands arrive on {prefix}/{device_id}/mic/cmd
 * ("mute"/"unmute", or HA's default "ON"/"OFF" where ON = muted) on the
 * esp_mqtt task; csi_mqtt latches them and the main loop drains via
 * take_pending_mic_mute — the loop owns the I2S lifecycle, matching the
 * module's "caller owns the cadence" contract.
 */
void publish_sensing(const char* json_payload);
void publish_mic_state(bool muted);

/** -1 = no command pending; 0 / 1 = HA asked to unmute / mute the mic. */
int take_pending_mic_mute();
#endif  /* FEATURE_ACOUSTIC_EVENTS */

/**
 * ── Firmware update entity (signed pull-OTA) ──────────────────────────
 *
 * publish_update_state pushes the HA MQTT `update` entity's JSON state
 * (installed_version / latest_version / in_progress / update_percentage /
 * release_summary / release_url) to {prefix}/{device_id}/update/state —
 * retained and republished on reconnect so HA stays in sync across
 * broker restarts. publish_update_auto_state mirrors the auto-update
 * switch the same way ("ON"/"OFF" on {prefix}/{device_id}/update/auto).
 *
 * Inbound commands arrive on the esp_mqtt task, so they are NOT
 * delivered via callback — csi_mqtt parses them into pending flags the
 * main loop drains with take_pending_install / take_pending_auto. That
 * keeps flash-cycle decisions on the loop that owns the OTA engine,
 * matching the module's "caller owns the cadence" contract.
 */
void publish_update_state(const char* json_payload);
void publish_update_auto_state(bool enabled);

/** True exactly once after HA pressed Install on the update entity. */
bool take_pending_install();

/** -1 = no change pending; 0 / 1 = HA set the auto-update switch off / on. */
int take_pending_auto();

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
