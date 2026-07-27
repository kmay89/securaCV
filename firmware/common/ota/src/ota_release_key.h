/*
 * SecuraCV — Release Signing Public Key (Ed25519)
 *
 * The 32-byte Ed25519 public key whose private half signs every firmware
 * image accepted over the air (pull OTA and BLE OTA). The private key MUST
 * live off-device — on a release engineer's machine or in the
 * OTA_SIGNING_KEY_PEM CI secret; the device only needs the public half.
 *
 * If this key is all zeros (the default), OTA installs are HARD-DISABLED —
 * both the pull-OTA engine and the BLE OTA BEGIN handler refuse every
 * image. Generate and embed a real key with:
 *
 *     python firmware/scripts/ota_release.py keygen --private-key releaser.pem
 *     python firmware/scripts/ota_release.py pubkey-header --private-key releaser.pem
 *
 * Key id (sha256 of pubkey, first 16 hex): 532429078dc47c04
 *
 * Threat model: a stolen release private key permits firmware substitution.
 * Treat it like a code-signing certificate. Rotate by shipping a firmware
 * release (signed with the old key) that carries the new public key.
 */

#ifndef SECURACV_OTA_RELEASE_KEY_H
#define SECURACV_OTA_RELEASE_KEY_H

#include <stdint.h>

static const uint8_t SECURACV_OTA_RELEASE_PUBKEY[32] = {
  0xae, 0x12, 0x3a, 0x5c, 0x22, 0x0c, 0x54, 0x99,
  0xe5, 0x79, 0xdc, 0x44, 0x9e, 0x66, 0x39, 0x2d,
  0x83, 0xd5, 0xad, 0x70, 0xd1, 0x00, 0x74, 0x75,
  0x74, 0xeb, 0x7b, 0x2b, 0xac, 0x49, 0xb2, 0x44,
};

#endif // SECURACV_OTA_RELEASE_KEY_H
