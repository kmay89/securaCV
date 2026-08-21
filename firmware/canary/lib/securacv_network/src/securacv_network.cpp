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

#if FEATURE_SD_STORAGE
#include "securacv_storage.h"
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

// Mesh REST API (PR-8). Gated on FEATURE_MESH_NETWORK — the dev/release
// CI envs build with this OFF, so these handlers get no CI compile
// coverage; the JSON-building logic is therefore factored into the pure
// mesh_api builders, which the securacv_mesh host tests exercise.
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
                              status_code == 503 ? "503 Service Unavailable" :
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

// Forward declarations for HTTP handlers
static esp_err_t handle_ui(httpd_req_t* req);
// Captive-portal probes + first-boot setup wizard (see the CAPTIVE-PORTAL
// section below for the per-platform strategy).
static esp_err_t handle_captive_probe(httpd_req_t* req);
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

#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
// Mesh / opera REST API (PR-8). Six endpoints only — status, peers, and
// the four pairing steps. remove/leave/name/enable/alerts-DELETE are
// deferred (see spec/canary_mesh_network_v0.md §8).
static esp_err_t handle_mesh_status(httpd_req_t* req);
static esp_err_t handle_mesh_peers(httpd_req_t* req);
static esp_err_t handle_mesh_pair_start(httpd_req_t* req);
static esp_err_t handle_mesh_pair_join(httpd_req_t* req);
static esp_err_t handle_mesh_pair_confirm(httpd_req_t* req);
static esp_err_t handle_mesh_pair_cancel(httpd_req_t* req);
#endif

bool ScvNetworkManager::startHttpServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.stack_size = 8192;
  // 42 base (incl. /api/witness + /api/thermal) + 8 captive-portal routes
  // (6 OS connectivity probes + /setup + the wildcard fallback) + 6 mesh
  // endpoints (PR-8) when the mesh feature is compiled in. Each registered
  // httpd_uri_t needs a slot.
  #if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
  config.max_uri_handlers = 56;
  #else
  config.max_uri_handlers = 50;
  #endif
  config.recv_wait_timeout = 30;
  config.send_wait_timeout = 30;
  config.lru_purge_enable  = true;

  if (httpd_start(&m_http_server, &config) != ESP_OK) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_NETWORK, "HTTP server start failed", nullptr);
    return false;
  }

  registerHttpHandlers();
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "HTTP server started", "port 80");
  return true;
}

void ScvNetworkManager::stopHttpServer() {
  if (m_http_server) {
    httpd_stop(m_http_server);
    m_http_server = nullptr;
  }
}

