// Host test: every Home Assistant discovery config the canary-wap publishes
// asks for the entity id the docs give it, and each still fits its buffer
// (sweep HA16).
//
// Through firmware 2.4.x the configs carried a name, a unique id and a
// device, and no entity id, so Home Assistant built each id from the device
// name and the entity name: the smoke sensor the docs (gen_wap.py, the
// HomeKit Bridge recipe) call binary_sensor.<id>_smoke_alarm came up as
// binary_sensor.canary_<id>_smoke_alarm_heard. Home Assistant 2025.10 added
// `default_entity_id` (abbreviated `def_ent_id`) to MQTT discovery: a full
// entity id whose part after the '.' becomes the object id when the entity is
// first registered (homeassistant/components/mqtt/entity.py). Every config
// now asks for <component>.<device id, as Home Assistant slugs it>_<object_id>.
//
// The REAL csi_mqtt.cpp, over stubs/mqtt and a fake esp_mqtt client that
// records what is published: the bridge boots, the client connects, and the
// connect burst's discovery configs are read back from the fake. Checked:
//
//  1. The whole set, and nothing else: the 24 table entities of the build
//     the host compiles (FULL on the S3, acoustic and transient detectors in)
//     plus the firmware update entity, the auto-update switch and the mic
//     mute switch. A body too long for its buffer is never published and the
//     burst stops at it, so a missing config is a body that did not fit.
//  2. Each body carries exactly one def_ent_id, equal to the documented id,
//     and its unique id is still canary_<id>_<object_id>, so an entity a
//     Home Assistant already registered keeps its registry entry.
//  3. The worst case for the buffers: the longest device id the bridge holds
//     (32 characters), the longest topic prefix config_load() hands back (31)
//     and the longest firmware version (23); every body is under
//     csi_mqtt.cpp's 768 bytes (pinned against the source).
//  4. The slug: letters lowercased, every other run of characters one '_',
//     none at either end, which is what Home Assistant's slugify makes of an
//     ASCII name.
//
// Host-tested only: what Home Assistant does with the key was read from its
// source, not seen in a running Home Assistant, and the sketch's Arduino
// compile is CI's.
//
// Run: ./test_ha_discovery_ids

#include "csi_mqtt.h"

#include <Arduino.h>       // stubs/mqtt
#include <Preferences.h>   // stubs/mqtt: the NVS

#include <freertos/task.h>

#include "csi_event_egress.h"
#include "csi_integration.h"
#include "device_signature.h"
#include "beacon_source_scan.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "mqtt_client.h"
}

#ifndef CSI_MQTT_CPP
#error "the Makefile passes CSI_MQTT_CPP, csi_mqtt.cpp's absolute path"
#endif

// ── The fake esp_mqtt client: records every publish ─────────────────────

struct esp_mqtt_client {
  esp_event_handler_t handler = nullptr;
  void* handler_args = nullptr;
  bool connected = false;
  bool alive = true;
};

namespace fake {
std::vector<esp_mqtt_client*> clients;   // every client made, never freed
std::vector<std::pair<std::string, std::string>> published;   // topic, payload

void connect(esp_mqtt_client* c) {
  c->connected = true;
  esp_mqtt_event_t e = {};
  e.event_id = MQTT_EVENT_CONNECTED;
  e.client = c;
  c->handler(c->handler_args, "MQTT_EVENTS", (int32_t)MQTT_EVENT_CONNECTED, &e);
}
}  // namespace fake

extern "C" {

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t*) {
  esp_mqtt_client* c = new esp_mqtt_client();
  fake::clients.push_back(c);
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

esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t c) {
  c->alive = false;
  return ESP_OK;
}

int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char* topic, const char* data,
                            int len, int, int) {
  if (c == nullptr || !c->alive || !c->connected) return -1;
  fake::published.emplace_back(topic, std::string(data, data + len));
  return 1;
}

