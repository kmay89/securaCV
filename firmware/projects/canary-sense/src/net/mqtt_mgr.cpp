// src/net/mqtt_mgr.cpp
#include "canary/net/mqtt_mgr.h"

#include <Arduino.h>
#include <cstring>

#include <WiFi.h>
#include <PubSubClient.h>

#include "canary/config.h"
#include "canary/log.h"
#include "canary/runtime_config.h"  // NVS-backed identity + broker credentials
#include "canary/diagnostics.h"     // heap health for the status heartbeat
#include "canary/net/wifi_mgr.h"    // RSSI + link state
#include "canary/ha/ha_discovery.h"
#include "identity/device_pseudonym.h"  // MAC-free client-ID suffix (Invariant III)

namespace canary::net {

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);
static Topics g_topics{};
static bool discovery_done = false;

// Inbound firmware-update commands. The PubSubClient callback fires inside
// mqtt.loop() on the main task, but flash-cycle decisions belong to the OTA
// glue (ota_mgr) — the callback only latches these flags and ota_loop()
// drains them. Same pattern as the other Canary variants.
static volatile bool s_pending_install = false;
static volatile int s_pending_auto = -1;

// Cached retained payloads, republished on every reconnect so HA's update
// entity and auto-update switch never sit at "unknown" after a broker
// restart.
static char s_update_state_cache[640] = {0};
static bool s_update_state_set = false;
static int s_update_auto_cache = -1;

static bool token_at(const char* p, int n, const char* tok, int tok_len) {
  auto boundary = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '"' || c == '}' || c == '\0';
  };
  return n >= tok_len && memcmp(p, tok, tok_len) == 0 &&
         (n == tok_len || boundary(p[tok_len]));
}

static void on_mqtt_message(char* topic, uint8_t* payload, unsigned int len) {
  if (!topic || !payload) return;

  const bool is_install = (strcmp(topic, g_topics.update_cmd) == 0);
  const bool is_auto = (strcmp(topic, g_topics.update_auto_cmd) == 0);
  if (!is_install && !is_auto) return;

  // Trim leading whitespace/quotes; require a token boundary after the
  // match so a mangled payload can't trigger a flash cycle.
  const char* p = (const char*)payload;
  int n = (int)len;
  while (n > 0 && (*p == ' ' || *p == '\t' || *p == '"')) { p++; n--; }

  if (is_install) {
    if (token_at(p, n, "install", 7)) s_pending_install = true;
    return;
  }
  if (token_at(p, n, "ON", 2) || token_at(p, n, "on", 2)) {
    s_pending_auto = 1;
  } else if (token_at(p, n, "OFF", 3) || token_at(p, n, "off", 3)) {
    s_pending_auto = 0;
  }
}

bool take_pending_install() {
  if (!s_pending_install) return false;
  s_pending_install = false;
  return true;
}

int take_pending_auto() {
  const int v = s_pending_auto;
  s_pending_auto = -1;
  return v;
}

static bool publish_checked(const char* tag, const char* topic, const char* payload, bool retain) {
  const bool ok = mqtt.publish(topic, payload, retain);

  log_header(tag);
  // NEVER call Serial directly in libs here; use dbg_serial() so CI/targets can swap it.
  canary::dbg_serial().printf("%s => %s (retain=%s len=%u)\n",
                              topic,
                              ok ? "OK" : "FAIL",
                              retain ? "true" : "false",
                              (unsigned)strlen(payload));
  return ok;
}

void mqtt_init(const Topics& topics) {
  g_topics = topics;
  const auto& cfg = canary::cfg::get();
  mqtt.setServer(cfg.mqtt_host, cfg.mqtt_port);
  mqtt.setBufferSize(MQTT_BUFFER_BYTES);
  mqtt.setCallback(on_mqtt_message);
}

bool mqtt_connected() { return mqtt.connected(); }

void mqtt_loop() { mqtt.loop(); }

