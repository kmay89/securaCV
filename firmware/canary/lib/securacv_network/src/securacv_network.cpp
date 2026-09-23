/*
 * SecuraCV Canary — Network Management Implementation
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_network.h"
#include "securacv_witness.h"
#include "securacv_crypto.h"
#include "securacv_auth.h"
#include "securacv_setup.h"

#if FEATURE_WIFI_AP || FEATURE_HTTP_SERVER

#include <ArduinoJson.h>
// F20 gap #11: BOOT-tap provisioning gate + the page-token policy (pure,
// host-tested) and the salted hardware pseudonym the receipt carries instead
// of a MAC (privacy Invariant III). Both live under firmware/common on the
// project's -I path.
#include "network/provisioning_gate.h"
#include "identity/device_pseudonym.h"
// getpeername()/getsockname() + the AP/STA netif addresses for the
// interface-scoped page-token policy and the receipt's base_url (the address
// the request actually arrived on).
#include <lwip/sockets.h>
#include <esp_netif.h>

// F15: self-signed HTTPS. One code path for both cores — dev/release/board
// envs are Arduino 2.0.17 / IDF 4.4.7, [env:full] is core 3.3.8 / IDF 5.5.4 —
// so every capability is detected, never assumed:
//   * SECURACV_HAS_HTTPS_SERVER: FEATURE_HTTPS on, the header present, AND the
//     core's prebuilt sdkconfig built the component (the header can exist
//     while the library is compiled out; that would only fail at link);
//   * SECURACV_HAS_TLS_CERTGEN: mbedTLS built with everything an on-device
//     ECDSA P-256 self-signed certificate needs.
// Whatever is missing compiles OUT and the device runs HTTP-only, naming the
// reason in /api/status tls_mode_reason (tls_policy::decide).
#include "network/tls_policy.h"
#include <esp_idf_version.h>
#include <sdkconfig.h>

// F16: SoftAP WPA2/WPA3 transition + PMF, STA PMF. The AP-side PMF config and
// SoftAP SAE exist only on cores whose prebuilt sdkconfig enables SoftAP SAE
// (an IDF 5.x feature; the IDF 4.4 core has no ap.pmf_cfg at all), so the
// whole AP write compiles out elsewhere and ap_security::decide reports why.
#include "network/ap_security_policy.h"
#include <esp_wifi.h>
#if defined(CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT) && CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT && \
    ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  #define CANARY_SOFTAP_SAE_IN_BUILD 1
#else
  #define CANARY_SOFTAP_SAE_IN_BUILD 0
#endif
// label_for() maps ESP-IDF's auth-mode numbers without including esp_wifi;
// pin the ones the firmware relies on to the real enum.
static_assert((int)WIFI_AUTH_OPEN == canary::net::ap_security::kAuthOpen,
              "ap_security_policy.h: WIFI_AUTH_OPEN renumbered");
static_assert((int)WIFI_AUTH_WPA2_PSK == canary::net::ap_security::kAuthWpa2Psk,
              "ap_security_policy.h: WIFI_AUTH_WPA2_PSK renumbered");
static_assert((int)WIFI_AUTH_WPA2_WPA3_PSK == canary::net::ap_security::kAuthWpa2Wpa3Psk,
              "ap_security_policy.h: WIFI_AUTH_WPA2_WPA3_PSK renumbered");
// IDF 5.x names the httpd control port's default; IDF 4.4 (Arduino core
// 2.0.17, the canary's pinned core) only writes the literal inside
// HTTPD_DEFAULT_CONFIG(). Same number either way.
#ifndef ESP_HTTPD_DEF_CTRL_PORT
  #define ESP_HTTPD_DEF_CTRL_PORT 32768
#endif
#if FEATURE_HTTPS && __has_include("esp_https_server.h") && \
    defined(CONFIG_ESP_HTTPS_SERVER_ENABLE) && CONFIG_ESP_HTTPS_SERVER_ENABLE
  #include "esp_https_server.h"
  #define SECURACV_HAS_HTTPS_SERVER 1
#else
  #define SECURACV_HAS_HTTPS_SERVER 0
#endif
#if SECURACV_HAS_HTTPS_SERVER && __has_include("mbedtls/x509write_crt.h")
  #include "mbedtls/x509write_crt.h"
  #include "mbedtls/pk.h"
  #include "mbedtls/ecp.h"
  #include "mbedtls/entropy.h"
  #include "mbedtls/ctr_drbg.h"
  #include "mbedtls/version.h"
  #if defined(MBEDTLS_X509_CRT_WRITE_C) && defined(MBEDTLS_PK_WRITE_C) && \
      defined(MBEDTLS_ECP_C) && defined(MBEDTLS_ECDSA_C) && \
      defined(MBEDTLS_ECP_DP_SECP256R1_ENABLED) && \
      defined(MBEDTLS_CTR_DRBG_C) && defined(MBEDTLS_ENTROPY_C)
    #define SECURACV_HAS_TLS_CERTGEN 1
  #else
    #define SECURACV_HAS_TLS_CERTGEN 0
  #endif
#else
  #define SECURACV_HAS_TLS_CERTGEN 0
#endif

#if FEATURE_SD_STORAGE
#include "securacv_storage.h"
// Card pages for the timeline (F35): handle_witness asks the loop task for
// them through this bridge — the httpd task never opens a file on the card.
#include "securacv_witness_history.h"
#endif

#if FEATURE_WATCHDOG
#include "esp_task_wdt.h"
#include <esp_wifi.h>   // esp_wifi_set_protocol/bandwidth/country_code/max_tx_power
#endif

#if FEATURE_CAMERA_PEEK
#include "securacv_camera.h"
#include <lwip/sockets.h>
#include <netinet/tcp.h>
#endif

#if FEATURE_OTA_UPDATE && !defined(SECURACV_BUILD_RELEASE)
#include <Update.h>
#endif

#if FEATURE_OTA_PULL
#include "securacv_ota.h"
#endif

#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
#include "securacv_sensing.h"
#endif
#if FEATURE_CSI
#include "securacv_csi.h"
#endif
#if FEATURE_ACOUSTIC_EVENTS
#include "securacv_audio.h"
#endif
#if FEATURE_TOUCH
#include "securacv_touch.h"
#endif
#if FEATURE_IR_RMT
#include "securacv_ir.h"
#endif
#if FEATURE_TEMP_TAMPER
#include "securacv_envsens.h"
#endif
#if FEATURE_VISION_DETECT
#include "securacv_vision.h"
#endif
/* Lowpower HAL is always compiled when any sensing is on, so the
 * Sensing endpoint can surface the wake reason and capability bits. */
#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
#include "securacv_lowpower.h"
#endif
#if FEATURE_DIAGNOSTICS
#include "securacv_diagnostics.h"
#endif
#if FEATURE_POWER_MONITOR
#include "securacv_power.h"
#endif
#if FEATURE_THERMAL_WATCHDOG
#include "securacv_thermal_watchdog.h"
#include <math.h>    /* lroundf */
#include <stdarg.h>  /* thermal_json_append */
#endif
// BLE Scout pairing surface (repo sweep F27). Same gate as the Scout lib
// itself: platformio.ini lib_ignores securacv_ble_scan outside [env:full].
#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
#include "ble_scout.h"
#endif

// Mesh REST API (PR-8, F10). Gated on FEATURE_MESH_NETWORK, which only
// [env:full] turns on; CI compiles that env (flavors.json build_envs), so
// the handlers build on every PR. What they EMIT is proven separately:
// the JSON-building logic lives in the pure mesh_api builders, which the
// securacv_mesh host tests exercise.
#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
#include "mesh_session.h"
#include "mesh_state.h"
#include "mesh_transport.h"
#include "mesh_crypto.h"
#include "mesh_api.h"
#include <esp_flash_encrypt.h>
#endif

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL INSTANCE
// ════════════════════════════════════════════════════════════════════════════

static ScvNetworkManager s_network;

ScvNetworkManager& network_get_instance() {
  return s_network;
}

// ════════════════════════════════════════════════════════════════════════════
// HTTP RESPONSE HELPERS
// ════════════════════════════════════════════════════════════════════════════

esp_err_t http_send_json(httpd_req_t* req, const char* json) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t http_send_error(httpd_req_t* req, int status_code, const char* error_code) {
  // Every status a caller actually passes must map, or it silently ships as
  // 400: the OTA handlers' 409 ("busy — retry") degraded exactly that way,
  // so the desktop Flasher's retry-on-busy branch never fired.
  httpd_resp_set_status(req, status_code == 400 ? "400 Bad Request" :
                              status_code == 404 ? "404 Not Found" :
                              status_code == 409 ? "409 Conflict" :
                              status_code == 413 ? "413 Payload Too Large" :
                              status_code == 503 ? "503 Service Unavailable" :
                              status_code == 504 ? "504 Gateway Timeout" :
                              status_code == 500 ? "500 Internal Server Error" : "400 Bad Request");
  char response[128];
  snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}", error_code);
  return http_send_json(req, response);
}

// ════════════════════════════════════════════════════════════════════════════
// NETWORK MANAGER IMPLEMENTATION
// ════════════════════════════════════════════════════════════════════════════

ScvNetworkManager::ScvNetworkManager()
  : m_http_server(nullptr),
    m_scan_in_progress(false),
    m_peers_last_browse_ms(0) {
  memset(&m_creds, 0, sizeof(m_creds));
  memset(&m_status, 0, sizeof(m_status));
  memset(m_peers, 0, sizeof(m_peers));
  m_mdns_hostname[0] = '\0';
  m_mdns_device_id[0] = '\0';
  m_ap_ssid[0] = '\0';
  m_ap_password[0] = '\0';
  m_https_server = nullptr;
  m_tls_enabled = false;
  m_tls_cert_der = nullptr;
  m_tls_cert_der_len = 0;
  m_tls_key_der = nullptr;
  m_tls_key_der_len = 0;
  m_tls_cert_fp_hex[0] = '\0';
  m_tls_reason = "initTls() not called (first-boot setup, or FEATURE_HTTPS=0)";
  m_tls_deferred_for_setup = false;
}

const char* ScvNetworkManager::getTlsModeReason() const {
  return canary::net::tls_policy::live_reason(
      m_tls_reason, m_tls_deferred_for_setup,
      setup_is_active() || setup_is_first_boot());
}

// F4 grace window: keep the SoftAP up this long after the STA link reports
// connected before tearing it down, so a just-provisioned phone still on the
// AP sees the "connected" result before its association drops. Mirrors the
// canary-wap sketch's AP_DROP_GRACE_MS.
static const uint32_t AP_DROP_GRACE_MS = 8000;

// Hard cap on how long an associated AP client can extend that grace (see the
// F4 drop in checkConnection). The captive sheet holds the phone on the AP
// while the wizard's success screen offers the optional hub step, so we won't
// yank the AP mid-wizard — but a sheet left open forever must not pin the
// unstable AP+STA+BLE radio combo, so coexistence stability wins after this.
static const uint32_t AP_CLIENT_HOLD_MAX_MS = 10UL * 60UL * 1000UL;

// mDNS hostname rules (RFC 6762 §16): only [a-z0-9-], must not start/end with
// hyphen. We lowercase the device_id and replace any other byte with '-'.
static void sanitize_mdns_hostname(const char* in, char* out, size_t cap) {
  if (cap == 0) return;
  size_t j = 0;
  for (size_t i = 0; in && in[i] && j < cap - 1; i++) {
    char c = in[i];
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
      out[j++] = c;
    } else {
      out[j++] = '-';
    }
  }
  // Trim leading/trailing hyphens
  while (j > 0 && out[j-1] == '-') j--;
  size_t start = 0;
  while (start < j && out[start] == '-') start++;
  if (start > 0) {
    memmove(out, out + start, j - start);
    j -= start;
  }
  if (j == 0) {
    // Fallback to a safe default rather than emitting an empty name.
    const char* fb = "canary";
    size_t fb_len = strlen(fb);
    if (fb_len >= cap) fb_len = cap - 1;
    memcpy(out, fb, fb_len);
    j = fb_len;
  }
  out[j] = '\0';
}

const char* ScvNetworkManager::stateName(WiFiProvState s) {
  switch (s) {
    case WIFI_PROV_IDLE:       return "idle";
    case WIFI_PROV_SCANNING:   return "scanning";
    case WIFI_PROV_CONNECTING: return "connecting";
    case WIFI_PROV_CONNECTED:  return "connected";
    case WIFI_PROV_FAILED:     return "failed";
    case WIFI_PROV_AP_ONLY:    return "ap_only";
    default:                   return "unknown";
  }
}

// The mDNS hostname this device advertises. Constant — RFC 6762 §9
// conflict resolution renames the second one to canary-2.local etc.,
// so multi-device homes still work; the SPA's _securacv._tcp browse
// uses the device_id TXT record to distinguish them.
static constexpr const char* MDNS_HOSTNAME = "canary";

// Stash the device_id between begin() and the deferred mDNS re-init
// triggered by the STA_GOT_IP event. The event handler is a member
// callback; it reads this back to repopulate the TXT records on the
// home-WiFi interface.
static char s_mdns_device_id[40] = {0};

// Helper: bring mDNS up on whichever netif is currently routable. We
// call MDNS.end() first because ESP-IDF mDNS doesn't auto-re-announce
// when a new netif gains an IP — it binds to the interfaces that were
// up at begin() time and stays there. After STA gets DHCP we have to
// end() and begin() again to advertise on the home WiFi interface.
static void start_mdns(const char* device_id) {
  MDNS.end();
  if (!MDNS.begin(MDNS_HOSTNAME)) {
    log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK,
               "mDNS begin failed", MDNS_HOSTNAME);
    return;
  }
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("securacv", "tcp", 80);
  MDNS.addServiceTxt("securacv", "tcp", "device_id",
                     (device_id && device_id[0]) ? device_id : MDNS_HOSTNAME);
  MDNS.addServiceTxt("securacv", "tcp", "fw", FIRMWARE_VERSION);
  MDNS.addServiceTxt("securacv", "tcp", "model", "XIAO ESP32S3");
  char fqdn[48];
  snprintf(fqdn, sizeof(fqdn), "%s.local", MDNS_HOSTNAME);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "mDNS started", fqdn);
}

// ════════════════════════════════════════════════════════════════════════════
// F16: SOFTAP / STA SECURITY (ap_security_policy.h, host-tested)
// ════════════════════════════════════════════════════════════════════════════

// Called right after every WiFi.softAP() (begin, raiseAp): the Arduino call
// always brings the AP up as WPA2-PSK; this asks the driver for WPA2/WPA3
// transition + PMF-capable when the policy allows it, and records what is
// actually on the air. Both call sites run before any client has joined, so
// the brief AP restart esp_wifi_set_config causes disrupts nobody.
static void apply_ap_security(WiFiStatus& st) {
  namespace aps = canary::net::ap_security;
  wifi_config_t c;
  memset(&c, 0, sizeof(c));
  size_t pw_len = 0;
  if (esp_wifi_get_config(WIFI_IF_AP, &c) == ESP_OK) {
    pw_len = strnlen((const char*)c.ap.password, sizeof(c.ap.password));
  }
  aps::Decision d = aps::decide(CANARY_AP_WPA3_TRANSITION != 0, CANARY_SOFTAP_SAE_IN_BUILD != 0, pw_len);
#if CANARY_SOFTAP_SAE_IN_BUILD
  if (d.mode == aps::AuthMode::WPA2_WPA3_TRANSITION) {
    c.ap.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
    c.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
    c.ap.pmf_cfg.capable = d.pmf_capable;
    c.ap.pmf_cfg.required = d.pmf_required;  // never true: WPA2 clients must still join
    const bool accepted = (esp_wifi_set_config(WIFI_IF_AP, &c) == ESP_OK);
    d = aps::after_driver(d, accepted);
    if (!accepted) {
      log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK,
                 "WPA3 SoftAP refused by driver", "WPA2-PSK kept");
    }
  }
#endif
  secure_zero(&c, sizeof(c));  // the read-back carries the AP passphrase
  strncpy(st.ap_auth, d.label, sizeof(st.ap_auth) - 1);
  st.ap_auth[sizeof(st.ap_auth) - 1] = '\0';
  st.ap_auth_reason = d.reason;
  Serial.printf("[WIFI] SoftAP security: %s (%s)\n", d.label, d.reason);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "SoftAP security", d.label);
}

// Called right after WiFi.begin(): the STA side asks for PMF capable, not
// required (a required PMF would refuse every router without it). On IDF 5.x
// the driver is always PMF-capable (pmf_cfg.capable is documented as
// deprecated there), so nothing is written. On IDF 4.4 the config is read
// back and written only when it does not already say capable — a needless
// esp_wifi_set_config restarts the association.
static bool ensure_sta_pmf_capable() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  return true;
#else
  wifi_config_t c;
  memset(&c, 0, sizeof(c));
  bool capable = false;
  if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK) {
    capable = c.sta.pmf_cfg.capable;
    if (!capable) {
      c.sta.pmf_cfg.capable = true;
      c.sta.pmf_cfg.required = false;
      capable = (esp_wifi_set_config(WIFI_IF_STA, &c) == ESP_OK);
      if (!capable) {
        log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "STA PMF config refused", nullptr);
      }
    }
  }
  secure_zero(&c, sizeof(c));  // the read-back carries the home Wi-Fi passphrase
  return capable;
#endif
}

bool ScvNetworkManager::begin(const char* ap_ssid, const char* ap_password,
                           const char* device_id) {
  // Load saved credentials
  bool has_creds = loadCredentials();

  // Stash the AP credentials so raiseAp() can bring the SoftAP back up after a
  // home-WiFi drop (see F4 coexistence handling in checkConnection()). Guard
  // for null like the device_id handling below — begin() is a public API.
  if (ap_ssid) {
    strncpy(m_ap_ssid, ap_ssid, sizeof(m_ap_ssid) - 1);
    m_ap_ssid[sizeof(m_ap_ssid) - 1] = '\0';
  } else {
    m_ap_ssid[0] = '\0';
  }
  if (ap_password) {
    strncpy(m_ap_password, ap_password, sizeof(m_ap_password) - 1);
    m_ap_password[sizeof(m_ap_password) - 1] = '\0';
  } else {
    m_ap_password[0] = '\0';
  }

  // Credentials live in our own NVS keys (NVS_KEY_WIFI_*); stop the Arduino
  // core from ALSO writing them into the SDK's wifi NVS namespace on every
  // WiFi.begin() (default persistent=true). That double-write wears flash and
  // can auto-rejoin a network from stale SDK creds after clearCredentials().
  // Mirrors the canary-wap sketch.
  WiFi.persistent(false);

  // Always use AP+STA mode
  WiFi.mode(WIFI_AP_STA);

  // Set the WiFi STA hostname BEFORE softAP() / begin() so DHCP also
  // propagates "canary" to the home router — some routers/clients
  // resolve via DHCP hostname rather than mDNS.
  WiFi.setHostname(MDNS_HOSTNAME);

  // Stash the device_id in two places:
  //   • The file-static so the deferred STA_GOT_IP re-announce lambda
  //     can read it (lambdas with empty captures can't see members).
  //   • The class member so browsePeers() can self-filter by TXT record
  //     (every device shares the same mDNS hostname now, so the old
  //     hostname-based filter would drop legitimate peers).
  if (device_id && device_id[0]) {
    strncpy(s_mdns_device_id, device_id, sizeof(s_mdns_device_id) - 1);
    s_mdns_device_id[sizeof(s_mdns_device_id) - 1] = '\0';
    strncpy(m_mdns_device_id, device_id, sizeof(m_mdns_device_id) - 1);
    m_mdns_device_id[sizeof(m_mdns_device_id) - 1] = '\0';
  } else {
    s_mdns_device_id[0] = '\0';
    m_mdns_device_id[0] = '\0';
  }

  // Record the active hostname for getMdnsHostname() consumers.
  sanitize_mdns_hostname(MDNS_HOSTNAME, m_mdns_hostname,
                         sizeof(m_mdns_hostname));

  // Register the STA_GOT_IP handler ONCE per boot. The lambda calls
  // back into the singleton — no captured state.
  static bool s_event_registered = false;
  if (!s_event_registered) {
    WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t /*info*/) {
      if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        // STA just got DHCP. Re-init mDNS so it advertises on both
        // AP and STA interfaces. Without this the home-WiFi side
        // can't resolve canary.local.
        start_mdns(s_mdns_device_id);
#if FEATURE_CSI
        // CSI transmitter filter: follow the (re)association on the next
        // csi::process() tick. Flag only — no driver call on this task.
        csi::request_bssid_refresh();
#endif
      }
    });
    s_event_registered = true;
  }

  // Start Access Point
  bool ap_ok = WiFi.softAP(ap_ssid, ap_password, AP_CHANNEL, false, AP_MAX_CONNECTIONS);

  if (!ap_ok) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_NETWORK, "WiFi AP start failed", nullptr);
    return false;
  }
  apply_ap_security(m_status);  // F16: WPA2/WPA3 transition + PMF when the core allows

  m_status.ap_active = true;
  witness_get_health().wifi_active = true;

  // Pin the radio PHY + regulatory + TX power now that the driver is up
  // (WiFi.mode/softAP have called esp_wifi_start). HT20 + 11bgn on both
  // interfaces keeps the CSI subcarrier count constant; the country code fixes
  // the channel set / TX ceiling (802.11d adapts it to the AP); an explicit
  // max-TX makes range deterministic. The AP has no clients yet at boot, so
  // pinning IF_AP here can't disrupt a live link. All failures are non-fatal.
  {
    if (esp_wifi_set_country_code(CANARY_WIFI_COUNTRY, true) != ESP_OK) {
      log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "WiFi country set failed", nullptr);
    }
    const uint8_t proto = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
    esp_wifi_set_protocol(WIFI_IF_STA, proto);
    esp_wifi_set_protocol(WIFI_IF_AP,  proto);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    esp_wifi_set_bandwidth(WIFI_IF_AP,  WIFI_BW_HT20);
    if (esp_wifi_set_max_tx_power(CANARY_WIFI_TX_QDBM) != ESP_OK) {
      log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "WiFi TX power set failed", nullptr);
    }
  }

  IPAddress ip = WiFi.softAPIP();
  snprintf(m_status.ap_ip, sizeof(m_status.ap_ip), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

  char msg[64];
  snprintf(msg, sizeof(msg), "AP: %s", ap_ssid);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, msg, m_status.ap_ip);

  // Bring mDNS up immediately so AP-only clients can already reach
  // `canary.local`. The STA_GOT_IP handler above re-runs this after
  // home-WiFi connects so the home-network interface is announced too.
  start_mdns(s_mdns_device_id);

  // Attempt to connect to home WiFi if configured
  if (has_creds && m_creds.enabled) {
    connectToHome();
  } else {
    m_status.state = WIFI_PROV_AP_ONLY;
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "AP-only mode", "No home WiFi configured");
  }

  return true;
}

bool ScvNetworkManager::loadCredentials() {
  memset(&m_creds, 0, sizeof(m_creds));

  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly()) return false;

  size_t ssid_len = nvs.getBytesLength(NVS_KEY_WIFI_SSID);
  if (ssid_len > 0 && ssid_len <= 32) {
    nvs.getBytes(NVS_KEY_WIFI_SSID, m_creds.ssid, ssid_len);
    m_creds.ssid[ssid_len] = '\0';

    size_t pass_len = nvs.getBytesLength(NVS_KEY_WIFI_PASS);
    if (pass_len > 0 && pass_len <= 64) {
      nvs.getBytes(NVS_KEY_WIFI_PASS, m_creds.password, pass_len);
      m_creds.password[pass_len] = '\0';
    }

    m_creds.enabled = nvs.getBool(NVS_KEY_WIFI_EN, true);
    m_creds.configured = (strlen(m_creds.ssid) > 0);
  } else if (ssid_len == 0 && nvs.isKey(NVS_KEY_WIFI_SSID)) {
    // String-typed seed: isKey() is type-blind and getBytesLength() is 0 for
    // string entries, so a present key with no blob bytes is the other
    // encoding, not absence (LESSONS_LEARNED "A seeded credential key is
    // honored whichever NVS TYPE wrote it"). Same caps as the blob path;
    // an empty string ssid stays unconfigured.
    if (nvs.getString(NVS_KEY_WIFI_SSID, m_creds.ssid, sizeof(m_creds.ssid)) > 0) {
      nvs.getString(NVS_KEY_WIFI_PASS, m_creds.password, sizeof(m_creds.password));
      m_creds.enabled = nvs.getBool(NVS_KEY_WIFI_EN, true);
      m_creds.configured = true;
    }
  }

  nvs.end();
  return m_creds.configured;
}

bool ScvNetworkManager::saveCredentials() {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;

  nvs.putBytes(NVS_KEY_WIFI_SSID, m_creds.ssid, strlen(m_creds.ssid));
  nvs.putBytes(NVS_KEY_WIFI_PASS, m_creds.password, strlen(m_creds.password));
  nvs.putBool(NVS_KEY_WIFI_EN, m_creds.enabled);

  nvs.end();
  m_creds.configured = true;

  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "WiFi credentials saved", m_creds.ssid);
  return true;
}

bool ScvNetworkManager::clearCredentials() {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;

  nvs.remove(NVS_KEY_WIFI_SSID);
  nvs.remove(NVS_KEY_WIFI_PASS);
  nvs.remove(NVS_KEY_WIFI_EN);

  nvs.end();

  memset(&m_creds, 0, sizeof(m_creds));
  m_status.state = WIFI_PROV_AP_ONLY;

  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "WiFi credentials cleared", nullptr);
  return true;
}

void ScvNetworkManager::connectToHome() {
  if (!m_creds.configured || !m_creds.enabled) {
    m_status.state = WIFI_PROV_AP_ONLY;
    return;
  }

  if (strlen(m_creds.ssid) == 0) {
    m_status.state = WIFI_PROV_AP_ONLY;
    return;
  }

  m_status.state = WIFI_PROV_CONNECTING;
  m_status.connect_attempts++;
  m_status.last_connect_ms = millis();

  char msg[64];
  snprintf(msg, sizeof(msg), "Connecting to: %s", m_creds.ssid);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, msg, nullptr);

  WiFi.begin(m_creds.ssid, m_creds.password);
  m_status.sta_pmf = ensure_sta_pmf_capable();  // F16: PMF capable, not required
}

void ScvNetworkManager::updateStatus() {
  m_status.ap_active = (WiFi.getMode() & WIFI_AP) != 0;
  m_status.sta_connected = WiFi.isConnected();
  m_status.ap_clients = WiFi.softAPgetStationNum();

  if (m_status.sta_connected) {
    m_status.rssi = WiFi.RSSI();
    IPAddress ip = WiFi.localIP();
    snprintf(m_status.sta_ip, sizeof(m_status.sta_ip), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  } else {
    m_status.rssi = 0;
    m_status.sta_ip[0] = '\0';
  }

  IPAddress apip = WiFi.softAPIP();
  snprintf(m_status.ap_ip, sizeof(m_status.ap_ip), "%d.%d.%d.%d", apip[0], apip[1], apip[2], apip[3]);
}

size_t ScvNetworkManager::getPeerCount() const {
  size_t n = 0;
  for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
    if (m_peers[i].valid) n++;
  }
  return n;
}

