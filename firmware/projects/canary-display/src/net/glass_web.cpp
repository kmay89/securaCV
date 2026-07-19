// src/net/glass_web.cpp — the display's own web page. See glass_web.h.
#include <config.h>
#include <Arduino.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <string.h>

#include "canary/net/glass_web.h"
#include "canary/net/wifi_mgr.h"
#include "canary/net/mirror_html.h"
#include "canary/glass_settings.h"
#include "canary/log.h"

namespace canary::net {

namespace {

WebServer* s_server = nullptr;
bool s_mdns_added = false;

// The live snapshot: written by the loop task in glass_web_publish, read
// by the same task inside handleClient() — WebServer runs on the caller's
// task, so this is single-threaded by construction (no locks needed).
struct WitnessSnap {
  char name[24];
  char room[16];
  uint8_t sev;       // canary::fleet::Sev ordinal
  uint32_t age_s;    // since last heard
  bool wb_present;
  bool wb_breathing;
};
struct GlassSnap {
  uint8_t n = 0;
  WitnessSnap w[CD_FLEET_MAX_DEVICES];
  uint8_t worst = 0;
  bool acked = false;
  bool night = false;
  bool time_valid = false;
  int clock_hh = 0, clock_mm = 0;
  bool wifi_ok = false, mqtt_ok = false;
  uint8_t bird = 0;  // canary::ui::CanaryMood ordinal
};
GlassSnap s_snap;

// Append printf-style into a fixed buffer; returns new offset (clamped).
size_t bappend(char* buf, size_t cap, size_t off, const char* fmt, ...) {
  if (off >= cap) return off;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + off, cap - off, fmt, ap);
  va_end(ap);
  if (n < 0) return off;
  size_t no = off + (size_t)n;
  return no < cap ? no : cap - 1;
}

// JSON string escape for names that arrive over MQTT meta — quotes,
// backslashes and control bytes must not break the document.
size_t bappend_jstr(char* buf, size_t cap, size_t off, const char* s) {
  off = bappend(buf, cap, off, "\"");
  for (; s && *s && off + 6 < cap; ++s) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\')
      off = bappend(buf, cap, off, "\\%c", c);
    else if (c < 0x20)
      off = bappend(buf, cap, off, "\\u%04x", c);
    else
      buf[off++] = (char)c;
  }
  buf[off] = '\0';
  return bappend(buf, cap, off, "\"");
}

void handle_root() {
  s_server->send_P(200, "text/html", MIRROR_HTML);
}

void handle_glass() {
  static char body[2048];
  size_t o = 0;
  const size_t C = sizeof(body);
#ifdef CD_FLAVOR_WATCH
  const char* flavor = "watch";
#else
  const char* flavor = "dash";
#endif
  o = bappend(body, C, o,
              "{\"flavor\":\"%s\",\"night\":%d,\"time_valid\":%d,"
              "\"hh\":%d,\"mm\":%d,\"wifi\":%d,\"hub\":%d,\"bird\":%u,"
              "\"worst\":%u,\"acked\":%d,\"witnesses\":[",
              flavor, s_snap.night, s_snap.time_valid, s_snap.clock_hh,
              s_snap.clock_mm, s_snap.wifi_ok, s_snap.mqtt_ok, s_snap.bird,
              s_snap.worst, s_snap.acked);
  for (uint8_t i = 0; i < s_snap.n; ++i) {
    const auto& w = s_snap.w[i];
    if (i) o = bappend(body, C, o, ",");
    o = bappend(body, C, o, "{\"name\":");
    o = bappend_jstr(body, C, o, w.name);
    o = bappend(body, C, o, ",\"room\":");
    o = bappend_jstr(body, C, o, w.room);
    o = bappend(body, C, o, ",\"sev\":%u,\"age_s\":%lu,\"wb\":%d,\"br\":%d}",
                w.sev, (unsigned long)w.age_s, w.wb_present, w.wb_breathing);
  }
  o = bappend(body, C, o, "]}");
  s_server->send(200, "application/json", body);
}

uint16_t floor_duty_now() {
  const auto& cal = canary::glass::nightcal();
  return cal.valid ? cal.floor_duty : canary::glass::NIGHT_FLOOR_DFLT;
}

