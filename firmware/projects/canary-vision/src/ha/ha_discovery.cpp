
#include "canary/ha/ha_discovery.h"

#include <Arduino.h>
#include <cstring>

#include "canary/config.h"
#include "canary/version.h"
#include "canary/log.h"
#include "canary/runtime_config.h"  // NVS-backed device id (OTA-safe)
#include "canary/detect_config.h"   // bounds for the settings number entities

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

  // WiFi RSSI (diagnostic) — published in the status heartbeat.
  {
    char t[192], p[1024];
    topic_for("sensor", "rssi", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"WiFi RSSI\","
             "\"unique_id\":\"%s_rssi\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.rssi }}\","
             "\"unit_of_measurement\":\"dBm\","
             "\"device_class\":\"signal_strength\","
             "\"entity_category\":\"diagnostic\","
             "\"icon\":\"mdi:wifi\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.status, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Free heap (diagnostic) — heap-health monitor, published in the status
  // heartbeat alongside the degradation level.
  {
    char t[192], p[1024];
    topic_for("sensor", "heap_free", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Free heap\","
             "\"unique_id\":\"%s_heap_free\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.heap_free }}\","
             "\"unit_of_measurement\":\"B\","
             "\"entity_category\":\"diagnostic\","
             "\"icon\":\"mdi:memory\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.status, availObj, devObj);
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

  // Runtime detection settings (NVS-backed, see canary/detect_config.h).
  // Number entities under the device's Configuration section: the loaded
  // SSCMA model decides class indices and score calibration, so these must
  // be adjustable without a rebuild when the model is swapped in SenseCraft.
  struct NumberEnt {
    const char* objectId;
    const char* name;
    const char* json_key;
    const char* cmd_topic;
    const char* unit;       // nullptr = unitless
    const char* icon;
    const char* mode;       // "slider" or "box"
    long min, max, step;
  };
  const NumberEnt numbers[] = {
    {"cfg_target", "Person class index", "target", topics.cfg_target_cmd,
     nullptr, "mdi:tag-outline", "box", 0, 255, 1},
    {"cfg_score", "Score threshold", "score", topics.cfg_score_cmd,
     "%", "mdi:chart-bell-curve", "slider",
     canary::cfg::DETECT_SCORE_MIN_LO, canary::cfg::DETECT_SCORE_MIN_HI, 1},
    {"cfg_lost", "Lost timeout", "lost_ms", topics.cfg_lost_cmd,
     "ms", "mdi:timer-off-outline", "box",
     (long)canary::cfg::DETECT_LOST_MS_LO, (long)canary::cfg::DETECT_LOST_MS_HI, 250},
    {"cfg_dwell", "Dwell start", "dwell_ms", topics.cfg_dwell_cmd,
     "ms", "mdi:timer-sand", "box",
     (long)canary::cfg::DETECT_DWELL_MS_LO, (long)canary::cfg::DETECT_DWELL_MS_HI, 500},
  };
  for (const auto& n : numbers) {
    char t[192], p[1280], unitField[64] = "";
    if (n.unit) {
      snprintf(unitField, sizeof(unitField), "\"unit_of_measurement\":\"%s\",", n.unit);
    }
    topic_for("number", n.objectId, t, sizeof(t));
    const int written = snprintf(p, sizeof(p),
             "{"
             "\"name\":\"%s\","
             "\"unique_id\":\"%s_%s\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.%s }}\","
             "\"command_topic\":\"%s\","
             "\"min\":%ld,\"max\":%ld,\"step\":%ld,"
             "\"mode\":\"%s\","
             "%s"
             "\"icon\":\"%s\","
             "\"entity_category\":\"config\","
             "%s,%s"
             "}",
             n.name, DEVICE_ID, n.objectId,
             topics.cfg_state, n.json_key, n.cmd_topic,
             n.min, n.max, n.step, n.mode,
             unitField, n.icon, availObj, devObj);
    if (written < 0 || written >= (int)sizeof(p)) {
      // Truncated JSON would be silently ignored by HA — fail loud instead.
      log_line("DISC", "number entity payload truncated — skipped (device_id too long?)");
      continue;
    }
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
