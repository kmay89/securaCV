/*
 * SecuraCV Canary — BLE Offline Console
 *
 * One GATT characteristic that exposes a compact, read-only JSON snapshot
 * of device state. Bonded phones connect over BLE and see the same fields
 * the SPA shows over HTTP — even when the WiFi/internet path is broken.
 *
 * SHAPE
 * ─────
 * Service UUID:   8fc1cee0-b162-4401-9607-c8ac21383e90
 * Snapshot CHR:   8fc1cee1-b162-4401-9607-c8ac21383e90
 *   properties: READ + NOTIFY
 *   security:   READ_ENC + READ_AUTHEN (bonded peers only)
 *   payload:    UTF-8 JSON, ≤ 200 bytes — single MTU-247 read.
 *
 * Sample payload:
 *   {"up":12345,"heap":154832,"wifi":"connected","wrssi":-55,
 *    "ctx":"home","owner_min":1,"hh":3,"ble":8421,"motion":12,
 *    "id":"3A4C","fw":"2.1.0"}
 *
 * SECURITY
 * ────────
 * - Bond-only read: NimBLE rejects unencrypted/unauthenticated peers.
 *   The bond is established via Numeric Comparison (MITM-safe after PR A),
 *   so a peer holding the bond has the same trust level as one holding
 *   the bearer token over WiFi — exposing the same fields is consistent.
 * - Read-only in v1. Provisioning / write actions land in PR-3.
 * - No new fields beyond what /api/status + /api/presence already expose.
 */

#ifndef SECURACV_BLE_CONSOLE_H
#define SECURACV_BLE_CONSOLE_H

#include <stdint.h>
#include <stddef.h>

class NimBLEServer;

namespace ble_console {

static const char* SERVICE_UUID  = "8fc1cee0-b162-4401-9607-c8ac21383e90";
static const char* SNAPSHOT_UUID = "8fc1cee1-b162-4401-9607-c8ac21383e90";

static constexpr size_t MAX_PAYLOAD_BYTES = 220;       // headroom under MTU 247
static constexpr uint32_t SNAPSHOT_PERIOD_MS = 5000;   // rebuild every 5 s

// Push device metadata in once at boot. Both fields are caller-owned C
// strings — we copy them. Optional; defaults are "" so JSON won't break
// if you skip this.
void set_device_metadata(const char* fingerprint_short, const char* fw_revision);

// Bring up the GATT service on the supplied NimBLE server. Must run after
// the main bluetooth_channel service is started (so security settings on
// the device are already applied). No-op if NimBLE isn't available.
bool init(NimBLEServer* server);

// Periodic tick — caller invokes this from the main loop. Internally
// throttled to SNAPSHOT_PERIOD_MS so it's safe to call every iteration.
// Rebuilds the JSON, updates the cached value, and notifies subscribers
// only when the bytes actually changed (so we don't burn radio time on
// a snapshot that's identical to last update).
void tick();

// Diagnostic counters (not exported anywhere yet; useful for debugging
// in a future status endpoint).
struct Stats {
  uint32_t snapshots_built;
  uint32_t notifications_sent;
  uint32_t reads_observed;
};
bool get_stats(Stats* out);

}  // namespace ble_console

#endif  // SECURACV_BLE_CONSOLE_H
