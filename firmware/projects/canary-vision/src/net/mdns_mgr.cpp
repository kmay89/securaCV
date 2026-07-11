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
  // cfg device_id is an in-struct array (never NULL), but guard anyway —
  // a NULL would dereference inside the mDNS library.
  const char* device_id = canary::cfg::get().device_id;
  if (!device_id) device_id = "";
  MDNS.addServiceTxt(SVC, PROTO, "device_id", device_id);
  // No separate friendly-name store on this variant (names live on the
  // HA/display side via the meta topic) — the id doubles as the name.
  MDNS.addServiceTxt(SVC, PROTO, "name", device_id);
  MDNS.addServiceTxt(SVC, PROTO, "host", (const char*)s_host);
  MDNS.addServiceTxt(SVC, PROTO, "fw", CANARY_FW_VERSION);
  MDNS.addServiceTxt(SVC, PROTO, "model", MODEL);
  char dt[32];
  canonical_dt(dt, sizeof(dt));
  MDNS.addServiceTxt(SVC, PROTO, "dt", dt);
  MDNS.addServiceTxt(SVC, PROTO, "role", "witness");
  if (s_broker[0]) {
    char p[8];
    snprintf(p, sizeof(p), "%u", (unsigned)s_bport);
    MDNS.addServiceTxt(SVC, PROTO, "broker", (const char*)s_broker);
    MDNS.addServiceTxt(SVC, PROTO, "bport", p);
  } else {
    // Empty value is the tombstone — the query side skips zero-length
    // broker TXT (same contract as canary-display / canary-wap).
    MDNS.addServiceTxt(SVC, PROTO, "broker", "");
    MDNS.addServiceTxt(SVC, PROTO, "bport", "");
  }
}

bool announce() {
  MDNS.end();
  if (!MDNS.begin(s_host)) return false;
  // The service port is a formality — this variant runs no HTTP listener;
  // the payload is the TXT set.
  MDNS.addService(SVC, PROTO, 1);
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

}  // namespace canary::net
