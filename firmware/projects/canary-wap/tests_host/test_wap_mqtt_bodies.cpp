// Host harness: what the canary-wap publishes over MQTT, from the REAL
// csi_mqtt.cpp (sweep F34, the MQTT contract fixture).
//
// Home Assistant reads two things off a canary-wap: the discovery configs
// (homeassistant/<component>/canary_<id>/<object_id>/config, which Home
// Assistant's MQTT integration turns into entities) and the retained state
// topics under <prefix>/<id>/ that those configs, and the SecuraCV
// integration's own sensors (custom_components/securacv), parse. This
// harness boots the bridge over stubs/mqtt and a fake esp_mqtt client that
// records every publish with its QoS and retain flag, and drives every
// publisher csi_mqtt.cpp has through a device's life, in phases:
//
//   connect      the connect burst: status, the discovery set, the device
//                triggers, the cached auto-update and mic states;
//   running      the loop's publishes on a healthy unit: status, health
//                (mains, card in, lid shut), egress, counts and chain
//                (signed), mesh, chirp, beacon, the update entity's state
//                and the acoustic snapshot; one committed presence event
//                (signed) and one tamper bridge publish;
//   trouble      the same unit on a low battery with the card out, the lid
//                open, the heap low, the event-id space low, Chirp disabled
//                and the mesh off the home channel;
//   unsigned     counts, chain and an event before device_signature is
//                ready (the bodies an early boot sends);
//   discovery_removed  remove_discovery(): what a connect with discovery
//                off sends, an empty retained payload per config topic.
//
// Beside the phases, the record carries the last will the client was
// configured with (what the broker publishes on the device's behalf when it
// vanishes) and the command topics it subscribed to.
//
// Two bodies are not csi_mqtt.cpp's: the update entity's state and the
// acoustic snapshot are built by the sketch (canary_wap.ino's
// ota_publish_update_state() and its loop()'s sensing snprintf) and only
// relayed here, byte for byte. The harness hands over bodies in the sketch's
// shape (kUpdateState, kSensing below); test_wap_mqtt_contract.py holds
// their keys to the keys the sketch writes, so the copies cannot drift.
//
// The signed bodies are signed for real: device_signature.cpp is compiled
// in (its canonical builders, init and envelope fields), with its three
// signers swapped for OpenSSL's Ed25519 over the canonical device_signature
// builds (RFC 8032 Ed25519 is deterministic, so the rweather library the
// firmware links writes the same signature), on the repo's test key (seed
// 0x42 * 32) spelled by mqtt_identity.h, as the sketch spells it. So Home
// Assistant's verifier can check them against the key the health body
// carries.
//
// Run with no argument it checks what it recorded (below) and prints
// "ALL ... PASSED"; with --json it prints the record, one JSON document, for
// custom_components/securacv/tests/test_wap_mqtt_contract.py, which feeds it
// to the integration's parsers (the HA side of the contract). Checked here:
//
//  1. The delivery bound: every publish the bridge makes is QoS 0 (at most
//     once); the last will is QoS 1 and retained; the command topics are
//     subscribed at QoS 1.
//  2. Retain: every state topic and every discovery config is retained; the
//     events and tamper topics are not.
//  3. Each phase published every topic it is meant to, and each non-empty
//     body on a JSON topic is one strict JSON object (json_strict.h) with no
//     key twice; the bare-word topics carry their words; the two relayed
//     bodies arrive byte for byte.
//
// Host-tested only: nothing here ran on a unit or against a broker, and the
// sketch's Arduino compile is CI's.
//
// Run: ./test_wap_mqtt_bodies [--json]

#include "csi_mqtt.h"

#include <Arduino.h>       // stubs/mqtt
#include <Preferences.h>   // stubs/mqtt: the NVS

#include <freertos/task.h>

#include "csi_event_egress.h"
#include "csi_integration.h"
#include "device_signature.h"
#include "json_strict.h"
#include "mqtt_identity.h"

