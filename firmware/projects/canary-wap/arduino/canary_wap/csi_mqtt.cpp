/**
 * @file csi_mqtt.cpp
 * @brief Implementation of the optional MQTT bridge declared in csi_mqtt.h.
 *
 * Threading model:
 *   - esp_mqtt_client maintains its own task, which connects, reads, pings
 *     and resends. A publish is NOT posted to that task: while the client
 *     is connected, esp_mqtt_client_publish() takes the client's API lock
 *     and writes the socket on the caller's task, so the loop task's
 *     publishes wait on the network. A stalled write, and the esp_mqtt
 *     task's own socket operation a publish waits behind, give up after the
 *     client's network timeout, kNetworkTimeoutMs (sweep F112; csi_mqtt.h
 *     says what it bounds and what it does not), set in open_client()
 *     instead of esp_mqtt's 10 s default, which outlasted the loop task's
 *     8 s panic watchdog. The link is announced to the loop task
 *     (s_connected) only after the CONNECTED burst, which esp_mqtt sends
 *     under the same lock, so a loop publish never waits behind it.
 *     Event callbacks fire on the MQTT task; we keep them to flag-flips,
 *     Serial logs and the reconnect republish, so we never block the
 *     network stack.
 *   - The client itself (s_client) is the loop task's (sweep F106): the
 *     boot init() opens it there (setup()), every re-init is served there
 *     (loop(), for request_reinit()), and the publishers run there. The
 *     config POST and the test handler run on the httpd task, and a QR
 *     provisioning on the scanner's; they request a re-init instead of
 *     running one, because a re-init retires the client a loop-task publish
 *     may be holding.
 *   - The loop task never stops a client itself. esp_mqtt_client_stop()
 *     takes the client's API lock, which the esp_mqtt task holds across a
 *     whole connect attempt (the network timeout for the TCP/TLS connect,
 *     again for the CONNECT write and again for the CONNACK; 10 s each
 *     before F112 set it), and then waits for that task to exit: against an
 *     unreachable broker it took ten seconds, and the loop task is
 *     subscribed to an 8 s panic watchdog. So a
 *     re-init detaches the client on the loop task (s_client = nullptr:
 *     nothing on this task can reach it again, and its events are ignored)
 *     and a one-shot worker (retire_task) stops and destroys it; a later
 *     loop pass opens the new client once the worker says the old one is
 *     gone. The esp_mqtt task's own publishes, from its event handler, use
 *     that task's own client, which its stop waits for before the destroy.
 *
 * Privacy model:
 *   - Every successful publish increments csi_integration's outbound
 *     byte counter (add_outbound_bytes), so the dashboard's privacy
 *     pill is honest about MQTT-driven egress. Failures (queue full,
 *     not connected) don't count — bytes that didn't leave the device
 *     don't get counted.
 *
 * Auth model:
 *   - The HTTP config / test handlers go through CSI_AUTH_OR_RETURN
 *     just like every other /api/csi/* route. A casual visitor on the
 *     SoftAP can't read the broker password back, can't change the
 *     broker, and can't trigger /test (which would leak the broker
 *     hostname via a connect attempt).
 */

#include "csi_mqtt.h"
#include "csi_integration.h"
#include "csi_event_egress.h"
#include "api_auth.h"
#include "device_signature.h"
#include "csi_event_wire.h"         /* staged copy of firmware/common/csi/src — the shared events body */
#include "csi_event_id_floor.h"     /* staged copy — space_low(): the health's event-id warning (F82) */
#include "mqtt_transport_logic.h"  /* staged copy of firmware/common/network/ — check_mqtt_transport_sync.sh */

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>       /* xTaskCreate: the retiring client's worker */

extern "C" {
#include "mqtt_client.h"
}

namespace csi_mqtt {

namespace {

constexpr const char* SETTINGS_NS = "csi";
constexpr uint16_t    DEFAULT_PORT = 1883;
constexpr uint16_t    DEFAULT_PORT_TLS = 8883;
constexpr const char* DEFAULT_PREFIX = "securacv";

/* The open client. Written only by the loop task (open_client, detach_client);
 * read by it (publish_raw) and by the esp_mqtt task's event handler, which
 * ignores every event whose client is not this one, so a detached client
 * still being stopped never sets s_connected or publishes. */
std::atomic<esp_mqtt_client_handle_t> s_client{nullptr};
std::atomic<bool>        s_connected{false};
/* The esp_mqtt task while its CONNECTED handler sends the connect burst
 * (the retained status, the discovery set, the cached states, the
 * subscribes), else nullptr. esp_mqtt dispatches CONNECTED with the
 * client's API lock held, and every publish takes that lock: a loop-task
 * publish that saw the link up would wait behind the whole burst, and only
 * the link's throughput bounds it (each write restarts the network
 * timeout). So s_connected turns true only once the burst is sent, the
 * burst's own publishes pass publish_raw()'s gate as this task, and a
 * DISCONNECTED during the burst (the burst's own failed write) clears this
 * so the link is not announced (the F112 review). */
std::atomic<TaskHandle_t> s_burst_task{nullptr};
/* A broker is configured (accepting()): written by open_client() on the loop
 * task, read by the egress on the loop task and by any task. */
std::atomic<bool>        s_accepting{false};
/* destination_epoch(): bumped by an init() whose destination_digest differs
 * from the last one an init() loaded this boot (s_dest_known: one has). */
std::atomic<uint32_t>    s_dest_epoch{0};
bool                     s_dest_known = false;
/* Inbound firmware-update commands. MQTT_EVENT_DATA fires on the
 * esp_mqtt task; flash-cycle decisions belong on the main loop, so the
 * handler only latches these flags and the .ino drains them via
 * take_pending_install / take_pending_auto. */
std::atomic<bool>        s_pending_install{false};
std::atomic<int>         s_pending_auto{-1};
#if FEATURE_ACOUSTIC_EVENTS
/* Inbound mic-mute commands follow the same latch-and-drain pattern:
 * the I2S lifecycle belongs to the main loop (audio_mute defers the
 * driver teardown to audio_process), so the handler only records the
 * request. Cached state mirrors s_last_update_auto for the reconnect
 * republish. */
std::atomic<int>         s_pending_mic{-1};
std::atomic<int>         s_last_mic_state{-1};
#endif
/* Cached update-entity state for the reconnect republish (same reason
 * the retained status republish exists: HA must not sit at "unknown"
 * after a broker restart). */
char                     s_last_update_state[512] = {};
std::atomic<bool>        s_update_state_set{false};
std::atomic<int>         s_last_update_auto{-1};
Config                   s_active_cfg   = {};
/* Broker TLS state. The CA lives here (not in Config: a 3 KB PEM has no
 * business on an httpd handler's stack) because esp_mqtt keeps the pointer
 * it is given for the life of the client. The decision is made once per
 * init() by the shared header; s_last_error is the transport failure in
 * words for the serial log and the /mqtt page — constants only. It is
 * written from the esp_mqtt task and read by httpd; a torn read is a
 * garbled diagnostic, never a secret. */
char                          s_ca_pem[MAX_CA_LEN + 1] = {};
canary::net::mqtt_tls::Decision s_tls_decision;
char                          s_last_error[192]  = {};
char                     s_device_id[33]      = {};
char                     s_firmware_version[24] = {};
char                     s_public_key_hex[65]   = {};
/* Re-inits another task asked for (request_reinit, sweep F106): the
 * number of the newest request, and of the newest one a loop-task init()
 * has served. A request is served by an init() that began after it was
 * made, so it reads the settings the requester saved. */
std::atomic<uint32_t>    s_reinit_wanted{0};
std::atomic<uint32_t>    s_reinit_served{0};
/* A client a re-init detached, being stopped and destroyed by retire_task
 * (loop task only), whether that worker exists yet, and the worker's word
 * that stop and destroy returned. The new client opens only after it. */
esp_mqtt_client_handle_t s_retiring          = nullptr;
bool                     s_retire_started    = false;
bool                     s_retire_create_failed_logged = false;
std::atomic<bool>        s_retire_done{false};
/* The worker's stack: esp_mqtt_client_stop() writes the DISCONNECT on the
 * caller's task (a TLS write under mbedTLS), so the esp_mqtt task's own
 * default stack (CONFIG_MQTT_TASK_STACK_SIZE, 6144) is the measure. */
constexpr uint32_t       kRetireStackBytes   = 6144;
/* set_update_auto_state() from any task; loop() publishes it. */
std::atomic<bool>        s_update_auto_dirty{false};
/* The config POST waits this long for its re-init, so the page's status
 * refresh sees the new client; the test handler's whole answer, re-init
 * and connect, comes within kTestBudgetMs (its connect wait always was). */
constexpr uint32_t       kReinitWaitMs  = 2000;
constexpr uint32_t       kTestBudgetMs  = 4000;
constexpr uint32_t       kReinitPollMs  = 10;

/* Build "{prefix}/{device_id}/{suffix}" into out. Returns out for
 * call-site composability. Out must be sized for prefix + device_id +
 * suffix + 2 separators + NUL — 192 bytes is comfortable. */
char* build_topic(char* out, size_t cap, const char* suffix) {
  if (!out || cap == 0) return nullptr;
  snprintf(out, cap, "%s/%s/%s",
           s_active_cfg.prefix[0] ? s_active_cfg.prefix : DEFAULT_PREFIX,
           s_device_id,
           suffix);
  return out;
}

/* Single chokepoint: every publish goes through here so the
 * privacy-budget accounting and the disconnected-state guard live in
 * exactly one place. Returns true on enqueue success. */
bool publish_raw(const char* topic, const char* payload, size_t len, bool retain) {
  /* Read once: on the esp_mqtt task (the reconnect republish) the loop task
   * may detach it meanwhile, and that task's own client outlives this call. */
  esp_mqtt_client_handle_t client = s_client.load(std::memory_order_acquire);
  if (!client) return false;
  /* The link is up for every task once the connect burst is sent; during
   * it, only for the esp_mqtt task sending it (s_burst_task). */
  if (!s_connected.load(std::memory_order_relaxed) &&
      s_burst_task.load(std::memory_order_relaxed) != xTaskGetCurrentTaskHandle()) {
    return false;
  }
  const int msg_id = esp_mqtt_client_publish(
      client, topic, payload, (int)len, /*qos=*/0, retain ? 1 : 0);
  if (msg_id < 0) return false;
  /* Only counted on successful enqueue. esp_mqtt at QoS 0 may still
   * silently drop on the wire under network failure; the dashboard's
   * privacy pill is "bytes the firmware handed off", not "bytes that
   * crossed the WAN" — same semantics every existing privacy budget
   * counter has. */
  csi_integration::add_outbound_bytes((uint32_t)len + (uint32_t)strlen(topic));
  return true;
}

void mqtt_event_handler(void* /*handler_args*/, esp_event_base_t /*base*/,
                        int32_t event_id, void* event_data) {
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;
  /* A client a re-init detached runs until its worker's stop returns: its
   * connects, drops and errors are not the bridge's any more (they would
   * set s_connected for a client nothing publishes to, or name the old
   * broker's failure). */
  if (!e || e->client != s_client.load(std::memory_order_acquire)) return;
  switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
      /* The connect burst below is sent before the link is announced
       * (s_burst_task): the loop task's publishes wait for the API lock
       * this task holds until it ends, so they wait for nothing. */
      TaskHandle_t self = xTaskGetCurrentTaskHandle();
      s_burst_task.store(self, std::memory_order_relaxed);
      s_last_error[0] = '\0';
      Serial.println("[MQTT] connected");
      /* Replace the LWT-published "offline" with a fresh "online" so
       * a freshly-connecting HA sees the right state immediately. */
      char topic[192];
      build_topic(topic, sizeof(topic), "status");
      /* device_type names the product the same way the mDNS `dt` record
       * and the fleet self-report do, so a hub's roll-call can carry it. */
      const char* online = "{\"online\":true,\"device_type\":\"canary-wap\"}";
      publish_raw(topic, online, strlen(online), /*retain=*/true);
      /* HA MQTT auto-discovery: when discovery=true, publish entity +
       * trigger definitions so a freshly-subscribing HA sees the
       * canary's sensors without anyone touching a config file. Order
       * matters — status MUST land before the discovery payloads
       * reference it as the availability topic, or the entities
       * flicker through "unavailable" on first appearance.
       *
       * When discovery=false, we still need to act: a previous run
       * may have left retained config payloads on the broker, and HA
       * would keep showing the entities forever as "unavailable".
       * remove_discovery() publishes empty retained payloads to all
       * known config topics, which HA reads as "evict this entity". */
      if (s_active_cfg.discovery) {
        publish_discovery();
      } else {
        remove_discovery();
      }
      /* Firmware update entity: subscribe to the Install + auto-update
       * command topics, and republish the cached states so HA reconciles
       * after a broker restart instead of showing "unknown". */
      {
        char cmd_topic[192];
        build_topic(cmd_topic, sizeof(cmd_topic), "update/cmd");
        esp_mqtt_client_subscribe(e->client, cmd_topic, /*qos=*/1);
        build_topic(cmd_topic, sizeof(cmd_topic), "update/auto/cmd");
        esp_mqtt_client_subscribe(e->client, cmd_topic, /*qos=*/1);

        if (s_update_state_set.load(std::memory_order_relaxed)) {
          char state_topic[192];
          build_topic(state_topic, sizeof(state_topic), "update/state");
          publish_raw(state_topic, s_last_update_state,
                      strlen(s_last_update_state), /*retain=*/true);
        }
        const int auto_state = s_last_update_auto.load(std::memory_order_relaxed);
        if (auto_state >= 0) {
          char auto_topic[192];
          build_topic(auto_topic, sizeof(auto_topic), "update/auto");
          const char* pl = auto_state ? "ON" : "OFF";
          publish_raw(auto_topic, pl, strlen(pl), /*retain=*/true);
        }
      }
#if FEATURE_ACOUSTIC_EVENTS
      /* Mic-mute switch: subscribe to the command topic and republish
       * the cached state, same reconnect-reconcile dance as the
       * update entity above. */
      {
        char mic_cmd_topic[192];
        build_topic(mic_cmd_topic, sizeof(mic_cmd_topic), "mic/cmd");
        esp_mqtt_client_subscribe(e->client, mic_cmd_topic, /*qos=*/1);

        const int mic_state = s_last_mic_state.load(std::memory_order_relaxed);
        if (mic_state >= 0) {
          char mic_state_topic[192];
          build_topic(mic_state_topic, sizeof(mic_state_topic), "mic/state");
          const char* pl = mic_state ? "muted" : "live";
          publish_raw(mic_state_topic, pl, strlen(pl), /*retain=*/true);
        }
      }
#endif
      /* No events from here: the egress (csi_event_egress.cpp) runs on the
       * main loop, sees the link up on its next pass and resumes the SD
       * backfill itself, in id order, before anything committed since. A
       * file-system walk on this task would block the reconnect fastpath,
       * and a publish from it would race the backfill. */
      /* The burst is sent: announce the link, unless a DISCONNECTED came
       * meanwhile (the burst's own failed write aborts the connection and
       * dispatches it on this task, clearing s_burst_task) or the loop task
       * detached this client meanwhile (its re-init opens the next one only
       * after this task lets go of the lock: the worker's stop takes it). */
      if (s_burst_task.compare_exchange_strong(self, nullptr, std::memory_order_relaxed) &&
          e->client == s_client.load(std::memory_order_acquire)) {
        s_connected.store(true, std::memory_order_relaxed);
      }
      break;
    }
    case MQTT_EVENT_DATA: {
      /* Inbound update commands. Match on the exact topic, trim
       * whitespace/quotes, require token boundaries — then latch a flag
       * for the main loop (never act on the esp_mqtt task). */
      if (!e || !e->topic || e->topic_len <= 0) break;

      char update_cmd[192];
      char auto_cmd[192];
      build_topic(update_cmd, sizeof(update_cmd), "update/cmd");
      build_topic(auto_cmd, sizeof(auto_cmd), "update/auto/cmd");

      const size_t tlen = (size_t)e->topic_len;
      const bool is_install = (tlen == strlen(update_cmd) &&
                               memcmp(e->topic, update_cmd, tlen) == 0);
      const bool is_auto = (tlen == strlen(auto_cmd) &&
                            memcmp(e->topic, auto_cmd, tlen) == 0);
#if FEATURE_ACOUSTIC_EVENTS
      char mic_cmd[192];
      build_topic(mic_cmd, sizeof(mic_cmd), "mic/cmd");
      const bool is_mic = (tlen == strlen(mic_cmd) &&
                           memcmp(e->topic, mic_cmd, tlen) == 0);
#else
      const bool is_mic = false;
#endif
      if (!is_install && !is_auto && !is_mic) break;

      const char* p = e->data;
      int n = e->data_len;
      while (n > 0 && (*p == ' ' || *p == '\t' || *p == '"')) { p++; n--; }
      auto at_boundary = [](char c) -> bool {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
               c == '"' || c == '}' || c == '\0';
      };

#if FEATURE_ACOUSTIC_EVENTS
      if (is_mic) {
        /* Accept the core securacv_mqtt vocabulary ("mute"/"unmute")
         * plus HA's default switch payloads (ON = muted, OFF = live)
         * so the discovery payload below and hand-rolled automations
         * both work. */
        if (n >= 6 && memcmp(p, "unmute", 6) == 0 &&
            (n == 6 || at_boundary(p[6]))) {
          s_pending_mic.store(0, std::memory_order_relaxed);
        } else if (n >= 4 && memcmp(p, "mute", 4) == 0 &&
                   (n == 4 || at_boundary(p[4]))) {
          s_pending_mic.store(1, std::memory_order_relaxed);
        } else if (n >= 2 && (p[0] == 'O' || p[0] == 'o') &&
                             (p[1] == 'N' || p[1] == 'n') &&
                   (n == 2 || at_boundary(p[2]))) {
          s_pending_mic.store(1, std::memory_order_relaxed);
        } else if (n >= 3 && (p[0] == 'O' || p[0] == 'o') &&
                             (p[1] == 'F' || p[1] == 'f') &&
                             (p[2] == 'F' || p[2] == 'f') &&
                   (n == 3 || at_boundary(p[3]))) {
          s_pending_mic.store(0, std::memory_order_relaxed);
        }
        break;
      }
#endif

      if (is_install) {
        if (n >= 7 && memcmp(p, "install", 7) == 0 &&
            (n == 7 || at_boundary(p[7]))) {
          s_pending_install.store(true, std::memory_order_relaxed);
        }
        break;
      }
      /* Auto switch: HA's default ON/OFF payloads. */
      if (n >= 2 && (p[0] == 'O' || p[0] == 'o') &&
                    (p[1] == 'N' || p[1] == 'n') &&
          (n == 2 || at_boundary(p[2]))) {
        s_pending_auto.store(1, std::memory_order_relaxed);
      } else if (n >= 3 && (p[0] == 'O' || p[0] == 'o') &&
                           (p[1] == 'F' || p[1] == 'f') &&
                           (p[2] == 'F' || p[2] == 'f') &&
                 (n == 3 || at_boundary(p[3]))) {
        s_pending_auto.store(0, std::memory_order_relaxed);
      }
      break;
    }
    case MQTT_EVENT_DISCONNECTED:
      s_burst_task.store(nullptr, std::memory_order_relaxed);
      s_connected.store(false, std::memory_order_relaxed);
      Serial.println("[MQTT] disconnected (will retry)");
      break;
    case MQTT_EVENT_BEFORE_CONNECT:
      /* The lab opt-in is acceptable only because it is named on EVERY
       * connect (mqtt_transport_logic.h) — esp_mqtt reconnects on its own
       * task, so this event is the one hook that fires each time. */
      if (s_tls_decision.warn_insecure()) {
        Serial.printf("[MQTT] %s\n", canary::net::mqtt_tls::insecure_warning());
      }
      break;
    case MQTT_EVENT_ERROR:
      if (e && e->error_handle) {
        Serial.printf("[MQTT] error: type=%d esp_tls_err=0x%x\n",
                      (int)e->error_handle->error_type,
                      (unsigned)e->error_handle->esp_tls_last_esp_err);
        /* Name the TLS reason (wrong CA, expired cert, plaintext listener
         * on a TLS port) so the fix is in the log, not just a hex code.
         * esp_tls_stack_err is the mbedTLS code; the verify flags are the
         * X.509 result when the handshake got that far. */
        if (s_tls_decision.tls() &&
            e->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
          if (canary::net::mqtt_tls::format_failure(
                  s_last_error, sizeof(s_last_error), s_tls_decision.transport,
                  canary::net::mqtt_tls::Reason::None,
                  (int)e->error_handle->esp_tls_stack_err,
                  (uint32_t)e->error_handle->esp_tls_cert_verify_flags,
                  /*fingerprint_mismatch=*/false) > 0) {
            Serial.printf("[MQTT] %s\n", s_last_error);
          }
        }
      }
      break;
    default:
      break;
  }
}

