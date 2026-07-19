// canary-local/emulator/src/emu_mqtt.cpp — PubSubClient over a scenario.
//
// The REAL src/net/mqtt_mgr.cpp compiles against the PubSubClient shim
// header; this file is that shim's behavior. The page injects fleet
// payloads (the same JSON real canaries publish) and they are delivered
// through the callback mqtt_mgr registered — so parsing, the fleet
// model feed, TOFU pinning, and Ed25519 chain verification are all the
// firmware's own code path. Outbound publishes (the display's retained
// status heartbeat, health row, LWT semantics) surface to the page's
// MQTT panel so a learner can watch the display's voice on the wire.
#include <PubSubClient.h>
#include <WiFi.h>

#include <emscripten.h>

#include <deque>
#include <string>
#include <vector>

#include "emu_bus.h"

namespace {

struct Inbound {
  std::string topic;
  std::vector<uint8_t> payload;
};

mqtt_callback_t g_cb = nullptr;
bool g_connected = false;
std::deque<Inbound> g_queue;
std::vector<std::string> g_subs;

// LWT registered at connect — when the scenario cuts the broker, the page
// plays broker: it can re-inject the will on siblings. We surface it.
std::string g_will_topic, g_will_msg;

EM_JS(void, js_mqtt_out, (const char* topic, const char* payload, int retained), {
  if (Module.onMqttPublish)
    Module.onMqttPublish(UTF8ToString(topic), UTF8ToString(payload), !!retained);
});
EM_JS(void, js_mqtt_sub, (const char* topic), {
  if (Module.onMqttSubscribe) Module.onMqttSubscribe(UTF8ToString(topic));
});
EM_JS(void, js_mqtt_conn, (int up, const char* will_topic, const char* will_msg), {
  if (Module.onMqttConnection)
    Module.onMqttConnection(!!up, UTF8ToString(will_topic), UTF8ToString(will_msg));
});

bool link_ok() { return emu_bus_wifi_up() && emu_bus_broker_up(); }

}  // namespace

extern "C" {

// Page → device: one fleet message. Delivered on the next mqtt.loop()
// pass, i.e. inside the firmware's own main loop, like a socket would.
EMSCRIPTEN_KEEPALIVE void emu_mqtt_inject(const char* topic,
                                          const char* payload, int len) {
  Inbound m;
  m.topic = topic ? topic : "";
  const uint8_t* p = (const uint8_t*)payload;
  m.payload.assign(p, p + (len >= 0 ? len : 0));
  g_queue.push_back(std::move(m));
}

EMSCRIPTEN_KEEPALIVE int emu_mqtt_connected(void) { return g_connected ? 1 : 0; }

}  // extern "C"

PubSubClient& PubSubClient::setServer(const char*, uint16_t) { return *this; }
PubSubClient& PubSubClient::setCallback(mqtt_callback_t cb) {
  g_cb = cb;
  return *this;
}
PubSubClient& PubSubClient::setBufferSize(uint16_t) { return *this; }

bool PubSubClient::connect(const char* id) {
  return connect(id, nullptr, nullptr, nullptr, 0, false, nullptr);
}
bool PubSubClient::connect(const char* id, const char* user, const char* pass) {
  return connect(id, user, pass, nullptr, 0, false, nullptr);
}
bool PubSubClient::connect(const char* /*id*/, const char* /*user*/,
                           const char* /*pass*/, const char* willTopic,
                           uint8_t /*willQos*/, bool /*willRetain*/,
                           const char* willMessage) {
  if (!link_ok()) {
    g_connected = false;
    return false;
  }
  g_connected = true;
  g_will_topic = willTopic ? willTopic : "";
  g_will_msg = willMessage ? willMessage : "";
  js_mqtt_conn(1, g_will_topic.c_str(), g_will_msg.c_str());
  return true;
}

void PubSubClient::disconnect() {
  if (g_connected) js_mqtt_conn(0, g_will_topic.c_str(), g_will_msg.c_str());
  g_connected = false;
}

bool PubSubClient::connected() {
  if (g_connected && !link_ok()) {
    // Link cut mid-session: the broker would fire our LWT; the page hears
    // about it and can propagate "offline" to sibling displays.
    js_mqtt_conn(0, g_will_topic.c_str(), g_will_msg.c_str());
    g_connected = false;
  }
  return g_connected;
}

int PubSubClient::state() { return g_connected ? MQTT_CONNECTED : MQTT_CONNECT_FAILED; }

bool PubSubClient::loop() {
  if (!connected()) return false;
  // Drain a bounded burst per pass — same shape as a socket read loop.
  int budget = 8;
  while (budget-- > 0 && !g_queue.empty()) {
    Inbound m = std::move(g_queue.front());
    g_queue.pop_front();
    if (g_cb) {
      // PubSubClient hands out mutable buffers; keep that contract.
      std::string topic = m.topic;
      g_cb(topic.data(), m.payload.data(), (unsigned int)m.payload.size());
    }
  }
  return true;
}

bool PubSubClient::publish(const char* topic, const char* payload) {
  return publish(topic, payload, false);
}
bool PubSubClient::publish(const char* topic, const char* payload, bool retained) {
  if (!connected()) return false;
  js_mqtt_out(topic ? topic : "", payload ? payload : "", retained ? 1 : 0);
  return true;
}
bool PubSubClient::publish(const char* topic, const uint8_t* payload,
                           unsigned int len, bool retained) {
  if (!connected()) return false;
  std::string p((const char*)payload, len);
  js_mqtt_out(topic ? topic : "", p.c_str(), retained ? 1 : 0);
  return true;
}
bool PubSubClient::subscribe(const char* topic) {
  if (!connected()) return false;
  g_subs.push_back(topic ? topic : "");
  js_mqtt_sub(topic ? topic : "");
  return true;
}
bool PubSubClient::subscribe(const char* topic, uint8_t /*qos*/) {
  return subscribe(topic);
}
bool PubSubClient::unsubscribe(const char*) { return true; }
