// tls_policy.h — the decisions behind the canary's self-signed HTTPS surface
// (F15), kept pure so they are host-tested rather than hoped.
//
// Pure: no Arduino, no httpd, no mbedTLS. firmware/canary's securacv_network
// consumes it; firmware/tests_host/test_tls_policy.cpp pins every row.
//
//   decide()                  — HTTPS-with-port-80-redirect or HTTP-only, and
//                               the reason, from four facts the firmware knows
//                               at start-up. The reason is what /api/status
//                               reports, so a device that fell back to HTTP
//                               says why instead of leaving the bench to guess.
//   build_redirect_location() — the port-80 server's Location header. Refuses
//                               (returns false) on truncation or on a
//                               MALFORMED target (userinfo, a path, whitespace
//                               or a control byte in the host), so the handler
//                               answers 500 instead of a truncated or
//                               header-splitting redirect (the WAP's fixed
//                               128-byte snprintf silently truncates; this does
//                               not copy that). It does not check that a
//                               well-formed Host names THIS device: `Host:
//                               other.example` is echoed back to the browser
//                               that sent it, as any Host-based redirect is.
//   live_reason()             — the reason /api/status reports later in the same
//                               boot: a start-up "setup wizard active" becomes
//                               "setup finished; HTTPS is tried at the next
//                               reboot" once setup completes (it does not
//                               reboot), instead of going stale.
//   plain_http_exempt()       — the paths that must stay on plain HTTP: the six
//                               OS connectivity probes always (no OS sends them
//                               over TLS), and / and /setup while the setup
//                               wizard runs (captive mini-browsers render a
//                               blank page on a self-signed certificate).
//   fingerprint_hex()         — the lowercase hex of the certificate's DER
//                               SHA-256: the pin the iOS app takes from the
//                               provisioning receipt's tls_cert_fp.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace canary {
namespace net {
namespace tls_policy {

enum class Mode : uint8_t {
  HTTPS_REDIRECT,  // httpd_ssl on 443 serves every route; port 80 keeps the
                   // probes and redirects everything else to https://
  HTTP_ONLY,       // one plain httpd on port 80 serves every route
};

// Order matters and is the order a reader should check things in: the build
// first, then the core, then the wizard, then the certificate. `reason` is a
// string literal (never freed) naming the first fact that ruled HTTPS out.
inline Mode decide(bool feature_on, bool server_lib_present,
                   bool cert_available, bool setup_active,
                   const char** reason = nullptr) {
  const char* why = "self-signed TLS on 443; port 80 redirects";
  Mode out = Mode::HTTPS_REDIRECT;
  if (!feature_on) {
    why = "FEATURE_HTTPS=0 in this build";
    out = Mode::HTTP_ONLY;
  } else if (!server_lib_present) {
    why = "this core has no esp_https_server";
    out = Mode::HTTP_ONLY;
  } else if (setup_active) {
    why = "setup wizard active: captive sheets go blank on a self-signed certificate";
    out = Mode::HTTP_ONLY;
  } else if (!cert_available) {
    why = "no TLS certificate (generation unavailable or failed)";
    out = Mode::HTTP_ONLY;
  }
  if (reason) *reason = why;
  return out;
}

// True when decide() ruled HTTPS out ONLY because the setup wizard was
// active — the build and the core could serve it (the setup check comes
// after those two and before the certificate, so this is exactly the case
// in which decide() answered with the wizard reason).
inline bool deferred_for_setup(bool feature_on, bool server_lib_present, bool setup_active) {
  return feature_on && server_lib_present && setup_active;
}

// What /api/status says NOW. decide() runs once, when the server starts, but
// setup can finish later WITHOUT a reboot (setup_mark_complete keeps the
// server up so the wizard's success screen survives), and the start-up
// "setup wizard active" reason would then be stale for the rest of the boot.
// HTTPS is only tried at the next boot, so the live reason says that.
inline const char* live_reason(const char* start_reason, bool deferred_for_setup_at_start,
                               bool setup_active_now) {
  if (deferred_for_setup_at_start && !setup_active_now) {
    return "setup finished; HTTPS is tried at the next reboot";
  }
  return start_reason;
}

// A syntactically plain host: a DNS-ish name or dotted IPv4 made only of
// [A-Za-z0-9.-], or a bracketed IPv6 literal of hex digits, ':' and '.'.
// Anything else (userinfo '@', a path, whitespace, a control byte, an empty
// string) is not a host we will put in a Location header. Syntax only — it
// does not decide whether the name is this device's.
inline bool host_is_plain(const char* host, size_t len) {
  if (!host || len == 0) return false;
  if (host[0] == '[') {
    if (len < 3 || host[len - 1] != ']') return false;
    for (size_t i = 1; i + 1 < len; ++i) {
      const char c = host[i];
      const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                      (c >= 'A' && c <= 'F') || c == ':' || c == '.';
      if (!ok) return false;
    }
    return true;
  }
  for (size_t i = 0; i < len; ++i) {
    const char c = host[i];
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') || c == '.' || c == '-';
    if (!ok) return false;
  }
  return true;
}

// Length of `host` with any ":port" suffix dropped (the redirect goes to the
// default HTTPS port; keeping ":80" would send the browser to https://x:80).
// A bracketed IPv6 literal keeps its brackets and drops only what follows ']'.
inline size_t host_without_port(const char* host) {
  if (!host) return 0;
  const size_t n = strlen(host);
  if (n > 0 && host[0] == '[') {
    const char* close = (const char*)memchr(host, ']', n);
    return close ? (size_t)(close - host) + 1 : n;
  }
  const char* colon = (const char*)memchr(host, ':', n);
  return colon ? (size_t)(colon - host) : n;
}

// "https://" + host + uri into `out`. `host_header` is the request's Host
// header (may be null/empty); when it is not a plain host, `fallback_host`
// (the address the request arrived on) is used instead, and when neither is
// usable the call refuses. `uri` must be origin-form (start with '/') and
// hold no control bytes — a request line that is not cannot be echoed into
// a header. Returns false, with `out` emptied, on any refusal or truncation.
inline bool build_redirect_location(char* out, size_t cap,
                                    const char* host_header,
                                    const char* fallback_host,
                                    const char* uri) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (!uri || uri[0] != '/') return false;
  for (const char* p = uri; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (c < 0x20 || c == 0x7f) return false;
  }
  const char* host = host_header;
  size_t host_len = host_without_port(host);
  if (!host_is_plain(host, host_len)) {
    host = fallback_host;
    host_len = host_without_port(host);
    if (!host_is_plain(host, host_len)) return false;
  }
  static const char kScheme[] = "https://";
  const size_t scheme_len = sizeof(kScheme) - 1;
  const size_t uri_len = strlen(uri);
  if (scheme_len + host_len + uri_len + 1 > cap) return false;
  memcpy(out, kScheme, scheme_len);
  memcpy(out + scheme_len, host, host_len);
  memcpy(out + scheme_len + host_len, uri, uri_len + 1);
  return true;
}

// True when `uri`'s path component (everything before '?' or '#') equals
// `lit` exactly.
inline bool path_is(const char* uri, const char* lit) {
  if (!uri || !lit) return false;
  size_t i = 0;
  for (; lit[i] != '\0'; ++i) {
    if (uri[i] != lit[i]) return false;
  }
  return uri[i] == '\0' || uri[i] == '?' || uri[i] == '#';
}

// The six OS connectivity probes the captive-portal strategy answers
// (Apple ×2, Android ×2, Windows ×2) — the same list registerHttpHandlers
// registers, in the same order.
inline bool is_connectivity_probe(const char* uri) {
  static const char* const kProbes[] = {
    "/hotspot-detect.html", "/library/test/success.html",
    "/generate_204",        "/gen_204",
    "/connecttest.txt",     "/ncsi.txt",
  };
  for (const char* p : kProbes) {
    if (path_is(uri, p)) return true;
  }
  return false;
}

// Paths the port-80 server must serve itself rather than redirect.
inline bool plain_http_exempt(const char* uri, bool setup_active) {
  if (is_connectivity_probe(uri)) return true;
  if (setup_active && (path_is(uri, "/") || path_is(uri, "/setup"))) return true;
  return false;
}

// Lowercase hex of a SHA-256 digest; `out` receives 64 chars + NUL.
inline void fingerprint_hex(const uint8_t sha[32], char out[65]) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; ++i) {
    out[2 * i]     = kHex[(sha[i] >> 4) & 0x0F];
    out[2 * i + 1] = kHex[sha[i] & 0x0F];
  }
  out[64] = '\0';
}

}  // namespace tls_policy
}  // namespace net
}  // namespace canary