/* The worker a re-init hands a detached client to (sweep F106 review): the
 * stop can wait out a connect attempt, which the loop task must not. One
 * worker per re-init, and only one at a time (serve_reinit opens nothing
 * until it is done). */
void retire_task(void* arg) {
  esp_mqtt_client_handle_t client = static_cast<esp_mqtt_client_handle_t>(arg);
  const uint32_t start = millis();
  esp_mqtt_client_stop(client);      /* waits for the client's esp_mqtt task to exit */
  esp_mqtt_client_destroy(client);
  Serial.printf("[MQTT] previous client stopped after %lu ms\n",
                (unsigned long)(millis() - start));
  s_retire_done.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

/* Loop task: take the open client out of service. Nothing on this task can
 * reach it after this (s_client is what every publish reads), and its event
 * handler ignores it; retire_finished() hands it to the worker. */
void detach_client() {
  esp_mqtt_client_handle_t client = s_client.exchange(nullptr, std::memory_order_acq_rel);
  s_connected.store(false, std::memory_order_relaxed);
  if (client) {
    s_retiring = client;
    s_retire_started = false;
  }
}

/* Loop task: is the client a re-init detached gone? Starts its worker (and
 * again on a later pass, if the task could not be created) and never waits. */
bool retire_finished() {
  if (s_retiring == nullptr) return true;
  if (!s_retire_started) {
    s_retire_done.store(false, std::memory_order_relaxed);
    if (xTaskCreate(retire_task, "mqtt_retire", kRetireStackBytes, s_retiring,
                    /*priority=*/1, nullptr) != pdPASS) {
      if (!s_retire_create_failed_logged) {
        Serial.println("[MQTT] could not start the old client's stop task; retrying");
        s_retire_create_failed_logged = true;
      }
      return false;
    }
    s_retire_started = true;
    s_retire_create_failed_logged = false;
  }
  if (!s_retire_done.load(std::memory_order_acquire)) return false;
  s_retiring = nullptr;
  s_retire_started = false;
  return true;
}

}  /* namespace */

/* ──────────────────────────────────────────────────────────────────────────
 * NVS load / save
 * ────────────────────────────────────────────────────────────────────────── */

bool config_load(Config* out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/true)) {
    /* No NVS yet (or namespace corrupt) — treat as disabled with
     * default port + prefix. Discovery defaults to true so a fresh
     * device with no NVS state still publishes HA auto-discovery on
     * the first connect — without this the memset above leaves
     * discovery=false and we'd silently break the "zero-config HA"
     * onboarding path (PR #396 review r3213931333). */
    out->port      = DEFAULT_PORT;
    out->discovery = true;
    strncpy(out->prefix, DEFAULT_PREFIX, MAX_PREFIX_LEN);
    return true;
  }
  out->enabled = prefs.getBool (NVS_KEY_ENABLED, false);
  out->port    = (uint16_t)prefs.getUShort(NVS_KEY_PORT, DEFAULT_PORT);
  out->tls     = prefs.getBool (NVS_KEY_TLS,     false);
  /* Mode byte wins; a row that predates it carries only the bool, which
   * maps to CA mode — refused by init() until a CA is on file, so the old
   * unverified mqtts:// never silently returns. */
  out->tls_mode = prefs.isKey(NVS_KEY_TLS_MODE)
                      ? prefs.getUChar(NVS_KEY_TLS_MODE, 0)
                      : (out->tls ? (uint8_t)canary::net::mqtt_tls::Mode::Ca : 0);
  out->tls    = out->tls_mode != 0;
  out->ca_set = prefs.isKey(NVS_KEY_CA);
  /* Discovery defaults true on first boot — HA users get auto-pickup
   * without a separate toggle. Non-HA users flip it off via the /mqtt
   * settings page; the persisted bool then survives reboots. */
  out->discovery = prefs.getBool(NVS_KEY_DISCOVERY, true);
  prefs.getString(NVS_KEY_HOST,   out->host,   MAX_HOST_LEN);
  prefs.getString(NVS_KEY_USER,   out->user,   MAX_USER_LEN);
  prefs.getString(NVS_KEY_PASS,   out->pass,   MAX_PASS_LEN);
  prefs.getString(NVS_KEY_PREFIX, out->prefix, MAX_PREFIX_LEN);
  prefs.end();
  if (!out->prefix[0]) strncpy(out->prefix, DEFAULT_PREFIX, MAX_PREFIX_LEN);
  if (out->port == 0)  out->port = out->tls ? DEFAULT_PORT_TLS : DEFAULT_PORT;
  return true;
}

bool config_save(const Config& cfg) {
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/false)) return false;
  prefs.putBool  (NVS_KEY_ENABLED,   cfg.enabled);
  prefs.putString(NVS_KEY_HOST,      cfg.host);
  prefs.putUShort(NVS_KEY_PORT,      cfg.port);
  prefs.putString(NVS_KEY_USER,      cfg.user);
  prefs.putString(NVS_KEY_PASS,      cfg.pass);
  prefs.putString(NVS_KEY_PREFIX,    cfg.prefix);
  prefs.putUChar (NVS_KEY_TLS_MODE,  cfg.tls_mode);
  prefs.putBool  (NVS_KEY_TLS,       cfg.tls_mode != 0);
  prefs.putBool  (NVS_KEY_DISCOVERY, cfg.discovery);
  prefs.end();
  return true;
}

