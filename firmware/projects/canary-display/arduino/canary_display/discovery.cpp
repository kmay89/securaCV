// src/net/discovery.cpp — mDNS fleet discovery (see discovery.h).
//
// Uses the core's bundled ESPmDNS (no extra lib_deps). All queries are
// bounded (~3 s inside ESPmDNS) and only run while the broker link is
// already down or unconfigured, so they never add latency to a healthy
// display and never trip the 30 s task watchdog.
#include "discovery.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <string.h>
#include <stdio.h>

#include "config.h"   // MODEL (CD_MODEL)
#include "version.h"  // CANARY_FW_VERSION
#include "log.h"
#include "device_pseudonym.h"  // MAC-free hostname suffix (Invariant III)

namespace canary::net {

namespace {

constexpr const char* SVC = "securacv";
constexpr const char* PROTO = "tcp";

bool s_up = false;

void copy_str(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

// "canary_watch_001" -> "canary-watch-001-a1b2c3": mDNS hostnames are
// hyphen-world, and the salted pseudonym suffix keeps two same-id units
// from colliding without ever touching the MAC.
void make_hostname(const char* device_id, char* out, size_t cap) {
  char devid_hex[device_pseudonym::HEX_LEN + 1] = {0};
  device_pseudonym::device_id_hex(devid_hex, sizeof(devid_hex));
  char base[24];
  copy_str(base, sizeof(base), device_id && device_id[0] ? device_id : "canary-display");
  for (char* p = base; *p; p++) {
    if (*p == '_' || *p == ' ' || *p == '.') *p = '-';
  }
  snprintf(out, cap, "%s-%.6s", base, devid_hex);
}

// Resolve a broker host string to something WiFiClient can connect to:
// `.local` names only exist in mDNS, so look them up here; anything else
// (IP or plain DNS name) passes through untouched.
//
// SECURITY: `host` is an UNAUTHENTICATED LAN TXT record. Reject anything
// that doesn't fit the local buffer outright — truncating and continuing
// would either index past the copy (the review-caught stack overflow) or
// silently connect to a mangled name. No sane broker hostname is 64+ chars.
bool resolve_if_mdns_local(const char* host, char* out, size_t cap) {
  char name[64];
  const size_t n = strlen(host);
  if (n == 0 || n >= sizeof(name)) return false;
  const char suffix[] = ".local";
  const size_t sn = sizeof(suffix) - 1;
  if (n <= sn || strcasecmp(host + n - sn, suffix) != 0) {
    copy_str(out, cap, host);
    return true;
  }
  copy_str(name, sizeof(name), host);
  name[n - sn] = '\0';
  const IPAddress ip = MDNS.queryHost(name);
  if (ip == IPAddress()) return false;
  copy_str(out, cap, ip.toString().c_str());
  return true;
}

}  // namespace

bool discovery_init(const char* device_id, const char* device_type,
                    const char* role) {
  char hostname[48];
  make_hostname(device_id, hostname, sizeof(hostname));

  if (!MDNS.begin(hostname)) {
    log_line("MDNS", "mDNS start FAILED — fleet discovery disabled this boot.");
    return false;
  }
  s_up = true;

  // Presence advert. The service port is a formality (nothing listens);
  // the payload is the TXT set.
  MDNS.addService(SVC, PROTO, 1);
  // Canonical fleet TXT identity — the same vocabulary the witness
  // variants (canary-wap/vision/sense) advertise, so one browser can
  // label every SecuraCV advertiser without special-casing displays.
  MDNS.addServiceTxt(SVC, PROTO, "device_id", device_id ? device_id : "");
  MDNS.addServiceTxt(SVC, PROTO, "name", device_id ? device_id : "");
  MDNS.addServiceTxt(SVC, PROTO, "host", (const char*)hostname);
  MDNS.addServiceTxt(SVC, PROTO, "fw", CANARY_FW_VERSION);
  MDNS.addServiceTxt(SVC, PROTO, "model", MODEL);
  MDNS.addServiceTxt(SVC, PROTO, "role", role ? role : "display");
  MDNS.addServiceTxt(SVC, PROTO, "dt", device_type ? device_type : "");

  log_header("MDNS");
  canary::dbg_serial().printf("Fleet advert up as %s.local (_%s._%s)\n",
                              hostname, SVC, PROTO);
  return true;
}

void discovery_advertise_broker(const char* host, uint16_t port) {
  if (!s_up || !host || !host[0]) return;
  MDNS.addServiceTxt(SVC, PROTO, "broker", host);
  char p[8];
  snprintf(p, sizeof(p), "%u", (unsigned)port);
  MDNS.addServiceTxt(SVC, PROTO, "bport", p);
}

void discovery_clear_broker() {
  if (!s_up) return;
  // ESPmDNS exposes no txt-item remove; an empty value is the tombstone —
  // the query side skips zero-length broker TXT. Ground-truth-only gossip
  // cuts both ways: the moment we can't reach the broker, we must stop
  // referring others to it, or a dead/moved endpoint keeps re-seeding
  // every rediscovery on the LAN (review catch).
  MDNS.addServiceTxt(SVC, PROTO, "broker", "");
  MDNS.addServiceTxt(SVC, PROTO, "bport", "");
}

bool discovery_find_broker(char* host_out, size_t host_cap, uint16_t* port_out) {
  if (!s_up || !host_out || host_cap == 0) return false;

  // 1) Ask the fleet: any SecuraCV device that has a working broker link
  //    gossips it in its TXT records.
  int n = MDNS.queryService(SVC, PROTO);
  for (int i = 0; i < n; i++) {
    String b = MDNS.txt(i, "broker");
    if (b.length() == 0) continue;
    char resolved[64];
    if (!resolve_if_mdns_local(b.c_str(), resolved, sizeof(resolved))) continue;
    copy_str(host_out, host_cap, resolved);
    const long bp = MDNS.txt(i, "bport").toInt();
    if (port_out) *port_out = (bp > 0 && bp <= 65535) ? (uint16_t)bp : 1883;
    log_header("MDNS");
    canary::dbg_serial().printf("Fleet referral: broker %s:%u (from %s)\n",
                                host_out, (unsigned)(port_out ? *port_out : 1883),
                                MDNS.hostname(i).c_str());
    return true;
  }

  // 2) Fallback: any plain MQTT broker advertising itself on the LAN
  //    (mosquitto/HA setups with avahi service files).
  n = MDNS.queryService("mqtt", PROTO);
  if (n > 0) {
    // Core 3 renamed the result accessor IP(idx) -> address(idx); the rest
    // of the ESPmDNS surface this file uses is identical across majors.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    const IPAddress ip = MDNS.address(0);
#else
    const IPAddress ip = MDNS.IP(0);
#endif
    if (!(ip == IPAddress())) {
      copy_str(host_out, host_cap, ip.toString().c_str());
      const uint16_t p = MDNS.port(0);
      if (port_out) *port_out = p ? p : 1883;
      log_header("MDNS");
      canary::dbg_serial().printf("_mqtt._tcp advert: broker %s:%u\n",
                                  host_out, (unsigned)(port_out ? *port_out : 1883));
      return true;
    }
  }

  log_line("MDNS", "No broker referral on the LAN (fleet quiet, no _mqtt._tcp).");
  return false;
}

}  // namespace canary::net
