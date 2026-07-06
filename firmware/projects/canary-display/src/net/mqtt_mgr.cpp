// src/net/mqtt_mgr.cpp — broker link: fleet subscriber + own liveness.
//
// The display is the fleet's MQTT *consumer*: it subscribes to the
// securacv/+/{status,availability,health,events,tamper,chain,state}
// wildcards, parses each payload (ArduinoJson), and feeds already-typed
// values into the fleet model. Its own publish surface is deliberately
// small — a retained status heartbeat with LWT, a retained health row, and
// the shared OTA engine's update entity — because a display witnesses
// nothing and should say nothing it can't stand behind.
#include "canary/net/mqtt_mgr.h"

#include <Arduino.h>
#include <cstring>

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "canary/config.h"
#include "canary/log.h"
#include "canary/version.h"
#include "canary/runtime_config.h"  // NVS-backed identity + broker credentials
#include "canary/diagnostics.h"     // heap health for the status heartbeat
#include "canary/trust.h"           // TOFU pins + Ed25519 chain verify
#include "canary/fleet/fleet_instance.h"
#include "canary/net/wifi_mgr.h"    // RSSI + link state
#include "identity/device_pseudonym.h"  // MAC-free client-ID suffix (Invariant III)

namespace canary::net {

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);
static Topics g_topics{};

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

// ── Topic parsing ───────────────────────────────────────────────────────

// Split "securacv/<device_id>/<suffix...>" in place-free fashion.
// Returns false for topics outside the securacv/ root.
static bool split_topic(const char* topic, char* id_out, size_t id_cap,
                        const char** suffix_out) {
  static const char ROOT[] = "securacv/";
  const size_t root_len = sizeof(ROOT) - 1;
  if (!topic || strncmp(topic, ROOT, root_len) != 0) return false;
  const char* p = topic + root_len;
  const char* slash = strchr(p, '/');
  if (!slash || slash == p) return false;
  const size_t id_len = (size_t)(slash - p);
  if (id_len + 1 > id_cap) return false;
  memcpy(id_out, p, id_len);
  id_out[id_len] = '\0';
  *suffix_out = slash + 1;
  return true;
}

static bool payload_is(const uint8_t* payload, unsigned int len, const char* tok) {
  const size_t tl = strlen(tok);
  return len == tl && memcmp(payload, tok, tl) == 0;
}

// ── Fleet dispatch ──────────────────────────────────────────────────────

static void dispatch_fleet(const char* device_id, const char* suffix,
                           const uint8_t* payload, unsigned int len) {
  using canary::fleet::the_fleet;
  const uint32_t now = canary::ms_now();
  auto& fleet = the_fleet();

  // availability is a bare string, not JSON.
  if (strcmp(suffix, "availability") == 0) {
    if (payload_is(payload, len, "online"))       fleet.on_availability(device_id, true, now);
    else if (payload_is(payload, len, "offline")) fleet.on_availability(device_id, false, now);
    return;
  }

  // Everything else is a JSON object. Filter chatter cheaply first.
  const bool want =
      strcmp(suffix, "status") == 0 || strcmp(suffix, "health") == 0 ||
      strcmp(suffix, "events") == 0 || strcmp(suffix, "tamper") == 0 ||
      strcmp(suffix, "chain") == 0  || strcmp(suffix, "state") == 0;
  if (!want) return;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload, len);
  if (err) {
    // Malformed payloads still prove the device is alive on the wire.
    fleet.on_activity(device_id, now);
    return;
  }

  if (strcmp(suffix, "status") == 0) {
    // Both families: sense/vision/wap carry "status":"online|offline";
    // the ACTIVE canary tree's status has no such field (its LWT rides
    // the availability topic) — treat that as plain liveness.
    const char* st = doc["status"] | (const char*)nullptr;
    const char* dt = doc["device_type"] | "";
    const int batt = doc["battery_soc"] | doc["battery"] | -1;
    if (st) fleet.on_status(device_id, dt, strcmp(st, "offline") != 0, batt, now);
    else    fleet.on_status(device_id, dt, true, batt, now);
    return;
  }

  if (strcmp(suffix, "health") == 0) {
    const int batt = doc["battery"] | -1;
    const bool batt_present = doc["battery_present"] | (batt >= 0);
    const char* fw = doc["firmware_version"] | doc["firmware"] | "";
    fleet.on_health(device_id, batt, batt_present, fw, now);
    // Trust surface: TOFU-pin the pubkey; a mismatch flags the pin store
    // and the next chain evaluation reports Failed.
    const char* pk = doc["public_key"] | (const char*)nullptr;
    if (pk && pk[0]) canary::trust::note_pubkey(device_id, pk);
    return;
  }

  if (strcmp(suffix, "events") == 0) {
    // Vocabulary differs per variant: sense uses "event", wap "event_type",
    // vision "event"/"state". Fall through the spellings; classify_event
    // owns severity.
    const char* name = doc["event"] | doc["event_type"] | doc["kind"] | "event";
    const bool signed_flag = (doc["signed"] | false) || !doc["sig"].isNull();
    fleet.on_event(device_id, name, signed_flag, now);
    return;
  }

  if (strcmp(suffix, "tamper") == 0) {
    // ACTIVE tree, retained: {"state":"on","confidence":0.87,"kind":"..."}
    const char* st = doc["state"] | "";
    const char* kind = doc["kind"] | "tamper";
    fleet.on_tamper(device_id, strcmp(st, "on") == 0, kind, now);
    return;
  }

  if (strcmp(suffix, "chain") == 0) {
    const uint32_t length = doc["length"] | 0UL;
    const char* hash = doc["latest_hash"] | "";
    const char* sig = doc["sig"] | "";
    const auto verdict = canary::trust::evaluate_chain(device_id, length, hash, sig);
    fleet.on_chain(device_id, length, verdict, now);
    return;
  }

  // "state": retained per-variant snapshot — liveness plus device_type.
  const char* dt = doc["device_type"] | "";
  if (dt[0]) fleet.on_status(device_id, dt, true, -1, now);
  else fleet.on_activity(device_id, now);
}

