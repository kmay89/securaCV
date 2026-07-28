// src/net/mqtt_mgr.cpp
#include "canary/net/mqtt_mgr.h"

#include <Arduino.h>
#include <cstring>

#include <WiFi.h>
#include <PubSubClient.h>

#include "canary/config.h"
#include "canary/log.h"
#include "canary/version.h"
#include "canary/runtime_config.h"  // NVS-backed identity + broker credentials
#include "canary/sense_config.h"    // NVS-backed radar reflexes (cfg/* dials)
#include "canary/diagnostics.h"     // heap health for the status heartbeat
#include "canary/witness.h"         // chain head/length for the trust surface
#include "canary/net/wifi_mgr.h"    // RSSI + link state
#include "canary/ha/ha_discovery.h"
#include "identity/device_pseudonym.h"  // MAC-free client-ID suffix (Invariant III)
#include "identity/device_signature.h"  // pubkey/fingerprint + chain signature

namespace canary::net {

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

// Bound a stuck MQTT connect/read to well under the task watchdog timeout
// (CS_WATCHDOG_TIMEOUT_SEC) instead of resting on PubSubClient's library
// default, so the socket timeout is a provable input to the WDT budget.
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

// Inbound identify command (HA identify button / companion app): the
// wizard's "which device is which" moment. Latch-and-drain like the OTA
// install command; main.cpp owns the blink window.
static volatile bool s_pending_identify = false;

// Inbound runtime radar reflexes (HA number entities → sense_config).
// Latched by the callback, drained from the main loop — the exact pattern
// canary-vision's detection dials use. -1 = nothing pending.
static volatile long s_pending_cfg_debounce = -1;
static volatile long s_pending_cfg_clear = -1;
static volatile long s_pending_cfg_stall = -1;
static volatile long s_pending_cfg_near = -1;
static volatile long s_pending_cfg_mid = -1;
static volatile long s_pending_cfg_vlock = -1;
static volatile long s_pending_cfg_vlost = -1;
static volatile long s_pending_cfg_bmin = -1;
static volatile long s_pending_cfg_bmax = -1;
static volatile long s_pending_cfg_hmin = -1;
static volatile long s_pending_cfg_hmax = -1;

// Parse a small non-negative integer from an MQTT payload (HA number
// entities send plain decimals, possibly as "12.0", possibly quoted).
// Returns -1 on junk, non-finite, or out-of-range input so a mangled
// payload can never latch a value. Mirrors canary-vision's parser.
static long parse_cfg_number(const uint8_t* payload, unsigned int len, long max_value) {
  char buf[24];
  unsigned int n = 0;
  for (unsigned int i = 0; i < len && n < sizeof(buf) - 1; i++) {
    const char c = (char)payload[i];
    if (c == ' ' || c == '\t' || c == '"' || c == '\r' || c == '\n') continue;
    buf[n++] = c;
  }
  buf[n] = '\0';
  if (!n) return -1;
  char* end = nullptr;
  const double d = strtod(buf, &end);
  if (!end || end == buf) return -1;
  // Accept a trailing ".0" tail strtod consumed; reject other trailing junk.
  if (*end != '\0') return -1;
  if (!(d >= 0) || d != d || d > (double)max_value) return -1;
  return (long)d;
}

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

  // Runtime radar reflexes (HA number entities). Max values here are only the
  // parser's sanity ceiling; sense_config's setters clamp to the real bounds.
  if (strcmp(topic, g_topics.cfg_debounce_cmd) == 0) {
    s_pending_cfg_debounce = parse_cfg_number(payload, len, 3000);
    return;
  }
  if (strcmp(topic, g_topics.cfg_clear_cmd) == 0) {
    s_pending_cfg_clear = parse_cfg_number(payload, len, 10000);
    return;
  }
  if (strcmp(topic, g_topics.cfg_stall_cmd) == 0) {
    s_pending_cfg_stall = parse_cfg_number(payload, len, 20000);
    return;
  }
  if (strcmp(topic, g_topics.cfg_near_cmd) == 0) {
    s_pending_cfg_near = parse_cfg_number(payload, len, 400);
    return;
  }
  if (strcmp(topic, g_topics.cfg_mid_cmd) == 0) {
    s_pending_cfg_mid = parse_cfg_number(payload, len, 600);
    return;
  }
  if (strcmp(topic, g_topics.cfg_vlock_cmd) == 0) {
    s_pending_cfg_vlock = parse_cfg_number(payload, len, 15000);
    return;
  }
  if (strcmp(topic, g_topics.cfg_vlost_cmd) == 0) {
    s_pending_cfg_vlost = parse_cfg_number(payload, len, 20000);
    return;
  }
  if (strcmp(topic, g_topics.cfg_bmin_cmd) == 0) {
    s_pending_cfg_bmin = parse_cfg_number(payload, len, 15);
    return;
  }
  if (strcmp(topic, g_topics.cfg_bmax_cmd) == 0) {
    s_pending_cfg_bmax = parse_cfg_number(payload, len, 60);
    return;
  }
  if (strcmp(topic, g_topics.cfg_hmin_cmd) == 0) {
    s_pending_cfg_hmin = parse_cfg_number(payload, len, 79);
    return;
  }
  if (strcmp(topic, g_topics.cfg_hmax_cmd) == 0) {
    s_pending_cfg_hmax = parse_cfg_number(payload, len, 220);
    return;
  }