#include <openssl/evp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "mqtt_client.h"
}

#ifndef DEVICE_SIGNATURE_CPP
#error "the Makefile passes DEVICE_SIGNATURE_CPP, device_signature.cpp's absolute path"
#endif

// device_signature.cpp, whole, with its three signers renamed out of the way
// (its host build signs nothing), so the ones below can take their names.
// device_signature.h was included above, so its declarations keep the real
// names; the file's own include of it is a no-op.
#define sign_chain host_unused_sign_chain
#define sign_event host_unused_sign_event
#define sign_counts host_unused_sign_counts
#include DEVICE_SIGNATURE_CPP
#undef sign_chain
#undef sign_event
#undef sign_counts

// ── The test key, with OpenSSL ──────────────────────────────────────────

namespace key {
uint8_t seed[32];
uint8_t pub[32];
uint8_t fp[8];
EVP_PKEY* pkey = nullptr;
char pub_hex[mqtt_identity::KEY_HEX_CAP];
char fp_hex[mqtt_identity::FP_HEX_CAP];
bool signing = true;   // false: the signers fail, as before device_signature::init

bool make() {
  std::memset(seed, 0x42, sizeof(seed));
  pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed, 32);
  size_t len = 32;
  if (!pkey || EVP_PKEY_get_raw_public_key(pkey, pub, &len) != 1 || len != 32) return false;
  // compute_fingerprint (canary_wap.ino): SHA-256 over the domain, a NUL and
  // the key; the first 8 bytes.
  static const char kDomain[] = "securacv:pubkey:fingerprint";
  const uint8_t nul = 0;
  uint8_t hash[32];
  unsigned int hlen = 0;
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  const bool ok = md && EVP_DigestInit_ex(md, EVP_sha256(), nullptr) == 1 &&
                  EVP_DigestUpdate(md, kDomain, std::strlen(kDomain)) == 1 &&
                  EVP_DigestUpdate(md, &nul, 1) == 1 && EVP_DigestUpdate(md, pub, 32) == 1 &&
                  EVP_DigestFinal_ex(md, hash, &hlen) == 1 && hlen == 32;
  EVP_MD_CTX_free(md);
  if (!ok) return false;
  std::memcpy(fp, hash, 8);
  mqtt_identity::public_key_hex(pub_hex, pub);
  mqtt_identity::fingerprint_hex(fp_hex, fp);
  return true;
}

bool sign(const char* canonical, size_t n, char* out, size_t cap) {
  if (!signing || pkey == nullptr || n == 0) return false;
  uint8_t sig[64];
  size_t sig_len = sizeof(sig);
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  const bool ok = md && EVP_DigestSignInit(md, nullptr, nullptr, nullptr, pkey) == 1 &&
                  EVP_DigestSign(md, sig, &sig_len,
                                 reinterpret_cast<const uint8_t*>(canonical), n) == 1 &&
                  sig_len == 64;
  EVP_MD_CTX_free(md);
  if (!ok) return false;
  return device_signature::b64url_encode_nopad(sig, 64, out, cap) ==
         device_signature::SIG_B64URL_LEN;
}
}  // namespace key

// device_signature's three signers: its own canonical builders, fed the
// device id init() cached, signed with OpenSSL instead of rweather's Ed25519.
namespace device_signature {
bool sign_chain(uint32_t length, const uint8_t latest_hash_32[32], char* sig_b64url_out,
                size_t sig_cap) {
  char canon[256];
  const size_t n = build_chain_canonical(length, latest_hash_32, device_id(), canon, sizeof(canon));
  return key::sign(canon, n, sig_b64url_out, sig_cap);
}
bool sign_event(uint32_t event_id, const char* state, const char* category_str,
                const char* privacy_str, int motion, int breath, int bpm, char* sig_b64url_out,
                size_t sig_cap) {
  char canon[256];
  const size_t n = build_event_canonical(event_id, state, category_str, privacy_str, motion,
                                         breath, bpm, device_id(), canon, sizeof(canon));
  return key::sign(canon, n, sig_b64url_out, sig_cap);
}
bool sign_counts(uint32_t total, char* sig_b64url_out, size_t sig_cap) {
  char canon[128];
  const size_t n = build_counts_canonical(total, device_id(), canon, sizeof(canon));
  return key::sign(canon, n, sig_b64url_out, sig_cap);
}
}  // namespace device_signature

