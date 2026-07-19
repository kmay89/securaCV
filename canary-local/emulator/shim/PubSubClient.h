// canary-local/emulator/shim/PubSubClient.h — the scenario broker.
//
// The display's REAL mqtt_mgr.cpp compiles against this. connect() answers
// according to the scenario's broker switch; inbound fleet payloads the
// page injects are delivered through the registered callback — so the
// firmware's own dispatcher, TOFU pinning, and Ed25519 chain verify run
// on every message, exactly as on glass. Outbound publishes surface in
// the page's MQTT panel: the display's own voice, visible.
#pragma once

#include <stdint.h>
#include <stddef.h>

#define MQTT_CONNECTED 0
#define MQTT_CONNECT_FAILED -2
#define MQTT_DISCONNECTED -1

class WiFiClient;

typedef void (*mqtt_callback_t)(char* topic, uint8_t* payload, unsigned int length);

class PubSubClient {
 public:
  explicit PubSubClient(WiFiClient&) {}
  PubSubClient& setServer(const char* host, uint16_t port);
  PubSubClient& setCallback(mqtt_callback_t cb);
  PubSubClient& setBufferSize(uint16_t size);
  PubSubClient& setKeepAlive(uint16_t) { return *this; }
  PubSubClient& setSocketTimeout(uint16_t) { return *this; }

  bool connect(const char* id);
  bool connect(const char* id, const char* user, const char* pass);
  bool connect(const char* id, const char* user, const char* pass,
               const char* willTopic, uint8_t willQos, bool willRetain,
               const char* willMessage);
  void disconnect();
  bool connected();
  int state();
  bool loop();

  bool publish(const char* topic, const char* payload);
  bool publish(const char* topic, const char* payload, bool retained);
  bool publish(const char* topic, const uint8_t* payload, unsigned int len, bool retained);
  bool subscribe(const char* topic);
  bool subscribe(const char* topic, uint8_t qos);
  bool unsubscribe(const char* topic);
};
