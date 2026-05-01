/*
 * SecuraCV Canary — BLE Witness Chain Export
 *
 * Bonded read-only access to the Ed25519-signed chain head + most recent
 * witness record, served over BLE so a paired phone can independently
 * verify the device's claimed chain state without needing WiFi or
 * canary.local. Same forensic-recovery promise as ble_log_export, but
 * for cryptographic evidence rather than operational events.
 *
 * SHAPE
 * ─────
 * Service:    8fc1cefa-b162-4401-9607-c8ac21383e90
 *   HEAD     cefb   READ + NOTIFY  ({"s":seq,"t":total,"h":head,"pk":pubkey})
 *   RECORD   cefc   READ           (full last-record JSON, up to 512 B)
 *
 * The HEAD value is small (~180 bytes) so it fits in a single MTU read
 * AND can notify on every new record. The RECORD value can reach ~440
 * bytes (32-byte hashes + 64-byte signature, all hex-encoded) so it's
 * read-only via BLE long-read protocol — no notify because the payload
 * exceeds MTU - 3.
 *
 * SECURITY
 * ────────
 * - Bonded peers only: every characteristic carries READ_ENC + READ_AUTHEN.
 *   NimBLE rejects unauthenticated reads.
 * - Read-only data plane. The PWA can verify the chain but cannot
 *   delete, modify, back-date, or alter records — the firmware exposes
 *   no mutation API for the chain.
 * - All exposed fields (head hash, pubkey, signed record) are PUBLIC
 *   in the cryptographic sense: they're verifiable but not secret.
 *   The device's PRIVATE key is never touched by this module.
 */

#ifndef SECURACV_BLE_WITNESS_EXPORT_H
#define SECURACV_BLE_WITNESS_EXPORT_H

#include <stdint.h>
#include <stddef.h>

class NimBLEServer;

// External-linkage bridges defined in canary_wap.ino. They marshal
// g_device + g_last_record into compact JSON so the BLE module doesn't
// need internal-linkage access to the device-state structs.
extern bool ble_witness_get_head_json(char* out, size_t out_len);
extern bool ble_witness_get_record_json(char* out, size_t out_len);

namespace ble_witness_export {

static const char* SERVICE_UUID = "8fc1cefa-b162-4401-9607-c8ac21383e90";
static const char* HEAD_UUID    = "8fc1cefb-b162-4401-9607-c8ac21383e90";
static const char* RECORD_UUID  = "8fc1cefc-b162-4401-9607-c8ac21383e90";

static constexpr size_t MAX_HEAD_PAYLOAD   = 240;
static constexpr size_t MAX_RECORD_PAYLOAD = 512;
static constexpr uint32_t TICK_PERIOD_MS   = 5000;

bool init(NimBLEServer* server);
void tick();   // refreshes HEAD periodically; notifies subscribers on change

struct Stats {
  uint32_t head_notifications;
  uint32_t snapshot_failures;
  uint32_t records_published;
};
bool get_stats(Stats* out);

}  // namespace ble_witness_export

#endif  // SECURACV_BLE_WITNESS_EXPORT_H
