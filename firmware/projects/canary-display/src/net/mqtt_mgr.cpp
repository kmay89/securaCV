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
#include <time.h>

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "canary/power_events_glue.h"

#include "canary/config.h"
#include "canary/log.h"
#include "canary/version.h"
#include "canary/runtime_config.h"  // NVS-backed identity + broker credentials
#include "canary/diagnostics.h"     // heap health for the status heartbeat
#include "canary/trust.h"           // TOFU pins + Ed25519 chain verify
#include "canary/fleet/fleet_instance.h"
#include "canary/net/wifi_mgr.h"    // RSSI + link state
#if defined(FEATURE_HUB_WEATHER) && FEATURE_HUB_WEATHER
#include "canary/care/bedside.h"    // nightstand wave: hub weather feed
#endif
#if defined(FEATURE_WAKE_ALARM) && FEATURE_WAKE_ALARM
#include "canary/care/wake_glue.h"  // nightstand wave: alarm config
#endif
#include "identity/device_pseudonym.h"  // MAC-free client-ID suffix (Invariant III)

namespace canary::net {

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);
static Topics g_topics{};

// Broker endpoint — rebindable (fleet discovery). PubSubClient::setServer
// stores the caller's pointer, so the storage must be static and stable.
static char s_broker_host[64] = {0};
static uint16_t s_broker_port = 1883;

// Inbound firmware-update commands. The PubSubClient callback fires inside
// mqtt.loop() on the main task, but flash-cycle decisions belong to the OTA
// glue (ota_mgr) — the callback only latches these flags and ota_loop()
// drains them. Same pattern as the other Canary variants.
static volatile bool s_pending_install = false;
static volatile int s_pending_auto = -1;
static volatile int s_pending_channel = -1;

// Cached retained payloads, republished on every reconnect so HA's update
// entity and auto-update switch never sit at "unknown" after a broker
// restart.
static char s_update_state_cache[640] = {0};
static bool s_update_state_set = false;
static int s_update_auto_cache = -1;
static int s_update_channel_cache = -1;

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