void ScvNetworkManager::registerHttpHandlers() {
  // UI
  httpd_uri_t ui = { .uri = "/", .method = HTTP_GET, .handler = handle_ui };
  httpd_register_uri_handler(m_http_server, &ui);

  // First-boot setup wizard + the OS captive-portal connectivity probes.
  // Registered with a trailing '*' because probe URLs sometimes carry a
  // cache-busting query and the wildcard matcher compares the FULL uri;
  // handle_captive_probe re-checks the exact path component itself.
  httpd_uri_t setup_page = { .uri = "/setup", .method = HTTP_GET, .handler = handle_setup_page };
  httpd_register_uri_handler(m_http_server, &setup_page);
  static const char* kProbePaths[] = {
    "/hotspot-detect.html*",        // Apple CNA
    "/library/test/success.html*",  // Apple (older probe)
    "/generate_204*",               // Android
    "/gen_204*",                    // Android (short variant)
    "/connecttest.txt*",            // Windows NCSI
    "/ncsi.txt*",                   // Windows NCSI (legacy)
  };
  for (const char* p : kProbePaths) {
    httpd_uri_t probe = { .uri = p, .method = HTTP_GET, .handler = handle_captive_probe };
    httpd_register_uri_handler(m_http_server, &probe);
  }

  // API endpoints
  httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = handle_status };
  httpd_register_uri_handler(m_http_server, &status);

  httpd_uri_t chain = { .uri = "/api/chain", .method = HTTP_GET, .handler = handle_chain };
  httpd_register_uri_handler(m_http_server, &chain);

  httpd_uri_t witness = { .uri = "/api/witness", .method = HTTP_GET, .handler = handle_witness };
  httpd_register_uri_handler(m_http_server, &witness);

  httpd_uri_t logs = { .uri = "/api/logs", .method = HTTP_GET, .handler = handle_logs };
  httpd_register_uri_handler(m_http_server, &logs);

  httpd_uri_t log_ack = { .uri = "/api/logs/*/ack", .method = HTTP_POST, .handler = handle_log_ack };
  httpd_register_uri_handler(m_http_server, &log_ack);

  httpd_uri_t ack_all = { .uri = "/api/logs/ack-all", .method = HTTP_POST, .handler = handle_ack_all };
  httpd_register_uri_handler(m_http_server, &ack_all);

  httpd_uri_t reboot = { .uri = "/api/reboot", .method = HTTP_POST, .handler = handle_reboot };
  httpd_register_uri_handler(m_http_server, &reboot);

  httpd_uri_t export_ep = { .uri = "/api/export", .method = HTTP_POST, .handler = handle_export };
  httpd_register_uri_handler(m_http_server, &export_ep);

  // WiFi management endpoints
  httpd_uri_t wifi_status = { .uri = "/api/wifi/status", .method = HTTP_GET, .handler = handle_wifi_status };
  httpd_register_uri_handler(m_http_server, &wifi_status);

  httpd_uri_t wifi_scan = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan };
  httpd_register_uri_handler(m_http_server, &wifi_scan);

  httpd_uri_t wifi_connect = { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = handle_wifi_connect };
  httpd_register_uri_handler(m_http_server, &wifi_connect);

  httpd_uri_t wifi_disconnect = { .uri = "/api/wifi/disconnect", .method = HTTP_POST, .handler = handle_wifi_disconnect };
  httpd_register_uri_handler(m_http_server, &wifi_disconnect);

  // Peer list (mDNS browse cache). Path matches canary-vision/docs/discovery.md
  // and the SPA's CanaryAPI.request(... '/api/v1/peers').
  httpd_uri_t peers_ep = { .uri = "/api/v1/peers", .method = HTTP_GET, .handler = handle_peers };
  httpd_register_uri_handler(m_http_server, &peers_ep);

  #if FEATURE_HA_MQTT
  httpd_uri_t mqtt_stat = { .uri = "/api/mqtt/status", .method = HTTP_GET, .handler = handle_mqtt_status };
  httpd_register_uri_handler(m_http_server, &mqtt_stat);

  httpd_uri_t mqtt_cfg = { .uri = "/api/mqtt/config", .method = HTTP_POST, .handler = handle_mqtt_config };
  httpd_register_uri_handler(m_http_server, &mqtt_cfg);
  #endif

  #if FEATURE_OTA_UPDATE && !defined(SECURACV_BUILD_RELEASE)
  httpd_uri_t ota = { .uri = "/api/ota", .method = HTTP_POST, .handler = handle_ota };
  httpd_register_uri_handler(m_http_server, &ota);
  #endif

  #if FEATURE_OTA_PULL
  httpd_uri_t ota_status = { .uri = "/api/ota/status", .method = HTTP_GET, .handler = handle_ota_status };
  httpd_register_uri_handler(m_http_server, &ota_status);

  httpd_uri_t ota_check = { .uri = "/api/ota/check", .method = HTTP_POST, .handler = handle_ota_check };
  httpd_register_uri_handler(m_http_server, &ota_check);

  httpd_uri_t ota_install = { .uri = "/api/ota/install", .method = HTTP_POST, .handler = handle_ota_install };
  httpd_register_uri_handler(m_http_server, &ota_install);

  httpd_uri_t ota_cfg = { .uri = "/api/ota/config", .method = HTTP_POST, .handler = handle_ota_config };
  httpd_register_uri_handler(m_http_server, &ota_cfg);
  #endif

  #if FEATURE_CAMERA_PEEK
  httpd_uri_t peek_start = { .uri = "/api/peek/start", .method = HTTP_POST, .handler = handle_peek_start };
  httpd_register_uri_handler(m_http_server, &peek_start);

  httpd_uri_t peek_stream = { .uri = "/api/peek/stream", .method = HTTP_GET, .handler = handle_peek_stream };
  httpd_register_uri_handler(m_http_server, &peek_stream);

  httpd_uri_t peek_stop = { .uri = "/api/peek/stop", .method = HTTP_POST, .handler = handle_peek_stop };
  httpd_register_uri_handler(m_http_server, &peek_stop);

  httpd_uri_t peek_status = { .uri = "/api/peek/status", .method = HTTP_GET, .handler = handle_peek_status };
  httpd_register_uri_handler(m_http_server, &peek_status);

  httpd_uri_t peek_init = { .uri = "/api/peek/init", .method = HTTP_POST, .handler = handle_peek_init };
  httpd_register_uri_handler(m_http_server, &peek_init);

  httpd_uri_t peek_res = { .uri = "/api/peek/resolution", .method = HTTP_POST, .handler = handle_peek_resolution };
  httpd_register_uri_handler(m_http_server, &peek_res);

  httpd_uri_t peek_sensor_g = { .uri = "/api/peek/sensor", .method = HTTP_GET, .handler = handle_peek_sensor_get };
  httpd_register_uri_handler(m_http_server, &peek_sensor_g);

  httpd_uri_t peek_sensor_s = { .uri = "/api/peek/sensor", .method = HTTP_POST, .handler = handle_peek_sensor_set };
  httpd_register_uri_handler(m_http_server, &peek_sensor_s);

  httpd_uri_t peek_snap = { .uri = "/api/peek/snapshot", .method = HTTP_GET, .handler = handle_peek_snapshot };
  httpd_register_uri_handler(m_http_server, &peek_snap);
  #endif

  #if FEATURE_CSI || FEATURE_ACOUSTIC_EVENTS || FEATURE_TOUCH || FEATURE_IR_RMT || FEATURE_TEMP_TAMPER
  httpd_uri_t sensing_ep = { .uri = "/api/sensing", .method = HTTP_GET, .handler = handle_sensing };
  httpd_register_uri_handler(m_http_server, &sensing_ep);
  #endif

  #if FEATURE_VISION_DETECT
  httpd_uri_t vision_cfg_g = { .uri = "/api/vision/config", .method = HTTP_GET, .handler = handle_vision_config_get };
  httpd_register_uri_handler(m_http_server, &vision_cfg_g);

  httpd_uri_t vision_cfg_s = { .uri = "/api/vision/config", .method = HTTP_POST, .handler = handle_vision_config_set };
  httpd_register_uri_handler(m_http_server, &vision_cfg_s);

  httpd_uri_t vision_cfg_save = { .uri = "/api/vision/config/save", .method = HTTP_POST, .handler = handle_vision_config_save };
  httpd_register_uri_handler(m_http_server, &vision_cfg_save);

  httpd_uri_t vision_thumb = { .uri = "/api/vision/thumbnail", .method = HTTP_GET, .handler = handle_vision_thumbnail };
  httpd_register_uri_handler(m_http_server, &vision_thumb);
  #endif

  #if FEATURE_ACOUSTIC_EVENTS
  // Live RMS for the UI level meter — same number the hysteresis uses,
  // not a second audio path. Returns 0 when muted.
  httpd_uri_t audio_level = { .uri = "/api/audio/level", .method = HTTP_GET, .handler = handle_audio_level };
  httpd_register_uri_handler(m_http_server, &audio_level);

  // Hard mute (physically uninstalls the I2S driver) — persisted in NVS.
  httpd_uri_t audio_mute_ep = { .uri = "/api/audio/mute", .method = HTTP_POST, .handler = handle_audio_mute };
  httpd_register_uri_handler(m_http_server, &audio_mute_ep);

  // Alarm-pattern self-test (relaxed thresholds, normal event callback
  // suppressed so a TEST-button press does NOT flow into HA automations).
  httpd_uri_t audio_test_start = { .uri = "/api/audio/test/start", .method = HTTP_POST, .handler = handle_audio_test_start };
  httpd_register_uri_handler(m_http_server, &audio_test_start);
  httpd_uri_t audio_test_status = { .uri = "/api/audio/test/status", .method = HTTP_GET, .handler = handle_audio_test_status };
  httpd_register_uri_handler(m_http_server, &audio_test_status);
  #endif

  #if FEATURE_DIAGNOSTICS
  httpd_uri_t diag_ep = { .uri = "/api/diagnostics", .method = HTTP_GET, .handler = handle_diagnostics };
  httpd_register_uri_handler(m_http_server, &diag_ep);

  httpd_uri_t selftest_ep = { .uri = "/api/selftest", .method = HTTP_GET, .handler = handle_selftest };
  httpd_register_uri_handler(m_http_server, &selftest_ep);
  #endif

  #if FEATURE_POWER_MONITOR
  httpd_uri_t batt_hist_ep = { .uri = "/api/battery/history", .method = HTTP_GET, .handler = handle_battery_history };
  httpd_register_uri_handler(m_http_server, &batt_hist_ep);
  #endif

  #if FEATURE_THERMAL_WATCHDOG
  httpd_uri_t thermal_ep = { .uri = "/api/thermal", .method = HTTP_GET, .handler = handle_thermal };
  httpd_register_uri_handler(m_http_server, &thermal_ep);
  #endif

  #if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
  // Mesh / opera REST API (PR-8). 6 endpoints — see spec §8.
  httpd_uri_t mesh_status_ep = { .uri = "/api/mesh", .method = HTTP_GET, .handler = handle_mesh_status };
  httpd_register_uri_handler(m_http_server, &mesh_status_ep);

  httpd_uri_t mesh_peers_ep = { .uri = "/api/mesh/peers", .method = HTTP_GET, .handler = handle_mesh_peers };
  httpd_register_uri_handler(m_http_server, &mesh_peers_ep);

  httpd_uri_t mesh_pair_start_ep = { .uri = "/api/mesh/pair/start", .method = HTTP_POST, .handler = handle_mesh_pair_start };
  httpd_register_uri_handler(m_http_server, &mesh_pair_start_ep);

  httpd_uri_t mesh_pair_join_ep = { .uri = "/api/mesh/pair/join", .method = HTTP_POST, .handler = handle_mesh_pair_join };
  httpd_register_uri_handler(m_http_server, &mesh_pair_join_ep);

  httpd_uri_t mesh_pair_confirm_ep = { .uri = "/api/mesh/pair/confirm", .method = HTTP_POST, .handler = handle_mesh_pair_confirm };
  httpd_register_uri_handler(m_http_server, &mesh_pair_confirm_ep);

  httpd_uri_t mesh_pair_cancel_ep = { .uri = "/api/mesh/pair/cancel", .method = HTTP_POST, .handler = handle_mesh_pair_cancel };
  httpd_register_uri_handler(m_http_server, &mesh_pair_cancel_ep);
  #endif

  // Wildcard fallback — MUST stay the last registration, so every exact
  // route above wins first. During setup it funnels stray hijacked-DNS
  // requests to the wizard; otherwise it 404s like before.
  httpd_uri_t catchall = { .uri = "/*", .method = HTTP_GET, .handler = handle_captive_catchall };
  httpd_register_uri_handler(m_http_server, &catchall);
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
static esp_err_t send_html_with_token(httpd_req_t* req, const char* html) {
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

  const char* token = auth_get_token();
  if (!token) token = "";
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

static esp_err_t handle_ui(httpd_req_t* req) {
  witness_get_health().http_requests++;
  return send_html_with_token(req, CANARY_UI_HTML);
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
    return send_html_with_token(req, CANARY_SETUP_HTML);
  }
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_sendstr(req, kAppleSuccessBody);
}

