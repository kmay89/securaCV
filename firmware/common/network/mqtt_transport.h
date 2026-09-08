// MQTT broker transport — the ESP32 half: the socket a PubSubClient rides.
//
// One object per firmware, replacing the `static WiFiClient wifiClient;`
// every mqtt_mgr.cpp used to hold. It reads the provisioned TLS mode, CA and
// pin from NVS, asks mqtt_transport_logic.h what the socket must be, and
// hands PubSubClient the right Client:
//
//   Plain          → a WiFiClient, exactly what shipped before
//   TlsCa          → WiFiClientSecure::setCACert(<provisioned PEM>)
//   TlsFingerprint → handshake without chain validation, then the peer
//                    certificate's SHA-256 must equal the pin BEFORE PubSubClient
//                    writes its first byte (PinnedClient::connect below), else
//                    the socket is closed and the connect reports failure
//   TlsInsecure    → WiFiClientSecure::setInsecure() — only when provisioned by
//                    name (Mode::InsecureLab), and prepare() hands the caller
//                    the warning it must log on EVERY connect
//   Refused        → prepare() returns false with the reason; nothing connects
//
// Header-only on purpose: it is included path-prefixed
// (`#include "network/mqtt_transport.h"`) through -I firmware/common, so
// there is no .cpp for a build_src_filter to forget (see
// scripts/lint_common_lib_manifests.py for why that matters), and the display
// stages it flat into its Arduino sketch the same way as wifi_join_policy.h.
//
// Not for the browser emulator: it needs the real WiFiClientSecure. The
// display's mqtt_mgr.cpp includes this only when EMU_BUILD_FLAVOR is unset.
//
// Dependency note: setCACert() keeps the POINTER it is given, so the PEM
// lives in this object (static storage in every consumer) for the life of
// the connection, never in a stack buffer.
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mqtt_transport_logic.h"

namespace canary::net::mqtt_tls {

// NVS keys, in the SAME namespace as the product's runtime_config
// (mqtt_host / mqtt_port / mqtt_user / mqtt_pass live in "securacv"), so
// the flashers seed all of them as one row. Contract shared with
// canary-local/assets/flash-core.js and desktop/src-tauri/src/provisioning.rs.
constexpr const char* NVS_KEY_MODE = "mqtt_tls";  // u8  — Mode
constexpr const char* NVS_KEY_CA   = "mqtt_ca";   // str — PEM certificate
constexpr const char* NVS_KEY_FP   = "mqtt_fp";   // str — SHA-256 pin, any accepted spelling

constexpr size_t kCaBufBytes = kCaPemMax + 1;          // 3072
constexpr size_t kFpBufBytes = kFingerprintTextLen + 1; // 96
constexpr unsigned long kHandshakeTimeoutSec = 15;      // RSA-4096 roots on an S3 take a few seconds

// WiFiClientSecure that refuses to stay connected to a peer whose
// certificate does not match the pin. PubSubClient calls Client::connect()
// through its base pointer, so overriding the two virtual overloads is
// enough to put the check between the handshake and the MQTT CONNECT packet.
class PinnedClient : public WiFiClientSecure {
 public:
  using WiFiClientSecure::connect;

  void set_pin(const char* canonical_fp) {
    if (canonical_fp == nullptr) { pin_[0] = '\0'; return; }
    strncpy(pin_, canonical_fp, sizeof(pin_) - 1);
    pin_[sizeof(pin_) - 1] = '\0';
  }
  void clear_pin() { pin_[0] = '\0'; }
  bool last_pin_mismatch() const { return mismatch_; }

  int connect(const char* host, uint16_t port) override {
    mismatch_ = false;
    const int rc = WiFiClientSecure::connect(host, port);
    if (rc == 0) return 0;
    if (pin_[0] != '\0' && !verify(pin_, nullptr)) {
      // Close before a single application byte leaves: the CONNECT packet
      // carries the broker username and password.
      stop();
      mismatch_ = true;
      return 0;
    }
    return rc;
  }

  int connect(IPAddress ip, uint16_t port) override {
    return connect(ip.toString().c_str(), port);
  }