bool ca_save(const char* pem) {
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/false)) return false;
  bool ok;
  if (!pem || !pem[0]) {
    /* remove() also reports false for a key that never existed — the
     * readback is the truth. */
    prefs.remove(NVS_KEY_CA);
    ok = !prefs.isKey(NVS_KEY_CA);
  } else {
    ok = prefs.putString(NVS_KEY_CA, pem) > 0;
  }
  prefs.end();
  return ok;
}

namespace {
void ca_load() {
  s_ca_pem[0] = '\0';
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/true)) return;
  if (prefs.isKey(NVS_KEY_CA)) {
    prefs.getString(NVS_KEY_CA, s_ca_pem, sizeof(s_ca_pem));
    s_ca_pem[sizeof(s_ca_pem) - 1] = '\0';
  }
  prefs.end();
}
}  /* namespace */

const char* transport_name() {
  /* A bridge that is switched off, or has no broker host yet, never reaches
   * the socket decision — say so rather than reporting the default
   * Decision's "refused" for a device nobody has configured. */
  if (!s_active_cfg.enabled) return "disabled";
  if (!s_active_cfg.host[0]) return "unconfigured";
  return canary::net::mqtt_tls::transport_name(s_tls_decision.transport);
}

const char* last_error() { return s_last_error; }

/* ──────────────────────────────────────────────────────────────────────────
 * Lifecycle
 * ────────────────────────────────────────────────────────────────────────── */

void set_identity(const char* device_id,
                  const char* firmware_version,
                  const char* public_key_hex) {
  if (device_id && device_id != s_device_id) {
    strncpy(s_device_id, device_id, sizeof(s_device_id) - 1);
    s_device_id[sizeof(s_device_id) - 1] = '\0';
  }
  if (firmware_version && firmware_version != s_firmware_version) {
    strncpy(s_firmware_version, firmware_version, sizeof(s_firmware_version) - 1);
    s_firmware_version[sizeof(s_firmware_version) - 1] = '\0';
  }
  if (public_key_hex && public_key_hex != s_public_key_hex) {
    strncpy(s_public_key_hex, public_key_hex, sizeof(s_public_key_hex) - 1);
    s_public_key_hex[sizeof(s_public_key_hex) - 1] = '\0';
  }
}

namespace {
/* Loop task, with no client open: read the settings from NVS and open the
 * client they name (or none: disabled, no host, a refused TLS mode). The
 * boot init() and every served re-init run it; neither ever stops a client
 * here (a re-init's old one is retire_task's). */
bool open_client() {
  /* The delivery watermark is not this function's: the egress restores it
   * once per boot (csi_event_egress::begin, from csi_integration::init after
   * the event-id floor), and a re-init keeps it. A re-init that changes the
   * destination (host, port, user or prefix) bumps destination_epoch(), and
   * the egress drops what waited for the old broker; the boot's first init
   * only records it. */
  const uint32_t prev_dest = destination_digest(s_active_cfg);
  if (!config_load(&s_active_cfg)) return false;
  if (s_dest_known && destination_digest(s_active_cfg) != prev_dest) {
    s_dest_epoch.fetch_add(1, std::memory_order_relaxed);
  }
  s_dest_known = true;
  s_accepting.store(s_active_cfg.enabled && s_active_cfg.host[0] != '\0',
                    std::memory_order_relaxed);
  if (!s_active_cfg.enabled) {
    Serial.println("[MQTT] disabled in NVS — bridge not started");
    return true;
  }
  if (!s_active_cfg.host[0]) {
    Serial.println("[MQTT] enabled but no broker host configured");
    return false;
  }

  /* The socket decision is the shared header's, not this file's: plain or
   * CA-verified — and REFUSED (with the reason in the log and on the /mqtt
   * page) for a TLS mode with no usable CA. esp_mqtt has no fingerprint
   * hook, so caps.fingerprint is false and a pinned provisioning is refused
   * with "use the CA mode". caps.insecure is false too: the pinned Arduino
   * core builds esp-tls WITHOUT CONFIG_ESP_TLS_INSECURE (sdkconfig of
   * every chip in framework-arduinoespressif32-libs 3.3.8), so an mqtts://
   * session with no verification option returns ESP_ERR_INVALID_STATE —
   * the lab mode could never connect here, and the header's own
   * InsecureUnsupported refusal says so instead of an opaque esp-tls error. */
  ca_load();
  {
    using namespace canary::net::mqtt_tls;
    Caps caps;
    caps.fingerprint = false;
    caps.insecure = false;
    s_tls_decision = decide_u8(s_active_cfg.tls_mode, s_ca_pem, nullptr, caps);
    if (!s_tls_decision.allowed()) {
      format_failure(s_last_error, sizeof(s_last_error), Transport::Refused,
                     s_tls_decision.reason, 0, 0, false);
      Serial.printf("[MQTT] %s\n", s_last_error);
      return false;
    }
    s_last_error[0] = '\0';
  }

  /* Build the broker URI once; ESP-IDF accepts mqtt:// and mqtts://. */
  char uri[160];
  snprintf(uri, sizeof(uri), "%s://%s:%u",
           canary::net::mqtt_tls::uri_scheme(s_tls_decision.transport),
           s_active_cfg.host,
           (unsigned)s_active_cfg.port);

  /* LWT topic: same status topic the on-connect handler will publish
   * "online" to. Setting the will message before connect lets the
   * broker auto-flip us back to offline if we vanish. */
  char will_topic[192];
  build_topic(will_topic, sizeof(will_topic), "status");
  static const char will_msg[] = "{\"online\":false}";

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = uri;
  if (s_active_cfg.user[0]) {
    cfg.credentials.username = s_active_cfg.user;
  }
  if (s_active_cfg.pass[0]) {
    cfg.credentials.authentication.password = s_active_cfg.pass;
  }
  /* TlsCa: esp-tls verifies the broker's chain against this PEM (NUL-
   * terminated, so certificate_len stays 0). Plain sets nothing. There is
   * no TlsInsecure branch: decide() refuses that mode on this transport
   * (caps.insecure above), so it never reaches the client config. */
  if (s_tls_decision.transport == canary::net::mqtt_tls::Transport::TlsCa) {
    cfg.broker.verification.certificate = s_ca_pem;
  }
  cfg.session.last_will.topic   = will_topic;
  cfg.session.last_will.msg     = will_msg;
  cfg.session.last_will.msg_len = (int)strlen(will_msg);
  cfg.session.last_will.qos     = 1;
  cfg.session.last_will.retain  = 1;
  cfg.session.keepalive         = 60;
  /* Sweep F112: every socket operation esp_mqtt runs for this client (the
   * loop task's publish writes, and the connect, pings and resends it holds
   * the API lock across) gives up after this long without progress, not
   * after esp_mqtt's 10 s default, past the loop's 8 s watchdog. The
   * nested IDF 5 field, like every field above: canary-wap builds only on
   * Arduino-ESP32 3.x (IDF 5.5). */
  cfg.network.timeout_ms        = (int)kNetworkTimeoutMs;

  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
  if (!client) {
    Serial.println("[MQTT] esp_mqtt_client_init returned null");
    return false;
  }
  esp_mqtt_client_register_event(
      client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, nullptr);
  /* The open client before its task starts: its handler checks it. A stale
   * s_connected from a detached client's last event is cleared first. */
  s_connected.store(false, std::memory_order_relaxed);
  s_client.store(client, std::memory_order_release);
  if (esp_mqtt_client_start(client) != ESP_OK) {
    Serial.println("[MQTT] esp_mqtt_client_start failed");
    s_client.store(nullptr, std::memory_order_release);
    /* Never started: there is no esp_mqtt task to wait for, so the destroy
     * returns at once (it stops only a running client). */
    esp_mqtt_client_destroy(client);
    return false;
  }
  Serial.printf("[MQTT] bridge started: %s transport=%s prefix=%s\n", uri,
                transport_name(), s_active_cfg.prefix);
  return true;
}

/* Loop task: a re-init another task asked for (sweep F106). It never waits:
 * the open client is detached and handed to retire_task, and the passes
 * after that return at once until the worker says it is gone; then one
 * open_client() serves every request made before it began, because it
 * reads NVS afresh (and keeps the identity set_identity() stored). A
 * request made while it runs waits for the next re-init. */
void serve_reinit() {
  if (!retire_finished()) return;                 /* the old client is still stopping */
  if (s_reinit_wanted.load(std::memory_order_acquire) ==
      s_reinit_served.load(std::memory_order_relaxed)) {
    return;
  }
  if (s_client.load(std::memory_order_relaxed) != nullptr) {
    detach_client();
    if (!retire_finished()) return;               /* a later pass opens the new one */
  }
  const uint32_t wanted = s_reinit_wanted.load(std::memory_order_acquire);
  (void)open_client();
  s_reinit_served.store(wanted, std::memory_order_release);
}
}  /* namespace */

bool init(const char* device_id,
          const char* firmware_version,
          const char* public_key_hex) {
  set_identity(device_id, firmware_version, public_key_hex);
  /* The boot's open (setup()'s start_http_server, before loop() runs). A
   * client already open, or still being retired, is never stopped here: that
   * could wait out a connect attempt. Hand it to loop()'s re-init instead. */
  if (s_client.load(std::memory_order_relaxed) != nullptr || s_retiring != nullptr) {
    (void)request_reinit();
    return true;
  }
  return open_client();
}

void loop() {
  /* First, so the pump below sees a changed destination's epoch the pass the
   * new client opens. */
  serve_reinit();

  /* The committed-event egress: the SD log, the live publishes and the
   * reconnect backfill, on this (the main loop's) task. */
  csi_event_egress::pump();

  /* The auto-update switch's state, set from any task. */
  if (s_update_auto_dirty.exchange(false, std::memory_order_acq_rel)) {
    const int state = s_last_update_auto.load(std::memory_order_relaxed);
    if (state >= 0) {
      char topic[192];
      build_topic(topic, sizeof(topic), "update/auto");
      const char* pl = state ? "ON" : "OFF";
      publish_raw(topic, pl, strlen(pl), /*retain=*/true);
    }
  }
}

