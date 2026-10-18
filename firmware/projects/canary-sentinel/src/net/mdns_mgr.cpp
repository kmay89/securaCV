// src/net/mdns_mgr.cpp — _securacv._tcp fleet advert (see mdns_mgr.h).
//
// Advertise-only sibling of canary-display's discovery.cpp: same service,
// same TXT vocabulary, no browsing. Uses the core's bundled ESPmDNS (no
// extra lib_deps).
#include "canary/net/mdns_mgr.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>

#include "canary/config.h"
#include "canary/version.h"
#include "canary/log.h"
#include "canary/runtime_config.h"      // NVS-backed device id (OTA-safe)
#include "identity/device_pseudonym.h"  // MAC-free hostname suffix (Invariant III)
#include "network/mdns_txt.h"           // the one _securacv._tcp TXT schema (common/)

namespace canary::net {

namespace {

constexpr const char* SVC = "securacv";
constexpr const char* PROTO = "tcp";

bool s_up = false;
char s_host[48] = {0};

// Set from the WiFi event task (STA_GOT_IP), drained by mdns_loop() on the
// main task — event handlers only latch state, they never run mDNS logic
// (single-task invariant; single-word write is atomic on ESP32).
volatile bool s_reannounce_pending = false;

// Broker gossip cache — re-applied on every re-announce so a WiFi link
// cycle can't silently drop (or resurrect) a referral.
char s_broker[64] = {0};
uint16_t s_bport = 0;
// Who coordinates the hub-less fleet, as the peer link last said it
// ("hub" | "self" | a peer's fingerprint hex | "" unknown) — re-applied with
// the rest of the TXT set, re-announced only when it changes.
char s_coord[17] = {0};

// What this build serves, as mdns_txt words — from the feature flags the
// build was made with, so the advert cannot claim a door or a beacon the
// image does not carry.
uint16_t caps_bits() {
  uint16_t caps = mdns_txt::CAP_STATUS | mdns_txt::CAP_FLEET | mdns_txt::CAP_OTA |
                  mdns_txt::CAP_MQTT | mdns_txt::CAP_PEER;
#if defined(FEATURE_IMPROV) && FEATURE_IMPROV
  caps |= mdns_txt::CAP_BLE_DOOR;
#endif
#if defined(FEATURE_FLEET_BEACON) && FEATURE_FLEET_BEACON
  caps |= mdns_txt::CAP_BEACON;
#endif
  return caps;
}

void copy_str(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

// "canary_vision_001" -> "canary-vision-001-a1b2c3": mDNS hostnames are
// hyphen-world, and the salted pseudonym suffix keeps two same-id units
// from colliding without ever touching the MAC. Same recipe as
// canary-display's make_hostname().
void make_hostname(const char* device_id, char* out, size_t cap) {
  char devid_hex[device_pseudonym::HEX_LEN + 1] = {0};
  device_pseudonym::device_id_hex(devid_hex, sizeof(devid_hex));
  char base[24];
  copy_str(base, sizeof(base), device_id && device_id[0] ? device_id : "canary");
  for (char* p = base; *p; p++) {
    if (*p == '_' || *p == ' ' || *p == '.') *p = '-';
  }
  snprintf(out, cap, "%s-%.6s", base, devid_hex);
}

// Canonical device type for the `dt` TXT key: lowercase, hyphenated —
// matches HA's canonical_device_type() and the display's advert.
void canonical_dt(char* out, size_t cap) {
  copy_str(out, cap, DEVICE_TYPE);
  for (char* p = out; *p; p++) {
    if (*p == '_' || *p == ' ') *p = '-';
    else *p = (char)tolower((unsigned char)*p);
  }
}

void apply_txt() {
  // The one TXT schema every node advertises (common/network/mdns_txt.h):
  // same keys, same order, same omit rules as the display, the WAP and the
  // PIO canary, so one browser labels every SecuraCV advertiser. cfg
  // device_id is an in-struct array (never NULL), but guard anyway — a
  // NULL would dereference inside the mDNS library.
  const char* device_id = canary::cfg::get().device_id;
  if (!device_id) device_id = "";
  char dt[32];
  canonical_dt(dt, sizeof(dt));
  mdns_txt::Fields f;
  f.device_id = device_id;
  // No separate friendly-name store on this variant (names live on the
  // HA/display side via the meta topic) — the id doubles as the name.
  f.name = device_id;
  f.host = s_host;
  f.fw = CANARY_FW_VERSION;
  f.model = MODEL;
  f.dt = dt;
  f.role = mdns_txt::ROLE_WITNESS;
#ifdef CANARY_FIGURE_HARDWARE
  f.hw = CANARY_FIGURE_HARDWARE;
#endif
  f.caps = caps_bits();
  f.coord = s_coord;
  // Empty values are the tombstone — the query side skips zero-length
  // broker TXT (same contract as canary-display / canary-wap).
  f.broker = s_broker;
  f.bport = s_bport;
  mdns_txt::Kv kv[mdns_txt::MAX_KEYS];
  mdns_txt::Scratch scratch;
  const size_t n = mdns_txt::enumerate(f, kv, mdns_txt::MAX_KEYS, scratch);
  for (size_t i = 0; i < n; i++) MDNS.addServiceTxt(SVC, PROTO, kv[i].key, kv[i].value);
}

bool announce() {
  MDNS.end();
  if (!MDNS.begin(s_host)) return false;
  // The port the node actually answers on: GET /api/status and /api/fleet
  // (common/network/node_http) serve here once the link is up. The dummy
  // port 1 is gone — a Witness Wall that followed the advert used to get a
  // refused connection.
  MDNS.addService(SVC, PROTO, mdns_txt::PORT_HTTP);
  apply_txt();
  return true;
}

}  // namespace

bool mdns_init() {
  make_hostname(canary::cfg::get().device_id, s_host, sizeof(s_host));

  if (!announce()) {
    log_line("MDNS", "mDNS start FAILED — fleet advert disabled this boot.");
    return false;
  }
  s_up = true;

  // ESP-IDF mDNS binds to whichever netifs are up at begin() time and does
  // not re-announce when the STA regains an IP later — so a WiFi outage
  // (wifi_mgr backoff) must not leave a stale advert. The event handler
  // only latches a flag (it runs on the system event task, which must
  // never drive mDNS logic directly); mdns_loop() drains it on the main
  // task. Same recovery canary-wap ships, via the loop instead.
  static bool s_event_registered = false;
  if (!s_event_registered) {
    WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t /*info*/) {
      if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        s_reannounce_pending = true;
      }
    });
    s_event_registered = true;
  }

  log_header("MDNS");
  canary::dbg_serial().printf("Fleet advert up as %s.local (_%s._%s)\n",
                              s_host, SVC, PROTO);
  return true;
}

