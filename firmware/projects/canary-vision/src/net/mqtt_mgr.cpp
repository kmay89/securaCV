// src/net/mqtt_mgr.cpp
#include "canary/net/mqtt_mgr.h"

#include <Arduino.h>
#include <cstring>

#include <WiFi.h>
#include <PubSubClient.h>

#include "canary/config.h"
#include "canary/log.h"
#include "canary/runtime_config.h"  // NVS-backed identity + broker credentials
#include "canary/detect_config.h"   // NVS-backed runtime detection settings
#include "canary/detect_profiles.h" // watch profile keys/labels (cfg select)
#include "canary/vision/optical_features.h"  // coarse posture/proximity/occupancy names
#include "canary/diagnostics.h"     // heap health for the status heartbeat
#include "canary/net/wifi_mgr.h"    // RSSI + link state
#include "canary/ha/ha_discovery.h"
#include "identity/device_pseudonym.h"  // MAC-free client-ID suffix (Invariant III)
#include "identity/device_signature.h"  // shared signer (health pubkey, chain sig)
#include "canary/version.h"             // CANARY_FW_VERSION for the health publish
#include "canary/witness.h"             // chain head/length for the chain publish

namespace canary::net {

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

// Bound a stuck MQTT connect/read to well under the task watchdog budget
// instead of resting on PubSubClient's library default, so the socket
// timeout is a provable input to the WDT budget.
static constexpr uint16_t MQTT_SOCKET_TIMEOUT_SEC = 5;
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

// Inbound aim-assist switch command (latch-and-drain like the OTA auto
// switch). Retained switch state cached for reconnect republish.
static volatile int s_pending_aim = -1;
static int s_aim_state_cache = -1;

// Inbound identify command (HA identify button / companion app): the
// wizard's "which device is which" moment. Latch-and-drain like the OTA
// install command; main.cpp owns the blink window.
static volatile bool s_pending_identify = false;

// Inbound runtime detection settings (HA number entities). Latched by the
// callback, drained from the main loop — same pattern as the OTA commands.
// -1 = nothing pending.
static volatile long s_pending_cfg_target = -1;
static volatile long s_pending_cfg_score = -1;
static volatile long s_pending_cfg_lost = -1;
static volatile long s_pending_cfg_dwell = -1;
static volatile long s_pending_cfg_profile = -1;

static bool token_at(const char* p, int n, const char* tok, int tok_len) {
  auto boundary = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '"' || c == '}' || c == '\0';
  };
  return n >= tok_len && memcmp(p, tok, tok_len) == 0 &&
         (n == tok_len || boundary(p[tok_len]));
}

// Parse a small non-negative integer from an MQTT payload (HA number
// entities send plain decimals, possibly as "12.0", possibly quoted).
// Returns -1 on junk, non-finite ("nan"/"inf" — NaN bypasses range
// comparisons and casting it to long is UB), or out-of-range input so a
// mangled payload can never latch a value.
static long parse_cfg_number(const uint8_t* payload, unsigned int len, long max_value) {
  if (payload == nullptr || len == 0 || len > 24) return -1;
  char buf[25];
  memcpy(buf, payload, len);
  buf[len] = '\0';

  const char* start = buf;
  while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n' || *start == '"') start++;

  char* end = nullptr;
  const double d = strtod(start, &end);
  if (end == start || !isfinite(d)) return -1;
  while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n' || *end == '"')) end++;
  if (end && *end != '\0') return -1;
  if (d < 0 || d > (double)max_value) return -1;
  return (long)(d + 0.5);
}

// Parse an inbound watch-profile payload (HA's select sends the option
// label; scripts send the machine key). Trims the same whitespace/quote
// noise as parse_cfg_number; an unmatched string returns -1 so junk can
// never latch a profile change.
static long parse_cfg_profile(const uint8_t* payload, unsigned int len) {
  if (payload == nullptr || len == 0 || len > 48) return -1;
  char buf[49];
  memcpy(buf, payload, len);
  buf[len] = '\0';

  char* start = buf;
  while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n' || *start == '"') start++;
  char* end = start + strlen(start);
  while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                         end[-1] == '\n' || end[-1] == '"')) {
    *--end = '\0';
  }
  return (long)canary::cfg::watch_profile_from_text(start);
}

