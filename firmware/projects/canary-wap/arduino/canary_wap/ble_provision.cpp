/*
 * SecuraCV Canary — BLE WiFi Provisioning — implementation
 */

#include "build_config.h"
#include "ble_provision.h"

#if FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>   // portMUX: the listed names (the F262 review)
#include <string.h>

#include "health_log.h"
#include "wap_json_writer.h"   // an SSID's control bytes as U+FFFD (F262)

// External-linkage bridge defined in canary_wap.ino. Validates input,
// persists credentials to NVS, kicks the existing connect state machine.
// Returns false on validation failure — we surface that to the BLE peer
// via the STATE characteristic without ever revealing what was wrong
// (don't leak "password too long" vs "ssid empty" — both = invalid).
extern bool ble_request_wifi_provisioning(const char* ssid, const char* password);

namespace ble_provision {

// ────────────────────────────────────────────────────────────────────────────
// SECURE WIPE
// ────────────────────────────────────────────────────────────────────────────

// volatile-pointer write loop the compiler can't elide. Used to scrub the
// password buffer immediately after the bridge consumes it — critical
// because BLE writes land in NimBLE's internal callback buffer too, but
// at least our local copy doesn't survive past the call window.
static void secure_wipe(void* p, size_t n) {
  volatile uint8_t* b = (volatile uint8_t*)p;
  while (n--) *b++ = 0;
  asm volatile("" ::: "memory");
}

// ────────────────────────────────────────────────────────────────────────────
// STATE
// ────────────────────────────────────────────────────────────────────────────

enum ProvState : uint8_t {
  PROV_IDLE      = 0,
  PROV_SCANNING  = 1,
  PROV_RESULTS_READY = 2,
  PROV_CONNECTING = 3,
  PROV_CONNECTED  = 4,
  PROV_FAILED     = 5,
  PROV_RATE_LIMITED = 6,
};

static const char* prov_state_name(ProvState s) {
  switch (s) {
    case PROV_IDLE:           return "idle";
    case PROV_SCANNING:       return "scanning";
    case PROV_RESULTS_READY:  return "results_ready";
    case PROV_CONNECTING:     return "connecting";
    case PROV_CONNECTED:      return "connected";
    case PROV_FAILED:         return "failed";
    case PROV_RATE_LIMITED:   return "rate_limited";
  }
  return "unknown";
}

static NimBLEService*        g_service        = nullptr;
static NimBLECharacteristic* g_scan_trigger   = nullptr;
static NimBLECharacteristic* g_scan_results   = nullptr;
static NimBLECharacteristic* g_creds          = nullptr;
static NimBLECharacteristic* g_state          = nullptr;

static ProvState  g_prov_state         = PROV_IDLE;
static bool       g_scan_in_flight     = false;
static uint32_t   g_scan_started_ms    = 0;
static char       g_state_buf[MAX_STATE_PAYLOAD] = {0};
static size_t     g_state_buf_len      = 0;

// Rate-limit bookkeeping for CREDS writes.
static uint32_t   g_last_creds_write_ms = 0;
static uint32_t   g_hourly_window_start_ms = 0;
static uint32_t   g_hourly_writes_count    = 0;

// The rows of the last list the phone was sent, by each network's own bytes
// (the F262 review): the list shows a control byte as U+FFFD and the phone
// sends back the text it showed, so CREDS reads it back to the network it
// names (listed_ssid()). The loop task writes them as it publishes a list,
// the NimBLE host task reads them in CredsCb::onWrite(), each only under
// g_listed_mux and only to copy or compare (nothing that blocks).
static constexpr int MAX_LISTED = 20;   // the list's rows at most (the MTU budget)
static char         g_listed[MAX_LISTED][MAX_SSID_LEN + 1] = {};
static int          g_listed_count = 0;
static portMUX_TYPE g_listed_mux = portMUX_INITIALIZER_UNLOCKED;

// Diagnostics.
static uint32_t g_scans_started = 0;
static uint32_t g_scans_completed = 0;
static uint32_t g_creds_accepted = 0;
static uint32_t g_creds_rejected = 0;
static uint32_t g_state_notifications = 0;

// ────────────────────────────────────────────────────────────────────────────
// STATE PUBLISHER
// ────────────────────────────────────────────────────────────────────────────

static void publish_state(const char* extra_field_key = nullptr,
                          const char* extra_field_val = nullptr) {
  if (!g_state) return;
  JsonDocument doc;
  doc["state"] = prov_state_name(g_prov_state);
  doc["scan_in_flight"] = g_scan_in_flight;
  if (g_prov_state == PROV_RATE_LIMITED) {
    doc["retry_after_sec"] =
      (g_last_creds_write_ms + WRITE_COOLDOWN_MS > millis())
        ? ((g_last_creds_write_ms + WRITE_COOLDOWN_MS - millis()) / 1000)
        : 0;
  }
  if (extra_field_key && extra_field_val) {
    doc[extra_field_key] = extra_field_val;
  }
  size_t n = serializeJson(doc, g_state_buf, sizeof(g_state_buf));
  if (n == 0 || n + 1 > sizeof(g_state_buf)) {
    // Fallback — minimal envelope.
    n = snprintf(g_state_buf, sizeof(g_state_buf),
                 "{\"state\":\"%s\"}", prov_state_name(g_prov_state));
    if (n >= sizeof(g_state_buf)) n = sizeof(g_state_buf) - 1;
  }
  g_state_buf_len = n;
  g_state->setValue((uint8_t*)g_state_buf, g_state_buf_len);
  g_state->notify();
  g_state_notifications++;
}

// ────────────────────────────────────────────────────────────────────────────
// SCAN HANDLING
// ────────────────────────────────────────────────────────────────────────────

static const char* security_short(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "wep";
    case WIFI_AUTH_WPA_PSK:         return "wpa";
    case WIFI_AUTH_WPA2_PSK:        return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-ent";
    case WIFI_AUTH_WPA3_PSK:        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa3";
    default:                        return "unknown";
  }
}

