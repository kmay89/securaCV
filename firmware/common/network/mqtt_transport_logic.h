// MQTT broker transport decision — the pure half, dependency-free and
// host-testable (firmware/tests_host/test_mqtt_transport_logic.cpp).
//
// Every Canary that speaks MQTT used to open a plain TCP socket to the
// broker, full stop: the broker username and password crossed the LAN in the
// clear on every connect, and nothing in any firmware could be told to do
// otherwise (canary-wap had a `tls` bool that produced an mqtts:// URI with
// no CA, i.e. an UNVERIFIED handshake — which is worse than honest plaintext,
// because it looks secured). This header is the one place the fleet decides
// what the broker socket will be, so the four clients cannot drift:
//
//   Mode (what the operator provisioned)      Transport (what the socket does)
//   ────────────────────────────────────      ─────────────────────────────────
//   Plain            (default, unchanged)  →  Plain
//   Ca + PEM                               →  TlsCa          (chain-verified)
//   Fingerprint + 32-byte SHA-256 pin      →  TlsFingerprint (cert pinned)
//   InsecureLab      (explicit opt-in)     →  TlsInsecure    (warns EVERY connect)
//   anything incomplete or unknown         →  Refused        (fail closed)
//
// The rule that matters: a TLS mode with a missing or malformed CA / pin is
// REFUSED, never quietly downgraded to an unverified handshake. The only way
// to get an unverified socket is to ask for one by name (Mode::InsecureLab),
// and the transport that grants it must log a warning on every connect. The
// I/O halves — network/mqtt_transport.h (WiFiClientSecure, the PubSubClient
// products) and canary-wap's csi_mqtt.cpp (esp_mqtt) — apply this decision;
// they do not make one of their own.
//
// Nothing here ever formats the CA, the pin, or a credential into a string:
// every text this header returns is a constant, so it is safe to print on
// any diagnostic channel.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace canary::net::mqtt_tls {

// ── Provisioned intent ─────────────────────────────────────────────────────
// Persisted as one NVS u8 (products: "securacv"/mqtt_tls; canary-wap:
// "csi"/mqtt.tlsmode). The numeric values are the wire/NVS contract shared
// with the flashers (canary-local/assets/flash-core.js mqttProvisioningToNvs,
// desktop/src-tauri/src/provisioning.rs) — do not renumber.
enum class Mode : uint8_t {
  Plain = 0,        // mqtt://  — what every Canary shipped with
  Ca = 1,           // mqtts:// — broker cert must chain to the configured CA (PEM)
  Fingerprint = 2,  // mqtts:// — broker cert's SHA-256 fingerprint must equal the pin
  InsecureLab = 3,  // mqtts:// — NO peer verification. Lab brokers only; warns each connect
};
constexpr uint8_t kModeCount = 4;

// ── What the socket will actually do ───────────────────────────────────────
enum class Transport : uint8_t { Plain, TlsCa, TlsFingerprint, TlsInsecure, Refused };

// Why a Refused decision was refused (or None). Also the vocabulary the
// clients use to name a failure on their log channel.
enum class Reason : uint8_t {
  None = 0,
  ModeUnknown,             // persisted mode byte outside the table
  CaMissing,               // Mode::Ca with no PEM
  CaMalformed,             // PEM without the BEGIN/END CERTIFICATE armor
  FingerprintMissing,      // Mode::Fingerprint with no pin
  FingerprintMalformed,    // pin is not 32 hex bytes
  FingerprintUnsupported,  // this transport cannot pin (esp_mqtt has no hook)
  InsecureUnsupported,     // this transport cannot skip verification
};

struct Settings {
  Mode mode = Mode::Plain;
  const char* ca_pem = nullptr;      // PEM text, or nullptr / ""
  const char* fingerprint = nullptr; // "AA:BB:...", "aabb...", or nullptr / ""
};