// ── MQTT callback ───────────────────────────────────────────────────────

static void on_mqtt_message(char* topic, uint8_t* payload, unsigned int len) {
  if (!topic || !payload) return;

  // Update-entity commands (exact-match own topics).
  if (strcmp(topic, g_topics.update_cmd) == 0) {
    // Trim leading whitespace/quotes; require a token boundary so a
    // mangled payload can't trigger a flash cycle.
    const char* p = (const char*)payload;
    unsigned int n = len;
    while (n > 0 && (*p == ' ' || *p == '\t' || *p == '"')) { p++; n--; }
    if (n >= 7 && memcmp(p, "install", 7) == 0 &&
        (n == 7 || p[7] == '"' || p[7] == ' ' || p[7] == '}')) {
      s_pending_install = true;
    }
    return;
  }
  if (strcmp(topic, g_topics.update_auto_cmd) == 0) {
    if (payload_is(payload, len, "ON") || payload_is(payload, len, "on"))   s_pending_auto = 1;
    if (payload_is(payload, len, "OFF") || payload_is(payload, len, "off")) s_pending_auto = 0;
    return;
  }

  char device_id[48];
  const char* suffix = nullptr;
  if (!split_topic(topic, device_id, sizeof(device_id), &suffix)) return;

  // Drop our own echo — the display must never count itself as a witness.
  if (strcmp(device_id, canary::cfg::get().device_id) == 0) return;

  dispatch_fleet(device_id, suffix, payload, len);
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
  const auto& fleet = canary::fleet::the_fleet();
  const uint32_t now = canary::ms_now();
  char msg[448];
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
           "\"fleet_devices\":%d,"
           "\"fleet_worst\":\"%s\","
           "\"fleet_all_verified\":%s,"
           "\"ts_ms\":%lu"
           "}",
           canary::cfg::get().device_id, DEVICE_TYPE, status,
           WiFi.localIP().toString().c_str(),
           wifi_rssi(),
           (unsigned long)d.free_heap,
           (unsigned long)d.min_heap,
           canary::diag::level_name(d.level),
           fleet.count(),
           canary::fleet::sev_name(fleet.worst(now)),
           fleet.all_verified() ? "true" : "false",
           (unsigned long)now);
  publish_checked("STATUS", topics.status, msg, true);
}

void publish_health_retained(const Topics& topics) {
  // Same field set as the other variants' mains-powered health publish so
  // fleet tooling renders one uniform table. A display has no witness key,
  // so there is deliberately no public_key field to pin.
  char msg[256];
  const int n = snprintf(msg, sizeof(msg),
           "{"
           "\"battery\":100,"
           "\"battery_present\":false,"
           "\"memory_free\":%lu,"
           "\"uptime\":%lu,"
           "\"firmware_version\":\"%s\""
           "}",
           (unsigned long)ESP.getFreeHeap(),
           (unsigned long)(canary::ms_now() / 1000UL),
           CANARY_FW_VERSION);
  if (n <= 0 || (size_t)n >= sizeof(msg)) return;
  publish_checked("HEALTH", topics.health, msg, true);
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

  // Privacy (Invariant III): the MQTT client ID reaches the broker, so its
  // unique suffix is the salted device pseudonym — never the raw efuse MAC.
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

  // Fleet wildcards. Retained status/health/chain/tamper replay instantly,
  // so the model repopulates within one broker round-trip of a reconnect.
  mqtt.subscribe(FleetSubs::STATUS, 1);
  mqtt.subscribe(FleetSubs::AVAILABILITY, 1);
  mqtt.subscribe(FleetSubs::HEALTH, 1);
  mqtt.subscribe(FleetSubs::EVENTS, 1);
  mqtt.subscribe(FleetSubs::TAMPER, 1);
  mqtt.subscribe(FleetSubs::CHAIN, 1);
  mqtt.subscribe(FleetSubs::STATE, 0);

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
  return true;
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
