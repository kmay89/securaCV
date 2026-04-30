/*
 * SecuraCV Canary — BLE Offline Console — implementation
 */

#include "build_config.h"
#include "ble_console.h"

#if FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <string.h>

#include "household.h"
#include "presence_context.h"
#include "notify.h"
#include "ble_presence.h"
#include "rf_presence.h"
#include "health_log.h"

namespace ble_console {

// ────────────────────────────────────────────────────────────────────────────
// STATE
// ────────────────────────────────────────────────────────────────────────────

static NimBLEService*        g_service     = nullptr;
static NimBLECharacteristic* g_snapshot    = nullptr;
static char                  g_last_payload[MAX_PAYLOAD_BYTES] = {0};
static size_t                g_last_payload_len = 0;
static uint32_t              g_last_built_ms    = 0;

static uint32_t g_snapshots_built = 0;
static uint32_t g_notifications_sent = 0;

static char g_meta_id[16] = {0};   // short fingerprint hex (≤ 8 chars)
static char g_meta_fw[24] = {0};

class SnapshotCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* /*c*/, NimBLEConnInfo& /*info*/) override {
    // No counter mutation in the hot path that the radio inspects under
    // a critical section, but we want the diagnostic for "is anyone
    // actually using this?" — increment is best-effort.
  }
};
static SnapshotCallbacks g_callbacks;

// ────────────────────────────────────────────────────────────────────────────
// WIFI STATE STRING
// ────────────────────────────────────────────────────────────────────────────

static const char* wifi_state_short() {
  // Arduino WiFi status enum -> short string for the snapshot. Matches the
  // names the SPA renders so a power user pivoting from /api/wifi to
  // /api/console-snapshot doesn't have to translate.
  switch (WiFi.status()) {
    case WL_CONNECTED:        return "connected";
    case WL_NO_SSID_AVAIL:    return "no_ssid";
    case WL_CONNECT_FAILED:   return "failed";
    case WL_CONNECTION_LOST:  return "lost";
    case WL_DISCONNECTED:     return "disconnected";
    case WL_IDLE_STATUS:      return "idle";
    case WL_NO_SHIELD:        return "off";
    default:                  return "unknown";
  }
}

static const char* notify_ctx_short(notify::Context c) {
  switch (c) {
    case notify::CTX_HOME:        return "home";
    case notify::CTX_AWAY:        return "away";
    case notify::CTX_QUIET_HOURS: return "quiet";
    case notify::CTX_TRAVELING:   return "traveling";
    default:                      return "home";
  }
}

// ────────────────────────────────────────────────────────────────────────────
// SNAPSHOT BUILDER
// ────────────────────────────────────────────────────────────────────────────

// Builds the JSON payload into `out` and returns the byte length. Returns 0
// on overflow — caller treats that as "skip this update."
static size_t build_snapshot(char* out, size_t out_len) {
  if (out_len == 0) return 0;

  JsonDocument doc;

  doc["up"]    = (uint32_t)(millis() / 1000);
  doc["heap"]  = (uint32_t)ESP.getFreeHeap();
  doc["wifi"]  = wifi_state_short();
  if (WiFi.status() == WL_CONNECTED) {
    doc["wrssi"] = (int)WiFi.RSSI();
  }

  // Presence + context. presence_context::get_status returns the current
  // auto + effective context plus the youngest owner-seen age in ms.
  presence_context::Status pc = {};
  presence_context::get_status(&pc);
  doc["ctx"]      = notify_ctx_short(pc.effective_context);
  if (pc.ms_since_any_owner != UINT32_MAX) {
    // Convert ms → minutes for compactness; this matches how the SPA
    // already renders "owner phone seen N min ago".
    doc["owner_min"] = pc.ms_since_any_owner / 60000;
  }
  if (pc.override_active) doc["ovr"] = true;

  // Counts
  doc["hh"] = (uint32_t)household::count();

  // BLE-sensor adverts seen (sanity check: we're really listening). The
  // ble_presence stubs return all-zero on no-NimBLE builds, but this TU
  // is gated on NimBLE so it'll be the real value here.
  ble_presence::Stats bs = {};
  if (ble_presence::get_stats(&bs)) {
    doc["ble"]    = bs.adverts_seen;
    doc["ble_hh"] = bs.adverts_resolved_household;
  }

  // RF-sensing motion score (CSI-derived; phase-6 baseline driver).
  doc["motion"] = (uint32_t)rf_presence::current_csi_motion_score();

  if (g_meta_id[0]) doc["id"] = g_meta_id;
  if (g_meta_fw[0]) doc["fw"] = g_meta_fw;

  // Serialize. measureJson + reserve avoids a second pass.
  size_t needed = measureJson(doc);
  if (needed + 1 > out_len) {
    // Snapshot would overrun the characteristic — drop the optional
    // fields and re-serialize only the essentials.
    JsonDocument minimal;
    minimal["up"]   = (uint32_t)(millis() / 1000);
    minimal["heap"] = (uint32_t)ESP.getFreeHeap();
    minimal["wifi"] = wifi_state_short();
    minimal["ctx"]  = notify_ctx_short(pc.effective_context);
    minimal["hh"]   = (uint32_t)household::count();
    needed = measureJson(minimal);
    if (needed + 1 > out_len) return 0;
    return serializeJson(minimal, out, out_len);
  }
  return serializeJson(doc, out, out_len);
}

