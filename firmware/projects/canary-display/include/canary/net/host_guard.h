// include/canary/net/host_guard.h — does the Host a request targeted name
// THIS device?
//
// The glass's write guard (net/glass_web.cpp) compares the Origin header's
// authority to the Host header: the page this device serves POSTs back with
// the two equal; a drive-by page on another site does not. Both headers are
// the browser's, though, and DNS rebinding makes them agree for the attacker:
// a page at http://evil.example whose name is re-pointed at this device's LAN
// IP arrives with Origin == Host == evil.example, passes as same-site, reads
// the CSRF token GET /api/settings hands out, and writes. So Host must ALSO be
// something that can only mean this device on this network:
//
//   · an IP literal — no DNS was involved, so nothing was rebound;
//   · a name public DNS cannot mint: `.local` (mDNS), `.lan` / `.internal` /
//     `.home.arpa` (reserved for private use), or a single label (resolved by
//     the LAN resolver only);
//
// and nothing else. The device's own mDNS name is a single label under
// `.local`, so every way a household actually reaches the glass — the .local
// name, the raw IP, a router alias under a private suffix — still passes,
// while a rebinding page's registrable public domain fails. Header-only and
// Arduino-free so the rule is host-tested (tests_host/test_host_guard.cpp);
// the WebServer handler passes hostHeader().c_str() in.

#pragma once
#include <stddef.h>
#include <string.h>

namespace canary::net {

// Copies the authority's name out of a raw Host header value: trimmed,
// lowercased, any :port removed (a bracketed IPv6 authority keeps its address
// without the brackets; a bare literal with several colons has no port to
// remove). Returns the length written, or 0 when the name is empty or does
// not fit `cap`.
inline size_t host_authority_name(const char* host, char* out, size_t cap) {
  if (!host || !out || cap == 0) return 0;
  size_t n = strlen(host);
  while (n && (host[0] == ' ' || host[0] == '\t')) { host++; n--; }
  while (n && (host[n - 1] == ' ' || host[n - 1] == '\t')) n--;
  if (n == 0) return 0;
  if (host[0] == '[') {
    const char* close = (const char*)memchr(host, ']', n);
    if (!close) return 0;
    host++;
    n = (size_t)(close - host);
  } else {
    const char* colon = (const char*)memchr(host, ':', n);
    if (colon && !memchr(colon + 1, ':', n - (size_t)(colon + 1 - host))) {
      n = (size_t)(colon - host);
    }
  }
  if (n == 0 || n + 1 > cap) return 0;
  for (size_t i = 0; i < n; i++) {
    const char c = host[i];
    out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
  }
  out[n] = '\0';
  return n;
}

// Four dotted decimal octets, each 0-255, nothing else.
inline bool host_is_ipv4_literal(const char* s) {
  int octets = 0, digits = 0, value = 0;
  for (const char* p = s;; p++) {
    if (*p >= '0' && *p <= '9') {
      value = value * 10 + (*p - '0');
      if (++digits > 3 || value > 255) return false;
    } else if (*p == '.' || *p == '\0') {
      if (digits == 0) return false;
      octets++;
      digits = 0;
      value = 0;
      if (*p == '\0') break;
      if (octets >= 4) return false;
    } else {
      return false;
    }
  }
  return octets == 4;
}

// Loose IPv6 shape: hex digits, ':' and '.' (v4-mapped tails) only, with at
// least two colons. Enough to tell a literal from a DNS name, which is all
// the guard needs — a literal never passed through DNS.
inline bool host_is_ipv6_literal(const char* s) {
  if (!*s) return false;
  int colons = 0;
  for (const char* p = s; *p; p++) {
    if (*p == ':') {
      colons++;
    } else if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || *p == '.')) {
      return false;
    }
  }
  return colons >= 2;
}

// True when `host` (a raw Host header value, port allowed) can only name this
// device on this network — see the file comment for the rule. Empty or
// oversized authorities are foreign.
inline bool host_names_this_device(const char* host) {
  char name[96];
  const size_t n = host_authority_name(host, name, sizeof(name));
  if (n == 0) return false;
  if (host_is_ipv4_literal(name) || host_is_ipv6_literal(name)) return true;
  if (!memchr(name, '.', n)) return true;  // single label: LAN resolver only
  static const char* const kLanSuffixes[] = {".local", ".lan", ".internal",
                                             ".home.arpa"};
  for (const char* suffix : kLanSuffixes) {
    const size_t sl = strlen(suffix);
    if (n > sl && strcmp(name + n - sl, suffix) == 0) return true;
  }
  return false;
}

}  // namespace canary::net
