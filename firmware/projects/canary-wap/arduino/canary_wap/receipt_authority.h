/**
 * @file receipt_authority.h
 * @brief Which address names the WAP to a caller: the provisioning receipt's
 *        base_url (sweep F255) and port 80's redirect to https.
 *
 * GET /api/provisioning-receipt answers `base_url` = scheme + "://" + an
 * authority. The sketch used to write the SoftAP address (192.168.4.1) for
 * the bearer and BOOT-tap grants and the device's `.local` name only for a
 * spent setup-door claim, so a receipt fetched over the home LAN while the
 * STA was connected sent its reader to 192.168.4.1: unreachable from the
 * LAN, and gone altogether once the SoftAP drops after its grace window
 * (wifi_drop_ap()). A recovery kit the dashboard saves after a BOOT tap
 * (F257) is that receipt, so it pointed at nothing. Port 80's redirect to
 * https (handle_https_redirect(), on every TLS unit) made the same choice
 * for every caller, so `http://canary-<suffix>.local/` typed on the home
 * network landed on https://192.168.4.1 too.
 *
 * The rule:
 *
 *   0. A setup-door claim, on a device with a name -> the `.local` name,
 *      whatever the STA or the SoftAP is doing. The claim receipt is the one
 *      the phone KEEPS for the life of the pairing, so it spells the
 *      steady-state address (docs/design/magic_pairing.md §3.5, "never the
 *      SoftAP address, which is dead once the AP drops"), and the phone that
 *      spends it provisioned over Bluetooth, not the SoftAP. This is #1804's
 *      rule; F255's first cut (#1802) sent a claim spent with the STA down
 *      (the join lost in the same moment) to the SoftAP address, and the
 *      post-#1802 verification put the contract back (2026-10-19). A claim
 *      on a device with no name goes on like any other grant.
 *   1. The STA holds no address           -> the SoftAP address. There is no
 *      home network to name; the request came over the setup network. (No
 *      SoftAP address either, the AP dropped and the join lost at once:
 *      the `.local` name, the one the device answers to on whichever
 *      network comes back, never "0.0.0.0".)
 *   2. The request arrived on the SoftAP  -> the SoftAP address, for the
 *      bearer and BOOT-tap grants and the redirect: that is where this
 *      caller is, and the only address a phone on the setup network is sure
 *      to reach. A setup-door claim (one with no name, past rule 0) goes on
 *      to rule 3 instead.
 *   3. Otherwise (on the STA, or the arrival could not be read) -> the
 *      device's `.local` name when it has one, else the STA's own address.
 *
 * Port 80's redirect to https (handle_https_redirect()) has one more input,
 * the request's Host header, and redirect_location() decides from it first:
 * a Host the device is reached at is kept as the Location's authority — an
 * IP literal (a browser that typed the device's address on a PC or phone
 * with no mDNS, Windows or Android without Bonjour: the request reached the
 * device at it, so https at it does too), the device's own `.local` name, or
 * the bare canary.local catch-all it answers — spelled from the alphabet
 * alone (letters, digits, dots, dashes, an IPv6 literal's brackets and
 * colons), its ":port" dropped (the HTTPS server is on 443). A Host header
 * is attacker-chosen text headed into a response header, so anything else
 * (a name that is not this device's, a byte outside the alphabet, a Host
 * that does not fit) is refused, never forwarded, and the Location falls
 * back to choose()'s answer for the HTTPS_REDIRECT grant. #1804 kept any
 * Host in the alphabet; #1802's merge kept none, so a browser that typed the
 * device's IP was sent to a `.local` name it could not resolve; this is the
 * rule since 2026-10-19. The Location is "https://" + the authority + the
 * request's URI, and a URI that does not fit is not cut (a cut URI redirects
 * to the wrong page): the root instead.
 *
 * "Arrived on the SoftAP" is the request socket's LOCAL address (getsockname)
 * equal to the SoftAP address and different from the STA address (a home
 * router that handed the STA the very address the SoftAP uses makes the
 * question unanswerable, and the STA's name reaches it either way). A socket
 * that could not say (0) never arrived on the SoftAP, even when the SoftAP
 * address reads 0 too: on Arduino-ESP32 core 3.x, WiFi.softAPIP() reads
 * 0.0.0.0 once wifi_drop_ap() has released the AP netif (APClass::onDisable
 * clears it; NetworkInterface::localIP() answers IPAddress() for a null
 * netif), which is every installed WAP's steady state.
 *
 * The fallback when the socket cannot say (request_local == 0): with the STA
 * up, rule 3 — the `.local` name. That is the answer every home-LAN reader
 * can use, and while the STA is up the SoftAP is either inside its grace
 * window or already gone; the ESP-IDF mDNS responder answers on the SoftAP's
 * netif too. The SoftAP address would be wrong for every LAN reader, every
 * time. With the STA down, rule 1 answers whatever the socket said.
 *
 * The `.local` name lasts as long as the device's name does, and no longer:
 * naming the device (the wizard's device_name, POST /api/device-name)
 * regenerates mdns_hostname and re-announces it, so a receipt or recovery
 * kit saved before a rename names a host nothing answers any more.
 *
 * Pure hosted C++ (no Arduino, ESP-IDF or heap): host-tested in
 * tests_host/test_receipt_authority.cpp, which also holds
 * ipv4_from_addr() to firmware/common/network/provisioning_gate.h's
 * ipv4_host_order_from_addr() on the same vectors.
 */

