/*
 * SecuraCV Canary — BLE Log Export — implementation
 */

#include "build_config.h"
#include "ble_log_export.h"

#if FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

#include "health_log.h"

// BleLogSnapshot + the two extern bridge declarations live in
// ble_log_export.h so they're visible to canary_wap.ino's auto-prototype
// generator (Arduino IDE quirk — see the comment in the header).

namespace ble_log_export {

// ────────────────────────────────────────────────────────────────────────────
// STATE
// ────────────────────────────────────────────────────────────────────────────

static NimBLEService*        g_service     = nullptr;
static NimBLECharacteristic* g_head        = nullptr;
static NimBLECharacteristic* g_request     = nullptr;
static NimBLECharacteristic* g_record      = nullptr;

static char     g_head_buf[MAX_HEAD_PAYLOAD] = {0};
static size_t   g_head_buf_len = 0;
static uint32_t g_head_last_count    = 0;
static uint32_t g_head_last_newest   = 0;
static uint32_t g_head_period_ms     = 5000;
static uint32_t g_head_last_built_ms = 0;

static uint32_t g_requests_handled    = 0;
static uint32_t g_out_of_range_drops  = 0;
static uint32_t g_records_emitted     = 0;
static uint32_t g_head_notifications  = 0;

// ────────────────────────────────────────────────────────────────────────────
// FORMATTERS — short names so each record fits in MAX_RECORD_PAYLOAD
// ────────────────────────────────────────────────────────────────────────────

// Match the existing log_level_name / log_category_name strings the SPA uses
// for /api/logs so a phone scrolling here sees the same vocabulary.
static const char* level_short(uint8_t l) {
  switch (l) {
    case 0: return "debug";
    case 1: return "info";
    case 2: return "notice";
    case 3: return "warn";
    case 4: return "error";
    case 5: return "crit";
    default: return "info";
  }
}
static const char* ack_short(uint8_t a) {
  switch (a) {
    case 0: return "unread";
    case 1: return "read";
    case 2: return "ack";
    default: return "unread";
  }
}

// ────────────────────────────────────────────────────────────────────────────
// HEAD
// ────────────────────────────────────────────────────────────────────────────

static void rebuild_head(bool force_notify) {
  uint32_t count = 0, oldest = 0, newest = 0, ring_size = 0;
  if (!ble_log_get_head(&count, &oldest, &newest, &ring_size)) return;

  // Only push notifications when the visible state actually changes —
  // count or newest_seq moving means a new entry landed. Avoids burning
  // radio time on idle ticks.
  const bool changed = (count != g_head_last_count) ||
                       (newest != g_head_last_newest);
  if (!changed && !force_notify) return;

  // Serialize BEFORE updating the cached "last seen" state. If
  // serializeJson fails (overflow, allocator pressure), we want the
  // next tick to retry — bumping the cache here would mark the change
  // as "processed" and silently stall notifications until another
  // entry lands and shifts newest_seq again. Caught by Gemini review.
  JsonDocument doc;
  doc["count"]      = count;
  doc["oldest_seq"] = oldest;
  doc["newest_seq"] = newest;
  doc["ring_size"]  = ring_size;

  size_t n = serializeJson(doc, g_head_buf, sizeof(g_head_buf));
  if (n == 0 || n + 1 > sizeof(g_head_buf)) return;

  // Success — commit the cache and emit.
  g_head_last_count  = count;
  g_head_last_newest = newest;
  g_head_buf_len     = n;

  if (g_head) {
    g_head->setValue((uint8_t*)g_head_buf, g_head_buf_len);
    g_head->notify();
    g_head_notifications++;
  }
}

// ────────────────────────────────────────────────────────────────────────────
// RECORD EMITTER
// ────────────────────────────────────────────────────────────────────────────

static void emit_record_at_index(size_t newest_first_index) {
  BleLogSnapshot snap = {};
  if (!ble_log_get_by_index(newest_first_index, &snap)) {
    g_out_of_range_drops++;
    return;
  }

  JsonDocument doc;
  doc["i"]    = (uint32_t)newest_first_index;
  doc["seq"]  = snap.seq;
  doc["ts"]   = snap.timestamp_ms;
  doc["lvl"]  = level_short(snap.level);
  doc["cat"]  = (uint32_t)snap.category;  // numeric — saves bytes vs name
  doc["msg"]  = snap.message;
  if (snap.detail[0]) doc["det"] = snap.detail;
  doc["ack"]  = ack_short(snap.ack_status);

  char buf[MAX_RECORD_PAYLOAD];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  if (n == 0 || n + 1 > sizeof(buf)) {
    // Overrun — fall back to a minimal subset that always fits. We'd
    // rather emit a truncated-but-valid JSON than corrupt a notify.
    JsonDocument minimal;
    minimal["i"]    = (uint32_t)newest_first_index;
    minimal["seq"]  = snap.seq;
    minimal["lvl"]  = level_short(snap.level);
    minimal["msg"]  = "[truncated]";
    n = serializeJson(minimal, buf, sizeof(buf));
    if (n == 0) return;
  }

  if (g_record) {
    g_record->setValue((uint8_t*)buf, n);
    g_record->notify();
    g_records_emitted++;
  }
}

// ────────────────────────────────────────────────────────────────────────────
// CALLBACKS
// ────────────────────────────────────────────────────────────────────────────

class RequestCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*info*/) override {
    g_requests_handled++;

