/*
 * SecuraCV Canary — BLE OTA over GATT (Ed25519-signed)
 *
 * GATT service that accepts firmware images streamed from a bonded phone.
 * Each image is verified against an Ed25519 signature
 * from the release engineer (see ota_release_key.h) before being written
 * to the inactive OTA partition and made bootable. A bad signature, a
 * SHA-256 mismatch, or an aborted transfer leaves the running firmware
 * untouched.
 *
 * Who may write: the control and data characteristics carry WRITE_ENC |
 * WRITE_AUTHEN and the status characteristic READ_ENC | READ_AUTHEN, the
 * properties ble_provision's characteristics carry, which ask NimBLE for a
 * link encrypted with an authenticated key (the bonded Numeric Comparison
 * pairing; the setup door's Just Works key is unauthenticated). Before
 * 2.4.20 they were plain WRITE / READ and the image policy was the only
 * gate. While a session is in flight its DATA and its control writes are
 * taken from the link whose BEGIN opened it (SessionLink, below). Both are
 * held by test_ble_ota_policy.cpp; neither has been run on a device.
 *
 * Wire protocol (control characteristic, write):
 *
 *   byte 0   command
 *   command 0x03 BEGIN_V2 — followed by an OtaHeaderV2 (168 bytes). The
 *            release signature covers product, version, image_size and
 *            sha256 through a domain-separated canonical message
 *            (layout in ble_ota_policy.h). Accepted only when the product
 *            matches the running product AND the version is at or above
 *            the anti-rollback floor shared with the pull-OTA engine
 *            (max(running version, NVS floor)).
 *   command 0x01 BEGIN    — followed by a legacy OtaHeader (132 bytes,
 *            signature over size||sha256 only). Break-glass only: refused
 *            unless the owner has armed the rescue path (below).
 *   command 0x02 ABORT    — no payload
 *
 *   After a BEGIN is accepted, the client streams the raw image bytes to
 *   the data characteristic in chunks of (MTU - 3) bytes. Once `image_size`
 *   total bytes have been received, the server verifies the running
 *   SHA-256 matches the signed digest, swaps the boot partition, and
 *   reboots. The floor is raised the same way the pull path raises it:
 *   the install records securacv_ota_mark_pending_install() and the new
 *   image raises the floor to its own version once it is confirmed (its
 *   boot self-test, then the one-minute confirm dwell) — never before.
 *
 * Break-glass (the rescue case): a v1 header, or a v2 header whose version
 * is below the floor, is accepted only if the owner has armed break-glass
 * through the hook passed to configure(). The sketch wires that hook to the
 * existing physical-presence surface — the BOOT-button provisioning gate
 * (short tap, 30 s single-use window) — so a downgrade needs the same
 * hands-on-the-device act as revealing the provisioning receipt, and the
 * arming is consumed by the one BEGIN it admits. Every such acceptance
 * writes a health-log line naming the bypass. Product mismatch and an
 * invalid signature are never bypassed.
 *
 * The status characteristic notifies an 8-byte tuple:
 *   {state:u8, progress_pct:u8, bytes_left:u32, reserved:u16}
 */

#ifndef SECURACV_BLE_OTA_H
#define SECURACV_BLE_OTA_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "ble_ota_policy.h"  // OtaHeader / OtaHeaderV2 wire structs + the policy

class NimBLEServer;

namespace ble_ota {

enum OtaState : uint8_t {
  OTA_IDLE      = 0,
  OTA_RECEIVING = 1,
  OTA_VERIFYING = 2,
  OTA_REBOOTING = 3,
  OTA_FAILED    = 4
};

// UUIDs (different from the main SecuraCV service so peers can discover
// the OTA service independently).
static const char* OTA_SERVICE_UUID = "8fc1ced0-b162-4401-9607-c8ac21383e90";
static const char* OTA_CONTROL_UUID = "8fc1ced1-b162-4401-9607-c8ac21383e90";
static const char* OTA_DATA_UUID    = "8fc1ced2-b162-4401-9607-c8ac21383e90";
static const char* OTA_STATUS_UUID  = "8fc1ced3-b162-4401-9607-c8ac21383e90";

// ── The link a session belongs to (pure; test_ble_ota_policy.cpp) ──────────
//
// An accepted BEGIN records the connection it arrived on: its handle AND its
// peer address, since a handle the stack reuses for a later peer is not that
// link (ble_improv binds its claim the same way). While the session is in
// flight (receiving or verifying):
//   - DATA is taken from that link alone, so no other connection can put
//     bytes into the image;
//   - a control write (ABORT, a second BEGIN, an unknown command: each ends
//     the session) is taken from that link alone, so no other connection
//     can end it. The exception is a session whose link the stack no longer
//     holds: nothing can finish it, so the next writer may end it, as any
//     writer could before.
struct SessionLink {
  bool     bound       = false;
  uint16_t conn_handle = 0xFFFF;
  uint8_t  addr_type   = 0;
  uint8_t  addr[6]     = {0, 0, 0, 0, 0, 0};
};

inline void session_link_bind(SessionLink& s, uint16_t conn_handle, uint8_t addr_type,
                              const uint8_t addr[6]) {
  s.bound       = true;
  s.conn_handle = conn_handle;
  s.addr_type   = addr_type;
  memcpy(s.addr, addr, sizeof(s.addr));
}

inline void session_link_release(SessionLink& s) {
  s = SessionLink();
}

// Is (conn_handle, addr) the link the session was opened on? Never for an
// unbound session.
inline bool session_link_is(const SessionLink& s, uint16_t conn_handle, uint8_t addr_type,
                            const uint8_t addr[6]) {
  return s.bound && s.conn_handle == conn_handle && s.addr_type == addr_type &&
         memcmp(s.addr, addr, sizeof(s.addr)) == 0;
}

// May this control write be acted on? `session_link_held` is whether the
// stack still holds the session's link.
inline bool control_write_allowed(bool in_flight, bool from_session_link, bool session_link_held) {
  return !in_flight || from_session_link || !session_link_held;
}

// May this data write be taken into the image?
inline bool data_write_allowed(bool receiving, bool from_session_link) {
  return receiving && from_session_link;
}

// Register the OTA service on `server`. release_pubkey points to a 32-byte
// Ed25519 public key (typically SECURACV_OTA_RELEASE_PUBKEY from
// ota_release_key.h). If the key is all zeros, BEGIN refuses every
// request — OTA stays hard-disabled until a real key is provisioned.
bool init(NimBLEServer* server, const uint8_t release_pubkey[32]);

// Owner-armed break-glass probe. Returns true — and CONSUMES the arming,
// it is single-use — iff the owner has armed the rescue path right now.
// Called only when a BEGIN needs it (v1 header, or v2 below the floor), so
// a routine v2 update never touches the gate.
typedef bool (*BreakGlassHook)(void);

// Bind the policy inputs: the running product id (OTA_PRODUCT), the
// running firmware version (FIRMWARE_VERSION) and the break-glass hook.
// Call before init(); until both strings are set, every BEGIN is refused
// ("OTA policy not configured") — fail closed, like an unprovisioned key.
// A NULL hook means break-glass can never be armed on this build.
void configure(const char* product, const char* running_version, BreakGlassHook hook);

OtaState     get_state();
uint32_t     get_progress_percent();
uint32_t     get_image_size();
uint32_t     get_bytes_received();
const char*  last_error();
const char*  state_name(OtaState s);
// True if the most recent accepted BEGIN went through break-glass (the
// floor, or for a v1 header the product and floor, were not enforced).
bool         last_break_glass();

} // namespace ble_ota

#endif // SECURACV_BLE_OTA_H