// Insert or refresh a peer in the cache. Slot is found by device_id match,
// then by mdns_hostname, then by the first invalid slot, then by oldest
// last_seen — guarantees the most recently seen PEER_CACHE_MAX peers stay.
static void peer_upsert(PeerEntry* peers,
                        const char* device_id,
                        const char* name,
                        const char* mdns_hostname,
                        const char* ip) {
  if (!device_id || !device_id[0]) return;

  int slot = -1;
  for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
    if (peers[i].valid && strncmp(peers[i].device_id, device_id,
                                  sizeof(peers[i].device_id)) == 0) {
      slot = (int)i; break;
    }
  }
  if (slot < 0 && mdns_hostname) {
    for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
      if (peers[i].valid && strncmp(peers[i].mdns_hostname, mdns_hostname,
                                    sizeof(peers[i].mdns_hostname)) == 0) {
        slot = (int)i; break;
      }
    }
  }
  if (slot < 0) {
    for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
      if (!peers[i].valid) { slot = (int)i; break; }
    }
  }
  if (slot < 0) {
    uint32_t oldest = UINT32_MAX;
    for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
      if (peers[i].last_seen_ms < oldest) {
        oldest = peers[i].last_seen_ms; slot = (int)i;
      }
    }
  }
  if (slot < 0) return;

  PeerEntry& p = peers[slot];
  strncpy(p.device_id, device_id, sizeof(p.device_id) - 1);
  p.device_id[sizeof(p.device_id) - 1] = '\0';
  if (name) {
    strncpy(p.name, name, sizeof(p.name) - 1);
    p.name[sizeof(p.name) - 1] = '\0';
  } else if (!p.valid) {
    p.name[0] = '\0';
  }
  if (mdns_hostname) {
    strncpy(p.mdns_hostname, mdns_hostname, sizeof(p.mdns_hostname) - 1);
    p.mdns_hostname[sizeof(p.mdns_hostname) - 1] = '\0';
  }
  if (ip) {
    strncpy(p.ip, ip, sizeof(p.ip) - 1);
    p.ip[sizeof(p.ip) - 1] = '\0';
  }
  p.last_seen_ms = millis();
  p.valid = true;
}

void ScvNetworkManager::browsePeers() {
  // Only browse when on home WiFi; in AP-only mode there's no LAN to browse.
  if (!m_status.sta_connected) return;
  if (m_mdns_hostname[0] == '\0') return;

  uint32_t now = millis();
  if (m_peers_last_browse_ms != 0 &&
      (now - m_peers_last_browse_ms) < PEER_BROWSE_INTERVAL_MS) {
    // Even when not browsing, prune stale entries so the cache reflects
    // peers that have actually disappeared.
    for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
      if (m_peers[i].valid && (now - m_peers[i].last_seen_ms) > PEER_STALE_MS) {
        m_peers[i].valid = false;
      }
    }
    return;
  }
  m_peers_last_browse_ms = now;

  // queryService blocks for up to ~1s waiting for responses. That's fine
  // here because we only call it on the 30s cadence above.
  int n = MDNS.queryService("securacv", "tcp");
  for (int i = 0; i < n; i++) {
    String host = MDNS.hostname(i);
    if (host.length() == 0) continue;

    // TXT records are advertised by addServiceTxt() in begin(); read them
    // back. ESPmDNS returns empty string when a key is absent.
    String tx_id   = MDNS.txt(i, "device_id");
    String tx_name = MDNS.txt(i, "name");

    // Filter ourselves out by comparing TXT device_id, not hostname:
    // every device shares "canary" as its mDNS hostname now (see
    // begin()), so hostname comparison would silently drop the real
    // peer and/or include this device in its own peer list.
    if (m_mdns_device_id[0] != '\0' &&
        tx_id.length() > 0 &&
        tx_id.equalsIgnoreCase(m_mdns_device_id)) continue;

    // ESPmDNS query-result accessor: Arduino-ESP32 core 3.x renamed
    // MDNSResponder::IP(idx) to address(idx). This lib compiles on BOTH core
    // lines — official-platform dev/release on core 2.x (IP), and the pioarduino
    // [env:full] BLE build on core 3.x (address) — so pick by core version.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    IPAddress ip = MDNS.address(i);
#else
    IPAddress ip = MDNS.IP(i);
#endif
    char ip_str[16] = {0};
    snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

    char fqdn[40] = {0};
    snprintf(fqdn, sizeof(fqdn), "%s.local", host.c_str());

    const char* device_id = tx_id.length() > 0 ? tx_id.c_str() : host.c_str();
    const char* name      = tx_name.length() > 0 ? tx_name.c_str() : nullptr;

    peer_upsert(m_peers, device_id, name, fqdn, ip_str);
  }

  // Drop entries we didn't refresh and that have aged past the stale window.
  for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
    if (m_peers[i].valid && (now - m_peers[i].last_seen_ms) > PEER_STALE_MS) {
      m_peers[i].valid = false;
    }
  }
}

void ScvNetworkManager::dropAp() {
  if (!(WiFi.getMode() & WIFI_AP)) return;  // already STA-only
  WiFi.softAPdisconnect(true);   // stop the SoftAP and release its netif
  WiFi.mode(WIFI_STA);
  m_status.ap_active = false;
  // The HTTP server keeps serving on the STA interface and mDNS was already
  // re-announced on STA at STA_GOT_IP, so canary.local stays reachable.
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK,
             "AP dropped — STA up, BLE-stable mode", m_status.sta_ip);
}

void ScvNetworkManager::raiseAp() {
  if (WiFi.getMode() & WIFI_AP) return;  // already up
  WiFi.mode(WIFI_AP_STA);
  bool ap_ok = WiFi.softAP(m_ap_ssid, m_ap_password, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  if (!ap_ok) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_NETWORK, "AP re-raise failed", nullptr);
    WiFi.mode(WIFI_STA);  // don't leave the radio half-configured in AP_STA with no AP up
    return;
  }
  apply_ap_security(m_status);  // F16: same request as begin()
  m_status.ap_active = true;
  witness_get_health().wifi_active = true;
  IPAddress ip = WiFi.softAPIP();
  snprintf(m_status.ap_ip, sizeof(m_status.ap_ip), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  start_mdns(s_mdns_device_id);  // re-announce on the re-raised AP interface
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "AP re-raised — STA link down", m_status.ap_ip);
}

void ScvNetworkManager::checkConnection() {
  uint32_t now = millis();
  updateStatus();

  switch (m_status.state) {
    case WIFI_PROV_CONNECTING:
      if (WiFi.isConnected()) {
        m_status.state = WIFI_PROV_CONNECTED;
        m_status.connected_since_ms = now;

        char msg[80];
        snprintf(msg, sizeof(msg), "Connected to %s", m_creds.ssid);
        log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, msg, m_status.sta_ip);
      } else if (now - m_status.last_connect_ms > WIFI_CONNECT_TIMEOUT_MS) {
        m_status.state = WIFI_PROV_FAILED;
        log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "WiFi connection timeout", m_creds.ssid);
      }
      break;

    case WIFI_PROV_CONNECTED:
      if (!WiFi.isConnected()) {
        m_status.state = WIFI_PROV_FAILED;
        log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "WiFi connection lost", nullptr);
        // F4: STA link gone — re-raise the management AP so the device stays
        // reachable for reconfiguration while it retries the home network.
        raiseAp();
      } else {
        /* Reset attempt counter on sustained connection so the next
         * disconnect starts backoff from scratch. */
        if (m_status.connect_attempts > 0) {
          m_status.connect_attempts = 0;
        }
        // F4: STA has held the home link past the grace window — drop the AP so
        // the radio runs the stable STA+BLE coexistence combo (vs the unstable
        // AP+STA+BLE) and no lingering management AP stays exposed. But never
        // out from under a still-associated client: the captive sheet keeps
        // the phone on the AP while the wizard's success screen offers the
        // optional hub step, and dropping then reroutes the sheet's relative
        // URLs to the home network (the probe hostname stops resolving to us),
        // so the hub save silently dies. Hold until the sheet closes (client
        // count hits 0), capped at AP_CLIENT_HOLD_MAX_MS.
        if ((WiFi.getMode() & WIFI_AP) &&
            now - m_status.connected_since_ms > AP_DROP_GRACE_MS &&
            (m_status.ap_clients == 0 ||  // fresh: updateStatus() ran above
             now - m_status.connected_since_ms > AP_CLIENT_HOLD_MAX_MS)) {
          dropAp();
        }
      }
      break;

    case WIFI_PROV_FAILED:
      /* Exponential backoff: 2 s → 4 s → 8 s → 16 s → 30 s cap.
       * m_status.connect_attempts counts consecutive failures since the
       * last successful connection. connectToHome() increments it; a
       * successful WIFI_PROV_CONNECTED transition above resets it to 0. */
      {
        uint32_t attempt = m_status.connect_attempts;
        if (attempt > 5) attempt = 5;
        /* Base 2 s, doubles each attempt, capped at 30 s. */
        uint32_t backoff_ms = 2000UL << (attempt > 0 ? (attempt - 1) : 0);
        if (backoff_ms > 30000UL) backoff_ms = 30000UL;

        if (m_creds.configured && m_creds.enabled &&
            now - m_status.last_connect_ms > backoff_ms) {
          connectToHome();
        }
      }
      break;

    case WIFI_PROV_AP_ONLY:
      // No home WiFi configured (or just cleared via /wifi/disconnect). Ensure
      // the management AP is up: F4's dropAp() may have torn it down while we
      // were CONNECTED, and clearCredentials() transitions straight here —
      // never through the CONNECTED-loss branch that would otherwise re-raise
      // it — which would leave the device with neither STA nor AP.
      raiseAp();  // idempotent: no-op when the AP is already up
      break;

    default:
      break;
  }
}

// ════════════════════════════════════════════════════════════════════════════
// WIFI POWER MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

#include <esp_wifi.h>

void network_set_wifi_power_save(bool enable) {
  wifi_ps_type_t ps = enable ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE;
  esp_err_t err = esp_wifi_set_ps(ps);
  if (err == ESP_OK) {
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK,
               enable ? "WiFi modem sleep enabled" : "WiFi modem sleep disabled",
               nullptr);
  } else {
    log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK,
               "WiFi power save set failed", nullptr);
  }
}

void network_set_tx_power(int8_t quarter_dbm) {
  /* esp_wifi_set_max_tx_power() takes quarter-dBm units (int8_t).
   * Valid range for ESP32-S3: 8 (2 dBm) .. 84 (21 dBm). */
  if (quarter_dbm < 8)  quarter_dbm = 8;
  if (quarter_dbm > 84) quarter_dbm = 84;
  esp_err_t err = esp_wifi_set_max_tx_power(quarter_dbm);
  if (err == ESP_OK) {
    char msg[40];
    snprintf(msg, sizeof(msg), "WiFi TX power set to %d (x0.25 dBm)", quarter_dbm);
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, msg, nullptr);
  } else {
    log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK,
               "WiFi TX power set failed", nullptr);
  }
}

bool network_is_sta_connected(void) {
  return WiFi.isConnected();
}

// ════════════════════════════════════════════════════════════════════════════
// RATE LIMITING
// ════════════════════════════════════════════════════════════════════════════

static RateLimitState s_rate_limit = {0, 0, 0};

bool rate_limit_check(httpd_req_t* req, bool is_action) {
  uint32_t now = millis();

  // Reset window if expired
  if (now - s_rate_limit.window_start_ms >= RATE_LIMIT_WINDOW_MS) {
    s_rate_limit.window_start_ms = now;
    s_rate_limit.request_count = 0;
    s_rate_limit.action_count = 0;
  }

  s_rate_limit.request_count++;
  if (is_action) {
    s_rate_limit.action_count++;
  }

  bool limited = (s_rate_limit.request_count > RATE_LIMIT_MAX_REQUESTS) ||
                 (is_action && s_rate_limit.action_count > RATE_LIMIT_MAX_ACTIONS);

  if (limited) {
    uint32_t remaining_ms = RATE_LIMIT_WINDOW_MS - (now - s_rate_limit.window_start_ms);
    uint32_t retry_after = (remaining_ms / 1000) + 1;

    char retry_str[8];
    snprintf(retry_str, sizeof(retry_str), "%lu", (unsigned long)retry_after);

    httpd_resp_set_status(req, "429 Too Many Requests");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Retry-After", retry_str);
    httpd_resp_sendstr(req, "{\"error\":\"rate_limited\"}");

    witness_get_health().http_errors++;
    return false;
  }

  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// AUTH GATE
// ════════════════════════════════════════════════════════════════════════════
//
// Wraps auth_check() against the device-provisioned API bearer token.
// On failure the AuthManager has already written the 401/403/429 response,
// so the handler can return ESP_OK directly after this returns false.
// Tracks http_errors so the status endpoint surfaces rejected calls.

static bool auth_gate(httpd_req_t* req) {
  const char* token = auth_get_token();
  if (!token || token[0] == '\0') {
    // Fail closed: if the bearer credential isn't provisioned, refuse.
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"not_provisioned\"}");
    witness_get_health().http_errors++;
    return false;
  }
  if (!auth_check(req, token)) {
    witness_get_health().http_errors++;
    return false;
  }
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// PROVISIONING GATE (F20 gap #11) — hooks + interface scoping
// ════════════════════════════════════════════════════════════════════════════
//
// main.cpp owns the gate State (it owns the BOOT button) and registers these
// two hooks at boot. Unregistered == closed, so the receipt and the page
// token fail closed on a build that never wires the button.

static network_gate_fn_t s_gate_take    = nullptr;
static network_gate_fn_t s_gate_is_open = nullptr;

void network_set_provisioning_gate_hooks(network_gate_fn_t take,
                                         network_gate_fn_t is_open) {
  s_gate_take    = take;
  s_gate_is_open = is_open;
}

// Every grant TAKES the gate (one tap = one consumer). The is_open hook is
// only a wiring check for /api/status; nothing here grants on a peek.
static bool provisioning_gate_take()    { return s_gate_take    ? s_gate_take()    : false; }

// Host-order a.b.c.d from an IPAddress (WiFi.softAPIP() and friends).
static uint32_t ip4_host_order(const IPAddress& ip) {
  return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
         ((uint32_t)ip[2] << 8)  |  (uint32_t)ip[3];
}

// Host-order value of an lwIP/esp_netif IPv4 word. The word is network byte
// order in memory, so the first byte is the first octet — no ntohl macro
// dependency, and the same on every core.
static uint32_t be_word_host_order(const void* word) {
  const uint8_t* b = (const uint8_t*)word;
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
         ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

// Host-order IPv4 of a sockaddr, or 0 when it carries none. esp_http_server's
// listener is a dual-stack AF_INET6 socket on every core with
// CONFIG_LWIP_IPV6 (both of ours), so an IPv4 client arrives as AF_INET6
// ::ffff:a.b.c.d — provisioning_gate::ipv4_host_order_from_addr (host-tested)
// unwraps that and answers 0 for a real IPv6 or link-local address. Reading
// only AF_INET here made every request look like "address unknown".
static uint32_t sockaddr_ip4_host_order(const struct sockaddr_storage& ss) {
  using canary::net::provisioning_gate::AddrFamily;
  using canary::net::provisioning_gate::ipv4_host_order_from_addr;
  if (ss.ss_family == AF_INET) {
    const struct sockaddr_in* sin = (const struct sockaddr_in*)&ss;
    return ipv4_host_order_from_addr(AddrFamily::IPV4,
                                     (const uint8_t*)&sin->sin_addr.s_addr);
  }
#if defined(LWIP_IPV6) && LWIP_IPV6
  if (ss.ss_family == AF_INET6) {
    const struct sockaddr_in6* sin6 = (const struct sockaddr_in6*)&ss;
    return ipv4_host_order_from_addr(AddrFamily::IPV6,
                                     (const uint8_t*)&sin6->sin6_addr);
  }
#endif
  return 0;
}

// Address + netmask of one default netif ("WIFI_AP_DEF" / "WIFI_STA_DEF"),
// host order; both 0 when the netif is absent or has no address. esp_netif
// reads the same on the IDF 4.4 (Arduino 2.0.17) and IDF 5.x cores — the
// Arduino WiFi class's subnet-mask accessor does not exist on the older one.
static void netif_ip4(const char* ifkey, uint32_t* ip, uint32_t* mask) {
  *ip = 0;
  *mask = 0;
  esp_netif_t* nif = esp_netif_get_handle_from_ifkey(ifkey);
  if (!nif) return;
  esp_netif_ip_info_t info;
  memset(&info, 0, sizeof(info));
  if (esp_netif_get_ip_info(nif, &info) != ESP_OK) return;
  *ip = be_word_host_order(&info.ip.addr);
  *mask = be_word_host_order(&info.netmask.addr);
}

// True only when this request provably came over the Canary's own Wi-Fi:
// the AP is up, the socket's LOCAL address is the AP address, the peer is
// inside the AP subnet, and the home network (if joined) does not overlap
// it — provisioning_gate::request_on_softap, host-tested. Anything else,
// IPv6 and link-local included, is "not AP", i.e. the home LAN, where the
// page token is withheld: a wrong match here would re-open the disclosure.
// The AP is dropped once the STA settles (dropAp), so this grant only exists
// while the AP is up; the SPA banner says so.
static bool from_ap_subnet(httpd_req_t* req) {
  if (!(WiFi.getMode() & WIFI_AP)) return false;
  const int fd = httpd_req_to_sockfd(req);
  if (fd < 0) return false;
  struct sockaddr_storage peer_ss;
  struct sockaddr_storage local_ss;
  socklen_t plen = sizeof(peer_ss);
  socklen_t llen = sizeof(local_ss);
  memset(&peer_ss, 0, sizeof(peer_ss));
  memset(&local_ss, 0, sizeof(local_ss));
  if (getpeername(fd, (struct sockaddr*)&peer_ss, &plen) != 0) return false;
  if (getsockname(fd, (struct sockaddr*)&local_ss, &llen) != 0) return false;
  uint32_t ap_ip, ap_mask, sta_ip, sta_mask;
  netif_ip4("WIFI_AP_DEF", &ap_ip, &ap_mask);
  netif_ip4("WIFI_STA_DEF", &sta_ip, &sta_mask);
  return canary::net::provisioning_gate::request_on_softap(
      sockaddr_ip4_host_order(peer_ss), sockaddr_ip4_host_order(local_ss),
      ap_ip, ap_mask, sta_ip, sta_mask);
}

// A valid bearer on this request. Fails closed when the device credential
// is not provisioned: AuthManager::checkOptional against an EMPTY expected
// token would accept "Authorization: Bearer " (a zero-length constant-time
// compare is equal), and here that would hand out the receipt.
static bool bearer_present_and_valid(httpd_req_t* req) {
  const char* token = auth_get_token();
  if (!token || token[0] == '\0') return false;
  return auth_check_optional(req, token);
}

// Dotted IPv4 of the interface this request arrived on (getsockname), so the
// receipt's base_url points at the address the client can actually reach —
// the STA address for a LAN caller, the AP address for an AP caller. Falls
// back to the SoftAP address when the socket cannot say.
static void local_addr_of(httpd_req_t* req, char* out, size_t cap) {
  uint32_t local = 0;
  const int fd = httpd_req_to_sockfd(req);
  if (fd >= 0) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    if (getsockname(fd, (struct sockaddr*)&ss, &len) == 0) {
      local = sockaddr_ip4_host_order(ss);
    }
  }
  if (local == 0) local = ip4_host_order(WiFi.softAPIP());
  snprintf(out, cap, "%u.%u.%u.%u",
           (unsigned)((local >> 24) & 0xFF), (unsigned)((local >> 16) & 0xFF),
           (unsigned)((local >> 8) & 0xFF),  (unsigned)(local & 0xFF));
}

// ════════════════════════════════════════════════════════════════════════════
// TLS CERTIFICATE (F15) — self-signed ECDSA P-256, generated once, kept in NVS
// ════════════════════════════════════════════════════════════════════════════
//
// The WAP's layout and naming (canary_wap.ino tls_*: CN=securacv-<fp>,
// O=SecuraCV, OU=Canary; validity 2020-01-01..2050-01-01; serial 1; DER in
// NVS keys tls_cert / tls_key) with one deliberate difference, option (b):
// an ECDSA P-256 key instead of RSA-2048 — expected to be far quicker than
// RSA-2048's 30-60 s (untimed: the "[TLS] Certificate generation took" line
// is what Track D D1 records), ~0.5 KB of DER instead of ~2 KB, a smaller
// handshake. Clients
// never see the difference: the iPhone app pins the SHA-256 of the
// certificate DER (tls_cert_fp), whatever the key type.
//
// Every step logs why it stopped, and the reason lands in m_tls_reason so
// /api/status tls_mode_reason says it. Serial lines never print key bytes.

#if SECURACV_HAS_HTTPS_SERVER
static bool tls_load_der_from_nvs(uint8_t** cert, size_t* cert_len,
                                  uint8_t** key, size_t* key_len) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly()) return false;
  const size_t clen = nvs.getBytesLength(NVS_KEY_TLS_CERT);
  const size_t klen = nvs.getBytesLength(NVS_KEY_TLS_KEY);
  if (clen == 0 || klen == 0) {
    nvs.end();
    return false;
  }
  uint8_t* c = (uint8_t*)malloc(clen);
  uint8_t* k = (uint8_t*)malloc(klen);
  bool ok = c && k &&
            nvs.getBytes(NVS_KEY_TLS_CERT, c, clen) == clen &&
            nvs.getBytes(NVS_KEY_TLS_KEY, k, klen) == klen;
  nvs.end();
  if (!ok) {
    if (k) { memset(k, 0, klen); free(k); }
    free(c);
    return false;
  }
  *cert = c; *cert_len = clen;
  *key = k;  *key_len = klen;
  return true;
}

static bool tls_store_der_to_nvs(const uint8_t* cert, size_t cert_len,
                                 const uint8_t* key, size_t key_len) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;
  const bool ok = nvs.putBytes(NVS_KEY_TLS_CERT, cert, cert_len) == cert_len &&
                  nvs.putBytes(NVS_KEY_TLS_KEY, key, key_len) == key_len;
  nvs.end();
  return ok;
}
#endif  // SECURACV_HAS_HTTPS_SERVER

#if SECURACV_HAS_TLS_CERTGEN
// Generates the pair into freshly malloc'd DER buffers. mbedTLS writes DER at
// the END of the scratch buffer and returns its length, so the copy starts at
// buf + sizeof(buf) - len (WAP parity).
static bool tls_generate_self_signed(const char* device_fp_hex,
                                     uint8_t** cert, size_t* cert_len,
                                     uint8_t** key, size_t* key_len,
                                     const char** why) {
  Serial.println("[TLS] Generating self-signed certificate (ECDSA P-256, first TLS boot only)...");
  int ret = -1;
  bool ok = false;
  uint8_t* c = nullptr;
  uint8_t* k = nullptr;
  mbedtls_pk_context pk;
  mbedtls_x509write_cert crt;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_mpi serial;
  mbedtls_pk_init(&pk);
  mbedtls_x509write_crt_init(&crt);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_mpi_init(&serial);

  static const char kPers[] = "securacv_tls_gen";
  char subject[96];
  static uint8_t scratch[1024];  // certificate DER; P-256 needs ~450 bytes

  *why = "certificate generation failed (DRBG seed)";
  ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char*)kPers, sizeof(kPers) - 1);
  if (ret != 0) goto done;

  *why = "certificate generation failed (P-256 keygen)";
  ret = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (ret != 0) goto done;
  ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk),
                            mbedtls_ctr_drbg_random, &drbg);
  if (ret != 0) goto done;

  *why = "certificate generation failed (subject/issuer)";
  snprintf(subject, sizeof(subject), "CN=securacv-%s,O=SecuraCV,OU=Canary",
           device_fp_hex);
  mbedtls_x509write_crt_set_subject_key(&crt, &pk);
  mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  ret = mbedtls_x509write_crt_set_subject_name(&crt, subject);
  if (ret != 0) goto done;
  ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject);
  if (ret != 0) goto done;
  // Serial 1 (WAP parity). mbedTLS 3.4+ deprecates the MPI setter in favor
  // of the raw-bytes one; IDF 5.5 ships 3.6, IDF 4.4 ships 2.28.
#if defined(MBEDTLS_VERSION_NUMBER) && MBEDTLS_VERSION_NUMBER >= 0x03040000
  {
    unsigned char serial_one[1] = {0x01};
    ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial_one, sizeof(serial_one));
  }
#else
  mbedtls_mpi_lset(&serial, 1);
  ret = mbedtls_x509write_crt_set_serial(&crt, &serial);
#endif
  if (ret != 0) goto done;
  ret = mbedtls_x509write_crt_set_validity(&crt, "20200101000000", "20500101000000");
  if (ret != 0) goto done;

  *why = "certificate generation failed (certificate DER)";
  ret = mbedtls_x509write_crt_der(&crt, scratch, sizeof(scratch),
                                  mbedtls_ctr_drbg_random, &drbg);
  if (ret <= 0) goto done;
  c = (uint8_t*)malloc((size_t)ret);
  if (!c) { ret = -1; goto done; }
  memcpy(c, scratch + sizeof(scratch) - ret, (size_t)ret);
  *cert_len = (size_t)ret;

  *why = "certificate generation failed (key DER)";
  ret = mbedtls_pk_write_key_der(&pk, scratch, sizeof(scratch));
  if (ret <= 0) goto done;
  k = (uint8_t*)malloc((size_t)ret);
  if (!k) { ret = -1; goto done; }
  memcpy(k, scratch + sizeof(scratch) - ret, (size_t)ret);
  *key_len = (size_t)ret;

  ok = true;
  *cert = c; c = nullptr;
  *key = k;  k = nullptr;
  *why = "certificate generated";

done:
  if (!ok && ret != 0) {
    Serial.printf("[TLS] %s: -0x%04X\n", *why, (unsigned)(-ret));
  }
  memset(scratch, 0, sizeof(scratch));  // the key DER passed through here
  free(c);
  if (k) { memset(k, 0, *key_len); free(k); }
  mbedtls_mpi_free(&serial);
  mbedtls_x509write_crt_free(&crt);
  mbedtls_pk_free(&pk);
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  return ok;
}
#endif  // SECURACV_HAS_TLS_CERTGEN

bool ScvNetworkManager::initTls() {
#if !FEATURE_HTTPS
  m_tls_reason = "FEATURE_HTTPS=0 in this build";
  return false;
#elif !SECURACV_HAS_HTTPS_SERVER
  m_tls_reason = "this core has no esp_https_server";
  Serial.println("[TLS] esp_https_server not in this core — HTTP only");
  log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "TLS unavailable", m_tls_reason);
  return false;
#else
  if (m_tls_cert_der && m_tls_key_der) return true;  // idempotent

  uint8_t* cert = nullptr;
  uint8_t* key = nullptr;
  size_t cert_len = 0, key_len = 0;
  if (tls_load_der_from_nvs(&cert, &cert_len, &key, &key_len)) {
    Serial.println("[TLS] Loaded certificate from NVS");
  } else {
#if SECURACV_HAS_TLS_CERTGEN
    char device_fp[17];
    hex_to_str(device_fp, witness_get_device().pubkey_fp, 8);
    const char* why = nullptr;
    const uint32_t gen_started_ms = millis();
    const bool generated =
        tls_generate_self_signed(device_fp, &cert, &cert_len, &key, &key_len, &why);
    // The keygen time is not measured anywhere else: this line is what the
    // bench (Track D D1) records, so the doc figure can become a number.
    Serial.printf("[TLS] Certificate generation took %lu ms\n",
                  (unsigned long)(millis() - gen_started_ms));
    if (!generated) {
      m_tls_reason = why;
      Serial.println("[TLS] Certificate generation FAILED — HTTP only");
      log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "TLS unavailable", why);
      return false;
    }
    if (!tls_store_der_to_nvs(cert, cert_len, key, key_len)) {
      // Serve with it this boot anyway; the next boot generates a new one,
      // which changes the pin — logged so a re-pair is not a mystery.
      Serial.println("[TLS] WARNING: certificate not stored; it changes next boot");
      log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "TLS cert not persisted", "NVS write failed");
    }
    Serial.printf("[TLS] CN: securacv-%s\n", device_fp);
#else
    m_tls_reason = "no stored certificate and this core cannot generate one (mbedTLS x509write/ECDSA)";
    Serial.println("[TLS] No certificate in NVS and no on-device generation — HTTP only");
    log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "TLS unavailable", "no certificate generation");
    return false;
