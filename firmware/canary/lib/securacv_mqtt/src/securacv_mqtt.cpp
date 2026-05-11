/*
 * SecuraCV Canary — MQTT Publisher Implementation
 *
 * MQTT is an OPTIONAL transport. The device functions fully without it.
 * No raw pixel data or identity substrates are ever published.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_mqtt.h"

#if FEATURE_HA_MQTT

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "securacv_crypto.h"
#include "securacv_witness.h"
#include "log_level.h"

// ════════════════════════════════════════════════════════════════════════════
// INTERNAL STATE
// ════════════════════════════════════════════════════════════════════════════

static WiFiClient s_wifi_client;
static PubSubClient s_mqtt(s_wifi_client);

static MqttCredentials s_creds;
static char s_device_id[32];
static char s_firmware_version[16];
static char s_client_id[32];

// Topic buffers (built once at init)
static char s_topic_status[64];
static char s_topic_events[64];
static char s_topic_health[64];
static char s_topic_chain[64];
static char s_topic_tamper[64];
static char s_topic_transport[64];
static char s_topic_sensing[64];
static char s_topic_avail[64];

// Reconnect backoff
static uint32_t s_reconnect_delay_ms = MQTT_RECONNECT_MIN_MS;
static uint32_t s_last_reconnect_attempt = 0;
static bool s_initialized = false;
static bool s_discovery_sent = false;

// NVS keys for MQTT credentials
static const char* NVS_KEY_MQTT_HOST = "mqtt_host";
static const char* NVS_KEY_MQTT_PORT = "mqtt_port";
static const char* NVS_KEY_MQTT_USER = "mqtt_user";
static const char* NVS_KEY_MQTT_PASS = "mqtt_pass";
static const char* NVS_KEY_MQTT_EN   = "mqtt_en";

// ════════════════════════════════════════════════════════════════════════════
// INTERNAL HELPERS
// ════════════════════════════════════════════════════════════════════════════

static void build_topics(const char* device_id) {
  snprintf(s_topic_status,    sizeof(s_topic_status),    "securacv/%s/status",    device_id);
  snprintf(s_topic_events,    sizeof(s_topic_events),    "securacv/%s/events",    device_id);
  snprintf(s_topic_health,    sizeof(s_topic_health),    "securacv/%s/health",    device_id);
  snprintf(s_topic_chain,     sizeof(s_topic_chain),     "securacv/%s/chain",     device_id);
  snprintf(s_topic_tamper,    sizeof(s_topic_tamper),    "securacv/%s/tamper",    device_id);
  snprintf(s_topic_transport, sizeof(s_topic_transport), "securacv/%s/transport", device_id);
  snprintf(s_topic_sensing,   sizeof(s_topic_sensing),   "securacv/%s/sensing",   device_id);
  snprintf(s_topic_avail,     sizeof(s_topic_avail),     "securacv/%s/availability", device_id);
}

static void build_client_id() {
  // Use last 6 chars of device_id for unique client ID
  size_t len = strlen(s_device_id);
  const char* suffix = (len > 6) ? s_device_id + len - 6 : s_device_id;
  snprintf(s_client_id, sizeof(s_client_id), "securacv-canary-%s", suffix);
}

static bool attempt_connect() {
  if (!s_creds.configured || !s_creds.enabled) {
    return false;
  }

  // Need WiFi STA connection to reach broker
  if (!WiFi.isConnected()) {
    return false;
  }

  s_mqtt.setServer(s_creds.host, s_creds.port);
  s_mqtt.setKeepAlive(MQTT_KEEPALIVE_SEC);
  s_mqtt.setBufferSize(MQTT_BUFFER_SIZE);

  bool connected;
  if (strlen(s_creds.username) > 0) {
    connected = s_mqtt.connect(
      s_client_id,
      s_creds.username,
      s_creds.password,
      s_topic_avail,    // LWT topic
      1,                // LWT QoS
      true,             // LWT retain
      "offline"         // LWT payload
    );
  } else {
    connected = s_mqtt.connect(
      s_client_id,
      nullptr,
      nullptr,
      s_topic_avail,
      1,
      true,
      "offline"
    );
  }

  if (connected) {
    // Publish online availability (retained)
    s_mqtt.publish(s_topic_avail, "online", true);

    s_reconnect_delay_ms = MQTT_RECONNECT_MIN_MS;
    s_discovery_sent = false; // Re-send discovery on reconnect

    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "MQTT connected", s_creds.host);

    #if FEATURE_HA_DISCOVERY
    mqtt_send_ha_discovery(s_device_id, s_firmware_version);
    #endif

    return true;
  }

  // Exponential backoff
  s_reconnect_delay_ms *= 2;
  if (s_reconnect_delay_ms > MQTT_RECONNECT_MAX_MS) {
    s_reconnect_delay_ms = MQTT_RECONNECT_MAX_MS;
  }

  return false;
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════════════════

bool mqtt_init(const char* device_id, const char* firmware_version) {
  strncpy(s_device_id, device_id, sizeof(s_device_id) - 1);
  s_device_id[sizeof(s_device_id) - 1] = '\0';
  strncpy(s_firmware_version, firmware_version, sizeof(s_firmware_version) - 1);
  s_firmware_version[sizeof(s_firmware_version) - 1] = '\0';

  build_topics(device_id);
  build_client_id();

  // Load credentials from NVS
  if (!mqtt_load_credentials(&s_creds)) {
    // No credentials configured — MQTT stays disabled
    Serial.println("[MQTT] No credentials configured — disabled");
    s_initialized = true;
    return true; // Not an error — MQTT is optional
  }

  if (!s_creds.enabled) {
    Serial.println("[MQTT] Disabled by configuration");
    s_initialized = true;
    return true;
  }

  Serial.printf("[MQTT] Broker: %s:%u\n", s_creds.host, s_creds.port);
  s_initialized = true;
  return true;
}

void mqtt_loop() {
  if (!s_initialized || !s_creds.configured || !s_creds.enabled) {
    return;
  }

  if (s_mqtt.connected()) {
    s_mqtt.loop();
    return;
  }

  // Reconnect with exponential backoff
  uint32_t now = millis();
  if (now - s_last_reconnect_attempt < s_reconnect_delay_ms) {
    return;
  }
  s_last_reconnect_attempt = now;

  attempt_connect();
}

bool mqtt_connected() {
  return s_initialized && s_mqtt.connected();
}

void mqtt_disconnect() {
  if (s_mqtt.connected()) {
    // Publish offline (not retained — LWT handles permanent offline)
    s_mqtt.publish(s_topic_avail, "offline", true);
    s_mqtt.disconnect();
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "MQTT disconnected", nullptr);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// CREDENTIAL MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

bool mqtt_load_credentials(MqttCredentials* creds) {
  memset(creds, 0, sizeof(MqttCredentials));
  creds->port = MQTT_PORT;

  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly()) return false;

  size_t host_len = nvs.getBytesLength(NVS_KEY_MQTT_HOST);
  if (host_len == 0 || host_len >= sizeof(creds->host)) {
    nvs.end();
    return false;
  }

  nvs.getBytes(NVS_KEY_MQTT_HOST, creds->host, host_len);
  creds->host[host_len] = '\0';

  creds->port = (uint16_t)nvs.getUInt(NVS_KEY_MQTT_PORT, MQTT_PORT);

  size_t user_len = nvs.getBytesLength(NVS_KEY_MQTT_USER);
  if (user_len > 0 && user_len < sizeof(creds->username)) {
    nvs.getBytes(NVS_KEY_MQTT_USER, creds->username, user_len);
    creds->username[user_len] = '\0';
  }

  size_t pass_len = nvs.getBytesLength(NVS_KEY_MQTT_PASS);
  if (pass_len > 0 && pass_len < sizeof(creds->password)) {
    nvs.getBytes(NVS_KEY_MQTT_PASS, creds->password, pass_len);
    creds->password[pass_len] = '\0';
  }

  creds->enabled = nvs.getBool(NVS_KEY_MQTT_EN, true);
  creds->configured = (strlen(creds->host) > 0);

  nvs.end();
  return creds->configured;
}

bool mqtt_save_credentials(const MqttCredentials* creds) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;

  nvs.putBytes(NVS_KEY_MQTT_HOST, creds->host, strlen(creds->host));
  nvs.putUInt(NVS_KEY_MQTT_PORT, creds->port);

  if (strlen(creds->username) > 0) {
    nvs.putBytes(NVS_KEY_MQTT_USER, creds->username, strlen(creds->username));
  }
  if (strlen(creds->password) > 0) {
    nvs.putBytes(NVS_KEY_MQTT_PASS, creds->password, strlen(creds->password));
  }

  nvs.putBool(NVS_KEY_MQTT_EN, creds->enabled);
  nvs.end();

  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "MQTT credentials saved", creds->host);

  // Update live credentials
  memcpy(&s_creds, creds, sizeof(MqttCredentials));
  s_creds.configured = true;

  return true;
}

bool mqtt_clear_credentials() {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;

  nvs.remove(NVS_KEY_MQTT_HOST);
  nvs.remove(NVS_KEY_MQTT_PORT);
  nvs.remove(NVS_KEY_MQTT_USER);
  nvs.remove(NVS_KEY_MQTT_PASS);
  nvs.remove(NVS_KEY_MQTT_EN);
  nvs.end();

  memset(&s_creds, 0, sizeof(MqttCredentials));
  s_creds.port = MQTT_PORT;

  mqtt_disconnect();
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "MQTT credentials cleared", nullptr);
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLISHING
// ════════════════════════════════════════════════════════════════════════════

bool mqtt_publish_status(const char* json_payload) {
  if (!s_mqtt.connected()) return false;
  return s_mqtt.publish(s_topic_status, json_payload);
}

bool mqtt_publish_event(const char* json_payload) {
  if (!s_mqtt.connected()) return false;
  return s_mqtt.publish(s_topic_events, json_payload);
}

bool mqtt_publish_health(const char* json_payload) {
  if (!s_mqtt.connected()) return false;
  return s_mqtt.publish(s_topic_health, json_payload);
}

bool mqtt_publish_chain(const char* json_payload) {
  if (!s_mqtt.connected()) return false;
  return s_mqtt.publish(s_topic_chain, json_payload);
}

bool mqtt_publish_tamper(const char* json_payload) {
  if (!s_mqtt.connected()) return false;
  // NOTE: PubSubClient only supports QoS 0 for publishing.
  // Use retained=true so the last tamper alert persists on the broker
  // for subscribers that connect after the event.
  return s_mqtt.publish(s_topic_tamper, json_payload, true);
}

bool mqtt_publish_transport(const char* json_payload) {
  if (!s_mqtt.connected()) return false;
  return s_mqtt.publish(s_topic_transport, json_payload);
}

bool mqtt_publish_sensing(const char* json_payload) {
  if (!s_mqtt.connected()) return false;
  /* Retained so a Home Assistant restart re-acquires the latest
   * snapshot immediately rather than seeing "Unknown" until the
   * next publish interval. The payload is small (< 600 bytes) so
   * broker storage cost is negligible. */
  return s_mqtt.publish(s_topic_sensing, json_payload, true);
}