// What the I/O half can do. WiFiClientSecure: both true. esp_mqtt: no
// fingerprint hook; insecure only when the core's esp-tls allows it.
struct Caps {
  bool fingerprint = true;
  bool insecure = true;
};

struct Decision {
  Transport transport = Transport::Refused;
  Reason reason = Reason::ModeUnknown;

  bool allowed() const { return transport != Transport::Refused; }
  bool tls() const {
    return transport == Transport::TlsCa || transport == Transport::TlsFingerprint ||
           transport == Transport::TlsInsecure;
  }
  // The mandatory per-connect warning applies exactly here.
  bool warn_insecure() const { return transport == Transport::TlsInsecure; }
};

// ── Small helpers ──────────────────────────────────────────────────────────

// Persisted byte → Mode. `known` (optional) reports whether the byte was in
// the table; an unknown byte maps to Plain here only so callers have SOMETHING
// to display — decide() below refuses it regardless.
inline Mode mode_from_u8(uint8_t v, bool* known = nullptr) {
  const bool ok = v < kModeCount;
  if (known) *known = ok;
  return ok ? static_cast<Mode>(v) : Mode::Plain;
}

inline const char* mode_name(Mode m) {
  switch (m) {
    case Mode::Plain:       return "plain";
    case Mode::Ca:          return "tls-ca";
    case Mode::Fingerprint: return "tls-fingerprint";
    case Mode::InsecureLab: return "tls-insecure-lab";
  }
  return "unknown";
}

inline const char* transport_name(Transport t) {
  switch (t) {
    case Transport::Plain:          return "plain";
    case Transport::TlsCa:          return "tls-ca";
    case Transport::TlsFingerprint: return "tls-fingerprint";
    case Transport::TlsInsecure:    return "tls-insecure";
    case Transport::Refused:        return "refused";
  }
  return "refused";
}

// URI scheme for transports that build one (esp_mqtt). Refused has none.
inline const char* uri_scheme(Transport t) {
  return (t == Transport::Plain || t == Transport::Refused) ? "mqtt" : "mqtts";
}

// The port a mode implies when the operator left it at zero.
inline uint16_t default_port(Mode m) { return m == Mode::Plain ? 1883 : 8883; }

inline bool is_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline size_t cstr_len(const char* s) {
  size_t n = 0;
  if (s) while (s[n] != '\0') n++;
  return n;
}

inline bool contains(const char* hay, const char* needle) {
  if (!hay || !needle) return false;
  const size_t nl = cstr_len(needle);
  if (nl == 0) return true;
  for (size_t i = 0; hay[i] != '\0'; i++) {
    size_t j = 0;
    while (j < nl && hay[i + j] != '\0' && hay[i + j] == needle[j]) j++;
    if (j == nl) return true;
  }
  return false;
}

// A PEM we are willing to hand to mbedTLS: non-empty, armored, and not
// absurdly large (NVS strings top out under 4 KB; a 4096-bit RSA root is
// ~2 KB). mbedTLS does the real parse — this only stops the obvious
// paste accidents (a fingerprint in the CA box, a truncated upload) from
// reaching the handshake as a mystery -0x2180.
constexpr size_t kCaPemMax = 3071;  // + NUL = the 3072-byte buffers downstream
inline bool ca_pem_looks_valid(const char* pem) {
  if (pem == nullptr || pem[0] == '\0') return false;
  const size_t n = cstr_len(pem);
  if (n > kCaPemMax) return false;
  return contains(pem, "-----BEGIN CERTIFICATE-----") &&
         contains(pem, "-----END CERTIFICATE-----");
}

// Canonical pin text: 32 uppercase hex pairs joined by ':' (95 chars + NUL).
// The ESP32 core's WiFiClientSecure::verify() hashes the peer's DER
// certificate with SHA-256 (32 bytes — not the 20-byte SHA-1 the ESP8266
// core uses), and parses exactly this pair-with-separators spelling. Get the
// value with:  openssl x509 -in broker.crt -noout -fingerprint -sha256
constexpr size_t kFingerprintBytes = 32;
constexpr size_t kFingerprintTextLen = kFingerprintBytes * 3 - 1;  // 95

