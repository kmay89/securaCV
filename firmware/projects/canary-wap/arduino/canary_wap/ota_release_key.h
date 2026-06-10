/*
 * SecuraCV — Release Signing Public Key (Ed25519)
 *
 * The 32-byte Ed25519 public key whose private half signs every firmware
 * image accepted over the air — both the pull-OTA engine (securacv_ota.h)
 * and the BLE OTA path (ble_ota.h) verify against this same key, so one
 * release signature serves every update channel.
 *
 * If this key is all zeros (the default), OTA installs are HARD-DISABLED —
 * the pull-OTA engine refuses to install and the BLE OTA BEGIN handler
 * refuses every request. Replace the bytes below with your own public key
 * before shipping a build that should accept OTA updates.
 *
 * Generating and embedding a keypair:
 *
 *     python firmware/scripts/ota_release.py keygen --private-key releaser.pem
 *     python firmware/scripts/ota_release.py pubkey-header \
 *       --private-key releaser.pem --out firmware/common/ota/src/ota_release_key.h
 *
 * The private key MUST live off-device and outside the repository — on a
 * release engineer's secure machine or in the OTA_SIGNING_KEY_PEM GitHub
 * Actions secret. The firmware-release workflow cross-checks that the
 * secret's public half matches this header before publishing a release.
 *
 * Signing a firmware image (what ota_release.py sign/manifest do):
 *
 *     1. SIZE = wc -c < firmware.bin   (4 bytes, little-endian)
 *     2. HASH = sha256(firmware.bin)   (32 bytes)
 *     3. SIG  = ed25519_sign(SIZE_LE32 || HASH, releaser.pem)   (64 bytes)
 *
 * Threat model: a stolen release private key permits firmware substitution.
 * Treat it like a code-signing certificate. Rotate by shipping a firmware
 * release (signed with the old key) that carries the new public key.
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
