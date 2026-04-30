/*
 * SecuraCV Canary — BLE Log Export
 *
 * Read-only access to the health-log ring (and, in v2, the witness chain
 * record store) from a paired phone. Useful for forensic recovery after
 * WiFi tampering, on-site triage when canary.local is unreachable, and
 * just for power users who want to grep through device events from a
 * generic BLE scanner.
 *
 * SHAPE
 * ─────
 * Service:     8fc1cef5-b162-4401-9607-c8ac21383e90
 *   HEAD      cef6   READ + NOTIFY  ({count, oldest_seq, newest_seq})
 *   REQUEST   cef7   WRITE          ({"index": N} — newest-first index)
 *   RECORD    cef8   READ + NOTIFY  (one log entry as compact JSON)
 *
 * SECURITY
 * ────────
 * - Bonded peers only. Every characteristic carries READ_ENC + READ_AUTHEN
 *   (and WRITE_ENC + WRITE_AUTHEN where applicable). NimBLE rejects
 *   unauthenticated reads and writes.
 * - Read-only. No mutate path — a compromised bonded phone can pull
 *   logs but can't change or delete them.
 * - Bounded: index must be in range [0, count); out-of-range writes are
 *   silently dropped (no error oracle for fingerprinting).
 * - JSON output uses generic field names so a casual scanner doesn't
 *   immediately see SecuraCV-specific category strings — power users
 *   familiar with the codebase can decode them.
 */

#ifndef SECURACV_BLE_LOG_EXPORT_H
#define SECURACV_BLE_LOG_EXPORT_H

#include <stdint.h>
#include <stddef.h>

class NimBLEServer;

// Wire-format snapshot of one health-log entry. Defined at global scope
// (not inside the namespace) so the bridge functions in canary_wap.ino
// can use the same type without namespace-qualification, AND so it's
// visible to the Arduino IDE's auto-prototype generator — Arduino
// pre-processes .ino files by lifting function signatures to the top
// of the unit BEFORE walking the body, so any type referenced in a .ino
// function signature must exist in a header that's #include'd up there.
struct BleLogSnapshot {
  uint32_t seq;
  uint32_t timestamp_ms;
  uint8_t  level;
  uint8_t  category;
  uint8_t  ack_status;
  char     message[80];
  char     detail[48];
};

// External-linkage bridges defined in canary_wap.ino. They wrap
// g_health_log_ring so the BLE module doesn't need to know about
// HEALTH_LOG_RING_SIZE / head-index arithmetic. ring_size is reported
// from the bridge so the BLE module never bakes the constant in — if
// the .ino-side ring grows, the BLE response reflects it without code
// changes here.
extern bool ble_log_get_head(uint32_t* count, uint32_t* oldest_seq,
                             uint32_t* newest_seq, uint32_t* ring_size);
extern bool ble_log_get_by_index(size_t newest_first_index, BleLogSnapshot* out);

namespace ble_log_export {

static const char* SERVICE_UUID  = "8fc1cef5-b162-4401-9607-c8ac21383e90";
static const char* HEAD_UUID     = "8fc1cef6-b162-4401-9607-c8ac21383e90";
static const char* REQUEST_UUID  = "8fc1cef7-b162-4401-9607-c8ac21383e90";
static const char* RECORD_UUID   = "8fc1cef8-b162-4401-9607-c8ac21383e90";

static constexpr size_t MAX_RECORD_PAYLOAD = 220;
static constexpr size_t MAX_HEAD_PAYLOAD   = 100;

bool init(NimBLEServer* server);
void tick();   // refreshes HEAD periodically so subscribers see new entries

struct Stats {
  uint32_t requests_handled;
  uint32_t out_of_range_drops;
  uint32_t records_emitted;
  uint32_t head_notifications;
};
bool get_stats(Stats* out);

}  // namespace ble_log_export

#endif  // SECURACV_BLE_LOG_EXPORT_H
