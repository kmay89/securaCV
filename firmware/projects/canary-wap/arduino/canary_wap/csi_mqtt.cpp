/**
 * @file csi_mqtt.cpp
 * @brief Implementation of the optional MQTT bridge declared in csi_mqtt.h.
 *
 * Threading model:
 *   - esp_mqtt_client maintains its own task. publishes are posted to
 *     that task's queue and the HTTP / main-loop callers return
 *     immediately. Event callbacks fire on the MQTT task; we keep them
 *     to flag-flips + Serial logs so we never block the network stack.
 *
 * Privacy model:
 *   - Every successful publish increments csi_integration's outbound
 *     byte counter (add_outbound_bytes), so the dashboard's privacy
 *     pill is honest about MQTT-driven egress. Failures (queue full,
 *     not connected) don't count — bytes that didn't leave the device
 *     don't get counted.
 *
 * Auth model:
 *   - The HTTP config / test handlers go through CSI_AUTH_OR_RETURN
 *     just like every other /api/csi/* route. A casual visitor on the
 *     SoftAP can't read the broker password back, can't change the
 *     broker, and can't trigger /test (which would leak the broker
 *     hostname via a connect attempt).
 */

#include "csi_mqtt.h"
#include "csi_integration.h"
#include "api_auth.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <atomic>

extern "C" {
#include "mqtt_client.h"
}

namespace csi_mqtt {

namespace {

constexpr const char* SETTINGS_NS = "csi";
constexpr uint16_t    DEFAULT_PORT = 1883;
constexpr uint16_t    DEFAULT_PORT_TLS = 8883;
constexpr const char* DEFAULT_PREFIX = "securacv";

esp_mqtt_client_handle_t s_client       = nullptr;
std::atomic<bool>        s_connected{false};
Config                   s_active_cfg   = {};
char                     s_device_id[33]      = {};
char                     s_firmware_version[24] = {};
char                     s_public_key_hex[65]   = {};

/* Build "{prefix}/{device_id}/{suffix}" into out. Returns out for
 * call-site composability. Out must be sized for prefix + device_id +
 * suffix + 2 separators + NUL — 192 bytes is comfortable. */
char* build_topic(char* out, size_t cap, const char* suffix) {
  if (!out || cap == 0) return nullptr;
  snprintf(out, cap, "%s/%s/%s",
           s_active_cfg.prefix[0] ? s_active_cfg.prefix : DEFAULT_PREFIX,
           s_device_id,
           suffix);
  return out;
}

/* Single chokepoint: every publish goes through here so the
 * privacy-budget accounting and the disconnected-state guard live in
 * exactly one place. Returns true on enqueue success. */
bool publish_raw(const char* topic, const char* payload, size_t len, bool retain) {
  if (!s_client || !s_connected.load(std::memory_order_relaxed)) return false;
  const int msg_id = esp_mqtt_client_publish(
      s_client, topic, payload, (int)len, /*qos=*/0, retain ? 1 : 0);
  if (msg_id < 0) return false;
  /* Only counted on successful enqueue. esp_mqtt at QoS 0 may still
   * silently drop on the wire under network failure; the dashboard's
   * privacy pill is "bytes the firmware handed off", not "bytes that
   * crossed the WAN" — same semantics every existing privacy budget
   * counter has. */
  csi_integration::add_outbound_bytes((uint32_t)len + (uint32_t)strlen(topic));
  return true;
}

void mqtt_event_handler(void* /*handler_args*/, esp_event_base_t /*base*/,
                        int32_t event_id, void* event_data) {
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;
  switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
      s_connected.store(true, std::memory_order_relaxed);
      Serial.println("[MQTT] connected");
      /* Replace the LWT-published "offline" with a fresh "online" so
       * a freshly-connecting HA sees the right state immediately. */
      char topic[192];
      build_topic(topic, sizeof(topic), "status");
      const char* online = "{\"online\":true}";
      publish_raw(topic, online, strlen(online), /*retain=*/true);
      break;
    }
    case MQTT_EVENT_DISCONNECTED:
      s_connected.store(false, std::memory_order_relaxed);
      Serial.println("[MQTT] disconnected (will retry)");
      break;
    case MQTT_EVENT_ERROR:
      if (e && e->error_handle) {
        Serial.printf("[MQTT] error: type=%d esp_tls_err=0x%x\n",
                      (int)e->error_handle->error_type,
                      (unsigned)e->error_handle->esp_tls_last_esp_err);
      }
      break;
    default:
      break;
  }
}