#ifndef CANARY_WAP_RECEIPT_AUTHORITY_H
#define CANARY_WAP_RECEIPT_AUTHORITY_H

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace receipt_authority {

/* The grant that admitted the request (handle_provisioning_receipt's order:
 * BEARER is a real `Authorization: Bearer` header, never the cv_session
 * cookie, sweep F289), or HTTPS_REDIRECT: no grant at all, port 80's redirect
 * to https (handle_https_redirect()), whose Location follows the caller
 * exactly as a bearer or BOOT-tap receipt does. */
enum class Grant : uint8_t { BEARER, BOOT_TAP, SETUP_DOOR_CLAIM, HTTPS_REDIRECT };

/* NONE: no name and no address to give (no mDNS name, the STA down and the
 * SoftAP dropped); write() then writes nothing. */
enum class Choice : uint8_t { SOFTAP_ADDRESS, MDNS_NAME, STA_ADDRESS, NONE };

/* Every address is host-order IPv4 (a.b.c.d packed as a<<24 | b<<16 | c<<8 | d,
 * ipv4_from_octets()); 0 means none or unknown. */
struct Inputs {
  uint32_t    request_local;  /* the address the request arrived on (getsockname); 0 = unread */
  uint32_t    softap_ip;      /* WiFi.softAPIP(); 0 once the AP netif is released (core 3.x) */
  uint32_t    sta_ip;         /* WiFi.localIP() while WiFi.isConnected(), else 0 */
  const char* mdns_host;      /* g_device.mdns_hostname, without ".local"; NULL or "" = none */
  Grant       grant;
};

/* a.b.c.d in the host order Inputs carries: the octets as an IPAddress
 * indexes them (ip[0] is the first). */
inline uint32_t ipv4_from_octets(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}

inline bool arrived_on_softap(const Inputs& in) {
  return in.request_local != 0 && in.request_local == in.softap_ip &&
         in.request_local != in.sta_ip;
}

inline bool has_name(const Inputs& in) { return in.mdns_host && in.mdns_host[0] != '\0'; }

inline Choice choose(const Inputs& in) {
  const bool claim = in.grant == Grant::SETUP_DOOR_CLAIM;
  if (claim && has_name(in)) return Choice::MDNS_NAME;   /* rule 0: the steady-state address */
  if (in.sta_ip == 0) {
    if (in.softap_ip != 0) return Choice::SOFTAP_ADDRESS;
    return has_name(in) ? Choice::MDNS_NAME : Choice::NONE;
  }
  if (arrived_on_softap(in) && !claim) return Choice::SOFTAP_ADDRESS;
  if (has_name(in)) return Choice::MDNS_NAME;
  return Choice::STA_ADDRESS;
}

/* The address a name that does not fit gives way to: the one this request
 * can reach the device at (the SoftAP's when it arrived there or the STA is
 * down, else the STA's). 0 when the device holds none. */
inline uint32_t fallback_address(const Inputs& in) {
  if (in.sta_ip != 0 && !arrived_on_softap(in)) return in.sta_ip;
  return in.softap_ip;
}

inline int write_dotted(char* out, size_t cap, uint32_t ip) {
  return snprintf(out, cap, "%u.%u.%u.%u",
                  (unsigned)((ip >> 24) & 0xFFu), (unsigned)((ip >> 16) & 0xFFu),
                  (unsigned)((ip >> 8) & 0xFFu), (unsigned)(ip & 0xFFu));
}

/* Writes choose(in)'s authority into out[cap] ("canary-kitchen.local",
 * "192.168.4.1") and returns its length. One that does not fit with its NUL
 * is not cut: fallback_address() is written instead. When there is nothing
 * to write (Choice::NONE with no address, or not even an address fits),
 * out[0] = '\0' and 0 is returned: the caller answers without an authority
 * rather than send "0.0.0.0" or half a name. */
inline size_t write(char* out, size_t cap, const Inputs& in) {
  if (!out || cap == 0) return 0;
  int n = -1;
  switch (choose(in)) {
    case Choice::MDNS_NAME:      n = snprintf(out, cap, "%s.local", in.mdns_host); break;
    case Choice::STA_ADDRESS:    n = write_dotted(out, cap, in.sta_ip); break;
    case Choice::SOFTAP_ADDRESS: n = write_dotted(out, cap, in.softap_ip); break;
    case Choice::NONE:           break;
  }
  if (n < 0 || (size_t)n >= cap) {
    const uint32_t ip = fallback_address(in);
    n = ip != 0 ? write_dotted(out, cap, ip) : -1;
  }
  if (n < 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n;
}

/* ── Port 80's redirect: the Host the request carried ───────────────────────
 * The alphabet a Host may be forwarded from: letters, digits, dots, dashes,
 * and an IPv6 literal's brackets and colons. Nothing else, however the
 * header came (a CR, a space, a slash, a percent): it is attacker-chosen
 * text headed into a response header. */
inline bool redirect_host_ok(const char* host) {
  if (!host || host[0] == '\0') return false;
  for (const char* p = host; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (!(isalnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']')) return false;
  }
  return true;
}

/* a.b.c.d: four decimal groups of one to three digits, each at most 255. */
inline bool is_ipv4_literal(const char* s) {
  int groups = 0;
  for (;;) {
    int digits = 0;
    unsigned v = 0;
    while (*s >= '0' && *s <= '9') {
      v = v * 10u + (unsigned)(*s - '0');
      if (++digits > 3) return false;
      ++s;
    }
    if (digits == 0 || v > 255u) return false;
    ++groups;
    if (*s != '.') break;
    ++s;
  }
  return *s == '\0' && groups == 4;
}

/* [hex:groups], as a URL spells an IPv6 address: brackets around hex digits,
 * colons and (for a mapped address) dots, with at least one colon. */
inline bool is_ipv6_literal(const char* s) {
  const size_t n = strlen(s);
  if (n < 4 || s[0] != '[' || s[n - 1] != ']') return false;
  bool colon = false;
  for (size_t i = 1; i + 1 < n; ++i) {
    const unsigned char c = (unsigned char)s[i];
    if (c == ':') colon = true;
    else if (!(isxdigit(c) || c == '.')) return false;
  }
  return colon;
}

/* DNS names compare without case. */
inline bool same_name(const char* a, const char* b) {
  for (;; ++a, ++b) {
    const int ca = tolower((unsigned char)*a);
    const int cb = tolower((unsigned char)*b);
    if (ca != cb) return false;
    if (ca == '\0') return true;
  }
}

/* The names this device answers to: its own `<mdns_host>.local` and the bare
 * canary.local catch-all (generate_mdns_hostname(), the delegated hostname
 * in wifi_init_provisioning()). */
inline bool own_name(const char* host, const Inputs& in) {
  if (same_name(host, "canary.local")) return true;
  if (!has_name(in)) return false;
  size_t i = 0;
  for (; in.mdns_host[i] != '\0'; ++i) {
    if (tolower((unsigned char)host[i]) != tolower((unsigned char)in.mdns_host[i])) return false;
  }
  return same_name(host + i, ".local");
}

/* Writes the Host the Location may name into out[cap], its ":port" dropped,
 * and returns its length; 0 (out[0] = '\0') when the Host is refused: null
 * or empty (HTTP/1.0, or a header too long for the sketch's buffer), a byte
 * outside the alphabet, neither an IP literal nor a name of this device, or
 * too long for out. */
inline size_t redirect_host(char* out, size_t cap, const char* host, const Inputs& in) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!redirect_host_ok(host)) return 0;
  size_t n = strlen(host);
  /* Drop a ":port": not the colons of a bracketed IPv6 literal, whose port
   * follows its closing bracket. */
  const char* colon = strrchr(host, ':');
  if (colon && strchr(colon, ']') == nullptr) n = (size_t)(colon - host);
  if (n == 0 || n >= cap) return 0;
  memcpy(out, host, n);
  out[n] = '\0';
  if (is_ipv4_literal(out) || is_ipv6_literal(out) || own_name(out, in)) return n;
  out[0] = '\0';
  return 0;
}

