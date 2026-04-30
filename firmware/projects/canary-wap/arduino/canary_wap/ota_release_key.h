/*
 * SecuraCV BLE OTA — Release Signing Public Key (Ed25519)
 *
 * The 32-byte Ed25519 public key whose private half signs every firmware
 * image accepted via BLE OTA. The private key MUST live off-device, on a
 * release engineer's secure machine; the device only needs the public half
 * to verify signatures.
 *
 * If this key is all zeros (the default), BLE OTA is HARD-DISABLED — the
 * BEGIN handler refuses every request. Replace the bytes below with your
 * own public key before shipping a build that should accept OTA updates.
 *
 * Generating a keypair (Linux / macOS):
 *
 *     openssl genpkey -algorithm Ed25519 -out releaser.pem
 *     openssl pkey -in releaser.pem -pubout -outform DER \
 *       | tail -c 32 | xxd -i
 *
 * Copy the 32 hex bytes from xxd into SECURACV_OTA_RELEASE_PUBKEY below.
 *
 * Signing a firmware image (the release process):
 *
 *     1. SIZE = wc -c < firmware.bin   (4 bytes, little-endian)
 *     2. HASH = sha256(firmware.bin)   (32 bytes)
 *     3. SIG  = ed25519_sign(SIZE_LE32 || HASH, releaser.pem)   (64 bytes)
 *     4. The OTA client writes BEGIN { SIZE, HASH, SIG, version_str }
 *        to the OTA control characteristic, then streams the image bytes
 *        to the data characteristic.
 *
 * Threat model: a stolen release private key permits firmware substitution
 * over BLE. Treat it like a code-signing certificate. Rotate by reflashing
 * a new SECURACV_OTA_RELEASE_PUBKEY constant (which itself requires the
 * old one to OTA — or USB).
 */

#ifndef SECURACV_OTA_RELEASE_KEY_H
#define SECURACV_OTA_RELEASE_KEY_H

#include <stdint.h>

static const uint8_t SECURACV_OTA_RELEASE_PUBKEY[32] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#endif // SECURACV_OTA_RELEASE_KEY_H