void teardown_client() {
  if (!s_client) return;
  esp_mqtt_client_stop(s_client);
  esp_mqtt_client_destroy(s_client);
  s_client = nullptr;
  s_connected.store(false, std::memory_order_relaxed);
}

}  /* namespace */

/* ──────────────────────────────────────────────────────────────────────────
 * NVS load / save
 * ────────────────────────────────────────────────────────────────────────── */

bool config_load(Config* out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/true)) {
    /* No NVS yet — treat as disabled with default port + prefix. */
    out->port = DEFAULT_PORT;
    strncpy(out->prefix, DEFAULT_PREFIX, MAX_PREFIX_LEN);
    return true;
  }
  out->enabled = prefs.getBool (NVS_KEY_ENABLED, false);
  out->port    = (uint16_t)prefs.getUShort(NVS_KEY_PORT, DEFAULT_PORT);
  out->tls     = prefs.getBool (NVS_KEY_TLS,     false);
  prefs.getString(NVS_KEY_HOST,   out->host,   MAX_HOST_LEN);
  prefs.getString(NVS_KEY_USER,   out->user,   MAX_USER_LEN);
  prefs.getString(NVS_KEY_PASS,   out->pass,   MAX_PASS_LEN);
  prefs.getString(NVS_KEY_PREFIX, out->prefix, MAX_PREFIX_LEN);
  prefs.end();
  if (!out->prefix[0]) strncpy(out->prefix, DEFAULT_PREFIX, MAX_PREFIX_LEN);
  if (out->port == 0)  out->port = out->tls ? DEFAULT_PORT_TLS : DEFAULT_PORT;
  return true;
}

bool config_save(const Config& cfg) {
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/false)) return false;
  prefs.putBool  (NVS_KEY_ENABLED, cfg.enabled);
  prefs.putString(NVS_KEY_HOST,    cfg.host);
  prefs.putUShort(NVS_KEY_PORT,    cfg.port);
  prefs.putString(NVS_KEY_USER,    cfg.user);
  prefs.putString(NVS_KEY_PASS,    cfg.pass);
  prefs.putString(NVS_KEY_PREFIX,  cfg.prefix);
  prefs.putBool  (NVS_KEY_TLS,     cfg.tls);
  prefs.end();
  return true;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Lifecycle
 * ────────────────────────────────────────────────────────────────────────── */

bool init(const char* device_id,
          const char* firmware_version,
          const char* public_key_hex) {
  if (device_id) {
    strncpy(s_device_id, device_id, sizeof(s_device_id) - 1);
    s_device_id[sizeof(s_device_id) - 1] = '\0';
  }
  if (firmware_version) {
    strncpy(s_firmware_version, firmware_version, sizeof(s_firmware_version) - 1);
    s_firmware_version[sizeof(s_firmware_version) - 1] = '\0';
  }
  if (public_key_hex) {
    strncpy(s_public_key_hex, public_key_hex, sizeof(s_public_key_hex) - 1);
    s_public_key_hex[sizeof(s_public_key_hex) - 1] = '\0';
  }

  /* Tear down any prior session so a /api/mqtt/config POST that flips
   * enabled / changes broker comes up cleanly. Idempotent on first
   * boot (s_client is nullptr). */
  teardown_client();

  if (!config_load(&s_active_cfg)) return false;
  if (!s_active_cfg.enabled) {
    Serial.println("[MQTT] disabled in NVS — bridge not started");
    return true;
  }
  if (!s_active_cfg.host[0]) {
    Serial.println("[MQTT] enabled but no broker host configured");
    return false;
  }

  /* Build the broker URI once; ESP-IDF accepts mqtt:// and mqtts://. */
  char uri[160];
  snprintf(uri, sizeof(uri), "%s://%s:%u",
           s_active_cfg.tls ? "mqtts" : "mqtt",
           s_active_cfg.host,
           (unsigned)s_active_cfg.port);

  /* LWT topic: same status topic the on-connect handler will publish
   * "online" to. Setting the will message before connect lets the
   * broker auto-flip us back to offline if we vanish. */
  char will_topic[192];
  build_topic(will_topic, sizeof(will_topic), "status");
  static const char will_msg[] = "{\"online\":false}";

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = uri;
  if (s_active_cfg.user[0]) {
    cfg.credentials.username = s_active_cfg.user;
  }
  if (s_active_cfg.pass[0]) {
    cfg.credentials.authentication.password = s_active_cfg.pass;
  }
  cfg.session.last_will.topic   = will_topic;
  cfg.session.last_will.msg     = will_msg;
  cfg.session.last_will.msg_len = (int)strlen(will_msg);
  cfg.session.last_will.qos     = 1;
  cfg.session.last_will.retain  = 1;
  cfg.session.keepalive         = 60;

  s_client = esp_mqtt_client_init(&cfg);
  if (!s_client) {
    Serial.println("[MQTT] esp_mqtt_client_init returned null");
    return false;
  }
  esp_mqtt_client_register_event(
      s_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, nullptr);
  if (esp_mqtt_client_start(s_client) != ESP_OK) {
    Serial.println("[MQTT] esp_mqtt_client_start failed");
    teardown_client();
    return false;
  }
  Serial.printf("[MQTT] bridge started: %s prefix=%s\n", uri, s_active_cfg.prefix);
  return true;
}