// ── The fake esp_mqtt client: records every publish ─────────────────────

struct esp_mqtt_client {
  esp_event_handler_t handler = nullptr;
  void* handler_args = nullptr;
  bool connected = false;
};

namespace fake {
struct Publish {
  std::string topic;
  std::string payload;
  int qos;
  bool retain;
};
struct Will {
  std::string topic;
  std::string msg;
  int qos = -1;
  int retain = -1;
};
std::vector<esp_mqtt_client*> clients;
std::vector<Publish> published;
std::vector<std::pair<std::string, int>> subscribed;   // topic, qos
Will will;

void connect(esp_mqtt_client* c) {
  c->connected = true;
  esp_mqtt_event_t e = {};
  e.event_id = MQTT_EVENT_CONNECTED;
  e.client = c;
  c->handler(c->handler_args, "MQTT_EVENTS", (int32_t)MQTT_EVENT_CONNECTED, &e);
}
}  // namespace fake

extern "C" {

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t* cfg) {
  esp_mqtt_client* c = new esp_mqtt_client();
  fake::clients.push_back(c);
  // The config is the caller's stack: copy the will now.
  const auto& w = cfg->session.last_will;
  fake::will.topic = w.topic ? w.topic : "";
  fake::will.msg = w.msg ? std::string(w.msg, w.msg + (w.msg_len > 0 ? w.msg_len : std::strlen(w.msg)))
                         : "";
  fake::will.qos = w.qos;
  fake::will.retain = w.retain;
  return c;
}

esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c, esp_mqtt_event_id_t,
                                         esp_event_handler_t handler, void* args) {
  c->handler = handler;
  c->handler_args = args;
  return ESP_OK;
}

esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t) { return ESP_OK; }

esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t c) {
  c->connected = false;
  return ESP_OK;
}

esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t) { return ESP_OK; }

int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char* topic, const char* data,
                            int len, int qos, int retain) {
  if (c == nullptr || !c->connected) return -1;
  fake::published.push_back({topic, std::string(data, data + len), qos, retain != 0});
  return 1;
}

int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c, const char* topic, int qos) {
  if (c == nullptr) return -1;
  fake::subscribed.emplace_back(topic, qos);
  return 1;
}

}  // extern "C"

// ── What the sketch links in on a device ────────────────────────────────

namespace csi_integration {
bool session_validate_cookie(httpd_req_t*) { return true; }
void add_outbound_bytes(uint32_t) {}
void hex_encode(const uint8_t* in, size_t len, char* out) {
  static const char k[] = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    out[2 * i] = k[in[i] >> 4];
    out[2 * i + 1] = k[in[i] & 15];
  }
  out[2 * len] = '\0';
}
}  // namespace csi_integration

namespace {
uint32_t g_next_event_id = 0xC0000010u;
csi_event_egress::Stats g_stats = {};
}  // namespace

extern "C" uint32_t csi_event_get_next_event_id(void) { return g_next_event_id; }

namespace csi_event_egress {
void pump() {}
Stats stats() { return g_stats; }
}  // namespace csi_event_egress

// ── The phases ──────────────────────────────────────────────────────────