    std::string val = c->getValue();
    if (val.empty() || val.size() > 32) {
      // Out-of-shape requests silently dropped — no error oracle.
      g_out_of_range_drops++;
      return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, val.data(), val.size()) != DeserializationError::Ok) {
      g_out_of_range_drops++;
      return;
    }
    if (!doc["index"].is<uint32_t>()) {
      g_out_of_range_drops++;
      return;
    }
    const uint32_t idx = doc["index"].as<uint32_t>();
    emit_record_at_index((size_t)idx);
  }
};
static RequestCb g_request_cb;

// ────────────────────────────────────────────────────────────────────────────
// PUBLIC API
// ────────────────────────────────────────────────────────────────────────────

bool init(NimBLEServer* server) {
  if (!server) return false;
  if (g_service) return true;

  g_service = server->createService(SERVICE_UUID);
  if (!g_service) return false;

  // Each createCharacteristic can return nullptr if the GATT table is
  // exhausted or the heap is fragmented. Without these guards a later
  // setCallbacks/setValue would crash and take BLE init down completely
  // instead of failing the OTA service gracefully. Caught by Codex.

  // HEAD — bonded read + notify, no write.
  g_head = g_service->createCharacteristic(
    NimBLEUUID(HEAD_UUID),
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
      | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN
  );
  if (!g_head) {
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
               "ble_log_export: HEAD characteristic alloc failed", nullptr);
    return false;
  }

  // REQUEST — bonded write only. Read property deliberately omitted —
  // the request payload is only meaningful as a side-effecting write.
  g_request = g_service->createCharacteristic(
    NimBLEUUID(REQUEST_UUID),
    NIMBLE_PROPERTY::WRITE
      | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN
  );
  if (!g_request) {
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
               "ble_log_export: REQUEST characteristic alloc failed", nullptr);
    return false;
  }
  g_request->setCallbacks(&g_request_cb);

  // RECORD — bonded read + notify. Each completed REQUEST writes the
  // payload here and fires a notification; subsequent reads return the
  // same buffer until the next REQUEST.
  g_record = g_service->createCharacteristic(
    NimBLEUUID(RECORD_UUID),
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
      | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN
  );
  if (!g_record) {
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
               "ble_log_export: RECORD characteristic alloc failed", nullptr);
    return false;
  }

  g_service->start();

  // Seed initial HEAD value so the first read returns sensible data.
  rebuild_head(/*force_notify=*/true);
  g_head_last_built_ms = millis();

  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
             "BLE Log Export service ready", nullptr);
  return true;
}

void tick() {
  const uint32_t now = millis();
  if (now - g_head_last_built_ms < g_head_period_ms) return;
  g_head_last_built_ms = now;
  rebuild_head(/*force_notify=*/false);
}

bool get_stats(Stats* out) {
  if (!out) return false;
  out->requests_handled    = g_requests_handled;
  out->out_of_range_drops  = g_out_of_range_drops;
  out->records_emitted     = g_records_emitted;
  out->head_notifications  = g_head_notifications;
  return true;
}

}  // namespace ble_log_export

#else  // !(FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>))

#include "ble_log_export.h"
namespace ble_log_export {
bool init(NimBLEServer* /*server*/) { return false; }
void tick() {}
bool get_stats(Stats* out) {
  if (!out) return false;
  out->requests_handled = 0;
  out->out_of_range_drops = 0;
  out->records_emitted = 0;
  out->head_notifications = 0;
  return true;
}
}  // namespace ble_log_export

#endif  // FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)
