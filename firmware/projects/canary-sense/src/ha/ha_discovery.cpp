
#include "canary/ha/ha_discovery.h"

#include <Arduino.h>
#include <cstring>

#include "canary/config.h"
#include "canary/version.h"
#include "canary/log.h"
#include "canary/runtime_config.h"  // NVS-backed device id (OTA-safe)
#include "canary/sense_config.h"    // bounds for the reflex number entities

// Entity set per the canary-sense design doc §5: presence / occupants /
// range-band / radar-link / illuminance on the P0 path; the wellbeing build
// adds the P0 "breathing confirmed" binary and — only with the P1 opt-in
// flag — the BPM numerics. BPM entities are provably absent from a
// presence-only build: the discovery payloads below are compiled out.

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
           "\"name\":\"SecuraCV Canary Sense %s\","
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

  // Presence (debounced radar occupancy)
  {
    char t[192], p[1024];
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
             "\"icon\":\"mdi:radar\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Occupants (bucketed count — 0 / 1 / 2+, never a track log)
  {
    char t[192], p[1024];
    topic_for("sensor", "occupants", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Occupants\","
             "\"unique_id\":\"%s_occupants\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.occupants }}\","
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

  // Radar link problem sensor (diagnostic; ON while the UART is stalled)
  {
    char t[192], p[1024];
    topic_for("binary_sensor", "radar_link", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Radar link problem\","
             "\"unique_id\":\"%s_radar_link\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ 'true' if not value_json.radar_ok else 'false' }}\","
             "\"payload_on\":\"true\","
             "\"payload_off\":\"false\","
             "\"device_class\":\"problem\","
             "\"entity_category\":\"diagnostic\","
             "\"icon\":\"mdi:radar\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Radar frame errors (diagnostic; checksum/oversize drops, monotonic)
  {
    char t[192], p[1024];
    topic_for("sensor", "frame_errors", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Radar frame errors\","
             "\"unique_id\":\"%s_frame_errors\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.frame_errors }}\","
             "\"state_class\":\"total_increasing\","
             "\"entity_category\":\"diagnostic\","
             "\"icon\":\"mdi:alert-circle-outline\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Illuminance (BH1750 — tamper corroboration: lights-out + presence)
  {
    char t[192], p[1024];
    topic_for("sensor", "illuminance", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Illuminance\","
             "\"unique_id\":\"%s_illuminance\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.lux }}\","
             "\"unit_of_measurement\":\"lx\","
             "\"device_class\":\"illuminance\","
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

#ifdef CANARY_SENSE_VITALS
  // Breathing confirmed (P0 binary lock — the only always-on vitals signal;
  // wellbeing channel, never sealed-logged)
  {
    char t[192], p[1024];
    topic_for("binary_sensor", "breathing", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Breathing confirmed\","
             "\"unique_id\":\"%s_breathing\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.breathing_locked | default(false) }}\","
             "\"payload_on\":\"true\","
             "\"payload_off\":\"false\","
             "\"icon\":\"mdi:lungs\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

#if defined(FEATURE_VITALS_BPM_P1) && FEATURE_VITALS_BPM_P1
  // P1 opt-in BPM numerics. Non-diagnostic radar estimates (85–90% accuracy,
  // <=1.5 m, single target) — wellbeing signals, not medical data.
  {
    char t[192], p[1024];
    topic_for("sensor", "breath_rate", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Breathing rate\","
             "\"unique_id\":\"%s_breath_rate\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.breath_bpm }}\","
             "\"unit_of_measurement\":\"bpm\","
             "\"icon\":\"mdi:lungs\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }
  {
    char t[192], p[1024];
    topic_for("sensor", "heart_rate", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Heart rate\","
             "\"unique_id\":\"%s_heart_rate\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.heart_bpm }}\","
             "\"unit_of_measurement\":\"bpm\","
             "\"icon\":\"mdi:heart-pulse\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }
#endif  // FEATURE_VITALS_BPM_P1
#endif  // CANARY_SENSE_VITALS

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

  // Identify button (HAP-style): flashes the WS2812 white for 10 s so the
  // wizard's "which device is which" moment works from HA and the
  // companion app.
  {
    char t[192], p[1024];
    topic_for("button", "identify", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Identify\","
             "\"unique_id\":\"%s_identify\","
             "\"command_topic\":\"%s\","
             "\"payload_press\":\"identify\","
             "\"device_class\":\"identify\","
             "\"entity_category\":\"config\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.identify_cmd, availObj, devObj);
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

  // Runtime radar reflexes — number entities over the cfg/* schema, the
  // exact pattern canary-vision's detection dials use. Bounds come from
  // sense_config.h so HA can never ask for a value the setters would refuse.
  // Vitals windows are compiled out of the presence-only build's discovery:
  // an entity for a knob the build doesn't use would be a lie.
  {
    struct NumberEnt {
      const char* objectId;
      const char* name;
      const char* json_key;   // key in cfg/state
      const char* cmd_topic;
      const char* unit;
      const char* icon;
      const char* mode;       // "slider" or "box"
      long min, max, step;
    };
    const NumberEnt numbers[] = {
      {"cfg_debounce", "Presence debounce", "debounce_ms", topics.cfg_debounce_cmd,
       "ms", "mdi:timer-play-outline", "slider",
       (long)canary::cfg::SENSE_DEBOUNCE_MS_LO, (long)canary::cfg::SENSE_DEBOUNCE_MS_HI, 50},
      {"cfg_clear", "Clear timeout", "clear_ms", topics.cfg_clear_cmd,
       "ms", "mdi:timer-off-outline", "box",
       (long)canary::cfg::SENSE_CLEAR_MS_LO, (long)canary::cfg::SENSE_CLEAR_MS_HI, 100},
      {"cfg_stall", "Radar stall alarm", "stall_ms", topics.cfg_stall_cmd,
       "ms", "mdi:radar", "box",
       (long)canary::cfg::SENSE_STALL_MS_LO, (long)canary::cfg::SENSE_STALL_MS_HI, 500},
      {"cfg_near", "Near band", "near_cm", topics.cfg_near_cmd,
       "cm", "mdi:map-marker-radius-outline", "slider",
       (long)canary::cfg::SENSE_NEAR_CM_LO, (long)canary::cfg::SENSE_NEAR_CM_HI, 10},
      {"cfg_mid", "Mid band", "mid_cm", topics.cfg_mid_cmd,
       "cm", "mdi:map-marker-distance", "slider",
       (long)canary::cfg::SENSE_MID_CM_LO, (long)canary::cfg::SENSE_MID_CM_HI, 10},
#if defined(FEATURE_VITALS) && FEATURE_VITALS
      {"cfg_vlock", "Vitals lock", "vitals_lock_ms", topics.cfg_vlock_cmd,
       "ms", "mdi:heart-pulse", "box",
       (long)canary::cfg::SENSE_VLOCK_MS_LO, (long)canary::cfg::SENSE_VLOCK_MS_HI, 500},
      {"cfg_vlost", "Vitals lost", "vitals_lost_ms", topics.cfg_vlost_cmd,
       "ms", "mdi:heart-off-outline", "box",
       (long)canary::cfg::SENSE_VLOST_MS_LO, (long)canary::cfg::SENSE_VLOST_MS_HI, 500},
#endif
    };
    for (const auto& n : numbers) {
      char t[192], p[1280];
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
               "\"unit_of_measurement\":\"%s\","
               "\"icon\":\"%s\","
               "\"entity_category\":\"config\","
               "%s,%s"
               "}",
               n.name, DEVICE_ID, n.objectId,
               topics.cfg_state, n.json_key, n.cmd_topic,
               n.min, n.max, n.step, n.mode,
               n.unit, n.icon, availObj, devObj);
      if (written < 0 || written >= (int)sizeof(p)) {
        // Truncated JSON would be silently ignored by HA — fail loud instead.
        log_line("DISC", "number entity payload truncated — skipped (device_id too long?)");
        continue;
      }
      publish_cfg(mqtt, t, p);
    }
  }

  log_line("DISC", "Home Assistant discovery published (retained).");
}

} // namespace canary::ha