/* Port 80's Location into out[cap]: "https://" + the authority + uri, the
 * authority the request's Host when redirect_host() keeps it, else
 * choose(in)'s (in.grant is HTTPS_REDIRECT). A URI that does not fit is not
 * cut, since a cut URI redirects to the wrong page: the root instead.
 * Returns the length; 0 (out[0] = '\0') when there is no authority to name
 * (write() wrote none: the sketch answers 503) or not even the root fits. */
inline size_t redirect_location(char* out, size_t cap, const char* host, const char* uri,
                                const Inputs& in) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  char authority[80];
  if (redirect_host(authority, sizeof(authority), host, in) == 0 &&
      receipt_authority::write(authority, sizeof(authority), in) == 0) {
    return 0;
  }
  int n = snprintf(out, cap, "https://%s%s", authority, uri ? uri : "");
  if (n < 0 || (size_t)n >= cap) n = snprintf(out, cap, "https://%s/", authority);
  if (n < 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n;
}

/* ── Socket address -> IPv4 ─────────────────────────────────────────────────
 * esp_http_server listens on one dual-stack AF_INET6 socket whenever the core
 * sets CONFIG_LWIP_IPV6 (both Arduino-ESP32 cores the WAP builds on do), and
 * lwIP's getsockname() then reports an IPv4 request as the IPv4-mapped
 * ::ffff:a.b.c.d. `bytes` is the address as it sits in the sockaddr, network
 * order: 4 bytes for IPV4 (sin_addr), 16 for IPV6 (sin6_addr). Any IPv6
 * address that is not IPv4-mapped, any other family, and a null pointer read
 * as 0 (unknown). The same mapping as provisioning_gate.h's
 * ipv4_host_order_from_addr(). The sketch stages that header since F343 for
 * the request's peer key (request_peer_key(), the bearer lockout's source
 * and, since F342, the BOOT tap's asker) and for the tap gate itself; this
 * copy stays here beside the receipt's other socket reads, and the host
 * test holds the two equal. */
enum class AddrFamily : uint8_t { IPV4, IPV6, OTHER };

inline uint32_t ipv4_from_addr(AddrFamily family, const uint8_t* bytes) {
  if (!bytes) return 0;
  const uint8_t* v4 = nullptr;
  if (family == AddrFamily::IPV4) {
    v4 = bytes;
  } else if (family == AddrFamily::IPV6) {
    for (int i = 0; i < 10; ++i) {
      if (bytes[i] != 0) return 0;
    }
    if (bytes[10] != 0xFF || bytes[11] != 0xFF) return 0;
    v4 = bytes + 12;
  } else {
    return 0;
  }
  return ipv4_from_octets(v4[0], v4[1], v4[2], v4[3]);
}

}  // namespace receipt_authority

#endif  // CANARY_WAP_RECEIPT_AUTHORITY_H