// GET /setup — the wizard at a stable address, reachable from a real browser
// too (canary.local/setup), not only through the captive sheet.
static esp_err_t handle_setup_page(httpd_req_t* req) {
  witness_get_health().http_requests++;
  return send_html_with_token(req, CANARY_SETUP_HTML);
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
  doc["witness_count"] = health.records_created;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["min_heap"] = health.min_heap;

  doc["crypto_healthy"] = health.crypto_healthy;
  doc["gps_healthy"] = health.gps_healthy;
  doc["sd_healthy"] = health.sd_healthy;
  doc["wifi_active"] = health.wifi_active;

  doc["logs_stored"] = health.logs_stored;
  doc["unacked_count"] = health.logs_unacked;

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

// Serve the recent witness-record ring for the timeline UI. The ring is bounded
// (display-only); the tamper-evident guarantee lives in the hash chain, and full
// history is available via /api/export. Reads only in-RAM state — no SD, no camera.
static esp_err_t handle_witness(httpd_req_t* req) {
  if (!rate_limit_check(req)) return ESP_OK;
  if (!auth_gate(req)) return ESP_OK;
  witness_get_health().http_requests++;

  const size_t ring_size = witness_get_record_ring_size();
  const size_t total = witness_get_record_count();
  const size_t head  = witness_get_record_head();

  // Optional ?last=N — clamp to [1, total]; default to all available records.
  size_t want = total;
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen > 0 && qlen < 128) {
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
      char val[12];
      if (httpd_query_key_value(query, "last", val, sizeof(val)) == ESP_OK) {
        int n = atoi(val);
        if (n > 0 && (size_t)n < want) want = (size_t)n;
      }
    }
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["total"] = total;

  JsonArray records = doc["records"].to<JsonArray>();

  // Emit chronological (oldest→newest) for the most recent `want` records so the UI's
  // slice(-50).reverse() shows newest first. Each slot is copied under the ring lock so a
  // concurrent record write can't be observed torn. Oldest of the window sits at this index:
  char hash[65];
  const size_t start = total - want;
  for (size_t j = start; j < total; j++) {
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

  DeviceIdentity& device = witness_get_device();
  nvs_store_u32(NVS_KEY_SEQ, device.seq);
  nvs_store_bytes(NVS_KEY_CHAIN, device.chain_head, 32);

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
    int part_len = snprintf(part_buf, sizeof(part_buf),
      "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
      (unsigned)fb->len);

    bool ok = sock_send_all(sockfd, part_buf, part_len);
    if (ok) ok = sock_send_all(sockfd, (const char*)fb->buf, fb->len);
    if (ok) ok = sock_send_all(sockfd, "\r\n", 2);

    uint32_t frame_bytes = (uint32_t)fb->len;
    cam.returnFrame(fb);

    if (!ok) break;
    cam.recordFrame(frame_bytes);

    uint32_t pace = cam.getFrameDelay();
    if (pace < 20)  pace = 20;
    if (pace > 500) pace = 500;
    vTaskDelay(pdMS_TO_TICKS(pace));
  }

  cam.setPeekActive(false);
  httpd_sess_trigger_close(server, sockfd);
  log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Peek stream ended", nullptr);

  __atomic_store_n(&s_stream_task, (TaskHandle_t)nullptr, __ATOMIC_SEQ_CST);
  vTaskDelete(nullptr);
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
    net["encryption"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "wpa";
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

  char body[256];
  int recv = httpd_req_recv(req, body, sizeof(body) - 1);
  if (recv <= 0) {
    return http_send_error(req, 400, "empty_body");
  }
  body[recv] = '\0';

  JsonDocument input;
  if (deserializeJson(input, body) != DeserializationError::Ok) {
    return http_send_error(req, 400, "invalid_json");
  }

  const char* ssid = input["ssid"];
  const char* password = input["password"];

  if (!ssid || strlen(ssid) == 0) {
    return http_send_error(req, 400, "missing_ssid");
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

  String response;
  serializeJson(doc, response);
  return http_send_json(req, response.c_str());
}

static esp_err_t handle_mqtt_config(httpd_req_t* req) {
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

  if (!mqtt_save_credentials(&creds)) {
    return http_send_error(req, 500, "save_failed");
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "MQTT configuration saved. Reboot to apply.";

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
  st["windows_emitted"]     = stats.windows_emitted;
  st["windows_degraded"]    = stats.windows_degraded;
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
// MESH / OPERA REST API (PR-8)
//
// Six endpoints, all auth-gated + rate-limited, all using the existing
// {ok:...} JSON convention via http_send_json / http_send_error:
//
//   GET  /api/mesh              — opera status (refreshOpera reads this)
//   GET  /api/mesh/peers        — peer list
//   POST /api/mesh/pair/start   — begin pairing as the initiator (add another)
//   POST /api/mesh/pair/join    — begin pairing as the joiner (new device)
//   POST /api/mesh/pair/confirm — user confirmed the 6-digit code matches
//   POST /api/mesh/pair/cancel  — abort an in-progress pairing
//
// The JSON-rendering for the two GET endpoints lives in the pure
// mesh_api builders so the response shape stays under host-test coverage
// even though CI compiles FEATURE_MESH_NETWORK out (dev/release envs).
//
// MAC↔fingerprint join limitation: the persisted trusted-peer set keys
// on Ed25519 pubkey (→ fingerprint), while the live transport peer table
// keys on MAC. There is no stored mapping between the two, so per-peer
// state / last_seen / rssi are best-effort placeholders here (state
// "OFFLINE", rssi 0). Documented in spec/canary_mesh_network_v0.md §8.
// ════════════════════════════════════════════════════════════════════════════

#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK

// Number of online peers from the live transport table (peers seen within
// the transport's ACTIVE window). Used for the status state mapping.
static size_t mesh_count_online_peers() {
  mesh_transport::Peer peers[16];
  const size_t n = mesh_transport::list_peers(peers, sizeof(peers) / sizeof(peers[0]));
  size_t online = 0;
  for (size_t i = 0; i < n; ++i) {
    if (peers[i].in_use && peers[i].state == mesh_transport::PeerState::ACTIVE) {
      ++online;
    }
  }
  return online;
}

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
  const size_t peers_online = mesh_count_online_peers();

  // alerts_received: opera-level alert count is not yet tracked in the
  // PIO mesh session (deferred with the alerts endpoints) — report 0.
  const uint32_t alerts_received = 0;
  const uint32_t pairing_code    = mesh_session::pairing_confirmation_code();

  char body[512];
  if (!mesh_api::build_mesh_status_json(
          body, sizeof(body),
          /*enabled=*/true, has_opera,
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

  // Trusted peers are the durable membership set (pubkeys). The live
  // transport table is keyed on MAC, with no stored MAC↔pubkey mapping,
  // so per-peer liveness is a best-effort placeholder (see section
  // header + spec §8).
  uint8_t pubkeys[mesh_state::MAX_TRUSTED_PEERS * mesh_crypto::PUBKEY_LEN];
  size_t  count = 0;
  if (!mesh_state::load_trusted_peers(pubkeys, sizeof(pubkeys), &count)) {
    return http_send_error(req, 500, "load_failed");
  }
  if (count > mesh_state::MAX_TRUSTED_PEERS) count = mesh_state::MAX_TRUSTED_PEERS;

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
    views[i].state        = "OFFLINE";     // no MAC↔fingerprint join
    views[i].last_seen_sec = 0xFFFFFFFFu;  // "never" (UI shows 'never')
    views[i].rssi          = 0;
  }

  char body[1024];
  if (!mesh_api::build_mesh_peers_json(body, sizeof(body), views, count)) {
    return http_send_error(req, 500, "encode_failed");
  }
  return http_send_json(req, body);
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
  // from NVS into a local buffer, hand it to the pairing initiator, then
  // zero the buffer. If no opera is persisted, there is nothing to add to.
  uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN];
  if (!mesh_state::load_opera_secret(opera_secret)) {
    return http_send_error(req, 400, "no_opera");
  }

  char opera_name[mesh_pairing::MAX_OPERA_NAME_LEN + 1];
  mesh_session::get_opera_name(opera_name, sizeof(opera_name));

  const bool ok = mesh_session::start_pairing_initiator(
      opera_secret, opera_name, millis());

  // Zero the local secret copy regardless of outcome.
  volatile uint8_t* z = opera_secret;
  for (size_t i = 0; i < sizeof(opera_secret); ++i) z[i] = 0;

  if (!ok) {
    return http_send_error(req, 400, "pair_start_failed");
  }

  JsonDocument doc;
  doc["ok"] = true;
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

  if (!mesh_session::start_pairing_joiner(millis())) {
    return http_send_error(req, 400, "pair_join_failed");
  }

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

  if (!mesh_session::confirm_pairing_code(millis())) {
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

  mesh_session::cancel_pairing();

  JsonDocument doc;
  doc["ok"] = true;
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