void loop() {
  /* esp_mqtt runs its own task; nothing to do here today. Reserved as
   * the place to land the SD-backed event backfill once that lands. */
}

bool connected() {
  return s_connected.load(std::memory_order_relaxed);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Concrete publishers
 *
 * Schemas locked against custom_components/securacv/sensor.py:
 *   events: {event_type, timestamp, zone, confidence, signed, motion,
 *            breathing, bpm, duration_sec, state}
 *   health: {battery, memory_free, uptime, firmware_version, public_key}
 *   chain : {length, latest_hash, algorithm}
 *   counts: {total}
 *   status: {online, csi_running, wifi_connected, rssi}
 *
 * "zone" is reserved for the future RF wizard flow (Phase 12); we
 * publish "" so the HA sensor's data.get("zone","") path remains
 * type-stable. ────────────────────────────────────────────────────── */

void publish_event(const char*               module_id,
                   const char*               type_name,
                   csi_event_category_t      category,
                   csi_privacy_class_t       privacy,
                   const csi_event_values_t* values) {
  if (!values) return;
  char topic[192];
  build_topic(topic, sizeof(topic), "events");

  const char* cat_s = (category == CSI_CATEGORY_AMBIENT) ? "ambient"
                    : (category == CSI_CATEGORY_ANOMALY) ? "anomaly" : "event";
  const char* priv_s = (privacy == CSI_PRIVACY_P2) ? "p2"
                     : (privacy == CSI_PRIVACY_P1) ? "p1" : "p0";
  const uint32_t ts_sec = (uint32_t)(millis() / 1000UL);

  char body[512];
  const int n = snprintf(body, sizeof(body),
    "{"
      "\"event_type\":\"%s\","
      "\"timestamp\":%lu,"
      "\"zone\":\"\","
      "\"confidence\":\"%s\","
      "\"signed\":true,"
      "\"module\":\"%s\","
      "\"type\":\"%s\","
      "\"category\":\"%s\","
      "\"privacy\":\"%s\","
      "\"state\":\"%s\","
      "\"motion\":%u,"
      "\"breathing\":%u,"
      "\"bpm\":%u,"
      "\"duration_sec\":%u"
    "}",
    values->state_name[0] ? values->state_name : "unknown",
    (unsigned long)ts_sec,
    values->confidence[0] ? values->confidence : "tentative",
    module_id ? module_id : "",
    type_name ? type_name : "",
    cat_s, priv_s,
    values->state_name[0] ? values->state_name : "unknown",
    (unsigned)values->motion_score,
    (unsigned)values->breathing_score,
    (unsigned)values->breathing_rate_bpm,
    (unsigned)values->duration_sec);
  if (n <= 0 || (size_t)n >= sizeof(body)) return;
  publish_raw(topic, body, (size_t)n, /*retain=*/false);
}

void publish_chain(uint32_t length, const uint8_t* latest_hash_32) {
  char topic[192];
  build_topic(topic, sizeof(topic), "chain");
  char hash_hex[65];
  hash_hex[0] = '\0';
  if (latest_hash_32) {
    /* Reuse csi_integration's canonical hex encoder rather than
     * duplicating the loop here (PR #394 review r3213674564). */
    csi_integration::hex_encode(latest_hash_32, 32, hash_hex);
  }
  char body[160];
  const int n = snprintf(body, sizeof(body),
    "{\"length\":%lu,\"latest_hash\":\"%s\",\"algorithm\":\"ed25519\"}",
    (unsigned long)length, hash_hex);
  if (n <= 0 || (size_t)n >= sizeof(body)) return;
  publish_raw(topic, body, (size_t)n, /*retain=*/true);
}

void publish_health(uint32_t free_heap_bytes, uint32_t uptime_sec) {
  char topic[192];
  build_topic(topic, sizeof(topic), "health");
  /* battery=100 reflects mains power; future hardware that ships a
   * battery-management IC overrides this from canary_wap.ino once the
   * reading is available. The field is published unconditionally so
   * the HA "healthy/warning/critical" derivation always has a number
   * to compare. */
  char body[256];
  const int n = snprintf(body, sizeof(body),
    "{"
      "\"battery\":100,"
      "\"memory_free\":%lu,"
      "\"uptime\":%lu,"
      "\"firmware_version\":\"%s\","
      "\"public_key\":\"%s\""
    "}",
    (unsigned long)free_heap_bytes,
    (unsigned long)uptime_sec,
    s_firmware_version,
    s_public_key_hex);
  if (n <= 0 || (size_t)n >= sizeof(body)) return;
  publish_raw(topic, body, (size_t)n, /*retain=*/true);
}

void publish_counts(uint32_t total) {
  char topic[192];
  build_topic(topic, sizeof(topic), "counts");
  char body[48];
  const int n = snprintf(body, sizeof(body),
    "{\"total\":%lu}", (unsigned long)total);
  if (n <= 0 || (size_t)n >= sizeof(body)) return;
  publish_raw(topic, body, (size_t)n, /*retain=*/true);
}

void publish_status(bool csi_running, bool wifi_connected, int rssi_dbm) {
  char topic[192];
  build_topic(topic, sizeof(topic), "status");
  char body[160];
  const int n = snprintf(body, sizeof(body),
    "{"
      "\"online\":true,"
      "\"csi_running\":%s,"
      "\"wifi_connected\":%s,"
      "\"rssi\":%d"
    "}",
    csi_running ? "true" : "false",
    wifi_connected ? "true" : "false",
    rssi_dbm);
  if (n <= 0 || (size_t)n >= sizeof(body)) return;
  publish_raw(topic, body, (size_t)n, /*retain=*/true);
}

/* ──────────────────────────────────────────────────────────────────────────
 * HTTP handlers
 *
 * The auth guard is delegated to a tiny wrapper so we don't pull
 * CSI_AUTH_OR_RETURN's macro definition (which lives inside
 * csi_integration.cpp's anonymous namespace) into this TU. Instead
 * we go through the Bearer + cookie surfaces directly the same way
 * the macro does; csi_integration::session_validate_cookie is
 * declared in csi_integration.h. ────────────────────────────────── */

static bool authorized(httpd_req_t* req, const char* api_token) {
  if (csi_integration::session_validate_cookie(req)) return true;
  if (api_token && api_auth_check_optional(req, api_token)) return true;
  return false;
}

/* The api_token for csi_mqtt's Bearer surface is the same one
 * /api/csi/* routes already check. We don't have direct access to
 * g_device.api_token_str from here, so the integration layer
 * registers an accessor at init time via set_api_token_provider. */
const char* (*s_api_token_provider)() = nullptr;

esp_err_t handle_config_get(httpd_req_t* req) {
  const char* tok = s_api_token_provider ? s_api_token_provider() : nullptr;
  if (!authorized(req, tok)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"securacv\"");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return ESP_OK;
  }
  Config c;
  config_load(&c);
  /* Echo everything but the password — the password is write-only on
   * the wire so a casual attacker who somehow obtained a session
   * cookie can't read the broker creds back out of GET. */
  char body[512];
  /* snprintf return value intentionally unchecked: we send via
   * HTTPD_RESP_USE_STRLEN below so a truncated payload still has a
   * NUL terminator and httpd_resp_send walks until it. Using
   * snprintf's return as the length would walk past the NUL on
   * truncation and over-read. (PR #394 review r3213674558.) */
  snprintf(body, sizeof(body),
    "{"
      "\"enabled\":%s,"
      "\"host\":\"%s\","
      "\"port\":%u,"
      "\"user\":\"%s\","
      "\"prefix\":\"%s\","
      "\"tls\":%s,"
      "\"password_set\":%s,"
      "\"connected\":%s"
    "}",
    c.enabled ? "true" : "false",
    c.host,
    (unsigned)c.port,
    c.user,
    c.prefix,
    c.tls ? "true" : "false",
    c.pass[0] ? "true" : "false",
    connected() ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/* Find a JSON string field's value into out (NUL-terminated, truncated to
 * out_cap-1). Returns true if the key was present (even if empty).
 * Hand-rolled to keep ArduinoJson out of this TU — same shape the
 * existing /api/settings POST parser uses. */
static bool json_extract_string(const char* body, const char* key,
                                char* out, size_t out_cap) {
  if (!body || !key || !out || out_cap == 0) return false;
  const char* k = strstr(body, key);
  if (!k) return false;
  const char* colon = strchr(k, ':');
  if (!colon) return false;
  const char* p = colon + 1;
  while (*p == ' ' || *p == '\t') p++;
  if (*p != '"') return false;
  p++;  /* past opening quote */
  size_t i = 0;
  while (*p && *p != '"' && i < out_cap - 1) {
    /* Standard JSON string escapes — covers everything a broker host /
     * username / password / prefix could legally carry. \uXXXX is
     * intentionally NOT decoded: those fields are ASCII in practice
     * and adding UTF-16 surrogate handling for one corner case isn't
     * worth the parser surface (PR #394 review r3213674569). An
     * unrecognised escape passes the next char through literally —
     * matches how the existing /api/settings POST parser handles
     * nonsense, and avoids a silent reject for marginally-malformed
     * input. */
    if (*p == '\\' && p[1]) {
      char esc = p[1];
      switch (esc) {
        case '"':  out[i++] = '"';  p += 2; break;
        case '\\': out[i++] = '\\'; p += 2; break;
        case '/':  out[i++] = '/';  p += 2; break;
        case 'b':  out[i++] = '\b'; p += 2; break;
        case 'f':  out[i++] = '\f'; p += 2; break;
        case 'n':  out[i++] = '\n'; p += 2; break;
        case 'r':  out[i++] = '\r'; p += 2; break;
        case 't':  out[i++] = '\t'; p += 2; break;
        default:   out[i++] = esc;  p += 2; break;
      }
    } else {
      out[i++] = *p++;
    }
  }
  out[i] = '\0';
  return true;
}

static bool json_extract_bool(const char* body, const char* key, bool* out) {
  const char* k = strstr(body, key);
  if (!k) return false;
  const char* colon = strchr(k, ':');
  if (!colon) return false;
  const char* p = colon + 1;
  while (*p == ' ' || *p == '\t') p++;
  if (strncmp(p, "true",  4) == 0) { *out = true;  return true; }
  if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
  return false;
}

static bool json_extract_int(const char* body, const char* key, long* out) {
  const char* k = strstr(body, key);
  if (!k) return false;
  const char* colon = strchr(k, ':');
  if (!colon) return false;
  const char* p = colon + 1;
  while (*p == ' ' || *p == '\t' || *p == '"') p++;
  char* end = nullptr;
  long n = strtol(p, &end, 10);
  if (end == p) return false;
  *out = n;
  return true;
}

esp_err_t handle_config_post(httpd_req_t* req) {
  const char* tok = s_api_token_provider ? s_api_token_provider() : nullptr;
  if (!authorized(req, tok)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"securacv\"");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return ESP_OK;
  }

  /* Body cap covers all fields including a 128-char password and a
   * 128-char host; anything longer is rejected as oversized rather
   * than silently truncated. */
  char body[640];
  const int got = httpd_req_recv(req, body, sizeof(body) - 1);
  if (got <= 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"empty body\"}", -1);
    return ESP_OK;
  }
  body[got] = '\0';

  /* Start from current NVS so PATCH-style requests (just toggling
   * enabled, leaving everything else alone) work. */
  Config c;
  config_load(&c);

  json_extract_bool  (body, "\"enabled\"", &c.enabled);
  json_extract_bool  (body, "\"tls\"",     &c.tls);
  long port_l = c.port;
  if (json_extract_int(body, "\"port\"", &port_l) && port_l > 0 && port_l <= 65535) {
    c.port = (uint16_t)port_l;
  }
  /* Empty string in any field clears it; missing field leaves it as-is. */
  char buf[MAX_PASS_LEN + 1] = {};
  if (json_extract_string(body, "\"host\"",   buf, sizeof(buf))) {
    strncpy(c.host,   buf, MAX_HOST_LEN);   c.host  [MAX_HOST_LEN]   = '\0';
  }
  if (json_extract_string(body, "\"user\"",   buf, sizeof(buf))) {
    strncpy(c.user,   buf, MAX_USER_LEN);   c.user  [MAX_USER_LEN]   = '\0';
  }
  if (json_extract_string(body, "\"prefix\"", buf, sizeof(buf))) {
    strncpy(c.prefix, buf, MAX_PREFIX_LEN); c.prefix[MAX_PREFIX_LEN] = '\0';
  }
  if (json_extract_string(body, "\"password\"", buf, sizeof(buf))) {
    strncpy(c.pass,   buf, MAX_PASS_LEN);   c.pass  [MAX_PASS_LEN]   = '\0';
  }
  if (!c.prefix[0]) strncpy(c.prefix, DEFAULT_PREFIX, MAX_PREFIX_LEN);
  if (c.port == 0)  c.port = c.tls ? DEFAULT_PORT_TLS : DEFAULT_PORT;

  if (!config_save(c)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"nvs unavailable\"}", -1);
    return ESP_OK;
  }

  /* Reinit so the new credentials take effect immediately. init()
   * tears down any prior client and re-opens. */
  init(s_device_id, s_firmware_version, s_public_key_hex);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"ok\":true}", -1);
  return ESP_OK;
}