// ────────────────────────────────────────────────────────────────────────────
// PUBLIC API
// ────────────────────────────────────────────────────────────────────────────

void set_device_metadata(const char* fingerprint_short, const char* fw_revision) {
  if (fingerprint_short) {
    strncpy(g_meta_id, fingerprint_short, sizeof(g_meta_id) - 1);
    g_meta_id[sizeof(g_meta_id) - 1] = '\0';
  }
  if (fw_revision) {
    strncpy(g_meta_fw, fw_revision, sizeof(g_meta_fw) - 1);
    g_meta_fw[sizeof(g_meta_fw) - 1] = '\0';
  }
}

bool init(NimBLEServer* server) {
  if (!server) return false;
  if (g_service) return true;

  g_service = server->createService(SERVICE_UUID);
  if (!g_service) return false;

  // Read + Notify, both gated on encrypted + authenticated link. NimBLE
  // 2.x property names: READ_ENC requires the link to be encrypted, and
  // READ_AUTHEN further requires it to be authenticated (i.e. bonded
  // after Numeric Comparison or Passkey Entry — the bond established by
  // our existing pairing flow satisfies both).
  uint32_t props = NIMBLE_PROPERTY::READ
                 | NIMBLE_PROPERTY::NOTIFY
                 | NIMBLE_PROPERTY::READ_ENC
                 | NIMBLE_PROPERTY::READ_AUTHEN;
  g_snapshot = g_service->createCharacteristic(NimBLEUUID(SNAPSHOT_UUID), props);
  if (!g_snapshot) return false;
  g_snapshot->setCallbacks(&g_callbacks);

  // Seed an initial value so the first read returns something sensible
  // even before tick() has run.
  g_last_payload_len = build_snapshot(g_last_payload, sizeof(g_last_payload));
  g_last_built_ms = millis();
  g_snapshots_built++;
  if (g_last_payload_len > 0) {
    g_snapshot->setValue((uint8_t*)g_last_payload, g_last_payload_len);
  }

  g_service->start();
  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE Offline Console ready", nullptr);
  return true;
}

void tick() {
  if (!g_snapshot) return;

  const uint32_t now = millis();
  if (now - g_last_built_ms < SNAPSHOT_PERIOD_MS) return;
  g_last_built_ms = now;

  char buf[MAX_PAYLOAD_BYTES];
  size_t n = build_snapshot(buf, sizeof(buf));
  if (n == 0) return;

  g_snapshots_built++;

  // Skip the radio if the bytes are identical to the last snapshot —
  // saves connection-event time and notification bandwidth when nothing
  // has changed (idle device).
  if (n == g_last_payload_len && memcmp(buf, g_last_payload, n) == 0) {
    return;
  }
  memcpy(g_last_payload, buf, n);
  g_last_payload_len = n;

  g_snapshot->setValue((uint8_t*)g_last_payload, g_last_payload_len);
  // notify() is a no-op if no client has subscribed, so this is cheap
  // even when nobody is listening.
  g_snapshot->notify();
  g_notifications_sent++;
}

bool get_stats(Stats* out) {
  if (!out) return false;
  out->snapshots_built     = g_snapshots_built;
  out->notifications_sent  = g_notifications_sent;
  out->reads_observed      = 0;  // see SnapshotCallbacks::onRead note
  return true;
}

}  // namespace ble_console

#else  // !(FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>))

// No-NimBLE stubs (same rationale as ble_presence.cpp — Arduino-CLI CI
// build doesn't have NimBLE-Arduino, so all symbols still need to link).
#include "ble_console.h"
namespace ble_console {
void set_device_metadata(const char* /*id*/, const char* /*fw*/) {}
bool init(NimBLEServer* /*server*/) { return false; }
void tick() {}
bool get_stats(Stats* out) {
  if (!out) return false;
  out->snapshots_built = 0;
  out->notifications_sent = 0;
  out->reads_observed = 0;
  return true;
}
}  // namespace ble_console

#endif  // FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)
