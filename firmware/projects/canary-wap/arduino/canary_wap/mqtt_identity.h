/**
 * @file mqtt_identity.h
 * @brief The two identity strings the canary-wap's MQTT publishes carry,
 *        spelled in lowercase hex (sweep HA20).
 *
 * The envelope `fp` of every signed chain / counts / events publish is the
 * string canary_wap.ino hands device_signature::init, and the health
 * `public_key` is the string it hands csi_mqtt::init (at boot, and again
 * after a QR hub provision). Through firmware 2.4.15 both came from
 * canary_wap.ino's hex_to_str, which writes capitals, while every other
 * build (firmware/canary, canary-sense, canary-sentinel, canary-vision)
 * writes them in lowercase. Home Assistant and the canary-display's fleet
 * model had to learn to ignore the case (HA18, HA19), and they keep doing
 * so for the units still sending capitals.
 *
 * Only these two strings move. hex_to_str keeps its capitals, and so does
 * everything it spells: g_device.fingerprint_hex (the TLS certificate CN,
 * the provisioning receipt, /api/device-info, the BLE metadata), the TLS
 * certificate fingerprint, /api/status, serial `i`, the BLE witness export
 * and the BLE Opera Device Info `id`. device_signature also serves the
 * fingerprint it was handed on /enroll and /api/device/enroll, so those
 * print it in lowercase too, beside the key they already printed in
 * lowercase.
 *
 * Pure hosted C++ (no Arduino/ESP-IDF includes), so the spelling and the
 * sketch's call sites are host-tested (tests_host/test_mqtt_identity.cpp).
 * The array parameters fix the lengths: a call site that hands the wrong
 * buffer or the wrong byte array does not compile.
 */

#ifndef CANARY_WAP_MQTT_IDENTITY_H
#define CANARY_WAP_MQTT_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

namespace mqtt_identity {

constexpr size_t FP_BYTES    = 8;                  // g_device.pubkey_fp
constexpr size_t KEY_BYTES   = 32;                 // g_device.pubkey (Ed25519)
constexpr size_t FP_HEX_CAP  = 2 * FP_BYTES + 1;   // 16 hex digits + NUL
constexpr size_t KEY_HEX_CAP = 2 * KEY_BYTES + 1;  // 64 hex digits + NUL

// Lowercase hex, no separators; out holds 2 * n + 1 bytes. Not named HEX:
// Arduino's Print.h defines HEX as a macro.
inline void hex_lower(char* out, const uint8_t* in, size_t n) {
  static const char kLowerHex[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i]     = kLowerHex[in[i] >> 4];
    out[2 * i + 1] = kLowerHex[in[i] & 0x0F];
  }
  out[2 * n] = '\0';
}

// The envelope `fp`: the 8-byte pubkey fingerprint as 16 lowercase hex.
inline void fingerprint_hex(char (&out)[FP_HEX_CAP],
                            const uint8_t (&fp)[FP_BYTES]) {
  hex_lower(out, fp, FP_BYTES);
}

// The health `public_key`: the 32-byte Ed25519 key as 64 lowercase hex.
inline void public_key_hex(char (&out)[KEY_HEX_CAP],
                           const uint8_t (&pub)[KEY_BYTES]) {
  hex_lower(out, pub, KEY_BYTES);
}

}  // namespace mqtt_identity

#endif  // CANARY_WAP_MQTT_IDENTITY_H
