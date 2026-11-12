#pragma once
// Arduino-ESP32 WiFi stand-in for test_improv_glue.cpp: the scan calls
// improv_ble.cpp makes for GET_WIFI_NETWORKS, in WiFiScan.h's shape, over
// IDF's wifi_auth_mode_t (esp_wifi_types.h: 4.4 for core 2, 5.x for core 3,
// which adds OWE), with the status rules of WiFiScan.cpp in the core each
// family builds on: 3.3.8 (pioarduino 55.03.38-1, Sense) when
// ESP_ARDUINO_VERSION_MAJOR >= 3, 2.0.17 (espressif32 6.9.0, Vision) below
// (firmware/envs/platformio/platforms.ini). The two differ where the glue
// can tell:
//   - the give-up. 2.0.17's scanNetworks() sets the timeout to
//     max_ms_per_chan * 20 (6 s for the glue's 300 ms) and scanComplete()
//     answers WIFI_SCAN_FAILED past it; 3.3.8 keeps 60 s. So on core 2 the
//     core gives up on a silent sweep before the glue's own 30 s; on core 3
//     the glue's 30 s comes first.
//   - scanDelete(). 3.3.8 clears the scanning bit as well as the results;
//     2.0.17 clears only the results, so a scanNetworks() after it still
//     answers RUNNING and joins the sweep in flight.
// Both answer WIFI_SCAN_FAILED once the results were handed back
// (scanDelete()) before the caller read them: the setup portal's /scan
// harvests whatever sweep completed, the glue's included (setup_portal.cpp).
// The radio's side is the test's: whether esp_wifi_scan_start() takes the
// sweep (driver_takes), when the driver's scan-done event lands and what it
// heard (scan_done()); millis() is the test's clock (Arduino.h). Every call
// the glue makes is logged in `calls` for the test to read in order.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "Arduino.h"
#include "esp_idf_version.h"

typedef enum {
  WIFI_AUTH_OPEN = 0,
  WIFI_AUTH_WEP,
  WIFI_AUTH_WPA_PSK,
  WIFI_AUTH_WPA2_PSK,
  WIFI_AUTH_WPA_WPA2_PSK,
  WIFI_AUTH_ENTERPRISE,
  WIFI_AUTH_WPA2_ENTERPRISE = WIFI_AUTH_ENTERPRISE,
  WIFI_AUTH_WPA3_PSK,
  WIFI_AUTH_WPA2_WPA3_PSK,
  WIFI_AUTH_WAPI_PSK,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  WIFI_AUTH_OWE,
#endif
  WIFI_AUTH_WPA3_ENT_192,
  WIFI_AUTH_MAX
} wifi_auth_mode_t;

#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED (-2)

class WiFiT {
 public:
  struct Row {
    std::string ssid;
    int32_t rssi;
    wifi_auth_mode_t auth;
  };
  // The driver, the test's to set: esp_wifi_scan_start() answers ESP_OK.
  bool driver_takes = true;
  // What happened: sweeps the driver took, and each call by name.
  int scans_started = 0;
  std::vector<std::string> calls;
  int count(const char* call) const {
    int n = 0;
    for (const std::string& c : calls) n += c == call;
    return n;
  }

  // The driver's scan-done event (WiFiScanClass::_scanDone): the rows it heard.
  void scan_done(const std::vector<Row>& heard) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    if (!scanning_) return;   // 3.3.8: "scan was started by other"
#else
    started_ = 0;             // 2.0.17: "reset after a scan is completed"
#endif
    results_ = heard;
    done_ = true;
    scanning_ = false;
  }

  int16_t scanNetworks(bool async = false, bool /*show_hidden*/ = false, bool /*passive*/ = false,
                       uint32_t max_ms_per_chan = 300, uint8_t /*channel*/ = 0,
                       const char* /*ssid*/ = nullptr, const uint8_t* /*bssid*/ = nullptr) {
    calls.push_back("scanNetworks");
    if (scanning_) return WIFI_SCAN_RUNNING;
#if ESP_ARDUINO_VERSION_MAJOR < 3
    timeout_ = max_ms_per_chan * 20;
#else
    (void)max_ms_per_chan;
#endif
    hand_back();   // both cores' scanNetworks() call scanDelete() first
    if (!driver_takes) return WIFI_SCAN_FAILED;
    if (!async) {
      // A blocking sweep waits on the driver's event inside this call; the
      // glue never asks for one and this stand-in does not play it.
      fprintf(stderr, "WiFi stand-in: a blocking scanNetworks() is not modeled\n");
      abort();
    }
    ++scans_started;
    started_ = millis();
#if ESP_ARDUINO_VERSION_MAJOR < 3
    if (!started_) ++started_;
#endif
    done_ = false;
    scanning_ = true;
    return WIFI_SCAN_RUNNING;
  }

  int16_t scanComplete() {
    calls.push_back("scanComplete");
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    if (done_) return (int16_t)results_.size();
    if (scanning_) {
      if (millis() - started_ > timeout_) {
        scanning_ = false;
        return WIFI_SCAN_FAILED;
      }
      return WIFI_SCAN_RUNNING;
    }
    return WIFI_SCAN_FAILED;
#else
    if (started_ && millis() - started_ > timeout_) {
      scanning_ = false;
      return WIFI_SCAN_FAILED;
    }
    if (done_) return (int16_t)results_.size();
    if (scanning_) return WIFI_SCAN_RUNNING;
    return WIFI_SCAN_FAILED;
#endif
  }

  void scanDelete() {
    calls.push_back("scanDelete");
    hand_back();
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    scanning_ = false;   // 3.3.8 clears the scanning bit too; 2.0.17 does not
#endif
  }

  // _getScanInfoByIndex(): nothing past the results the event copied.
  String SSID(uint8_t i) { return i < results_.size() ? String(results_[i].ssid.c_str()) : String(); }
  int32_t RSSI(uint8_t i) { return i < results_.size() ? results_[i].rssi : 0; }
  wifi_auth_mode_t encryptionType(uint8_t i) { return i < results_.size() ? results_[i].auth : WIFI_AUTH_OPEN; }

 private:
  void hand_back() {
    done_ = false;
    results_.clear();
  }
  bool scanning_ = false;   // WIFI_SCANNING_BIT
  bool done_ = false;       // WIFI_SCAN_DONE_BIT
  uint32_t started_ = 0;    // _scanStarted
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  uint32_t timeout_ = 60000;   // 3.3.8's _scanTimeout
#else
  uint32_t timeout_ = 10000;   // 2.0.17's, until scanNetworks() sets it
#endif
  std::vector<Row> results_;
};
extern WiFiT WiFi;