esp_err_t handle_test(httpd_req_t* req) {
  const char* tok = s_api_token_provider ? s_api_token_provider() : nullptr;
  if (!authorized(req, tok)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"securacv\"");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return ESP_OK;
  }
  /* esp_mqtt's connect is async — give it a couple of seconds to
   * either flip s_connected or return an error event. The MQTT task
   * runs on its own core so this poll doesn't block the network
   * stack; we just yield often enough that the WiFi worker stays
   * responsive. */
  init(s_device_id, s_firmware_version, s_public_key_hex);
  uint32_t waited = 0;
  while (!s_connected.load(std::memory_order_relaxed) && waited < 4000) {
    delay(100);
    waited += 100;
  }
  const bool ok = s_connected.load(std::memory_order_relaxed);
  httpd_resp_set_type(req, "application/json");
  char body[96];
  snprintf(body, sizeof(body),
    "{\"ok\":%s,\"connected\":%s,\"waited_ms\":%lu}",
    ok ? "true" : "false",
    ok ? "true" : "false",
    (unsigned long)waited);
  httpd_resp_send(req, body, -1);
  return ESP_OK;
}

/* Tiny settings-page UI served at /mqtt — auth-gated like /tune via
 * cookie. The form POSTs to /api/mqtt/config; the Test button hits
 * /api/mqtt/test. Microcopy stays plain (FKGL ≤ 6) per the lint trio. */
