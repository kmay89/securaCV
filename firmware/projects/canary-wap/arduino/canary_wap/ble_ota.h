/*
 * SecuraCV Canary — BLE OTA over GATT (Ed25519-signed)
 *
 * GATT service that accepts firmware images streamed from a paired phone
 * or trusted client. Each image is verified against an Ed25519 signature
 * from the release engineer (see ota_release_key.h) before being written
 * to the inactive OTA partition and made bootable. A bad signature, a
 * SHA-256 mismatch, or an aborted transfer leaves the running firmware
 * untouched.
 *
 * Wire protocol (control characteristic, write):
 *
 *   byte 0   command
 *   command 0x01 BEGIN — followed by an OtaHeader (132 bytes)
 *   command 0x02 ABORT — no payload
 *
 *   After BEGIN, the client streams the raw image bytes to the data
 *   characteristic in chunks of (MTU - 3) bytes. Once `image_size` total
 *   bytes have been received, the server verifies the running SHA-256
 *   matches header.sha256, swaps the boot partition, and reboots.
 *
 * The status characteristic notifies an 8-byte tuple:
 *   {state:u8, progress_pct:u8, bytes_left:u32, reserved:u16}
 */

#ifndef SECURACV_BLE_OTA_H
#define SECURACV_BLE_OTA_H

#include <stdint.h>
#include <stddef.h>

class NimBLEServer;

namespace ble_ota {

enum OtaState : uint8_t {
  OTA_IDLE      = 0,
  OTA_RECEIVING = 1,
  OTA_VERIFYING = 2,
  OTA_REBOOTING = 3,
  OTA_FAILED    = 4
};

// 132-byte BEGIN payload. Packed so the wire layout is fixed regardless
// of compiler padding choices.
struct __attribute__((packed)) OtaHeader {
  uint32_t image_size;        // total firmware bytes to expect
  uint8_t  sha256[32];        // SHA-256 of the firmware image
  uint8_t  signature[64];     // Ed25519 signature over (image_size_LE32 || sha256)
  char     version[32];       // null-terminated version string (informational)
};
static_assert(sizeof(OtaHeader) == 132, "OtaHeader wire size must be 132 bytes");

// UUIDs (different from the main SecuraCV service so peers can discover
// the OTA service independently).
static const char* OTA_SERVICE_UUID = "8fc1ced0-b162-4401-9607-c8ac21383e90";
static const char* OTA_CONTROL_UUID = "8fc1ced1-b162-4401-9607-c8ac21383e90";
static const char* OTA_DATA_UUID    = "8fc1ced2-b162-4401-9607-c8ac21383e90";
static const char* OTA_STATUS_UUID  = "8fc1ced3-b162-4401-9607-c8ac21383e90";

// Register the OTA service on `server`. release_pubkey points to a 32-byte
// Ed25519 public key (typically SECURACV_OTA_RELEASE_PUBKEY from
// ota_release_key.h). If the key is all zeros, BEGIN refuses every
// request — OTA stays hard-disabled until a real key is provisioned.
bool init(NimBLEServer* server, const uint8_t release_pubkey[32]);

OtaState     get_state();
uint32_t     get_progress_percent();
uint32_t     get_image_size();
uint32_t     get_bytes_received();
const char*  last_error();
const char*  state_name(OtaState s);

} // namespace ble_ota

#endif // SECURACV_BLE_OTA_H