uint32_t request_reinit() {
  return s_reinit_wanted.fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool reinit_done(uint32_t request) {
  return (int32_t)(s_reinit_served.load(std::memory_order_acquire) - request) >= 0;
}

namespace {
/* Any task but the loop task (it would wait for itself): wait up to
 * timeout_ms for the loop task to serve `request`. True when it has. */
bool wait_reinit(uint32_t request, uint32_t timeout_ms) {
  const uint32_t start = millis();
  while (!reinit_done(request)) {
    if ((uint32_t)(millis() - start) >= timeout_ms) return false;
    delay(kReinitPollMs);
  }
  return true;
}
}  /* namespace */

bool accepting() {
  return s_accepting.load(std::memory_order_relaxed);
}

uint32_t destination_epoch() {
  return s_dest_epoch.load(std::memory_order_relaxed);
}

bool connected() {
  return s_connected.load(std::memory_order_relaxed);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Concrete publishers
 *
 * Schemas locked against custom_components/securacv/sensor.py:
 *   events: {event_type, timestamp, zone, confidence, signed, motion,
 *            breathing, bpm, duration_sec, state}
 *   health: {battery, memory_free, uptime, firmware_version, public_key}
 *   egress: csi_event_egress::stats_json (the canary's health
 *           csi_event_egress object; HA's health sensor attributes)
 *   chain : {length, latest_hash, algorithm}
 *   counts: {total}
 *   status: {online, csi_running, wifi_connected, rssi}
 *
 * "zone" is reserved for the future RF wizard flow (Phase 12); we
 * publish "" so the HA sensor's data.get("zone","") path remains
 * type-stable. ────────────────────────────────────────────────────── */

/* Shared body builder so live publishes and backfill replays share one
 * wire shape — and, since F29, so this sketch and the canary PIO tree
 * share it too: the body is built by csi_event_wire.h (staged from
 * firmware/common/csi/src, byte-gated by check_csi_sync.sh; golden-tested
 * in firmware/tests_host/test_csi_event_wire.cpp). This wrapper injects
 * device_signature as the signer. timestamp_ms is the device-monotonic
 * millisecond mark the event committed at — published as the seconds
 * figure HA stores in `timestamp` (live: now; backfill: the event's
 * first_seen_ms).
 *
 * is_replay flips the JSON's `replay` field so HA Device Triggers can
 * filter backfill traffic out of their match expressions and avoid
 * re-firing automations for old events after a reconnect (PR #398
 * review r3214114357). Live publishes pass false; the SD backfill
 * passes true. Either way, sensors and binary_sensors that ignore the
 * replay flag still see the up-to-date state — only the trigger path
 * gates on it.
 *
 * If signing fails (device_signature::init not called yet on a very
 * early boot publish) the body goes out without sig fields and
 * `"signed":false` — HA marks the device "unverified" but still accepts
 * the publish.
 *
 * Returns the byte count written, or 0 on overflow. */
namespace {
static_assert(csi_event_wire::SIG_B64URL_CAP == device_signature::SIG_B64URL_CAP,
              "csi_event_wire's signature buffer must hold a device_signature sig");

size_t build_event_body(char* body, size_t cap,
                        uint32_t                  event_id,
                        const char*               module_id,
                        const char*               type_name,
                        csi_event_category_t      category,
                        csi_privacy_class_t       privacy,
                        const csi_event_values_t* values,
                        uint32_t                  timestamp_ms,
                        uint16_t                  bundled_count,
                        bool                      is_replay) {
  const csi_event_wire::Signer signer = {
    &device_signature::sign_event,
    device_signature::SCHEMA_V,
    device_signature::ALG_NAME,
    device_signature::fingerprint_hex(),
  };
  return csi_event_wire::build_event_body(body, cap, event_id, module_id,
                                          type_name, category, privacy,
                                          values, timestamp_ms, bundled_count,
                                          is_replay, signer);
}
}  /* namespace */

EventSend publish_event_row(const csi_event_record_t& rec,
                            uint16_t bundled_count,
                            bool replay) {
  char topic[192];
  build_topic(topic, sizeof(topic), "events");
  /* body[] grew from 512 → 768 to accommodate the new sig envelope
   * (~120 extra bytes for v/alg/fp/sig + event_id). 768 is still
   * comfortably below esp_mqtt's per-publish ceiling (~1.4 KB after
   * topic + retained-flag overhead). */
  char body[768];
  const size_t n = build_event_body(body, sizeof(body),
      rec.event_id,
      rec.module_id, rec.type_name, rec.category, rec.privacy, &rec.values,
      rec.first_seen_ms, bundled_count, replay);
  if (n == 0) return EventSend::kUnbuildable;
  return publish_raw(topic, body, n, /*retain=*/false) ? EventSend::kSent
                                                      : EventSend::kNotNow;
}

bool publish_tamper_bridge(const char* module_id,
                           const char* type_name,
                           const csi_event_values_t* values) {
  /* Per-kind tamper bridge: the HA integration's tamper binary sensors —
   * the general one and the ten per-type ones — have subscribed to the
   * dedicated `tamper` topic since day one
   * (custom_components/securacv/binary_sensor.py), while committed events
   * ride `events`; until this bridge, a system.integrity commit narrated
   * on the phone while every HA tamper entity stayed silent. Republish it
   * here in the exact shape those sensors already parse: the per-type
   * sensor matches data.type == its kind, the general one fires on any
   * publish and reads type/detail as attributes (the body is
   * csi_event_wire::build_tamper_bridge_body, the same one the canary PIO
   * tree publishes). state_name is chokepoint-sanitized ASCII, so no
   * escaping is needed. LIVE commits only — the backfill replay path stays
   * off this topic on purpose: it carries no is_replay marker, and
   * re-firing tamper automations for old events is exactly what the
   * replay flag exists to prevent. */
  char tbody[128];
  const size_t tn = csi_event_wire::build_tamper_bridge_body(
      tbody, sizeof(tbody), module_id, type_name, values);
  if (tn == 0) return false;
  char ttopic[192];
  build_topic(ttopic, sizeof(ttopic), "tamper");
  return publish_raw(ttopic, tbody, tn, /*retain=*/false);
}

void publish_chain(uint32_t length, const uint8_t* latest_hash_32) {
  char topic[192];
  build_topic(topic, sizeof(topic), "chain");
  char hash_hex[65];
  hash_hex[0] = '\0';
  if (latest_hash_32) {
    /* Reuse csi_integration's canonical hex encoder rather than
     * duplicating the loop here (PR #394 review r3213674564). */
    csi_integration::hex_encode(latest_hash_32, 32, hash_hex);
  }

  /* PKI envelope: alongside the existing length/latest_hash fields we
   * publish v (canonical-schema version), sig (b64url Ed25519 over the
   * canonical "chain" message), fp (device fingerprint hex), and alg
   * (always "ed25519" for v1). HA's signature.py reconstructs the
   * canonical string from (length, latest_hash, device_id-from-topic)
   * and verifies. sig is best-effort — if device_signature isn't
   * initialized yet we publish the data without a sig and HA marks
   * the device "unverified" but still updates state. Keeping
   * "algorithm":"ed25519" alongside the new "alg" field so older HA
   * builds that only read "algorithm" don't regress. */
  char sig_b64[device_signature::SIG_B64URL_CAP] = "";
  bool signed_ok = false;
  if (latest_hash_32) {
    signed_ok = device_signature::sign_chain(length, latest_hash_32,
                                             sig_b64, sizeof(sig_b64));
  }

  char body[320];
  int n;
  if (signed_ok) {
    n = snprintf(body, sizeof(body),
      "{\"v\":%d,\"length\":%lu,\"latest_hash\":\"%s\","
      "\"algorithm\":\"ed25519\","
      "\"alg\":\"%s\",\"fp\":\"%s\",\"sig\":\"%s\"}",
      device_signature::SCHEMA_V,
      (unsigned long)length, hash_hex,
      device_signature::ALG_NAME,
      device_signature::fingerprint_hex(),
      sig_b64);
  } else {
    n = snprintf(body, sizeof(body),
      "{\"v\":%d,\"length\":%lu,\"latest_hash\":\"%s\","
      "\"algorithm\":\"ed25519\"}",
      device_signature::SCHEMA_V,
      (unsigned long)length, hash_hex);
  }
  if (n <= 0 || (size_t)n >= sizeof(body)) return;
  publish_raw(topic, body, (size_t)n, /*retain=*/true);
}

void publish_health(uint32_t free_heap_bytes, uint32_t uptime_sec,
                    const MqttBatteryInfo* battery,
                    const MqttTamperLevels* tamper) {
  char topic[192];
  build_topic(topic, sizeof(topic), "health");
  /* battery=100 with battery_present=false reflects mains power, so the
   * HA "healthy/warning/critical" derivation always has a number to
   * compare. With a battery wired (power_monitor HW ADC mode) the .ino
   * passes the real state and HA gets SoC, charge state, and the
   * cycle-fade health estimate. The object is closed below, after the
   * optional tamper levels. Worst case (battery, the 11-char
   * "discharging", 10-digit counters, a 23-char firmware version, the
   * event-id flag, both levels): 350 bytes, so 384 still fits. */
  char body[384];
  int n;
  if (battery) {
    n = snprintf(body, sizeof(body),
      "{"
        "\"battery\":%u,"
        "\"battery_present\":true,"
        "\"charge_state\":\"%s\","
        "\"battery_health_pct\":%u,"
        "\"battery_mv\":%u,"
        "\"memory_free\":%lu,"
        "\"uptime\":%lu,"
        "\"firmware_version\":\"%s\","
        "\"public_key\":\"%s\"",
      (unsigned)battery->soc_pct,
      battery->charge_state ? battery->charge_state : "unknown",
      (unsigned)battery->health_pct,
      (unsigned)battery->battery_mv,
      (unsigned long)free_heap_bytes,
      (unsigned long)uptime_sec,
      s_firmware_version,
      s_public_key_hex);
  } else {
    n = snprintf(body, sizeof(body),
      "{"
        "\"battery\":100,"
        "\"battery_present\":false,"
        "\"memory_free\":%lu,"
        "\"uptime\":%lu,"
        "\"firmware_version\":\"%s\","
        "\"public_key\":\"%s\"",
      (unsigned long)free_heap_bytes,
      (unsigned long)uptime_sec,
      s_firmware_version,
      s_public_key_hex);
  }
  if (n <= 0 || (size_t)n >= sizeof(body)) return;
  size_t len = (size_t)n;
  /* The event-id space is running out (sweep F82), the canary PIO tree's
   * flag of the same name: true once the allocator reaches
   * csi_event_id_floor::kHoldLimit, and after it wraps, when Home Assistant
   * starts refusing this device's events. A warning only: the recovery is
   * not decided yet. */
  n = snprintf(body + len, sizeof(body) - len, ",\"event_id_space_low\":%s",
               csi_event_id_floor::space_low(csi_event_get_next_event_id()) ? "true" : "false");
  if (n <= 0 || (size_t)n >= sizeof(body) - len) return;
  len += (size_t)n;
  /* The tamper levels (F41), each only when the .ino reports it — see
   * MqttTamperLevels for why an absent key is the right answer. */
  if (tamper && tamper->sd_mounted >= 0) {
    n = snprintf(body + len, sizeof(body) - len, ",\"sd_mounted\":%s",
                 tamper->sd_mounted ? "true" : "false");
    if (n <= 0 || (size_t)n >= sizeof(body) - len) return;
    len += (size_t)n;
  }
  if (tamper && tamper->enclosure_open >= 0) {
    n = snprintf(body + len, sizeof(body) - len, ",\"enclosure_open\":%s",
                 tamper->enclosure_open ? "true" : "false");
    if (n <= 0 || (size_t)n >= sizeof(body) - len) return;
    len += (size_t)n;
  }
  if (len + 1 >= sizeof(body)) return;
  body[len++] = '}';
  body[len] = '\0';
  publish_raw(topic, body, len, /*retain=*/true);
}

void publish_egress() {
  char topic[192];
  build_topic(topic, sizeof(topic), "egress");
  char body[csi_event_egress::kStatsJsonMax];
  const size_t n = csi_event_egress::stats_json(csi_event_egress::stats(), body, sizeof(body));
  if (n == 0) return;
  publish_raw(topic, body, n, /*retain=*/true);
}

void publish_update_state(const char* json_payload) {
  if (!json_payload) return;
  /* Cache first so the reconnect republish always has the latest
   * snapshot, even if the broker is down right now. */
  strncpy(s_last_update_state, json_payload, sizeof(s_last_update_state) - 1);
  s_last_update_state[sizeof(s_last_update_state) - 1] = '\0';
  s_update_state_set.store(true, std::memory_order_relaxed);

  char topic[192];
  build_topic(topic, sizeof(topic), "update/state");
  publish_raw(topic, s_last_update_state, strlen(s_last_update_state),
              /*retain=*/true);
}

void set_update_auto_state(bool enabled) {
  /* Cached for the reconnect republish; loop() publishes it (an httpd
   * handler sets it, and a publish from there could hold a client the
   * loop task's re-init destroys: sweep F106). */
  s_last_update_auto.store(enabled ? 1 : 0, std::memory_order_relaxed);
  s_update_auto_dirty.store(true, std::memory_order_release);
}

bool take_pending_install() {
  return s_pending_install.exchange(false, std::memory_order_relaxed);
}

int take_pending_auto() {
  return s_pending_auto.exchange(-1, std::memory_order_relaxed);
}

#if FEATURE_ACOUSTIC_EVENTS
void publish_sensing(const char* json_payload) {
  if (!json_payload) return;
  char topic[192];
  build_topic(topic, sizeof(topic), "sensing");
  /* Retained so a Home Assistant restart re-acquires the latest
   * acoustic snapshot immediately rather than sitting at "Unknown"
   * until the next publish. */
  publish_raw(topic, json_payload, strlen(json_payload), /*retain=*/true);
}

void publish_mic_state(bool muted) {
  /* Cache first so the CONNECTED republish reflects the latest state
   * even when this publish happens while disconnected (e.g. the boot
   * state is recorded before the broker session is up). */
  s_last_mic_state.store(muted ? 1 : 0, std::memory_order_relaxed);
  char topic[192];
  build_topic(topic, sizeof(topic), "mic/state");
  const char* pl = muted ? "muted" : "live";
  publish_raw(topic, pl, strlen(pl), /*retain=*/true);
}

int take_pending_mic_mute() {
  return s_pending_mic.exchange(-1, std::memory_order_relaxed);
}
#endif  /* FEATURE_ACOUSTIC_EVENTS */

void publish_counts(uint32_t total) {
  char topic[192];
  build_topic(topic, sizeof(topic), "counts");

  /* Sign the total — defends against an adversary on the broker
   * spoofing a witness count (which would make the "Witness Records"
   * sensor lie, masking dropped or replayed records). Format mirrors
   * publish_chain: same v/alg/fp/sig fields, omitted if signing fails. */
  char sig_b64[device_signature::SIG_B64URL_CAP] = "";
  const bool signed_ok = device_signature::sign_counts(total, sig_b64,
                                                       sizeof(sig_b64));

  char body[224];
  int n;
  if (signed_ok) {
    n = snprintf(body, sizeof(body),
      "{\"v\":%d,\"total\":%lu,"
      "\"alg\":\"%s\",\"fp\":\"%s\",\"sig\":\"%s\"}",
      device_signature::SCHEMA_V,
      (unsigned long)total,
      device_signature::ALG_NAME,
      device_signature::fingerprint_hex(),
      sig_b64);
  } else {
    n = snprintf(body, sizeof(body),
      "{\"v\":%d,\"total\":%lu}",
      device_signature::SCHEMA_V, (unsigned long)total);
  }
  if (n <= 0 || (size_t)n >= sizeof(body)) return;
  publish_raw(topic, body, (size_t)n, /*retain=*/true);
}

void publish_status(bool csi_running, bool wifi_connected, int rssi_dbm) {
  char topic[192];
  build_topic(topic, sizeof(topic), "status");
  char body[160];
  const int n = snprintf(body, sizeof(body),
    "{"
      "\"online\":true,"
      "\"device_type\":\"canary-wap\","
      "\"csi_running\":%s,"
      "\"wifi_connected\":%s,"
      "\"rssi\":%d"
    "}",
    csi_running ? "true" : "false",
    wifi_connected ? "true" : "false",
    rssi_dbm);
  if (n <= 0 || (size_t)n >= sizeof(body)) return;
  publish_raw(topic, body, (size_t)n, /*retain=*/true);
}

void publish_chirp_state(const char* state_name) {
  /* Chirp's NFPA-72-style state surface. State is a string enum produced
   * by chirp_channel::state_name (or a derived NFPA-72 mapping in the
   * loop). HA renders it as a single sensor with automation hooks. */
  char topic[192];
  build_topic(topic, sizeof(topic), "chirp");
  char body[128];
  const int n = snprintf(body, sizeof(body),
    "{\"state\":\"%s\"}",
    state_name ? state_name : "unknown");
  if (n <= 0 || (size_t)n >= sizeof(body)) return;
  publish_raw(topic, body, (size_t)n, /*retain=*/true);
}

void publish_beacon_state(const char* state_name,
                          uint16_t beacon_airtime_pct_x100,
                          const char* active_template,
                          uint32_t beacon_sends,
                          uint8_t beacon_set_size,
                          uint16_t trouble_mask) {
  char topic[192];
  build_topic(topic, sizeof(topic), "beacon");
  char body[256];
  const int n = snprintf(body, sizeof(body),
    "{"
      "\"state\":\"%s\","
      "\"beacon_airtime_pct_x100\":%u,"
      "\"active_template\":\"%s\","
      "\"beacon_sends\":%lu,"
      "\"beacon_set_size\":%u,"
      "\"trouble_mask\":%u"
    "}",
    state_name ? state_name : "unknown",
    (unsigned)beacon_airtime_pct_x100,
    active_template ? active_template : "",
    (unsigned long)beacon_sends,
    (unsigned)beacon_set_size,
    (unsigned)trouble_mask);
  if (n <= 0 || (size_t)n >= sizeof(body)) return;
  publish_raw(topic, body, (size_t)n, /*retain=*/true);
}

void publish_mesh(uint16_t airtime_pct_x100,
                  uint8_t  channel,
                  bool     locked_to_sta,
                  bool     locked_to_ap,
                  bool     fallback,
                  uint32_t routine_allowed,
                  uint32_t routine_denied,
                  uint32_t urgent_sends) {
  char topic[192];
  build_topic(topic, sizeof(topic), "mesh");
  /* All fields are denormalized into one JSON blob so HA can build
   * value_template expressions without cross-topic state. Retained so
   * a freshly-subscribing HA picks up the latest snapshot regardless
   * of when it comes online. */
  char body[224];
  const int n = snprintf(body, sizeof(body),
    "{"
      "\"airtime_pct_x100\":%u,"
      "\"channel\":%u,"
      "\"locked_to_sta\":%s,"
      "\"locked_to_ap\":%s,"
      "\"fallback\":%s,"
      "\"routine_allowed\":%lu,"
      "\"routine_denied\":%lu,"
      "\"urgent_sends\":%lu"
    "}",
    (unsigned)airtime_pct_x100,
    (unsigned)channel,
    locked_to_sta ? "true" : "false",
    locked_to_ap ? "true" : "false",
    fallback ? "true" : "false",
    (unsigned long)routine_allowed,
    (unsigned long)routine_denied,
    (unsigned long)urgent_sends);
  if (n <= 0 || (size_t)n >= sizeof(body)) return;
  publish_raw(topic, body, (size_t)n, /*retain=*/true);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Home Assistant MQTT auto-discovery
 *
 * Published once per CONNECTED so a freshly-subscribing HA picks up the
 * entire entity set in one round-trip. Discovery topics live on
 *   homeassistant/{component}/canary_{device_id}/{object_id}/config
 * with retain=true. State topics stay on the existing
 * securacv/{prefix}/{device_id}/{topic} surface — discovery just tells
 * HA which value_template to extract from each.
 *
 * Abbreviated keys (uniq_id, stat_t, val_tpl, dev, ids, mf, mdl, sw)
 * trim ~30% off the wire size vs the long forms; HA accepts both.
 * Source of truth for the abbreviation table:
 *   https://www.home-assistant.io/integrations/mqtt/#discovery-payload
 *
 * Entity set (12 entities) — chosen so a stock HA dashboard surfaces:
 *   - "Is someone home?"        (binary_sensor.presence, motion class)
 *   - "Is the canary online?"   (binary_sensor.online, connectivity class)
 *   - Live presence state name  (sensor.state)
 *   - Motion / breathing scores (sensors with measurement state_class)
 *   - Heart-rate-like BPM       (sensor.bpm)
 *   - Confidence level          (sensor.confidence)
 *   - Witness count + chain     (sensor.witness_count / chain_length,
 *                                total_increasing state class)
 *   - Health: uptime + free heap + RSSI
 * ────────────────────────────────────────────────────────────────────────── */

namespace {

constexpr const char* DISCOVERY_PREFIX = "homeassistant";

/* Single-row description for one HA entity. Strings live in flash so
 * the table costs ~0 RAM. Optional fields use nullptr; the emitter
 * inserts the corresponding key only when non-null. */
struct DiscoveryEntity {
  const char* component;       /* "sensor" / "binary_sensor" */
  const char* object_id;       /* unique within device, alphanumeric+_ */
  const char* name;            /* HA entity friendly name */
  const char* state_topic;     /* suffix appended to securacv/<id>/ */
  const char* val_tpl;         /* Jinja over the JSON state payload */
  const char* unit;            /* nullable unit_of_measurement */
  const char* dev_class;       /* nullable device_class */
  const char* state_class;     /* nullable state_class */
  const char* icon;            /* nullable mdi: icon */
};

const DiscoveryEntity ENTITIES[] = {
  /* Binary sensors — surfaced in the headline HA card. */
  /* Presence keys on the PRESENCE vocabulary, never on "any non-empty
   * state": the events topic also carries acoustic anomalies and (since
   * the system.integrity module) tamper kinds like "watchdog" — and the
   * old any-state template rendered a brownout reboot as motion ON, a
   * false someone-is-home through every HomeKit-bridged automation. A
   * state outside the allowlist renders neither payload, which an MQTT
   * binary_sensor IGNORES (state unchanged) — an alarm or a tamper says
   * nothing about whether the room is occupied, so it must not move
   * this entity in either direction. */
  { "binary_sensor", "presence", "Presence", "events",
    "{% if value_json.state in ['active','subtle','quiet','together',"
    "'breathing_nearby','breathing_lost'] %}ON"
    "{% elif value_json.state == 'empty' %}OFF{% endif %}",
    nullptr, "motion", nullptr, "mdi:account" },
  { "binary_sensor", "online", "Online", "status",
    "{% if value_json.online %}ON{% else %}OFF{% endif %}",
    nullptr, "connectivity", nullptr, nullptr },

  /* Live state from /events. */
  { "sensor", "state", "State", "events",
    "{{ value_json.state | default('unknown') }}",
    nullptr, nullptr, nullptr, "mdi:eye-outline" },
  { "sensor", "confidence", "Confidence", "events",
    "{{ value_json.confidence | default('tentative') }}",
    nullptr, nullptr, nullptr, "mdi:gauge" },
  { "sensor", "motion", "Motion Score", "events",
    "{{ value_json.motion | default(0) }}",
    nullptr, nullptr, "measurement", "mdi:run" },
  { "sensor", "breathing", "Breathing Score", "events",
    "{{ value_json.breathing | default(0) }}",
    nullptr, nullptr, "measurement", "mdi:lungs" },
  { "sensor", "bpm", "Breathing Rate", "events",
    "{{ value_json.bpm | default(0) }}",
    "bpm", nullptr, "measurement", "mdi:heart-pulse" },

  /* Witness chain + counts (total_increasing matches HA's "this only
   * goes up" semantic so the long-term graph integrates correctly). */
  { "sensor", "witness_count", "Witness Records", "counts",
    "{{ value_json.total | default(0) }}",
    nullptr, nullptr, "total_increasing", "mdi:counter" },
  { "sensor", "chain_length", "Chain Length", "chain",
    "{{ value_json.length | default(0) }}",
    "blocks", nullptr, "total_increasing", "mdi:link-variant" },

  /* Device health. */
  { "sensor", "uptime", "Uptime", "health",
    "{{ value_json.uptime | default(0) }}",
    "s", "duration", "measurement", nullptr },
  { "sensor", "memory_free", "Free Memory", "health",
    "{{ value_json.memory_free | default(0) }}",
    "B", "data_size", "measurement", nullptr },
  { "sensor", "rssi", "Signal Strength", "status",
    "{{ value_json.rssi | default(0) }}",
    "dBm", "signal_strength", "measurement", nullptr },

  /* Mesh coexistence — surfaces the airtime governor + channel policy
   * decision so installers can see in HA that the multi-Canary mesh is
   * (a) following the home WiFi channel and (b) staying under the
   * airtime cap. airtime_pct is the rolling 10-second utilization
   * scaled out of x100 (215 → 2.15%). HA renders one decimal which
   * matches the granularity the governor reports.
   * channel_locked_to_sta is a binary_sensor (on = "channel is
   * following your home WiFi", the only behavior installers should
   * see steady-state on a working deployment).
   * See docs/network_coexistence.md for the policy + verification. */
  { "sensor", "mesh_airtime_pct", "Mesh Airtime", "mesh",
    "{{ (value_json.airtime_pct_x100 | default(0)) / 100 }}",
    "%", nullptr, "measurement", "mdi:radio-tower" },
  { "sensor", "mesh_channel", "Mesh Channel", "mesh",
    "{{ value_json.channel | default(0) }}",
    nullptr, nullptr, "measurement", "mdi:wifi-cog" },
  { "binary_sensor", "mesh_channel_locked_to_sta", "Mesh Follows Home WiFi", "mesh",
    "{% if value_json.locked_to_sta %}ON{% else %}OFF{% endif %}",
    nullptr, nullptr, nullptr, "mdi:wifi-check" },

  /* Chirp + Beacon NFPA-72-style state surfaces. See
   * spec/beacon_channel_v0.md §7.2 and spec/chirp_channel_v0.md §15.
   * Reported as a string enum: Normal | Trouble | Alarm | Supervisory.
   * HA can attach automations on Alarm (treat like a tamper) and
   * Trouble (treat like a warning, not an alert). */
  { "sensor", "chirp_state", "Chirp Channel State", "chirp",
    "{{ value_json.state | default('Normal') }}",
    nullptr, nullptr, nullptr, "mdi:bell-ring-outline" },
  { "sensor", "beacon_state", "Beacon Channel State", "beacon",
    "{{ value_json.state | default('Normal') }}",
    nullptr, nullptr, nullptr, "mdi:shield-alert-outline" },
  { "sensor", "beacon_airtime_pct", "Beacon Airtime", "beacon",
    "{{ (value_json.beacon_airtime_pct_x100 | default(0)) / 100 }}",
    "%", nullptr, "measurement", "mdi:radio-tower" },
  { "sensor", "beacon_active_template", "Beacon Active Alarm", "beacon",
    "{{ value_json.active_template | default('') }}",
    nullptr, nullptr, nullptr, "mdi:alarm-light-outline" },

#if FEATURE_ACOUSTIC_EVENTS
  /* Acoustic events — ride the retained /sensing topic, edge-triggered
   * via the `acoustic_event` string. The main loop clears the string
   * back to "none" after a 30 s hold, which flips these binary sensors
   * OFF — same contract as the canary core's securacv_mqtt. */
  { "binary_sensor", "smoke_alarm", "Smoke Alarm Heard", "sensing",
    "{{ 'ON' if value_json.acoustic_event == 'smoke_alarm_t3' else 'OFF' }}",
    nullptr, "smoke", nullptr, nullptr },
  { "binary_sensor", "co_alarm", "CO Alarm Heard", "sensing",
    "{{ 'ON' if value_json.acoustic_event == 'co_alarm_t4' else 'OFF' }}",
    nullptr, "carbon_monoxide", nullptr, nullptr },
#if FEATURE_ACOUSTIC_TRANSIENTS
  { "binary_sensor", "knock", "Knock Detected", "sensing",
    "{{ 'ON' if value_json.acoustic_event == 'knock' else 'OFF' }}",
    nullptr, "sound", nullptr, "mdi:door" },
  { "binary_sensor", "doorbell", "Doorbell Detected", "sensing",
    "{{ 'ON' if value_json.acoustic_event == 'doorbell' else 'OFF' }}",
    nullptr, "sound", nullptr, "mdi:bell-ring" },
  { "binary_sensor", "glass_break", "Glass Break Detected", "sensing",
    "{{ 'ON' if value_json.acoustic_event == 'glass_break' else 'OFF' }}",
    nullptr, "sound", nullptr, "mdi:image-broken-variant" },
#endif
#endif
};

/* Emit one entity's config payload. Returns true on enqueue success.
 * The discovery JSON is built into a fixed 768-byte buffer; current
 * worst-case payload is ~620 bytes (state-topic entity with full
 * device block + availability), so 768 leaves a comfortable margin. */
bool publish_one_discovery(const DiscoveryEntity& e) {
  const char* prefix = s_active_cfg.prefix[0] ? s_active_cfg.prefix : DEFAULT_PREFIX;

  /* Topic: homeassistant/{component}/canary_<device_id>/{object_id}/config.
   * Validate against truncation — a clipped topic would publish to a
   * different path than HA expects to subscribe on, silently breaking
   * the entity (PR #396 review r3213930617). 192 bytes covers the
   * worst case (DISCOVERY_PREFIX 13 + component 13 + "canary_" 7 +
   * device_id 32 + "/" + object_id 16 + "/config" 7 ≈ 90), so any
   * truncation here is a programming error worth bailing on. */
  char topic[192];
  const int tn = snprintf(topic, sizeof(topic),
           "%s/%s/canary_%s/%s/config",
           DISCOVERY_PREFIX, e.component, s_device_id, e.object_id);
  if (tn <= 0 || (size_t)tn >= sizeof(topic)) return false;

  /* Build optional-field segments first so the final snprintf is one
   * call (snprintf into a moving cursor would be more bytes of code
   * for very little gain). */
  char unit_kv[40]      = "";
  char dev_class_kv[48] = "";
  char state_class_kv[48] = "";
  char icon_kv[48]      = "";
  if (e.unit)        snprintf(unit_kv,        sizeof(unit_kv),        ",\"unit_of_meas\":\"%s\"", e.unit);
  if (e.dev_class)   snprintf(dev_class_kv,   sizeof(dev_class_kv),   ",\"dev_cla\":\"%s\"",      e.dev_class);
  if (e.state_class) snprintf(state_class_kv, sizeof(state_class_kv), ",\"stat_cla\":\"%s\"",     e.state_class);
  if (e.icon)        snprintf(icon_kv,        sizeof(icon_kv),        ",\"ic\":\"%s\"",           e.icon);

  char body[768];
  const int n = snprintf(body, sizeof(body),
    "{"
      "\"name\":\"%s\","
      "\"uniq_id\":\"canary_%s_%s\","
      "\"stat_t\":\"%s/%s/%s\","
      "\"val_tpl\":\"%s\","
      "\"avty_t\":\"%s/%s/status\","
      "\"avty_tpl\":\"{{ 'online' if value_json.online else 'offline' }}\""
      "%s%s%s%s,"
      "\"dev\":{"
        "\"ids\":[\"canary_%s\"],"
        "\"name\":\"Canary %s\","
        "\"mf\":\"SecuraCV\","
        "\"mdl\":\"Canary WAP\","
        "\"sw\":\"%s\""
      "}"
    "}",
    e.name,
    s_device_id, e.object_id,
    prefix, s_device_id, e.state_topic,
    e.val_tpl,
    prefix, s_device_id,
    unit_kv, dev_class_kv, state_class_kv, icon_kv,
    s_device_id,
    s_device_id,
    s_firmware_version);
  if (n <= 0 || (size_t)n >= sizeof(body)) return false;
  return publish_raw(topic, body, (size_t)n, /*retain=*/true);
}

/* Emit the firmware `update` entity + the auto-update `switch`. These
 * need command topics, which DiscoveryEntity doesn't model, so they get
 * a dedicated builder with the same wire-shape principles (abbreviated
 * keys, availability gate, full device block, truncation bail-out). */
bool publish_update_discovery() {
  const char* prefix = s_active_cfg.prefix[0] ? s_active_cfg.prefix : DEFAULT_PREFIX;

  char topic[192];
  char body[768];

  /* HA `update` entity: installed/latest version with release notes and
   * an Install button; progress reported via the JSON state payload. */
  int tn = snprintf(topic, sizeof(topic),
                    "%s/update/canary_%s/firmware/config",
                    DISCOVERY_PREFIX, s_device_id);
  if (tn <= 0 || (size_t)tn >= sizeof(topic)) return false;
  int n = snprintf(body, sizeof(body),
    "{"
      "\"name\":\"Firmware\","
      "\"uniq_id\":\"canary_%s_firmware\","
      "\"stat_t\":\"%s/%s/update/state\","
      "\"cmd_t\":\"%s/%s/update/cmd\","
      "\"pl_inst\":\"install\","
      "\"dev_cla\":\"firmware\","
      "\"avty_t\":\"%s/%s/status\","
      "\"avty_tpl\":\"{{ 'online' if value_json.online else 'offline' }}\","
      "\"dev\":{"
        "\"ids\":[\"canary_%s\"],"
        "\"name\":\"Canary %s\","
        "\"mf\":\"SecuraCV\","
        "\"mdl\":\"Canary WAP\","
        "\"sw\":\"%s\""
      "}"
    "}",
    s_device_id,
    prefix, s_device_id,
    prefix, s_device_id,
    prefix, s_device_id,
    s_device_id, s_device_id, s_firmware_version);
  if (n <= 0 || (size_t)n >= sizeof(body)) return false;
  if (!publish_raw(topic, body, (size_t)n, /*retain=*/true)) return false;

  /* Auto-update opt-in switch. Off by default — a witness device should
   * not reboot unattended unless its owner chose that. */
  tn = snprintf(topic, sizeof(topic),
                "%s/switch/canary_%s/auto_update/config",
                DISCOVERY_PREFIX, s_device_id);
  if (tn <= 0 || (size_t)tn >= sizeof(topic)) return false;
  n = snprintf(body, sizeof(body),
    "{"
      "\"name\":\"Auto Update\","
      "\"uniq_id\":\"canary_%s_auto_update\","
      "\"stat_t\":\"%s/%s/update/auto\","
      "\"cmd_t\":\"%s/%s/update/auto/cmd\","
      "\"ic\":\"mdi:update\","
      "\"ent_cat\":\"config\","
      "\"avty_t\":\"%s/%s/status\","
      "\"avty_tpl\":\"{{ 'online' if value_json.online else 'offline' }}\","
      "\"dev\":{"
        "\"ids\":[\"canary_%s\"],"
        "\"name\":\"Canary %s\","
        "\"mf\":\"SecuraCV\","
        "\"mdl\":\"Canary WAP\","
        "\"sw\":\"%s\""
      "}"
    "}",
    s_device_id,
    prefix, s_device_id,
    prefix, s_device_id,
    prefix, s_device_id,
    s_device_id, s_device_id, s_firmware_version);
  if (n <= 0 || (size_t)n >= sizeof(body)) return false;
  return publish_raw(topic, body, (size_t)n, /*retain=*/true);
}

#if FEATURE_ACOUSTIC_EVENTS
/* Mic hard-mute switch. Same command-topic shape as the auto-update
 * switch; payload vocabulary matches the canary core's securacv_mqtt
 * (HA "switch ON" = muted, state strings "muted"/"live") so existing
 * automations written against the core firmware carry over. */
bool publish_mic_discovery() {
  const char* prefix = s_active_cfg.prefix[0] ? s_active_cfg.prefix : DEFAULT_PREFIX;

  char topic[192];
  const int tn = snprintf(topic, sizeof(topic),
                          "%s/switch/canary_%s/mic_mute/config",
                          DISCOVERY_PREFIX, s_device_id);
  if (tn <= 0 || (size_t)tn >= sizeof(topic)) return false;

  char body[768];
  const int n = snprintf(body, sizeof(body),
    "{"
      "\"name\":\"Microphone Mute\","
      "\"uniq_id\":\"canary_%s_mic_mute\","
      "\"stat_t\":\"%s/%s/mic/state\","
      "\"cmd_t\":\"%s/%s/mic/cmd\","
      "\"pl_on\":\"mute\","
      "\"pl_off\":\"unmute\","
      "\"stat_on\":\"muted\","
      "\"stat_off\":\"live\","
      "\"ic\":\"mdi:microphone-off\","
      "\"ent_cat\":\"config\","
      "\"avty_t\":\"%s/%s/status\","
      "\"avty_tpl\":\"{{ 'online' if value_json.online else 'offline' }}\","
      "\"dev\":{"
        "\"ids\":[\"canary_%s\"],"
        "\"name\":\"Canary %s\","
        "\"mf\":\"SecuraCV\","
        "\"mdl\":\"Canary WAP\","
        "\"sw\":\"%s\""
      "}"
    "}",
    s_device_id,
    prefix, s_device_id,
    prefix, s_device_id,
    prefix, s_device_id,
    s_device_id, s_device_id, s_firmware_version);
  if (n <= 0 || (size_t)n >= sizeof(body)) return false;
  return publish_raw(topic, body, (size_t)n, /*retain=*/true);
}
#endif  /* FEATURE_ACOUSTIC_EVENTS */

}  /* namespace */

/* Publish all discovery payloads. Called from MQTT_EVENT_CONNECTED
 * AFTER the online-status publish so HA sees the device as online
 * before the entity definitions reference its availability topic.
 * No-op when discovery is disabled in NVS.
 *
 * Bails on the first failed publish — if esp_mqtt's queue is full or
 * the broker dropped between the status publish and the discovery
 * burst, the remaining payloads almost certainly fail too, and
 * issuing them only burns task time and wifi airtime. The next
 * MQTT_EVENT_CONNECTED republishes the full set (retained, so HA
 * sees the latest version regardless), so an interrupted run is
 * self-healing. (PR #396 review r3213930620.) */
void publish_discovery() {
  if (!s_active_cfg.discovery) return;
  size_t ok = 0;
  for (const DiscoveryEntity& e : ENTITIES) {
    if (!publish_one_discovery(e)) break;
    ok++;
  }
  Serial.printf("[MQTT] discovery: %u/%u entities announced\n",
                (unsigned)ok, (unsigned)(sizeof(ENTITIES) / sizeof(ENTITIES[0])));
  /* Firmware update entity + auto-update switch (command topics, so
   * they don't fit the DiscoveryEntity table). */
  publish_update_discovery();
#if FEATURE_ACOUSTIC_EVENTS
  /* Mic hard-mute switch — command topic, same exception. */
  publish_mic_discovery();
#endif
  publish_triggers();
}

/* ──────────────────────────────────────────────────────────────────────────
 * HA Device Triggers
 *
 * Exposes "this happened" events that HA's automation editor surfaces
 * under the device's trigger picker, without users having to
 * hand-author value_template comparisons. Each trigger fires when a
 * matching event lands on a watched topic; HA users then drag-and-drop
 * the trigger into the visual automation builder.
 *
 * Schema lives at homeassistant/device_automation/canary_<id>/{trigger}/config
 * (retained). type/subtype is the HA convention — many entries share a
 * `type` so they're grouped under one heading in the UI dropdown.
 *
 * Source of truth for the schema:
 *   https://www.home-assistant.io/integrations/device_trigger.mqtt/
 * ────────────────────────────────────────────────────────────────────────── */

namespace {

struct DiscoveryTrigger {
  const char* trigger_id;       /* unique within device, used in the topic */
  const char* type;             /* HA convention; groups related triggers */
  const char* subtype;          /* HA convention; the specific variant */
  const char* state_topic;      /* suffix appended to securacv/<id>/ */
  const char* val_tpl;          /* Jinja that distills the JSON to a scalar */
  const char* payload;          /* trigger fires when val_tpl == this */
};

/* Every trigger's value_template gates on `not value_json.replay` so
 * MQTT-reconnect backfill traffic doesn't re-fire user automations
 * (PR #398 review r3214114357). When the gate fails, value_template
 * returns None and HA's MQTT trigger never matches its payload —
 * sensors and binary_sensors that ignore the replay flag still see
 * the up-to-date state (HA's history view fills correctly). */
const DiscoveryTrigger TRIGGERS[] = {
  /* Presence-state transitions — the stuff users actually want to wire
   * automations against ("turn on porch light when room becomes
   * empty", "alert me if presence detected after midnight", etc.). */
  { "presence_active",   "presence_changed", "active",   "events",
    "{% if not value_json.replay %}{{ value_json.state }}{% endif %}", "active" },
  { "presence_subtle",   "presence_changed", "subtle",   "events",
    "{% if not value_json.replay %}{{ value_json.state }}{% endif %}", "subtle" },
  { "presence_quiet",    "presence_changed", "quiet",    "events",
    "{% if not value_json.replay %}{{ value_json.state }}{% endif %}", "quiet" },
  { "presence_together", "presence_changed", "together", "events",
    "{% if not value_json.replay %}{{ value_json.state }}{% endif %}", "together" },
  { "presence_empty",    "presence_changed", "empty",    "events",
    "{% if not value_json.replay %}{{ value_json.state }}{% endif %}", "empty" },

  /* Anomaly category fires whenever the chokepoint commits an anomaly
   * event regardless of state. Useful for security automations
   * (notify on unexpected motion in a quiet hour). */
  { "anomaly",           "anomaly",          "any",      "events",
    "{% if not value_json.replay %}{{ value_json.category }}{% endif %}", "anomaly" },
};

/* Emit one trigger config payload. Returns true on enqueue success.
 * Same wire shape principles as publish_one_discovery: abbreviated
 * keys, full device block, validated topic. */
bool publish_one_trigger(const DiscoveryTrigger& t) {
  const char* prefix = s_active_cfg.prefix[0] ? s_active_cfg.prefix : DEFAULT_PREFIX;

  char topic[192];
  const int tn = snprintf(topic, sizeof(topic),
           "%s/device_automation/canary_%s/%s/config",
           DISCOVERY_PREFIX, s_device_id, t.trigger_id);
  if (tn <= 0 || (size_t)tn >= sizeof(topic)) return false;

  /* Triggers don't need availability or unique_id (HA dedupes by
   * topic + type + subtype + device.ids). The payload is small;
   * 512 bytes is comfortably above the worst case (~280 bytes). */
  char body[512];
  const int n = snprintf(body, sizeof(body),
    "{"
      "\"automation_type\":\"trigger\","
      "\"type\":\"%s\","
      "\"subtype\":\"%s\","
      "\"topic\":\"%s/%s/%s\","
      "\"val_tpl\":\"%s\","
      "\"pl\":\"%s\","
      "\"dev\":{"
        "\"ids\":[\"canary_%s\"],"
        "\"name\":\"Canary %s\","
        "\"mf\":\"SecuraCV\","
        "\"mdl\":\"Canary WAP\","
        "\"sw\":\"%s\""
      "}"
    "}",
    t.type, t.subtype,
    prefix, s_device_id, t.state_topic,
    t.val_tpl, t.payload,
    s_device_id, s_device_id, s_firmware_version);
  if (n <= 0 || (size_t)n >= sizeof(body)) return false;
  return publish_raw(topic, body, (size_t)n, /*retain=*/true);
}

}  /* namespace */

void publish_triggers() {
  if (!s_active_cfg.discovery) return;
  size_t ok = 0;
  for (const DiscoveryTrigger& t : TRIGGERS) {
    if (!publish_one_trigger(t)) break;
    ok++;
  }
  Serial.printf("[MQTT] discovery: %u/%u triggers announced\n",
                (unsigned)ok, (unsigned)(sizeof(TRIGGERS) / sizeof(TRIGGERS[0])));
}

/* ──────────────────────────────────────────────────────────────────────────
 * Discovery removal
 *
 * When a user toggles discovery OFF post-install, the retained config
 * payloads we previously sent linger on the broker forever and HA
 * keeps showing the entities (eventually as "unavailable"). Publishing
 * an empty retained payload to the same topic is HA's documented
 * "remove this entity" signal — the broker evicts the retained
 * message and HA drops the entity.
 *
 * We always send removals on connect when discovery=false. There's no
 * harm in sending empty payloads to topics that have no retained
 * message (HA simply ignores them), and we don't need to track
 * "last published" state across reboots.
 * ────────────────────────────────────────────────────────────────────────── */

void remove_discovery() {
  /* Empty zero-byte payload = HA "remove entity" signal. Retained so
   * a freshly-connecting HA also sees the removal and drops any
   * cached entity definitions.
   *
   * Both loops bail on the first publish_raw failure — same reasoning
   * as publish_discovery: if esp_mqtt's queue is full or the broker
   * dropped, remaining publishes almost certainly fail too and just
   * burn task time + airtime (PR #398 review r3214113402). The next
   * MQTT_EVENT_CONNECTED with discovery=false re-attempts the full
   * removal sweep, so an interrupted run is self-healing. */
  size_t removed = 0;
  bool ok = true;
  for (const DiscoveryEntity& e : ENTITIES) {
    char topic[192];
    const int tn = snprintf(topic, sizeof(topic),
             "%s/%s/canary_%s/%s/config",
             DISCOVERY_PREFIX, e.component, s_device_id, e.object_id);
    if (tn <= 0 || (size_t)tn >= sizeof(topic)) continue;
    if (!publish_raw(topic, "", 0, /*retain=*/true)) { ok = false; break; }
    removed++;
  }
  if (ok) {
    for (const DiscoveryTrigger& t : TRIGGERS) {
      char topic[192];
      const int tn = snprintf(topic, sizeof(topic),
               "%s/device_automation/canary_%s/%s/config",
               DISCOVERY_PREFIX, s_device_id, t.trigger_id);
      if (tn <= 0 || (size_t)tn >= sizeof(topic)) continue;
      if (!publish_raw(topic, "", 0, /*retain=*/true)) { ok = false; break; }
      removed++;
    }
  }
  if (ok) {
    /* The firmware update entity + auto-update switch live outside the
     * ENTITIES table (they carry command topics) — evict them too. */
    char topic[192];
    int tn = snprintf(topic, sizeof(topic), "%s/update/canary_%s/firmware/config",
                      DISCOVERY_PREFIX, s_device_id);
    if (tn > 0 && (size_t)tn < sizeof(topic) &&
        publish_raw(topic, "", 0, /*retain=*/true)) {
      removed++;
      tn = snprintf(topic, sizeof(topic), "%s/switch/canary_%s/auto_update/config",
                    DISCOVERY_PREFIX, s_device_id);
      if (tn > 0 && (size_t)tn < sizeof(topic) &&
          publish_raw(topic, "", 0, /*retain=*/true)) {
        removed++;
      }
#if FEATURE_ACOUSTIC_EVENTS
      tn = snprintf(topic, sizeof(topic), "%s/switch/canary_%s/mic_mute/config",
                    DISCOVERY_PREFIX, s_device_id);
      if (tn > 0 && (size_t)tn < sizeof(topic) &&
          publish_raw(topic, "", 0, /*retain=*/true)) {
        removed++;
      }
#endif
    }
  }
  Serial.printf("[MQTT] discovery removal: %u entries cleared\n",
                (unsigned)removed);
}

/* ──────────────────────────────────────────────────────────────────────────
 * HTTP handlers
 *
 * The auth guard is delegated to a tiny wrapper so we don't pull
 * CSI_AUTH_OR_RETURN's macro definition (which lives inside
 * csi_integration.cpp's anonymous namespace) into this TU. Instead
 * we go through the Bearer + cookie surfaces directly the same way
 * the macro does; csi_integration::session_validate_cookie is
 * declared in csi_integration.h. ────────────────────────────────── */

static bool authorized(httpd_req_t* req, const char* api_token) {
  if (csi_integration::session_validate_cookie(req)) return true;
  if (api_token && api_auth_check_optional(req, api_token)) return true;
  return false;
}

/* The api_token for csi_mqtt's Bearer surface is the same one
 * /api/csi/* routes already check. We don't have direct access to
 * g_device.api_token_str from here, so the integration layer
 * registers an accessor at init time via set_api_token_provider. */
const char* (*s_api_token_provider)() = nullptr;

esp_err_t handle_config_get(httpd_req_t* req) {
  const char* tok = s_api_token_provider ? s_api_token_provider() : nullptr;
  if (!authorized(req, tok)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"securacv\"");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return ESP_OK;
  }
  Config c;
  config_load(&c);
  /* Echo everything but the password — the password is write-only on
   * the wire so a casual attacker who somehow obtained a session
   * cookie can't read the broker creds back out of GET. */
  char body[800];
  /* snprintf return value intentionally unchecked: we send via
   * HTTPD_RESP_USE_STRLEN below so a truncated payload still has a
   * NUL terminator and httpd_resp_send walks until it. Using
   * snprintf's return as the length would walk past the NUL on
   * truncation and over-read. (PR #394 review r3213674558.)
   * tls_mode / ca_set / transport / last_error are the broker-TLS
   * surface: the CA itself is never echoed (a page that can read it
   * back is a page that can be tricked into showing it), and last_error
   * is one of the header's constant strings — no quotes, no backslashes
   * (the host suite asserts that), so it is JSON-safe as-is. */
  snprintf(body, sizeof(body),
    "{"
      "\"enabled\":%s,"
      "\"host\":\"%s\","
      "\"port\":%u,"
      "\"user\":\"%s\","
      "\"prefix\":\"%s\","
      "\"tls\":%s,"
      "\"tls_mode\":%u,"
      "\"ca_set\":%s,"
      "\"transport\":\"%s\","
      "\"last_error\":\"%s\","
      "\"discovery\":%s,"
      "\"password_set\":%s,"
      "\"connected\":%s"
    "}",
    c.enabled ? "true" : "false",
    c.host,
    (unsigned)c.port,
    c.user,
    c.prefix,
    c.tls ? "true" : "false",
    (unsigned)c.tls_mode,
    c.ca_set ? "true" : "false",
    transport_name(),
    s_last_error,
    c.discovery ? "true" : "false",
    c.pass[0] ? "true" : "false",
    connected() ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* Find a JSON string field's value into out (NUL-terminated, truncated to
 * out_cap-1). Returns true if the key was present (even if empty).
 * Hand-rolled to keep ArduinoJson out of this TU — same shape the
 * existing /api/settings POST parser uses. */
static bool json_extract_string(const char* body, const char* key,
                                char* out, size_t out_cap) {
  if (!body || !key || !out || out_cap == 0) return false;
  const char* k = strstr(body, key);
  if (!k) return false;
  const char* colon = strchr(k, ':');
  if (!colon) return false;
  const char* p = colon + 1;
  while (*p == ' ' || *p == '\t') p++;
  if (*p != '"') return false;
  p++;  /* past opening quote */
  size_t i = 0;
  while (*p && *p != '"' && i < out_cap - 1) {
    /* Standard JSON string escapes — covers everything a broker host /
     * username / password / prefix could legally carry. \uXXXX is
     * intentionally NOT decoded: those fields are ASCII in practice
     * and adding UTF-16 surrogate handling for one corner case isn't
     * worth the parser surface (PR #394 review r3213674569). An
     * unrecognized escape passes the next char through literally —
     * matches how the existing /api/settings POST parser handles
     * nonsense, and avoids a silent reject for marginally-malformed
     * input. */
    if (*p == '\\' && p[1]) {
      char esc = p[1];
      switch (esc) {
        case '"':  out[i++] = '"';  p += 2; break;
        case '\\': out[i++] = '\\'; p += 2; break;
        case '/':  out[i++] = '/';  p += 2; break;
        case 'b':  out[i++] = '\b'; p += 2; break;
        case 'f':  out[i++] = '\f'; p += 2; break;
        case 'n':  out[i++] = '\n'; p += 2; break;
        case 'r':  out[i++] = '\r'; p += 2; break;
        case 't':  out[i++] = '\t'; p += 2; break;
        default:   out[i++] = esc;  p += 2; break;
      }
    } else {
      out[i++] = *p++;
    }
  }
  out[i] = '\0';
  return true;
}

static bool json_extract_bool(const char* body, const char* key, bool* out) {
  const char* k = strstr(body, key);
  if (!k) return false;
  const char* colon = strchr(k, ':');
  if (!colon) return false;
  const char* p = colon + 1;
  while (*p == ' ' || *p == '\t') p++;
  if (strncmp(p, "true",  4) == 0) { *out = true;  return true; }
  if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
  return false;
}

static bool json_extract_int(const char* body, const char* key, long* out) {
  const char* k = strstr(body, key);
  if (!k) return false;
  const char* colon = strchr(k, ':');
  if (!colon) return false;
  const char* p = colon + 1;
  while (*p == ' ' || *p == '\t' || *p == '"') p++;
  char* end = nullptr;
  long n = strtol(p, &end, 10);
  if (end == p) return false;
  *out = n;
  return true;
}

esp_err_t handle_config_post(httpd_req_t* req) {
  const char* tok = s_api_token_provider ? s_api_token_provider() : nullptr;
  if (!authorized(req, tok)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"securacv\"");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return ESP_OK;
  }

  /* Body cap covers all fields including a 128-char password, a
   * 128-char host and a JSON-escaped CA PEM (3071 bytes raw, newlines as
   * \n); anything longer is rejected as oversized rather than silently
   * truncated. Heap, not stack: 5 KB on the httpd task's stack is how a
   * settings save becomes a reboot. */
  struct HeapBuf {
    char* p;
    explicit HeapBuf(size_t n) : p((char*)malloc(n)) {}
    ~HeapBuf() { free(p); }
    HeapBuf(const HeapBuf&) = delete;
    HeapBuf& operator=(const HeapBuf&) = delete;
  };
  constexpr size_t kBodyCap = 640 + 2 * MAX_CA_LEN + 64;
  HeapBuf bodybuf(kBodyCap);
  HeapBuf cabuf(MAX_CA_LEN + 1);
  if (!bodybuf.p || !cabuf.p) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"out of memory\"}", -1);
    return ESP_OK;
  }
  char* body = bodybuf.p;
  const int got = httpd_req_recv(req, body, kBodyCap - 1);
  if (got <= 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"empty body\"}", -1);
    return ESP_OK;
  }
  body[got] = '\0';

  /* Start from current NVS so PATCH-style requests (just toggling
   * enabled, leaving everything else alone) work. */
  Config c;
  config_load(&c);

  json_extract_bool  (body, "\"enabled\"",   &c.enabled);
  json_extract_bool  (body, "\"discovery\"", &c.discovery);
  /* Broker TLS mode. The numeric mode is the contract
   * (mqtt_transport_logic.h); the legacy "tls" bool is still accepted
   * from older pages as on→CA / off→plain. Fingerprint pinning (2) is
   * refused here, at save time, with the reason — this transport cannot
   * honor it and a silent save would only surface as "won't connect". */
  {
    bool legacy_tls = c.tls;
    if (json_extract_bool(body, "\"tls\"", &legacy_tls)) {
      c.tls_mode = legacy_tls ? (uint8_t)canary::net::mqtt_tls::Mode::Ca : 0;
    }
    long mode_l = -1;
    if (json_extract_int(body, "\"tls_mode\"", &mode_l)) {
      if (mode_l == (long)canary::net::mqtt_tls::Mode::Fingerprint) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req,
          "{\"ok\":false,\"reason\":\"fingerprint pinning is not available on this device - use the CA mode\"}", -1);
        return ESP_OK;
      }
      if (mode_l == (long)canary::net::mqtt_tls::Mode::InsecureLab) {
        /* Same reason init() would refuse it (caps.insecure = false): the
         * core's esp-tls cannot skip verification, so saving the mode would
         * only surface later as "won't connect". */
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req,
          "{\"ok\":false,\"reason\":\"unverified TLS is not available on this build - use the CA mode\"}", -1);
        return ESP_OK;
      }
      bool known = false;
      (void)canary::net::mqtt_tls::mode_from_u8((uint8_t)(mode_l & 0xFF), &known);
      if (mode_l < 0 || mode_l > 255 || !known) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"ok\":false,\"reason\":\"tls_mode must be 0 (off) or 1 (CA)\"}", -1);
        return ESP_OK;
      }
      c.tls_mode = (uint8_t)mode_l;
    }
    c.tls = c.tls_mode != 0;
  }
  /* The CA: present-and-empty removes it, present-and-PEM replaces it,
   * absent leaves it. Shape-checked before it can reach NVS so a pasted
   * fingerprint or a half-copied PEM is a 400 here, not a -0x2180 later. */
  bool ca_changed = false;
  if (json_extract_string(body, "\"ca\"", cabuf.p, MAX_CA_LEN + 1)) {
    if (cabuf.p[0] != '\0' && !canary::net::mqtt_tls::ca_pem_looks_valid(cabuf.p)) {
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_send(req,
        "{\"ok\":false,\"reason\":\"ca must be a PEM certificate (the BEGIN/END CERTIFICATE lines included)\"}", -1);
      return ESP_OK;
    }
    ca_changed = true;
  }
  long port_l = c.port;
  if (json_extract_int(body, "\"port\"", &port_l) && port_l > 0 && port_l <= 65535) {
    c.port = (uint16_t)port_l;
  }
  /* Empty string in any field clears it; missing field leaves it as-is. */
  char buf[MAX_PASS_LEN + 1] = {};
  if (json_extract_string(body, "\"host\"",   buf, sizeof(buf))) {
    strncpy(c.host,   buf, MAX_HOST_LEN);   c.host  [MAX_HOST_LEN]   = '\0';
  }
  if (json_extract_string(body, "\"user\"",   buf, sizeof(buf))) {
    strncpy(c.user,   buf, MAX_USER_LEN);   c.user  [MAX_USER_LEN]   = '\0';
  }
  if (json_extract_string(body, "\"prefix\"", buf, sizeof(buf))) {
    /* Restrict prefix to a safe character set so we can splat it into
     * the discovery JSON's stat_t / avty_t fields via %s without an
     * escaper. JSON-unsafe (", \, control chars) would corrupt the
     * payload; MQTT wildcards (+, #) would make the topic match
     * unintended subscriptions; whitespace at edges trips brokers
     * that strip-then-compare. (PR #396 review r3213931334.) Empty
     * input falls through to the DEFAULT_PREFIX backfill below. */
    bool prefix_ok = (buf[0] != '\0');
    for (const char* p = buf; *p && prefix_ok; ++p) {
      const unsigned char ch = (unsigned char)*p;
      const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') ||
                      ch == '_' || ch == '-' || ch == '/';
      if (!ok) prefix_ok = false;
    }
    if (!prefix_ok) {
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_send(req,
        "{\"ok\":false,\"reason\":\"prefix must be [a-zA-Z0-9_/-]+\"}", -1);
      return ESP_OK;
    }
    strncpy(c.prefix, buf, MAX_PREFIX_LEN); c.prefix[MAX_PREFIX_LEN] = '\0';
  }
  if (json_extract_string(body, "\"password\"", buf, sizeof(buf))) {
    strncpy(c.pass,   buf, MAX_PASS_LEN);   c.pass  [MAX_PASS_LEN]   = '\0';
  }
  if (!c.prefix[0]) strncpy(c.prefix, DEFAULT_PREFIX, MAX_PREFIX_LEN);
  if (c.port == 0)  c.port = c.tls ? DEFAULT_PORT_TLS : DEFAULT_PORT;

  if (!config_save(c) || (ca_changed && !ca_save(cabuf.p))) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"nvs unavailable\"}", -1);
    return ESP_OK;
  }

  /* The new credentials take effect on the loop task, which tears the old
   * client down and opens the new one (sweep F106: init() here destroyed
   * the client under a loop-task publish). Wait a moment for it so the
   * page's status refresh sees the new client; the save stands either
   * way, and the re-init runs when the loop task gets to it. */
  (void)wait_reinit(request_reinit(), kReinitWaitMs);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"ok\":true}", -1);
  return ESP_OK;
}

