/**
 * @file csi_mqtt.h
 * @brief Optional MQTT bridge: publishes the canary-wap firmware's CSI
 *        sensing events, health, counts, and chain state to a
 *        user-supplied MQTT broker so the Home Assistant integration in
 *        custom_components/securacv/ (already shipped) sees live data.
 *
 * Backed by ESP-IDF's native esp_mqtt client (mqtt_client.h). Bundled
 * with arduino-esp32 — no lib_deps addition. ESP-IDF runs the MQTT
 * task internally and handles auto-reconnect.
 *
 * Threading (sweep F106): the client is the loop task's. It opens it — at
 * boot from setup() (init()), then from loop() when request_reinit() asked
 * — and the publish_*() functions are called from it too (and, for the
 * reconnect republish, from the esp_mqtt task's own event handler). Another
 * task asks: request_reinit() for a re-init (the config POST, POST
 * /api/mqtt/test and a QR provisioning do), and set_update_auto_state() for
 * the auto-update switch (an httpd handler sets it). A publish from another
 * task could hold the old handle while a re-init retires it.
 *
 * The loop task never STOPS a client: esp_mqtt_client_stop() can wait out a
 * whole connect attempt (the esp_mqtt task holds the client's lock across
 * it: a TCP/TLS connect, the CONNECT write and the CONNACK wait, each given
 * the client's network timeout, kNetworkTimeoutMs below), and then waits for
 * that task to exit. A re-init detaches the old client (no loop publish can
 * reach it, and its events are ignored), a one-shot worker task stops and
 * destroys it, and a later loop pass opens the new one once the worker is
 * done.
 *
 * The loop task's publishes do run esp_mqtt: esp_mqtt_client_publish()
 * takes the same lock and, while connected, writes the socket on the
 * caller's task. Each of those waits is bounded by kNetworkTimeoutMs, set
 * on every client (sweep F112), not esp_mqtt's 10 s default.
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
/* Broker TLS: the MODE byte the shared decision header reads
 * (mqtt_transport_logic.h — 0 plain, 1 CA-verified, 3 lab/unverified; 2,
 * fingerprint pinning, is refused on this esp_mqtt transport) and the CA
 * in PEM form. The older `mqtt.tls` bool is still written, derived as
 * (mode != 0), so nothing that read it breaks; when ONLY the bool exists
 * it reads back as CA mode, which the decision layer REFUSES until a CA is
 * provided — the encrypted-but-unverified socket that bool used to produce
 * never comes back by default (lab mode must be chosen by name). */
constexpr const char* NVS_KEY_TLS_MODE  = "mqtt.tlsmode";
constexpr const char* NVS_KEY_CA        = "mqtt.ca";
/* The backfill's delivery ceiling (F47): always above every event id handed
 * to the broker, written before the id goes. The same key the canary PIO
 * tree's egress writes. csi_integration reads it at boot too, to hold the
 * event-id floor above it (csi_event_id_floor::boot_floor, backlog F46). */
constexpr const char* NVS_KEY_DELIVERED = "csi.evsent";

constexpr size_t MAX_HOST_LEN   = 128;
constexpr size_t MAX_USER_LEN   = 64;
constexpr size_t MAX_PASS_LEN   = 128;
constexpr size_t MAX_PREFIX_LEN = 32;
constexpr size_t MAX_CA_LEN     = 3071;   /* PEM bytes, NUL excluded (mqtt_transport_logic.h kCaPemMax) */