static void on_mqtt_message(char* topic, uint8_t* payload, unsigned int len) {
  if (!topic || !payload) return;

  if (strcmp(topic, g_topics.cfg_profile_cmd) == 0) {
    s_pending_cfg_profile = parse_cfg_profile(payload, len);
    return;
  }
  if (strcmp(topic, g_topics.cfg_target_cmd) == 0) {
    s_pending_cfg_target = parse_cfg_number(payload, len, 255);
    return;
  }
  if (strcmp(topic, g_topics.cfg_score_cmd) == 0) {
    s_pending_cfg_score = parse_cfg_number(payload, len, 100);
    return;
  }
  if (strcmp(topic, g_topics.cfg_lost_cmd) == 0) {
    s_pending_cfg_lost = parse_cfg_number(payload, len, 60000);
    return;
  }
  if (strcmp(topic, g_topics.cfg_dwell_cmd) == 0) {
    s_pending_cfg_dwell = parse_cfg_number(payload, len, 600000);
    return;
  }

  const bool is_install = (strcmp(topic, g_topics.update_cmd) == 0);
  const bool is_auto = (strcmp(topic, g_topics.update_auto_cmd) == 0);
  const bool is_aim = (strcmp(topic, g_topics.aim_cmd) == 0);
  const bool is_identify = (strcmp(topic, g_topics.identify_cmd) == 0);
  if (!is_install && !is_auto && !is_aim && !is_identify) return;

  // Trim leading whitespace/quotes; require a token boundary after the
  // match so a mangled payload can't trigger a flash cycle.
  const char* p = (const char*)payload;
  int n = (int)len;
  while (n > 0 && (*p == ' ' || *p == '\t' || *p == '"')) { p++; n--; }

  if (is_install) {
    if (token_at(p, n, "install", 7)) s_pending_install = true;
    return;
  }
  if (is_identify) {
    if (token_at(p, n, "identify", 8) || token_at(p, n, "ON", 2) ||
        token_at(p, n, "on", 2)) {
      s_pending_identify = true;
    }
    return;
  }
  if (is_aim) {
    if (token_at(p, n, "ON", 2) || token_at(p, n, "on", 2)) {
      s_pending_aim = 1;
    } else if (token_at(p, n, "OFF", 3) || token_at(p, n, "off", 3)) {
      s_pending_aim = 0;
    }
    return;
  }
  if (token_at(p, n, "ON", 2) || token_at(p, n, "on", 2)) {
    s_pending_auto = 1;
  } else if (token_at(p, n, "OFF", 3) || token_at(p, n, "off", 3)) {
    s_pending_auto = 0;
  }
}

int take_pending_aim() {
  const int v = s_pending_aim;
  s_pending_aim = -1;
  return v;
}

bool take_pending_identify() {
  if (!s_pending_identify) return false;
  s_pending_identify = false;
  return true;
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

static long take_pending(volatile long& slot) {
  const long v = slot;
  slot = -1;
  return v;
}

long take_pending_cfg_target()  { return take_pending(s_pending_cfg_target); }
long take_pending_cfg_score()   { return take_pending(s_pending_cfg_score); }
long take_pending_cfg_lost()    { return take_pending(s_pending_cfg_lost); }
long take_pending_cfg_dwell()   { return take_pending(s_pending_cfg_dwell); }
long take_pending_cfg_profile() { return take_pending(s_pending_cfg_profile); }

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
  mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_SEC);
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

void publish_heartbeat(const Topics& topics, const StateSnapshot& s) {
  const auto& d = canary::diag::get();
  char msg[384];
  snprintf(msg, sizeof(msg),
           "{"
           "\"device_id\":\"%s\","
           "\"device_type\":\"%s\","
           "\"status\":\"online\","
           "\"presence\":%s,"
           "\"dwelling\":%s,"
           "\"rssi\":%d,"
           "\"heap_free\":%lu,"
           "\"heap_min\":%lu,"
           "\"degraded\":\"%s\","
           "\"ts_ms\":%lu"
           "}",
           canary::cfg::get().device_id, DEVICE_TYPE,
           s.presence ? "true" : "false",
           s.dwelling ? "true" : "false",
           wifi_rssi(),
           (unsigned long)d.free_heap,
           (unsigned long)d.min_heap,
           canary::diag::level_name(d.level),
           (unsigned long)ms_now());
  publish_checked("HEART", topics.status, msg, true);
}