// canary-sense coarse-vocabulary decoders (the ONLY words that ever cross the
// wire about what the radar saw — see the device's privacy chokepoint). An
// unrecognized word maps to the safe "unknown/0" slot, never a crash.
static uint8_t str_to_presence(const char* s) {
  if (!s) return 0;
  if (strcmp(s, "present") == 0) return 2;
  if (strcmp(s, "clear") == 0)   return 1;
  return 0;  // "unknown"
}
static uint8_t str_to_occupants(const char* s) {
  if (!s) return 0;
  if (strcmp(s, "2+") == 0) return 2;
  if (strcmp(s, "1") == 0)  return 1;
  return 0;  // "0"
}
static uint8_t str_to_range(const char* s) {
  if (!s) return 0;
  if (strcmp(s, "near") == 0) return 1;
  if (strcmp(s, "mid") == 0)  return 2;
  if (strcmp(s, "far") == 0)  return 3;
  return 0;  // "unknown"
}
static int constrain_u8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

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
      strcmp(suffix, "chain") == 0  || strcmp(suffix, "state") == 0 ||
      strcmp(suffix, "meta") == 0;
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
    // Self-reported WiFi RSSI rides the status row (every variant publishes
    // it) — the Roll Call page's signal column.
    if (doc["rssi"].is<int>()) fleet.on_rssi(device_id, doc["rssi"].as<int>(), now);
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
    // WAP reconnect backfill republishes missed events with replay:true. The
    // display renders LIVE state (millis-age) and journals at wall-clock time,
    // so re-ingesting backfilled history would both mis-age the glance log and
    // duplicate entries in the time machine at the wrong (reconnect) time. Drop
    // replays — the journal's durability is its own (persistence), not re-heard
    // from the WAP.
    if (doc["replay"] | false) return;
    // Vocabulary differs per variant: sense uses "event", wap "event_type",
    // vision "event"/"state". Fall through the spellings; classify_event
    // owns severity.
    const char* name = doc["event"] | doc["event_type"] | doc["kind"] | "event";
    // The WAP's system.integrity rows mark themselves type:"tamper" and
    // carry the KIND as the event word ("watchdog", "sd_remove") — words
    // the severity ladder was never taught, so a crash-reboot classified
    // through the "boot" rung and painted the glass GREEN. Rebuild the
    // name as tamper_<kind>: the ladder's worst-first "tamper" rung
    // classifies it, the journal still names the kind, and a kind minted
    // after this build inherits the right severity by construction.
    char tamper_name[48];
    if (strcmp(doc["type"] | "", "tamper") == 0 &&
        strncmp(name, "tamper", 6) != 0) {
      snprintf(tamper_name, sizeof(tamper_name), "tamper_%s", name);
      name = tamper_name;
    }
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
    const char* fp = doc["fp"] | "";
    const auto verdict = canary::trust::evaluate_chain(device_id, length, hash, sig);
    // Keep the verbatim payload (Proof-on-Glass QR body) and the
    // fingerprint (the off-grid Chirp correlator).
    fleet.on_chain(device_id, length, verdict, now, (const char*)payload, len, fp);
    return;
  }

  if (strcmp(suffix, "meta") == 0) {
    // Rooms & names (spec §8): retained, human-authored.
    fleet.on_meta(device_id, doc["name"] | "", doc["room"] | "", now);
    return;
  }

  // "state": retained per-variant snapshot — liveness plus device_type,
  // plus the wellbeing surface (spec §9) when the witness publishes one.
  const char* dt = doc["device_type"] | "";
  if (dt[0]) fleet.on_status(device_id, dt, true, -1, now);
  else fleet.on_activity(device_id, now);
  if (doc["breathing_locked"].is<bool>()) {
    fleet.on_wellbeing(device_id, doc["breathing_locked"].as<bool>(), now);
  }
  // Radar claim surface (canary-sense): the coarse presence/count/range
  // vocabulary + lux + P1-gated BPM. Only a payload that actually carries the
  // radar vocabulary is treated as a sense row, so non-radar variants never
  // get spuriously marked card-bearing. Raw distance/per-target data never
  // appear on the wire — the device coarsened them at its own chokepoint.
  // The coarse presence vocabulary rides "presence_state" ("unknown"/"clear"/
  // "present"); "presence" itself is the HA-facing bool. Detect a radar row on
  // any of the radar-only keys.
  if (doc["presence_state"].is<const char*>() || doc["range"].is<const char*>() ||
      doc["radar_ok"].is<bool>()) {
    canary::fleet::SenseState ss;
    ss.presence  = str_to_presence(doc["presence_state"] | "unknown");
    ss.occupants = str_to_occupants(doc["occupants"] | "0");
    ss.range     = str_to_range(doc["range"] | "unknown");
    ss.radar_ok  = doc["radar_ok"] | false;
    ss.frame_errors = doc["frame_errors"] | 0UL;
    // lux may serialize as "138.0" (float) or "138" (int); accept either.
    if (doc["lux"].is<float>() || doc["lux"].is<int>()) {
      const float lx = doc["lux"].as<float>();
      ss.have_lux = true;
      ss.lux = (lx < 0) ? -1 : (int)(lx + 0.5f);
    }
    // BPM rides the state payload only on a wellbeing build, under the same
    // gate as breathing_locked — so its presence is the reliable "this device
    // offers BPM" signal (the values themselves publish as a number when the
    // lock holds, or JSON null otherwise, which is_int cleanly distinguishes).
    if (doc["breathing_locked"].is<bool>()) {
      ss.have_bpm = true;
      const bool br_num = doc["breath_bpm"].is<int>();
      const bool hr_num = doc["heart_bpm"].is<int>();
      ss.bpm_valid = br_num && hr_num;  // both numeric == lock holds
      if (br_num) ss.breath_bpm = (uint8_t)constrain_u8(doc["breath_bpm"] | 0);
      if (hr_num) ss.heart_bpm  = (uint8_t)constrain_u8(doc["heart_bpm"] | 0);
    }
    fleet.on_sense_state(device_id, ss, now);
  }
  // Pool water-chemistry surface (canary-pool): pH / ORP / water temp / TDS
  // (docs/research/pool_water_monitor.md §6). A chemistry row is recognized
  // either by the device_type (so an all-null "flow stopped" snapshot — where
  // every value is JSON null, §8 — is still a pool row and clears the cards)
  // or by any numeric chemistry key (so a pool node is recognized before its
  // type is known). A non-pool variant carries neither, so it is never marked
  // pool-bearing. Each value is taken only when it is a real number; a null or
  // omitted key leaves have_* false, and on_pool_state OVERWRITES every flag,
  // so a value that was valid and is now null renders "—", never stale-good.
  {
    const bool ph_num  = doc["ph"].is<float>() || doc["ph"].is<int>();
    const bool orp_num = doc["orp"].is<float>() || doc["orp"].is<int>();
    const bool wt_num  = doc["water_temp_c"].is<float>() ||
                         doc["water_temp_c"].is<int>();
    const bool tds_num = doc["tds"].is<float>() || doc["tds"].is<int>();
    const bool pool_type = strcmp(dt, "canary-pool") == 0 ||
                           strcmp(dt, "canary_pool") == 0;
    if (pool_type || ph_num || orp_num || wt_num || tds_num) {
      canary::fleet::PoolState ps;
      if (ph_num) {
        ps.have_ph = true;
        ps.ph_x10 = (int16_t)(doc["ph"].as<float>() * 10.0f + 0.5f);
      }
      if (orp_num) {
        ps.have_orp = true;
        ps.orp_mv = (int16_t)doc["orp"].as<int>();
      }
      if (wt_num) {
        ps.have_water_temp = true;
        ps.water_temp_c10 =
            (int16_t)(doc["water_temp_c"].as<float>() * 10.0f + 0.5f);
      }
      if (tds_num) {
        ps.have_tds = true;
        ps.tds_ppm = (int16_t)doc["tds"].as<int>();
      }
      fleet.on_pool_state(device_id, ps, now);
    }
  }
  // Room comfort, when the variant reports it (spellings differ; °C either
  // way). Tenths keep 21.5° honest on the glass.
  {
    bool have_temp = false;
    int temp_c10 = 0;
    if (doc["temperature"].is<float>()) {
      temp_c10 = (int)(doc["temperature"].as<float>() * 10.0f);
      have_temp = true;
    } else if (doc["temp_c"].is<float>()) {
      temp_c10 = (int)(doc["temp_c"].as<float>() * 10.0f);
      have_temp = true;
    }
    const int rh = doc["humidity"] | -1;
    if (have_temp || rh >= 0) {
      fleet.on_comfort(device_id, temp_c10, have_temp, rh, now);
    }
  }
}

