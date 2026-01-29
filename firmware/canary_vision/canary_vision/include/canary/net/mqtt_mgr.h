#pragma once
#include <Arduino.h>
#include "canary/types.h"
#include "canary/topics.h"

namespace canary::net {

  void mqtt_init(const Topics& topics);
  void mqtt_loop();
  bool mqtt_connected();
  void mqtt_reconnect_blocking();

  // Publishing
  void publish_status_retained(const Topics& topics, const char* status);   // online/offline
  void publish_heartbeat(const Topics& topics, const StateSnapshot& s);     // online + booleans
  void publish_state_retained(const Topics& topics, const StateSnapshot& s);
  void publish_event(const Topics& topics, const char* json_payload);       // non-retained

  // HA discovery (retained)
  void ha_discovery_publish_once(const Topics& topics);

} // namespace