void publish_status_retained(const Topics& topics, const char* status) {
  const auto& d = canary::diag::get();
  char msg[384];
  snprintf(msg, sizeof(msg),
           "{"
           "\"device_id\":\"%s\","
           "\"device_type\":\"%s\","
           "\"status\":\"%s\","
           "\"ip\":\"%s\","
           "\"rssi\":%d,"
           "\"heap_free\":%lu,"
           "\"heap_min\":%lu,"
           "\"degraded\":\"%s\","
           "\"ts_ms\":%lu"
           "}",
           canary::cfg::get().device_id, DEVICE_TYPE, status,
           WiFi.localIP().toString().c_str(),
           wifi_rssi(),
           (unsigned long)d.free_heap,
           (unsigned long)d.min_heap,
           canary::diag::level_name(d.level),
           (unsigned long)ms_now());
  publish_checked("STATUS", topics.status, msg, true);
}

void publish_heartbeat(const Topics& topics, const SenseSnapshot& s) {
  const auto& d = canary::diag::get();
  char msg[384];
  snprintf(msg, sizeof(msg),
           "{"
           "\"device_id\":\"%s\","
           "\"device_type\":\"%s\","
           "\"status\":\"online\","
           "\"presence\":%s,"
           "\"radar_ok\":%s,"
           "\"rssi\":%d,"
           "\"heap_free\":%lu,"
           "\"heap_min\":%lu,"
           "\"degraded\":\"%s\","
           "\"ts_ms\":%lu"
           "}",
           canary::cfg::get().device_id, DEVICE_TYPE,
           s.present ? "true" : "false",
           s.radar_ok ? "true" : "false",
           wifi_rssi(),
           (unsigned long)d.free_heap,
           (unsigned long)d.min_heap,
           canary::diag::level_name(d.level),
           (unsigned long)ms_now());
  publish_checked("HEART", topics.status, msg, true);
}

void publish_state_retained(const Topics& topics, const SenseSnapshot& s) {
  // Lux: "null" when the BH1750 is absent so HA shows unknown, not a fake 0.
  char lux_val[16];
  if (s.lux < 0) snprintf(lux_val, sizeof(lux_val), "null");
  else           snprintf(lux_val, sizeof(lux_val), "%.1f", (double)s.lux);

#ifdef CANARY_SENSE_VITALS
  // BPM numerics are only meaningful while the breathing lock holds; publish
  // null otherwise so the P1 entities read unknown instead of a stale value.
  char breath_val[16], heart_val[16];
  if (s.bpm_valid) {
    snprintf(breath_val, sizeof(breath_val), "%u", (unsigned)s.breath_bpm);
    snprintf(heart_val, sizeof(heart_val), "%u", (unsigned)s.heart_bpm);
  } else {
    snprintf(breath_val, sizeof(breath_val), "null");
    snprintf(heart_val, sizeof(heart_val), "null");
  }
#endif

  char msg[768];
  snprintf(msg, sizeof(msg),
           "{"
           "\"device_id\":\"%s\","
           "\"device_type\":\"%s\","
           "\"presence\":%s,"
           "\"presence_state\":\"%s\","
           "\"occupants\":\"%s\","
           "\"range\":\"%s\","
           "\"radar_ok\":%s,"
           "\"frame_errors\":%lu,"
           "\"lux\":%s,"
#ifdef CANARY_SENSE_VITALS
           "\"breathing_locked\":%s,"
           "\"breath_bpm\":%s,"
           "\"heart_bpm\":%s,"
#endif
           "\"last_event\":\"%s\","
           "\"uptime_s\":%lu,"
           "\"ts_ms\":%lu"
           "}",
           canary::cfg::get().device_id, DEVICE_TYPE,
           s.present ? "true" : "false",
           s.presence,
           s.occupants,
           s.range,
           s.radar_ok ? "true" : "false",
           (unsigned long)s.frame_errors,
           lux_val,
#ifdef CANARY_SENSE_VITALS
           s.breathing_locked ? "true" : "false",
           breath_val,
           heart_val,
#endif
           s.last_event ? s.last_event : "boot",
           (unsigned long)s.uptime_s,
           (unsigned long)s.ts_ms);

  publish_checked("STATE", topics.state, msg, true);
}