/* The client's network timeout (esp_mqtt_client_config_t's
 * network.timeout_ms; sweep F112): how long esp_mqtt lets one socket
 * operation go without progress before it gives up. Its default is 10 s,
 * and the loop task, which publishes, is subscribed to an 8 s panic
 * watchdog (WATCHDOG_TIMEOUT_SEC in canary_wap.ino).
 *
 * From esp-mqtt's source at the commit ESP-IDF 5.5.4 pins (6af4446; the
 * pinned core 3.3.8 is built on IDF 5.5.4), not probed on a device:
 * esp_mqtt_client_publish() takes the client's API lock and, while
 * connected, writes the message on the caller's task through
 * esp_mqtt_write(), which hands this timeout to every
 * esp_transport_write(). A write that sends nothing within it fails the
 * publish (-1, which publish_raw() reports) and aborts the connection
 * (DISCONNECTED, which clears the bridge's link), and a publish to a client
 * that is not connected returns -1 as soon as it has the lock. The esp_mqtt
 * task holds the same lock across its own socket operations, each given
 * this timeout too: a keepalive ping, a resend, the rest of a long incoming
 * message. A link that stops costs a loop pass one timeout: whichever
 * operation meets the stall (its own write, or the esp_mqtt task's it
 * queued behind) gives up and aborts, and every publish after that returns
 * at publish_raw()'s gate. The timeout restarts on every partial write or
 * read, so it bounds a stall, not a slow trickle: a link that makes a
 * little progress just inside the timeout, write after write, holds a
 * publish longer.
 *
 * What the loop task does not wait for: the esp_mqtt task's connect (the
 * TCP/TLS connect, the CONNECT write and the CONNACK wait, three operations
 * in a row under the lock) and its CONNECTED burst (the status, the
 * discovery set, the cached states and the subscribes, sent under the lock
 * from the event handler). publish_raw() lets nothing but that burst
 * through until the burst is sent (csi_mqtt.cpp's s_burst_task), an abort
 * clears the link before esp_mqtt waits out its reconnect delay with the
 * lock released, and refresh_connection_after_ms (which reconnects in the
 * same locked pass as its abort) is not set. Only the re-init's worker
 * waits out a connect (esp_mqtt_client_stop, F106).
 *
 * kNetworkOpsBudget of them must fit under the loop's watchdog: two back to
 * back (an esp_mqtt operation that finishes just inside the timeout, then
 * the publish's own write meeting the stall), and one more for the rest of
 * the pass. canary_wap.ino static_asserts it, and
 * firmware/scripts/check_wap_loop_commands.py holds this file to setting it
 * on the client. A side effect: a broker that takes longer than this to
 * answer one step of a connect (the TCP connect, a TLS handshake read, the
 * CONNACK) fails that attempt, and esp_mqtt retries it 10 s later. */
constexpr uint32_t kNetworkTimeoutMs = 2000;
constexpr uint32_t kNetworkOpsBudget = 3;

/* Configuration mirror of the NVS row. password is loaded but
 * intentionally never returned by handle_config_get. */
