// ap_security_policy.h — what security the SoftAP (and the STA link) asks
// the Wi-Fi driver for (F16), kept pure so the choice is host-tested and both
// firmware trees make the same one.
//
// Pure: no Arduino, no esp_wifi. Canonical here; the canary (PIO) tree
// includes it from firmware/common, and the canary-wap Arduino sketch carries
// a byte-identical staged copy next to the .ino
// (firmware/scripts/check_ap_security_sync.sh fails CI on drift). Host test:
// firmware/tests_host/test_ap_security_policy.cpp.
//
// The decision (option (b) — maintainer to confirm):
//   * WPA2/WPA3 transition on the SoftAP when the build asks for it
//     (CANARY_AP_WPA3_TRANSITION), the core was built with SoftAP SAE, and the
//     passphrase is a valid WPA2 one (>= 8 chars). SAE-capable phones get
//     WPA3; everything else still joins on WPA2 — nobody is locked out.
//   * PMF (802.11w) CAPABLE, never REQUIRED, in transition mode: a required
//     PMF drops every WPA2 client that lacks it, which is exactly the
//     lock-out transition mode exists to avoid.
//   * Otherwise plain WPA2-PSK, with the reason named, so a device that could
//     not do WPA3 says why in /api/wifi/status and /api/status (ap_auth).
//   * STA: PMF capable, not required — only written when the driver does not
//     already say capable, so a live association is not needlessly restarted.
//
// NOT decided here (a separate maintainer decision): the canary (PIO) AP
// passphrase is 8 characters ("cv-" + 5), derived from the key fingerprint on
// every boot. Widening it silently changes the Wi-Fi password of every
// provisioned device after an OTA; it needs a derivation-version marker first.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace canary {
namespace net {
namespace ap_security {

enum class AuthMode : uint8_t {
  WPA2_PSK,              // the driver default the Arduino softAP() call sets
  WPA2_WPA3_TRANSITION,  // WIFI_AUTH_WPA2_WPA3_PSK: SAE for capable clients, PSK for the rest
};

struct Decision {
  AuthMode mode;
  bool pmf_capable;
  bool pmf_required;
  const char* label;   // the ap_auth value the status endpoints report
  const char* reason;  // why (string literal, never freed)
};

// The WPA2 passphrase floor (IEEE 802.11i: 8..63 characters).
static const size_t kMinPassphrase = 8;

inline Decision decide(bool wpa3_knob, bool sae_in_build, size_t password_len) {
  Decision d;
  d.mode = AuthMode::WPA2_PSK;
  d.pmf_capable = true;   // advertised whenever the driver takes it
  d.pmf_required = false; // never: a required PMF locks WPA2-only clients out
  d.label = "wpa2";
  if (!wpa3_knob) {
    d.reason = "CANARY_AP_WPA3_TRANSITION=0 in this build";
  } else if (!sae_in_build) {
    d.reason = "core sdkconfig lacks SoftAP SAE";
  } else if (password_len < kMinPassphrase) {
    d.reason = "passphrase shorter than the 8-character WPA2 floor";
  } else {
    d.mode = AuthMode::WPA2_WPA3_TRANSITION;
    d.label = "wpa2-wpa3";
    d.reason = "WPA2/WPA3 transition, PMF capable";
  }
  return d;
}

// The label after the driver answered: a transition request the driver
// refused (esp_wifi_set_config != ESP_OK) is reported as what is actually
// on the air — WPA2 — with its own reason.
inline Decision after_driver(const Decision& asked, bool driver_accepted) {
  if (asked.mode != AuthMode::WPA2_WPA3_TRANSITION || driver_accepted) return asked;
  Decision d = asked;
  d.mode = AuthMode::WPA2_PSK;
  d.label = "wpa2";
  d.reason = "driver refused WPA2/WPA3 transition; WPA2-PSK kept";
  return d;
}

// Label for an ESP-IDF wifi_auth_mode_t value, for the scan JSON and the
// status endpoints. The numbers are ESP-IDF's (stable since IDF 4.x); the
// device-side translation unit static_asserts the ones it relies on against
// the real enum, so a renumbering fails the build instead of mislabeling.
inline const char* label_for(int esp_auth_mode) {
  switch (esp_auth_mode) {
    case 0:  return "open";        // WIFI_AUTH_OPEN
    case 1:  return "wep";         // WIFI_AUTH_WEP
    case 2:  return "wpa";         // WIFI_AUTH_WPA_PSK
    case 3:  return "wpa2";        // WIFI_AUTH_WPA2_PSK
    case 4:  return "wpa-wpa2";    // WIFI_AUTH_WPA_WPA2_PSK
    case 5:  return "enterprise";  // WIFI_AUTH_WPA2_ENTERPRISE / WIFI_AUTH_ENTERPRISE
    case 6:  return "wpa3";        // WIFI_AUTH_WPA3_PSK
    case 7:  return "wpa2-wpa3";   // WIFI_AUTH_WPA2_WPA3_PSK
    case 8:  return "wapi";        // WIFI_AUTH_WAPI_PSK
    case 9:  return "owe";         // WIFI_AUTH_OWE
    case 10: return "enterprise";  // WIFI_AUTH_WPA3_ENT_192
    default: return "other";
  }
}

// Numeric constants the device side static_asserts against esp_wifi_types.h.
static const int kAuthOpen = 0;
static const int kAuthWpa2Psk = 3;
static const int kAuthWpa2Wpa3Psk = 7;

}  // namespace ap_security
}  // namespace net
}  // namespace canary
