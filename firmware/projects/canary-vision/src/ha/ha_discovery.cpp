
#include "canary/ha/ha_discovery.h"

#include <Arduino.h>
#include <cstring>

#include "canary/config.h"
#include "canary/version.h"
#include "canary/log.h"
#include "canary/runtime_config.h"  // NVS-backed device id (OTA-safe)
#include "canary/detect_config.h"   // bounds for the settings number entities
#include "canary/detect_profiles.h" // watch profile options for the select
#include "canary/doorbell.h"         // the Vision Doorbell: switch, event, glow, button health
#include "canary/doorbell_audio.h"   // ...and its speaker: the volume number, the reply select

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
    "\"name\":\"SecuraCV Canary Vision %s\","
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
  // model (kDeviceObjectMost, above; sweep F181): 259 bytes at worst, and it
  // was 256, so from a 46-character id it was cut mid-JSON.
  char devObj[320];
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
  // Presence
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
             "\"icon\":\"mdi:shield-eye\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Dwelling
  {
    char t[192], p[1024];
    topic_for("binary_sensor", "dwelling", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Dwelling\","
             "\"unique_id\":\"%s_dwelling\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ 'true' if value_json.dwelling | default(false) else 'false' }}\","
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

  // Occupancy — coarse count bucket (none/one/two/several). Deliberately NOT
  // an exact running tally, so it can't become a per-household occupancy
  // history (free-signals §5, §7).
  {
    char t[192], p[768];
    topic_for("sensor", "occupancy", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Occupancy\","
             "\"unique_id\":\"%s_occupancy\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.occupancy | default('unknown') }}\","
             "\"icon\":\"mdi:account-group\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Posture — coarse upright/ambiguous/horizontal from the bounding-box aspect
  // ratio (not a skeleton). Advisory, physics-not-politics.
  {
    char t[192], p[768];
    topic_for("sensor", "posture", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Posture\","
             "\"unique_id\":\"%s_posture\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.posture | default('unknown') }}\","
             "\"icon\":\"mdi:human\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.state, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Proximity — coarse far/mid/near from the bounding-box area fraction.
  {
    char t[192], p[768];
    topic_for("sensor", "proximity", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Proximity\","
             "\"unique_id\":\"%s_proximity\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.proximity | default('unknown') }}\","
             "\"icon\":\"mdi:map-marker-distance\","
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

  // Watch profile select (detect_profiles.h): one-step per-use-case preset —
  // room presence vs litter box. Picking an option applies that profile's
  // recommended tuning to the four numbers above (each stays individually
  // adjustable afterward) and retargets the fleet beacon's detect class.
  {
    char options[192] = "";
    size_t off = 0;
    for (uint8_t i = 0; i < canary::cfg::WATCH_PROFILE_COUNT; ++i) {
      const int w = snprintf(options + off, sizeof(options) - off, "%s\"%s\"",
                             i ? "," : "", canary::cfg::WATCH_PROFILES[i].label);
      if (w < 0 || (size_t)w >= sizeof(options) - off) {
        log_line("DISC", "watch profile options truncated — select skipped");
        options[0] = '\0';
        break;
      }
      off += (size_t)w;
    }
    if (options[0] != '\0') {
      char t[192], p[1280];
      topic_for("select", "watch_profile", t, sizeof(t));
      const int written = snprintf(p, sizeof(p),
               "{"
               "\"name\":\"Watch profile\","
               "\"unique_id\":\"%s_watch_profile\","
               "\"state_topic\":\"%s\","
               "\"value_template\":\"{{ value_json.profile_label | default('Room presence') }}\","
               "\"command_topic\":\"%s\","
               "\"options\":[%s],"
               "\"icon\":\"mdi:eye-settings-outline\","
               "\"entity_category\":\"config\","
               "%s,%s"
               "}",
               DEVICE_ID, topics.cfg_state, topics.cfg_profile_cmd, options,
               availObj, devObj);
      if (written < 0 || written >= (int)sizeof(p)) {
        log_line("DISC", "watch profile select payload truncated — skipped");
      } else {
        publish_cfg(mqtt, t, p);
      }
    }
  }

  // Aim assist switch (bench/aiming): streams a boxes-only live channel
  // (coordinates + scores, never pixels) to securacv/<id>/aim for the
  // Lovelace aim card. Off by default and auto-off after 10 minutes.
  {
    char t[192], p[1024];
    topic_for("switch", "aim_assist", t, sizeof(t));
    snprintf(p, sizeof(p),
             "{"
             "\"name\":\"Aim assist\","
             "\"unique_id\":\"%s_aim_assist\","
             "\"state_topic\":\"%s\","
             "\"command_topic\":\"%s\","
             "\"icon\":\"mdi:crosshairs-gps\","
             "\"entity_category\":\"config\","
             "%s,%s"
             "}",
             DEVICE_ID, topics.aim_state, topics.aim_cmd, availObj, devObj);
    publish_cfg(mqtt, t, p);
  }

  // Identify button (HAP-style): blinks the LED for 10 s so the wizard's
  // "which device is which" moment works from HA and the companion app.
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

  // Vision Doorbell (boards with its pins: canary/doorbell.h). The switch
  // is always announced there; the doorbell's own entities exist only while
  // it is on — switched off, an empty retained config removes them, so a
  // plain Vision never shows a doorbell. The event entity's device_class
  // makes the press a real doorbell to Home Assistant (and to HomeKit
  // through its bridge); the sealed record of the press is the `doorbell`
  // event on the events topic, not this.
  if (canary::doorbell_hw::available()) {
    const bool on = canary::doorbell_hw::enabled();
    {
      char t[192], p[1024];
      topic_for("switch", "doorbell", t, sizeof(t));
      snprintf(p, sizeof(p),
               "{"
               "\"name\":\"Doorbell enabled\","
               "\"unique_id\":\"%s_doorbell_switch\","
               "\"state_topic\":\"%s\","
               "\"value_template\":\"{{ value_json.enabled | default('OFF') }}\","
               "\"command_topic\":\"%s\","
               "\"icon\":\"mdi:doorbell\","
               "\"entity_category\":\"config\","
               "%s,%s"
               "}",
               DEVICE_ID, topics.doorbell_state, topics.doorbell_enable_cmd, availObj, devObj);
      publish_cfg(mqtt, t, p);
    }
    {
      char t[192], p[1024];
      topic_for("event", "doorbell", t, sizeof(t));
      snprintf(p, sizeof(p),
               "{"
               "\"name\":\"Doorbell\","
               "\"unique_id\":\"%s_doorbell\","
               "\"state_topic\":\"%s\","
               "\"event_types\":[\"press\"],"
               "\"device_class\":\"doorbell\","
               "%s,%s"
               "}",
               DEVICE_ID, topics.doorbell, availObj, devObj);
      publish_cfg(mqtt, t, on ? p : "");
    }
    {
      // min/max are doorbell_logic.h's kGlowPctMin/Max (asserted below): a
      // doorbell that is on never glows dark.
      static_assert(::doorbell::kGlowPctMin == 10 && ::doorbell::kGlowPctMax == 100,
                    "the Doorbell glow number's min/max literals follow doorbell_logic.h");
      char t[192], p[1024];
      topic_for("number", "doorbell_glow", t, sizeof(t));
      snprintf(p, sizeof(p),
               "{"
               "\"name\":\"Doorbell glow\","
               "\"unique_id\":\"%s_doorbell_glow\","
               "\"state_topic\":\"%s\","
               "\"value_template\":\"{{ value_json.glow }}\","
               "\"command_topic\":\"%s\","
               "\"min\":10,\"max\":100,\"step\":5,"
               "\"mode\":\"slider\","
               "\"unit_of_measurement\":\"%%\","
               "\"icon\":\"mdi:brightness-6\","
               "\"entity_category\":\"config\","
               "%s,%s"
               "}",
               DEVICE_ID, topics.doorbell_state, topics.doorbell_glow_cmd, availObj, devObj);
      publish_cfg(mqtt, t, on ? p : "");
    }
    {
      char t[192], p[1024];
      topic_for("sensor", "doorbell_button", t, sizeof(t));
      snprintf(p, sizeof(p),
               "{"
               "\"name\":\"Doorbell button\","
               "\"unique_id\":\"%s_doorbell_button\","
               "\"state_topic\":\"%s\","
               "\"value_template\":\"{{ value_json.button | default('ok') }}\","
               "\"icon\":\"mdi:gesture-tap-button\","
               "\"entity_category\":\"diagnostic\","
               "%s,%s"
               "}",
               DEVICE_ID, topics.doorbell_state, availObj, devObj);
      publish_cfg(mqtt, t, on ? p : "");
    }
    // The speaker (canary/doorbell_audio.h, boards with DOORBELL_AUDIO_PIN):
    // the chime's volume, and the reply select — three TONES the household
    // can send the visitor ("we're coming", "leave it", "no thanks"), the
    // quick replies a witness can make without a voice (dossier §4.3). The
    // option texts are doorbell_audio.h's phrase_name()s, which the device
    // matches on the command topic.
    if (canary::doorbell_audio::available()) {
      static_assert(::doorbell::kVolumeMin == 0 && ::doorbell::kVolumeMax == 100,
                    "the Doorbell volume number's min/max literals follow doorbell_audio.h");
      // the select's option literals are doorbell_audio.h's phrase_name()s,
      // which the device matches on the command topic — held here, not trusted
      static_assert(::doorbell::same_name(::doorbell::phrase_name(::doorbell::Phrase::NONE), "—")
                    && ::doorbell::same_name(::doorbell::phrase_name(::doorbell::Phrase::WAIT), "we're coming")
                    && ::doorbell::same_name(::doorbell::phrase_name(::doorbell::Phrase::LEAVE), "leave it")
                    && ::doorbell::same_name(::doorbell::phrase_name(::doorbell::Phrase::NO), "no thanks"),
                    "the Doorbell reply select's option literals follow doorbell_audio.h's phrase_name()");
      {
        char t[192], p[1024];
        topic_for("number", "doorbell_volume", t, sizeof(t));
        snprintf(p, sizeof(p),
                 "{"
                 "\"name\":\"Doorbell volume\","
                 "\"unique_id\":\"%s_doorbell_volume\","
                 "\"state_topic\":\"%s\","
                 "\"value_template\":\"{{ value_json.volume }}\","
                 "\"command_topic\":\"%s\","
                 "\"min\":0,\"max\":100,\"step\":10,"
                 "\"mode\":\"slider\","
                 "\"unit_of_measurement\":\"%%\","
                 "\"icon\":\"mdi:volume-high\","
                 "\"entity_category\":\"config\","
                 "%s,%s"
                 "}",
                 DEVICE_ID, topics.doorbell_state, topics.doorbell_volume_cmd, availObj, devObj);
        publish_cfg(mqtt, t, on ? p : "");
      }
      {
        char t[192], p[1024];
        topic_for("select", "doorbell_reply", t, sizeof(t));
        snprintf(p, sizeof(p),
                 "{"
                 "\"name\":\"Doorbell reply\","
                 "\"unique_id\":\"%s_doorbell_reply\","
                 "\"state_topic\":\"%s\","
                 "\"value_template\":\"{{ value_json.reply }}\","
                 "\"command_topic\":\"%s\","
                 "\"options\":[\"—\",\"we're coming\",\"leave it\",\"no thanks\"],"
                 "\"icon\":\"mdi:bullhorn-outline\","
                 "%s,%s"
                 "}",
                 DEVICE_ID, topics.doorbell_state, topics.doorbell_reply_cmd, availObj, devObj);
        publish_cfg(mqtt, t, on ? p : "");
      }
    }
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