// ── MQTT callback ───────────────────────────────────────────────────────

#if defined(FEATURE_ACK_SYNC) && FEATURE_ACK_SYNC
// Household ack-sync: apply a remote display's acknowledge. Epoch-anchored
// so retained replays and clock differences stay honest — and the ack maps
// into this device's millis domain at its true age, so the local ack-hold
// window and new-alert invalidation behave exactly as if the long-press
// had happened here.
static uint32_t s_last_ack_epoch = 0;

static void handle_fleet_ack(const uint8_t* payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  const uint32_t at = doc["at"] | 0UL;
  const char* by = doc["by"] | "";
  if (at == 0 || strcmp(by, canary::cfg::get().device_id) == 0) return;

  const time_t epoch = time(nullptr);
  if (epoch < 1700000000) return;             // no local clock — stay honest
  if (at <= s_last_ack_epoch) return;         // stale / retained replay

  // Clock skew tolerance: two SNTP-synced siblings can disagree by a few
  // seconds — a slightly future-stamped ack is genuine, treat it as "now".
  // Beyond the tolerance it's malformed/malicious: reject. (review catch)
  uint32_t age_s;
  if (at > (uint32_t)epoch) {
    if (at - (uint32_t)epoch > 10) return;    // future beyond skew tolerance
    age_s = 0;
  } else {
    age_s = (uint32_t)epoch - at;
  }
  // Expiry compared in SECONDS: age_s*1000 would wrap uint32 for acks older
  // than ~49.7 days and resurrect them as fresh. (review catch)
  if (age_s >= (uint32_t)(CD_ACK_HOLD_MS / 1000UL)) return;  // expired

  s_last_ack_epoch = at;
  const uint32_t now_ms = canary::ms_now();
  // Attribution travels with the ack — the glass can say WHICH display
  // quieted the house (the who-disarmed audit line security panels keep).
  canary::fleet::the_fleet().acknowledge_by(now_ms - age_s * 1000UL, by);
  log_line("ACK", "Household acknowledge received (synced from a sibling).");
}
#endif

