#pragma once
/*
  secrets.example.h
  -----------------
  Copy to: secrets.h

  DO NOT COMMIT secrets.h
  This repo ignores it by default.
*/

// WiFi
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASS "your_wifi_password"

// MQTT broker
#define MQTT_HOST "192.168.1.10"
#define MQTT_PORT 1883

// Optional MQTT auth (use nullptr if your broker allows anonymous)
#define MQTT_USER "securacv"
#define MQTT_PASS "your_mqtt_password"

// Optional: local timezone for the clock and quiet-hours night mode
// (POSIX TZ string; default is UTC). Examples:
//   US Eastern:  "EST5EDT,M3.2.0,M11.1.0"
//   US Pacific:  "PST8PDT,M3.2.0,M11.1.0"
//   Central EU:  "CET-1CEST,M3.5.0,M10.5.0/3"
// #define CD_TZ "EST5EDT,M3.2.0,M11.1.0"