struct Config {
  bool     enabled;
  char     host[MAX_HOST_LEN + 1];
  uint16_t port;
  char     user[MAX_USER_LEN + 1];
  char     pass[MAX_PASS_LEN + 1];
  char     prefix[MAX_PREFIX_LEN + 1];
  bool     tls;        /* derived: tls_mode != 0 (kept for older readers) */
  uint8_t  tls_mode;   /* canary::net::mqtt_tls::Mode as a byte */
  bool     ca_set;     /* a CA PEM is on file under NVS_KEY_CA (never loaded into this struct) */
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

/* Store (or, with an empty/null PEM, remove) the broker CA under
 * NVS_KEY_CA. Callers validate the PEM shape first
 * (canary::net::mqtt_tls::ca_pem_looks_valid); init() re-reads it. */
bool ca_save(const char* pem);

/* What the socket is doing right now ("plain", "tls-ca", "tls-insecure",
 * "refused") and the last transport-level failure in words (empty when
 * none) — constants only, never the CA or a credential. */
const char* transport_name();
const char* last_error();

/**
 * The identity every publish carries (copied). setup() calls it before the
 * network starts, so a re-init a QR provisioning asks for has it even if
 * the HTTP server (and the boot init() in it) never started. Loop task,
 * before any re-init is requested; init() takes the same three again.
 */
void set_identity(const char* device_id,
                  const char* firmware_version,
                  const char* public_key_hex);

/**
 * Cold-boot init, on the loop task (setup()'s start_http_server). Reads
 * NVS, opens the esp_mqtt client if enabled, and arms the LWT. Called once;
 * a call that finds a client open does not stop it (that can block for
 * seconds) but asks loop() for a re-init and returns true. Safe to call
 * before WiFi STA is up; the client stays disconnected until TCP can
 * establish.
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
 * Per-tick pump, main loop. esp_mqtt manages its own task and supervises
 * reconnection internally. This runs, on the loop task, never waiting: a
 * re-init request_reinit() asked for (detach the open client and start the
 * worker that stops it; on a later pass, once it is gone, open the new one:
 * one open serves every request made before it began, since it reads NVS
 * afresh); then the committed-event egress (csi_event_egress::pump): the SD
 * event log, the live publishes and the reconnect backfill; then the
 * auto-update switch state set_update_auto_state() left.
 */
void loop();

/**
 * Any task: ask the loop task to re-run init() (the broker settings in
 * NVS changed, or the owner asked for a fresh connect). Returns the
 * request's number for reinit_done(). Sweep F106: init() ran on the httpd
 * task here, and destroyed the client under a publish on the loop task.
 */
uint32_t request_reinit();

/** Any task: has a re-init whose NVS read began after `request` was made
 *  opened its client (or found the bridge disabled)? */
bool reinit_done(uint32_t request);

/** True iff the underlying MQTT client is connected to the broker. */
bool connected();

/* ── The events egress's wire ──────────────────────────────────────────
 * csi_event_egress.cpp decides which committed csi_event goes out when,
 * and keeps the delivery watermark; these are the publishes it asks for.
 * The egress is their only caller, on the loop task. publish_raw only
 * checks that the client exists and is connected; that is enough because
 * a re-init detaches the client on the same task (loop(), sweep F106), and
 * only a detached client is ever stopped. Before, a config POST or a test
 * ran init() on the httpd task and could destroy the client under one of
 * these publishes. */

/* What one publish attempt did. */
enum class EventSend : uint8_t {
  kSent,         /* handed to esp_mqtt */
  kNotNow,       /* not connected, or the client refused the publish */
  kUnbuildable,  /* the body does not build: this row can never go out */
};

/**
 * Publish one committed csi_event on {prefix}/{device_id}/events, in the
 * shared body (csi_event_wire.h): signed, its event_id, the row's
 * first_seen_ms as the timestamp, `bundled_count`, and `replay` (HA Device
 * Triggers filter replayed rows out, so old events do not re-fire
 * automations after a reconnect).
 */
EventSend publish_event_row(const csi_event_record_t& rec,
                            uint16_t bundled_count,
                            bool replay);

/**
 * The per-kind tamper bridge: a system.integrity row republished on
 * {prefix}/{device_id}/tamper in the shape HA's tamper binary sensors
 * parse (csi_event_wire::build_tamper_bridge_body). Not retained. Returns
 * false when the row is not a tamper kind or the publish failed.
 */
bool publish_tamper_bridge(const char* module_id,
                           const char* type_name,
                           const csi_event_values_t* values);

/** A broker is configured (the bridge is enabled and names a host): the
 *  committed events are owed to it. False rows are logged and owed to
 *  nobody (csi_event_egress.h). Any task. */
bool accepting();

/**
 * Where a committed event is delivered: the broker's host, port and user,
 * and this device's topic prefix (a different prefix is a different topic
 * tree, so another consumer). A password, TLS or discovery change is the
 * same destination. A 32-bit FNV-1a digest, fields separated by a NUL, so
 * init() can tell a change without keeping a second Config: two different
 * destinations share a digest with probability 2^-32.
 */
inline uint32_t destination_digest(const Config& c) {
  uint32_t h = 2166136261u;
  auto mix = [&h](unsigned char b) { h = (h ^ b) * 16777619u; };
  auto mix_str = [&mix](const char* s) {
    for (; *s; ++s) mix((unsigned char)*s);
    mix(0);
  };
  mix_str(c.host);
  mix((unsigned char)(c.port & 0xFF));
  mix((unsigned char)(c.port >> 8));
  mix_str(c.user);
  mix_str(c.prefix);
  return h;
}

/**
 * Bumped by an init() (a config POST, a QR provisioning) that changes the
 * destination (destination_digest) from the one an earlier init() this boot
 * loaded; the boot's first init() only records it. The events egress drops
 * its backlog on a change, as the canary's does on mqtt_destination_epoch():
 * what waited for one broker is not the next one's to see. Any task.
 */
uint32_t destination_epoch();

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
 * The tamper LEVELS the health publish carries (F41), in the canary PIO
 * tree's field names (firmware/canary/src/main.cpp
 * mqtt_publish_health_update). HA's per-type tamper sensors are set by the
 * tamper topic's edge and then follow these levels on every health publish;
 * without them each publish re-cleared a sensor whose lid was still open or
 * whose card was still out. Each is -1 (key omitted), 0 or 1:
 *   sd_mounted      1 while a card is in the slot (MOUNTED or ERROR — a
 *                   failing card is SD Error's story, not SD Removed's),
 *                   0 once it is gone; -1 until a card has mounted this boot
 *                   (a card-less boot is a configuration, not a removal —
 *                   HA reads an absent key as mounted).
 *   enclosure_open  the debounced contact once adopted; -1 on builds without
 *                   one (FEATURE_TAMPER_GPIO=0).
 */
