/*
 * SecuraCV Canary — BLE WiFi Provisioning
 *
 * Replaces (well — augments) the captive-AP setup flow with a phone-pairs-
 * Canary, write-credentials-over-BLE ceremony. AP mode is preserved as a
 * fallback for users who can't / won't use BLE.
 *
 * SHAPE
 * ─────
 * Service UUID:    8fc1cef0-b162-4401-9607-c8ac21383e90
 * Characteristics:
 *   SCAN_TRIGGER  cef1   WRITE        — write any byte to kick a scan
 *   SCAN_RESULTS  cef2   READ + NOTIFY — JSON list of nearby SSIDs
 *   CREDS         cef3   WRITE only   — { "ssid": "...", "password": "..." }
 *                                       (no read property — never readable)
 *   STATE         cef4   READ + NOTIFY — current provisioning state JSON
 *
 * SECURITY
 * ────────
 * - Every characteristic carries READ_ENC + READ_AUTHEN and / or
 *   WRITE_ENC + WRITE_AUTHEN. NimBLE rejects unbonded peers.
 * - CREDS has no READ property at all. A bonded peer can SET credentials
 *   but can never read them back, even with a valid bond.
 * - Write rate-limited to 1 / 5 s and capped at 5 attempts per hour to
 *   mitigate brute-force from a stolen-bonded-phone scenario.
 * - SSID ≤ 32 bytes, password ≤ 64 bytes (WPA2 spec) — anything longer is
 *   rejected with INVALID without ever touching the credential store.
 * - Every successful credential write is audit-logged via health_logging
 *   so the witness chain captures the change. The password itself is
 *   never logged — only the SSID and a "creds applied" event.
 * - In-memory credential buffer in this TU is secure-wiped after the
 *   bridge function consumes it.
 */

#ifndef SECURACV_BLE_PROVISION_H
#define SECURACV_BLE_PROVISION_H

#include <stdint.h>
#include <stddef.h>

class NimBLEServer;

namespace ble_provision {

static const char* SERVICE_UUID       = "8fc1cef0-b162-4401-9607-c8ac21383e90";
static const char* SCAN_TRIGGER_UUID  = "8fc1cef1-b162-4401-9607-c8ac21383e90";
static const char* SCAN_RESULTS_UUID  = "8fc1cef2-b162-4401-9607-c8ac21383e90";
static const char* CREDS_UUID         = "8fc1cef3-b162-4401-9607-c8ac21383e90";
static const char* STATE_UUID         = "8fc1cef4-b162-4401-9607-c8ac21383e90";

static constexpr size_t MAX_RESULTS_PAYLOAD = 480;   // chunked over notify if needed
static constexpr size_t MAX_STATE_PAYLOAD   = 200;
static constexpr size_t MAX_SSID_LEN        = 32;    // WPA2 spec
static constexpr size_t MAX_PASSWORD_LEN    = 64;    // WPA2 spec
static constexpr uint32_t WRITE_COOLDOWN_MS = 5000;  // 1 creds-write per 5 s
// 10 attempts/hour gives a fumbling user room to retype a password a
// few times without locking the BLE channel out for an hour. The 5 s
// per-write cooldown still rate-limits brute-force, and BLE requires
// physical proximity, so 10/hour is a comfortable security ceiling.
static constexpr uint32_t HOURLY_WRITE_CAP  = 10;

bool init(NimBLEServer* server);
void tick();   // drives async scan completion + state notifications

struct Stats {
  uint32_t scans_started;
  uint32_t scans_completed;
  uint32_t creds_writes_accepted;
  uint32_t creds_writes_rejected;
  uint32_t state_notifications;
};
bool get_stats(Stats* out);

}  // namespace ble_provision

#endif  // SECURACV_BLE_PROVISION_H