#endif
  }

  m_tls_cert_der = cert;
  m_tls_cert_der_len = cert_len;
  m_tls_key_der = key;
  m_tls_key_der_len = key_len;
  uint8_t digest[32];
  sha256_raw(m_tls_cert_der, m_tls_cert_der_len, digest);
  canary::net::tls_policy::fingerprint_hex(digest, m_tls_cert_fp_hex);
  m_tls_reason = "certificate ready";
  Serial.printf("[TLS] Cert fingerprint: %.16s...\n", m_tls_cert_fp_hex);
  return true;
#endif
}

// Forward declarations for HTTP handlers
static esp_err_t handle_ui(httpd_req_t* req);
// BOOT-tap gated provisioning receipt (F20 gap #11; WAP parity).
static esp_err_t handle_provisioning_receipt(httpd_req_t* req);
// Captive-portal probes + first-boot setup wizard (see the CAPTIVE-PORTAL
// section below for the per-platform strategy).
static esp_err_t handle_captive_probe(httpd_req_t* req);
// FEATURE_HTTPS port-80 server: 307 to https:// for everything that is not
// a connectivity probe (F15).
static esp_err_t handle_https_redirect(httpd_req_t* req);
static esp_err_t handle_setup_page(httpd_req_t* req);
static esp_err_t handle_captive_catchall(httpd_req_t* req);
static esp_err_t handle_status(httpd_req_t* req);
static esp_err_t handle_chain(httpd_req_t* req);
static esp_err_t handle_witness(httpd_req_t* req);
static esp_err_t handle_logs(httpd_req_t* req);
static esp_err_t handle_log_ack(httpd_req_t* req);
static esp_err_t handle_ack_all(httpd_req_t* req);
static esp_err_t handle_reboot(httpd_req_t* req);
static esp_err_t handle_export(httpd_req_t* req);

// WiFi API endpoints (for MQTT STA configuration)
static esp_err_t handle_wifi_status(httpd_req_t* req);
static esp_err_t handle_wifi_scan(httpd_req_t* req);
static esp_err_t handle_wifi_connect(httpd_req_t* req);
static esp_err_t handle_wifi_disconnect(httpd_req_t* req);

// Peer discovery
static esp_err_t handle_peers(httpd_req_t* req);

#if FEATURE_HA_MQTT
static esp_err_t handle_mqtt_status(httpd_req_t* req);
static esp_err_t handle_mqtt_config(httpd_req_t* req);
static esp_err_t handle_mqtt_ca(httpd_req_t* req);  // POST stores the broker CA (PEM), DELETE forgets it
#endif

#if FEATURE_OTA_UPDATE && !defined(SECURACV_BUILD_RELEASE)
// Dev-only raw push endpoint (no signature). Compiled out of release
// builds — production updates go through the signed pull-OTA flow below.
static esp_err_t handle_ota(httpd_req_t* req);
#endif

#if FEATURE_OTA_PULL
static esp_err_t handle_ota_status(httpd_req_t* req);
static esp_err_t handle_ota_check(httpd_req_t* req);
static esp_err_t handle_ota_install(httpd_req_t* req);
static esp_err_t handle_ota_config(httpd_req_t* req);
#endif

#if FEATURE_CAMERA_PEEK
static esp_err_t handle_peek_start(httpd_req_t* req);
static esp_err_t handle_peek_stream(httpd_req_t* req);
static esp_err_t handle_peek_stop(httpd_req_t* req);
static esp_err_t handle_peek_status(httpd_req_t* req);
static esp_err_t handle_peek_init(httpd_req_t* req);
static esp_err_t handle_peek_resolution(httpd_req_t* req);
static esp_err_t handle_peek_sensor_get(httpd_req_t* req);
static esp_err_t handle_peek_sensor_set(httpd_req_t* req);
static esp_err_t handle_peek_snapshot(httpd_req_t* req);
#endif

#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
static esp_err_t handle_sensing(httpd_req_t* req);
#endif

#if FEATURE_VISION_DETECT
static esp_err_t handle_vision_config_get(httpd_req_t* req);
static esp_err_t handle_vision_config_set(httpd_req_t* req);
static esp_err_t handle_vision_config_save(httpd_req_t* req);
static esp_err_t handle_vision_thumbnail(httpd_req_t* req);
#endif

#if FEATURE_ACOUSTIC_EVENTS
// Microphone testability + privacy controls (see docs/getting_started_canary.md).
static esp_err_t handle_audio_level(httpd_req_t* req);
static esp_err_t handle_audio_mute(httpd_req_t* req);
static esp_err_t handle_audio_test_start(httpd_req_t* req);
static esp_err_t handle_audio_test_status(httpd_req_t* req);
#endif

#if FEATURE_DIAGNOSTICS
static esp_err_t handle_diagnostics(httpd_req_t* req);
static esp_err_t handle_selftest(httpd_req_t* req);
#endif

#if FEATURE_POWER_MONITOR
static esp_err_t handle_battery_history(httpd_req_t* req);
#endif

#if FEATURE_THERMAL_WATCHDOG
static esp_err_t handle_thermal(httpd_req_t* req);
#endif

// Household time zone (F28): GET/POST /api/settings.
static esp_err_t handle_settings_get(httpd_req_t* req);
static esp_err_t handle_settings_post(httpd_req_t* req);

#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
// BLE Scout paired beacons + the proximity pairing window (F27).
static esp_err_t handle_scout_list(httpd_req_t* req);
static esp_err_t handle_scout_pair_start(httpd_req_t* req);
static esp_err_t handle_scout_pair_status(httpd_req_t* req);
static esp_err_t handle_scout_pair_cancel(httpd_req_t* req);
static esp_err_t handle_scout_unpair(httpd_req_t* req);
#endif

#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
// Mesh / opera REST API (PR-8, F10, F10-rekey). Twelve registrations:
// status, peers, the four pairing steps, leave, name, enable, alerts GET +
// DELETE, and remove (which rotates opera_secret — spec §5.6 PIO; crypto
// review + bench pending, see spec/canary_mesh_network_v0.md §8.3).
static esp_err_t handle_mesh_status(httpd_req_t* req);
static esp_err_t handle_mesh_peers(httpd_req_t* req);
static esp_err_t handle_mesh_pair_start(httpd_req_t* req);
static esp_err_t handle_mesh_pair_join(httpd_req_t* req);
static esp_err_t handle_mesh_pair_confirm(httpd_req_t* req);
static esp_err_t handle_mesh_pair_cancel(httpd_req_t* req);
static esp_err_t handle_mesh_leave(httpd_req_t* req);
static esp_err_t handle_mesh_name(httpd_req_t* req);
static esp_err_t handle_mesh_enable(httpd_req_t* req);
static esp_err_t handle_mesh_alerts(httpd_req_t* req);
static esp_err_t handle_mesh_alerts_clear(httpd_req_t* req);
static esp_err_t handle_mesh_remove(httpd_req_t* req);
#endif

// esp_http_server drops a registration past max_uri_handlers and returns an
// error the old call sites ignored — a full table 404s whole route families
// in silence (the wildcard fallback, registered last, goes first). Every
// route goes through here so a full table is on the serial log, by name.
static void register_route(httpd_handle_t server, const httpd_uri_t* uri) {
  const esp_err_t err = httpd_register_uri_handler(server, uri);
  if (err != ESP_OK) {
    Serial.printf("[HTTP] route NOT registered: %s (method %d): %s - raise max_uri_handlers\n",
                  uri->uri, (int)uri->method, esp_err_to_name(err));
  }
}

// The six OS connectivity probes (tls_policy::is_connectivity_probe names the
// same list). Registered with a trailing '*' because probe URLs sometimes
// carry a cache-busting query and the wildcard matcher compares the FULL uri;
// handle_captive_probe re-checks the exact path component itself. File scope
// because both the main route table and the FEATURE_HTTPS port-80 server
// register them.
static const char* kProbePaths[] = {
  "/hotspot-detect.html*",        // Apple CNA
  "/library/test/success.html*",  // Apple (older probe)
  "/generate_204*",               // Android
  "/gen_204*",                    // Android (short variant)
  "/connecttest.txt*",            // Windows NCSI
  "/ncsi.txt*",                   // Windows NCSI (legacy)
};

// Worst case with every feature on: 14 unconditional + 4 MQTT (/api/mqtt/ca
// twice — POST and DELETE are separate registrations) + 1 dev-only POST
// /api/ota (FEATURE_OTA_UPDATE && !SECURACV_BUILD_RELEASE; a release build
// leaves that slot spare, which is cheaper than a dropped route) + 4
// OTA-pull + 9 peek + 1 sensing + 4 vision + 4 audio + 2 diagnostics + 1
// power + 1 thermal = 45 base, + 8 captive-portal routes (6 OS connectivity
// probes + /setup + the wildcard fallback) + 1 provisioning receipt
// (GET /api/provisioning-receipt, F20 gap #11) + 2 settings (GET/POST
// /api/settings — household time zone, F28), always + 5 BLE Scout pairing
// endpoints (F27) when FEATURE_BLE_SCAN is compiled in + 12 mesh
// registrations (PR-8's 6 + F10's leave/name/enable/alerts GET/alerts DELETE
// + F10-rekey's remove) when the mesh feature is compiled in. The BLE Scout five are in
// both numbers because the audit counts every #if branch (the worst case);
// a build without FEATURE_BLE_SCAN leaves them spare. Each registered httpd_uri_t needs
// a slot; register_route() names any that does not get one. The same table
// goes on whichever server is primary (TLS or plain), so one budget — and
// every registration in registerHttpHandlers uses its `server` parameter,
// never m_http_server (nullptr there on FEATURE_HTTPS builds).
// firmware/canary/scripts/check_route_security.py enforces both: it counts
// every #if branch against these two numbers and fails a member handle.
#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
static const uint16_t kRouteTableSlots = 73;
#else
static const uint16_t kRouteTableSlots = 61;
#endif

bool ScvNetworkManager::startHttpServer() {
  using canary::net::tls_policy::Mode;
  const bool setup_active = setup_is_active() || setup_is_first_boot();
  const char* why = nullptr;
  const Mode mode = canary::net::tls_policy::decide(
      FEATURE_HTTPS != 0, SECURACV_HAS_HTTPS_SERVER != 0,
      m_tls_cert_der != nullptr && m_tls_key_der != nullptr, setup_active, &why);
  m_tls_reason = why;
  m_tls_deferred_for_setup = canary::net::tls_policy::deferred_for_setup(
      FEATURE_HTTPS != 0, SECURACV_HAS_HTTPS_SERVER != 0, setup_active);

#if SECURACV_HAS_HTTPS_SERVER
  if (mode == Mode::HTTPS_REDIRECT) {
    httpd_ssl_config_t ssl = HTTPD_SSL_CONFIG_DEFAULT();
    // The server certificate field was renamed in IDF 5.0 (cacert_pem was
    // the server certificate on 4.4 and became the client-verify CA). DER is
    // accepted by both: mbedTLS parses PEM only when it finds the header.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    ssl.servercert     = m_tls_cert_der;
    ssl.servercert_len = m_tls_cert_der_len;
#else
    ssl.cacert_pem     = m_tls_cert_der;
    ssl.cacert_len     = m_tls_cert_der_len;
#endif
    ssl.prvtkey_pem = m_tls_key_der;
    ssl.prvtkey_len = m_tls_key_der_len;
    ssl.port_secure = HTTPS_PORT;
    ssl.httpd.uri_match_fn = httpd_uri_match_wildcard;
    ssl.httpd.stack_size = 10240;  // TLS handshake + the in-handler MJPEG loop
    ssl.httpd.max_uri_handlers = kRouteTableSlots;
    ssl.httpd.recv_wait_timeout = 30;
    ssl.httpd.send_wait_timeout = 30;
    ssl.httpd.lru_purge_enable = true;
    // Two httpd instances need two control ports; pin both explicitly
    // rather than trusting the two DEFAULT macros to differ.
    ssl.httpd.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;

    if (httpd_ssl_start(&m_https_server, &ssl) == ESP_OK) {
      m_tls_enabled = true;
      registerHttpHandlers(m_https_server);
      Serial.printf("[HTTPS] Server started on port %d\n", HTTPS_PORT);
      log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "HTTPS server started", "port 443");
      if (!startRedirectServer()) {
        // The API is up on 443; only the plain-HTTP conveniences (probes,
        // the redirect) are missing. Logged, not fatal.
        log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK,
                   "HTTPS redirect server start failed", "probes unanswered");
      }
      return true;
    }
    m_https_server = nullptr;
    m_tls_enabled = false;
    m_tls_reason = "httpd_ssl_start failed; serving HTTP";
    Serial.println("[HTTPS] Server start FAILED — falling back to HTTP");
    log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "HTTPS start failed, using HTTP", nullptr);
  }
#else
  (void)mode;  // HTTP-only build: the reason above is still reported
#endif  // SECURACV_HAS_HTTPS_SERVER

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.stack_size = 8192;
  config.max_uri_handlers = kRouteTableSlots;
  config.recv_wait_timeout = 30;
  config.send_wait_timeout = 30;
  config.lru_purge_enable  = true;

  if (httpd_start(&m_http_server, &config) != ESP_OK) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_NETWORK, "HTTP server start failed", nullptr);
    return false;
  }

  registerHttpHandlers(m_http_server);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "HTTP server started", "port 80");
  return true;
}

// FEATURE_HTTPS: the plain server beside the TLS one. It serves only what must
// stay plain — the six connectivity probes (no OS sends them over TLS; a
// redirect breaks detection and the phone drops the AP) — and redirects every
// other GET/POST to https:// (tls_policy::build_redirect_location). The canary
// serves no public /api/fleet, so unlike the WAP's there is nothing else here.
// 6 probes + 2 wildcard redirects, with headroom.
bool ScvNetworkManager::startRedirectServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = HTTP_REDIRECT_PORT;  // the port-80 redirect-to-https server
  config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 12;
  config.lru_purge_enable = true;
  if (httpd_start(&m_http_server, &config) != ESP_OK) {
    m_http_server = nullptr;
    return false;
  }
  for (const char* p : kProbePaths) {
    httpd_uri_t probe = { .uri = p, .method = HTTP_GET, .handler = handle_captive_probe };
    register_route(m_http_server, &probe);
  }
  // Registered after the probes: esp_http_server matches in registration order.
  httpd_uri_t redirect_get = { .uri = "/*", .method = HTTP_GET, .handler = handle_https_redirect };
  register_route(m_http_server, &redirect_get);
  httpd_uri_t redirect_post = { .uri = "/*", .method = HTTP_POST, .handler = handle_https_redirect };
  register_route(m_http_server, &redirect_post);
  Serial.println("[HTTP]  Port 80 redirects to HTTPS (connectivity probes answered in plain HTTP)");
  return true;
}

void ScvNetworkManager::stopHttpServer() {
#if SECURACV_HAS_HTTPS_SERVER
  if (m_https_server) {
    httpd_ssl_stop(m_https_server);
    m_https_server = nullptr;
  }
#endif
  m_tls_enabled = false;
  if (m_http_server) {
    httpd_stop(m_http_server);
    m_http_server = nullptr;
  }
}

void ScvNetworkManager::registerHttpHandlers(httpd_handle_t server) {
  // UI
  httpd_uri_t ui = { .uri = "/", .method = HTTP_GET, .handler = handle_ui };
  register_route(server, &ui);

  // First-boot setup wizard + the OS captive-portal connectivity probes.
  httpd_uri_t setup_page = { .uri = "/setup", .method = HTTP_GET, .handler = handle_setup_page };
  register_route(server, &setup_page);
  for (const char* p : kProbePaths) {
    httpd_uri_t probe = { .uri = p, .method = HTTP_GET, .handler = handle_captive_probe };
    register_route(server, &probe);
  }

  // Provisioning receipt: bearer OR one BOOT tap (the handler gates itself;
  // firmware/canary/scripts/check_route_security.py lists it as self-gating).
  httpd_uri_t receipt = { .uri = "/api/provisioning-receipt", .method = HTTP_GET, .handler = handle_provisioning_receipt };
  register_route(server, &receipt);

  // API endpoints
  httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = handle_status };
  register_route(server, &status);

  httpd_uri_t chain = { .uri = "/api/chain", .method = HTTP_GET, .handler = handle_chain };
  register_route(server, &chain);

  httpd_uri_t witness = { .uri = "/api/witness", .method = HTTP_GET, .handler = handle_witness };
  register_route(server, &witness);

  httpd_uri_t logs = { .uri = "/api/logs", .method = HTTP_GET, .handler = handle_logs };
  register_route(server, &logs);

  httpd_uri_t log_ack = { .uri = "/api/logs/*/ack", .method = HTTP_POST, .handler = handle_log_ack };
  register_route(server, &log_ack);

  httpd_uri_t ack_all = { .uri = "/api/logs/ack-all", .method = HTTP_POST, .handler = handle_ack_all };
  register_route(server, &ack_all);

  httpd_uri_t reboot = { .uri = "/api/reboot", .method = HTTP_POST, .handler = handle_reboot };
  register_route(server, &reboot);

  httpd_uri_t export_ep = { .uri = "/api/export", .method = HTTP_POST, .handler = handle_export };
  register_route(server, &export_ep);

  // WiFi management endpoints
  httpd_uri_t wifi_status = { .uri = "/api/wifi/status", .method = HTTP_GET, .handler = handle_wifi_status };
  register_route(server, &wifi_status);

  httpd_uri_t wifi_scan = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan };
  register_route(server, &wifi_scan);

  httpd_uri_t wifi_connect = { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = handle_wifi_connect };
  register_route(server, &wifi_connect);

  httpd_uri_t wifi_disconnect = { .uri = "/api/wifi/disconnect", .method = HTTP_POST, .handler = handle_wifi_disconnect };
  register_route(server, &wifi_disconnect);

  // Peer list (mDNS browse cache). Path matches canary-vision/docs/discovery.md
  // and the SPA's CanaryAPI.request(... '/api/v1/peers').
  httpd_uri_t peers_ep = { .uri = "/api/v1/peers", .method = HTTP_GET, .handler = handle_peers };
  register_route(server, &peers_ep);

  #if FEATURE_HA_MQTT
  httpd_uri_t mqtt_stat = { .uri = "/api/mqtt/status", .method = HTTP_GET, .handler = handle_mqtt_status };
  register_route(server, &mqtt_stat);

  httpd_uri_t mqtt_cfg = { .uri = "/api/mqtt/config", .method = HTTP_POST, .handler = handle_mqtt_config };
  register_route(server, &mqtt_cfg);

  // The broker CA (PEM, up to kCaPemMax) has its own route: the config body
  // is 512 bytes and a certificate is not. POST stores, DELETE forgets —
  // an explicit verb, so a body that arrives empty can never mean "clear".
  httpd_uri_t mqtt_ca_post = { .uri = "/api/mqtt/ca", .method = HTTP_POST, .handler = handle_mqtt_ca };
  register_route(server, &mqtt_ca_post);
  httpd_uri_t mqtt_ca_del = { .uri = "/api/mqtt/ca", .method = HTTP_DELETE, .handler = handle_mqtt_ca };
  register_route(server, &mqtt_ca_del);
  #endif

  #if FEATURE_OTA_UPDATE && !defined(SECURACV_BUILD_RELEASE)
  httpd_uri_t ota = { .uri = "/api/ota", .method = HTTP_POST, .handler = handle_ota };
  register_route(server, &ota);
  #endif

  #if FEATURE_OTA_PULL
  httpd_uri_t ota_status = { .uri = "/api/ota/status", .method = HTTP_GET, .handler = handle_ota_status };
  register_route(server, &ota_status);

  httpd_uri_t ota_check = { .uri = "/api/ota/check", .method = HTTP_POST, .handler = handle_ota_check };
  register_route(server, &ota_check);

  httpd_uri_t ota_install = { .uri = "/api/ota/install", .method = HTTP_POST, .handler = handle_ota_install };
  register_route(server, &ota_install);

  httpd_uri_t ota_cfg = { .uri = "/api/ota/config", .method = HTTP_POST, .handler = handle_ota_config };
  register_route(server, &ota_cfg);
  #endif

  #if FEATURE_CAMERA_PEEK
  httpd_uri_t peek_start = { .uri = "/api/peek/start", .method = HTTP_POST, .handler = handle_peek_start };
  register_route(server, &peek_start);

  httpd_uri_t peek_stream = { .uri = "/api/peek/stream", .method = HTTP_GET, .handler = handle_peek_stream };
  register_route(server, &peek_stream);

  httpd_uri_t peek_stop = { .uri = "/api/peek/stop", .method = HTTP_POST, .handler = handle_peek_stop };
  register_route(server, &peek_stop);

  httpd_uri_t peek_status = { .uri = "/api/peek/status", .method = HTTP_GET, .handler = handle_peek_status };
  register_route(server, &peek_status);

  httpd_uri_t peek_init = { .uri = "/api/peek/init", .method = HTTP_POST, .handler = handle_peek_init };
  register_route(server, &peek_init);

  httpd_uri_t peek_res = { .uri = "/api/peek/resolution", .method = HTTP_POST, .handler = handle_peek_resolution };
  register_route(server, &peek_res);

  httpd_uri_t peek_sensor_g = { .uri = "/api/peek/sensor", .method = HTTP_GET, .handler = handle_peek_sensor_get };
  register_route(server, &peek_sensor_g);

  httpd_uri_t peek_sensor_s = { .uri = "/api/peek/sensor", .method = HTTP_POST, .handler = handle_peek_sensor_set };
  register_route(server, &peek_sensor_s);

  httpd_uri_t peek_snap = { .uri = "/api/peek/snapshot", .method = HTTP_GET, .handler = handle_peek_snapshot };
  register_route(server, &peek_snap);
  #endif

  #if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
  httpd_uri_t sensing_ep = { .uri = "/api/sensing", .method = HTTP_GET, .handler = handle_sensing };
  register_route(server, &sensing_ep);
  #endif

  #if FEATURE_VISION_DETECT
  httpd_uri_t vision_cfg_g = { .uri = "/api/vision/config", .method = HTTP_GET, .handler = handle_vision_config_get };
  register_route(server, &vision_cfg_g);

  httpd_uri_t vision_cfg_s = { .uri = "/api/vision/config", .method = HTTP_POST, .handler = handle_vision_config_set };
  register_route(server, &vision_cfg_s);

  httpd_uri_t vision_cfg_save = { .uri = "/api/vision/config/save", .method = HTTP_POST, .handler = handle_vision_config_save };
  register_route(server, &vision_cfg_save);

  httpd_uri_t vision_thumb = { .uri = "/api/vision/thumbnail", .method = HTTP_GET, .handler = handle_vision_thumbnail };
  register_route(server, &vision_thumb);
  #endif

  #if FEATURE_ACOUSTIC_EVENTS
  // Live RMS for the UI level meter — same number the hysteresis uses,
  // not a second audio path. Returns 0 when muted.
  httpd_uri_t audio_level = { .uri = "/api/audio/level", .method = HTTP_GET, .handler = handle_audio_level };
  register_route(server, &audio_level);

  // Hard mute (physically uninstalls the I2S driver) — persisted in NVS.
  httpd_uri_t audio_mute_ep = { .uri = "/api/audio/mute", .method = HTTP_POST, .handler = handle_audio_mute };
  register_route(server, &audio_mute_ep);

  // Alarm-pattern self-test (relaxed thresholds, normal event callback
  // suppressed so a TEST-button press does NOT flow into HA automations).
  httpd_uri_t audio_test_start = { .uri = "/api/audio/test/start", .method = HTTP_POST, .handler = handle_audio_test_start };
  register_route(server, &audio_test_start);
  httpd_uri_t audio_test_status = { .uri = "/api/audio/test/status", .method = HTTP_GET, .handler = handle_audio_test_status };
  register_route(server, &audio_test_status);
  #endif

  #if FEATURE_DIAGNOSTICS
  httpd_uri_t diag_ep = { .uri = "/api/diagnostics", .method = HTTP_GET, .handler = handle_diagnostics };
  register_route(server, &diag_ep);

  httpd_uri_t selftest_ep = { .uri = "/api/selftest", .method = HTTP_GET, .handler = handle_selftest };
  register_route(server, &selftest_ep);
  #endif

  #if FEATURE_POWER_MONITOR
  httpd_uri_t batt_hist_ep = { .uri = "/api/battery/history", .method = HTTP_GET, .handler = handle_battery_history };
  register_route(server, &batt_hist_ep);
  #endif

  #if FEATURE_THERMAL_WATCHDOG
  httpd_uri_t thermal_ep = { .uri = "/api/thermal", .method = HTTP_GET, .handler = handle_thermal };
  register_route(server, &thermal_ep);
  #endif

  // Household time zone (F28). 2 endpoints — see the SETTINGS section below.
  httpd_uri_t settings_get_ep = { .uri = "/api/settings", .method = HTTP_GET, .handler = handle_settings_get };
  register_route(server, &settings_get_ep);
  httpd_uri_t settings_post_ep = { .uri = "/api/settings", .method = HTTP_POST, .handler = handle_settings_post };
  register_route(server, &settings_post_ep);

  #if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
  // BLE Scout pairing (F27). 5 endpoints — see the SCOUT section below.
  httpd_uri_t scout_list_ep = { .uri = "/api/scout", .method = HTTP_GET, .handler = handle_scout_list };
  register_route(server, &scout_list_ep);

  httpd_uri_t scout_pair_start_ep = { .uri = "/api/scout/pair/start", .method = HTTP_POST, .handler = handle_scout_pair_start };
  register_route(server, &scout_pair_start_ep);

  httpd_uri_t scout_pair_status_ep = { .uri = "/api/scout/pair/status", .method = HTTP_GET, .handler = handle_scout_pair_status };
  register_route(server, &scout_pair_status_ep);

  httpd_uri_t scout_pair_cancel_ep = { .uri = "/api/scout/pair/cancel", .method = HTTP_POST, .handler = handle_scout_pair_cancel };
  register_route(server, &scout_pair_cancel_ep);

  httpd_uri_t scout_unpair_ep = { .uri = "/api/scout/unpair", .method = HTTP_POST, .handler = handle_scout_unpair };
  register_route(server, &scout_unpair_ep);
  #endif

  #if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
  // Mesh / opera REST API (PR-8, F10, F10-rekey). 12 registrations — see spec §8.1.
  httpd_uri_t mesh_status_ep = { .uri = "/api/mesh", .method = HTTP_GET, .handler = handle_mesh_status };
  register_route(server, &mesh_status_ep);

  httpd_uri_t mesh_peers_ep = { .uri = "/api/mesh/peers", .method = HTTP_GET, .handler = handle_mesh_peers };
  register_route(server, &mesh_peers_ep);

  httpd_uri_t mesh_pair_start_ep = { .uri = "/api/mesh/pair/start", .method = HTTP_POST, .handler = handle_mesh_pair_start };
  register_route(server, &mesh_pair_start_ep);

  httpd_uri_t mesh_pair_join_ep = { .uri = "/api/mesh/pair/join", .method = HTTP_POST, .handler = handle_mesh_pair_join };
  register_route(server, &mesh_pair_join_ep);

  httpd_uri_t mesh_pair_confirm_ep = { .uri = "/api/mesh/pair/confirm", .method = HTTP_POST, .handler = handle_mesh_pair_confirm };
  register_route(server, &mesh_pair_confirm_ep);

  httpd_uri_t mesh_pair_cancel_ep = { .uri = "/api/mesh/pair/cancel", .method = HTTP_POST, .handler = handle_mesh_pair_cancel };
  register_route(server, &mesh_pair_cancel_ep);

  httpd_uri_t mesh_leave_ep = { .uri = "/api/mesh/leave", .method = HTTP_POST, .handler = handle_mesh_leave };
  register_route(server, &mesh_leave_ep);

  httpd_uri_t mesh_name_ep = { .uri = "/api/mesh/name", .method = HTTP_POST, .handler = handle_mesh_name };
  register_route(server, &mesh_name_ep);

  httpd_uri_t mesh_enable_ep = { .uri = "/api/mesh/enable", .method = HTTP_POST, .handler = handle_mesh_enable };
  register_route(server, &mesh_enable_ep);

  httpd_uri_t mesh_alerts_ep = { .uri = "/api/mesh/alerts", .method = HTTP_GET, .handler = handle_mesh_alerts };
  register_route(server, &mesh_alerts_ep);

  httpd_uri_t mesh_alerts_clear_ep = { .uri = "/api/mesh/alerts", .method = HTTP_DELETE, .handler = handle_mesh_alerts_clear };
  register_route(server, &mesh_alerts_clear_ep);

  httpd_uri_t mesh_remove_ep = { .uri = "/api/mesh/remove", .method = HTTP_POST, .handler = handle_mesh_remove };
  register_route(server, &mesh_remove_ep);
  #endif

  // Wildcard fallback — MUST stay the last registration, so every exact
  // route above wins first. During setup it funnels stray hijacked-DNS
  // requests to the wizard; otherwise it 404s like before.
  httpd_uri_t catchall = { .uri = "/*", .method = HTTP_GET, .handler = handle_captive_catchall };
  register_route(server, &catchall);
}