static void start_scan() {
  if (g_scan_in_flight) {
    // Existing scan covers it — no need to kick another. Just bump state
    // so the peer sees we noticed.
    publish_state();
    return;
  }
  // async=true so we don't block the BLE callback. show_hidden=false.
  // passive=false (active probes are needed to surface most home APs).
  int n = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false, /*passive=*/false);
  if (n == WIFI_SCAN_RUNNING) {
    // WIFI_SCAN_RUNNING (-1) is the only success return for an async start.
    // WIFI_SCAN_FAILED (-2) means the radio refused — n == 0 means a
    // synchronous empty result, which shouldn't happen with async=true and
    // is also not a valid in-flight signal. Either way, don't pretend the
    // scan is in flight if it isn't.
    g_scan_in_flight   = true;
    g_scan_started_ms  = millis();
    g_scans_started++;
    g_prov_state = PROV_SCANNING;
    publish_state();
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE provisioning: scan started", nullptr);
  } else {
    g_prov_state = PROV_FAILED;
    publish_state("error", "scan_start_failed");
  }
}

// The scan list with its first `rows` networks, `names` their own bytes, as
// the phone reads it. Each name is up to 32 bytes the access point chose, any
// of them a control byte ArduinoJson would write raw (sweep F262: the phone's
// JSON.parse refuses the list), so each is written through replaced_text().
static String scan_list_json(int n_found, uint32_t scanned_ms,
                             const char (*names)[MAX_SSID_LEN + 1], int rows, bool truncated) {
  JsonDocument doc;
  doc["count"] = n_found < 0 ? 0 : n_found;
  doc["scanned_ms"] = scanned_ms;
  if (truncated) doc["truncated"] = true;
  JsonArray arr = doc["aps"].to<JsonArray>();
  for (int i = 0; i < rows; i++) {
    JsonObject ap = arr.add<JsonObject>();
    char ssid[wap_json::replaced_cap(MAX_SSID_LEN)];
    wap_json::replaced_text(ssid, sizeof(ssid), names[i]);
    ap["ssid"] = ssid;
    ap["rssi"] = WiFi.RSSI(i);
    ap["sec"]  = security_short(WiFi.encryptionType(i));
  }
  String buf;
  serializeJson(doc, buf);
  return buf;
}

