/*
 * SecuraCV Canary — First-Time Setup implementation
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_setup.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <string.h>

#include "canary_config.h"
#include "log_level.h"
#include "securacv_witness.h"

namespace setup_wiz {

static bool s_initialized = false;
static bool s_first_boot = false;
static bool s_active = false;
static uint32_t s_started_ms = 0;

static WiFiUDP s_dns_udp;
static bool s_dns_running = false;
static constexpr uint16_t DNS_PORT = 53;

static char s_device_name[SETUP_DEVICE_NAME_MAX + 1] = {0};

static void dns_respond(const uint8_t* query, size_t len, IPAddress client_ip, uint16_t client_port) {
  if (len < 12) return;

  IPAddress ap_ip = WiFi.softAPIP();
  uint8_t response[512];
  if (len > sizeof(response) - 16) return;

  memcpy(response, query, len);

  response[2] = 0x84 | (query[2] & 0x01);
  response[3] = 0x00;
  response[6] = 0x00;
  response[7] = 0x01;
  response[8] = 0x00;
  response[9] = 0x00;
  response[10] = 0x00;
  response[11] = 0x00;

  size_t pos = len;
  response[pos++] = 0xC0; response[pos++] = 0x0C;
  response[pos++] = 0x00; response[pos++] = 0x01;
  response[pos++] = 0x00; response[pos++] = 0x01;
  response[pos++] = 0x00; response[pos++] = 0x00;
  response[pos++] = 0x00; response[pos++] = 0x3C;
  response[pos++] = 0x00; response[pos++] = 0x04;
  response[pos++] = ap_ip[0]; response[pos++] = ap_ip[1];
  response[pos++] = ap_ip[2]; response[pos++] = ap_ip[3];

  s_dns_udp.beginPacket(client_ip, client_port);
  s_dns_udp.write(response, pos);
  s_dns_udp.endPacket();
}

}  /* namespace setup_wiz */


extern "C" {

bool setup_init(void) {
  using namespace setup_wiz;
  if (s_initialized) return true;

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
    log_health(LOG_LEVEL_INFO, LOG_CAT_SYSTEM,
               "First boot detected", "setup mode active");
  }

  s_initialized = true;
  return true;
}

bool setup_is_first_boot(void) {
  return setup_wiz::s_first_boot;
}

bool setup_is_active(void) {
  return setup_wiz::s_active;
}

void setup_mark_complete(void) {
  using namespace setup_wiz;
  Preferences prefs;
  if (prefs.begin("securacv", false)) {
    prefs.putBool("setup_ok", true);
    prefs.end();
  }
  s_first_boot = false;
  s_active = false;
  setup_stop_captive_portal();
  log_health(LOG_LEVEL_INFO, LOG_CAT_SYSTEM,
             "Setup complete", "normal operation");
}

void setup_dns_process(void) {
  using namespace setup_wiz;
  if (!s_dns_running) return;

  int pkt_size = s_dns_udp.parsePacket();
  if (pkt_size <= 0) return;

  uint8_t buf[512];
  int len = s_dns_udp.read(buf, sizeof(buf));
  if (len < 12) return;

  dns_respond(buf, (size_t)len, s_dns_udp.remoteIP(), s_dns_udp.remotePort());
}

bool setup_start_captive_portal(void) {
  using namespace setup_wiz;
  if (s_dns_running) return true;
  if (s_dns_udp.begin(DNS_PORT)) {
    s_dns_running = true;
    log_health(LOG_LEVEL_INFO, LOG_CAT_SYSTEM,
               "Captive portal DNS started", "port 53");
    return true;
  }
  return false;
}

void setup_stop_captive_portal(void) {
  using namespace setup_wiz;
  if (!s_dns_running) return;
  s_dns_udp.stop();
  s_dns_running = false;
}

void setup_check_timeout(void) {
  using namespace setup_wiz;
  if (!s_active) return;
  if ((millis() - s_started_ms) >= SETUP_TIMEOUT_MS) {
    s_active = false;
    setup_stop_captive_portal();
    log_health(LOG_LEVEL_INFO, LOG_CAT_SYSTEM,
               "Setup timeout", "rebooting to normal operation");
    delay(500);
    ESP.restart();
  }
}

bool setup_get_device_name(char* out, size_t len) {
  if (!out || len == 0) return false;
  if (setup_wiz::s_device_name[0] == '\0') {
    out[0] = '\0';
    return false;
  }
  strncpy(out, setup_wiz::s_device_name, len - 1);
  out[len - 1] = '\0';
  return true;
}

bool setup_set_device_name(const char* name) {
  using namespace setup_wiz;
  if (!name) return false;
  size_t nlen = strlen(name);
  if (nlen == 0 || nlen > SETUP_DEVICE_NAME_MAX) return false;

  strncpy(s_device_name, name, sizeof(s_device_name) - 1);
  s_device_name[sizeof(s_device_name) - 1] = '\0';

  Preferences prefs;
  if (prefs.begin("securacv", false)) {
    prefs.putString("dev_name", s_device_name);
    prefs.end();
  }
  log_health(LOG_LEVEL_INFO, LOG_CAT_SYSTEM,
             "Device name updated", s_device_name);
  return true;
}

}  /* extern "C" */
