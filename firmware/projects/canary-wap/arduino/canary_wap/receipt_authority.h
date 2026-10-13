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
 * The rule, every grant alike except where noted:
 *
 *   1. The STA holds no address           -> the SoftAP address. There is no
 *      home network to name; the request came over the setup network. (No
 *      SoftAP address either, the AP dropped and the join lost at once:
 *      the `.local` name, the one the device answers to on whichever
 *      network comes back, never "0.0.0.0".)
 *   2. The request arrived on the SoftAP  -> the SoftAP address, for the
 *      bearer and BOOT-tap grants and the redirect: that is where this
 *      caller is, and the only address a phone on the setup network is sure
 *      to reach. A setup-door claim is the exception and goes on to rule 3:
 *      the claim's contract is the `.local` name
 *      (docs/design/magic_pairing.md §3.5, "never the SoftAP address, which
 *      is dead once the AP drops"), and the phone that spends it
 *      provisioned over Bluetooth, not the SoftAP.
 *   3. Otherwise (on the STA, or the arrival could not be read) -> the
 *      device's `.local` name when it has one, else the STA's own address.
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

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace receipt_authority {

/* The grant that admitted the request (handle_provisioning_receipt's order),
 * or HTTPS_REDIRECT: no grant at all, port 80's redirect to https
 * (handle_https_redirect()), whose Location follows the caller exactly as a
 * bearer or BOOT-tap receipt does. */
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
  if (in.sta_ip == 0) {
    if (in.softap_ip != 0) return Choice::SOFTAP_ADDRESS;
    return has_name(in) ? Choice::MDNS_NAME : Choice::NONE;
  }
  if (arrived_on_softap(in) && in.grant != Grant::SETUP_DOOR_CLAIM) {
    return Choice::SOFTAP_ADDRESS;
  }
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

/* ── Socket address -> IPv4 ─────────────────────────────────────────────────
 * esp_http_server listens on one dual-stack AF_INET6 socket whenever the core
 * sets CONFIG_LWIP_IPV6 (both Arduino-ESP32 cores the WAP builds on do), and
 * lwIP's getsockname() then reports an IPv4 request as the IPv4-mapped
 * ::ffff:a.b.c.d. `bytes` is the address as it sits in the sockaddr, network
 * order: 4 bytes for IPV4 (sin_addr), 16 for IPV6 (sin6_addr). Any IPv6
 * address that is not IPv4-mapped, any other family, and a null pointer read
 * as 0 (unknown). The same mapping as provisioning_gate.h's
 * ipv4_host_order_from_addr(), which the sketch cannot include (the Arduino
 * sketch compiles only its own directory); the host test holds the two equal. */
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