static void publish_scan_results(int n_found) {
  if (!g_scan_results) return;

  // Each listed network's own name, read once (WiFi.SSID(i) is a String).
  // The loop task's alone: a static, not 660 bytes of its stack.
  static char names[MAX_LISTED][MAX_SSID_LEN + 1];
  const int found = n_found < 0 ? 0 : n_found;
  const int listed = found < MAX_LISTED ? found : MAX_LISTED;   // cap at 20 to fit MTU budget
  for (int i = 0; i < listed; i++) {
    strncpy(names[i], WiFi.SSID(i).c_str(), MAX_SSID_LEN);
    names[i][MAX_SSID_LEN] = '\0';
  }
  const uint32_t scanned_ms = millis() - g_scan_started_ms;

  // At most MAX_RESULTS_PAYLOAD bytes: the strongest rows that fit (the core
  // sorts a scan by RSSI, strongest first), each whole, dropped from the end
  // and the list measured again after each (the F262 review: the old trim
  // kept 8 rows without measuring them, 639 bytes for 8 names of 32
  // characters and 1151 once each control byte is three bytes of U+FFFD).
  // No rows at all is about 60 bytes, so the list always fits.
  int rows = listed;
  String buf = scan_list_json(n_found, scanned_ms, names, rows, false);
  while (buf.length() > MAX_RESULTS_PAYLOAD && rows > 0) {
    rows--;
    buf = scan_list_json(n_found, scanned_ms, names, rows, true);
  }

  portENTER_CRITICAL(&g_listed_mux);
  memcpy(g_listed, names, sizeof(g_listed[0]) * (size_t)rows);
  g_listed_count = rows;
  portEXIT_CRITICAL(&g_listed_mux);

  g_scan_results->setValue((uint8_t*)buf.c_str(), buf.length());
  g_scan_results->notify();
}

// The listed network a phone's CREDS names (the F262 review): when `posted`
// is a name the last list showed as a page shows it
// (wap_json::listed_by_shown()), that network's own bytes go to out[cap] and
// the answer is true; a name the list did not show (typed by hand, a hidden
// network) answers false, and the caller takes the name as posted.
static bool listed_ssid(const char* posted, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return false;
  bool found = false;
  portENTER_CRITICAL(&g_listed_mux);
  const int i = wap_json::listed_by_shown(posted, g_listed_count,
                                          [](int k) { return (const char*)g_listed[k]; });
  if (i >= 0) {
    const size_t len = strlen(g_listed[i]);
    if (len < cap) {
      memcpy(out, g_listed[i], len + 1);
      found = true;
    }
  }
  portEXIT_CRITICAL(&g_listed_mux);
  return found;
}

// ────────────────────────────────────────────────────────────────────────────
// CALLBACKS
// ────────────────────────────────────────────────────────────────────────────

class ScanTriggerCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* /*c*/, NimBLEConnInfo& /*info*/) override {
    start_scan();
  }
};

class CredsCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*info*/) override {
    const uint32_t now = millis();

    // Hourly window roll-over. We treat the cap as a sliding window from
    // the first write of the hour — once 60 minutes have elapsed since the
    // window started, reset the counter.
    if (g_hourly_window_start_ms == 0 ||
        (now - g_hourly_window_start_ms) >= 60UL * 60UL * 1000UL) {
      g_hourly_window_start_ms = now;
      g_hourly_writes_count    = 0;
    }