void handle_settings_get() {
  const auto& gs = canary::glass::settings();
  char body[256];
  snprintf(body, sizeof(body),
           "{\"day_pct\":%u,\"night_screen\":%u,\"red_shift\":%u,"
           "\"peek_s\":%u,\"night_start_hh\":%u,\"night_end_hh\":%u,"
           "\"night_step\":%d,\"night_steps\":%d}",
           gs.day_pct, gs.night_screen, gs.red_shift, gs.peek_s,
           gs.night_start_hh, gs.night_end_hh,
           canary::glass::night_duty_step(floor_duty_now(), gs.night_duty),
           canary::glass::NIGHT_STEPS);
  s_server->send(200, "application/json", body);
}

// One knob per request: /api/set?k=day_pct&v=60. The glass's own settings
// engine validates and debounces the NVS commit, exactly as if the change
// had been made on the panel.
void handle_settings_set() {
  const String k = s_server->arg("k");
  const long v = s_server->arg("v").toInt();
  auto gs = canary::glass::settings();  // copy; setters below persist
  bool ok = true;
  if (k == "day_pct" && v >= 20 && v <= 100) gs.day_pct = (uint8_t)v;
  else if (k == "night_screen" && v >= 0 && v <= 2) gs.night_screen = (uint8_t)v;
  else if (k == "red_shift" && (v == 0 || v == 1)) gs.red_shift = (uint8_t)v;
  else if (k == "peek_s" && (v == 3 || v == 5 || v == 10)) gs.peek_s = (uint8_t)v;
  else if (k == "night_start_hh" && v >= 0 && v <= 23) gs.night_start_hh = (uint8_t)v;
  else if (k == "night_end_hh" && v >= 0 && v <= 23) gs.night_end_hh = (uint8_t)v;
  else if (k == "night_step" && v >= 1 && v <= canary::glass::NIGHT_STEPS)
    gs.night_duty = canary::glass::night_step_duty(floor_duty_now(), (int)v);
  else ok = false;
  if (ok) {
    canary::glass::settings_mut() = gs;
    canary::glass::settings_mark_dirty();  // debounced NVS commit, same as
                                           // a change made on the panel
  }
  s_server->send(ok ? 200 : 400, "application/json",
                 ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

}  // namespace

void glass_web_init() {
  if (s_server) return;
  s_server = new WebServer(80);
  s_server->on("/", HTTP_GET, handle_root);
  s_server->on("/api/glass", HTTP_GET, handle_glass);
  s_server->on("/api/settings", HTTP_GET, handle_settings_get);
  s_server->on("/api/set", HTTP_POST, handle_settings_set);
  s_server->onNotFound([]() { s_server->send(404, "text/plain", "not here"); });
  s_server->begin();
  canary::log_line("WEB", "Glass mirror serving on :80");
}

void glass_web_tick(uint32_t) {
  if (!s_server) return;
  if (!s_mdns_added && wifi_connected()) {
    // Advertised once the STA is genuinely up — the WAP fleet page's
    // "Open" link and any phone browser resolve this.
    MDNS.addService("http", "tcp", 80);
    s_mdns_added = true;
  }
  s_server->handleClient();
}

void glass_web_publish(const canary::fleet::Fleet& fleet, uint32_t now_ms,
                       bool night, bool time_valid, int clock_hh,
                       int clock_mm, bool wifi_ok, bool mqtt_ok,
                       canary::ui::CanaryMood bird) {
  using canary::fleet::Sev;
  GlassSnap& s = s_snap;
  const int n = fleet.count();
  s.n = (uint8_t)(n > CD_FLEET_MAX_DEVICES ? CD_FLEET_MAX_DEVICES : n);
  for (int i = 0; i < (int)s.n; ++i) {
    const auto* w = fleet.at(i);
    auto& d = s.w[i];
    if (!w) {
      d.name[0] = d.room[0] = '\0';
      d.sev = 0;
      d.age_s = 0;
      d.wb_present = d.wb_breathing = false;
      continue;
    }
    strlcpy(d.name, w->name[0] ? w->name : w->id, sizeof(d.name));
    strlcpy(d.room, w->room, sizeof(d.room));
    d.sev = (uint8_t)fleet.witness_sev(*w, now_ms);
    d.age_s = w->last_seen_ms ? (now_ms - w->last_seen_ms) / 1000 : 0;
    d.wb_present = w->wb_present;
    d.wb_breathing = w->wb_breathing;
  }
  s.worst = (uint8_t)fleet.worst(now_ms);
  s.acked = fleet.ack_active(now_ms);
  s.night = night;
  s.time_valid = time_valid;
  s.clock_hh = clock_hh;
  s.clock_mm = clock_mm;
  s.wifi_ok = wifi_ok;
  s.mqtt_ok = mqtt_ok;
  s.bird = (uint8_t)bird;
}

}  // namespace canary::net