int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c, const char*, int) {
  return (c != nullptr && c->alive) ? 1 : -1;
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

namespace device_signature {
bool sign_chain(uint32_t, const uint8_t[32], char*, size_t) { return false; }
bool sign_event(uint32_t, const char*, const char*, const char*, int, int, int, char*, size_t) {
  return false;
}
bool sign_counts(uint32_t, char*, size_t) { return false; }
const char* fingerprint_hex() { return "0011223344556677"; }
}  // namespace device_signature

extern "C" uint32_t csi_event_get_next_event_id(void) { return 0xC0000000u; }

namespace csi_event_egress {
void pump() {}
Stats stats() { return Stats{}; }
}  // namespace csi_event_egress

// ── The test ────────────────────────────────────────────────────────────

namespace {

int g_checks = 0;
int g_failures = 0;

int g_failures_before = 0;   // g_failures when the running test began

void begin_test() { g_failures_before = g_failures; }
void end_test(const std::string& line) {
  if (g_failures == g_failures_before) std::printf("PASS %s\n", line.c_str());
  else std::printf("FAIL %s\n", line.c_str());
}

void check(bool cond, const std::string& what) {
  ++g_checks;
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

// csi_mqtt.cpp builds every discovery body into `char body[768]`; the four
// builders are pinned below so this cannot drift from the source.
constexpr size_t kBodyBytes = 768;

constexpr size_t kLongestPrefix = csi_mqtt::MAX_PREFIX_LEN - 1;

// The discovery set of the build the host compiles (build_config.h's
// defaults: FULL on the XIAO ESP32-S3, so FEATURE_ACOUSTIC_EVENTS and
// FEATURE_ACOUSTIC_TRANSIENTS are on): csi_mqtt.cpp's ENTITIES table, then
// publish_update_discovery's two and publish_mic_discovery's one.
const std::vector<std::pair<std::string, std::string>> kConfigs = {
    {"binary_sensor", "presence"},
    {"binary_sensor", "online"},
    {"sensor", "state"},
    {"sensor", "confidence"},
    {"sensor", "motion"},
    {"sensor", "breathing"},
    {"sensor", "bpm"},
    {"sensor", "witness_count"},
    {"sensor", "chain_length"},
    {"sensor", "uptime"},
    {"sensor", "memory_free"},
    {"sensor", "rssi"},
    {"sensor", "mesh_airtime_pct"},
    {"sensor", "mesh_channel"},
    {"binary_sensor", "mesh_channel_locked_to_sta"},
    {"sensor", "chirp_state"},
    {"sensor", "beacon_state"},
    {"sensor", "beacon_airtime_pct"},
    {"sensor", "beacon_active_template"},
    {"binary_sensor", "smoke_alarm"},
    {"binary_sensor", "co_alarm"},
    {"binary_sensor", "knock"},
    {"binary_sensor", "doorbell"},
    {"binary_sensor", "glass_break"},
    {"update", "firmware"},
    {"switch", "auto_update"},
    {"switch", "mic_mute"},
};

// The ids the docs name, spelled out (docs/integrations/apple-home-homekit-bridge.md
// §1, gen_wap.py's sandbox, docs/network_coexistence.md and the hardware
// checklist's Chirp and Beacon rows), for a device whose id slugs to `slug`.
std::vector<std::string> documented_ids(const std::string& slug) {
  return {
      "binary_sensor." + slug + "_smoke_alarm",
      "binary_sensor." + slug + "_co_alarm",
      "switch." + slug + "_mic_mute",
      "sensor." + slug + "_mesh_airtime_pct",
      "sensor." + slug + "_mesh_channel",
      "binary_sensor." + slug + "_mesh_channel_locked_to_sta",
      "sensor." + slug + "_chirp_state",
      "sensor." + slug + "_beacon_state",
      "sensor." + slug + "_beacon_airtime_pct",
      "sensor." + slug + "_beacon_active_template",
  };
}

// Every occurrence of `"key":"` in a body, as the string value after it.
std::vector<std::string> string_values(const std::string& body, const std::string& key) {
  std::vector<std::string> out;
  const std::string needle = "\"" + key + "\":\"";
  for (size_t at = body.find(needle); at != std::string::npos; at = body.find(needle, at + 1)) {
    const size_t start = at + needle.size();
    const size_t end = body.find('"', start);
    out.push_back(end == std::string::npos ? std::string() : body.substr(start, end - start));
  }
  return out;
}

struct Config {
  std::string component;
  std::string object_id;
  std::string body;
};

// The entity discovery configs among what the fake recorded:
// homeassistant/<component>/canary_<device_id>/<object_id>/config, non-empty,
// device triggers left out.
std::vector<Config> discovery_configs(const std::string& device_id) {
  std::vector<Config> out;
  const std::string head = "homeassistant/";
  const std::string node = "/canary_" + device_id + "/";
  const std::string tail = "/config";
  for (const auto& p : fake::published) {
    const std::string& t = p.first;
    if (t.compare(0, head.size(), head) != 0) continue;
    if (t.size() < tail.size() || t.compare(t.size() - tail.size(), tail.size(), tail) != 0) continue;
    const size_t n = t.find(node, head.size());
    if (n == std::string::npos) continue;
    Config c;
    c.component = t.substr(head.size(), n - head.size());
    // Device triggers (device_automation) are not entities: no entity id.
    if (c.component == "device_automation") continue;
    const size_t obj = n + node.size();
    c.object_id = t.substr(obj, t.size() - tail.size() - obj);
    c.body = p.second;
    if (!c.body.empty()) out.push_back(c);
  }
  return out;
}

// One connect of the bridge with `device_id`: the connect burst's discovery
// configs, each held to the set, its buffer and its documented id.
void connect_and_check(const char* device_id, const std::string& slug, size_t* longest,
                       std::string* longest_what = nullptr) {
  csi_mqtt::set_identity(device_id, "2.4.15-wap-rc.123456789", "ab");
  fake::published.clear();
  fake::connect(fake::clients.back());
  const std::string tag = std::string("[") + device_id + "] ";

  // The burst opens with the retained status on <prefix>/<id>/status: the
  // prefix the bodies carry is the longest the settings take.
  check(!fake::published.empty() &&
            fake::published.front().first ==
                std::string(kLongestPrefix, 'p') + "/" + device_id + "/status",
        tag + "the bodies are built with the longest prefix");

  const std::vector<Config> got = discovery_configs(device_id);
  std::set<std::pair<std::string, std::string>> seen;
  for (const Config& c : got) seen.insert({c.component, c.object_id});
  check(got.size() == kConfigs.size(),
        tag + "the connect publishes " + std::to_string(kConfigs.size()) +
            " discovery configs (got " + std::to_string(got.size()) + ")");
  for (const auto& want : kConfigs) {
    check(seen.count(want) == 1,
          tag + "a config for " + want.first + "/" + want.second + " was published");
  }

  std::set<std::string> ids;
  for (const Config& c : got) {
    const std::string what = tag + c.component + "/" + c.object_id + ": ";
    check(c.body.size() < kBodyBytes,
          what + "body is " + std::to_string(c.body.size()) + " bytes, under " +
              std::to_string(kBodyBytes));
    if (c.body.size() > *longest) {
      *longest = c.body.size();
      if (longest_what != nullptr) *longest_what = c.component + "/" + c.object_id;
    }
    check(c.body.front() == '{' && c.body.back() == '}', what + "body is one JSON object");

    const std::vector<std::string> def = string_values(c.body, "def_ent_id");
    const std::string want = c.component + "." + slug + "_" + c.object_id;
    check(def.size() == 1, what + "carries exactly one def_ent_id (got " +
                               std::to_string(def.size()) + ")");
    if (def.size() == 1) {
      check(def[0] == want, what + "def_ent_id is '" + def[0] + "', want '" + want + "'");
      ids.insert(def[0]);
    }
    const std::vector<std::string> uniq = string_values(c.body, "uniq_id");
    check(uniq.size() == 1 && uniq[0] == "canary_" + std::string(device_id) + "_" + c.object_id,
          what + "unique id is still canary_<id>_<object_id>");
  }
  for (const std::string& id : documented_ids(slug)) {
    check(ids.count(id) == 1, tag + "a config asks for the documented " + id);
  }
}

void boot() {
  stub_nvs().clear();
  csi_mqtt::Config c;
  csi_mqtt::config_load(&c);
  c.enabled = true;
  c.discovery = true;
  std::snprintf(c.host, sizeof c.host, "%s", "10.0.0.9");
  // The longest prefix config_load() hands back: MAX_PREFIX_LEN - 1, since
  // it reads with Preferences::getString(key, buf, MAX_PREFIX_LEN), whose
  // length counts the NUL.
  std::string prefix(kLongestPrefix, 'p');
  std::snprintf(c.prefix, sizeof c.prefix, "%s", prefix.c_str());
  check(csi_mqtt::config_save(c), "the settings are stored");
  csi_mqtt::set_identity("canary-s3-AbCd", "2.4.15-wap", "ab");
  check(csi_mqtt::init("canary-s3-AbCd", "2.4.15-wap", "ab"), "the boot init opens a client");
  check(fake::clients.size() == 1, "one client");
}

void test_every_config_asks_for_its_documented_id() {
  begin_test();
  size_t longest = 0;
  // The id the sketch generates: DEVICE_ID_PREFIX and four characters of the
  // no-confusion alphabet, which has capitals.
  connect_and_check("canary-s3-AbCd", "canary_s3_abcd", &longest);
  connect_and_check("canary-c3-x7Qk", "canary_c3_x7qk", &longest);
  end_test("every_config_asks_for_its_documented_id (longest body " + std::to_string(longest) +
           " of " + std::to_string(kBodyBytes) + ")");
}

void test_every_config_fits_its_buffer_at_the_longest_identity() {
  begin_test();
  // 32 characters, the most s_device_id holds; the boot gave the longest
  // prefix, and connect_and_check the longest firmware version (23).
  const char* longest_id = "canary-s3-WXYZwxyz23456789ABCDEF";
  check(std::strlen(longest_id) == 32, "the longest device id is 32 characters");
  size_t longest = 0;
  std::string longest_what;
  connect_and_check(longest_id, "canary_s3_wxyzwxyz23456789abcdef", &longest, &longest_what);
  end_test("every_config_fits_its_buffer_at_the_longest_identity (longest body " +
           std::to_string(longest) + " of " + std::to_string(kBodyBytes) + ", " + longest_what + ")");
}

void test_the_slug_is_home_assistants() {
  begin_test();
  size_t longest = 0;
  // Runs of anything but a letter or a digit become one '_' (an '_' too, as
  // in Home Assistant's slugify), and none is left at either end.
  connect_and_check("-Canary--S3__x-", "canary_s3_x", &longest);
  connect_and_check("CANARY.WAP 1", "canary_wap_1", &longest);
  end_test("the_slug_is_home_assistants");
}

void test_the_buffer_size_is_the_sources() {
  begin_test();
  bool ok = false;
  const std::string src = beacon_source_scan::strip_comments(
      beacon_source_scan::read_source(CSI_MQTT_CPP, &ok));
  check(ok, "csi_mqtt.cpp is readable");
  const std::string decl = "char body[" + std::to_string(kBodyBytes) + "];";
  // The three builders of the configs above; each has the one body buffer.
  for (const char* fn : {"publish_one_discovery", "publish_update_discovery",
                         "publish_mic_discovery"}) {
    const std::string body = beacon_source_scan::function_body(src, fn);
    check(!body.empty(), std::string(fn) + " is in csi_mqtt.cpp");
    check(beacon_source_scan::count(body, "char body[") == 1 &&
              beacon_source_scan::count(body, decl) == 1,
          std::string(fn) + " builds into one " + decl);
  }
  end_test("the_buffer_size_is_the_sources");
}

}  // namespace

int main() {
  boot();
  test_every_config_asks_for_its_documented_id();
  test_every_config_fits_its_buffer_at_the_longest_identity();
  test_the_slug_is_home_assistants();
  test_the_buffer_size_is_the_sources();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d of %d HA discovery id checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d HA discovery id checks PASSED\n", g_checks);
  return 0;
}
