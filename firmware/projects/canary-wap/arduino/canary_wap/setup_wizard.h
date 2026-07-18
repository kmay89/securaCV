/*
 * SecuraCV Canary WAP — First-Time Setup & Captive Portal
 *
 * Arduino-compatible equivalent of firmware/canary/lib/securacv_setup/.
 * Single-header implementation following the WAP's existing pattern.
 *
 * A captive-portal DNS server redirects all A-record queries to 192.168.4.1
 * so canary.local (and any typed hostname) lands on the device. It runs for
 * the whole lifetime of the always-on AP — not just first boot — so a phone
 * that joins the management AP after provisioning (e.g. home WiFi dropped)
 * still resolves the device instead of being flagged "no internet" and
 * disconnected. The AP SSID ("SecuraCV-XXXX") is the same in setup and in
 * steady state.
 *
 * The first-boot *wizard* (NVS "setup_ok" absent → is_active()) is a separate
 * concern layered on top: it gates the setup-vs-dashboard landing and the
 * 15-minute setup timeout. Setup completes when WiFi credentials are saved.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_SETUP_WIZARD_H
#define SECURACV_SETUP_WIZARD_H

#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "captive_dns.h"          // pure DNS response builder (host-tested)
#include "provisioning_logic.h"   // pure timeout/teardown decisions (host-tested)

namespace setup_wizard {

static constexpr size_t DEVICE_NAME_MAX = 32;
static constexpr uint32_t SETUP_TIMEOUT_MS = 15UL * 60UL * 1000UL;
static constexpr uint16_t DNS_PORT = 53;

static bool s_first_boot = false;
static bool s_active = false;
static uint32_t s_started_ms = 0;
static char s_device_name[DEVICE_NAME_MAX + 1] = {0};

static WiFiUDP s_dns_udp;
static bool s_dns_running = false;

inline bool init() {
  Preferences prefs;
  if (prefs.begin("securacv", true)) {
    s_first_boot = !prefs.getBool("setup_ok", false);
    prefs.getString("dev_name", s_device_name, sizeof(s_device_name));
    prefs.end();
  } else {
    s_first_boot = true;
  }
  if (s_first_boot) {
    s_active = true;
    s_started_ms = millis();
  }
  return true;
}

inline bool is_first_boot() { return s_first_boot; }
inline bool is_active() { return s_active; }

// The 15-minute window exists to close an *abandoned* portal, not to reboot
// the device under a slow human. Any wizard-driven API activity (scan,
// connect, token refresh) calls touch() so the countdown restarts from the
// most recent sign of life. touch() runs on the HTTP server task while
// check_timeout() reads from the main loop task, so the stamp goes through
// atomic builtins (same pattern as g_provisioning_gate_opened_at).
inline void touch() {
  if (s_active) {
    __atomic_store_n(&s_started_ms, millis(), __ATOMIC_RELEASE);
  }
}

inline void mark_complete() {
  Preferences prefs;
  if (prefs.begin("securacv", false)) {
    prefs.putBool("setup_ok", true);
    prefs.end();
  }
  s_first_boot = false;
  s_active = false;
  if (s_dns_running) {
    s_dns_udp.stop();
    s_dns_running = false;
  }
}

inline bool start_captive_portal() {
  if (s_dns_running) return true;
  if (s_dns_udp.begin(DNS_PORT)) {
    s_dns_running = true;
    return true;
  }
  return false;
}

inline void stop_captive_portal() {
  if (!s_dns_running) return;
  s_dns_udp.stop();
  s_dns_running = false;
}

inline void dns_process() {
  if (!s_dns_running) return;
  // Drain a small burst per loop pass, not one packet. A phone probing the
  // captive network fires several DNS queries back-to-back (connectivity
  // check, OS probe hosts, the portal URL); answering one per pass under a
  // busy loop let the rest time out and the first captive sheet attempt
  // "fail" — retry then worked off warm caches. Budget-bounded so a DNS
  // flood still can't own the loop task.
  for (int budget = 0; budget < 6; ++budget) {
    int pkt_size = s_dns_udp.parsePacket();
    if (pkt_size <= 0) return;

    uint8_t buf[512];
    int len = s_dns_udp.read(buf, sizeof(buf));
    if (len < 12) continue;

    IPAddress ap_ip = WiFi.softAPIP();
    uint8_t ip[4] = { ap_ip[0], ap_ip[1], ap_ip[2], ap_ip[3] };
    uint8_t response[512];
    size_t out_len = captive_dns::build_response(buf, (size_t)len, ip,
                                                 response, sizeof(response));
    if (out_len == 0) continue;

    s_dns_udp.beginPacket(s_dns_udp.remoteIP(), s_dns_udp.remotePort());
    s_dns_udp.write(response, out_len);
    s_dns_udp.endPacket();
  }
}

inline void check_timeout() {
  uint32_t started = __atomic_load_n(&s_started_ms, __ATOMIC_ACQUIRE);
  if (provisioning_logic::setup_timeout_due(s_active, millis(), started,
                                            SETUP_TIMEOUT_MS)) {
    s_active = false;
    stop_captive_portal();
    delay(500);
    ESP.restart();
  }
}

inline const char* get_device_name() {
  return s_device_name[0] ? s_device_name : nullptr;
}

inline bool set_device_name(const char* name) {
  if (!name || strlen(name) == 0 || strlen(name) > DEVICE_NAME_MAX) return false;
  strncpy(s_device_name, name, sizeof(s_device_name) - 1);
  s_device_name[sizeof(s_device_name) - 1] = '\0';
  Preferences prefs;
  if (prefs.begin("securacv", false)) {
    prefs.putString("dev_name", s_device_name);
    prefs.end();
  }
  return true;
}

}  /* namespace setup_wizard */

#endif  /* SECURACV_SETUP_WIZARD_H */