void mdns_loop(uint32_t /*now_ms*/) {
  if (!s_up || !s_reannounce_pending) return;
  s_reannounce_pending = false;
  if (WiFi.status() != WL_CONNECTED) return;  // next GOT_IP re-latches
  if (!announce()) {
    log_line("MDNS", "mDNS re-announce failed.");
  }
}

void mdns_advertise_broker(const char* host, uint16_t port) {
  if (!host || !host[0]) return;
  copy_str(s_broker, sizeof(s_broker), host);
  s_bport = port ? port : 1883;
  if (!s_up) return;
  char p[8];
  snprintf(p, sizeof(p), "%u", (unsigned)s_bport);
  MDNS.addServiceTxt(SVC, PROTO, "broker", (const char*)s_broker);
  MDNS.addServiceTxt(SVC, PROTO, "bport", p);
}

void mdns_clear_broker() {
  s_broker[0] = '\0';
  s_bport = 0;
  if (!s_up) return;
  // Ground-truth-only gossip: the moment we can't reach the broker we stop
  // referring others to it, or a dead/moved endpoint keeps re-seeding every
  // rediscovery on the LAN.
  MDNS.addServiceTxt(SVC, PROTO, "broker", "");
  MDNS.addServiceTxt(SVC, PROTO, "bport", "");
}

const char* mdns_host_label() { return s_host; }

void mdns_set_coord(const char* word) {
  const char* w = word ? word : "";
  if (strcmp(w, s_coord) == 0) return;
  snprintf(s_coord, sizeof(s_coord), "%s", w);
  if (!s_up) return;
  MDNS.addServiceTxt(SVC, PROTO, "coord", (const char*)s_coord);
}

}  // namespace canary::net
