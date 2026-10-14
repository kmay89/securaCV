/* Host stand-in for <WiFi.h> (the canary-wap BLE provisioning host harness,
 * test_ble_provision.cpp, the F262 review): the calls ble_provision.cpp
 * makes, over a scan the test sets up.
 *   - scanNetworks(async = true, ...) answers WIFI_SCAN_RUNNING, as
 *     arduino-esp32 does for an async start the radio took (WIFI_SCAN_FAILED
 *     when the test says the radio refused);
 *   - scanComplete() answers WIFI_SCAN_RUNNING until the test finishes the
 *     scan, then the number of networks found;
 *   - SSID(i), RSSI(i) and encryptionType(i) read the i-th network the test
 *     set, in the order the test set them (the core sorts a scan by RSSI,
 *     strongest first; the test sets them so), and an index past them reads
 *     "" / 0 / WIFI_AUTH_OPEN, as the core does;
 *   - scanDelete() forgets the results; status() answers what the test set. */
#ifndef STUB_BLE_PROV_WIFI_H
#define STUB_BLE_PROV_WIFI_H

#include <stdint.h>

#include <string>
#include <vector>

#include "Arduino.h"   // String

#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED  (-2)

typedef enum {
  WIFI_AUTH_OPEN = 0,
  WIFI_AUTH_WEP,
  WIFI_AUTH_WPA_PSK,
  WIFI_AUTH_WPA2_PSK,
  WIFI_AUTH_WPA_WPA2_PSK,
  WIFI_AUTH_WPA2_ENTERPRISE,
  WIFI_AUTH_WPA3_PSK,
  WIFI_AUTH_WPA2_WPA3_PSK,
} wifi_auth_mode_t;

typedef enum {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6,
} wl_status_t;

struct HostNetwork {
  std::string ssid;
  int32_t rssi;
  wifi_auth_mode_t auth;
};

class HostWiFi {
 public:
  // The test's side.
  std::vector<HostNetwork> networks;
  bool refuse_scan = false;
  bool scan_done = false;
  wl_status_t station = WL_DISCONNECTED;
  int scans = 0;
  int deletes = 0;

  // ble_provision.cpp's side.
  int16_t scanNetworks(bool async = false, bool /*show_hidden*/ = false, bool /*passive*/ = false) {
    ++scans;
    scan_done = !async;
    if (refuse_scan) return WIFI_SCAN_FAILED;
    return async ? WIFI_SCAN_RUNNING : (int16_t)networks.size();
  }
  int16_t scanComplete() const { return scan_done ? (int16_t)networks.size() : WIFI_SCAN_RUNNING; }
  void scanDelete() { ++deletes; }
  String SSID(uint8_t i) const { return i < networks.size() ? String(networks[i].ssid) : String(""); }
  int32_t RSSI(uint8_t i) const { return i < networks.size() ? networks[i].rssi : 0; }
  wifi_auth_mode_t encryptionType(uint8_t i) const { return i < networks.size() ? networks[i].auth : WIFI_AUTH_OPEN; }
  wl_status_t status() const { return station; }
};
inline HostWiFi WiFi;

#endif
