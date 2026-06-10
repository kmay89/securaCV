
#include "canary/ha/ha_discovery.h"

#include <Arduino.h>
#include <cstring>

#include "canary/config.h"
#include "canary/version.h"
#include "canary/log.h"
#include "canary/runtime_config.h"  // NVS-backed device id (OTA-safe)

namespace canary::ha {

static bool publish_cfg(PubSubClient& mqtt, const char* topic, const char* payload) {
  const bool ok = mqtt.publish(topic, payload, true);

  log_header("DISC");
  // IMPORTANT: do NOT use raw Serial in CI; always go through dbg_serial()
  canary::dbg_serial().printf("%s => %s (retain=true len=%u)\n",
                              topic,
                              ok ? "OK" : "FAIL",
                              (unsigned)strlen(payload));
  return ok;
}

void publish_discovery(PubSubClient& mqtt, const Topics& topics) {
  const char* DEVICE_ID = canary::cfg::get().device_id;  // shadows config.h's compiled default
  char devObj[256];
  snprintf(devObj, sizeof(devObj),
           "\"device\":{"
           "\"identifiers\":[\"securacv_%s\"],"
           "\"name\":\"SecuraCV Canary Vision %s\","
           "\"manufacturer\":\"%s\","
           "\"model\":\"%s\","
           "\"sw_version\":\"%s\""
           "}",
           DEVICE_ID, DEVICE_ID, MANUFACTURER, MODEL, CANARY_FW_VERSION);

  char availObj[256];
  snprintf(availObj, sizeof(availObj),
           "\"availability_topic\":\"%s\","
           "\"availability_template\":\"{{ value_json.status }}\","
           "\"payload_available\":\"online\","
           "\"payload_not_available\":\"offline\"",
           topics.status);

  auto topic_for = [&](const char* component, const char* objectId, char* out, size_t n) {
    snprintf(out, n, "%s/%s/%s/%s/config", HA_DISCOVERY_PREFIX, component, DEVICE_ID, objectId);
  };

  // Presence
  {
    char t[192], p[768];
    topic_for("binary_sensor", "presence", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Presence\","
             "\"unique_id\":\"%s_presence\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.presence | default(false) }}\","
             "\"payload_on\":\"true\","
             "\"payload_off\":\"false\","
             "\"device_class\":\"occupancy\","
             "\"icon\":\"mdi:shield-eye\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Dwelling
  {
    char t[192], p[768];
    topic_for("binary_sensor", "dwelling", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Dwelling\","
             "\"unique_id\":\"%s_dwelling\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.dwelling | default(false) }}\","
             "\"payload_on\":\"true\","
             "\"payload_off\":\"false\","
             "\"icon\":\"mdi:timer-sand\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Confidence
  {
    char t[192], p[768];
    topic_for("sensor", "confidence", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Confidence\","
             "\"unique_id\":\"%s_confidence\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.confidence }}\","
             "\"unit_of_measurement\":\"%%\","
             "\"icon\":\"mdi:chart-bell-curve\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Voxel
  {
    char t[192], p[768];
    topic_for("sensor", "voxel", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Voxel\","
             "\"unique_id\":\"%s_voxel\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.voxel.r }},{{ value_json.voxel.c }}\","
             "\"icon\":\"mdi:grid\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Last event
  {
    char t[192], p[768];
    topic_for("sensor", "last_event", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Last event\","
             "\"unique_id\":\"%s_last_event\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.last_event }}\","
             "\"icon\":\"mdi:bell-ring\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Uptime
  {
    char t[192], p[768];
    topic_for("sensor", "uptime", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Uptime\","
             "\"unique_id\":\"%s_uptime\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.uptime_s }}\","
             "\"unit_of_measurement\":\"s\","
             "\"device_class\":\"duration\","
             "\"icon\":\"mdi:clock-outline\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Firmware update entity (signed pull-OTA). HA renders a proper update
  // card: installed vs latest version, release notes, Install button, and
  // a live progress bar while the device downloads/verifies/installs.
  {
    char t[192], p[1024];
    topic_for("update", "firmware", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Firmware\","
             "\"unique_id\":\"%s_firmware\","
             "\"state_topic\":\"%s\","
             "\"command_topic\":\"%s\","
             "\"payload_install\":\"install\","
             "\"device_class\":\"firmware\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.update_state, topics.update_cmd, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Auto-update opt-in switch. Off by default — a witness device should
  // not restart unattended unless its owner chose that.
  {
    char t[192], p[1024];
    topic_for("switch", "auto_update", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Auto Update\","
             "\"unique_id\":\"%s_auto_update\","
             "\"state_topic\":\"%s\","
             "\"command_topic\":\"%s\","
             "\"icon\":\"mdi:update\","
             "\"entity_category\":\"config\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.update_auto, topics.update_auto_cmd, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  log_line("DISC", "Home Assistant discovery published (retained).");
}

} // namespace canary::ha