static const char MQTT_UI_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Canary &middot; MQTT</title>
<style>
body{font-family:system-ui,-apple-system,sans-serif;max-width:520px;margin:40px auto;padding:24px;background:#fffbec;color:#1a1605;line-height:1.5;}
h1{font-weight:500;font-size:24px;margin:0 0 8px;}
p{color:#3a311e;margin:8px 0 18px;}
form{display:grid;gap:14px;}
label{display:grid;gap:4px;font-size:13px;color:#6b6049;}
input[type=text],input[type=password],input[type=number]{font:inherit;padding:10px;border:1px solid #d4c994;border-radius:8px;background:#fff;}
.row{display:grid;grid-template-columns:1fr 1fr;gap:12px;}
.row.toggle{grid-template-columns:auto auto;justify-content:start;align-items:center;gap:10px;}
.actions{display:flex;gap:10px;margin-top:12px;}
button{font:inherit;padding:10px 20px;border-radius:10px;border:0;cursor:pointer;}
button.save{background:#f0c319;color:#1a1605;font-weight:500;}
button.test{background:transparent;color:#3a311e;border:1px solid #d4c994;}
.status{font-size:13px;color:#6b6049;margin-top:8px;}
.status.good{color:#1a7a1a;}
.status.bad{color:#a02020;}
@media (prefers-color-scheme:dark){
  body{background:#1a1605;color:#fffbec;}
  p,label,.status{color:#a89e85;}
  input[type=text],input[type=password],input[type=number]{background:#2a2310;color:#fffbec;border-color:#403718;}
  button.test{color:#fffbec;border-color:#403718;}
}
</style></head><body>
<h1>Send sensing to Home Assistant</h1>
<p>Set up the broker once. The canary will send presence and breathing notes there, and your existing SecuraCV add-on will pick them up.</p>
<form id="f">
  <label class="row toggle">
    <input type="checkbox" id="enabled"> Send to a broker
  </label>
  <div class="row">
    <label>Broker host<input type="text" id="host" placeholder="192.168.1.10"></label>
    <label>Port<input type="number" id="port" min="1" max="65535" value="1883"></label>
  </div>
  <div class="row">
    <label>Username (optional)<input type="text" id="user"></label>
    <label>Password (optional)<input type="password" id="password" placeholder="leave blank to keep"></label>
  </div>
  <div class="row">
    <label>Topic prefix<input type="text" id="prefix" value="securacv"></label>
    <label class="row toggle"><input type="checkbox" id="tls"> Use TLS</label>
  </div>
  <div class="actions">
    <button class="save" type="submit">Save</button>
    <button class="test" type="button" id="test">Test connection</button>
  </div>
  <div class="status" id="status"></div>
</form>
<script>
function cvFetch(url, opts){ return fetch(url, opts); }
async function load(){
  try {
    const r = await cvFetch('/api/mqtt/config', {cache:'no-store'});
    if (!r.ok) return;
    const j = await r.json();
    enabled.checked = !!j.enabled;
    host.value      = j.host || '';
    port.value      = j.port || 1883;
    user.value      = j.user || '';
    prefix.value    = j.prefix || 'securacv';
    tls.checked     = !!j.tls;
    if (j.password_set) password.placeholder = '•••• saved (leave blank to keep)';
    document.getElementById('status').textContent = j.connected ? 'Connected to broker.' : 'Not connected.';
    document.getElementById('status').className = 'status ' + (j.connected ? 'good' : '');
  } catch {}
}
load();
f.addEventListener('submit', async (e) => {
  e.preventDefault();
  const body = {
    enabled: enabled.checked,
    host: host.value.trim(),
    port: parseInt(port.value, 10) || 1883,
    user: user.value,
    prefix: prefix.value.trim() || 'securacv',
    tls: tls.checked,
  };
  if (password.value) body.password = password.value;
  const r = await cvFetch('/api/mqtt/config', {
    method: 'POST',
    headers: {'content-type': 'application/json'},
    body: JSON.stringify(body),
  });
  const ok = r.ok;
  document.getElementById('status').textContent = ok ? 'Saved.' : 'Save failed.';
  document.getElementById('status').className = 'status ' + (ok ? 'good' : 'bad');
  if (ok) setTimeout(load, 600);
});
document.getElementById('test').addEventListener('click', async () => {
  document.getElementById('status').textContent = 'Trying to reach the broker…';
  document.getElementById('status').className = 'status';
  const r = await cvFetch('/api/mqtt/test', {method: 'POST'});
  const j = await r.json().catch(() => ({}));
  document.getElementById('status').textContent = j.ok
    ? 'Reached the broker.'
    : 'Could not reach the broker. Check host, port, and credentials.';
  document.getElementById('status').className = 'status ' + (j.ok ? 'good' : 'bad');
});
</script>
</body></html>
)HTML";

esp_err_t handle_ui(httpd_req_t* req) {
  /* Top-level page navigation can't carry a Bearer header, so we gate
   * on the cv_session cookie (same pattern as handle_tune_page). A
   * visitor without a session lands on / first and pairs there. */
  if (!csi_integration::session_validate_cookie(req)) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
  }
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, MQTT_UI_HTML, HTTPD_RESP_USE_STRLEN);
}

/* Provided so csi_integration::init can register a token accessor at
 * boot without csi_mqtt needing to know about g_device. */
void set_api_token_provider(const char* (*fn)()) {
  s_api_token_provider = fn;
}

}  /* namespace csi_mqtt */