esp_err_t handle_test(httpd_req_t* req) {
  const char* tok = s_api_token_provider ? s_api_token_provider() : nullptr;
  if (!authorized(req, tok)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"securacv\"");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return ESP_OK;
  }
  /* A fresh connect, run by the loop task (sweep F106: init() here
   * destroyed the client under a loop-task publish), then esp_mqtt's
   * async connect: s_connected flips, or an error event lands. Both
   * within the 4 s this always waited for the connect, polling with
   * delay() so the WiFi worker stays responsive. The old client's
   * s_connected is not read: only after the re-init has torn it down. */
  const uint32_t start = millis();
  const bool reinit = wait_reinit(request_reinit(), kTestBudgetMs);
  uint32_t waited = (uint32_t)(millis() - start);
  while (reinit && !s_connected.load(std::memory_order_relaxed) && waited < kTestBudgetMs) {
    delay(100);
    waited = (uint32_t)(millis() - start);
  }
  const bool ok = reinit && s_connected.load(std::memory_order_relaxed);
  httpd_resp_set_type(req, "application/json");
  char body[320];
  snprintf(body, sizeof(body),
    "{\"ok\":%s,\"connected\":%s,\"waited_ms\":%lu,\"transport\":\"%s\",\"last_error\":\"%s\"}",
    ok ? "true" : "false",
    ok ? "true" : "false",
    (unsigned long)waited,
    transport_name(),
    s_last_error);
  httpd_resp_send(req, body, -1);
  return ESP_OK;
}