    // Per-write cooldown.
    if (g_last_creds_write_ms != 0 &&
        (now - g_last_creds_write_ms) < WRITE_COOLDOWN_MS) {
      g_creds_rejected++;
      g_prov_state = PROV_RATE_LIMITED;
      publish_state("error", "cooldown");
      log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
        "BLE provisioning: creds write rejected (cooldown)", nullptr);
      return;
    }
    if (g_hourly_writes_count >= HOURLY_WRITE_CAP) {
      g_creds_rejected++;
      g_prov_state = PROV_RATE_LIMITED;
      publish_state("error", "hourly_cap");
      log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
        "BLE provisioning: creds write rejected (hourly cap)", nullptr);
      return;
    }

    // Past the rate-limit gate — count this as a "spent attempt" RIGHT NOW
    // so any failure path below (oversized payload, bad JSON, validation
    // refusal) still consumes one of the hourly slots and arms the
    // cooldown. Without this, an attacker could flood malformed writes
    // and never trip the rate limiter, which is what Codex P2 caught.
    g_last_creds_write_ms    = now;
    g_hourly_writes_count++;

    std::string val = c->getValue();
    if (val.empty() || val.size() > 256) {  // SSID + pw + JSON envelope
      g_creds_rejected++;
      publish_state("error", "invalid");
      return;
    }

    // Parse JSON. Failure surfaces as generic "invalid" — don't leak
    // structural details to a possible attacker enumerating shapes.
    JsonDocument doc;
    if (deserializeJson(doc, val.data(), val.size()) != DeserializationError::Ok) {
      g_creds_rejected++;
      publish_state("error", "invalid");
      return;
    }

    // A network the list showed is joined by its own bytes: the phone sends
    // back the text it was shown, U+FFFD and all (the F262 review).
    const char* posted_ssid = doc["ssid"] | "";
    char listed[MAX_SSID_LEN + 1];
    const char* ssid_in = listed_ssid(posted_ssid, listed, sizeof(listed)) ? listed : posted_ssid;
    const char* pw_in   = doc["password"] | "";

    // Reject (don't silently truncate) over-spec inputs. WPA2 caps SSID
    // at 32 bytes and PSK at 64 bytes; anything longer can't be a valid
    // home credential, so accepting a truncated version would persist a
    // wrong value the user never typed. Codex P1 flagged this.
    const size_t ssid_in_len = strlen(ssid_in);
    const size_t pw_in_len   = strlen(pw_in);
    if (ssid_in_len == 0 || ssid_in_len > MAX_SSID_LEN ||
        pw_in_len > MAX_PASSWORD_LEN) {
      g_creds_rejected++;
      publish_state("error", "invalid");
      log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
        "BLE provisioning: creds rejected (size out of range)", nullptr);
      return;
    }

    char ssid[MAX_SSID_LEN + 1] = {0};
    char pw[MAX_PASSWORD_LEN + 1] = {0};
    memcpy(ssid, ssid_in, ssid_in_len);
    memcpy(pw,   pw_in,   pw_in_len);
    // ssid/pw are zero-initialized so the trailing NUL is already in place.

    bool ok = ble_request_wifi_provisioning(ssid, pw);

    // Scrub local password copy regardless of outcome. The bridge has
    // already either persisted to NVS (success) or discarded (failure).
    secure_wipe(pw,   sizeof(pw));
    secure_wipe(ssid, sizeof(ssid));

    if (ok) {
      g_creds_accepted++;
      g_prov_state = PROV_CONNECTING;
      publish_state();
      log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
        "BLE provisioning: creds accepted, connecting", nullptr);
    } else {
      g_creds_rejected++;
      g_prov_state = PROV_FAILED;
      publish_state("error", "invalid");
      log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
        "BLE provisioning: creds rejected (validation)", nullptr);
    }
  }
};

static ScanTriggerCb g_scan_trigger_cb;
static CredsCb       g_creds_cb;

// ────────────────────────────────────────────────────────────────────────────
// PUBLIC API
// ────────────────────────────────────────────────────────────────────────────