 private:
  char pin_[kFpBufBytes] = {0};
  bool mismatch_ = false;
};

enum class Prepared : uint8_t { Ok, OkWarnInsecure, Refused };

class BrokerTransport {
 public:
  // Read mode / CA / pin from NVS namespace `ns` and decide. Safe to call
  // again after reprovisioning; a missing key means Plain (the pre-TLS
  // default), an unreadable NVS means Plain too — a device that cannot read
  // its settings has no secret to protect on this socket yet.
  const Decision& load(const char* ns) {
    uint8_t mode_byte = 0;
    ca_[0] = '\0';
    fp_[0] = '\0';
    Preferences prefs;
    if (prefs.begin(ns, /*readOnly=*/true)) {
      mode_byte = prefs.getUChar(NVS_KEY_MODE, 0);
      if (prefs.isKey(NVS_KEY_CA)) {
        // Preferences::getString(key, buf, cap) copies a value that fits and
        // leaves the buffer alone (returning 0) for one that does not — so an
        // oversize PEM reads back empty here and decide() refuses it as
        // CaMissing rather than handing mbedTLS a truncated certificate.
        prefs.getString(NVS_KEY_CA, ca_, sizeof(ca_));
        ca_[sizeof(ca_) - 1] = '\0';
      }
      if (prefs.isKey(NVS_KEY_FP)) {
        char raw[128] = {0};
        prefs.getString(NVS_KEY_FP, raw, sizeof(raw));
        raw[sizeof(raw) - 1] = '\0';
        if (fingerprint_normalize(raw, fp_, sizeof(fp_)) == 0) {
          // Keep the malformed text out of the object but remember the key
          // WAS set — including a value too long for `raw`, which
          // getString() leaves empty — so decide() reports
          // FingerprintMalformed, never Missing, for a pin that exists.
          strncpy(fp_, "?", sizeof(fp_) - 1);
        }
      }
      prefs.end();
    }
    mode_ = mode_from_u8(mode_byte);
    Caps caps;
    caps.fingerprint = true;
    caps.insecure = true;
    decision_ = decide_u8(mode_byte, ca_, fp_, caps);
    apply();
    return decision_;
  }

  const Decision& decision() const { return decision_; }
  Mode mode() const { return mode_; }
  const char* name() const { return transport_name(decision_.transport); }
  bool connect_allowed() const { return decision_.allowed(); }

  // The Client for PubSubClient::setClient(). Refused hands back the plain
  // client so the object is always bindable; prepare() is what stops a
  // Refused transport from ever connecting.
  Client& client() {
    return decision_.tls() ? static_cast<Client&>(secure_) : static_cast<Client&>(plain_);
  }

  // Call once per connect attempt, BEFORE PubSubClient::connect(). Fills
  // `msg` (bounded, secret-free) with the refusal reason or the mandatory
  // insecure warning; `msg` is left empty for a plain Ok.
  Prepared prepare(char* msg, size_t cap) {
    if (msg && cap) msg[0] = '\0';
    if (!decision_.allowed()) {
      format_failure(msg, cap, Transport::Refused, decision_.reason, 0, 0, false);
      return Prepared::Refused;
    }
    if (decision_.warn_insecure()) {
      if (msg && cap) {
        strncpy(msg, insecure_warning(), cap - 1);
        msg[cap - 1] = '\0';
      }
      return Prepared::OkWarnInsecure;
    }
    return Prepared::Ok;
  }

  // After a failed PubSubClient::connect(): one line naming the TLS reason
  // (pin mismatch, CA verify failure, wrong-port plaintext, ...). Returns
  // false when there is nothing TLS-specific to add (plain transport, or a
  // TLS socket that connected and failed at the MQTT layer instead).
  bool describe_failure(char* out, size_t cap) {
    if (out && cap) out[0] = '\0';
    if (!decision_.tls()) return false;
    char detail[128];
    // lastError() is the core's last start_ssl_client() result: NEGATIVE is
    // an mbedTLS error, non-negative is the socket fd of a handshake that
    // succeeded — after which a failure belongs to the MQTT layer (refused
    // credentials, dead broker), not to TLS, and must not be dressed up as
    // one. Only a real code or a pin mismatch earns a line here.
    const int raw = secure_.lastError(detail, sizeof(detail));
    const int err = raw < 0 ? raw : 0;
    const bool mismatch = secure_.last_pin_mismatch();
    if (err == 0 && !mismatch) return false;
    return format_failure(out, cap, decision_.transport, Reason::None, err, 0, mismatch) > 0;
  }

 private:
  void apply() {
    // Reset every verification setting before applying the current one, so
    // a reprovision from Fingerprint to Ca cannot leave a stale pin (or,
    // worse, a stale setInsecure) behind.
    secure_.clear_pin();
    secure_.setCACert(nullptr);
    secure_.setHandshakeTimeout(kHandshakeTimeoutSec);
    switch (decision_.transport) {
      case Transport::TlsCa:
        secure_.setCACert(ca_);
        break;
      case Transport::TlsFingerprint:
        // The ESP32 core has no "verify by fingerprint" handshake mode; the
        // documented pattern is an unvalidated handshake followed by
        // verify(fingerprint) on the peer certificate, which PinnedClient
        // enforces before any application data. The pin, not the CA, is the
        // trust anchor here.
        secure_.setInsecure();
        secure_.set_pin(fp_);
        break;
      case Transport::TlsInsecure:
        secure_.setInsecure();
        break;
      case Transport::Plain:
      case Transport::Refused:
        break;
    }
  }

  WiFiClient plain_;
  PinnedClient secure_;
  Decision decision_{};
  Mode mode_ = Mode::Plain;
  char ca_[kCaBufBytes] = {0};
  char fp_[kFpBufBytes] = {0};
};

}  // namespace canary::net::mqtt_tls