void publish_state_retained(const Topics& topics, const StateSnapshot& s) {
  char msg[768];
  snprintf(msg, sizeof(msg),
           "{"
           "\"device_id\":\"%s\","
           "\"device_type\":\"%s\","
           "\"profile\":\"%s\","
           "\"presence\":%s,"
           "\"dwelling\":%s,"
           "\"presence_ms\":%lu,"
           "\"dwell_ms\":%lu,"
           "\"confidence\":%d,"
           "\"voxel\":{\"rows\":%u,\"cols\":%u,\"r\":%d,\"c\":%d},"
           "\"bbox\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},"
           "\"occupancy\":\"%s\","
           "\"posture\":\"%s\","
           "\"proximity\":\"%s\","
           "\"occ_mask\":%u,"
           "\"last_event\":\"%s\","
           "\"uptime_s\":%lu,"
           "\"ts_ms\":%lu"
           "}",
           canary::cfg::get().device_id, DEVICE_TYPE,
           canary::cfg::watch_profile(canary::cfg::detect().profile).key,
           s.presence ? "true" : "false",
           s.dwelling ? "true" : "false",
           (unsigned long)s.presence_ms,
           (unsigned long)s.dwell_ms,
           (int)s.confidence,
           (unsigned)s.voxel.rows, (unsigned)s.voxel.cols, s.voxel.r, s.voxel.c,
           s.bbox.x, s.bbox.y, s.bbox.w, s.bbox.h,
           canary::vision::optical::occupancy_name(s.person_count),
           canary::vision::optical::posture_name(s.posture),
           canary::vision::optical::proximity_name(s.proximity),
           (unsigned)s.voxel_mask,
           s.last_event ? s.last_event : "boot",
           (unsigned long)s.uptime_s,
           (unsigned long)s.ts_ms);

  publish_checked("STATE", topics.state, msg, true);
}

// Witness surfaces — ported verbatim from canary-sense (mqtt_mgr.cpp):
// the byte-identical envelopes are what lets HA's existing handlers and
// TOFU pinning work for vision with zero integration changes.
void publish_health_retained(const Topics& topics) {
  char msg[384];
  const int n = snprintf(msg, sizeof(msg),
           "{"
           "\"battery\":100,"
           "\"battery_present\":false,"
           "\"memory_free\":%lu,"
           "\"uptime\":%lu,"
           "\"firmware_version\":\"%s\","
           "\"public_key\":\"%s\""
           "}",
           (unsigned long)ESP.getFreeHeap(),
           (unsigned long)(millis() / 1000UL),
           CANARY_FW_VERSION,
           device_signature::pubkey_hex());
  if (n <= 0 || (size_t)n >= sizeof(msg)) return;
  publish_checked("HEALTH", topics.health, msg, true);
}

void publish_chain_retained(const Topics& topics) {
  const uint32_t length = canary::witness::chain_length();
  const uint8_t* head = canary::witness::chain_head();

  char hash_hex[65];
  {
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
      hash_hex[2 * i]     = H[(head[i] >> 4) & 0xF];
      hash_hex[2 * i + 1] = H[(head[i] >> 0) & 0xF];
    }
    hash_hex[64] = '\0';
  }

  char sig_b64[device_signature::SIG_B64URL_CAP] = "";
  const bool signed_ok =
      canary::witness::ready() &&
      device_signature::sign_chain(length, head, sig_b64, sizeof(sig_b64));

  char msg[320];
  int n;
  if (signed_ok) {
    n = snprintf(msg, sizeof(msg),
        "{\"v\":%d,\"length\":%lu,\"latest_hash\":\"%s\","
        "\"algorithm\":\"ed25519\","
        "\"alg\":\"%s\",\"fp\":\"%s\",\"sig\":\"%s\"}",
        device_signature::SCHEMA_V,
        (unsigned long)length, hash_hex,
        device_signature::ALG_NAME,
        device_signature::fingerprint_hex(),
        sig_b64);
  } else {
    n = snprintf(msg, sizeof(msg),
        "{\"v\":%d,\"length\":%lu,\"latest_hash\":\"%s\","
        "\"algorithm\":\"ed25519\"}",
        device_signature::SCHEMA_V,
        (unsigned long)length, hash_hex);
  }
  if (n <= 0 || (size_t)n >= sizeof(msg)) return;
  publish_checked("CHAIN", topics.chain, msg, true);
}

