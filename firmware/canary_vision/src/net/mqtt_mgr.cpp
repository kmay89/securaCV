#include "canary/net/mqtt_mgr.h"
#include "canary/config.h"
#include "canary/log.h"

#include <WiFi.h>
#include <PubSubClient.h>

#include "secrets/secrets.h"
#include "canary/ha/ha_discovery.h"

namespace canary::net {

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);
static Topics g_topics{};
static bool discovery_done=false;

static bool publish_checked(const char* tag, const char* topic, const char* payload, bool retain) {
  bool ok = mqtt.publish(topic, payload, retain);
  log_header(tag);
  Serial.printf("%s => %s (retain=%s len=%u)\n",
                topic,
                ok ? "OK" : "FAIL",
                retain ? "true" : "false",
                (unsigned)strlen(payload));
  return ok;
}

void mqtt_init(const Topics& topics) {
  g_topics = topics;
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(MQTT_BUFFER_BYTES);
}

bool mqtt_connected() { return mqtt.connected(); }

void mqtt_loop() { mqtt.loop(); }

void publish_status_retained(const Topics& topics, const char* status) {
  char msg[256];
  snprintf(msg, sizeof(msg),
    "{"
      "\"device_id\":\"%s\","
      "\"device_type\":\"%s\","
      "\"status\":\"%s\","
      "\"ip\":\"%s\","
      "\"ts_ms\":%lu"
    "}",
    DEVICE_ID, DEVICE_TYPE, status,
    WiFi.localIP().toString().c_str(),
    (unsigned long)ms_now()
  );
  publish_checked("STATUS", topics.status, msg, true);
}

void publish_heartbeat(const Topics& topics, const StateSnapshot& s) {
  char msg[256];
  snprintf(msg, sizeof(msg),
    "{"
      "\"device_id\":\"%s\","
      "\"device_type\":\"%s\","
      "\"status\":\"online\","
      "\"presence\":%s,"
      "\"dwelling\":%s,"
      "\"ts_ms\":%lu"
    "}",
    DEVICE_ID, DEVICE_TYPE,
    s.presence ? "true" : "false",
    s.dwelling ? "true" : "false",
    (unsigned long)ms_now()
  );
  publish_checked("HEART", topics.status, msg, true);
}

void publish_state_retained(const Topics& topics, const StateSnapshot& s) {
  char msg[768];
  snprintf(msg, sizeof(msg),
    "{"
      "\"device_id\":\"%s\","
      "\"device_type\":\"%s\","
      "\"presence\":%s,"
      "\"dwelling\":%s,"
      "\"presence_ms\":%lu,"
      "\"dwell_ms\":%lu,"
      "\"confidence\":%d,"
      "\"voxel\":{\"rows\":%u,\"cols\":%u,\"r\":%d,\"c\":%d},"
      "\"bbox\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},"
      "\"last_event\":\"%s\","
      "\"uptime_s\":%lu,"
      "\"ts_ms\":%lu"
    "}",
    DEVICE_ID, DEVICE_TYPE,
    s.presence ? "true" : "false",
    s.dwelling ? "true" : "false",
    (unsigned long)s.presence_ms,
    (unsigned long)s.dwell_ms,
    (int)s.confidence,
    (unsigned)s.voxel.rows, (unsigned)s.voxel.cols, s.voxel.r, s.voxel.c,
    s.bbox.x, s.bbox.y, s.bbox.w, s.bbox.h,
    s.last_event ? s.last_event : "boot",
    (unsigned long)s.uptime_s,
    (unsigned long)s.ts_ms
  );

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
    DEVICE_ID, DEVICE_TYPE
  );

  while (!mqtt.connected()) {
    String clientId = String("securacv-") + DEVICE_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

    log_header("MQTT");
    Serial.printf("Connecting %s:%u as %s ...\n", MQTT_HOST, MQTT_PORT, clientId.c_str());

    bool ok=false;
    if (MQTT_USER != nullptr && MQTT_PASS != nullptr) {
      ok = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS, g_topics.status, 1, true, lwtPayload);
    } else {
      ok = mqtt.connect(clientId.c_str(), nullptr, nullptr, g_topics.status, 1, true, lwtPayload);
    }

    if (!ok) {
      log_header("MQTT");
      Serial.printf("Connect FAIL rc=%d. Retry 1s\n", mqtt.state());
      delay(1000);
    }
  }

  log_line("MQTT", "Connected.");
  publish_status_retained(g_topics, "online");
  ha_discovery_publish_once(g_topics);
}

} // namespace
