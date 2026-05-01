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
// Scratch for record JSON. Module-static rather than stack-local to
// keep the worst-case stack frame small — rebuild_record() is called
// from rebuild_head() which has its own local cache, so a stacked
// allocation here would put ~750 B on the loop task's 8 KB stack
// during the same critical section. Caught by gemini.
static char     g_record_scratch[MAX_RECORD_PAYLOAD] = {0};
static char     g_head_scratch[MAX_HEAD_PAYLOAD] = {0};
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
  if (!ble_witness_get_record_json(g_record_scratch, sizeof(g_record_scratch))) {
    // No record yet (chain seq == 0). Publish a sentinel so a connected
    // peer reading the characteristic gets valid JSON instead of stale
    // bytes from a previous boot.
    static const char EMPTY[] = "{\"empty\":true}";
    g_record->setValue((uint8_t*)EMPTY, sizeof(EMPTY) - 1);
    return;
  }
  g_record->setValue((uint8_t*)g_record_scratch, strlen(g_record_scratch));
  g_records_published++;
}

static void rebuild_head(uint32_t /*now*/, bool force_notify){
  if (!g_head) return;

  // Fast path: cheap O(1) check via the bridge before doing any work.
  // Replaces the earlier "rebuild HEAD then parse total back out of the
  // JSON we just built" — which was both slow on idle ticks and brittle
  // (manual JSON scan, silent failure mode if format ever drifted).
  // Caught by gemini.
  const uint32_t total = ble_witness_get_total_records();
  if (total == g_last_total && !force_notify) return;

  // Now build. If snprintf overflows / fails the cache stays stale and
  // we'll retry on the next tick — same idempotent recovery as the
  // ble_log_export review-fix from #332.
  if (!ble_witness_get_head_json(g_head_scratch, sizeof(g_head_scratch))) {
    g_snapshot_failures++;
    return;
  }
  const size_t n = strlen(g_head_scratch);

  // Mutate cached state AFTER we know the snapshot is valid.
  memcpy(g_head_buf, g_head_scratch, n);
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
  // Treat "fully initialised" as "all three handles are non-null". The
  // earlier `if (g_service) return true;` short-circuit was wrong — a
  // partial-init left g_service set with g_head or g_record null, then
  // the next call falsely reported success and tick() ran against a
  // half-built service. Caught by codex P2.
  if (g_service && g_head && g_record) return true;

  // Helper: tear down whatever partial state we accumulated so a retry
  // starts from a clean slate. NimBLE owns service / characteristic
  // memory once createService runs, so we just drop our references —
  // the orphaned service stays in the GATT table until the next deinit
  // but won't be exposed to callers as "ready."
  auto reset_partial = [](){
    g_service = nullptr;
    g_head    = nullptr;
    g_record  = nullptr;
  };

  g_service = server->createService(SERVICE_UUID);
  if (!g_service) { reset_partial(); return false; }

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
    reset_partial();
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
    reset_partial();
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