bool init(NimBLEServer* server) {
  if (!server) return false;
  if (g_service) return true;

  g_service = server->createService(SERVICE_UUID);
  if (!g_service) return false;

  // SCAN_TRIGGER — write only, bonded.
  g_scan_trigger = g_service->createCharacteristic(
    NimBLEUUID(SCAN_TRIGGER_UUID),
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN
  );
  g_scan_trigger->setCallbacks(&g_scan_trigger_cb);

  // SCAN_RESULTS — read + notify, bonded.
  g_scan_results = g_service->createCharacteristic(
    NimBLEUUID(SCAN_RESULTS_UUID),
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
      | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN
  );

  // CREDS — write only. Deliberately NO read property: a bonded peer can
  // SET credentials but can never read them back. NimBLE rejects reads
  // with INSUFFICIENT_AUTHORIZATION even on a bonded link.
  g_creds = g_service->createCharacteristic(
    NimBLEUUID(CREDS_UUID),
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN
  );
  g_creds->setCallbacks(&g_creds_cb);

  // STATE — read + notify, bonded.
  g_state = g_service->createCharacteristic(
    NimBLEUUID(STATE_UUID),
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
      | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN
  );

  g_service->start();

  // Seed initial values so first reads return something.
  publish_state();

  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
             "BLE provisioning service ready", nullptr);
  return true;
}

void tick() {
  // Drain async scan completion. WiFi.scanComplete returns:
  //   WIFI_SCAN_RUNNING (-1) while in flight,
  //   WIFI_SCAN_FAILED (-2) on radio error,
  //   ≥0 = number of APs found.
  if (g_scan_in_flight) {
    int rc = WiFi.scanComplete();
    if (rc == WIFI_SCAN_RUNNING) {
      // Still going. Time-out after 30 s so a stuck scan doesn't pin us
      // in PROV_SCANNING forever.
      if (millis() - g_scan_started_ms > 30000) {
        WiFi.scanDelete();
        g_scan_in_flight = false;
        g_prov_state = PROV_FAILED;
        publish_state("error", "scan_timeout");
      }
    } else {
      // Done — either success or failure.
      g_scan_in_flight = false;
      g_scans_completed++;
      if (rc < 0) {
        g_prov_state = PROV_FAILED;
        publish_state("error", "scan_failed");
      } else {
        g_prov_state = PROV_RESULTS_READY;
        publish_scan_results(rc);
        publish_state();
      }
      WiFi.scanDelete();
    }
  }

  // Mirror the WiFi connect outcome into our STATE characteristic. We can't
  // see the canary_wap.ino state struct directly, but WL_CONNECTED is a
  // clear signal that the credentials we just wrote worked.
  if (g_prov_state == PROV_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      g_prov_state = PROV_CONNECTED;
      publish_state();
    } else if (WiFi.status() == WL_CONNECT_FAILED ||
               WiFi.status() == WL_NO_SSID_AVAIL) {
      g_prov_state = PROV_FAILED;
      const char* reason = (WiFi.status() == WL_NO_SSID_AVAIL)
                             ? "no_ssid" : "auth_failed";
      publish_state("error", reason);
    }
    // Other states (DISCONNECTED, IDLE_STATUS) might just be transient
    // during association — keep showing PROV_CONNECTING until one of the
    // terminal statuses fires.
  }
}

bool get_stats(Stats* out) {
  if (!out) return false;
  out->scans_started         = g_scans_started;
  out->scans_completed       = g_scans_completed;
  out->creds_writes_accepted = g_creds_accepted;
  out->creds_writes_rejected = g_creds_rejected;
  out->state_notifications   = g_state_notifications;
  return true;
}

}  // namespace ble_provision

#else  // !(FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>))

// No-NimBLE stubs (Arduino-CLI CI build pattern — see #328 fix).
#include "ble_provision.h"
namespace ble_provision {
bool init(NimBLEServer* /*server*/) { return false; }
void tick() {}
bool get_stats(Stats* out) {
  if (!out) return false;
  out->scans_started = 0;
  out->scans_completed = 0;
  out->creds_writes_accepted = 0;
  out->creds_writes_rejected = 0;
  out->state_notifications = 0;
  return true;
}
}  // namespace ble_provision

#endif  // FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)
