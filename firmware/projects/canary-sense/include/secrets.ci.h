#pragma once

// CI stub — provides dummy credentials so the build compiles.
// Real credentials live in secrets/secrets.h (git-ignored).

#define WIFI_SSID "ci-placeholder"
#define WIFI_PASS "ci-placeholder"

#define MQTT_HOST "127.0.0.1"
#define MQTT_PORT 1883

#define MQTT_USER nullptr
#define MQTT_PASS nullptr
