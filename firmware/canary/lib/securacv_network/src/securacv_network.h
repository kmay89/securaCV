/*
 * SecuraCV Canary — Network Management
 *
 * WiFi AP, mDNS, HTTP server, and OTA receiver.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_NETWORK_H
#define SECURACV_NETWORK_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "esp_http_server.h"
#include "canary_config.h"

#if FEATURE_WIFI_AP || FEATURE_HTTP_SERVER

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

enum WiFiProvState : uint8_t {
  WIFI_PROV_IDLE         = 0,
  WIFI_PROV_SCANNING     = 1,
  WIFI_PROV_CONNECTING   = 2,
  WIFI_PROV_CONNECTED    = 3,
  WIFI_PROV_FAILED       = 4,
  WIFI_PROV_AP_ONLY      = 5
};

struct WiFiCredentials {
  char ssid[33];
  char password[65];
  bool enabled;
  bool configured;
};

struct WiFiStatus {
  WiFiProvState state;
  bool ap_active;
  bool sta_connected;
  int8_t rssi;
  char sta_ip[16];
  char ap_ip[16];
  uint8_t ap_clients;
  uint32_t connect_attempts;
  uint32_t last_connect_ms;
  uint32_t connected_since_ms;
};

// ════════════════════════════════════════════════════════════════════════════
// NETWORK MANAGER
// ════════════════════════════════════════════════════════════════════════════

class NetworkManager {
public:
  NetworkManager();

  // Initialize WiFi provisioning (AP + optional STA)
  bool begin(const char* ap_ssid, const char* ap_password = AP_PASSWORD_DEFAULT);

  // HTTP server
  bool startHttpServer();
  void stopHttpServer();

  // Status
  const WiFiStatus& getStatus() const { return m_status; }
  const WiFiCredentials& getCredentials() const { return m_creds; }

  // WiFi provisioning
  bool loadCredentials();
  bool saveCredentials();
  bool clearCredentials();
  void connectToHome();
  void updateStatus();
  void checkConnection();

  // State name
  static const char* stateName(WiFiProvState s);

  // HTTP server handle (for external handlers)
  httpd_handle_t getHttpServer() const { return m_http_server; }

private:
  void registerHttpHandlers();

  WiFiCredentials m_creds;
  WiFiStatus m_status;
  httpd_handle_t m_http_server;
  bool m_scan_in_progress;
};

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL INSTANCE
// ════════════════════════════════════════════════════════════════════════════

NetworkManager& network_get_instance();

// Convenience functions
bool network_init(const char* ap_ssid, const char* ap_password = AP_PASSWORD_DEFAULT);
bool network_start_http();
void network_update();
httpd_handle_t network_get_http_server();

// HTTP response helpers
esp_err_t http_send_json(httpd_req_t* req, const char* json);
esp_err_t http_send_error(httpd_req_t* req, int status_code, const char* error_code);

#endif // FEATURE_WIFI_AP || FEATURE_HTTP_SERVER

#endif // SECURACV_NETWORK_H
