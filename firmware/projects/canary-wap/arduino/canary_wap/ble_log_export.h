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