// ════════════════════════════════════════════════════════════════════════════
// HTTP HANDLERS
// ════════════════════════════════════════════════════════════════════════════

// Include triggers PlatformIO LDF to build+link the securacv_webui library
#include "securacv_webui.h"

// Stream an embedded HTML page, injecting the bearer credential at the
// `__CV_TOKEN__` placeholder so the page's fetch() helper can send
// `Authorization: Bearer cv_…`. The pages are tens of KB and ESP32 heap
// fragments fast, so we stream prefix/token/suffix as three chunks rather
// than allocating a rendered copy. Shared by the dashboard (/) and the
// first-boot setup wizard (/setup + the captive-portal probe paths).
//
// `inject` is the page-token policy's verdict (page_token_inject below).
// When it is false the placeholder is streamed EMPTY and the response
// carries `X-CV-Token: withheld`: the SPA's api() helper already skips the
// Authorization header for an empty/placeholder token and renders the
// unlock banner (tap BOOT, use the Canary's own Wi-Fi, or paste the kit
// token). This is what stops any device on the home LAN from reading the
// credential out of view-source (F20 gap #11).
static esp_err_t send_html_with_token(httpd_req_t* req, const char* html, bool inject) {
  httpd_resp_set_type(req, "text/html");
  // no-store: captive sheets cache aggressively, and a cached copy of this
  // page carries the PREVIOUS Canary's bearer token when the same phone
  // provisions a second device — every wizard API call then fails auth.
  // Mirrors canary-display's provision.cpp, which learned this the hard way.
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  static const char kTokenPlaceholder[] = "__CV_TOKEN__";
  const size_t placeholder_len = sizeof(kTokenPlaceholder) - 1;

  const char* needle = strstr(html, kTokenPlaceholder);
  if (!needle) {
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
  }

  const char* token = inject ? auth_get_token() : "";
  if (!token) token = "";
  if (!inject) httpd_resp_set_hdr(req, "X-CV-Token", "withheld");
  const size_t token_len = strlen(token);
  const size_t prefix_len = needle - html;

  esp_err_t result = httpd_resp_send_chunk(req, html, prefix_len);
  if (result != ESP_OK) return result;

  if (token_len > 0) {
    result = httpd_resp_send_chunk(req, token, token_len);
    if (result != ESP_OK) return result;
  }

  result = httpd_resp_send_chunk(req, needle + placeholder_len, HTTPD_RESP_USE_STRLEN);
  if (result != ESP_OK) return result;

  return httpd_resp_send_chunk(req, NULL, 0);
}

// The page-token decision for this request (provisioning_gate::page_token_decide,
// host-tested): inject while the first-boot wizard is active, for a
// bearer-authenticated caller, for a peer inside the live SoftAP subnet, or
// by SPENDING an unspent BOOT tap — taken, never peeked, so one tap unlocks
// exactly one home-LAN page load (or, if the app asks first, one receipt
// fetch; never both). The page that got the token holds the bearer, so its
// "Save recovery kit" needs no second tap. Everything else (the home LAN)
// gets the page without the credential.
static bool page_token_inject(httpd_req_t* req) {
  using canary::net::provisioning_gate::PageToken;
  using canary::net::provisioning_gate::page_token_decide;
  const bool setup_active = setup_is_active() || setup_is_first_boot();
  const bool bearer_ok    = bearer_present_and_valid(req);
  const bool on_ap        = from_ap_subnet(req);
  bool tap_spent = false;
  const char* why = nullptr;
  const PageToken verdict = page_token_decide(
      setup_active, bearer_ok, on_ap,
      [&tap_spent]() {
        tap_spent = provisioning_gate_take();
        return tap_spent;
      },
      &why);
  if (verdict == PageToken::WITHHOLD) {
    Serial.printf("[AUTH] page token withheld (%s)\n", why ? why : "");
  } else if (tap_spent) {
    Serial.println("[AUTH] page token handed to one page load on a BOOT tap. Gate closed.");
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Page token unlocked", "BOOT gate");
  }
  return verdict == PageToken::INJECT;
}

static esp_err_t handle_ui(httpd_req_t* req) {
  witness_get_health().http_requests++;
  return send_html_with_token(req, CANARY_UI_HTML, page_token_inject(req));
}

// ════════════════════════════════════════════════════════════════════════════
// PROVISIONING RECEIPT (F20 gap #11) — bearer OR one BOOT tap
// ════════════════════════════════════════════════════════════════════════════
//
// The WAP's receipt shape (canary_wap.ino send_provisioning_receipt), which
// the iOS app's ProvisioningReceipt parses: device_id, base_url, token,
// pubkey_fp, firmware, hw_token, ap_ssid, ap_password, tls_cert_fp,
// provisioned_at. With HTTPS up (F15) the base_url is https:// and
// tls_cert_fp carries the certificate pin; HTTP-only, http:// and "". The
// route lives on the primary server only — on the TLS server when HTTPS is
// up, never on the port-80 redirect server (WAP parity).

static esp_err_t send_provisioning_receipt(httpd_req_t* req) {
  DeviceIdentity& device = witness_get_device();
  ScvNetworkManager& net = network_get_instance();

  char fp_hex[17];
  hex_to_str(fp_hex, device.pubkey_fp, 8);
  // Privacy (Invariant III): salted pseudonym, never the raw MAC.
  char hw_token[device_pseudonym::HEX_LEN + 1];
  if (!device_pseudonym::device_id_hex(hw_token, sizeof(hw_token))) hw_token[0] = '\0';
  char addr[16];
  local_addr_of(req, addr, sizeof(addr));
  char base_url[32];
  snprintf(base_url, sizeof(base_url), "%s://%s",
           net.isTlsEnabled() ? "https" : "http", addr);
  char provisioned_at[24];
  snprintf(provisioned_at, sizeof(provisioned_at), "boot:%lu", (unsigned long)device.boot_count);

  JsonDocument doc;
  doc["device_id"]      = device.device_id;
  doc["base_url"]       = base_url;
  doc["token"]          = auth_get_token();
  doc["pubkey_fp"]      = fp_hex;
  doc["firmware"]       = FIRMWARE_VERSION;
  doc["hw_token"]       = hw_token;
  doc["ap_ssid"]        = net.getApSsid();
  doc["ap_password"]    = net.getApPassword();
  // The pin and the scheme move together: the iPhone app refuses an https
  // base_url whose receipt carries no tls_cert_fp.
  doc["tls_cert_fp"]    = net.isTlsEnabled() ? net.getTlsCertFp() : "";
  doc["provisioned_at"] = provisioned_at;

  String response;
  serializeJson(doc, response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, response.c_str());
}

static esp_err_t handle_provisioning_receipt(httpd_req_t* req) {
  witness_get_health().http_requests++;

  // A valid bearer always gets the receipt (the SPA's "Save recovery kit"
  // button, the iOS app re-syncing) — silently checked through
  // auth_check_optional(), no 401 on miss, refused outright while the
  // credential is unprovisioned.
  if (bearer_present_and_valid(req)) {
    return send_provisioning_receipt(req);
  }

  // No bearer: consume the physical gate in ONE atomic step (a second poll,
  // or a second consumer on another task, reads it closed).
  if (!provisioning_gate_take()) {
    char body[256];
    if (!canary::net::provisioning_gate::build_gate_refusal_json(
            body, sizeof(body), PROVISIONING_GATE_TTL_MS)) {
      // Truncation guard: never ship half-JSON to the app.
      return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                 "Failed to build provisioning gate response");
    }
    witness_get_health().http_errors++;
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
  }

  esp_err_t result = send_provisioning_receipt(req);
  Serial.println("[AUTH] Provisioning receipt served. Gate closed.");
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Provisioning receipt served", "BOOT gate");
  return result;
}

// ════════════════════════════════════════════════════════════════════════════
// CAPTIVE-PORTAL PROBES + SETUP WIZARD
// ════════════════════════════════════════════════════════════════════════════
//
// During first-boot setup the DNS responder (securacv_setup) answers every
// name with the AP's address, so the phone's OS sends its connectivity probe
// here. The per-platform "hybrid" strategy — proven on canary-wap, see its
// captive_probe.h and firmware/LESSONS_LEARNED.md "Networking & Captive
// Portal" — decides what each probe gets:
//
//   - Apple   (/hotspot-detect.html, /library/test/success.html): while setup
//     is active, 200 + the setup WIZARD itself — that pops the Captive
//     Network Assistant sheet automatically (no "open Safari" folklore) and
//     the sheet keeps the Wi-Fi association up while the user works. Once
//     setup is complete, Apple's own Success token, so the sheet can close
//     cleanly and never nags again.
//   - Android (/generate_204, /gen_204): 204 No Content, so Android marks the
//     AP validated and never falls back to cellular mid-setup.
//   - Windows (/connecttest.txt, /ncsi.txt): the exact NCSI success bodies.
//
// The wizard page itself is small, static-plus-vanilla-JS HTML
// (CANARY_SETUP_HTML) — captive mini-browsers choke on the full dashboard
// SPA, which is why the probe never serves CANARY_UI_HTML.

static const char kAppleSuccessBody[] =
    "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";

// True when `uri`'s path component — everything before '?' or '#' — equals
// `lit` exactly. Probe URLs sometimes carry a cache-busting query and
// req->uri keeps it, so plain strcmp would misroute those.
static bool probe_path_is(const char* uri, const char* lit) {
  size_t i = 0;
  for (; lit[i] != '\0'; ++i) {
    if (uri[i] != lit[i]) return false;
  }
  return uri[i] == '\0' || uri[i] == '?' || uri[i] == '#';
}

// F15: what the Apple probe gets on the plain port-80 server while HTTPS is
// up and the home Wi-Fi is down (the retry case above). A captive sheet
// renders a blank page on a self-signed certificate, so it cannot simply be
// redirected; this names the https:// address to open in a real browser.
static esp_err_t send_tls_captive_hint(httpd_req_t* req) {
  char addr[16];
  local_addr_of(req, addr, sizeof(addr));
  char page[640];
  const int n = snprintf(page, sizeof(page),
      "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>Canary</title></head><body style=\"font-family:sans-serif;padding:1.5rem;\">"
      "<h2>Open your Canary in a browser</h2>"
      "<p>This Canary's dashboard uses an encrypted connection. Open "
      "<b>https://%s/setup</b> in Safari or Chrome.</p>"
      "<p>Your browser will warn that the certificate is not trusted. That is "
      "expected: the Canary made it for itself. Continue to the page.</p>"
      "</body></html>",
      addr);
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (n < 0 || (size_t)n >= sizeof(page)) {
    return httpd_resp_sendstr(req, kAppleSuccessBody);
  }
  return httpd_resp_sendstr(req, page);
}

static esp_err_t handle_captive_probe(httpd_req_t* req) {
  witness_get_health().http_requests++;
  const char* uri = req->uri;

  if (probe_path_is(uri, "/generate_204") || probe_path_is(uri, "/gen_204")) {
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
  }
  if (probe_path_is(uri, "/connecttest.txt") || probe_path_is(uri, "/ncsi.txt")) {
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(
        req, strstr(uri, "ncsi") ? "Microsoft NCSI" : "Microsoft Connect Test");
  }
  // Apple (and the connection-preserving fallback for anything misrouted).
  // Serve the wizard while setup is active OR while the home-Wi-Fi link is
  // down — a typo'd password saves credentials (which completes "setup") but
  // leaves the Canary offline, and the sheet must keep offering the wizard
  // for the retry, not declare Success. Only a live STA link earns Apple's
  // Success token (which lets the sheet close cleanly and stop nagging).
  if (setup_is_active() || !network_get_instance().getStatus().sta_connected) {
    // F15: with HTTPS up, this probe arrived on the plain port-80 server,
    // and the wizard's API calls would all be redirected to a self-signed
    // https:// origin the captive sheet cannot open. Point at it instead.
    if (network_get_instance().isTlsEnabled() &&
        req->handle != network_get_instance().getHttpsServer()) {
      return send_tls_captive_hint(req);
    }
    return send_html_with_token(req, CANARY_SETUP_HTML, page_token_inject(req));
  }
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_sendstr(req, kAppleSuccessBody);
}

// GET /setup — the wizard at a stable address, reachable from a real browser
// too (canary.local/setup), not only through the captive sheet.
static esp_err_t handle_setup_page(httpd_req_t* req) {
  witness_get_health().http_requests++;
  return send_html_with_token(req, CANARY_SETUP_HTML, page_token_inject(req));
}

// Wildcard fallback, registered LAST. While setup is active every stray
// hijacked-DNS request (favicon fetches, portals the phone remembers, …)
// funnels to the wizard instead of 404ing — a 404 here would make the
// captive sheet look broken. Outside setup, keep the old 404 behavior.
static esp_err_t handle_captive_catchall(httpd_req_t* req) {
  witness_get_health().http_requests++;
  if (!setup_is_active()) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    return ESP_OK;
  }
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/setup");
  return httpd_resp_send(req, NULL, 0);
}

// FEATURE_HTTPS port-80 server (F15): everything that is not a probe goes to
// https:// on the host the client asked for (or, when its Host header is not a
// plain host, the address the request arrived on). 307, not 301: a 301 is
// cached as permanent, and a factory-reset Canary serves its setup wizard on
// plain HTTP again — a browser remembering "always https" could not reach it;
// 307 also keeps a POST a POST. Truncation or an unusable target answers 500,
// never a cut-off Location (tls_policy::build_redirect_location, host-tested).
static esp_err_t handle_https_redirect(httpd_req_t* req) {
  witness_get_health().http_requests++;
  if (canary::net::tls_policy::plain_http_exempt(req->uri, setup_is_active())) {
    // Defensive: the probes are registered ahead of this wildcard, but a
    // probe path that reaches here still gets its plain answer.
    return handle_captive_probe(req);
  }
  char host[64];
  host[0] = '\0';
  const size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
  if (host_len == 0 || host_len >= sizeof(host) ||
      httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
    host[0] = '\0';
  }
  char arrived_on[16];
  local_addr_of(req, arrived_on, sizeof(arrived_on));
  char location[256];
  if (!canary::net::tls_policy::build_redirect_location(
          location, sizeof(location), host, arrived_on, req->uri)) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "https redirect target too long");
  }
  httpd_resp_set_status(req, "307 Temporary Redirect");
  httpd_resp_set_hdr(req, "Location", location);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, "Redirecting to https");
}