void publish_event(const Topics& topics, const char* json_payload) {
  publish_checked("EVENT", topics.events, json_payload, false);
}

bool publish_aim(const Topics& topics, const char* json_payload) {
  if (!json_payload || !mqtt.connected()) return false;
  // Deliberately quiet: this runs at ~5 Hz while aim assist is on, and
  // publish_checked's per-publish serial line would drown the console.
  return mqtt.publish(topics.aim, json_payload, false);
}

bool publish_aim_state_retained(const Topics& topics, bool enabled) {
  s_aim_state_cache = enabled ? 1 : 0;
  if (!mqtt.connected()) return false;
  return publish_checked("AIM", topics.aim_state, enabled ? "ON" : "OFF", true);
}

bool publish_identify_echo(const Topics& topics, bool active) {
  if (!mqtt.connected()) return false;
  // Non-retained on purpose: the echo marks a live blink window, not a
  // state — a card should only pulse while the LED is actually flashing.
  return publish_checked("IDFY", topics.identify_echo, active ? "on" : "off",
                         false);
}

void ha_discovery_publish_once(const Topics& topics) {
  if (discovery_done) return;
  canary::ha::publish_discovery(mqtt, topics);
  discovery_done = true;
}

// ONE bounded connect attempt (TCP connect + MQTT CONNECT). On success it
// republishes the retained surfaces and re-subscribes every command topic;
// on failure it returns immediately so the caller's backoff owns the retry
// cadence (canary-sense parity). Replaces the unbounded blocking loop that
// froze the witness on a live-WiFi/dead-broker network — with delay(1000)
// yielding to IDLE, not even the idle watchdog fired (docs/strategy/12, F1).
bool mqtt_connect_attempt() {
  if (mqtt.connected()) return true;

  // A dead WiFi link makes every broker attempt hopeless — the caller's
  // wifi_loop() supervision owns that recovery.
  if (!wifi_connected()) return false;

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
    canary::dbg_serial().printf("Connect FAIL rc=%d — retrying on the main-loop backoff.\n", mqtt.state());
    return false;
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

  // Identify button: re-subscribe so the wizard's blink request always
  // reaches a connected device.
  mqtt.subscribe(g_topics.identify_cmd, 1);

  // Aim assist: re-subscribe the switch command and reconcile the retained
  // state. Defaults to OFF on a boot where main.cpp hasn't set it yet.
  mqtt.subscribe(g_topics.aim_cmd, 1);
  publish_checked("AIM", g_topics.aim_state,
                  (s_aim_state_cache == 1) ? "ON" : "OFF", true);

  // Runtime detection settings: re-subscribe the command topics and
  // reconcile the retained state from the NVS-backed values.
  mqtt.subscribe(g_topics.cfg_target_cmd, 1);
  mqtt.subscribe(g_topics.cfg_score_cmd, 1);
  mqtt.subscribe(g_topics.cfg_lost_cmd, 1);
  mqtt.subscribe(g_topics.cfg_dwell_cmd, 1);
  mqtt.subscribe(g_topics.cfg_profile_cmd, 1);
  publish_detect_cfg_retained(g_topics);
  return true;
}

bool publish_detect_cfg_retained(const Topics& topics) {
  if (!mqtt.connected()) return false;
  const auto& d = canary::cfg::detect();
  const auto& prof = canary::cfg::watch_profile(d.profile);
  char msg[256];
  snprintf(msg, sizeof(msg),
           "{"
           "\"target\":%u,"
           "\"score\":%u,"
           "\"lost_ms\":%lu,"
           "\"dwell_ms\":%lu,"
           "\"profile\":\"%s\","
           "\"profile_label\":\"%s\""
           "}",
           (unsigned)d.person_target,
           (unsigned)d.score_min,
           (unsigned long)d.lost_timeout_ms,
           (unsigned long)d.dwell_start_ms,
           prof.key, prof.label);
  return publish_checked("CFG", topics.cfg_state, msg, true);
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