// Accepts "AA:BB:...", "AA BB ...", "aabb..." (any mix of separators ':' or
// ' ' between pairs, none inside a pair). Writes the canonical form to `out`
// when it fits; returns the canonical length (95) or 0 if the input is not a
// 32-byte fingerprint. `out` may be nullptr to validate only.
inline size_t fingerprint_normalize(const char* in, char* out, size_t cap) {
  if (in == nullptr) return 0;
  char buf[kFingerprintTextLen + 1];
  size_t bytes = 0;
  size_t i = 0;
  while (in[i] != '\0') {
    if (in[i] == ':' || in[i] == ' ') { i++; continue; }
    if (!is_hex(in[i]) || !is_hex(in[i + 1])) return 0;
    if (bytes >= kFingerprintBytes) return 0;
    auto up = [](char c) -> char { return (c >= 'a' && c <= 'f') ? (char)(c - 'a' + 'A') : c; };
    const size_t o = bytes * 3;
    if (bytes > 0) buf[o - 1] = ':';
    buf[o] = up(in[i]);
    buf[o + 1] = up(in[i + 1]);
    bytes++;
    i += 2;
  }
  if (bytes != kFingerprintBytes) return 0;
  buf[kFingerprintTextLen] = '\0';
  if (out != nullptr) {
    if (cap < kFingerprintTextLen + 1) return 0;
    for (size_t k = 0; k <= kFingerprintTextLen; k++) out[k] = buf[k];
  }
  return kFingerprintTextLen;
}

inline bool fingerprint_is_valid(const char* fp) {
  return fingerprint_normalize(fp, nullptr, 0) == kFingerprintTextLen;
}

// ── The decision ───────────────────────────────────────────────────────────
inline Decision decide(const Settings& s, const Caps& caps) {
  Decision d;
  switch (s.mode) {
    case Mode::Plain:
      d.transport = Transport::Plain;
      d.reason = Reason::None;
      return d;

    case Mode::Ca:
      if (s.ca_pem == nullptr || s.ca_pem[0] == '\0') {
        d.transport = Transport::Refused; d.reason = Reason::CaMissing; return d;
      }
      if (!ca_pem_looks_valid(s.ca_pem)) {
        d.transport = Transport::Refused; d.reason = Reason::CaMalformed; return d;
      }
      d.transport = Transport::TlsCa; d.reason = Reason::None; return d;

    case Mode::Fingerprint:
      if (!caps.fingerprint) {
        d.transport = Transport::Refused; d.reason = Reason::FingerprintUnsupported; return d;
      }
      if (s.fingerprint == nullptr || s.fingerprint[0] == '\0') {
        d.transport = Transport::Refused; d.reason = Reason::FingerprintMissing; return d;
      }
      if (!fingerprint_is_valid(s.fingerprint)) {
        d.transport = Transport::Refused; d.reason = Reason::FingerprintMalformed; return d;
      }
      d.transport = Transport::TlsFingerprint; d.reason = Reason::None; return d;

    case Mode::InsecureLab:
      if (!caps.insecure) {
        d.transport = Transport::Refused; d.reason = Reason::InsecureUnsupported; return d;
      }
      d.transport = Transport::TlsInsecure; d.reason = Reason::None; return d;
  }
  d.transport = Transport::Refused;
  d.reason = Reason::ModeUnknown;
  return d;
}

// Same entry point from the persisted byte (unknown byte → Refused).
inline Decision decide_u8(uint8_t mode_byte, const char* ca_pem, const char* fingerprint,
                          const Caps& caps) {
  bool known = false;
  Settings s;
  s.mode = mode_from_u8(mode_byte, &known);
  s.ca_pem = ca_pem;
  s.fingerprint = fingerprint;
  if (!known) {
    Decision d;
    d.transport = Transport::Refused;
    d.reason = Reason::ModeUnknown;
    return d;
  }
  return decide(s, caps);
}