struct MqttTamperLevels {
  int8_t sd_mounted;
  int8_t enclosure_open;
};

/**
 * Push the canonical health snapshot to {prefix}/{device_id}/health.
 * Schema (matches custom_components/securacv/sensor.py health handler):
 *   battery, battery_present, memory_free, uptime, firmware_version,
 *   public_key — plus charge_state, battery_health_pct, battery_mv
 *   when a battery is present, and sd_mounted / enclosure_open when
 *   `tamper` reports them (binary_sensor.py's per-type tamper sensors).
 * The HA sensor derives "healthy/warning/critical" from battery +
 * memory_free; charging devices and mains-powered devices (battery
 * nullptr → battery=100) never trip the battery thresholds.
 */
void publish_health(uint32_t free_heap_bytes, uint32_t uptime_sec,
                    const MqttBatteryInfo* battery = nullptr,
                    const MqttTamperLevels* tamper = nullptr);

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
 * broker restarts (loop task). set_update_auto_state mirrors the
 * auto-update switch the same way ("ON"/"OFF" on
 * {prefix}/{device_id}/update/auto), from any task.
 *
 * Inbound commands arrive on the esp_mqtt task, so they are NOT
 * delivered via callback — csi_mqtt parses them into pending flags the
 * main loop drains with take_pending_install / take_pending_auto. That
 * keeps flash-cycle decisions on the loop that owns the OTA engine,
 * matching the module's "caller owns the cadence" contract.
 */
void publish_update_state(const char* json_payload);

/* The auto-update switch's state, from any task (an httpd handler sets it):
 * cached for the reconnect republish, and published by loop() on the loop
 * task (sweep F106). */
void set_update_auto_state(bool enabled);

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
 * CSI_AUTH_OR_RETURN uses). The /test handler asks the loop task for a
 * reconnect with current NVS settings (request_reinit), waits for it, and
 * reports the broker's reachability within about 4 s, so the dashboard's
 * "Test connection" button gives a real signal rather than a spinner. The
 * config POST saves, asks for the same re-init and waits up to 2 s for it,
 * so the page's status refresh sees the new client. */
esp_err_t handle_config_get(httpd_req_t* req);
esp_err_t handle_config_post(httpd_req_t* req);
esp_err_t handle_test(httpd_req_t* req);
esp_err_t handle_ui(httpd_req_t* req);

}  /* namespace csi_mqtt */

#endif  /* SECURACV_CSI_MQTT_H */