static void on_mqtt_message(char* topic, uint8_t* payload, unsigned int len) {
  if (!topic || !payload) return;

#if defined(FEATURE_ACK_SYNC) && FEATURE_ACK_SYNC
  if (strcmp(topic, FleetSubs::FLEET_ACK) == 0) {
    handle_fleet_ack(payload, len);
    return;
  }
#endif

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
  if (strcmp(topic, g_topics.update_channel_cmd) == 0) {
    // Exact tokens only — an unrecognized payload changes nothing, and
    // "release" is the value everything unclear resolves to elsewhere
    // (the engine reads unknown NVS as RELEASE too).
    if (payload_is(payload, len, "release")) s_pending_channel = 0;
    if (payload_is(payload, len, "dev"))     s_pending_channel = 1;
    return;
  }
#if defined(FEATURE_HUB_WEATHER) && FEATURE_HUB_WEATHER
  // Nightstand wave: the hub's ONE retained forecast blob.
  if (strcmp(topic, FleetSubs::WEATHER) == 0) {
    canary::care::bedside_on_weather((const char*)payload, len);
    return;
  }
#endif
#if defined(FEATURE_WAKE_ALARM) && FEATURE_WAKE_ALARM
  if (strcmp(topic, g_topics.alarm_set) == 0) {
    canary::care::wake_alarm_on_config((const char*)payload, len);
    return;
  }
#endif

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

int take_pending_channel() {
  const int v = s_pending_channel;
  s_pending_channel = -1;
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
  strncpy(s_broker_host, cfg.mqtt_host, sizeof(s_broker_host) - 1);
  s_broker_host[sizeof(s_broker_host) - 1] = '\0';
  s_broker_port = cfg.mqtt_port ? cfg.mqtt_port : 1883;
  mqtt.setServer(s_broker_host, s_broker_port);
  mqtt.setBufferSize(MQTT_BUFFER_BYTES);
  mqtt.setCallback(on_mqtt_message);
}

void mqtt_set_broker(const char* host, uint16_t port) {
  if (!host || !host[0]) return;
  const bool same = (strcmp(s_broker_host, host) == 0 && s_broker_port == port);
  strncpy(s_broker_host, host, sizeof(s_broker_host) - 1);
  s_broker_host[sizeof(s_broker_host) - 1] = '\0';
  s_broker_port = port ? port : 1883;
  if (mqtt.connected()) mqtt.disconnect();
  mqtt.setServer(s_broker_host, s_broker_port);

  // Persist to the keys runtime_config reads so the endpoint survives a
  // reboot. Note the precedence rule there: a REAL compiled MQTT_HOST wins
  // over NVS at boot — a unit with hand-compiled broker creds re-fails and
  // re-discovers after ~2 min instead of silently forgetting its build.
  Preferences prefs;
  if (prefs.begin("securacv", /*readOnly=*/false)) {
    prefs.putString("mqtt_host", s_broker_host);
    prefs.putUShort("mqtt_port", s_broker_port);
    prefs.end();
  }

  if (!same) {
    log_header("MQTT");
    canary::dbg_serial().printf("Broker rebound to %s:%u (persisted)\n",
                                s_broker_host, (unsigned)s_broker_port);
  }
}

const char* mqtt_broker_host() { return s_broker_host; }
uint16_t mqtt_broker_port() { return s_broker_port; }

bool mqtt_broker_is_placeholder() {
  return s_broker_host[0] == '\0' ||
         strcmp(s_broker_host, "127.0.0.1") == 0 ||
         strcmp(s_broker_host, "ci") == 0 ||
         strcmp(s_broker_host, "ci-placeholder") == 0;
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
  // so there is deliberately no public_key field to pin. The power lineage
  // flags are held for an hour after an incident boot so a hub that reboots
  // slower than this display still latches HA's Power Loss sensor, then
  // clears it when the flags drop (same contract as the canary base tree).
  char msg[320];
  const int n = snprintf(msg, sizeof(msg),
           "{"
           "\"battery\":100,"
           "\"battery_present\":false,"
           "\"memory_free\":%lu,"
           "\"uptime\":%lu,"
           "\"firmware_version\":\"%s\","
           "\"power_loss_detected\":%s,"
           "\"unexpected_reboot\":%s"
           "}",
           (unsigned long)ESP.getFreeHeap(),
           (unsigned long)(canary::ms_now() / 1000UL),
           CANARY_FW_VERSION,
           cd_pe::health_power_flag(millis()) ? "true" : "false",
           cd_pe::health_fault_flag(millis()) ? "true" : "false");
  if (n <= 0 || (size_t)n >= sizeof(msg)) return;
  publish_checked("HEALTH", topics.health, msg, true);
}

void publish_fleet_ack(uint32_t epoch_s) {
#if defined(FEATURE_ACK_SYNC) && FEATURE_ACK_SYNC
  if (!mqtt.connected() || epoch_s == 0) return;
  s_last_ack_epoch = epoch_s;  // don't re-apply our own retained echo
  char msg[128];
  snprintf(msg, sizeof(msg), "{\"at\":%lu,\"by\":\"%s\"}",
           (unsigned long)epoch_s, canary::cfg::get().device_id);
  publish_checked("ACK", FleetSubs::FLEET_ACK, msg, true);
#else
  (void)epoch_s;
#endif
}

void publish_fleet_escalation(uint32_t epoch_s, const char* worst,
                              const char* witness) {
  if (!mqtt.connected() || epoch_s == 0) return;
  // The witness name is human-authored (retained meta payload) — escape it
  // for JSON or a name like `Living "Room"` breaks every downstream parser.
  // Worst case doubles: 32 name chars -> 64 + NUL fits esc (review catch).
  char esc[66];
  {
    size_t o = 0;
    for (const char* p = witness ? witness : ""; *p && o + 2 < sizeof(esc);
         p++) {
      const unsigned char c = (unsigned char)*p;
      if (c == '"' || c == '\\') esc[o++] = '\\';
      else if (c < 0x20) continue;  // control chars have no place in a name
      esc[o++] = (char)c;
      if (o >= 64) break;  // cap the name at 32 source chars' worst case
    }
    esc[o] = '\0';
  }
  char msg[224];
  snprintf(msg, sizeof(msg),
           "{\"at\":%lu,\"by\":\"%s\",\"worst\":\"%s\",\"witness\":\"%s\"}",
           (unsigned long)epoch_s, canary::cfg::get().device_id,
           worst ? worst : "", esc);
  // Deliberately NOT retained: an escalation is a moment, not a state — a
  // display that reconnects hours later must not re-fire the phone tree.
  publish_checked("ESCALATE", FleetSubs::FLEET_ESCALATION, msg, false);
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
  canary::dbg_serial().printf("Connecting %s:%u as %s ...\n", s_broker_host,
                              (unsigned)s_broker_port, clientId.c_str());

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
#if defined(FEATURE_ACK_SYNC) && FEATURE_ACK_SYNC
  mqtt.subscribe(FleetSubs::FLEET_ACK, 1);
#endif
  mqtt.subscribe(FleetSubs::AVAILABILITY, 1);
  mqtt.subscribe(FleetSubs::HEALTH, 1);
  mqtt.subscribe(FleetSubs::EVENTS, 1);
  mqtt.subscribe(FleetSubs::TAMPER, 1);
  mqtt.subscribe(FleetSubs::CHAIN, 1);
  mqtt.subscribe(FleetSubs::STATE, 0);
  mqtt.subscribe(FleetSubs::META, 1);

  // Firmware update entity: re-subscribe the command topics (the broker
  // may have dropped them) and reconcile the retained states.
  mqtt.subscribe(g_topics.update_cmd, 1);
  mqtt.subscribe(g_topics.update_auto_cmd, 1);
  mqtt.subscribe(g_topics.update_channel_cmd, 1);
#if defined(FEATURE_HUB_WEATHER) && FEATURE_HUB_WEATHER
  mqtt.subscribe(FleetSubs::WEATHER, 1);
#endif
#if defined(FEATURE_WAKE_ALARM) && FEATURE_WAKE_ALARM
  mqtt.subscribe(g_topics.alarm_set, 1);
#endif
  if (s_update_state_set) {
    publish_checked("OTA", g_topics.update_state, s_update_state_cache, true);
  }
  if (s_update_auto_cache >= 0) {
    publish_checked("OTA", g_topics.update_auto,
                    s_update_auto_cache ? "ON" : "OFF", true);
  }
  if (s_update_channel_cache >= 0) {
    publish_checked("OTA", g_topics.update_channel,
                    s_update_channel_cache ? "dev" : "release", true);
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

bool publish_update_channel_retained(const Topics& topics, bool dev) {
  s_update_channel_cache = dev ? 1 : 0;
  if (!mqtt.connected()) return false;
  return publish_checked("OTA", topics.update_channel, dev ? "dev" : "release", true);
}

} // namespace canary::net
