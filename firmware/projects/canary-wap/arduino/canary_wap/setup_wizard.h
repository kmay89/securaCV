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
  int pkt_size = s_dns_udp.parsePacket();
  if (pkt_size <= 0) return;

  uint8_t buf[512];
  int len = s_dns_udp.read(buf, sizeof(buf));
  if (len < 12) return;

  if (buf[2] & 0x80) return;                        // already a response
  uint16_t qdcount = ((uint16_t)buf[4] << 8) | buf[5];
  if (qdcount < 1) return;

  // Walk the first QNAME to find its QTYPE. Query QNAMEs aren't compressed,
  // so this is a simple label walk: each label is [len][bytes], terminated
  // by a zero-length label.
  size_t q = 12;
  while (q < (size_t)len && buf[q] != 0x00) {
    q += (size_t)buf[q] + 1;
  }
  uint16_t qtype = 0;
  if (q + 2 < (size_t)len) {                        // QTYPE follows the 0x00
    qtype = ((uint16_t)buf[q + 1] << 8) | buf[q + 2];
  }

  IPAddress ap_ip = WiFi.softAPIP();
  uint8_t response[512];
  if ((size_t)len > sizeof(response) - 16) return;

  memcpy(response, buf, len);
  response[2] = 0x84 | (buf[2] & 0x01);             // QR=1, AA=1, preserve RD
  response[3] = 0x00;                               // RA=0, RCODE=0 (NOERROR)
  response[6] = 0x00;                               // ANCOUNT high byte
  response[8] = 0x00; response[9]  = 0x00;          // NSCOUNT
  response[10] = 0x00; response[11] = 0x00;         // ARCOUNT

  size_t pos = len;

  // Only A (type 1) queries get the captive-redirect address. AAAA (28),
  // HTTPS/SVCB (65) and everything else get NOERROR + ANCOUNT=0 (NODATA), so
  // the client falls straight back to its IPv4 (A) lookup instead of stalling
  // on a malformed answer. Android Chrome fires AAAA and HTTPS probes
  // alongside the A query, so answering those wrong is what made canary.local
  // / the redirect resolve slowly or not at all on Android.
  if (qtype == 0x0001) {
    response[7] = 0x01;                             // ANCOUNT = 1
    response[pos++] = 0xC0; response[pos++] = 0x0C; // NAME -> offset 12
    response[pos++] = 0x00; response[pos++] = 0x01; // TYPE A
    response[pos++] = 0x00; response[pos++] = 0x01; // CLASS IN
    response[pos++] = 0x00; response[pos++] = 0x00;
    response[pos++] = 0x00; response[pos++] = 0x3C; // TTL 60s
    response[pos++] = 0x00; response[pos++] = 0x04; // RDLENGTH 4
    response[pos++] = ap_ip[0]; response[pos++] = ap_ip[1];
    response[pos++] = ap_ip[2]; response[pos++] = ap_ip[3];
  } else {
    response[7] = 0x00;                             // ANCOUNT = 0 (NODATA)
  }

  s_dns_udp.beginPacket(s_dns_udp.remoteIP(), s_dns_udp.remotePort());
  s_dns_udp.write(response, pos);
  s_dns_udp.endPacket();
}

inline void check_timeout() {
  if (!s_active) return;
  if ((millis() - s_started_ms) >= SETUP_TIMEOUT_MS) {
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
