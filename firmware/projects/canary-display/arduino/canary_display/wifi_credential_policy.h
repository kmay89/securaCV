/**
 * @file wifi_credential_policy.h
 * @brief The ONE rule for a home Wi-Fi credential a Canary is handed, and the
 *        ONE set of words every surface shows beside the field. Pure: no
 *        Arduino, no heap, no I/O — host-tested by
 *        firmware/tests_host/test_wifi_credential_policy.cpp.
 *
 * WHAT THE FIELD MEANS
 *
 *   The password field on every setup surface — the shared SoftAP portal, the
 *   display's portal, the canary (PIO) wizard and dashboard, the WAP's wizard
 *   and dashboard, the Improv door, the iPhone app, the two flashers — is the
 *   password of the Wi-Fi network the person just picked, exactly as their
 *   router has it. It is never a password they are asked to invent. The words
 *   below say so in the same way everywhere (FIELD_LABEL, FIELD_HINT), so a
 *   person who met one Canary recognizes the next.
 *
 * THE RULE (bytes, not glyphs — the firmware buffers are byte arrays)
 *
 *   SSID      1..32 bytes.
 *   Password  0..64 bytes. 0 bytes is an open network and is accepted.
 *
 *   There is NO minimum length and NO complexity rule, on purpose. The router
 *   sets the rule for its own network: WPA2-PSK takes 8..63 characters, a raw
 *   PSK is 64 hex digits, WPA3-SAE accepts any length, WEP keys are 5 or 13
 *   characters. A client that guesses at the rule refuses real networks; the
 *   honest answer to a password the router rejects is the join verdict
 *   ("Wrong password", wifi_join_policy.h), reported live, with a retry.
 *   Clients used to add an 8-character minimum of their own — that is the
 *   drift this header ends.
 *
 *   No trimming either: a router keeps the spaces a person typed.
 *
 * TWINS (held to the same vectors by their own tests)
 *
 *   canary-local/assets/flash-core.js    wifiCredentialVerdict / WIFI_CREDENTIAL
 *   desktop/src/app.js                   the same block, inlined (desktop_parity pins it)
 *   desktop/flash-engine/src/provisioning.rs  validate()
 *   ios/Sources/SecuraCV/Model/SetupPortal.swift credentialProblem / passwordHint
 *
 *   The canary-wap Arduino sketch carries a byte-identical staged copy of this
 *   file (firmware/scripts/check_wifi_credential_sync.sh); the canary-display
 *   sketch stages it through setup.sh regen.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace canary {
namespace net {
namespace wifi_credential {

/// The firmware's own buffers: char[33] and char[65].
inline constexpr size_t SSID_MAX_BYTES     = 32;
inline constexpr size_t PASSWORD_MAX_BYTES = 64;
/// Stated once so no surface re-derives a floor: zero. An open network has
/// no password, and a secured network's floor is its router's business.
inline constexpr size_t PASSWORD_MIN_BYTES = 0;

enum class Verdict : uint8_t {
  Ok = 0,
  SsidEmpty,
  SsidTooLong,
  PasswordTooLong,
};

/// The rule over explicit lengths (a wire parser knows its lengths; a C
/// string has them counted below).
inline Verdict check(const char* ssid, size_t ssid_len,
                     const char* pass, size_t pass_len) {
  (void)ssid;
  (void)pass;
  if (ssid_len == 0) return Verdict::SsidEmpty;
  if (ssid_len > SSID_MAX_BYTES) return Verdict::SsidTooLong;
  if (pass_len > PASSWORD_MAX_BYTES) return Verdict::PasswordTooLong;
  return Verdict::Ok;
}

/// The rule over NUL-terminated strings; a null password is an open network.
inline Verdict check(const char* ssid, const char* pass) {
  const size_t sl = ssid ? strlen(ssid) : 0;
  const size_t pl = pass ? strlen(pass) : 0;
  return check(ssid, sl, pass, pl);
}

/// Machine word for a JSON `error` field. Stable: clients switch on it.
inline const char* verdict_code(Verdict v) {
  switch (v) {
    case Verdict::Ok:              return "ok";
    case Verdict::SsidEmpty:       return "ssid_empty";
    case Verdict::SsidTooLong:     return "ssid_too_long";
    case Verdict::PasswordTooLong: return "password_too_long";
  }
  return "ok";
}

// ── The words every surface shows ────────────────────────────────────────
// Changing a sentence here changes it on every portal page, the dashboard
// wizards, and (by test) the apps. No surface invents its own.

/// The label over, or placeholder in, the field.
inline constexpr const char* FIELD_LABEL =
    "Wi-Fi network password";
/// The one-line hint under it.
inline constexpr const char* FIELD_HINT =
    "The password you already use for this network, exactly as your router has it. Nothing to invent.";
/// Shown instead of the field when the picked network is open.
inline constexpr const char* OPEN_NETWORK_NOTE =
    "Open network — no password needed.";

/// Human words for a refused credential (the `reason` a portal answers).
inline const char* verdict_label(Verdict v) {
  switch (v) {
    case Verdict::Ok:              return "";
    case Verdict::SsidEmpty:       return "Enter the network name.";
    case Verdict::SsidTooLong:     return "A network name is at most 32 characters.";
    case Verdict::PasswordTooLong: return "A Wi-Fi password is at most 64 characters.";
  }
  return "";
}

}  // namespace wifi_credential
}  // namespace net
}  // namespace canary
