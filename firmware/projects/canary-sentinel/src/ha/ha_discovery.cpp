
#include "canary/ha/ha_discovery.h"

#include <Arduino.h>
#include <cstring>

#include "canary/config.h"
#include "canary/version.h"
#include "canary/log.h"
#include "canary/runtime_config.h"  // NVS-backed device id (OTA-safe)

// Entity set per docs/canary_sentinel_fusion_design.md: the coarse fused
// claim and nothing finer (requirement R4) — presence (level present /
// confirmed / loiter), the anomaly overlay, the ordinal level, 0..100
// confidence and anomaly scores, the 0/1/2+ occupancy bucket, the near/mid/far
// band and which modality CLASSES corroborated. A "channel blinded" problem
// sensor surfaces a denied channel as a claim, never a silent gap (R2, R6).
// Plus the canary-sense diagnostics + update entity set. No identify button
// and no reflex dials: Phase 1a wires neither.

namespace canary::ha {

// The device object every announcement embeds (sweep F181): this format with
// the device id twice and this build's manufacturer, model and firmware
// version. It used to sit in a fixed buffer that a long id overran, cut
// mid-JSON, which made every entity's config invalid JSON (Home Assistant
// ignores those). kDeviceObjectMost is the most it can take: the format
// without its five %s, the longest id runtime_config.h accepts (device_id[48],
// so 47 characters) twice, this build's strings and the terminator.
// publish_discovery's buffer is checked against it at compile time, in every
// flavor's build, and scripts/tests/test_ha_discovery_binary_sensors.py
// formats it at the buffer's declared size for a 47-character id and every
// flavor's model.
static constexpr char kDeviceObjectFormat[] =
    "\"device\":{"
    "\"identifiers\":[\"securacv_%s\"],"
    "\"name\":\"SecuraCV Canary Sentinel %s\","
    "\"manufacturer\":\"%s\","
    "\"model\":\"%s\","
    "\"sw_version\":\"%s\""
    "}";
static constexpr size_t text_bytes(const char* s) { return *s ? 1 + text_bytes(s + 1) : 0; }
static constexpr size_t conversions(const char* s) {
  return *s ? (*s == '%' ? 1 : 0) + conversions(s + 1) : 0;
}
static_assert(conversions(kDeviceObjectFormat) == 5,
              "the device object takes five %s: the id twice, manufacturer, model, version");
static constexpr size_t kDeviceObjectMost =
    text_bytes(kDeviceObjectFormat) - 2 * conversions(kDeviceObjectFormat) +
    2 * (sizeof(canary::cfg::RuntimeConfig::device_id) - 1) +
    text_bytes(MANUFACTURER) + text_bytes(MODEL) + text_bytes(CANARY_FW_VERSION) + 1;

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
  // The device object, sized for the longest device id and this build's
  // model (kDeviceObjectMost, above; sweep F181). 384 here: the Sentinel's
  // model strings are the longest (its worst case is 293 bytes).
  char devObj[384];
  static_assert(sizeof(devObj) >= kDeviceObjectMost,
                "devObj cannot hold the device object for the longest device id and this "
                "build's model: make it at least kDeviceObjectMost (sweep F181)");
  snprintf(devObj, sizeof(devObj), kDeviceObjectFormat,
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

  // Binary sensors render payload_on/payload_off themselves (sweep HA25):
  // HA compares the rendered text to "true"/"false" with plain equality, and
  // `{{ value_json.x | default(false) }}` over a JSON boolean renders "True" /
  // "False", which matched neither, so the entity stayed unknown. The
  // default(false) inside the test reads a row without the field as off,
  // with no template warning. scripts/tests/test_ha_discovery_binary_sensors.py
  // renders every one the way HA does.
  // Presence — the fused level at present / confirmed / loiter.
  {
    char t[192], p[1024];
    topic_for("binary_sensor", "presence", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Presence\","
             "\"unique_id\":\"%s_presence\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ 'true' if value_json.presence | default(false) else 'false' }}\","
             "\"payload_on\":\"true\","
             "\"payload_off\":\"false\","
             "\"device_class\":\"occupancy\","
             "\"icon\":\"mdi:shield-home\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Anomaly — the overlay that wins and latches: blinding, inconsistency or a
  // dwelling body no other class corroborates (requirements R2, R3).
  {
    char t[192], p[1024];
    topic_for("binary_sensor", "anomaly", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Anomaly\","
             "\"unique_id\":\"%s_anomaly\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ 'true' if value_json.anomaly_active | default(false) else 'false' }}\","
             "\"payload_on\":\"true\","
             "\"payload_off\":\"false\","
             "\"icon\":\"mdi:shield-alert\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Level — the ordinal decision (clear / aware / present / confirmed /
  // loiter / anomaly).
  {
    char t[192], p[1024];
    topic_for("sensor", "level", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Level\","
             "\"unique_id\":\"%s_level\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.level }}\","
             "\"icon\":\"mdi:stairs\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Confidence — the 0..100 fused evidence score.
  {
    char t[192], p[1024];
    topic_for("sensor", "confidence", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Confidence\","
             "\"unique_id\":\"%s_confidence\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.confidence }}\","
             "\"unit_of_measurement\":\"%%\","
             "\"state_class\":\"measurement\","
             "\"icon\":\"mdi:gauge\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Anomaly score — the 0..100 suspicion accumulator behind the overlay
  // (diagnostic: the binary sensor above is the actionable one).
  {
    char t[192], p[1024];
    topic_for("sensor", "anomaly_score", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Anomaly score\","
             "\"unique_id\":\"%s_anomaly_score\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.anomaly }}\","
             "\"unit_of_measurement\":\"%%\","
             "\"state_class\":\"measurement\","
             "\"entity_category\":\"diagnostic\","
             "\"icon\":\"mdi:alert-decagram-outline\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Occupancy (bucketed — unknown / 0 / 1 / 2+, never a track log)
  {
    char t[192], p[1024];
    topic_for("sensor", "occupancy", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Occupancy\","
             "\"unique_id\":\"%s_occupancy\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.occupancy }}\","
             "\"icon\":\"mdi:account-group\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Range band (diagnostic; coarse near/mid/far only — raw distance never
  // leaves the device, per the privacy chokepoint)
  {
    char t[192], p[1024];
    topic_for("sensor", "range_band", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Range band\","
             "\"unique_id\":\"%s_range_band\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.range }}\","
             "\"icon\":\"mdi:ruler\","
             "\"entity_category\":\"diagnostic\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Corroborating modalities — which physical CLASSES voted (thermal, radar,
  // csi, carried_radio, optical, mechanical), never which device or MAC.
  {
    char t[192], p[1024];
    topic_for("sensor", "modalities", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Corroborating modalities\","
             "\"unique_id\":\"%s_modalities\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.modalities }}\","
             "\"icon\":\"mdi:set-center\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Channel blinded (diagnostic problem sensor; ON while any enabled channel
  // is denied — covered, jammed, unplugged or stalled). Degrade honestly (R6).
  {
    char t[192], p[1024];
    topic_for("binary_sensor", "channel_denied", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Channel blinded\","
             "\"unique_id\":\"%s_channel_denied\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ 'true' if value_json.channel_denied | default(false) else 'false' }}\","
             "\"payload_on\":\"true\","
             "\"payload_off\":\"false\","
             "\"device_class\":\"problem\","
             "\"entity_category\":\"diagnostic\","
             "\"icon\":\"mdi:eye-off-outline\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Last event
  {
    char t[192], p[1024];
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
    char t[192], p[1024];
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