void publish_event(const Topics& topics, const char* json_payload) {
  publish_checked("EVENT", topics.events, json_payload, false);
}

void ha_discovery_publish_once(const Topics& topics) {
  if (discovery_done) return;
  canary::ha::publish_discovery(mqtt, topics);
  discovery_done = true;
}

void mqtt_reconnect_blocking() {
  char lwtPayload[160];
  snprintf(lwtPayload, sizeof(lwtPayload),
           "{"
           "\"device_id\":\"%s\","
           "\"device_type\":\"%s\","
           "\"status\":\"offline\","
           "\"ts_ms\":0"
           "}",
           canary::cfg::get().device_id, DEVICE_TYPE);

  // Privacy (Invariant III): the MQTT client ID reaches the broker, so its unique
  // suffix is the salted device pseudonym — never the raw efuse MAC.
  char devid_hex[device_pseudonym::HEX_LEN + 1] = {0};
  device_pseudonym::device_id_hex(devid_hex, sizeof(devid_hex));

  const auto& cfg = canary::cfg::get();
  while (!mqtt.connected()) {
    // A dead WiFi link makes every broker attempt hopeless — hand control
    // back to the caller so wifi_loop()'s backoff/reboot supervision runs
    // instead of spinning here forever.
    if (!wifi_connected()) {
      log_line("MQTT", "WiFi is down — deferring broker reconnect.");
      return;
    }

    String clientId = String("securacv-") + cfg.device_id + "-" + devid_hex;

    log_header("MQTT");
    canary::dbg_serial().printf("Connecting %s:%u as %s ...\n", cfg.mqtt_host, cfg.mqtt_port, clientId.c_str());

    bool ok = false;
    if (cfg.mqtt_user[0] != '\0') {
      ok = mqtt.connect(clientId.c_str(), cfg.mqtt_user, cfg.mqtt_pass, g_topics.status, 1, true, lwtPayload);
    } else {
      ok = mqtt.connect(clientId.c_str(), nullptr, nullptr, g_topics.status, 1, true, lwtPayload);
    }

    if (!ok) {
      log_header("MQTT");
      canary::dbg_serial().printf("Connect FAIL rc=%d. Retry 1s\n", mqtt.state());
      delay(1000);
    }
  }

  log_line("MQTT", "Connected.");
  publish_status_retained(g_topics, "online");
  ha_discovery_publish_once(g_topics);

  // Firmware update entity: re-subscribe the command topics (the broker
  // may have dropped them) and reconcile the retained states.
  mqtt.subscribe(g_topics.update_cmd, 1);
  mqtt.subscribe(g_topics.update_auto_cmd, 1);
  if (s_update_state_set) {
    publish_checked("OTA", g_topics.update_state, s_update_state_cache, true);
  }
  if (s_update_auto_cache >= 0) {
    publish_checked("OTA", g_topics.update_auto,
                    s_update_auto_cache ? "ON" : "OFF", true);
  }
}

bool publish_update_state_retained(const Topics& topics, const char* json_payload) {
  if (!json_payload) return false;
  /* Cache first so the reconnect republish always has the latest snapshot,
   * even if the broker is down right now. */
  strncpy(s_update_state_cache, json_payload, sizeof(s_update_state_cache) - 1);
  s_update_state_cache[sizeof(s_update_state_cache) - 1] = '\0';
  s_update_state_set = true;
  if (!mqtt.connected()) return false;
  return publish_checked("OTA", topics.update_state, s_update_state_cache, true);
}

bool publish_update_auto_retained(const Topics& topics, bool enabled) {
  s_update_auto_cache = enabled ? 1 : 0;
  if (!mqtt.connected()) return false;
  return publish_checked("OTA", topics.update_auto, enabled ? "ON" : "OFF", true);
}

} // namespace canary::net