// ════════════════════════════════════════════════════════════════════════════
// HA MQTT DISCOVERY
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_HA_DISCOVERY

// Helper: build shared device JSON object for HA discovery
static void add_device_info(JsonObject& device, const char* device_id, const char* fw_version) {
  char identifier[48];
  snprintf(identifier, sizeof(identifier), "securacv_canary_%s", device_id);

  JsonArray ids = device["ids"].to<JsonArray>();
  ids.add(identifier);

  char name[48];
  snprintf(name, sizeof(name), "SecuraCV Canary %s", device_id);
  device["name"] = name;
  device["mf"] = "ERRERlabs";
  device["mdl"] = "Canary v1";
  device["sw"] = fw_version;
}

// Helper: publish a single HA discovery config message
static bool publish_discovery(const char* component, const char* device_id,
                               const char* object_id, const char* payload) {
  char topic[128];
  snprintf(topic, sizeof(topic), "homeassistant/%s/securacv_%s/%s/config",
           component, device_id, object_id);
  return s_mqtt.publish(topic, (const uint8_t*)payload, strlen(payload), true);
}

bool mqtt_send_ha_discovery(const char* device_id, const char* firmware_version) {
  if (!s_mqtt.connected()) return false;
  if (s_discovery_sent) return true;

  Serial.println("[MQTT] Sending HA Discovery config...");

  /* Track success across the ~22 entity-config publishes. PubSubClient's
   * publish() returns false if the broker is busy, the network is
   * unstable, or the buffer is full — silently failing here would leave
   * an entity stuck at "Unknown" in HA until the next reconnect. We
   * gate s_discovery_sent on `all_ok` so a transient broker hiccup
   * forces a clean retry on the next connect.
   *
   * Each entity block is wrapped in `if (all_ok)` so a mid-cycle
   * failure also skips the JsonDocument construction and serialisation
   * for the remaining entities — saves ~half a kilobyte of stack +
   * heap churn per skipped block when the broker drops out partway
   * through. (The `&&` short-circuit alone wasn't enough; it only
   * skipped the publish call, not the doc-build that came before it.) */
  bool all_ok = true;

  // ── Sensor: witness_count ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Witness Count";
    doc["stat_t"] = s_topic_status;
    doc["val_tpl"] = "{{ value_json.records_created }}";
    doc["unit_of_meas"] = "records";
    doc["stat_cla"] = "total_increasing";
    doc["ic"] = "mdi:counter";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_witness_count", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("sensor", device_id, "witness_count", payload.c_str());
  }

  // ── Sensor: chain_sequence ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Chain Sequence";
    doc["stat_t"] = s_topic_status;
    doc["val_tpl"] = "{{ value_json.seq }}";
    doc["ic"] = "mdi:link-variant";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_chain_seq", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("sensor", device_id, "chain_seq", payload.c_str());
  }

  // ── Sensor: uptime ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Uptime";
    doc["stat_t"] = s_topic_status;
    doc["val_tpl"] = "{{ value_json.uptime }}";
    doc["unit_of_meas"] = "s";
    doc["ic"] = "mdi:timer-outline";
    doc["ent_cat"] = "diagnostic";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_uptime", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("sensor", device_id, "uptime", payload.c_str());
  }

  // ── Sensor: free_heap ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Free Heap";
    doc["stat_t"] = s_topic_status;
    doc["val_tpl"] = "{{ value_json.free_heap }}";
    doc["unit_of_meas"] = "B";
    doc["ic"] = "mdi:memory";
    doc["ent_cat"] = "diagnostic";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_free_heap", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("sensor", device_id, "free_heap", payload.c_str());
  }

  // ── Sensor: gps_satellites ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "GPS Satellites";
    doc["stat_t"] = s_topic_status;
    doc["val_tpl"] = "{{ value_json.satellites }}";
    doc["ic"] = "mdi:satellite-variant";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_gps_sats", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("sensor", device_id, "gps_sats", payload.c_str());
  }

  // ── Sensor: firmware_version ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Firmware Version";
    doc["stat_t"] = s_topic_status;
    doc["val_tpl"] = "{{ value_json.firmware }}";
    doc["ic"] = "mdi:chip";
    doc["ent_cat"] = "diagnostic";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_fw_ver", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("sensor", device_id, "fw_ver", payload.c_str());
  }

  // ── Binary Sensor: online (via LWT) ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Online";
    doc["stat_t"] = s_topic_avail;
    doc["pl_on"] = "online";
    doc["pl_off"] = "offline";
    doc["dev_cla"] = "connectivity";
    doc["ic"] = "mdi:access-point-network";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_online", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("binary_sensor", device_id, "online", payload.c_str());
  }

  // ── Binary Sensor: chain_valid ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Chain Valid";
    doc["stat_t"] = s_topic_status;
    doc["val_tpl"] = "{{ 'ON' if value_json.chain_valid else 'OFF' }}";
    doc["ic"] = "mdi:shield-check";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_chain_valid", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("binary_sensor", device_id, "chain_valid", payload.c_str());
  }

  // ── Binary Sensor: tamper_detected ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Tamper Detected";
    doc["stat_t"] = s_topic_tamper;
    doc["val_tpl"] = "{{ 'ON' if value_json.active else 'OFF' }}";
    doc["dev_cla"] = "tamper";
    doc["ic"] = "mdi:shield-alert";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_tamper", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("binary_sensor", device_id, "tamper", payload.c_str());
  }

  // ── Binary Sensor: gps_fix ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "GPS Fix";
    doc["stat_t"] = s_topic_status;
    doc["val_tpl"] = "{{ 'ON' if value_json.gps_fix else 'OFF' }}";
    doc["ic"] = "mdi:crosshairs-gps";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_gps_fix", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("binary_sensor", device_id, "gps_fix", payload.c_str());
  }

  // ── Binary Sensor: sd_healthy ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "SD Card Healthy";
    doc["stat_t"] = s_topic_status;
    doc["val_tpl"] = "{{ 'ON' if value_json.sd_healthy else 'OFF' }}";
    doc["ic"] = "mdi:sd";
    doc["ent_cat"] = "diagnostic";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_sd_healthy", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("binary_sensor", device_id, "sd_healthy", payload.c_str());
  }

  // ════════════════════════════════════════════════════════════════════
  // SENSING entities (Phase 1–5) — sourced from the retained
  // securacv/{id}/sensing topic. All entities are emitted regardless
  // of which FEATURE_* flags the firmware was built with; entities
  // for missing sources stay at "Unknown" in HA, which the user can
  // hide. Saves the lib from needing per-feature compile gates.
  // ════════════════════════════════════════════════════════════════════

  // ── Sensor: activity_label (quiet / presence / motion / active) ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Activity";
    doc["stat_t"] = s_topic_sensing;
    doc["val_tpl"] = "{{ value_json.label | default('unknown') }}";
    doc["ic"] = "mdi:radar";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_activity", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("sensor", device_id, "activity", payload.c_str());
  }

  // ── Sensor: motion_score (0..100) ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Motion Score";
    doc["stat_t"] = s_topic_sensing;
    doc["val_tpl"] = "{{ value_json.motion | default(0) }}";
    doc["unit_of_meas"] = "%";
    doc["ic"] = "mdi:run";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_motion_score", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("sensor", device_id, "motion_score", payload.c_str());
  }

  // ── Sensor: breathing_score (0..100) ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Breathing Score";
    doc["stat_t"] = s_topic_sensing;
    doc["val_tpl"] = "{{ value_json.breathing | default(0) }}";
    doc["unit_of_meas"] = "%";
    doc["ic"] = "mdi:lungs";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_breathing_score", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("sensor", device_id, "breathing_score", payload.c_str());
  }

  // ── Binary Sensor: smoke_alarm (T3 cadence) ──
  // NOTE: device_class "smoke" is correct (HA renders the right icon /
  // automation surface), but the name MUST make clear this is a pattern
  // match of an EXISTING smoke alarm, not a UL-listed smoke detector. The
  // Canary cannot detect smoke directly. See docs/getting_started_canary.md.
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Pattern: Smoke Alarm Cadence";
    doc["stat_t"] = s_topic_sensing;
    doc["val_tpl"] =
      "{{ 'ON' if value_json.acoustic_event == 'smoke_alarm_t3' else 'OFF' }}";
    doc["dev_cla"] = "smoke";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_smoke_alarm", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("binary_sensor", device_id, "smoke_alarm", payload.c_str());
  }

  // ── Binary Sensor: co_alarm (T4 cadence) ──
  // Same naming rationale as the smoke entity above.
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Pattern: CO Alarm Cadence";
    doc["stat_t"] = s_topic_sensing;
    doc["val_tpl"] =
      "{{ 'ON' if value_json.acoustic_event == 'co_alarm_t4' else 'OFF' }}";
    doc["dev_cla"] = "carbon_monoxide";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_co_alarm", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("binary_sensor", device_id, "co_alarm", payload.c_str());
  }

  // ── Binary Sensor: silent_panic (touch long-press) ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Silent Panic";
    doc["stat_t"] = s_topic_sensing;
    doc["val_tpl"] =
      "{{ 'ON' if value_json.touch_event == 'silent_panic' else 'OFF' }}";
    doc["dev_cla"] = "safety";
    doc["ic"] = "mdi:alert-octagon";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_silent_panic", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("binary_sensor", device_id, "silent_panic", payload.c_str());
  }

  // ── Binary Sensor: enclosure_tamper (touch tamper OR temp drift) ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Enclosure Tamper";
    doc["stat_t"] = s_topic_sensing;
    doc["val_tpl"] =
      "{{ 'ON' if (value_json.touch_event == 'enclosure_tamper' or "
                  "value_json.temp_drift_active) else 'OFF' }}";
    doc["dev_cla"] = "tamper";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_enclosure_tamper", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("binary_sensor", device_id, "enclosure_tamper", payload.c_str());
  }

  // ── Sensor: ir_last_protocol (NEC/RC5/Sony/none) ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Last IR Protocol";
    doc["stat_t"] = s_topic_sensing;
    doc["val_tpl"] = "{{ value_json.ir_protocol | default('none') }}";
    doc["ic"] = "mdi:remote";
    doc["ent_cat"] = "diagnostic";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_ir_protocol", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("sensor", device_id, "ir_protocol", payload.c_str());
  }

  // ── Sensor: ir_hash_bucket (0..15, per-session salted) ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Last IR Bucket";
    doc["stat_t"] = s_topic_sensing;
    doc["val_tpl"] = "{{ value_json.ir_bucket | default(0) }}";
    doc["ic"] = "mdi:numeric";
    doc["ent_cat"] = "diagnostic";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_ir_bucket", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("sensor", device_id, "ir_bucket", payload.c_str());
  }

  // ── Sensor: wake_reason (cold_boot/timer/touch/ext0/ext1/ulp/gpio/other) ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Last Wake";
    doc["stat_t"] = s_topic_sensing;
    doc["val_tpl"] = "{{ value_json.wake_reason | default('cold_boot') }}";
    doc["ic"] = "mdi:power-sleep";
    doc["ent_cat"] = "diagnostic";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_wake_reason", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("sensor", device_id, "wake_reason", payload.c_str());
  }

  // ── Sensor: rssi_dbm (CSI window mean RSSI) ──
  if (all_ok) {
    JsonDocument doc;
    doc["name"] = "Sensing RSSI";
    doc["stat_t"] = s_topic_sensing;
    doc["val_tpl"] = "{{ value_json.rssi_dbm | default(0) }}";
    doc["unit_of_meas"] = "dBm";
    doc["dev_cla"] = "signal_strength";
    doc["ent_cat"] = "diagnostic";
    char uid[64];
    snprintf(uid, sizeof(uid), "securacv_%s_sensing_rssi", device_id);
    doc["uniq_id"] = uid;
    JsonObject dev = doc["dev"].to<JsonObject>();
    add_device_info(dev, device_id, firmware_version);
    String payload;
    serializeJson(doc, payload);
    all_ok = all_ok && publish_discovery("sensor", device_id, "sensing_rssi", payload.c_str());
  }

  /* Only latch the discovery flag if every publish succeeded; otherwise
   * leave it false so the next reconnect cycle retries. PubSubClient
   * publish() can fail on a busy broker, network blip, or full TX
   * buffer, and a half-sent discovery would leave HA entities stuck at
   * "Unknown" until the user toggled MQTT off and back on. */
  if (all_ok) {
    s_discovery_sent = true;
    Serial.println("[MQTT] HA Discovery config sent");
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "HA Discovery sent", nullptr);
  } else {
    Serial.println("[MQTT] HA Discovery: some entities failed to publish; will retry on reconnect");
    log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK,
               "HA Discovery partial — will retry", nullptr);
  }
  return all_ok;
}

#endif // FEATURE_HA_DISCOVERY

#endif // FEATURE_HA_MQTT