static esp_err_t handle_status(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  DeviceIdentity& device = witness_get_device();
  SystemHealth& health = witness_get_health();

  JsonDocument doc;
  doc["ok"] = true;
  doc["device_id"] = device.device_id;
  doc["device_type"] = DEVICE_TYPE;
  doc["firmware"] = FIRMWARE_VERSION;
  doc["ruleset"] = RULESET_ID;

  char fp_hex[17];
  hex_to_str(fp_hex, device.pubkey_fp, 8);
  doc["fingerprint"] = fp_hex;

  char pubkey_hex[65];
  hex_to_str(pubkey_hex, device.pubkey, 32);
  doc["pubkey"] = pubkey_hex;

  doc["uptime_sec"] = uptime_seconds();
  doc["boot_count"] = device.boot_count;
  doc["chain_seq"] = device.seq;
  // Where the identity key sleeps, read live from the eFuses (a wire label,
  // never key material — common/identity/key_at_rest.h).
  doc["key_at_rest"] = crypto_key_at_rest_label();
  doc["witness_count"] = health.records_created;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["min_heap"] = health.min_heap;

  doc["crypto_healthy"] = health.crypto_healthy;
  doc["gps_healthy"] = health.gps_healthy;
  doc["sd_healthy"] = health.sd_healthy;
  doc["wifi_active"] = health.wifi_active;

  doc["logs_stored"] = health.logs_stored;
  doc["unacked_count"] = health.logs_unacked;

  // F20 gap #11: "physical_button" (the WAP's /api/device-info value for the
  // same field, so a client reads one vocabulary) once main.cpp has wired the
  // BOOT-tap hooks;
  // "unwired" means the receipt can only be fetched with the bearer and a
  // home-LAN page load can never be unlocked by a tap (fails closed, and the
  // bench can see it instead of guessing).
  doc["provisioning_gate"] = (s_gate_take && s_gate_is_open) ? "physical_button" : "unwired";

  // F15: what actually came up, and why — a device that fell back to HTTP
  // says so here instead of leaving the bench to guess (tls_policy::decide).
  {
    ScvNetworkManager& net = network_get_instance();
    doc["tls_enabled"] = net.isTlsEnabled();
    doc["tls_cert_fp"] = net.isTlsEnabled() ? net.getTlsCertFp() : "";
    doc["tls_mode_reason"] = net.getTlsModeReason();
    // F16: SoftAP / STA security as it actually came up.
    const WiFiStatus& ws = net.getStatus();
    doc["ap_auth"] = ws.ap_auth[0] ? ws.ap_auth : "unknown";
    doc["sta_pmf"] = ws.sta_pmf;
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// GET /api/v1/peers
// Returns the cached list of other Canaries this device has discovered via
// mDNS (_securacv._tcp). The cache is populated by ScvNetworkManager::browsePeers
// on a slow cadence; this handler is read-only and never blocks on the
// network. Response shape matches canary-vision/docs/discovery.md.
static esp_err_t handle_peers(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  // Refresh opportunistically — internally throttled so this is cheap if
  // we already browsed within the interval.
  network_get_instance().browsePeers();

  const PeerEntry* peers = network_get_instance().getPeers();

  JsonDocument doc;
  doc["ok"] = true;
  JsonArray arr = doc["peers"].to<JsonArray>();
  uint32_t now = millis();
  for (size_t i = 0; i < PEER_CACHE_MAX; i++) {
    if (!peers[i].valid) continue;
    JsonObject p = arr.add<JsonObject>();
    p["device_id"]      = peers[i].device_id;
    if (peers[i].name[0]) p["name"] = peers[i].name;
    if (peers[i].ip[0])   p["ip"]   = peers[i].ip;
    if (peers[i].mdns_hostname[0]) p["mdns_hostname"] = peers[i].mdns_hostname;
    p["last_seen_ms_ago"] = (uint32_t)(now - peers[i].last_seen_ms);
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_chain(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  DeviceIdentity& device = witness_get_device();
  WitnessRecord& last = witness_get_last_record();

  JsonDocument doc;
  doc["ok"] = true;

  char chain_hex[65];
  hex_to_str(chain_hex, device.chain_head, 32);
  doc["chain_head"] = chain_hex;
  doc["sequence"] = device.seq;

  if (last.seq > 0) {
    JsonArray blocks = doc["blocks"].to<JsonArray>();
    JsonObject block = blocks.add<JsonObject>();
    char hash[65];
    hex_to_str(hash, last.chain_hash, 32);
    block["seq"] = last.seq;
    block["hash"] = hash;
    block["type"] = record_type_name(last.type);
    block["verified"] = last.verified;
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

#if FEATURE_SD_STORAGE
// A timeline page from the card (F35): records older than the ring, read by
// the loop task through the bridge (securacv_witness_history.h) — this task
// only posts the request and waits, at most WAIT_MS. Rows say where they came
// from and whether they chain to the next older record on the card; they are
// never "verified": no signature is checked on this path (the off-device
// verifier, tools/verify_witness_log.py, is how a card is verified).
static esp_err_t send_card_page(httpd_req_t* req, uint32_t before_seq, size_t last,
                                bool has_hint, uint32_t hint, size_t ring_total) {
  namespace whb = witness_history_bridge;
  whb::Request q;
  memset(&q, 0, sizeof(q));
  q.before_seq = before_seq;
  q.has_hint = has_hint;
  q.hint = hint;
  q.want = (uint8_t)((last == 0 || last > whb::PAGE_ROWS_MAX) ? whb::PAGE_ROWS_MAX : last);

  const whb::Response* page = nullptr;
  uint32_t gen = 0;
  switch (witness_history_request(q, &page, &gen)) {
    case WitnessHistoryWait::BUSY:    return http_send_error(req, 503, "history_busy");
    case WitnessHistoryWait::TIMEOUT: return http_send_error(req, 504, "history_timeout");
    case WitnessHistoryWait::PAGE:    break;
  }
  if (page->result != whb::Result::OK) {
    const bool no_card = (page->result == whb::Result::NO_CARD);
    witness_history_release(gen);
    return no_card ? http_send_error(req, 503, "no_card")
                   : http_send_error(req, 500, "history_read_failed");
  }

  // Build the answer while the page is ours, then free the slot before the
  // (slower) send. Oldest -> newest, like the ring page.
  JsonDocument doc;
  doc["ok"] = true;
  doc["source"] = "sd";
  doc["total"] = ring_total;
  JsonArray records = doc["records"].to<JsonArray>();
  char hash[65];
  for (size_t k = page->n; k-- > 0;) {
    const size_t i = (size_t)page->first + k;
    const witness_history::HistoryRow& row = page->rows[i];
    JsonObject r = records.add<JsonObject>();
    r["seq"] = row.seq;
    r["type_name"] = record_type_name((RecordType)row.type);
    hex_to_str(hash, row.ch, 32);
    r["chain_hash"] = hash;
    r["time_bucket"] = row.tb;
    r["source"] = "sd";
    if (page->linked[i] == whb::Link::NONE) r["linked"] = nullptr;  // nothing older on the card
    else r["linked"] = (page->linked[i] == whb::Link::LINKED);
  }
  doc["next_hint"] = page->next_hint;
  doc["more"] = page->more;
  if (page->joins != whb::Link::NONE) doc["joins"] = (page->joins == whb::Link::LINKED);
  doc["hint_refused"] = page->hint_refused;
  doc["skipped"] = page->skipped;
  witness_history_release(gen);

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}
#endif  // FEATURE_SD_STORAGE

// Serve the timeline: the recent witness-record ring, and — once a page asks
// for records older than the ring holds — pages from the card (F35). The ring
// is bounded RAM (display-only; the tamper-evident guarantee lives in the
// hash chain). The card pages come from the loop task through the history
// bridge: this handler reads only in-RAM state itself — no SD, no camera.
static esp_err_t handle_witness(httpd_req_t* req) {
  // Parse first (no side effects): whether this is a card page decides how
  // the rate limiter counts it — an SD page counts as an action.
  //   ?last=N    clamp to [1, total] (card pages: [1, PAGE_ROWS_MAX]).
  //   ?before=S  exclusive upper bound: only records with seq < S — how the
  //              timeline's "Load More" pages backward.
  //   ?hint=H    the previous card page's next_hint: where the next one
  //              starts. Client input — the bridge re-checks it before use.
  size_t last = 0;          // 0 = not given
  uint32_t before_seq = 0;  // 0 = no bound
  uint32_t hint = 0;
  bool has_hint = false;
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen > 0 && qlen < 128) {
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
      char val[12];
      if (httpd_query_key_value(query, "last", val, sizeof(val)) == ESP_OK) {
        int n = atoi(val);
        if (n > 0) last = (size_t)n;
      }
      if (httpd_query_key_value(query, "before", val, sizeof(val)) == ESP_OK) {
        long b = atol(val);
        if (b > 0) before_seq = (uint32_t)b;
      }
      if (httpd_query_key_value(query, "hint", val, sizeof(val)) == ESP_OK &&
          val[0] >= '0' && val[0] <= '9') {
        char* end = nullptr;
        const unsigned long h = strtoul(val, &end, 10);  // overflow: ULONG_MAX, past any file
        if (end != nullptr && *end == '\0') {
          hint = (uint32_t)h;
          has_hint = true;
        }
      }
    }
  }

  const size_t ring_size = witness_get_record_ring_size();
  const size_t total = witness_get_record_count();
  const size_t head  = witness_get_record_head();
  uint32_t ring_oldest_seq = 0;
  if (total > 0) {
    WitnessRecord oldest;
    if (witness_copy_record_at((head + ring_size - total) % ring_size, &oldest))
      ring_oldest_seq = oldest.seq;
  }

#if FEATURE_SD_STORAGE
  // Nothing below `before` is in the ring: the next records are on the card.
  const bool card_page = before_seq > 0 && (total == 0 || before_seq <= ring_oldest_seq);
#else
  const bool card_page = false;
#endif

  if (!rate_limit_check(req, card_page)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

#if FEATURE_SD_STORAGE
  if (card_page) return send_card_page(req, before_seq, last, has_hint, hint, total);
#else
  (void)has_hint;
  (void)hint;
  (void)ring_oldest_seq;
#endif

  size_t want = total;
  if (last > 0 && last < want) want = last;

  // With a bound, shrink the window to the records older than it. Ring seqs
  // are contiguous ascending, so count the newest entries at or past the
  // bound and drop them from the tail of the chronological window.
  size_t bounded_total = total;
  if (before_seq > 0) {
    size_t at_or_past = 0;
    for (size_t j = total; j > 0; j--) {
      const size_t idx = (head + ring_size - total + (j - 1)) % ring_size;
      WitnessRecord rec;
      if (!witness_copy_record_at(idx, &rec)) break;
      if (rec.seq < before_seq) break;  // older half reached — done
      at_or_past++;
    }
    bounded_total = total - at_or_past;
    if (want > bounded_total) want = bounded_total;
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["total"] = total;

  JsonArray records = doc["records"].to<JsonArray>();

  // Emit chronological (oldest→newest) for the most recent `want` records of
  // the (possibly ?before-bounded) window so the UI's reverse shows newest
  // first. Each slot is copied under the ring lock so a concurrent record
  // write can't be observed torn. Oldest of the window sits at this index:
  char hash[65];
  const size_t start = bounded_total - want;
  for (size_t j = start; j < bounded_total; j++) {
    const size_t idx = (head + ring_size - total + j) % ring_size;
    WitnessRecord rec;
    if (!witness_copy_record_at(idx, &rec)) continue;

    JsonObject r = records.add<JsonObject>();
    r["seq"] = rec.seq;
    r["type_name"] = record_type_name(rec.type);
    hex_to_str(hash, rec.chain_hash, 32);
    r["chain_hash"] = hash;
    r["time_bucket"] = rec.time_bucket;
    r["payload_len"] = (uint32_t)rec.payload_len;
    r["verified"] = rec.verified;
  }

  // Is anything older to page to? Older ring records below this window, or —
  // on a build with a card — whatever precedes the ring's oldest record
  // (seq 1 is the chain's first). A card page answers for itself.
#if FEATURE_SD_STORAGE
  doc["more"] = (want > 0) && (start > 0 || ring_oldest_seq > 1);
#else
  doc["more"] = (want > 0) && (start > 0);
#endif

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_logs(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  HealthLogRingEntry* ring = witness_get_health_log_ring();
  size_t count = witness_get_health_log_count();
  size_t head = witness_get_health_log_head();

  JsonDocument doc;
  doc["ok"] = true;
  doc["total"] = count;

  JsonArray logs = doc["logs"].to<JsonArray>();

  for (size_t i = 0; i < count; i++) {
    size_t idx = (head + 100 - 1 - i) % 100;
    HealthLogRingEntry& entry = ring[idx];

    JsonObject log = logs.add<JsonObject>();
    log["seq"] = entry.seq;
    log["timestamp_ms"] = entry.timestamp_ms;
    log["level"] = (int)entry.level;
    log["level_name"] = log_level_name(entry.level);
    log["category"] = log_category_name(entry.category);
    log["message"] = entry.message;
    if (entry.detail[0]) {
      log["detail"] = entry.detail;
    }
    log["ack_status"] = ack_status_name(entry.ack_status);
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_log_ack(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  const char* uri = req->uri;
  const char* seq_start = strstr(uri, "/logs/");
  if (!seq_start) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI");
  }
  seq_start += 6;
  uint32_t seq = atoi(seq_start);

  bool success = acknowledge_log_entry(seq, ACK_STATUS_ACKNOWLEDGED, "");

  JsonDocument doc;
  doc["ok"] = success;
  if (!success) {
    doc["error"] = "Log entry not found";
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_ack_all(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  HealthLogRingEntry* ring = witness_get_health_log_ring();
  size_t count = witness_get_health_log_count();

  uint32_t acked = 0;
  for (size_t i = 0; i < count; i++) {
    if (ring[i].ack_status == ACK_STATUS_UNREAD) {
      ring[i].ack_status = ACK_STATUS_ACKNOWLEDGED;
      acked++;
    }
  }
  witness_get_health().logs_unacked = 0;

  log_health(LOG_LEVEL_INFO, LOG_CAT_USER, "Bulk acknowledgment", nullptr);

  JsonDocument doc;
  doc["ok"] = true;
  doc["acknowledged"] = acked;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_reboot(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  log_health(LOG_LEVEL_NOTICE, LOG_CAT_USER, "Reboot requested", nullptr);

  // The witness lib owns chain persistence (one atomic blob — never the
  // legacy seq/chain pair from here, which was the second torn-write site).
  witness_persist_chain_state();

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Rebooting...";

  String response;
  serializeJson(doc, response);
  http_send_json(req, response.c_str());

  delay(500);
  ESP.restart();
  return ESP_OK;
}

#if FEATURE_OTA_UPDATE && !defined(SECURACV_BUILD_RELEASE)
static esp_err_t handle_ota(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  if (req->content_len <= 0 || req->content_len > 2 * 1024 * 1024) {
    return http_send_error(req, 400, "invalid_size");
  }

  if (!Update.begin(req->content_len)) {
    return http_send_error(req, 500, "ota_begin_failed");
  }

  char buf[4096];
  int remaining = req->content_len;
  while (remaining > 0) {
    int recv_len = httpd_req_recv(req, buf, (remaining < (int)sizeof(buf)) ? remaining : sizeof(buf));
    if (recv_len <= 0) {
      Update.abort();
      return http_send_error(req, 500, "receive_failed");
    }
    if (Update.write((uint8_t*)buf, recv_len) != recv_len) {
      Update.abort();
      return http_send_error(req, 500, "write_failed");
    }
    remaining -= recv_len;
  }

  if (Update.end(true)) {
    const char* resp = "{\"ok\":true,\"message\":\"Rebooting...\"}";
    http_send_json(req, resp);
    delay(500);
    ESP.restart();
    return ESP_OK;
  } else {
    return http_send_error(req, 500, "ota_end_failed");
  }
}
#endif

// ════════════════════════════════════════════════════════════════════════════
// SIGNED PULL-OTA — status / check / install / config
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_OTA_PULL

// GET /api/ota/status — everything the Settings UI needs in one call.
// `state_text` / `error_text` are the plain-language strings shown to the
// user; the technical `state` / `error` fields feed diagnostics.
static esp_err_t handle_ota_status(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  const securacv_ota_state_t state = securacv_ota_get_state();
  const securacv_ota_error_t error = securacv_ota_get_last_error();

  JsonDocument doc;
  doc["ok"] = true;
  doc["installed_version"] = securacv_ota_get_version();
  doc["state"] = securacv_ota_state_str(state);
  doc["state_text"] = securacv_ota_friendly_state(state);
  doc["progress"] = securacv_ota_get_progress();
  doc["error"] = securacv_ota_error_str(error);
  doc["error_text"] = securacv_ota_friendly_error(error);
  doc["update_available"] = securacv_ota_update_available();
  doc["auto_update"] = securacv_ota_get_auto_update();
  doc["local_http_allowed"] = securacv_ota_get_local_http_allowed();
  doc["last_check"] = securacv_ota_get_last_check_time();

  char url[256];
  if (securacv_ota_get_manifest_url(url, sizeof(url)) == ESP_OK) {
    doc["manifest_url"] = url;
  }
  doc["manifest_url_is_override"] = securacv_ota_manifest_url_is_override();

  const securacv_ota_manifest_t* m = securacv_ota_get_manifest();
  if (m != NULL) {
    doc["latest_version"] = m->version;
    if (m->release_notes[0] != '\0') doc["release_notes"] = m->release_notes;
    if (m->release_url[0] != '\0') doc["release_url"] = m->release_url;
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// POST /api/ota/check — fetch the manifest and compare versions, without
// installing. Results land in /api/ota/status (poll while state=Checking).
static esp_err_t handle_ota_check(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  esp_err_t err = securacv_ota_check();
  if (err == ESP_ERR_INVALID_STATE) {
    return http_send_error(req, 409, "ota_busy");
  }
  if (err != ESP_OK) {
    return http_send_error(req, 500, "ota_check_failed");
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Checking for updates…";
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// POST /api/ota/install — full signed install pipeline. The device
// reboots into the new firmware on success; progress is visible via
// /api/ota/status and the HA update entity.
static esp_err_t handle_ota_install(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  log_health(LOG_LEVEL_NOTICE, LOG_CAT_USER,
             "Firmware install requested from dashboard", nullptr);

  esp_err_t err = securacv_ota_check_and_install();
  if (err == ESP_ERR_INVALID_STATE) {
    return http_send_error(req, 409, "ota_busy");
  }
  if (err != ESP_OK) {
    return http_send_error(req, 500, "ota_install_failed");
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Installing the update. Your Canary will restart on its own.";
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// POST /api/ota/config — persist update settings (NVS).
// Body (all fields optional): {"manifest_url": "...", "auto_update": bool,
// "local_http_allowed": bool}. An empty manifest_url clears the override.
static esp_err_t handle_ota_config(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  char body[512];
  int recv = httpd_req_recv(req, body, sizeof(body) - 1);
  if (recv <= 0) {
    return http_send_error(req, 400, "empty_body");
  }
  body[recv] = '\0';

  JsonDocument input;
  if (deserializeJson(input, body) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }

  // Order matters: apply the local-http opt-in first so a manifest_url
  // pointing at a LAN server in the same request validates correctly.
  if (input["local_http_allowed"].is<bool>()) {
    securacv_ota_set_local_http_allowed(input["local_http_allowed"].as<bool>());
  }

  if (input["manifest_url"].is<const char*>()) {
    const char* url = input["manifest_url"].as<const char*>();
    if (securacv_ota_set_manifest_url(url) != ESP_OK) {
      return http_send_error(req, 400, "url_rejected");
    }
    log_health(LOG_LEVEL_INFO, LOG_CAT_USER, "OTA manifest URL changed",
               (url && url[0]) ? "override" : "default");
  }

  if (input["auto_update"].is<bool>()) {
    securacv_ota_set_auto_update(input["auto_update"].as<bool>());
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Update settings saved.";
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

#endif // FEATURE_OTA_PULL

#if FEATURE_CAMERA_PEEK
static esp_err_t handle_peek_start(httpd_req_t* req) {
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  if (!camera_is_initialized()) {
    return http_send_error(req, 503, "camera_not_initialized");
  }

  camera_set_peek_active(true);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Peek started", nullptr);

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Peek stream activated";
  doc["resolution"] = camera_get_instance().getResolutionName();

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// PEEK STREAM — Async worker task
//
// The MJPEG loop runs in its own FreeRTOS task so the httpd worker is
// free to service /api/peek/status, /api/peek/sensor, etc. while the
// stream is active. Uses raw socket writes (IDF 4.4 compatible) since
// httpd_req_async_handler_begin/complete requires IDF 5.x.
// ════════════════════════════════════════════════════════════════════════════

static TaskHandle_t s_stream_task = nullptr;

struct StreamTaskCtx {
  int sockfd;
  httpd_handle_t server;
};

// The MJPEG part header and the inter-frame pace, shared by both stream paths
// (the raw-socket task for plain HTTP, the in-handler loop for TLS) so they
// cannot drift apart.
static int peek_part_header(char* buf, size_t cap, size_t jpeg_len) {
  return snprintf(buf, cap,
    "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
    (unsigned)jpeg_len);
}

static uint32_t peek_pace_ms(uint32_t frame_delay_ms) {
  if (frame_delay_ms < 20)  return 20;
  if (frame_delay_ms > 500) return 500;
  return frame_delay_ms;
}

static bool sock_send_all(int fd, const char* buf, size_t len) {
  while (len > 0) {
    int sent = send(fd, buf, len, 0);
    if (sent <= 0) return false;
    buf += sent;
    len -= sent;
  }
  return true;
}

static void stream_task_fn(void* param) {
  StreamTaskCtx* ctx = (StreamTaskCtx*)param;
  int sockfd = ctx->sockfd;
  httpd_handle_t server = ctx->server;
  delete ctx;

  CameraManager& cam = camera_get_instance();

  while (cam.isPeekActive()) {
    cam.checkThermal();

    if (cam.getThermalState() == THERMAL_PAUSED) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    camera_fb_t* fb = cam.captureFrame();
    if (!fb) {
      if (cam.checkFreeze(millis())) {
        cam.setPeekActive(true);
      } else if (!cam.isInitialized()) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    char part_buf[128];
    int part_len = peek_part_header(part_buf, sizeof(part_buf), fb->len);

    bool ok = sock_send_all(sockfd, part_buf, part_len);
    if (ok) ok = sock_send_all(sockfd, (const char*)fb->buf, fb->len);
    if (ok) ok = sock_send_all(sockfd, "\r\n", 2);

    uint32_t frame_bytes = (uint32_t)fb->len;
    cam.returnFrame(fb);

    if (!ok) break;
    cam.recordFrame(frame_bytes);

    vTaskDelay(pdMS_TO_TICKS(peek_pace_ms(cam.getFrameDelay())));
  }

  cam.setPeekActive(false);
  httpd_sess_trigger_close(server, sockfd);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Peek stream ended", nullptr);

  __atomic_store_n(&s_stream_task, (TaskHandle_t)nullptr, __ATOMIC_SEQ_CST);
  vTaskDelete(nullptr);
}

// F15: the stream over TLS runs synchronously IN the handler, on the httpd
// task, through httpd_resp_send_chunk (WAP parity, canary_wap.ino's peek
// stream). The plain-HTTP path below hands the socket to a worker task that
// send()s raw bytes — over TLS that would write plaintext into the record
// stream, and httpd_ssl's send override is not safe to call from a second
// task while the httpd task may read the same mbedTLS session. The cost:
// while a TLS stream runs, the TLS server answers nothing else (the WAP has
// the same limit); stopping the peek or closing the tab ends the loop.
static esp_err_t peek_stream_in_handler(httpd_req_t* req, CameraManager& cam) {
  httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Peek stream started (TLS, in handler)", nullptr);

  uint32_t fail_since_ms = 0;
  esp_err_t err = ESP_OK;
  while (cam.isPeekActive()) {
    cam.checkThermal();
    if (cam.getThermalState() == THERMAL_PAUSED) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    camera_fb_t* fb = cam.captureFrame();
    if (!fb) {
      if (cam.checkFreeze(millis())) {
        cam.setPeekActive(true);
      } else if (!cam.isInitialized()) {
        break;
      }
      // Give up after ~1 s of consecutive capture failures rather than
      // holding the TLS server's only task on a dead sensor.
      if (fail_since_ms == 0) fail_since_ms = millis();
      if (millis() - fail_since_ms > 1000) break;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    fail_since_ms = 0;

    char part_buf[128];
    const int part_len = peek_part_header(part_buf, sizeof(part_buf), fb->len);
    err = httpd_resp_send_chunk(req, part_buf, part_len);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, "\r\n", 2);
    const uint32_t frame_bytes = (uint32_t)fb->len;
    cam.returnFrame(fb);
    if (err != ESP_OK) break;  // client went away
    cam.recordFrame(frame_bytes);
    vTaskDelay(pdMS_TO_TICKS(peek_pace_ms(cam.getFrameDelay())));
  }

  cam.setPeekActive(false);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Peek stream ended", nullptr);
  if (err == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t handle_peek_stream(httpd_req_t* req) {
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  CameraManager& cam = camera_get_instance();
  if (!cam.isInitialized()) {
    return httpd_resp_send(req, "Camera not initialized", HTTPD_RESP_USE_STRLEN);
  }

  // Wait for any prior stream task to fully exit before starting a new one.
  if (__atomic_load_n(&s_stream_task, __ATOMIC_SEQ_CST) != nullptr) {
    cam.setPeekActive(false);
    int timeout_ms = 2000;
    while (__atomic_load_n(&s_stream_task, __ATOMIC_SEQ_CST) != nullptr && timeout_ms > 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      timeout_ms -= 10;
    }
    if (__atomic_load_n(&s_stream_task, __ATOMIC_SEQ_CST) != nullptr) {
      log_health(LOG_LEVEL_ERROR, LOG_CAT_NETWORK, "Old stream task failed to exit", nullptr);
      return httpd_resp_send(req, "Previous stream still active", HTTPD_RESP_USE_STRLEN);
    }
  }

  cam.setPeekActive(true);
  cam.resetMetrics();

  // TLS: stream on this task (see peek_stream_in_handler for why).
  if (req->handle == network_get_instance().getHttpsServer()) {
    return peek_stream_in_handler(req, cam);
  }

  int sockfd = httpd_req_to_sockfd(req);
  if (sockfd < 0) {
    cam.setPeekActive(false);
    return http_send_error(req, 500, "socket_error");
  }

  int yes = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
  int idle = 5, intvl = 5, cnt = 3;
  setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));

  // Send HTTP response headers synchronously via httpd, then hand the
  // socket to the worker task for raw MJPEG frame writes.
  httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, pre-check=0, post-check=0, max-age=0");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");

  // Send a zero-length chunk to flush the headers to the client.
  httpd_resp_send_chunk(req, "", 0);

  httpd_handle_t server = req->handle;
  StreamTaskCtx* ctx = new StreamTaskCtx{sockfd, server};
  TaskHandle_t new_task = nullptr;
  BaseType_t rc = xTaskCreatePinnedToCore(
    stream_task_fn, "peek_stream", 6144, ctx, 5, &new_task, tskNO_AFFINITY);
  if (rc != pdPASS) {
    delete ctx;
    cam.setPeekActive(false);
    log_health(LOG_LEVEL_ERROR, LOG_CAT_NETWORK, "Stream task creation failed", nullptr);
    return httpd_resp_send(req, "Task creation failed", HTTPD_RESP_USE_STRLEN);
  }
  __atomic_store_n(&s_stream_task, new_task, __ATOMIC_SEQ_CST);

  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Peek stream started (async)", nullptr);
  // Return without closing — the task owns the socket now.
  return ESP_OK;
}

static esp_err_t handle_peek_stop(httpd_req_t* req) {
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  camera_set_peek_active(false);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Peek stopped", nullptr);

  return http_send_json(req, "{\"ok\":true,\"message\":\"Peek stopped\"}");
}

static esp_err_t handle_peek_status(httpd_req_t* req) {
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  CameraManager& cam = camera_get_instance();

  JsonDocument doc;
  doc["ok"] = true;
  doc["camera_initialized"] = cam.isInitialized();
  doc["peek_active"] = cam.isPeekActive();
  doc["resolution"] = (int)cam.getResolution();
  doc["resolution_name"] = cam.getResolutionName();
  doc["sensor_model"] = cam.getSensorModelName();
  doc["frame_delay_ms"] = cam.getFrameDelay();

  PeekMetrics m = cam.snapshotMetrics();
  doc["frame_count"]      = m.frame_count;
  doc["last_frame_bytes"] = m.last_frame_bytes;
  doc["total_bytes"]      = m.total_bytes;
  doc["fps"]              = m.fps_last;
  if (m.stream_start_ms > 0 && m.frame_count > 0) {
    uint32_t elapsed = millis() - m.stream_start_ms;
    if (elapsed > 0) {
      doc["stream_uptime_ms"] = elapsed;
      doc["avg_kbps"] = (uint32_t)((m.total_bytes * 8ULL) / elapsed);
    }
  }

  const char* thermal_names[] = {"normal", "throttled", "paused"};
#if FEATURE_THERMAL_WATCHDOG
  /* The camera only samples while streaming, so its reading goes stale
   * the moment a peek stops. Source the temperature from the always-on
   * watchdog cache (<= 30 s old); report the camera's state while it is
   * actively streaming (it is the actuator) and the watchdog's shadow
   * classification otherwise. */
  {
    thermal_wd_state_t wd;
    if (thermal_wd_get_state(&wd)) {
      uint8_t si = cam.isPeekActive() ? (uint8_t)cam.getThermalState()
                                      : (wd.shadow_state <= 2 ? wd.shadow_state : 0);
      doc["thermal_state"] = thermal_names[si];
      doc["die_temp_c"]    = (int)lroundf(wd.die_temp_c);
      doc["thermal_sensor_ok"] = wd.sensor_ok && cam.getThermalSensorOk();
    } else {
      doc["thermal_state"] = thermal_names[cam.getThermalState()];
      doc["die_temp_c"]    = cam.getDieTempC();
      doc["thermal_sensor_ok"] = cam.getThermalSensorOk();
    }
  }
#else
  doc["thermal_state"]  = thermal_names[cam.getThermalState()];
  doc["die_temp_c"]     = cam.getDieTempC();
  doc["thermal_sensor_ok"] = cam.getThermalSensorOk();
#endif
  doc["freeze_count"]   = cam.getFreezeCount();

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// PEEK INIT — POST /api/peek/init
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_peek_init(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Camera re-init requested", nullptr);

  CameraManager& cam = camera_get_instance();
  bool ok = cam.reinit();

  JsonDocument doc;
  doc["ok"] = ok;
  doc["camera_initialized"] = cam.isInitialized();
  if (ok) {
    doc["resolution_name"] = cam.getResolutionName();
    doc["sensor_model"] = cam.getSensorModelName();
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Camera re-init succeeded", nullptr);
  } else {
    doc["error"] = "Camera initialization failed";
    log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "Camera re-init failed", nullptr);
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// PEEK RESOLUTION — POST /api/peek/resolution
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_peek_resolution(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  CameraManager& cam = camera_get_instance();
  if (!cam.isInitialized()) {
    return http_send_error(req, 503, "camera_not_initialized");
  }

  if (req->content_len >= 64) {
    return http_send_error(req, 413, "payload_too_large");
  }

  char content[64] = {0};
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    return http_send_error(req, 400, "no_body");
  }

  JsonDocument body;
  if (deserializeJson(body, content) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }

  int size = body["size"] | -1;
  if (size < 0 || size > FRAMESIZE_UXGA) {
    return http_send_error(req, 400, "invalid_resolution");
  }

  bool was_active = cam.isPeekActive();
  cam.setPeekActive(false);
  vTaskDelay(pdMS_TO_TICKS(100));

  bool success = cam.setResolution((framesize_t)size);

  if (was_active && success) {
    cam.setPeekActive(true);
  }

  JsonDocument doc;
  doc["ok"] = success;
  if (success) {
    doc["resolution"] = size;
    doc["resolution_name"] = cam.getResolutionName();
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Resolution changed", cam.getResolutionName());
  } else {
    doc["error"] = "Failed to set resolution";
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// PEEK SENSOR — GET /api/peek/sensor
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_peek_sensor_get(httpd_req_t* req) {
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  CameraManager& cam = camera_get_instance();
  if (!cam.isInitialized()) {
    return http_send_error(req, 503, "camera_not_initialized");
  }

  JsonDocument doc;
  if (!cam.getSensorParams(doc)) {
    return http_send_error(req, 500, "sensor_read_failed");
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// PEEK SENSOR — POST /api/peek/sensor
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_peek_sensor_set(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  CameraManager& cam = camera_get_instance();
  if (!cam.isInitialized()) {
    return http_send_error(req, 503, "camera_not_initialized");
  }

  if (req->content_len >= 512) {
    return http_send_error(req, 413, "payload_too_large");
  }

  char content[512] = {0};
  int total = 0;
  while (total < (int)sizeof(content) - 1) {
    int r = httpd_req_recv(req, content + total, sizeof(content) - 1 - total);
    if (r <= 0) break;
    total += r;
  }
  if (total <= 0) {
    return http_send_error(req, 400, "no_body");
  }

  JsonDocument body;
  if (deserializeJson(body, content) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }
  if (!body.is<JsonObject>()) {
    return http_send_error(req, 400, "body_must_be_object");
  }
  JsonObject obj = body.as<JsonObject>();

  // Apply a named preset if requested (overrides individual fields)
  if (obj["preset"].is<const char*>()) {
    const char* preset = obj["preset"].as<const char*>();
    if (cam.applyPreset(preset)) {
      log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Preset applied", preset);
    } else {
      return http_send_error(req, 400, "unknown_preset");
    }
  } else {
    cam.applySensorParams(obj);
  }
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Sensor params updated", nullptr);

  // Echo back current state so the UI doesn't need a second round-trip.
  JsonDocument doc;
  cam.getSensorParams(doc);

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// PEEK SNAPSHOT — GET /api/peek/snapshot
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_peek_snapshot(httpd_req_t* req) {
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  CameraManager& cam = camera_get_instance();
  if (!cam.isInitialized()) {
    return http_send_error(req, 503, "camera_not_initialized");
  }

  camera_fb_t* fb = cam.captureFrame();
  if (!fb) {
    return http_send_error(req, 500, "frame_capture_failed");
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=peek.jpg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  cam.returnFrame(fb);
  return res;
}
#endif

// ════════════════════════════════════════════════════════════════════════════
// EXPORT ENDPOINT
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_export(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  DeviceIdentity& device = witness_get_device();
  SystemHealth& health = witness_get_health();
  WitnessRecord& last = witness_get_last_record();

  JsonDocument doc;
  doc["ok"] = true;
  doc["version"] = PROTOCOL_VERSION;
  doc["device_id"] = device.device_id;
  doc["firmware"] = FIRMWARE_VERSION;
  doc["ruleset"] = RULESET_ID;
  doc["export_time_ms"] = millis();
  doc["chain_seq"] = device.seq;
  doc["key_at_rest"] = crypto_key_at_rest_label();
  doc["records_total"] = health.records_created;

  char pubkey_hex[65];
  hex_to_str(pubkey_hex, device.pubkey, 32);
  doc["pubkey"] = pubkey_hex;

  char chain_hex[65];
  hex_to_str(chain_hex, device.chain_head, 32);
  doc["chain_head"] = chain_hex;

  if (last.seq > 0) {
    JsonObject last_rec = doc["last_record"].to<JsonObject>();
    last_rec["seq"] = last.seq;
    char hash[65];
    hex_to_str(hash, last.chain_hash, 32);
    last_rec["hash"] = hash;
    last_rec["type"] = record_type_name(last.type);
    last_rec["verified"] = last.verified;
  }

  doc["sd_available"] = health.sd_healthy;

  log_health(LOG_LEVEL_INFO, LOG_CAT_USER, "Export bundle created", nullptr);

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// WIFI API ENDPOINTS
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_wifi_status(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  ScvNetworkManager& net = network_get_instance();
  const WiFiStatus& status = net.getStatus();

  JsonDocument doc;
  doc["ok"] = true;
  doc["state"] = ScvNetworkManager::stateName(status.state);
  doc["ap_active"] = status.ap_active;
  doc["sta_connected"] = status.sta_connected;
  doc["ap_ip"] = status.ap_ip;
  if (status.sta_connected) {
    doc["sta_ip"] = status.sta_ip;
    doc["rssi"] = status.rssi;
  }
  doc["ap_clients"] = status.ap_clients;
  // F16: what the SoftAP is actually broadcasting, and why (a core without
  // SoftAP SAE, or a driver refusal, stays WPA2 and says so here).
  doc["ap_auth"] = status.ap_auth[0] ? status.ap_auth : "unknown";
  doc["ap_auth_reason"] = status.ap_auth_reason ? status.ap_auth_reason : "";
  doc["sta_pmf"] = status.sta_pmf;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_wifi_scan(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  int n = WiFi.scanNetworks(false, false, false, 300);

  JsonDocument doc;
  doc["ok"] = true;
  JsonArray networks = doc["networks"].to<JsonArray>();

  for (int i = 0; i < n && i < 20; i++) {
    JsonObject net = networks.add<JsonObject>();
    net["ssid"] = WiFi.SSID(i);
    net["rssi"] = WiFi.RSSI(i);
    net["channel"] = WiFi.channel(i);
    // F16: the real auth mode ("open" stays "open": the setup page keys its
    // lock icon off exactly that value).
    net["encryption"] = canary::net::ap_security::label_for((int)WiFi.encryptionType(i));
  }

  WiFi.scanDelete();

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_wifi_connect(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  char body[384];  // ssid + password + tz_iana (F28); 256 left no room for the zone
  // The whole body or a refusal by name: one httpd_req_recv() returns what
  // one socket read delivered, so a body in two segments would parse as a
  // fragment (the loop /api/settings and the Scout routes use).
  if (req->content_len == 0) return http_send_error(req, 400, "empty_body");
  if (req->content_len >= sizeof(body)) return http_send_error(req, 413, "body_too_large");
  size_t total = 0;
  while (total < req->content_len) {
    const int r = httpd_req_recv(req, body + total, req->content_len - total);
    if (r <= 0) return http_send_error(req, 400, "empty_body");
    total += (size_t)r;
  }
  body[total] = '\0';

  JsonDocument input;
  if (deserializeJson(input, body) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }

  const char* ssid = input["ssid"];
  const char* password = input["password"];

  if (!ssid || strlen(ssid) == 0) {
    return http_send_error(req, 400, "missing_ssid");
  }

  // Household time zone seed (repo sweep F28): the setup page sends the
  // phone's own IANA zone. Mapped on the device; an unknown or absent zone
  // stores nothing and never fails the join, but the answer says so
  // ("tz": set | unknown_zone | not_set | not_sent) so the page can tell
  // the person their Canary is still on world time (UTC).
  const char* tz_iana = input["tz_iana"] | "";
  const char* tz_outcome = "not_sent";
  if (tz_iana[0] != '\0') {
    switch (setup_set_tz(nullptr, tz_iana)) {
      case 0:  tz_outcome = "set"; break;
      case 3:  tz_outcome = "unknown_zone"; break;
      default: tz_outcome = "not_set"; break;
    }
  }

  ScvNetworkManager& net = network_get_instance();
  WiFiCredentials creds;
  memset(&creds, 0, sizeof(creds));
  strncpy(creds.ssid, ssid, sizeof(creds.ssid) - 1);
  if (password) {
    strncpy(creds.password, password, sizeof(creds.password) - 1);
  }
  creds.enabled = true;
  creds.configured = true;

  // Transfer local credentials to the manager, then save and connect
  net.setCredentials(creds);
  net.saveCredentials();
  net.connectToHome();

  // Mark first-time setup as complete now that WiFi credentials are saved
  if (setup_is_active()) {
    setup_mark_complete();
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Connecting to WiFi...";
  doc["tz"] = tz_outcome;
  doc["ssid"] = ssid;

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_wifi_disconnect(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  WiFi.disconnect(false);

  ScvNetworkManager& net = network_get_instance();
  net.clearCredentials();

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Disconnected from home WiFi";

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// MQTT API ENDPOINTS
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_HA_MQTT
#include "securacv_mqtt.h"
#include "mqtt_tls_fields.h"  // save-time judgment of tls / fp / the CA — the shared decision, host-tested

// A refusal from the TLS field judgment: the API's error code plus the
// shared header's constant reason, so the response says exactly what the
// serial log would have said at connect. Never the pin, the PEM or a
// credential (the header cannot format them).
static esp_err_t send_tls_refusal(httpd_req_t* req, canary::net::mqtt_tls_fields::Verdict v,
                                  const canary::net::mqtt_tls::Decision& d) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = canary::net::mqtt_tls_fields::error_code(v);
  doc["reason"] = canary::net::mqtt_tls_fields::reason(v, d);
  String response;
  serializeJson(doc, response);
  httpd_resp_set_status(req, v == canary::net::mqtt_tls_fields::Verdict::CaTooLarge
                                 ? "413 Payload Too Large" : "400 Bad Request");
  witness_get_health().http_errors++;
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_mqtt_status(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  JsonDocument doc;
  doc["ok"] = true;
  doc["connected"] = mqtt_connected();

  MqttCredentials creds;
  if (mqtt_load_credentials(&creds)) {
    doc["enabled"] = creds.enabled;
    doc["host"] = creds.host;
    doc["port"] = creds.port;
    // Do not expose username/password in API response
  } else {
    doc["enabled"] = false;
    doc["configured"] = false;
  }

  // Broker transport: the provisioned mode, what the socket does with it,
  // and — when refused — the reason (a constant from the shared header).
  // Presence flags only for the CA and the pin: never their values.
  MqttTransportStatus tls;
  mqtt_transport_status(&tls);
  doc["tls_loaded"] = tls.loaded;
  doc["tls"] = tls.mode;
  doc["tls_mode"] = tls.mode_byte;
  doc["transport"] = tls.transport;
  if (!tls.allowed) doc["tls_reason"] = tls.reason;
  if (tls.warn_insecure) doc["tls_warning"] = canary::net::mqtt_tls::insecure_warning();
  MqttTlsCurrent cur;
  if (mqtt_tls_read_current(&cur)) {
    doc["ca_set"] = cur.ca_set;
    doc["fp_set"] = cur.fp_set;
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// POST /api/mqtt/config — host / port / username / password / enabled as
// before, plus two optional broker-TLS fields:
//   tls  0 plain, 1 CA-verified, 2 SHA-256 fingerprint pin, 3 lab (unverified, warns)
//   fp   the broker certificate's SHA-256 pin, any spelling the firmware
//        accepts ("AA:BB:...", "aabb...", spaces); "" forgets the stored pin
// A field the body does not name leaves its NVS key alone. The CA is not a
// field here — it has its own route (POST /api/mqtt/ca) because it does not
// fit this body. The pair is judged by mqtt_tls_fields::plan against what
// NVS already holds, with the shared decision, BEFORE anything is written:
// a body the firmware would refuse at connect (mode 1 with no CA uploaded,
// mode 2 with no pin here or stored, a malformed pin, an unknown mode) is a
// 400 with the header's own reason text, and NVS is untouched.
static esp_err_t handle_mqtt_config(httpd_req_t* req) {
  namespace tf = canary::net::mqtt_tls_fields;
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  // Body budget: the longest well-formed body is host 63 + username 31 +
  // password 63 (the MqttCredentials field widths) + port 5 + enabled +
  // tls 1 + fp 95 (the canonical pin) and the JSON framing around them —
  // about 340 bytes, so 512 holds it with room for escapes in the password.
  // Anything longer is not this API's body and is refused as such rather
  // than truncated into an "invalid_json".
  char body[512];
  if (req->content_len >= sizeof(body)) {
    return http_send_error(req, 413, "payload_too_large");
  }
  int total = 0;
  while (total < (int)sizeof(body) - 1) {
    int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
    if (r <= 0) break;
    total += r;
    if (total >= (int)req->content_len) break;
  }
  if (total <= 0) {
    return http_send_error(req, 400, "empty_body");
  }
  body[total] = '\0';

  JsonDocument input;
  if (deserializeJson(input, body) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }

  MqttCredentials creds;
  memset(&creds, 0, sizeof(creds));

  const char* host = input["host"];
  if (!host || strlen(host) == 0) {
    return http_send_error(req, 400, "missing_host");
  }

  strncpy(creds.host, host, sizeof(creds.host) - 1);
  creds.port = input["port"] | MQTT_PORT;

  const char* user = input["username"];
  if (user) strncpy(creds.username, user, sizeof(creds.username) - 1);

  const char* pass = input["password"];
  if (pass) strncpy(creds.password, pass, sizeof(creds.password) - 1);

  creds.enabled = input["enabled"] | true;
  creds.configured = true;

  // The optional TLS pair, typed strictly: a `tls` that is not an integer or
  // an `fp` that is not a string is the same refusal a bad value gets.
  tf::Request tls_req;
  if (!input["tls"].isNull()) {
    if (!input["tls"].is<int>()) return http_send_error(req, 400, "tls_mode_invalid");
    tls_req.has_mode = true;
    tls_req.mode = input["tls"].as<long>();
  }
  if (!input["fp"].isNull()) {
    if (!input["fp"].is<const char*>()) return http_send_error(req, 400, "fp_malformed");
    tls_req.fp = input["fp"].as<const char*>();
  }

  MqttTlsCurrent stored;
  if (!mqtt_tls_read_current(&stored)) {
    return http_send_error(req, 500, "nvs_read_failed");
  }
  tf::Current cur;
  cur.mode_byte = stored.mode_byte;
  cur.fp = stored.fp_set ? stored.fp : nullptr;
  cur.ca_set = stored.ca_set;

  tf::Plan tls_plan;
  const tf::Verdict verdict = tf::plan(cur, tls_req, tls_plan);
  if (verdict != tf::Verdict::Ok) {
    return send_tls_refusal(req, verdict, tls_plan.decision);
  }

  // One NVS session for the whole request — the TLS keys first, the
  // credentials last, one reload after the session closes (mqtt_save_config
  // walks mqtt_tls_fields::write_order, host-tested). Two sessions with the
  // credentials first would let the main task reconnect with the NEW
  // password on the OLD, plain socket in the gap between them, and close the
  // shared NVS handle under the second write.
  const MqttTlsWrite tls_write = {tls_plan.set_mode, tls_plan.mode, tls_plan.set_fp, tls_plan.fp,
                                  tls_plan.clear_fp};
  const bool any_tls = tls_plan.set_mode || tls_plan.set_fp || tls_plan.clear_fp;
  if (!mqtt_save_config(&creds, any_tls ? &tls_write : nullptr)) {
    return http_send_error(req, 500, "save_failed");
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "MQTT configuration saved. The Canary reconnects with these settings.";
  // What the socket will do with the saved settings — the same words the
  // serial log uses — so a caller sees "tls-ca" / "tls-fingerprint" / "plain"
  // come back, and the lab opt-in's warning with it.
  doc["transport"] = canary::net::mqtt_tls::transport_name(tls_plan.decision.transport);
  if (tls_plan.decision.warn_insecure()) doc["warning"] = canary::net::mqtt_tls::insecure_warning();

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// POST /api/mqtt/ca — the broker's CA certificate, PEM, as the RAW body
// (text/plain, not JSON: a 3 KB certificate is not escaped into a 6 KB
// string, and the 512-byte config body never has to hold it). Bounded to
// the firmware's kCaPemMax (3071), judged by the shared ca_pem_looks_valid()
// before it can reach NVS, stored as the NVS string BrokerTransport::load
// reads back (with the trailing newline the flashers write), never echoed.
// DELETE /api/mqtt/ca forgets the stored CA. Same auth gate and rate limit
// as every other mutating handler; the socket is re-decided on the main
// loop's next pass. Storing a CA does not switch the mode: POST
// /api/mqtt/config with tls=1 does, and is refused until this has landed.
static esp_err_t handle_mqtt_ca(httpd_req_t* req) {
  namespace tf = canary::net::mqtt_tls_fields;
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  if (req->method == HTTP_DELETE) {
    if (!mqtt_tls_clear_ca()) return http_send_error(req, 500, "save_failed");
    JsonDocument doc;
    doc["ok"] = true;
    doc["ca_set"] = false;
    doc["message"] = "Broker CA forgotten. A CA-verified mode now refuses to connect until a CA is uploaded again.";
    String response;
    serializeJson(doc, response);
    return http_send_json(req, response.c_str());
  }

  // One static receive buffer: the PEM (kCaPemMax at most, the '\n' this
  // handler appends counted inside it) plus the NUL — sized from the logic
  // header this file includes, not the Arduino transport header's
  // kCaBufBytes, which lives one include away. The HTTP server runs its
  // handlers on a single task, so a static is safe here and keeps 3 KB off
  // that task's stack. Wiped after use either way.
  static char s_ca_body[canary::net::mqtt_tls::kCaPemMax + 1];
  const canary::net::mqtt_tls::Decision none;
  if (req->content_len > canary::net::mqtt_tls::kCaPemMax) {
    return send_tls_refusal(req, tf::Verdict::CaTooLarge, none);
  }
  if (req->content_len == 0) {
    return http_send_error(req, 400, "empty_body");
  }
  size_t total = 0;
  while (total < req->content_len) {
    int r = httpd_req_recv(req, s_ca_body + total, req->content_len - total);
    if (r <= 0) break;
    total += (size_t)r;
  }
  if (total != req->content_len) {
    memset(s_ca_body, 0, sizeof(s_ca_body));
    return http_send_error(req, 400, "short_body");
  }
  // One trailing newline, exactly: strip what came, add ours, so the stored
  // bytes match what the flashers write and the cap counts the newline.
  while (total > 0 && (s_ca_body[total - 1] == '\n' || s_ca_body[total - 1] == '\r' ||
                       s_ca_body[total - 1] == ' ' || s_ca_body[total - 1] == '\t')) {
    total--;
  }
  if (total + 1 > canary::net::mqtt_tls::kCaPemMax) {
    memset(s_ca_body, 0, sizeof(s_ca_body));
    return send_tls_refusal(req, tf::Verdict::CaTooLarge, none);
  }
  s_ca_body[total++] = '\n';
  s_ca_body[total] = '\0';

  const tf::Verdict verdict = tf::check_ca(s_ca_body, total);
  if (verdict != tf::Verdict::Ok) {
    memset(s_ca_body, 0, sizeof(s_ca_body));
    return send_tls_refusal(req, verdict, none);
  }

  const bool saved = mqtt_tls_save_ca(s_ca_body);
  const size_t stored_bytes = total;
  memset(s_ca_body, 0, sizeof(s_ca_body));
  if (!saved) {
    return http_send_error(req, 500, "save_failed");
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["ca_set"] = true;
  doc["bytes"] = (uint32_t)stored_bytes;
  doc["message"] = "Broker CA saved. Set tls=1 on /api/mqtt/config and the Canary verifies the broker against it.";
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}
#endif // FEATURE_HA_MQTT

// ════════════════════════════════════════════════════════════════════════════
// CSI SENSING ENDPOINT — privacy-safe scalars + bar-graph arrays
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
// GET /api/sensing — returns the live aggregated sensing snapshot for the
// dashboard's Sensing panel. No raw subcarrier samples, no MAC/BSSID, no
// audio samples, no per-frame timestamps. The data exposed here is the
// distilled scalars + small int8 bar-graph arrays that the in-tree
// aggregator (securacv_sensing) builds from CSI feature callbacks and
// audio cadence-detector callbacks. Both are optional at compile time.
static esp_err_t handle_sensing(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  sensing_state_t s;
  sensing_snapshot(&s);

  JsonDocument doc;
  doc["ok"] = true;

#if FEATURE_CSI
  csi_stats_t stats = {0};
  csi_get_stats(&stats);
  doc["enabled"] = csi_is_running();

  // Headline scalars for the Apple-style status card.
  doc["motion"]    = s.motion_score;
  doc["breathing"] = s.breathing_score;
  doc["label"]     = sensing_label_name(s.activity_label);

  doc["rssi_dbm"] = (int)s.rssi_dbm;
  doc["rssi_std"] = (int)s.rssi_std;
  doc["frames_in_window"] = s.frames_in_window;
  doc["dropped_estimate"] = s.dropped_estimate;
  doc["channel"]         = s.channel;
  doc["bandwidth_code"]  = s.bandwidth_code;
  doc["time_bucket"]     = s.time_bucket;
  doc["windows_seen"]    = s.windows_seen;
  doc["last_window_age_ms"] =
      s.last_window_ms == 0 ? -1L : (long)(millis() - s.last_window_ms);

  // Bar-graph arrays — int8 buckets straight from the feature vector.
  JsonArray amp = doc["amp_bands"].to<JsonArray>();
  for (int i = 0; i < 8; i++) amp.add((int)s.amp_bands[i]);
  JsonArray dop = doc["doppler"].to<JsonArray>();
  for (int i = 0; i < 4; i++) dop.add((int)s.doppler[i]);
  JsonArray br = doc["breathing_bins"].to<JsonArray>();
  for (int i = 0; i < 8; i++) br.add((int)s.breathing_bins[i]);

  // Driver-level counters for the diagnostics tile.
  JsonObject st = doc["stats"].to<JsonObject>();
  st["frames_received"]     = stats.frames_received;
  st["frames_dropped_rssi"] = stats.frames_dropped_rssi;
  st["frames_dropped_rate"] = stats.frames_dropped_rate;
  st["frames_dropped_full"] = stats.frames_dropped_full;
  // Transmitter filter: frames from a transmitter other than the associated
  // router (neighbor beacons, other stations). A count, never an address.
  st["frames_dropped_foreign"] = stats.frames_dropped_foreign;
  // Armed = the filter is on AND has a BSSID to compare against (a held
  // BSSID alone outlives a config with the filter off).
  st["filter_foreign"]      = csi::get_filter_foreign();
  st["filter_armed"]        = csi::get_filter_foreign() && csi::has_associated_bssid();
  st["windows_emitted"]     = stats.windows_emitted;
  st["windows_degraded"]    = stats.windows_degraded;
  // Breathing-envelope cadence: how far the loop's real window pace was
  // from the 1 Hz grid the feature layer resamples onto.
  st["windows_held"]        = stats.windows_held;
  st["windows_merged"]      = stats.windows_merged;
  st["window_period_ms"]    = stats.window_period_ms;
#endif // FEATURE_CSI

#if FEATURE_ACOUSTIC_EVENTS
  audio_stats_t a_stats = {0};
  audio_get_stats(&a_stats);

  JsonObject ac = doc["acoustic"].to<JsonObject>();
  ac["enabled"]     = audio_is_running();
  ac["muted"]       = audio_is_muted();
  /* Flat-signal watchdog, mirrored from /api/audio/level: the always-
   * visible acoustic card is fed from THIS endpoint, so a dead data line
   * must be visible here too — not only inside the fold-out test panel. */
  ac["mic_silent"]  = (audio_is_running() &&
                       a_stats.zero_rms_streak >= AUDIO_SILENT_STREAK_FRAMES);
  ac["last_event"]  = audio_event_name(s.last_audio_event_type);
  ac["confidence"]  = s.last_audio_event_conf;
  ac["cycle_count"] = s.last_audio_event_count;
  ac["last_event_age_ms"] =
      s.last_audio_event_ms == 0 ? -1L : (long)(millis() - s.last_audio_event_ms);

  /* Last applied mute toggle — lets the dashboard show "Muted by Home
   * Assistant · 2 min ago" so the user can tell who flipped the mic.
   * source is 0=boot, 1=http (dashboard), 2=mqtt (Home Assistant). */
  audio_mute_info_t mi;
  audio_get_mute_info(&mi);
  if (mi.age_ms != UINT32_MAX) {
    ac["last_mute_source"]  = mi.source;
    ac["last_mute_age_ms"]  = (long)mi.age_ms;
  } else {
    ac["last_mute_source"]  = -1;
    ac["last_mute_age_ms"]  = -1L;
  }

  JsonObject ast = ac["stats"].to<JsonObject>();
  ast["frames_processed"] = a_stats.frames_processed;
  ast["on_transitions"]   = a_stats.on_transitions;
  ast["off_transitions"]  = a_stats.off_transitions;
  ast["t3_detected"]      = a_stats.t3_detected;
  ast["t4_detected"]      = a_stats.t4_detected;
  ast["i2s_read_errors"]  = a_stats.i2s_read_errors;
  #if FEATURE_ACOUSTIC_TRANSIENTS
  ast["knock_detected"]       = a_stats.knock_detected;
  ast["doorbell_detected"]    = a_stats.doorbell_detected;
  ast["glass_break_detected"] = a_stats.glass_break_detected;
  #endif
#endif // FEATURE_ACOUSTIC_EVENTS

#if FEATURE_TOUCH
  touch_stats_t t_stats = {0};
  touch_get_stats(&t_stats);

  JsonObject tc = doc["touch"].to<JsonObject>();
  tc["enabled"]     = touch_is_running();
  tc["last_event"]  = touch_event_name(s.last_touch_event_type);
  tc["confidence"]  = s.last_touch_event_conf;
  tc["pad_channel"] = s.last_touch_pad_channel;
  tc["last_event_age_ms"] =
      s.last_touch_event_ms == 0 ? -1L : (long)(millis() - s.last_touch_event_ms);
  tc["baseline_locked"] = t_stats.baseline_locked;
  tc["baseline_value"]  = t_stats.baseline_value;
  tc["last_value"]      = t_stats.last_value;

  JsonObject tst = tc["stats"].to<JsonObject>();
  tst["reads_total"]     = t_stats.reads_total;
  tst["panic_events"]    = t_stats.panic_events;
  tst["tamper_events"]   = t_stats.tamper_events;
  tst["approach_events"] = t_stats.approach_events;
#endif // FEATURE_TOUCH

#if FEATURE_IR_RMT
  ir_stats_t ir_stats = {0};
  ir_get_stats(&ir_stats);

  JsonObject ir_obj = doc["ir"].to<JsonObject>();
  ir_obj["enabled"]      = ir_is_running();
  ir_obj["last_protocol"] = ir_protocol_name(s.last_ir_category);
  ir_obj["hash_bucket"]  = s.last_ir_hash_bucket;
  ir_obj["confidence"]   = s.last_ir_confidence;
  ir_obj["last_event_age_ms"] =
      s.last_ir_event_ms == 0 ? -1L : (long)(millis() - s.last_ir_event_ms);

  JsonObject ist = ir_obj["stats"].to<JsonObject>();
  ist["frames_received"] = ir_stats.frames_received;
  ist["frames_decoded"]  = ir_stats.frames_decoded;
  ist["frames_unknown"]  = ir_stats.frames_unknown;
  ist["events_emitted"]  = ir_stats.events_emitted;
#endif // FEATURE_IR_RMT

#if FEATURE_TEMP_TAMPER
  envsens_stats_t e_stats = {0};
  envsens_get_stats(&e_stats);

  JsonObject ts = doc["temp"].to<JsonObject>();
  ts["enabled"]         = envsens_is_running();
  ts["confidence"]      = s.last_temp_drift_conf;
  ts["baseline_locked"] = e_stats.baseline_locked;
  ts["last_event_age_ms"] =
      s.last_temp_drift_ms == 0 ? -1L : (long)(millis() - s.last_temp_drift_ms);

  JsonObject est = ts["stats"].to<JsonObject>();
  est["samples_taken"] = e_stats.samples_taken;
  est["drift_events"]  = e_stats.drift_events;
  /* Whole-degree rounding only; never exposes raw temperature. */
  est["baseline_c"]    = (int)e_stats.baseline_c_rounded;
  est["last_c"]        = (int)e_stats.last_c_rounded;
#endif // FEATURE_TEMP_TAMPER

#if FEATURE_VISION_DETECT
  JsonObject vis = doc["vision"].to<JsonObject>();
  vis["enabled"]        = vision_is_running();
  const char* vtypes[] = {"none", "motion", "motion_end", "person", "tamper", "obj_removed"};
  vis["last_event"]     = vtypes[s.last_vision_event_type < 6 ? s.last_vision_event_type : 0];
  vis["confidence"]     = s.last_vision_confidence;
  vis["zone"]           = s.last_vision_zone;
  vis["last_event_age_ms"] =
      s.last_vision_event_ms == 0 ? -1L : (long)(millis() - s.last_vision_event_ms);

  vision_stats_t v_stats = {};
  vision_get_stats(&v_stats);
  JsonObject vst = vis["stats"].to<JsonObject>();
  vst["frames_analyzed"] = v_stats.frames_analyzed;
  vst["layer1_passes"]   = v_stats.layer1_passes;
  vst["layer2_passes"]   = v_stats.layer2_passes;
  vst["layer3_passes"]   = v_stats.layer3_passes;
  vst["motion_events"]   = v_stats.motion_events;
  vst["person_events"]   = v_stats.person_events;
  vst["motion_active"]   = v_stats.motion_active;
  vst["tamper_events"]   = v_stats.tamper_events;
  vst["tamper_active"]   = v_stats.tamper_active;
  vst["obj_removed_events"] = v_stats.obj_removed_events;

  JsonArray grid = vis["grid"].to<JsonArray>();
  for (int i = 0; i < VISION_GRID_TOTAL; i++) {
    grid.add(v_stats.block_intensity[i]);
  }

  vision_history_entry_t hist[VISION_HISTORY_SIZE];
  int hist_n = vision_get_history(hist, VISION_HISTORY_SIZE);
  if (hist_n > 0) {
    uint32_t now_ms = millis();
    const char* htypes[] = {"none", "motion", "motion_end", "person", "tamper", "obj_removed"};
    JsonArray events = vis["events"].to<JsonArray>();
    for (int i = 0; i < hist_n; i++) {
      JsonObject e = events.add<JsonObject>();
      e["type"] = htypes[hist[i].event_type < 6 ? hist[i].event_type : 0];
      e["confidence"] = hist[i].confidence;
      e["zone"] = hist[i].zone;
      e["age_ms"] = (long)(now_ms - hist[i].timestamp_ms);
    }
  }
#endif // FEATURE_VISION_DETECT

#if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
  /* Lowpower wake reason + capability bits — useful for the installer
   * to confirm the device booted from a touch-pad wake (forensic trail)
   * vs a normal cold boot, and for the dashboard to surface what wake
   * sources are available on the chip. */
  JsonObject lp = doc["lowpower"].to<JsonObject>();
  lp["wake_reason"] = lowpower_wake_reason_name(lowpower_get_wake_reason());
  lp["wake_touch_pad"] = lowpower_get_wake_touch_pad();
  lp["caps"] = lowpower_get_caps();
#endif

  char* response = (char*)malloc(3072);
  if (!response) return ESP_ERR_NO_MEM;
  serializeJson(doc, response, 3072);
  esp_err_t err = http_send_json(req, response);
  free(response);
  return err;
}
#endif // FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS

#if FEATURE_VISION_DETECT
// ════════════════════════════════════════════════════════════════════════════
// VISION CONFIG — GET/POST /api/vision/config
// ════════════════════════════════════════════════════════════════════════════

static void vision_config_to_json(JsonDocument& doc, const vision_config_t& cfg) {
  doc["ok"] = true;
  doc["jpeg_delta_pct"]         = cfg.jpeg_delta_pct;
  doc["block_change_pct"]       = cfg.block_change_pct;
  doc["person_confidence_min"]  = cfg.person_confidence_min;
  doc["luminance_threshold"]    = cfg.luminance_threshold;
  doc["process_interval_ms"]    = cfg.process_interval_ms;
  doc["motion_hold_ms"]         = cfg.motion_hold_ms;
  doc["layer3_cooldown_ms"]     = cfg.layer3_cooldown_ms;
  doc["sustained_backoff_ms"]   = cfg.sustained_backoff_ms;
  doc["sustained_threshold"]    = cfg.sustained_threshold;
  doc["duty_cycle_ms"]          = cfg.duty_cycle_ms;
  doc["duty_active_pct"]        = cfg.duty_active_pct;
  JsonArray mask = doc["zone_mask"].to<JsonArray>();
  for (int i = 0; i < 10; i++) mask.add(cfg.zone_mask[i]);
  JsonArray sens = doc["zone_sensitivity"].to<JsonArray>();
  for (int i = 0; i < VISION_GRID_TOTAL; i++) sens.add(cfg.zone_sensitivity[i]);
  doc["adaptive_enabled"]       = (int)cfg.adaptive_enabled;
  doc["tamper_hold_frames"]     = (int)cfg.tamper_hold_frames;
  doc["running"]                = vision_is_running();
#if FEATURE_VISION_TFLITE
  doc["tflite_available"]       = true;
#else
  doc["tflite_available"]       = false;
#endif
  vision_config_t n;
  bool match = vision_load_config_from_nvs(&n) &&
      n.jpeg_delta_pct        == cfg.jpeg_delta_pct &&
      n.block_change_pct      == cfg.block_change_pct &&
      n.person_confidence_min == cfg.person_confidence_min &&
      n.luminance_threshold   == cfg.luminance_threshold &&
      n.process_interval_ms   == cfg.process_interval_ms &&
      n.motion_hold_ms        == cfg.motion_hold_ms &&
      n.layer3_cooldown_ms    == cfg.layer3_cooldown_ms &&
      n.sustained_backoff_ms  == cfg.sustained_backoff_ms &&
      n.sustained_threshold   == cfg.sustained_threshold &&
      n.duty_cycle_ms         == cfg.duty_cycle_ms &&
      n.duty_active_pct       == cfg.duty_active_pct;
  for (int i = 0; match && i < 10; i++) match = n.zone_mask[i] == cfg.zone_mask[i];
  if (match) match = memcmp(cfg.zone_sensitivity, n.zone_sensitivity, sizeof(cfg.zone_sensitivity)) == 0;
  if (match) match = n.adaptive_enabled == cfg.adaptive_enabled && n.tamper_hold_frames == cfg.tamper_hold_frames;
  doc["saved"] = match;
}

static esp_err_t handle_vision_config_get(httpd_req_t* req) {
  if (!rate_limit_check(req, false)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  vision_config_t cfg;
  if (!vision_get_config(&cfg)) {
    return http_send_error(req, 503, "vision_not_initialized");
  }

  JsonDocument doc;
  vision_config_to_json(doc, cfg);

  char response[1024];
  serializeJson(doc, response, sizeof(response));
  return http_send_json(req, response);
}

static esp_err_t handle_vision_config_set(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  if (req->content_len >= 512) {
    return http_send_error(req, 413, "payload_too_large");
  }

  char content[512] = {0};
  int total = 0;
  while (total < (int)sizeof(content) - 1) {
    int r = httpd_req_recv(req, content + total, sizeof(content) - 1 - total);
    if (r <= 0) break;
    total += r;
  }
  if (total <= 0) {
    return http_send_error(req, 400, "no_body");
  }

  JsonDocument body;
  if (deserializeJson(body, content) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }
  if (!body.is<JsonObject>()) {
    return http_send_error(req, 400, "body_must_be_object");
  }
  JsonObject obj = body.as<JsonObject>();

  vision_config_t cfg;
  if (!vision_get_config(&cfg)) {
    return http_send_error(req, 503, "vision_not_initialized");
  }

  if (obj["reset"].is<bool>() && obj["reset"].as<bool>()) {
    vision_config_t defaults = VISION_CONFIG_DEFAULT;
    cfg = defaults;
  } else {
    if (obj["jpeg_delta_pct"].is<int>())
      cfg.jpeg_delta_pct = constrain(obj["jpeg_delta_pct"].as<int>(), 1, 100);
    if (obj["block_change_pct"].is<int>())
      cfg.block_change_pct = constrain(obj["block_change_pct"].as<int>(), 1, 100);
    if (obj["person_confidence_min"].is<int>())
      cfg.person_confidence_min = constrain(obj["person_confidence_min"].as<int>(), 1, 100);
    if (obj["luminance_threshold"].is<int>())
      cfg.luminance_threshold = constrain(obj["luminance_threshold"].as<int>(), 1, 255);
    if (obj["process_interval_ms"].is<int>())
      cfg.process_interval_ms = constrain(obj["process_interval_ms"].as<int>(), 50, 5000);
    if (obj["motion_hold_ms"].is<int>())
      cfg.motion_hold_ms = constrain(obj["motion_hold_ms"].as<int>(), 500, 30000);
    if (obj["layer3_cooldown_ms"].is<int>())
      cfg.layer3_cooldown_ms = constrain(obj["layer3_cooldown_ms"].as<int>(), 1000, 60000);
    if (obj["sustained_backoff_ms"].is<int>())
      cfg.sustained_backoff_ms = constrain(obj["sustained_backoff_ms"].as<int>(), 100, 10000);
    if (obj["sustained_threshold"].is<int>())
      cfg.sustained_threshold = constrain(obj["sustained_threshold"].as<int>(), 1, 255);
    if (obj["duty_cycle_ms"].is<int>())
      cfg.duty_cycle_ms = constrain(obj["duty_cycle_ms"].as<int>(), 1000, 60000);
    if (obj["duty_active_pct"].is<int>())
      cfg.duty_active_pct = constrain(obj["duty_active_pct"].as<int>(), 10, 100);
    if (obj["zone_mask"].is<JsonArray>()) {
      JsonArray zm = obj["zone_mask"].as<JsonArray>();
      for (int i = 0; i < 10 && i < (int)zm.size(); i++) {
        if (zm[i].is<int>()) cfg.zone_mask[i] = (uint8_t)zm[i].as<int>();
      }
    }
    if (obj["zone_sensitivity"].is<JsonArray>()) {
      JsonArray zs = obj["zone_sensitivity"].as<JsonArray>();
      for (int i = 0; i < VISION_GRID_TOTAL && i < (int)zs.size(); i++) {
        if (zs[i].is<int>()) cfg.zone_sensitivity[i] = (uint8_t)constrain(zs[i].as<int>(), 0, 255);
      }
    }
    if (obj["adaptive_enabled"].is<int>())
      cfg.adaptive_enabled = obj["adaptive_enabled"].as<int>() ? 1 : 0;
    if (obj["tamper_hold_frames"].is<int>())
      cfg.tamper_hold_frames = constrain(obj["tamper_hold_frames"].as<int>(), 5, 100);
  }

  vision_set_config(&cfg);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Vision config updated", nullptr);

  JsonDocument doc;
  vision_config_to_json(doc, cfg);

  char response[1024];
  serializeJson(doc, response, sizeof(response));
  return http_send_json(req, response);
}

static esp_err_t handle_vision_config_save(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  vision_config_t cfg;
  if (!vision_get_config(&cfg)) {
    return http_send_error(req, 503, "vision_not_initialized");
  }

  bool ok = vision_save_config_to_nvs();
  if (!ok) {
    return http_send_error(req, 500, "nvs_write_failed");
  }
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Vision config saved to NVS", nullptr);

  JsonDocument doc;
  vision_config_to_json(doc, cfg);

  char response[1024];
  serializeJson(doc, response, sizeof(response));
  return http_send_json(req, response);
}

static esp_err_t handle_vision_thumbnail(httpd_req_t* req) {
  if (!rate_limit_check(req, false)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  uint8_t buf[VISION_THUMB_W * VISION_THUMB_H];
  if (!vision_get_thumbnail(buf, sizeof(buf))) {
    return http_send_error(req, 503, "no_thumbnail");
  }

  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "X-Width", "40");
  httpd_resp_set_hdr(req, "X-Height", "30");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, (const char*)buf, sizeof(buf));
}
#endif // FEATURE_VISION_DETECT

#if FEATURE_ACOUSTIC_EVENTS
// ════════════════════════════════════════════════════════════════════════════
// AUDIO TESTABILITY + PRIVACY ENDPOINTS
//
// These exist so a user can (a) verify the mic is alive without setting off
// their actual smoke alarm and (b) turn the mic off at runtime in a way they
// can verify (the I2S driver is uninstalled and GPIO 41/42 are released).
//
// The "live level" endpoint exposes the SAME 20 ms RMS scalar the on/off
// hysteresis uses — not a new audio path. Self-test mode runs the existing
// T3/T4 matcher with relaxed timing tolerance and DOES NOT fire the normal
// event callback, so a TEST-button press never flows into Home Assistant.
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t handle_audio_level(httpd_req_t* req) {
  if (!rate_limit_check(req, false)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  uint16_t rms = 0;
  uint32_t age_ms = 0;
  const bool running = audio_get_live_level(&rms, &age_ms);

  /* Fetch the LIVE thresholds (which init() may have customized) rather
   * than the compile-time defaults — keeps the UI level-meter notches
   * accurate if a future build tunes them at runtime. */
  audio_config_t cfg = AUDIO_CONFIG_DEFAULT;
  audio_get_config(&cfg);

  JsonDocument doc;
  doc["ok"] = true;
  doc["running"] = running;
  doc["muted"]   = audio_is_muted();
  doc["rms"]     = rms;
  doc["rms_on_threshold"]  = cfg.rms_on_threshold;
  doc["rms_off_threshold"] = cfg.rms_off_threshold;
  doc["envelope_high"]     = (rms >= cfg.rms_on_threshold);
  doc["age_ms"] = (age_ms == UINT32_MAX) ? -1L : (long)age_ms;

  /* Flat-signal watchdog: running but every 20 ms window for 30+ s
   * computed RMS == 0 — a dead data line, not a quiet room (a live PDM
   * mic's noise floor never holds an exact zero that long). Lets the UI
   * warn instead of showing a healthy meter stuck at zero. */
  audio_stats_t a_stats;
  memset(&a_stats, 0, sizeof(a_stats));
  audio_get_stats(&a_stats);
  doc["mic_silent"] =
      (running && a_stats.zero_rms_streak >= AUDIO_SILENT_STREAK_FRAMES);

  /* Last 8 transitions, newest first, for the cadence-trace view.
   * `tone` is the alarm-band ratio ×100 the T3/T4 tone gate checks —
   * it shows WHY a beep did or didn't count (≥50 = alarm-band). */
  audio_transition_t trans[8];
  const size_t n = audio_get_recent_transitions(trans, 8, 0);
  JsonArray arr = doc["transitions"].to<JsonArray>();
  for (size_t i = 0; i < n; i++) {
    JsonObject e = arr.add<JsonObject>();
    e["on"]     = (bool)trans[i].is_on;
    e["age_ms"] = trans[i].age_ms;
    e["dur_ms"] = trans[i].dur_ms;
    e["tone"]   = trans[i].tone_x100;
  }

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_audio_mute(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  char body[64];
  const int recv = httpd_req_recv(req, body, sizeof(body) - 1);
  if (recv <= 0) return http_send_error(req, 400, "empty_body");
  body[recv] = '\0';

  JsonDocument input;
  if (deserializeJson(input, body) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }
  if (!input["muted"].is<bool>()) {
    return http_send_error(req, 400, "missing_muted_bool");
  }
  const bool want_muted = input["muted"].as<bool>();

  /* Apply at runtime. audio_mute(true) physically uninstalls I2S so the
   * GPIOs go tri-state — a user-verifiable hardware-level mute. The
   * source byte is recorded in the witness chain audit trail. */
  const bool ok = audio_mute(want_muted, AUDIO_MUTE_SOURCE_HTTP);
  if (!ok && !want_muted) {
    /* Unmute failed (I2S didn't come up). Still persist the user's
     * intent — they may have hardware issues we can't paper over. */
  }

  /* Persist user intent regardless of apply result, using the shared
   * helper so HTTP, MQTT, and any future control path stay in sync on
   * the NVS namespace + key. */
  const bool persisted = audio_save_mute_intent(want_muted);

  JsonDocument doc;
  doc["ok"] = true;
  doc["muted"] = audio_is_muted();
  doc["running"] = audio_is_running();
  doc["persisted"] = persisted;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_audio_test_start(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  if (audio_is_muted() || !audio_is_running()) {
    return http_send_error(req, 400, "mic_unavailable");
  }

  /* Body is optional: { "duration_ms": N } (clamped 5_000..60_000). */
  uint32_t duration_ms = 30000;
  char body[64] = {0};
  const int recv = httpd_req_recv(req, body, sizeof(body) - 1);
  if (recv > 0) {
    body[recv] = '\0';
    JsonDocument input;
    if (deserializeJson(input, body) == DeserializationError::Ok) {
      if (input["duration_ms"].is<uint32_t>()) {
        duration_ms = input["duration_ms"].as<uint32_t>();
      }
    }
  }
  if (duration_ms < 5000)  duration_ms = 5000;
  if (duration_ms > 60000) duration_ms = 60000;

  if (!audio_selftest_start(duration_ms)) {
    return http_send_error(req, 500, "selftest_start_failed");
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["duration_ms"] = duration_ms;
  doc["note"] = "Press your alarm's TEST button now. A match in this mode "
                "does NOT fire any Home Assistant automation.";
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_audio_test_status(httpd_req_t* req) {
  if (!rate_limit_check(req, false)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  audio_selftest_status_t st;
  audio_selftest_status(&st);

  /* Ship the live ON threshold alongside peak_rms so the failure copy can
   * distinguish "too quiet" (0 < peak < on) from "loud but not an alarm". */
  audio_config_t cfg = AUDIO_CONFIG_DEFAULT;
  audio_get_config(&cfg);

  JsonDocument doc;
  doc["ok"]               = true;
  doc["active"]           = (bool)st.active;
  doc["remaining_ms"]     = st.remaining_ms;
  doc["matched"]          = audio_event_name(st.matched_type);
  doc["confidence"]       = st.matched_conf;
  doc["transitions_seen"] = st.transitions_seen;
  doc["peak_rms"]         = st.peak_rms;
  doc["rms_on_threshold"] = cfg.rms_on_threshold;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}
#endif // FEATURE_ACOUSTIC_EVENTS

// ════════════════════════════════════════════════════════════════════════════
// DIAGNOSTICS DASHBOARD HANDLERS
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_DIAGNOSTICS

// GET /api/diagnostics — Full diagnostic snapshot as JSON
static esp_err_t handle_diagnostics(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  diag_snapshot_t snap;
  if (!diag_get_snapshot(&snap)) {
    return http_send_error(req, 500, "diagnostics_unavailable");
  }

  const char* degrade_name = "none";
  switch (snap.heap.degrade_level) {
    case DEGRADE_WARN:      degrade_name = "warn"; break;
    case DEGRADE_CRITICAL:  degrade_name = "critical"; break;
    case DEGRADE_EMERGENCY: degrade_name = "emergency"; break;
  }

  /* Build JSON with snprintf into a stack buffer. This avoids
   * ArduinoJson heap allocation during a diagnostics call when
   * memory pressure is the very thing being diagnosed. */
  char buf[2048];
  int pos = 0;

  /* heap */
  pos += snprintf(buf + pos, sizeof(buf) - pos,
    "{\"heap\":{\"free\":%u,\"min\":%u,\"largest_block\":%u,"
    "\"psram_free\":%u,\"psram_total\":%u,"
    "\"stack_hwm\":%u,\"fragmentation_pct\":%u,"
    "\"degrade_level\":\"%s\"},",
    snap.heap.free_heap, snap.heap.min_heap, snap.heap.largest_block,
    snap.heap.psram_free, snap.heap.psram_total,
    snap.heap.stack_hwm_main, snap.heap.fragmentation_pct,
    degrade_name);

  /* sd */
  pos += snprintf(buf + pos, sizeof(buf) - pos,
    "\"sd\":{\"mounted\":%s,\"usage_pct\":%u,"
    "\"total_writes\":%u,\"write_errors\":%u,"
    "\"space_warning\":%s,\"space_critical\":%s},",
    snap.sd.mounted ? "true" : "false",
    snap.sd.usage_pct, snap.sd.total_writes, snap.sd.write_errors,
    snap.sd.space_warning ? "true" : "false",
    snap.sd.space_critical ? "true" : "false");

  /* selftest */
  pos += snprintf(buf + pos, sizeof(buf) - pos,
    "\"selftest\":{\"has_run\":%s,\"health_score\":%u,"
    "\"passed\":%u,\"total\":%u,\"tests\":[",
    snap.selftest.has_run ? "true" : "false",
    snap.selftest.health_score,
    snap.selftest.passed_count, snap.selftest.total_count);

  for (uint8_t i = 0; i < snap.selftest.total_count && i < SELFTEST_COUNT; i++) {
    if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      "{\"name\":\"%s\",\"passed\":%s,\"ms\":%u}",
      snap.selftest.tests[i].name ? snap.selftest.tests[i].name : "unknown",
      snap.selftest.tests[i].passed ? "true" : "false",
      snap.selftest.tests[i].duration_ms);
    if ((size_t)pos >= sizeof(buf) - 64) break;  /* safety margin */
  }

  pos += snprintf(buf + pos, sizeof(buf) - pos, "]},");

  /* system */
  pos += snprintf(buf + pos, sizeof(buf) - pos,
    "\"system\":{\"uptime_sec\":%u,\"boot_count\":%u,"
    "\"reset_reason\":%u,\"firmware\":\"%s\"}}",
    snap.uptime_sec, snap.boot_count,
    snap.reset_reason, FIRMWARE_VERSION);

  return http_send_json(req, buf);
}

// GET /api/selftest — Re-run self-test and return results
static esp_err_t handle_selftest(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  /* Run self-test (blocks ~2-5 seconds). */
  uint8_t score = diag_run_selftest();

  selftest_report_t report;
  if (!diag_get_selftest(&report)) {
    return http_send_error(req, 500, "selftest_failed");
  }

  char buf[1024];
  int pos = 0;

  pos += snprintf(buf + pos, sizeof(buf) - pos,
    "{\"ok\":true,\"health_score\":%u,"
    "\"passed\":%u,\"total\":%u,\"tests\":[",
    score, report.passed_count, report.total_count);

  for (uint8_t i = 0; i < report.total_count && i < SELFTEST_COUNT; i++) {
    if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      "{\"name\":\"%s\",\"passed\":%s,\"ms\":%u}",
      report.tests[i].name ? report.tests[i].name : "unknown",
      report.tests[i].passed ? "true" : "false",
      report.tests[i].duration_ms);
    if ((size_t)pos >= sizeof(buf) - 64) break;
  }

  pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");

  return http_send_json(req, buf);
}

#endif // FEATURE_DIAGNOSTICS

#if FEATURE_POWER_MONITOR

// GET /api/battery/history — Battery health history from NVS
static esp_err_t handle_battery_history(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  power_state_t pwr;
  power_history_t hist;

  bool have_state = power_get_state(&pwr);
  bool have_hist  = power_get_history(&hist);

  if (!have_state && !have_hist) {
    return http_send_error(req, 500, "power_unavailable");
  }

  char buf[512];
  int pos = 0;

  pos += snprintf(buf + pos, sizeof(buf) - pos,
    "{\"ok\":true,\"charge_cycles\":%u,"
    "\"total_runtime_min\":%u,"
    "\"voltage_min_mv\":%u,"
    "\"voltage_max_mv\":%u,"
    "\"soc_min_pct\":%u,"
    "\"brownout_count\":%u,"
    "\"last_full_charge_ms\":%u,",
    have_hist ? hist.charge_cycles : (have_state ? pwr.charge_cycles : 0u),
    have_hist ? hist.total_runtime_min : 0u,
    have_hist ? (hist.voltage_min_mv == 0xFFFF ? 0u : (unsigned)hist.voltage_min_mv) :
                (have_state && pwr.min_voltage_mv != 0xFFFF ? (unsigned)pwr.min_voltage_mv : 0u),
    have_hist ? (unsigned)hist.voltage_max_mv :
                (have_state ? (unsigned)pwr.max_voltage_mv : 0u),
    have_hist ? (unsigned)hist.soc_min_pct : 100u,
    have_hist ? hist.brownout_count : 0u,
    have_hist ? hist.last_full_charge_ms : 0u);

  /* Current state for context. */
  if (have_state) {
    pos += snprintf(buf + pos, sizeof(buf) - pos,
      "\"current\":{\"voltage_mv\":%u,\"soc_pct\":%u,"
      "\"charge_state\":%u,\"trend_mv_per_min\":%d,"
      "\"samples_taken\":%u}}",
      pwr.voltage_mv, pwr.soc_pct,
      pwr.charge_state, pwr.trend_mv_per_min,
      pwr.samples_taken);
  } else {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\"current\":null}");
  }

  return http_send_json(req, buf);
}

#endif // FEATURE_POWER_MONITOR

#if FEATURE_THERMAL_WATCHDOG

/* Bounded JSON append: caps pos at the buffer end and turns further
 * writes into no-ops, so an oversized payload truncates instead of
 * underflowing `cap - pos` into a huge size for the next vsnprintf. */
static void thermal_json_append(char* buf, size_t cap, int* pos,
                                const char* fmt, ...) {
  if (*pos < 0 || (size_t)*pos >= cap) return;
  va_list ap;
  va_start(ap, fmt);
  int written = vsnprintf(buf + *pos, cap - (size_t)*pos, fmt, ap);
  va_end(ap);
  if (written <= 0) return;
  size_t rem = cap - (size_t)*pos;
  *pos += ((size_t)written < rem) ? written : (int)(rem - 1);
}

// GET /api/thermal — current die temp + lifetime thermal history.
// All data comes from the passive watchdog (always-on, NVS-persisted),
// so it stays fresh whether or not the camera is streaming.
static esp_err_t handle_thermal(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  thermal_wd_state_t st;
  thermal_wd_history_t hist;
  if (!thermal_wd_get_state(&st) || !thermal_wd_get_history(&hist)) {
    return http_send_error(req, 500, "thermal_unavailable");
  }

  const char* state_names[] = {"normal", "throttled", "paused"};
  uint8_t si = st.shadow_state <= 2 ? st.shadow_state : 0;

  char buf[768];
  int pos = 0;

  if (st.last_sample_ms != 0) {
    thermal_json_append(buf, sizeof(buf), &pos,
      "{\"ok\":true,\"die_temp_c\":%.1f,\"last_sample_age_ms\":%u,",
      st.die_temp_c, (unsigned)(millis() - st.last_sample_ms));
  } else {
    thermal_json_append(buf, sizeof(buf), &pos,
      "{\"ok\":true,\"die_temp_c\":null,\"last_sample_age_ms\":null,");
  }

  thermal_json_append(buf, sizeof(buf), &pos,
    "\"thermal_state\":\"%s\",\"sensor_ok\":%s,\"advisories\":[",
    state_names[si], st.sensor_ok ? "true" : "false");

  {
    static const struct { uint8_t bit; const char* name; } adv_map[] = {
      { THERMAL_ADV_SENSOR_FAULT, "sensor_fault" },
      { THERMAL_ADV_CRITICAL,     "critical" },
      { THERMAL_ADV_SATURATION,   "saturation" },
      { THERMAL_ADV_ENV_LIMITED,  "env_limited" },
      { THERMAL_ADV_COLD,         "cold" },
    };
    bool first = true;
    for (size_t i = 0; i < sizeof(adv_map) / sizeof(adv_map[0]); i++) {
      if (st.advisories & adv_map[i].bit) {
        thermal_json_append(buf, sizeof(buf), &pos, "%s\"%s\"",
                            first ? "" : ",", adv_map[i].name);
        first = false;
      }
    }
  }

  /* min=127 / max=-128 are the "never sampled" sentinels. */
  thermal_json_append(buf, sizeof(buf), &pos, "],\"history\":{");
  if (hist.alltime_min_c == 127 && hist.alltime_max_c == -128) {
    thermal_json_append(buf, sizeof(buf), &pos,
      "\"alltime_min_c\":null,\"alltime_max_c\":null,");
  } else {
    thermal_json_append(buf, sizeof(buf), &pos,
      "\"alltime_min_c\":%d,\"alltime_max_c\":%d,",
      (int)hist.alltime_min_c, (int)hist.alltime_max_c);
  }
  thermal_json_append(buf, sizeof(buf), &pos,
    "\"total_runtime_min\":%u,\"throttled_min\":%u,\"paused_min\":%u,"
    "\"throttle_events\":%u,\"pause_events\":%u,\"critical_events\":%u,"
    "\"sensor_fail_events\":%u,\"cold_events\":%u,\"max_seen_runtime_min\":%u},",
    hist.total_runtime_min, hist.throttled_min, hist.paused_min,
    hist.throttle_events, hist.pause_events, hist.critical_events,
    hist.sensor_fail_events, hist.cold_events, hist.max_seen_runtime_min);

  thermal_json_append(buf, sizeof(buf), &pos,
    "\"thresholds\":{\"throttle_c\":%d,\"pause_c\":%d,"
    "\"recover_margin_c\":%d,\"critical_c\":85,\"cold_c\":5}}",
    THERMAL_THROTTLE_TEMP_C, THERMAL_PAUSE_TEMP_C, THERMAL_RECOVER_MARGIN_C);

  return http_send_json(req, buf);
}

#endif // FEATURE_THERMAL_WATCHDOG

// ════════════════════════════════════════════════════════════════════════════
// SETTINGS — household time zone (repo sweep F28, option A)
//
//   GET  /api/settings  — {ok, tz, tz_iana}: "" while unset (the Canary keeps UTC)
//   POST /api/settings  — {tz: "<POSIX rule>"} or {tz_iana: "<IANA zone>"};
//                         {tz: ""} alone clears the zone (UTC again)
//
// Stored and applied by securacv_setup (setenv + tzset, never configTzTime),
// resolved through the shared table (common/time/tz_rule.h): a typed rule
// wins, an IANA name maps, an unknown zone or a rule outside the strict
// POSIX grammar (tz_rule::posix_valid) is refused by name and stores
// nothing. The CSI day offset picks the change up on the next loop pass
// (main.cpp updateCsiClockOffset).
// ════════════════════════════════════════════════════════════════════════════

static esp_err_t send_settings(httpd_req_t* req) {
  char tz[SETUP_TZ_MAX + 1];
  char tz_iana[SETUP_TZ_MAX + 1];
  if (!setup_get_tz(tz, sizeof(tz))) tz[0] = '\0';
  if (!setup_get_tz_iana(tz_iana, sizeof(tz_iana))) tz_iana[0] = '\0';

  JsonDocument doc;
  doc["ok"] = true;
  doc["tz"] = tz;
  doc["tz_iana"] = tz_iana;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_settings_get(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;
  return send_settings(req);
}

static esp_err_t handle_settings_post(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  char body[256];
  if (req->content_len == 0) return http_send_error(req, 400, "empty_body");
  if (req->content_len >= sizeof(body)) return http_send_error(req, 413, "body_too_large");
  size_t total = 0;
  while (total < req->content_len) {
    const int r = httpd_req_recv(req, body + total, req->content_len - total);
    if (r <= 0) return http_send_error(req, 400, "empty_body");
    total += (size_t)r;
  }
  body[total] = '\0';

  JsonDocument input;
  if (deserializeJson(input, body) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }
  const bool has_tz   = input["tz"].is<const char*>();
  const bool has_iana = input["tz_iana"].is<const char*>();
  if (!has_tz && !has_iana) return http_send_error(req, 400, "no_recognized_keys");
  const char* tz      = input["tz"] | "";
  const char* tz_iana = input["tz_iana"] | "";

  if (has_tz && tz[0] == '\0' && tz_iana[0] == '\0') {
    if (!setup_clear_tz()) return http_send_error(req, 500, "nvs_unavailable");
  } else {
    switch (setup_set_tz(tz, tz_iana)) {
      case 0:  break;
      case 3:  return http_send_error(req, 400, "unknown_zone");
      default: return http_send_error(req, 400, "bad_time_zone");
    }
  }
  return send_settings(req);
}

// ════════════════════════════════════════════════════════════════════════════
// BLE SCOUT PAIRING (repo sweep F27, option B — proximity pairing window)
//
//   GET  /api/scout               — paired beacons: [{hashed_id, label}]
//   POST /api/scout/pair/start    — {label, window_s<=60, rssi_min=-45}: arm
//   GET  /api/scout/pair/status   — the window: state, remaining_s, result
//   POST /api/scout/pair/cancel   — cancel an armed window
//   POST /api/scout/unpair        — {hashed_id}: forget a beacon
//
// No MAC crosses this API in either direction. The window is armed here and
// the pairing happens inside the NimBLE scan callback (ble_scout_on_advert:
// the first advert from an unpaired beacon at/above rssi_min), which hashes
// the MAC with the per-device key and discards it. hashed_id is that keyed
// hash as 32 lowercase hex characters — unlinkable to the same tag on any
// other device. Every access to the registry/window goes through
// ble_scout.cpp's portMUX; the NVS write of the registry blob happens later
// on the loop task (ble_scout_tick), never on this HTTP task.
// ════════════════════════════════════════════════════════════════════════════

#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN

// Read a small JSON body (the Scout bodies are < 128 bytes).
static bool scout_read_body(httpd_req_t* req, JsonDocument& input, esp_err_t* sent) {
  char body[192];
  if (req->content_len == 0) {
    *sent = http_send_error(req, 400, "empty_body");
    return false;
  }
  if (req->content_len >= sizeof(body)) {
    *sent = http_send_error(req, 413, "body_too_large");
    return false;
  }
  size_t total = 0;
  while (total < req->content_len) {
    const int r = httpd_req_recv(req, body + total, req->content_len - total);
    if (r <= 0) {
      *sent = http_send_error(req, 400, "empty_body");
      return false;
    }
    total += (size_t)r;
  }
  body[total] = '\0';
  if (deserializeJson(input, body) != DeserializationError::Ok) {
    *sent = http_send_error(req, 400, "invalid_json");
    return false;
  }
  return true;
}

static esp_err_t handle_scout_list(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  ble_scan::PairedBeacon snap[ble_scan::MAX_PAIRED_BEACONS];
  const size_t n = ble_scout::ble_scout_registry_snapshot(snap, ble_scan::MAX_PAIRED_BEACONS);

  JsonDocument doc;
  doc["ok"] = true;
  doc["count"] = (unsigned)n;
  doc["max"] = (unsigned)ble_scan::MAX_PAIRED_BEACONS;
  JsonArray arr = doc["beacons"].to<JsonArray>();
  for (size_t i = 0; i < n; ++i) {
    char hex[2 * ble_scan::HASHED_ID_LEN + 1];
    ble_scout::pairing::id_to_hex(snap[i].hashed_id, hex);
    JsonObject o = arr.add<JsonObject>();
    o["hashed_id"] = hex;
    o["label"] = snap[i].label;
  }
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static void scout_status_json(JsonDocument& doc, const ble_scout::pairing::Status& st) {
  doc["ok"] = true;
  doc["state"] = ble_scout::pairing::state_name(st.state);
  doc["label"] = st.label;
  doc["window_s"] = (unsigned)(st.window_ms / 1000u);
  doc["remaining_s"] = (unsigned)((st.remaining_ms + 999u) / 1000u);
  doc["rssi_min"] = (int)st.rssi_min;
  if (st.state == ble_scout::pairing::State::PAIRED) {
    char hex[2 * ble_scan::HASHED_ID_LEN + 1];
    ble_scout::pairing::id_to_hex(st.paired_id, hex);
    doc["hashed_id"] = hex;
  }
}

static esp_err_t handle_scout_pair_start(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  JsonDocument input;
  esp_err_t sent = ESP_OK;
  if (!scout_read_body(req, input, &sent)) return sent;

  const char* label = input["label"];
  const int window_s = input["window_s"] | 60;
  const int rssi_min = input["rssi_min"] | (int)ble_scout::pairing::DEFAULT_RSSI_MIN;
  if (window_s <= 0) {
    return http_send_error(req, 400, "bad_window");
  }
  // Over 60 s is clamped to 60 s (the window FSM clamps too; this keeps the
  // multiply from wrapping on an absurd value).
  const uint32_t window_ms = (uint32_t)(window_s > 60 ? 60 : window_s) * 1000u;

  const uint32_t now = millis();
  switch (ble_scout::ble_scout_pair_window_start(label, window_ms, rssi_min, now)) {
    case ble_scout::pairing::ArmResult::OK:            break;
    case ble_scout::pairing::ArmResult::BUSY:          return http_send_error(req, 409, "window_busy");
    case ble_scout::pairing::ArmResult::BAD_LABEL:     return http_send_error(req, 400, "bad_label");
    case ble_scout::pairing::ArmResult::REGISTRY_FULL: return http_send_error(req, 409, "registry_full");
    case ble_scout::pairing::ArmResult::NOT_READY:     return http_send_error(req, 503, "scout_not_ready");
  }

  JsonDocument doc;
  scout_status_json(doc, ble_scout::ble_scout_pair_window_status(now));
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_scout_pair_status(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  JsonDocument doc;
  scout_status_json(doc, ble_scout::ble_scout_pair_window_status(millis()));
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_scout_pair_cancel(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  const uint32_t now = millis();
  const bool canceled = ble_scout::ble_scout_pair_window_cancel(now);
  JsonDocument doc;
  scout_status_json(doc, ble_scout::ble_scout_pair_window_status(now));
  doc["canceled"] = canceled;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_scout_unpair(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  JsonDocument input;
  esp_err_t sent = ESP_OK;
  if (!scout_read_body(req, input, &sent)) return sent;

  uint8_t id[ble_scan::HASHED_ID_LEN];
  if (!ble_scout::pairing::id_from_hex(input["hashed_id"] | "", id)) {
    return http_send_error(req, 400, "bad_hashed_id");
  }
  if (!ble_scout::ble_scout_unpair(id)) {
    return http_send_error(req, 404, "not_paired");
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["count"] = (unsigned)ble_scout::ble_scout_count();
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

#endif // FEATURE_BLE_SCAN

// ════════════════════════════════════════════════════════════════════════════
// MESH / OPERA REST API (PR-8, F10)
//
// Twelve registrations, all auth-gated + rate-limited, all using the
// existing {ok:...} JSON convention via http_send_json / http_send_error:
//
//   GET    /api/mesh              — opera status (refreshOpera reads this)
//   GET    /api/mesh/peers        — peer list
//   POST   /api/mesh/pair/start   — begin pairing as the initiator (add another)
//   POST   /api/mesh/pair/join    — begin pairing as the joiner (new device)
//   POST   /api/mesh/pair/confirm — user confirmed the 6-digit code matches
//   POST   /api/mesh/pair/cancel  — abort an in-progress pairing
//   POST   /api/mesh/leave        — forget the opera + signed LEAVE_OPERA notify (F10)
//   POST   /api/mesh/name {name}  — rename this device's opera label, local only (F10)
//   POST   /api/mesh/enable {enabled} — mesh on/off, NVS-persisted (F10)
//   GET    /api/mesh/alerts       — received TAMPER_ALERT history (F10)
//   DELETE /api/mesh/alerts       — clear that history (counters keep counting) (F10)
//   POST   /api/mesh/remove {fingerprint} — drop a peer AND rotate opera_secret
//                                  (spec §5.6 PIO, F10-rekey option B; CRYPTO:
//                                  maintainer review + bench pending)
//
// The JSON-rendering for the GET endpoints lives in the pure mesh_api
// builders so the response shape stays under host-test coverage; CI's
// [env:full] leg compiles these handlers but cannot run them.
//
// Threading: mesh_session's state belongs to the main loop (loop() runs
// mesh_session::process()). The mutators — leave, name, enable, alerts
// DELETE, remove, and since F33 part 5 the four pairing routes (start, join,
// confirm, cancel) — therefore never run here: each handler hands ONE
// request to mesh_session's request slot and waits, bounded, for loop() to
// execute it (mesh_call below). The pragma after this comment makes a
// direct call to any of those nine a compile error in the rest of this
// file. The GET handlers only read.
//
// MAC↔fingerprint join: the persisted trusted-peer set keys on Ed25519
// pubkey (→ fingerprint), while the live transport peer table keys on
// MAC. mesh_session bridges them — it records the source MAC of every
// FULLY VERIFIED opera-authenticated frame against the sender's
// fingerprint (get_peer_links), so per-peer state / last_seen / rssi
// below are the transport table's real numbers once a peer has spoken
// this boot. A peer that has not yet sent a verified frame reports
// OFFLINE/never — best-effort by design, documented in
// spec/canary_mesh_network_v0.md §8. (The table itself is filled by
// mesh_session from each peer's persisted radio MAC — F33 part 1 — so a
// peer's entry exists from boot; the verified-frame MAC is what says it
// has actually been heard.)
// ════════════════════════════════════════════════════════════════════════════

#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK

// The main-loop-only mesh_session mutators (see "Threading" above): from
// here to the end of this file, naming one is a compile error. Reach them
// through mesh_call() / mesh_session::submit_request().
#pragma GCC poison leave_opera set_opera_name set_enabled clear_alerts remove_peer
#pragma GCC poison start_pairing_initiator start_pairing_joiner confirm_pairing_code cancel_pairing

static esp_err_t handle_mesh_status(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  const bool has_opera = mesh_session::has_opera();

  uint8_t opera_id[mesh_crypto::OPERA_ID_LEN];
  const bool have_id = mesh_session::get_opera_id(opera_id);

  char opera_name[mesh_pairing::MAX_OPERA_NAME_LEN + 1];
  mesh_session::get_opera_name(opera_name, sizeof(opera_name));

  const mesh_pairing::State pstate = mesh_session::pairing_state();
  const size_t peers_total  = mesh_session::trusted_peer_count();
  // Trusted peers heard this boot (verified frame) whose transport entry is
  // in the ACTIVE window. Not the raw transport table any more: since F33
  // the table holds every bound peer from boot, fresh entries start ACTIVE,
  // and a peer that has said nothing is not online.
  const size_t peers_online = mesh_session::online_peer_count();

  // alerts_received: verified TAMPER_ALERT frames from any peer this boot
  // (F10/F11 — counted only after signature + opera_id + replay checks).
  const uint32_t alerts_received = mesh_session::alerts_received();
  const uint32_t pairing_code    = mesh_session::pairing_confirmation_code();

  char body[512];
  if (!mesh_api::build_mesh_status_json(
          body, sizeof(body),
          mesh_session::is_enabled(), has_opera,
          have_id ? opera_id : nullptr,
          opera_name, pstate,
          peers_total, peers_online, alerts_received, pairing_code)) {
    return http_send_error(req, 500, "encode_failed");
  }
  return http_send_json(req, body);
}

static esp_err_t handle_mesh_peers(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  // Trusted peers are the durable membership set (pubkeys); liveness
  // comes from joining each fingerprint's verified-frame MAC
  // (mesh_session::get_peer_links) against the live transport table
  // (see section header + spec §8).
  uint8_t pubkeys[mesh_state::MAX_TRUSTED_PEERS * mesh_crypto::PUBKEY_LEN];
  size_t  count = 0;
  if (!mesh_state::load_trusted_peers(pubkeys, sizeof(pubkeys), &count)) {
    return http_send_error(req, 500, "load_failed");
  }
  if (count > mesh_state::MAX_TRUSTED_PEERS) count = mesh_state::MAX_TRUSTED_PEERS;

  mesh_session::PeerLink links[mesh_session::MAX_TRUSTED_PEERS];
  const size_t n_links = mesh_session::get_peer_links(
      links, sizeof(links) / sizeof(links[0]));

  mesh_transport::Peer live[16];
  const size_t n_live = mesh_transport::list_peers(
      live, sizeof(live) / sizeof(live[0]));

  const uint32_t now_ms = millis();

  mesh_api::PeerView views[mesh_state::MAX_TRUSTED_PEERS];
  for (size_t i = 0; i < count; ++i) {
    uint8_t fp[mesh_crypto::FINGERPRINT_LEN];
    mesh_crypto::compute_fingerprint(pubkeys + i * mesh_crypto::PUBKEY_LEN, fp);
    static const char kHex[] = "0123456789abcdef";
    for (size_t b = 0; b < mesh_crypto::FINGERPRINT_LEN; ++b) {
      views[i].fingerprint[2 * b]     = kHex[(fp[b] >> 4) & 0xF];
      views[i].fingerprint[2 * b + 1] = kHex[fp[b] & 0xF];
    }
    views[i].fingerprint[mesh_crypto::FINGERPRINT_LEN * 2] = '\0';
    views[i].name[0]      = '\0';          // best-effort: name unknown
    views[i].state        = "OFFLINE";     // until a verified frame joins it
    views[i].last_seen_sec = 0xFFFFFFFFu;  // "never" (UI shows 'never')
    views[i].rssi          = 0;
    views[i].alerts_received = 0;          // until the session has a link row

    // fp → last verified MAC → live transport entry. A peer that has
    // not sent a verified frame this boot, or whose MAC has left the
    // transport table, keeps the OFFLINE/never defaults above.
    for (size_t l = 0; l < n_links; ++l) {
      if (memcmp(links[l].fp, fp, mesh_crypto::FINGERPRINT_LEN) != 0) {
        continue;
      }
      // Per-peer alert attribution (F11) does not depend on liveness.
      views[i].alerts_received = links[l].alerts_received;
      if (!links[l].mac_known) break;
      for (size_t t = 0; t < n_live; ++t) {
        if (!live[t].in_use ||
            memcmp(live[t].mac, links[l].mac, mesh_transport::MESH_TRANSPORT_MAC_LEN) != 0) {
          continue;
        }
        switch (live[t].state) {
          case mesh_transport::PeerState::ACTIVE: views[i].state = "CONNECTED"; break;
          case mesh_transport::PeerState::STALE:  views[i].state = "STALE";     break;
          default:                                views[i].state = "OFFLINE";   break;
        }
        views[i].last_seen_sec = (now_ms - live[t].last_seen_ms) / 1000u;
        views[i].rssi          = live[t].rssi_dbm;
        break;
      }
      break;
    }
  }

  // Sized for 8 worst-case rows (host-test pinned, mesh_api.h). 1024 held
  // the pre-F11 row; the alerts_received field needs the headroom.
  char body[mesh_api::PEERS_JSON_CAP];
  if (!mesh_api::build_mesh_peers_json(body, sizeof(body), views, count)) {
    return http_send_error(req, 500, "encode_failed");
  }
  return http_send_json(req, body);
}

static bool mesh_call(httpd_req_t* req, const mesh_session::Request& r,
                      mesh_session::RequestResult* out, esp_err_t* rc);

// The pairing routes' refusals from the main loop (F33 part 5): true, with
// the error response sent through *rc, for any status but OK.
static bool mesh_pair_refused(httpd_req_t* req, mesh_session::RequestStatus st,
                              const char* refused_code, esp_err_t* rc) {
  switch (st) {
    case mesh_session::RequestStatus::OK:
      return false;
    case mesh_session::RequestStatus::MESH_DISABLED:
      *rc = http_send_error(req, 400, "mesh_disabled");
      return true;
    case mesh_session::RequestStatus::REKEY_IN_FLIGHT:
      // Pairing during a secret rotation would hand the joiner the secret
      // being retired, or overwrite the one about to arrive (review fix).
      *rc = http_send_error(req, 409, "rekey_in_flight");
      return true;
    default:
      *rc = http_send_error(req, 400, refused_code);
      return true;
  }
}

static esp_err_t handle_mesh_pair_start(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  // Flash-encryption gate: refuse to touch the opera_secret on FE-off
  // hardware (matches mesh_state's load/save posture).
  if (!esp_flash_encryption_enabled()) {
    return http_send_error(req, 400, "no_flash_encryption");
  }

  // "Add another" — an opera already exists. Load its secret straight
  // from NVS into the request, hand it to the main loop, then zero this
  // copy (the session wipes its own). If no opera is persisted, the main
  // loop founds one first (spec §5.4, F33 part 4 — canary-wap's
  // create-on-start, behind the gates above): it draws the secret, has it
  // persisted before anything uses it, and never replaces an opera the
  // session already holds (opera_exists).
  mesh_session::Request r;
  memset(&r, 0, sizeof(r));
  r.type = mesh_session::RequestType::PAIR_START;
  r.create = !mesh_state::load_opera_secret(r.opera_secret);
  mesh_session::RequestResult res;
  esp_err_t rc = ESP_OK;
  const bool ran = mesh_call(req, r, &res, &rc);
  volatile uint8_t* z = r.opera_secret;   // regardless of outcome
  for (size_t i = 0; i < sizeof(r.opera_secret); ++i) z[i] = 0;
  if (!ran) return rc;
  if (res.status == mesh_session::RequestStatus::OPERA_EXISTS) {
    // A join landed after the load above, or NVS would not give back the
    // secret the session holds: never found a second opera over it.
    return http_send_error(req, 409, "opera_exists");
  }
  if (res.status == mesh_session::RequestStatus::NOT_PERSISTED) {
    return http_send_error(req, 500, "opera_not_persisted");
  }
  if (res.created) {
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Opera created", nullptr);
  }
  if (mesh_pair_refused(req, res.status, "pair_start_failed", &rc)) return rc;

  JsonDocument doc;
  doc["ok"] = true;
  doc["created"] = res.created;
  doc["state"] = "PAIRING_INIT";
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_mesh_pair_join(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  // Flash-encryption gate: the joiner will receive + persist the
  // opera_secret on success, so refuse on FE-off hardware up front.
  if (!esp_flash_encryption_enabled()) {
    return http_send_error(req, 400, "no_flash_encryption");
  }

  mesh_session::Request r;
  memset(&r, 0, sizeof(r));
  r.type = mesh_session::RequestType::PAIR_JOIN;
  mesh_session::RequestResult res;
  esp_err_t rc = ESP_OK;
  if (!mesh_call(req, r, &res, &rc)) return rc;
  if (mesh_pair_refused(req, res.status, "pair_join_failed", &rc)) return rc;

  JsonDocument doc;
  doc["ok"] = true;
  doc["state"] = "PAIRING_JOIN";
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_mesh_pair_confirm(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  mesh_session::Request r;
  memset(&r, 0, sizeof(r));
  r.type = mesh_session::RequestType::PAIR_CONFIRM;
  mesh_session::RequestResult res;
  esp_err_t rc = ESP_OK;
  if (!mesh_call(req, r, &res, &rc)) return rc;
  if (res.status != mesh_session::RequestStatus::OK) {
    return http_send_error(req, 400, "confirm_failed");
  }

  JsonDocument doc;
  doc["ok"] = true;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_mesh_pair_cancel(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  mesh_session::Request r;
  memset(&r, 0, sizeof(r));
  r.type = mesh_session::RequestType::PAIR_CANCEL;
  mesh_session::RequestResult res;
  esp_err_t rc = ESP_OK;
  if (!mesh_call(req, r, &res, &rc)) return rc;

  JsonDocument doc;
  doc["ok"] = true;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// Run one mesh mutation on the main loop (review fix; F33 part 5 added the
// four pairing routes). mesh_session's state belongs to the task that runs
// mesh_session::process() — loop() — and the httpd task must not touch it,
// so these handlers validate what they can locally, submit ONE request into
// mesh_session's one-deep slot and wait:
// loop() executes it inside its next mesh_session::process() pass, which a
// healthy main loop reaches within milliseconds. Returns true with *out
// filled; false after sending the error response itself (409 mesh_busy
// when another request holds the slot, 503 mesh_timeout when the main
// loop did not get to it — withdrawn, so it never runs).
static constexpr uint32_t kMeshCallStepMs = 10;
static constexpr uint32_t kMeshCallWaitMs = 3000;

static bool mesh_call(httpd_req_t* req, const mesh_session::Request& r,
                      mesh_session::RequestResult* out, esp_err_t* rc) {
  if (!mesh_session::submit_request(r)) {
    *rc = http_send_error(req, 409, "mesh_busy");
    return false;
  }
  for (uint32_t waited = 0; waited < kMeshCallWaitMs; waited += kMeshCallStepMs) {
    if (mesh_session::take_request_result(out)) return true;
    vTaskDelay(pdMS_TO_TICKS(kMeshCallStepMs));
  }
  if (!mesh_session::withdraw_request()) {
    // The main loop took it just now: its result lands within that same
    // process() call. Wait once more, then give up for good.
    for (uint32_t waited = 0; waited < kMeshCallWaitMs; waited += kMeshCallStepMs) {
      if (mesh_session::take_request_result(out)) return true;
      vTaskDelay(pdMS_TO_TICKS(kMeshCallStepMs));
    }
    mesh_session::abandon_request();
  }
  *rc = http_send_error(req, 503, "mesh_timeout");
  return false;
}

// POST /api/mesh/leave — forget this device's opera (F10). The session
// signs a LEAVE_OPERA under the opera it is leaving (best effort — the
// survivors drop only this device's trust entry), then wipes its RAM
// state and the radio peer table, on the main loop; the NVS copies go
// here. No rekey: the leaver discards its own secret, and a signed LEAVE
// can only remove its signer. Refused (409) while a secret rotation runs —
// leaving mid-rotation would split the survivors.
static esp_err_t handle_mesh_leave(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  mesh_session::Request r;
  memset(&r, 0, sizeof(r));
  r.type = mesh_session::RequestType::LEAVE;
  mesh_session::RequestResult res;
  esp_err_t rc = ESP_OK;
  if (!mesh_call(req, r, &res, &rc)) return rc;
  if (res.status == mesh_session::RequestStatus::REKEY_IN_FLIGHT) {
    return http_send_error(req, 409, "rekey_in_flight");
  }
  const bool notified = res.notified;
  // Each clear is idempotent; AND them so a real NVS failure is reported
  // rather than hidden behind a local wipe that did happen. replay_ctrs is
  // deliberately NOT cleared: the counters are replay defense, not
  // membership — the session keeps them as tombstones so a later re-pair
  // into this opera cannot be fed the peers' old frames, and the entries
  // already in NVS restore as exactly those tombstones at the next boot.
  bool cleared = mesh_state::clear_opera_secret();
  cleared = mesh_state::clear_trusted_peers()   && cleared;
  cleared = mesh_state::clear_elected_hub()     && cleared;
  cleared = mesh_state::clear_opera_name()      && cleared;
  log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "Left opera",
             notified ? "peers notified" : "no peer took the LEAVE frame");

  JsonDocument doc;
  doc["ok"] = true;
  doc["notified"] = notified;
  doc["persisted"] = cleared;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// POST /api/mesh/name {name} — this device's label for its opera (F10).
// Local only: nothing is sent to peers. Printable ASCII, 1..32 bytes.
// Persisted through the flash-encryption gate; on an FE-off board the
// rename holds until reboot and the response says persisted:false.
static esp_err_t handle_mesh_name(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  char body[128];
  const int recv = httpd_req_recv(req, body, sizeof(body) - 1);
  if (recv <= 0) return http_send_error(req, 400, "empty_body");
  body[recv] = '\0';

  JsonDocument input;
  if (deserializeJson(input, body) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }
  if (!input["name"].is<const char*>()) {
    return http_send_error(req, 400, "missing_name");
  }
  const char* name = input["name"].as<const char*>();
  const size_t len = strnlen(name, mesh_pairing::MAX_OPERA_NAME_LEN + 1);
  if (len == 0 || len > mesh_pairing::MAX_OPERA_NAME_LEN) {
    return http_send_error(req, 400, "invalid_name");
  }
  for (size_t i = 0; i < len; ++i) {
    const unsigned char c = (unsigned char)name[i];
    if (c < 0x20 || c > 0x7E) return http_send_error(req, 400, "invalid_name");
  }

  mesh_session::Request r;
  memset(&r, 0, sizeof(r));
  r.type = mesh_session::RequestType::SET_NAME;
  memcpy(r.name, name, len);   // len <= MAX_OPERA_NAME_LEN; r.name[len] stays '\0'
  mesh_session::RequestResult res;
  esp_err_t rc = ESP_OK;
  if (!mesh_call(req, r, &res, &rc)) return rc;
  if (res.status == mesh_session::RequestStatus::NO_OPERA) {
    return http_send_error(req, 400, "no_opera");
  }
  const bool persisted = mesh_state::save_opera_name(r.name);

  JsonDocument doc;
  doc["ok"] = true;
  doc["persisted"] = persisted;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// POST /api/mesh/enable {enabled} — mesh on/off (F10). Disabling stops
// the session (and cancels a pairing in flight) without leaving the
// opera; the choice is persisted (NVS "mesh_enabled", a preference, not
// FE-gated) and re-applied at boot.
static esp_err_t handle_mesh_enable(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  char body[64];
  const int recv = httpd_req_recv(req, body, sizeof(body) - 1);
  if (recv <= 0) return http_send_error(req, 400, "empty_body");
  body[recv] = '\0';

  JsonDocument input;
  if (deserializeJson(input, body) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }
  if (!input["enabled"].is<bool>()) {
    return http_send_error(req, 400, "missing_enabled_bool");
  }
  const bool want = input["enabled"].as<bool>();

  mesh_session::Request r;
  memset(&r, 0, sizeof(r));
  r.type    = mesh_session::RequestType::SET_ENABLED;
  r.enabled = want;
  mesh_session::RequestResult res;
  esp_err_t rc = ESP_OK;
  if (!mesh_call(req, r, &res, &rc)) return rc;
  // A rotation cannot finish while the mesh is off; refuse rather than
  // strand the survivors (F10-rekey). It ends within 60 s either way.
  if (res.status == mesh_session::RequestStatus::REKEY_IN_FLIGHT) {
    return http_send_error(req, 409, "rekey_in_flight");
  }
  const bool persisted = mesh_state::save_mesh_enabled(want);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK,
             want ? "Mesh enabled" : "Mesh disabled", "via /api/mesh/enable");

  JsonDocument doc;
  doc["ok"] = true;
  doc["enabled"] = res.enabled;
  doc["persisted"] = persisted;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

// GET /api/mesh/alerts — the received-alert history, newest first (F10).
static esp_err_t handle_mesh_alerts(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  static_assert(mesh_session::MAX_ALERT_HISTORY <= mesh_api::MAX_ALERTS_JSON,
                "ALERTS_JSON_CAP is pinned for MAX_ALERTS_JSON rows");
  mesh_alert::Record recs[mesh_session::MAX_ALERT_HISTORY];
  const size_t n = mesh_session::get_alerts(recs, mesh_session::MAX_ALERT_HISTORY);

  // Worst-case body (host-test pinned, mesh_api.h) — heap, not the httpd
  // task's stack.
  const size_t cap = mesh_api::ALERTS_JSON_CAP;
  char* body = (char*)malloc(cap);
  if (body == nullptr) return http_send_error(req, 500, "oom");
  esp_err_t rc;
  // millis(): the uptime each alert's timestamp_ms is measured against —
  // the web UI shows an age, never a date (F33 part 7).
  if (!mesh_api::build_mesh_alerts_json(body, cap, recs, n, (uint32_t)millis())) {
    rc = http_send_error(req, 500, "encode_failed");
  } else {
    rc = http_send_json(req, body);
  }
  free(body);
  return rc;
}

// DELETE /api/mesh/alerts — clear the history (F10), on the main loop that
// writes it. The per-peer and opera-wide alerts_received counters keep
// counting for the boot (canary-wap parity).
static esp_err_t handle_mesh_alerts_clear(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  mesh_session::Request r;
  memset(&r, 0, sizeof(r));
  r.type = mesh_session::RequestType::CLEAR_ALERTS;
  mesh_session::RequestResult res;
  esp_err_t rc = ESP_OK;
  if (!mesh_call(req, r, &res, &rc)) return rc;
  return http_send_json(req, "{\"ok\":true}");
}

// POST /api/mesh/remove {fingerprint} — drop a peer AND rotate
// opera_secret (spec §5.6; F10-rekey option B). The session starts the
// rotation first and forgets the peer only if it started; the survivors
// get the new secret over an ephemeral-X25519 exchange inside signed
// envelopes and this device commits on all ACKs or at the 60 s timeout
// (the rekey-commit handler re-persists it). rekey:"committed" means there
// was nobody left to tell and the secret rotated locally at once. Runs on
// the main loop (mesh_call); refused while a pairing runs.
// CRYPTO: maintainer review required before merge; bench-gated U1 C3.
static esp_err_t handle_mesh_remove(httpd_req_t* req) {
  if (!rate_limit_check(req, true)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  char body[96];
  const int recv = httpd_req_recv(req, body, sizeof(body) - 1);
  if (recv <= 0) return http_send_error(req, 400, "empty_body");
  body[recv] = '\0';

  JsonDocument input;
  if (deserializeJson(input, body) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }
  if (!input["fingerprint"].is<const char*>()) {
    return http_send_error(req, 400, "missing_fingerprint");
  }
  mesh_session::Request r;
  memset(&r, 0, sizeof(r));
  r.type = mesh_session::RequestType::REMOVE;
  if (!mesh_api::parse_fingerprint_hex(input["fingerprint"].as<const char*>(), r.fp)) {
    return http_send_error(req, 400, "invalid_fingerprint");
  }

  mesh_session::RequestResult res;
  esp_err_t rc = ESP_OK;
  if (!mesh_call(req, r, &res, &rc)) return rc;
  const mesh_session::RemoveResult rr = res.remove;
  switch (rr) {
    case mesh_session::RemoveResult::STARTED:
    case mesh_session::RemoveResult::COMMITTED:
      break;
    case mesh_session::RemoveResult::MESH_DISABLED:  return http_send_error(req, 400, "mesh_disabled");
    case mesh_session::RemoveResult::NO_OPERA:  return http_send_error(req, 400, "no_opera");
    case mesh_session::RemoveResult::NOT_FOUND: return http_send_error(req, 404, "unknown_peer");
    case mesh_session::RemoveResult::IN_FLIGHT: return http_send_error(req, 409, "rekey_in_flight");
    case mesh_session::RemoveResult::PAIRING:   return http_send_error(req, 409, "pairing_in_progress");
    default:                                    return http_send_error(req, 500, "rekey_failed");
  }
  // The removed peer's own NVS entry (FE-gated), dropped now so a reboot
  // mid-rotation does not bring it back; the rotation's commit drops it
  // again (idempotent) before it persists the new secret.
  const bool persisted = mesh_state::remove_trusted_peer(res.removed_pubkey);
  log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "Opera peer removed",
             rr == mesh_session::RemoveResult::COMMITTED ? "secret rotated locally"
                                                         : "secret rotation started");

  JsonDocument doc;
  doc["ok"] = true;
  doc["rekey"] = (rr == mesh_session::RemoveResult::COMMITTED) ? "committed" : "started";
  doc["persisted"] = persisted;
  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

#endif // FEATURE_MESH_NETWORK

// ════════════════════════════════════════════════════════════════════════════
// CONVENIENCE FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

bool network_init(const char* ap_ssid, const char* ap_password,
                  const char* device_id) {
  return network_get_instance().begin(ap_ssid, ap_password, device_id);
}

bool network_start_http() {
  return network_get_instance().startHttpServer();
}

void network_update() {
  network_get_instance().checkConnection();
}

httpd_handle_t network_get_http_server() {
  return network_get_instance().getHttpServer();
}

#endif // FEATURE_WIFI_AP || FEATURE_HTTP_SERVER