// ── Operator-facing text (constants only — never the secret) ───────────────
inline const char* reason_text(Reason r) {
  switch (r) {
    case Reason::None:
      return "";
    case Reason::ModeUnknown:
      return "broker TLS mode byte is not one this firmware knows - refusing to connect until it is reprovisioned (0 plain, 1 CA, 2 fingerprint, 3 lab)";
    case Reason::CaMissing:
      return "broker TLS mode is CA-verified but no CA certificate is provisioned - refusing to connect (provide the broker's CA in PEM form, or pick a different mode)";
    case Reason::CaMalformed:
      return "provisioned broker CA is not a PEM certificate (needs the BEGIN/END CERTIFICATE lines) - refusing to connect";
    case Reason::FingerprintMissing:
      return "broker TLS mode is fingerprint-pinned but no fingerprint is provisioned - refusing to connect";
    case Reason::FingerprintMalformed:
      return "provisioned broker fingerprint is not a 32-byte SHA-256 (64 hex digits, ':' separators optional) - refusing to connect";
    case Reason::FingerprintUnsupported:
      return "fingerprint pinning is not available on this device's MQTT transport - use the CA mode";
    case Reason::InsecureUnsupported:
      return "unverified TLS is not available on this build - use the CA mode";
  }
  return "";
}

// The warning the transport MUST emit on every connect in TlsInsecure.
inline const char* insecure_warning() {
  return "WARNING: broker TLS is in lab mode - the socket is encrypted but the broker is NOT verified; anyone on this LAN can impersonate it. Provision a CA for anything that matters.";
}

// ── Failure explanation (mbedTLS / esp-tls codes → words) ──────────────────
// The PubSubClient products get one integer back from WiFiClientSecure
// (lastError()), the wap gets esp-tls's stack error plus the X.509 verify
// flags. Both are opaque to a person reading a serial log. These map the
// codes worth recognizing to the fix; anything else stays a numbered code
// so the log is still actionable. Values are the mbedTLS constants, copied
// here so the header stays dependency-free on the host.
constexpr int kMbedNetConnectFailed  = -0x0044;  // MBEDTLS_ERR_NET_CONNECT_FAILED
constexpr int kMbedNetRecvFailed     = -0x004C;  // MBEDTLS_ERR_NET_RECV_FAILED
constexpr int kMbedNetConnReset      = -0x0050;  // MBEDTLS_ERR_NET_CONN_RESET
constexpr int kMbedX509VerifyFailed  = -0x2700;  // MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
constexpr int kMbedX509BadFormat     = -0x2180;  // MBEDTLS_ERR_X509_INVALID_FORMAT
constexpr int kMbedSslBadInput       = -0x7100;  // MBEDTLS_ERR_SSL_BAD_INPUT_DATA
constexpr int kMbedSslInvalidRecord  = -0x7200;  // MBEDTLS_ERR_SSL_INVALID_RECORD
constexpr int kMbedSslConnEof        = -0x7280;  // MBEDTLS_ERR_SSL_CONN_EOF
constexpr int kMbedSslFatalAlert     = -0x7780;  // MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE
constexpr int kMbedSslPeerClose      = -0x7880;  // MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY
constexpr int kMbedSslHandshakeFail  = -0x7980;  // MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE

inline const char* handshake_error_text(int mbedtls_err) {
  switch (mbedtls_err) {
    case kMbedNetConnectFailed:  return "TCP connect to the broker failed (host or port unreachable)";
    case kMbedNetRecvFailed:
    case kMbedNetConnReset:      return "the broker closed the connection during the TLS handshake (is that port really a TLS listener?)";
    case kMbedX509VerifyFailed:  return "the broker's certificate did not verify against the provisioned CA (wrong CA, expired certificate, or the certificate is for a different name)";
    case kMbedX509BadFormat:     return "the provisioned CA certificate could not be parsed (paste the full PEM, BEGIN to END)";
    case kMbedSslBadInput:       return "TLS was configured without a CA or pin (this is a firmware bug - the decision layer should have refused)";
    case kMbedSslInvalidRecord:  return "the broker did not speak TLS on this port (a plain MQTT listener on a TLS-configured port, usually 1883 vs 8883)";
    case kMbedSslConnEof:
    case kMbedSslPeerClose:      return "the broker ended the TLS handshake early (it may require a client certificate, or reject this device's cipher suites)";
    case kMbedSslFatalAlert:     return "the broker sent a fatal TLS alert (often: it requires a client certificate, or a newer TLS version than this device offers)";
    case kMbedSslHandshakeFail:  return "TLS handshake failed (no common cipher suite or protocol version with the broker)";
    default:                     return nullptr;
  }
}

// X.509 verify flags (mbedTLS MBEDTLS_X509_BADCERT_*), as esp-tls reports
// them in esp_mqtt's error handle. Returns the most useful single reason.
constexpr uint32_t kX509Expired     = 0x01;
constexpr uint32_t kX509Revoked     = 0x02;
constexpr uint32_t kX509CnMismatch  = 0x04;
constexpr uint32_t kX509NotTrusted  = 0x08;
constexpr uint32_t kX509Future      = 0x200;

inline const char* verify_flags_text(uint32_t flags) {
  if (flags == 0) return nullptr;
  if (flags & kX509NotTrusted) return "the broker's certificate does not chain to the provisioned CA (wrong CA, or the broker is not the one you provisioned)";
  if (flags & kX509CnMismatch) return "the broker's certificate is for a different host name than the one this device connects to";
  if (flags & kX509Expired)    return "the broker's certificate has expired";
  if (flags & kX509Future)     return "the broker's certificate is not valid yet (check the device clock and the broker's certificate dates)";
  if (flags & kX509Revoked)    return "the broker's certificate is revoked";
  return "the broker's certificate failed verification";
}

// One bounded, NUL-terminated line for a log channel: "<transport>: <why>
// (mbedtls -0xNNNN)". Never includes the CA, the pin, or a credential.
// Returns chars written (0 on bad args).
inline size_t format_failure(char* out, size_t cap, Transport t, Reason refused,
                             int mbedtls_err, uint32_t verify_flags,
                             bool fingerprint_mismatch) {
  if (out == nullptr || cap == 0) return 0;
  const char* why = nullptr;
  if (t == Transport::Refused) why = reason_text(refused);
  else if (fingerprint_mismatch) why = "the broker's certificate fingerprint does not match the provisioned pin (rotated certificate, or a different broker answering)";
  else if ((why = verify_flags_text(verify_flags)) == nullptr) why = handshake_error_text(mbedtls_err);
  if (why == nullptr) {
    if (mbedtls_err == 0) { out[0] = '\0'; return 0; }
    why = "TLS handshake failed";
  }
  // Manual, dependency-free formatting: "<name>: <why>[ (mbedtls -0xNNNN)]".
  size_t o = 0;
  auto put = [&](const char* s) {
    for (size_t i = 0; s[i] != '\0' && o + 1 < cap; i++) out[o++] = s[i];
  };
  put(transport_name(t));
  put(": ");
  put(why);
  if (mbedtls_err != 0) {
    put(" (mbedtls -0x");
    const unsigned v = (unsigned)(-mbedtls_err) & 0xFFFFu;
    static const char kHex[] = "0123456789ABCDEF";
    char hex[5] = {kHex[(v >> 12) & 0xF], kHex[(v >> 8) & 0xF], kHex[(v >> 4) & 0xF], kHex[v & 0xF], '\0'};
    put(hex);
    put(")");
  }
  out[o] = '\0';
  return o;
}

}  // namespace canary::net::mqtt_tls
