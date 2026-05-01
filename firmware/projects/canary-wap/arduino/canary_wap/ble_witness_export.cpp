/*
 * SecuraCV Canary — BLE Witness Chain Export — implementation
 */

#include "build_config.h"
#include "ble_witness_export.h"

#if FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <Arduino.h>
#include <string.h>

#include "health_log.h"

namespace ble_witness_export {

// ────────────────────────────────────────────────────────────────────────────
// STATE
// ────────────────────────────────────────────────────────────────────────────

static NimBLEService*        g_service = nullptr;
static NimBLECharacteristic* g_head    = nullptr;
static NimBLECharacteristic* g_record  = nullptr;

static char     g_head_buf[MAX_HEAD_PAYLOAD] = {0};
static size_t   g_head_buf_len = 0;
static uint32_t g_last_total = 0;        // tracked to detect changes
static uint32_t g_last_built_ms = 0;

static uint32_t g_head_notifications = 0;
static uint32_t g_snapshot_failures  = 0;
static uint32_t g_records_published  = 0;

// ────────────────────────────────────────────────────────────────────────────
// REBUILD
// ────────────────────────────────────────────────────────────────────────────

static void rebuild_record(){
  if (!g_record) return;
  char buf[MAX_RECORD_PAYLOAD];
  if (!ble_witness_get_record_json(buf, sizeof(buf))) {
    // No record yet (chain seq == 0). Publish a sentinel so a connected
    // peer reading the characteristic gets valid JSON instead of stale
    // bytes from a previous boot.
    static const char EMPTY[] = "{\"empty\":true}";
    g_record->setValue((uint8_t*)EMPTY, sizeof(EMPTY) - 1);
    return;
  }
  g_record->setValue((uint8_t*)buf, strlen(buf));
  g_records_published++;
}

static void rebuild_head(uint32_t /*now*/, bool force_notify){
  if (!g_head) return;
  char buf[MAX_HEAD_PAYLOAD];
  if (!ble_witness_get_head_json(buf, sizeof(buf))) {
    g_snapshot_failures++;
    return;
  }
  const size_t n = strlen(buf);

  // Detect "something actually changed" by parsing the total field out of
  // the JSON. The bridge format is stable (`"t":<digits>`) so a tiny inline
  // scan beats including a JSON parser here. If parsing fails we always
  // notify, which is the safe default (over-notifies, never under-notifies).
  uint32_t total = 0;
  const char* p = strstr(buf, "\"t\":");
  if (p) {
    p += 4;
    while (*p >= '0' && *p <= '9') { total = total * 10 + (uint32_t)(*p - '0'); p++; }
  }
  const bool changed = (total != g_last_total);

  // Only re-set value + notify when the bytes actually moved. Saves
  // connection-event time when nothing's happening.
  if (!changed && !force_notify) return;

  // Mutate cached state AFTER we know the snapshot is valid. Mirrors the
  // pattern from ble_log_export's review-fix in #332.
  memcpy(g_head_buf, buf, n);
  g_head_buf_len = n;
  g_last_total = total;

  g_head->setValue((uint8_t*)g_head_buf, g_head_buf_len);
  g_head->notify();
  g_head_notifications++;

  // The record characteristic is paired with HEAD: when the chain
  // advances, the latest record changes too. Refresh both in the same
  // pass so a peer reading RECORD right after a HEAD notify sees the
  // new entry, not the previous one.
  rebuild_record();
}

// ────────────────────────────────────────────────────────────────────────────
// PUBLIC API
// ────────────────────────────────────────────────────────────────────────────

bool init(NimBLEServer* server){
  if (!server) return false;
  if (g_service) return true;

  g_service = server->createService(SERVICE_UUID);
  if (!g_service) return false;

  // HEAD — bonded read + notify, no write. Carries chain head + pubkey
  // so a peer can verify any signed record against it.
  g_head = g_service->createCharacteristic(
    NimBLEUUID(HEAD_UUID),
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
      | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN
  );
  if (!g_head) {
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
               "ble_witness_export: HEAD characteristic alloc failed", nullptr);
    return false;
  }

  // RECORD — bonded read only. Value can exceed MTU, so peers use BLE
  // long-read (read-blob) procedure to retrieve in chunks. NimBLE handles
  // that transparently. Notify is intentionally omitted: notifications
  // cap at MTU - 3 and a short-record JSON fits, but long records would
  // truncate; HEAD's notification is the change signal — peers respond
  // by reading RECORD.
  g_record = g_service->createCharacteristic(
    NimBLEUUID(RECORD_UUID),
    NIMBLE_PROPERTY::READ
      | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN
  );
  if (!g_record) {
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
               "ble_witness_export: RECORD characteristic alloc failed", nullptr);
    return false;
  }

  g_service->start();

  // Seed both characteristic values so the first read returns sensible
  // data even before the first tick().
  rebuild_head(millis(), /*force_notify=*/true);
  g_last_built_ms = millis();

  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
             "BLE Witness Export service ready", nullptr);
  return true;
}

void tick(){
  const uint32_t now = millis();
  if (now - g_last_built_ms < TICK_PERIOD_MS) return;
  g_last_built_ms = now;
  rebuild_head(now, /*force_notify=*/false);
}

bool get_stats(Stats* out){
  if (!out) return false;
  out->head_notifications = g_head_notifications;
  out->snapshot_failures  = g_snapshot_failures;
  out->records_published  = g_records_published;
  return true;
}

}  // namespace ble_witness_export

#else  // !(FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>))

#include "ble_witness_export.h"
namespace ble_witness_export {
bool init(NimBLEServer* /*server*/) { return false; }
void tick() {}
bool get_stats(Stats* out) {
  if (!out) return false;
  out->head_notifications = 0;
  out->snapshot_failures = 0;
  out->records_published = 0;
  return true;
}
}  // namespace ble_witness_export

#endif  // FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)