/* Tiny settings-page UI served at /mqtt — auth-gated like /tune via
 * cookie. The form POSTs to /api/mqtt/config; the Test button hits
 * /api/mqtt/test. Microcopy stays plain (FKGL ≤ 6) per the lint trio. */
static const char MQTT_UI_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Canary &middot; MQTT</title>
<style>
body{font-family:system-ui,-apple-system,sans-serif;max-width:520px;margin:40px auto;padding:24px;background:#fffbec;color:#1a1605;line-height:1.5;}
h1{font-weight:500;font-size:24px;margin:0 0 8px;}
p{color:#3a311e;margin:8px 0 18px;}
form{display:grid;gap:14px;}
label{display:grid;gap:4px;font-size:13px;color:#6b6049;}
input[type=text],input[type=password],input[type=number],select,textarea{font:inherit;padding:10px;border:1px solid #d4c994;border-radius:8px;background:#fff;}
.pw-masked{-webkit-text-security:disc;text-security:disc;}
.row{display:grid;grid-template-columns:1fr 1fr;gap:12px;}
.row.toggle{grid-template-columns:auto auto;justify-content:start;align-items:center;gap:10px;}
.actions{display:flex;gap:10px;margin-top:12px;}
button{font:inherit;padding:10px 20px;border-radius:10px;border:0;cursor:pointer;}
button.save{background:#f0c319;color:#1a1605;font-weight:500;}
button.test{background:transparent;color:#3a311e;border:1px solid #d4c994;}
.status{font-size:13px;color:#6b6049;margin-top:8px;}
.status.good{color:#1a7a1a;}
.status.bad{color:#a02020;}
@media (prefers-color-scheme:dark){
  body{background:#1a1605;color:#fffbec;}
  p,label,.status{color:#a89e85;}
  input[type=text],input[type=password],input[type=number],select,textarea{background:#2a2310;color:#fffbec;border-color:#403718;}
  button.test{color:#fffbec;border-color:#403718;}
}
</style></head><body>
<h1>Send sensing to Home Assistant</h1>
<p>Set up the broker once. The canary will send presence and breathing notes there, and your existing SecuraCV add-on will pick them up.</p>
<form id="f">
  <label class="row toggle">
    <input type="checkbox" id="enabled"> Send to a broker
  </label>
  <div class="row">
    <label>Broker host<input type="text" id="host" placeholder="192.168.1.10"></label>
    <label>Port<input type="number" id="port" min="1" max="65535" value="1883"></label>
  </div>
  <div class="row">
    <label>Username (optional)<input type="text" id="user"></label>
    <label>Password (optional)<input type="text" class="pw-masked" id="password" placeholder="leave blank to keep" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false"></label>
  </div>
  <div class="row">
    <label>Topic prefix<input type="text" id="prefix" value="securacv"></label>
    <label>Encryption<select id="tls_mode">
      <option value="0">Off (plain, port 1883)</option>
      <option value="1">On, check the broker's certificate (port 8883)</option>
    </select></label>
  </div>
  <div class="row">
    <label>Broker CA certificate (PEM, for the "check" option)<textarea id="ca" rows="4" placeholder="-----BEGIN CERTIFICATE-----" spellcheck="false"></textarea></label>
  </div>
  <div class="status" id="ca_note"></div>
  <label class="row toggle">
    <input type="checkbox" id="ca_clear"> Forget the saved CA certificate
  </label>
  <label class="row toggle">
    <input type="checkbox" id="discovery" checked>
    Let Home Assistant find the canary on its own
  </label>
  <div class="actions">
    <button class="save" type="submit">Save</button>
    <button class="test" type="button" id="test">Test connection</button>
  </div>
  <div class="status" id="status"></div>
</form>
<script>
function cvFetch(url, opts){ return fetch(url, opts); }
async function load(){
  try {
    const r = await cvFetch('/api/mqtt/config', {cache:'no-store'});
    if (!r.ok) return;
    const j = await r.json();
    enabled.checked = !!j.enabled;
    host.value      = j.host || '';
    port.value      = j.port || 1883;
    user.value      = j.user || '';
    prefix.value    = j.prefix || 'securacv';
    tls_mode.value  = String(j.tls_mode != null ? j.tls_mode : (j.tls ? 1 : 0));
    discovery.checked = j.discovery !== false;  /* default-on if missing */
    if (j.password_set) password.placeholder = '•••• saved (leave blank to keep)';
    ca_note.textContent = j.ca_set
      ? 'A CA certificate is saved. Paste a new one to replace it.'
      : 'No CA certificate saved yet.';
    ca_clear.checked = false;
    const why = j.last_error ? ' ' + j.last_error : '';
    document.getElementById('status').textContent =
      (j.connected ? 'Connected to broker (' + j.transport + ').' : 'Not connected.') + why;
    document.getElementById('status').className = 'status ' + (j.connected ? 'good' : (why ? 'bad' : ''));
  } catch {}
}
load();
f.addEventListener('submit', async (e) => {
  e.preventDefault();
  const body = {
    enabled: enabled.checked,
    host: host.value.trim(),
    port: parseInt(port.value, 10) || 1883,
    user: user.value,
    prefix: prefix.value.trim() || 'securacv',
    tls_mode: parseInt(tls_mode.value, 10) || 0,
    discovery: discovery.checked,
  };
  if (password.value) body.password = password.value;
  if (ca_clear.checked) body.ca = '';
  else if (ca.value.trim()) body.ca = ca.value.trim() + '\n';
  const r = await cvFetch('/api/mqtt/config', {
    method: 'POST',
    headers: {'content-type': 'application/json'},
    body: JSON.stringify(body),
  });
  const ok = r.ok;
  const j = await r.json().catch(() => ({}));
  document.getElementById('status').textContent = ok ? 'Saved.' : ('Save failed. ' + (j.reason || ''));
  document.getElementById('status').className = 'status ' + (ok ? 'good' : 'bad');
  if (ok) { ca.value = ''; setTimeout(load, 600); }
});
document.getElementById('test').addEventListener('click', async () => {
  document.getElementById('status').textContent = 'Trying to reach the broker…';
  document.getElementById('status').className = 'status';
  const r = await cvFetch('/api/mqtt/test', {method: 'POST'});
  const j = await r.json().catch(() => ({}));
  document.getElementById('status').textContent = j.ok
    ? 'Reached the broker (' + j.transport + ').'
    : ('Could not reach the broker. ' + (j.last_error || 'Check host, port, and credentials.'));
  document.getElementById('status').className = 'status ' + (j.ok ? 'good' : 'bad');
});
</script>
</body></html>
)HTML";

esp_err_t handle_ui(httpd_req_t* req) {
  /* Top-level page navigation can't carry a Bearer header, so we gate
   * on the cv_session cookie (same pattern as handle_tune_page). A
   * visitor without a session lands on / first and pairs there. */
  if (!csi_integration::session_validate_cookie(req)) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
  }
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, MQTT_UI_HTML, HTTPD_RESP_USE_STRLEN);
}

/* Provided so csi_integration::init can register a token accessor at
 * boot without csi_mqtt needing to know about g_device. */
void set_api_token_provider(const char* (*fn)()) {
  s_api_token_provider = fn;
}

}  /* namespace csi_mqtt */
