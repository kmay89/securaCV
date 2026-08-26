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

#include "config.h"     // MODEL (CD_MODEL)
#include "version.h"    // CANARY_FW_VERSION
#include "log.h"
#include "fleet_instance.h"  // the_fleet()
#include "fleet_model.h"     // on_status / on_meta
#include "hostname.h"  // MAC-free hostname (Invariant III), shared
#include "pins.h"  // CANARY_FIGURE_HARDWARE — which board this is (board -I path)

namespace canary::net {

namespace {

constexpr const char* SVC = "securacv";
constexpr const char* PROTO = "tcp";

// Untrusted-TXT length ceiling: mirrors resolve_if_mdns_local's 64-char
// buffer. No sane device_id is 64+ chars; anything longer is a mangled or
// hostile advert and is rejected outright rather than truncated.
constexpr size_t TXT_MAX = 64;

bool s_up = false;

// This display's own device_id, captured at discovery_init, so the witness
// scan can skip our own advert (we advertise role="display" anyway, but the
// id compare is the belt-and-suspenders self-skip).
char s_self_id[TXT_MAX] = {0};

void copy_str(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

// make_hostname moved to canary/net/hostname.h so the settings network
// page and the transparency sheet print the exact name registered here.

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
  // Remember our own id for the witness-scan self-skip.
  copy_str(s_self_id, sizeof(s_self_id), device_id);

  char hostname[48];
  make_hostname(device_id, hostname, sizeof(hostname));

  if (!MDNS.begin(hostname)) {
    log_line("MDNS", "mDNS start FAILED — fleet discovery disabled this boot.");
    return false;
  }
  s_up = true;

  // Presence advert. Unlike vision/sense (whose port 1 is a formality —
  // they run no server), a display DOES listen: glass_web serves the live
  // mirror, help, and /api/fleet on :80, unconditionally (main.cpp starts
  // it right after provisioning). Advertise the REAL port so a fleet
  // browser (the Flasher's fleet book, the iPhone app) knows this device
  // answers HTTP without a guess-and-probe round.
  MDNS.addService(SVC, PROTO, 80);
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
  // WHICH BOARD, beside what it calls itself. `dt` names the product and
  // several products share a board (and one board, the 7" glass, serves two
  // products) — so `dt` cannot pin down the SHAPE and this can. A browser
  // draws the right hardware from the advert alone, before it has asked the
  // device anything. Omitted rather than sent empty on a build with no pins
  // header to name: absent means "cannot say", which "" does not.
#ifdef CANARY_FIGURE_HARDWARE
  MDNS.addServiceTxt(SVC, PROTO, "hw", CANARY_FIGURE_HARDWARE);
#endif

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

void discovery_scan_witnesses(uint32_t now, bool broker_up) {
  if (!s_up) return;

  // Rate-limit the whole scan: the ESPmDNS query blocks ~3 s, so it must not
  // run every loop. First call fires immediately (s_next_due == 0).
  //
  // This used to be skipped ENTIRELY while the broker was up, on the reasoning
  // that MQTT is the richer source. It is richer, but it is not a superset:
  // MQTT only ever shows Canaries that are configured to talk to THAT broker. A
  // Canary sitting on the same LAN, advertising itself over mDNS, but not
  // pointed at the hub was invisible to the display for exactly as long as the
  // hub was healthy — so adding a hub REDUCED what the display could see. That
  // is backwards: the fleet is what is on the network, and the hub is an
  // upgrade on top of it, never a precondition for seeing your own devices.
  //
  // So the browse always runs; it just runs less often when the broker is
  // carrying the load (60 s vs 20 s), which keeps the ~3 s blocking query off
  // the critical path of a healthy display.
  static uint32_t s_next_due = 0;
  static bool s_armed = false;
  if (s_armed && (int32_t)(now - s_next_due) < 0) return;
  s_armed = true;
  s_next_due = now + (broker_up ? 60000UL : 20000UL);

  auto& fleet = canary::fleet::the_fleet();

  const int n = MDNS.queryService(SVC, PROTO);
  for (int i = 0; i < n; i++) {
    // Witnesses AND sibling displays join the fleet: a display is a peer with
    // its own sensor data (the I2C header) and its screen state, so on a
    // broker-down LAN the screens still see each other — same as they do over
    // MQTT. Only roleless adverts are dropped (no identity, no seat).
    // UNAUTHENTICATED LAN input — every read is bounded.
    const String role = MDNS.txt(i, "role");
    if (role != "witness" && role != "display") continue;

    const String id = MDNS.txt(i, "device_id");
    const size_t id_len = id.length();
    if (id_len == 0 || id_len >= TXT_MAX) continue;  // empty or absurd -> drop
    if (s_self_id[0] && strcmp(id.c_str(), s_self_id) == 0) continue;  // that's us

    const String dt = MDNS.txt(i, "dt");    // device type (e.g. "canary-wap")
    const String name = MDNS.txt(i, "name");

    // mDNS TXT is unauthenticated: seen + named only, never a trust badge.
    // battery_soc = -1: mDNS carries no charge state.
    fleet.on_status(id.c_str(), dt.length() ? dt.c_str() : "",
                    /*online=*/true, /*battery_soc=*/-1, now);
    if (name.length() && name.length() < TXT_MAX && name != id) {
      fleet.on_meta(id.c_str(), name.c_str(), "", now);
    }
  }
}

}  // namespace canary::net
