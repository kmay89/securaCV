// ap_password.h — the key of a Canary's own setup network (its SoftAP),
// derived from the device's PRIVATE key (sweep F350).
//
// Why the private key. A setup network's key is only a secret if nothing the
// device publishes can compute it. The flagship's first derivation read the
// public-key fingerprint, and the device publishes that fingerprint: the
// setup network's SSID suffix spells two of its bytes, every signed event
// body it sends over MQTT carries all eight (`fp`), and so do the
// provisioning receipt, `/api/status` and the serial log. On the flagship a
// peer on its own SoftAP is handed the page token, so that key was the whole
// boundary. An HMAC of the private key under its own domain label is a real
// per-device secret, the same on every boot, and needs no storage.
//
// Why the parameter is `const uint8_t (&)[32]`. A pointer parameter would
// take the 8-byte fingerprint too (the array decays) and read 24 bytes past
// it, so a revert of the call to `device.pubkey_fp` would compile without a
// word. An array reference refuses anything that is not exactly 32 bytes;
// tests_host/test_ap_password.cpp static_asserts both directions. (The
// 32-byte public key still binds; what pins the argument to the private key
// is the call site in firmware/canary/src/main.cpp's setup().)
//
// This is canary-wap's derive_ap_password (canary_wap.ino, "DEVICE-UNIQUE AP
// PASSWORD"): the same domain label, 12 characters of the same unambiguous
// alphabet, "cv-" + 12 = 15 characters. test_ap_password.cpp cuts the WAP's
// function out of the sketch and holds the two equal over 256 keys. The one
// place they can differ: when fewer than 12 of the 32 HMAC bytes are below
// 216, the WAP pads with '2' and render_password tops up with alphabet[i];
// for a uniform HMAC that is a binomial tail of about 3e-10.
//
// Why it can refuse a buffer that is big enough. fleet_key's HMAC zeroes its
// output when it fails (on the device, an allocation failure inside
// mbedtls_md_hmac), and 32 zero bytes render as "cv-222222222222": one key
// for every unit, on the network whose peers are handed the page token. So
// this takes fleet_key::hmac_sha256_checked and refuses (false, "") when the
// HMAC did not run; the result is [[nodiscard]], and setup() halts on a
// refusal rather than raise the setup network on an empty or constant key.
// test_ap_password_hmac_fail.cpp holds that with a failing HMAC().
//
// Pure: HMAC-SHA256 through fleet_key.h (mbedTLS on the device, OpenSSL on
// the host, RFC 4231-tested in test_fleet_key.cpp) and the rendering through
// provision_core.h's render_password. No Arduino, no NVS.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "fleet_key.h"       // hmac_sha256_checked
#include "provision_core.h"  // render_password: the unambiguous alphabet

namespace canary {
namespace net {

static constexpr const char AP_PASSWORD_DOMAIN[] = "securacv:ap-password:v1";
static constexpr size_t AP_PASSWORD_DOMAIN_LEN = sizeof(AP_PASSWORD_DOMAIN) - 1;  // 23
static constexpr size_t AP_PASSWORD_CHARS = 12;                     // ~69 bits
static constexpr size_t AP_PASSWORD_LEN = 3 + AP_PASSWORD_CHARS;    // "cv-" + 12 = 15

/// Writes "cv-" + 12 characters, NUL-terminated, into `out`. Refuses (false)
/// a null `out` or a `cap` below AP_PASSWORD_LEN + 1, writing "" when there
/// is room for it: a truncated key would be a different, weaker key. Refuses
/// the same way when the HMAC fails: a key rendered from its zeroed output
/// would be the same on every unit.
[[nodiscard]] inline bool derive_ap_password(const uint8_t (&privkey)[32], char* out,
                                             size_t cap) {
  if (out == nullptr || cap < AP_PASSWORD_LEN + 1) {
    if (out != nullptr && cap >= 1) out[0] = '\0';
    return false;
  }
  out[0] = '\0';
  uint8_t k[fleet_key::HASH_BYTES];
  const bool ran = fleet_key::hmac_sha256_checked(
      privkey, sizeof(privkey), reinterpret_cast<const uint8_t*>(AP_PASSWORD_DOMAIN),
      AP_PASSWORD_DOMAIN_LEN, k);
  char enc[AP_PASSWORD_CHARS + 1] = {0};
  if (ran) {
    render_password(k, sizeof(k), enc, sizeof(enc));
    snprintf(out, cap, "cv-%s", enc);
  }
  // Wipe both transient copies (DCE-safe): the key lives on only in `out`.
  volatile uint8_t* vk = k;
  for (size_t i = 0; i < sizeof(k); i++) vk[i] = 0;
  volatile char* ve = enc;
  for (size_t i = 0; i < sizeof(enc); i++) ve[i] = 0;
  return ran;
}

}  // namespace net
}  // namespace canary
