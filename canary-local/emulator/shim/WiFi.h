// canary-local/emulator/shim/WiFi.h — scenario-controlled radio.
//
// Two firmware surfaces reach the radio, and the shim answers each at the
// silicon line, never above it:
//
//  * The steady-state STA supervisor (wifi_mgr.cpp) is replaced wholesale —
//    emu_net.cpp implements the canary::net contract against the page's
//    Wi-Fi switch ("unplug the router" is a button), through the shared
//    common/network/wifi_join_policy.h. Other TUs only name WiFiClient (for
//    PubSubClient's socket) and WiFi.localIP().
//
//  * The first-boot portal (net/provision.cpp) compiles VERBATIM, so the
//    Arduino calls it makes are real here: a SoftAP a phone can join, an
//    async 13-channel scan, and a STA join whose verdict comes from the
//    staged LAN (emu_radio.cpp). The shim decides nothing the firmware
//    decides: it plays the radio and the neighborhood's routers — which
//    networks are on the air, which key each one takes — and the firmware's
//    own code turns that into the list, the reason and the glass.
//
// The WebServer and the WiFiUDP socket the portal serves on are their own
// shims (WebServer.h, WiFiUdp.h).
#pragma once

#include <stdint.h>
#include <stdio.h>

#include "WString.h"

class IPAddress {
 public:
  IPAddress() : b_{192, 168, 1, 50} {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : b_{a, b, c, d} {}
  uint8_t operator[](int i) const { return (i >= 0 && i < 4) ? b_[i] : 0; }
  String toString() const {
    char buf[20];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b_[0], b_[1], b_[2], b_[3]);
    return String(buf);
  }

 private:
  uint8_t b_[4];
};

class WiFiClient {
 public:
  int connected() { return 0; }
  void stop() {}
};

typedef enum {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6,
} wl_status_t;

typedef enum { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 } wifi_mode_t;

// esp_wifi_types.h spellings (the two the portal reads).
typedef enum {
  WIFI_AUTH_OPEN = 0,
  WIFI_AUTH_WEP = 1,
  WIFI_AUTH_WPA_PSK = 2,
  WIFI_AUTH_WPA2_PSK = 3,
} wifi_auth_mode_t;

// WiFiScan.h: scanComplete() while a sweep runs / when none exists.
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED (-2)

class WiFiClass {
 public:
  bool mode(wifi_mode_t m);
  wifi_mode_t getMode();

  // STA. begin() starts an association against the staged LAN; status()
  // reports it (WL_DISCONNECTED while in flight, then the router's verdict).
  // With no portal join in play, status() is the page's Wi-Fi switch — the
  // emulated supervisor's link, as before.
  wl_status_t begin(const char* ssid, const char* passphrase = nullptr);
  wl_status_t status();
  int32_t RSSI();
  IPAddress localIP() { return IPAddress(); }
  bool disconnect(bool wifioff = false, bool eraseap = false);
  void persistent(bool) {}
  void setSleep(bool) {}
  void setHostname(const char*) {}

  // Scan (WiFiScan.h). Async only matters here: the portal never blocks on
  // a sweep, and a sweep takes 13 channels x max_ms_per_chan of virtual time.
  int16_t scanNetworks(bool async = false, bool show_hidden = false,
                       bool passive = false, uint32_t max_ms_per_chan = 300,
                       uint8_t channel = 0);
  int16_t scanComplete();
  void scanDelete();
  String SSID(uint8_t i);
  int32_t RSSI(uint8_t i);
  wifi_auth_mode_t encryptionType(uint8_t i);

  // SoftAP (WiFiAP.h). The phone on the page joins it with the key the
  // firmware chose (the one its glass QR carries).
  bool softAP(const char* ssid, const char* passphrase = nullptr,
              int channel = 1, int ssid_hidden = 0, int max_connection = 4);
  bool softAPdisconnect(bool wifioff = false);
  uint8_t softAPgetStationNum();
  IPAddress softAPIP() { return IPAddress(192, 168, 4, 1); }
};

extern WiFiClass WiFi;