namespace {

constexpr const char* kDeviceId = "canary-s3-4dC2";
constexpr const char* kFirmware = "2.4.16-wap";

// The sketch's two relayed bodies, in its shape: ota_publish_update_state()
// (ArduinoJson, keys in the order it sets them, no update on offer) and the
// acoustic snapshot loop() formats (a smoke alarm heard, the mic live).
constexpr const char* kUpdateState =
    "{\"installed_version\":\"2.4.16-wap\",\"latest_version\":\"2.4.16-wap\","
    "\"in_progress\":false,\"update_percentage\":null}";
constexpr const char* kSensing =
    "{\"acoustic_event\":\"smoke_alarm_t3\",\"mic_muted\":false,\"t3_detected\":1,"
    "\"t4_detected\":0,\"knock_detected\":0,\"doorbell_detected\":0,"
    "\"glass_break_detected\":0,\"i2s_read_errors\":0}";

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const std::string& what) {
  ++g_checks;
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

struct Phase {
  std::string name;
  size_t begin;
  size_t end;
};
std::vector<Phase> g_phases;

void begin_phase(const char* name) { g_phases.push_back({name, fake::published.size(), 0}); }
void end_phase() { g_phases.back().end = fake::published.size(); }

std::string topic(const char* suffix) { return std::string("securacv/") + kDeviceId + "/" + suffix; }

csi_event_record_t event_row(uint32_t id, const char* module, const char* type,
                             csi_event_category_t category, const char* state,
                             const char* confidence, uint8_t motion, uint8_t breathing,
                             uint8_t bpm) {
  csi_event_record_t r;
  std::memset(&r, 0, sizeof(r));
  r.event_id = id;
  r.first_seen_ms = 3600u * 1000u;
  r.last_seen_ms = r.first_seen_ms;
  r.category = category;
  r.privacy = CSI_PRIVACY_P0;
  r.bundled_count = 1;
  r.values.category = category;
  std::snprintf(r.values.state_name, sizeof(r.values.state_name), "%s", state);
  std::snprintf(r.values.confidence, sizeof(r.values.confidence), "%s", confidence);
  r.values.motion_score = motion;
  r.values.breathing_score = breathing;
  r.values.breathing_rate_bpm = bpm;
  r.values.duration_sec = 120;
  std::snprintf(r.module_id, sizeof(r.module_id), "%s", module);
  std::snprintf(r.type_name, sizeof(r.type_name), "%s", type);
  return r;
}

void boot() {
  stub_nvs().clear();
  check(key::make(), "OpenSSL makes the Ed25519 test key");
  // As canary_wap.ino's setup() hands it over: the encoder's strings.
  device_signature::init(key::seed, key::pub, kDeviceId, key::fp_hex);
  csi_mqtt::Config c;
  csi_mqtt::config_load(&c);   // the defaults: prefix "securacv"
  c.enabled = true;
  c.discovery = true;
  std::snprintf(c.host, sizeof c.host, "%s", "10.0.0.9");
  check(csi_mqtt::config_save(c), "the settings are stored");
  csi_mqtt::set_identity(kDeviceId, kFirmware, key::pub_hex);
  check(csi_mqtt::init(kDeviceId, kFirmware, key::pub_hex), "the boot init opens a client");
  check(fake::clients.size() == 1, "one client");
}

void phase_connect() {
  // The cached states the burst republishes: the auto-update switch (an
  // httpd handler sets it) and the mic (the boot records it).
  csi_mqtt::set_update_auto_state(false);
  csi_mqtt::publish_mic_state(false);
  begin_phase("connect");
  fake::connect(fake::clients.back());
  end_phase();
}

uint8_t g_hash[32];

void phase_running() {
  begin_phase("running");
  csi_mqtt::publish_status(/*csi_running=*/true, /*wifi_connected=*/true, /*rssi_dbm=*/-58);
  const csi_mqtt::MqttTamperLevels levels = {/*sd_mounted=*/1, /*enclosure_open=*/0};
  csi_mqtt::publish_health(182000, 3600, nullptr, &levels);
  g_stats.dropped = 1;
  g_stats.planner.live = 12;
  g_stats.planner.replayed = 3;
  csi_mqtt::publish_egress();
  for (int i = 0; i < 32; ++i) g_hash[i] = (uint8_t)(0xA0 + i);
  csi_mqtt::publish_counts(42);
  csi_mqtt::publish_chain(42, g_hash);
  csi_mqtt::publish_mesh(/*airtime_pct_x100=*/215, /*channel=*/6, /*locked_to_sta=*/true,
                         /*locked_to_ap=*/false, /*fallback=*/false, 100, 2, 1);
  csi_mqtt::publish_chirp_state("active");
  csi_mqtt::publish_beacon_state("Normal", 12, "", 3, 2, 0);
  csi_mqtt::publish_update_state(kUpdateState);
  csi_mqtt::publish_sensing(kSensing);
  const csi_event_record_t presence =
      event_row(0xC0000007u, "core.presence", "presence", CSI_CATEGORY_EVENT, "active", "likely",
                42, 17, 14);
  check(csi_mqtt::publish_event_row(presence, 1, false) == csi_mqtt::EventSend::kSent,
        "running: the presence event goes out");
  csi_event_values_t tamper;
  std::memset(&tamper, 0, sizeof(tamper));
  tamper.category = CSI_CATEGORY_ANOMALY;
  std::snprintf(tamper.state_name, sizeof(tamper.state_name), "%s", "enclosure");
  check(csi_mqtt::publish_tamper_bridge("system.integrity", "tamper", &tamper),
        "running: the tamper bridge publishes");
  end_phase();
}

void phase_trouble() {
  begin_phase("trouble");
  csi_mqtt::publish_status(/*csi_running=*/false, /*wifi_connected=*/true, /*rssi_dbm=*/-87);
  g_next_event_id = 0xF0000001u;   // past csi_event_id_floor::kHoldLimit
  const csi_mqtt::MqttBatteryInfo battery = {/*soc_pct=*/8, /*health_pct=*/80,
                                             /*battery_mv=*/3400, "discharging"};
  const csi_mqtt::MqttTamperLevels levels = {/*sd_mounted=*/0, /*enclosure_open=*/1};
  csi_mqtt::publish_health(9000, 7200, &battery, &levels);
  csi_mqtt::publish_egress();
  csi_mqtt::publish_mesh(/*airtime_pct_x100=*/1890, /*channel=*/1, /*locked_to_sta=*/false,
                         /*locked_to_ap=*/true, /*fallback=*/false, 140, 30, 9);
  csi_mqtt::publish_chirp_state("disabled");
  csi_mqtt::publish_beacon_state("Trouble", 0, "active", 4, 1, 3);
  g_next_event_id = 0xC0000010u;
  end_phase();
}

void phase_unsigned() {
  begin_phase("unsigned");
  key::signing = false;
  csi_mqtt::publish_counts(43);
  csi_mqtt::publish_chain(43, g_hash);
  const csi_event_record_t empty =
      event_row(0xC0000008u, "core.presence", "presence", CSI_CATEGORY_EVENT, "empty", "certain",
                0, 0, 0);
  check(csi_mqtt::publish_event_row(empty, 1, false) == csi_mqtt::EventSend::kSent,
        "unsigned: the event goes out unsigned");
  key::signing = true;
  end_phase();
}

void phase_discovery_removed() {
  begin_phase("discovery_removed");
  csi_mqtt::remove_discovery();
  end_phase();
}

// ── The checks ──────────────────────────────────────────────────────────

const Phase* phase(const std::string& name) {
  const auto it = std::find_if(g_phases.begin(), g_phases.end(),
                               [&](const Phase& p) { return p.name == name; });
  return it == g_phases.end() ? nullptr : &*it;
}

bool ends_with(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// No key twice in any object: readers disagree on a doubled key (Python's
// json, which the integration parses with, keeps the last one without a
// word), so a body that has one means two things.
bool no_doubled_key(const json_strict::Value& v) {
  if (v.kind == json_strict::Value::Object) {
    std::set<std::string> seen;
    for (const auto& kv : v.keys) {
      if (!seen.insert(kv.first).second) return false;
      if (!no_doubled_key(kv.second)) return false;
    }
  }
  if (v.kind == json_strict::Value::Array) {
    return std::all_of(v.items.begin(), v.items.end(),
                       [](const json_strict::Value& item) { return no_doubled_key(item); });
  }
  return true;
}

void check_bodies() {
  // 1. The delivery bound.
  for (const fake::Publish& p : fake::published) {
    check(p.qos == 0, p.topic + ": published at QoS 0 (got " + std::to_string(p.qos) + ")");
  }
  check(fake::will.topic == topic("status"), "the last will is on the status topic");
  check(fake::will.msg == "{\"online\":false}", "the last will says offline");
  check(fake::will.qos == 1 && fake::will.retain == 1, "the last will is QoS 1, retained");
  std::set<std::string> cmds;
  for (const auto& s : fake::subscribed) {
    cmds.insert(s.first);
    check(s.second == 1, s.first + ": subscribed at QoS 1");
  }
  check(cmds == std::set<std::string>{topic("update/cmd"), topic("update/auto/cmd"),
                                      topic("mic/cmd")},
        "the command topics are the update, auto-update and mic ones");

  // 2. Retain.
  for (const fake::Publish& p : fake::published) {
    const bool event = p.topic == topic("events") || p.topic == topic("tamper");
    check(p.retain != event, p.topic + (event ? ": not retained" : ": retained"));
  }

  // 3. Each phase's topics, and the bodies.
  const std::map<std::string, std::set<std::string>> want = {
      {"connect", {"status", "update/auto", "mic/state"}},
      {"running",
       {"status", "health", "egress", "counts", "chain", "mesh", "chirp", "beacon",
        "update/state", "sensing", "events", "tamper"}},
      {"trouble", {"status", "health", "egress", "mesh", "chirp", "beacon"}},
      {"unsigned", {"counts", "chain", "events"}},
  };
  for (const auto& w : want) {
    const Phase* p = phase(w.first);
    check(p != nullptr, "phase " + w.first + " ran");
    if (p == nullptr) continue;
    std::set<std::string> got;
    const std::string head = std::string("securacv/") + kDeviceId + "/";
    for (size_t i = p->begin; i < p->end; ++i) {
      const std::string& t = fake::published[i].topic;
      if (t.compare(0, head.size(), head) == 0) got.insert(t.substr(head.size()));
    }
    check(got == w.second, "phase " + w.first + " publishes each of its state topics once over");
  }
  const Phase* connect = phase("connect");
  size_t configs = 0;
  if (connect != nullptr) {
    for (size_t i = connect->begin; i < connect->end; ++i) {
      if (ends_with(fake::published[i].topic, "/config")) ++configs;
    }
  }
  // 27 entity configs (test_ha_discovery_ids.cpp holds the set) and 6 triggers.
  check(configs == 33, "the connect burst carries 33 discovery configs (got " +
                           std::to_string(configs) + ")");
  const Phase* removed = phase("discovery_removed");
  check(removed != nullptr && removed->end - removed->begin == configs,
        "remove_discovery() sends one removal per config");

  // The sketch's two bodies are relayed as handed over.
  size_t relayed = 0;
  for (const fake::Publish& p : fake::published) {
    if (p.topic == topic("update/state")) {
      ++relayed;
      check(p.payload == kUpdateState, "update/state: relayed byte for byte");
    }
    if (p.topic == topic("sensing")) {
      ++relayed;
      check(p.payload == kSensing, "sensing: relayed byte for byte");
    }
  }
  check(relayed == 2, "the update state and the acoustic snapshot each go out once");

  for (const fake::Publish& p : fake::published) {
    if (p.payload.empty()) continue;   // a removal
    if (ends_with(p.topic, "/update/auto")) {
      check(p.payload == "ON" || p.payload == "OFF", p.topic + ": ON or OFF");
      continue;
    }
    if (ends_with(p.topic, "/mic/state")) {
      check(p.payload == "muted" || p.payload == "live", p.topic + ": muted or live");
      continue;
    }
    json_strict::Value v;
    const bool parsed = json_strict::parse(p.payload, &v);
    check(parsed && v.kind == json_strict::Value::Object,
          p.topic + ": one strict JSON object: " + p.payload);
    check(!parsed || no_doubled_key(v), p.topic + ": no key twice: " + p.payload);
  }
}

// ── The record, as JSON ─────────────────────────────────────────────────

std::string quoted(const std::string& s) {
  std::string out = "\"";
  for (const unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back((char)c);
    } else if (c < 0x20 || c >= 0x7F) {
      char esc[8];
      std::snprintf(esc, sizeof esc, "\\u%04x", (unsigned)c);
      out += esc;
    } else {
      out.push_back((char)c);
    }
  }
  return out + "\"";
}

void print_json() {
  std::printf("{\n  \"harness\": \"test_wap_mqtt_bodies\",\n");
  std::printf("  \"device_id\": %s,\n", quoted(kDeviceId).c_str());
  std::printf("  \"prefix\": \"securacv\",\n");
  std::printf("  \"firmware_version\": %s,\n", quoted(kFirmware).c_str());
  std::printf("  \"public_key\": %s,\n", quoted(key::pub_hex).c_str());
  std::printf("  \"fingerprint\": %s,\n", quoted(key::fp_hex).c_str());
  std::printf("  \"last_will\": {\"topic\": %s, \"payload\": %s, \"qos\": %d, \"retain\": %s},\n",
              quoted(fake::will.topic).c_str(), quoted(fake::will.msg).c_str(), fake::will.qos,
              fake::will.retain ? "true" : "false");
  std::printf("  \"subscriptions\": [");
  for (size_t i = 0; i < fake::subscribed.size(); ++i) {
    std::printf("%s{\"topic\": %s, \"qos\": %d}", i ? ", " : "",
                quoted(fake::subscribed[i].first).c_str(), fake::subscribed[i].second);
  }
  std::printf("],\n  \"phases\": [\n");
  for (size_t k = 0; k < g_phases.size(); ++k) {
    const Phase& p = g_phases[k];
    std::printf("    {\"name\": %s, \"publishes\": [\n", quoted(p.name).c_str());
    for (size_t i = p.begin; i < p.end; ++i) {
      const fake::Publish& pub = fake::published[i];
      std::printf("      {\"topic\": %s, \"payload\": %s, \"qos\": %d, \"retain\": %s}%s\n",
                  quoted(pub.topic).c_str(), quoted(pub.payload).c_str(), pub.qos,
                  pub.retain ? "true" : "false", i + 1 < p.end ? "," : "");
    }
    std::printf("    ]}%s\n", k + 1 < g_phases.size() ? "," : "");
  }
  std::printf("  ]\n}\n");
}

}  // namespace

int main(int argc, char** argv) {
  const bool json = argc > 1 && std::strcmp(argv[1], "--json") == 0;
  if (argc > 1 && !json) {
    std::fprintf(stderr, "usage: %s [--json]\n", argv[0]);
    return 2;
  }
  boot();
  phase_connect();
  phase_running();
  phase_trouble();
  phase_unsigned();
  phase_discovery_removed();
  check_bodies();
  if (key::pkey != nullptr) EVP_PKEY_free(key::pkey);
  if (g_failures != 0) {
    std::fprintf(stderr, "%d of %d WAP MQTT body checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  if (json) {
    print_json();
  } else {
    std::printf("ALL %d WAP MQTT body checks PASSED\n", g_checks);
  }
  return 0;
}
