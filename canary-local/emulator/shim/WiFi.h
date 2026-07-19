// canary-local/emulator/shim/WiFi.h — scenario-controlled radio.
//
// The emulator replaces wifi_mgr.cpp wholesale (emu_wifi.cpp implements
// the canary::net contract), so this header only has to satisfy the
// types other firmware TUs name: WiFiClient for PubSubClient's socket,
// and a WiFi object for incidental includes. Link state comes from the
// scenario ("unplug the router" is a button on the page).
#pragma once

#include <stdint.h>
#include <stdio.h>

#include "WString.h"

class IPAddress {
 public:
  IPAddress() : a_(192), b_(168), c_(1), d_(50) {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : a_(a), b_(b), c_(c), d_(d) {}
  String toString() const {
    char buf[20];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a_, b_, c_, d_);
    return String(buf);
  }
 private:
  uint8_t a_, b_, c_, d_;
};

class WiFiClient {
 public:
  int connected() { return 0; }
  void stop() {}
};

typedef enum {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_DISCONNECTED = 6,
} wl_status_t;

typedef enum { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 } wifi_mode_t;

class WiFiClass {
 public:
  void mode(wifi_mode_t) {}
  void begin(const char*, const char*) {}
  wl_status_t status();
  int32_t RSSI();
  IPAddress localIP() { return IPAddress(); }
  void disconnect(bool = false) {}
  void persistent(bool) {}
  void setSleep(bool) {}
  void setHostname(const char*) {}
};

extern WiFiClass WiFi;