  const bool is_install = (strcmp(topic, g_topics.update_cmd) == 0);
  const bool is_auto = (strcmp(topic, g_topics.update_auto_cmd) == 0);
  const bool is_identify = (strcmp(topic, g_topics.identify_cmd) == 0);
  if (!is_install && !is_auto && !is_identify) return;

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

bool take_pending_identify() {
  if (!s_pending_identify) return false;
  s_pending_identify = false;
  return true;
}

static long take_pending(volatile long& slot) {
  const long v = slot;
  slot = -1;
  return v;
}

long take_pending_cfg_debounce() { return take_pending(s_pending_cfg_debounce); }
long take_pending_cfg_clear()    { return take_pending(s_pending_cfg_clear); }
long take_pending_cfg_stall()    { return take_pending(s_pending_cfg_stall); }
long take_pending_cfg_near()     { return take_pending(s_pending_cfg_near); }
long take_pending_cfg_mid()      { return take_pending(s_pending_cfg_mid); }
long take_pending_cfg_vlock()    { return take_pending(s_pending_cfg_vlock); }
long take_pending_cfg_vlost()    { return take_pending(s_pending_cfg_vlost); }
long take_pending_cfg_bmin()     { return take_pending(s_pending_cfg_bmin); }
long take_pending_cfg_bmax()     { return take_pending(s_pending_cfg_bmax); }
long take_pending_cfg_hmin()     { return take_pending(s_pending_cfg_hmin); }
long take_pending_cfg_hmax()     { return take_pending(s_pending_cfg_hmax); }

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

void publish_health_retained(const Topics& topics) {
  // Same field set as canary-wap's mains-powered health publish: HA's
  // health handler reads memory/uptime/firmware and — crucially —
  // TOFU-pins the device from `public_key` on first sight.
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
           (unsigned long)(ms_now() / 1000UL),
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

  // Identical envelope to canary-wap's publish_chain: v/length/latest_hash
  // (+ legacy "algorithm") always; alg/fp/sig when signing is available.
  // HA's verify_chain rebuilds the canonical from (device_id-from-topic,
  // length, latest_hash) and verifies against the pinned pubkey.
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

void ha_discovery_publish_once(const Topics& topics) {
  if (discovery_done) return;
  canary::ha::publish_discovery(mqtt, topics);
  discovery_done = true;
}

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

  // Identify button: re-subscribe so the wizard's blink request always
  // reaches a connected device.
  mqtt.subscribe(g_topics.identify_cmd, 1);

  // Runtime radar reflexes: re-subscribe the command topics and reconcile
  // the retained snapshot from the NVS-backed values.
  mqtt.subscribe(g_topics.cfg_debounce_cmd, 1);
  mqtt.subscribe(g_topics.cfg_clear_cmd, 1);
  mqtt.subscribe(g_topics.cfg_stall_cmd, 1);
  mqtt.subscribe(g_topics.cfg_near_cmd, 1);
  mqtt.subscribe(g_topics.cfg_mid_cmd, 1);
  mqtt.subscribe(g_topics.cfg_vlock_cmd, 1);
  mqtt.subscribe(g_topics.cfg_vlost_cmd, 1);
  mqtt.subscribe(g_topics.cfg_bmin_cmd, 1);
  mqtt.subscribe(g_topics.cfg_bmax_cmd, 1);
  mqtt.subscribe(g_topics.cfg_hmin_cmd, 1);
  mqtt.subscribe(g_topics.cfg_hmax_cmd, 1);
  publish_sense_cfg_retained(g_topics);
  if (s_update_state_set) {
    publish_checked("OTA", g_topics.update_state, s_update_state_cache, true);
  }
  if (s_update_auto_cache >= 0) {
    publish_checked("OTA", g_topics.update_auto,
                    s_update_auto_cache ? "ON" : "OFF", true);
  }
  return true;
}

bool publish_sense_cfg_retained(const Topics& topics) {
  if (!mqtt.connected()) return false;
  const auto& s = canary::cfg::sense();
  char msg[320];
  snprintf(msg, sizeof(msg),
           "{"
           "\"debounce_ms\":%lu,"
           "\"clear_ms\":%lu,"
           "\"stall_ms\":%lu,"
           "\"near_cm\":%lu,"
           "\"mid_cm\":%lu,"
           "\"vitals_lock_ms\":%lu,"
           "\"vitals_lost_ms\":%lu,"
           "\"breath_min_bpm\":%lu,"
           "\"breath_max_bpm\":%lu,"
           "\"heart_min_bpm\":%lu,"
           "\"heart_max_bpm\":%lu"
           "}",
           (unsigned long)s.present_debounce_ms,
           (unsigned long)s.clear_timeout_ms,
           (unsigned long)s.stall_timeout_ms,
           (unsigned long)s.near_cm,
           (unsigned long)s.mid_cm,
           (unsigned long)s.vitals_lock_ms,
           (unsigned long)s.vitals_lost_ms,
           (unsigned long)s.breath_min_bpm,
           (unsigned long)s.breath_max_bpm,
           (unsigned long)s.heart_min_bpm,
           (unsigned long)s.heart_max_bpm);
  return publish_checked("CFG", topics.cfg_state, msg, true);
}

bool publish_identify_echo(const Topics& topics, bool active) {
  if (!mqtt.connected()) return false;
  // Non-retained on purpose: the echo marks a live blink window, not a
  // state — a card should only pulse while the LED is actually flashing.
  return publish_checked("IDFY", topics.identify_echo, active ? "on" : "off",
                         false);
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
