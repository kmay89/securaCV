// src/net/glass_web.cpp — the display's own web page. See glass_web.h.
#include "flavor_config.h"
#include <Arduino.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <string.h>

#include <WiFi.h>

#include "glass_web.h"
#include "wifi_mgr.h"
#include "mirror_html.h"
#include "tv_html.h"
#include "character.h"
#include "glass_settings.h"
#include "runtime_config.h"
#include "version.h"
#include "log.h"
#include "fleet_selfreport.h"  // shared /api/fleet body builder
#ifdef CD_NIGHTLIGHT
// The nightlight's /api/settings extras: the lamp (lantern prefs), the
// nightlight glue prefs, and the scene catalog served by name.
#include "lantern.h"
#include "nightlight_glue.h"
#include "fleet_instance.h"
#include "color/look_engine.h"
#endif

namespace canary {
// The browser serial monitor's hook (declared in log.h).
LogSink g_log_sink = nullptr;
}  // namespace canary

namespace canary::net {

namespace {

WebServer* s_server = nullptr;
bool s_mdns_added = false;

// ── Log ring: the last N lines log_line() spoke, timestamped. RAM-only,
// loop-task-only (every log_line caller and the server share the loop).
constexpr int LOG_LINES = 48;
constexpr int LOG_W = 104;
char s_log[LOG_LINES][LOG_W];
uint8_t s_log_head = 0, s_log_n = 0;

void log_capture(const char* tag, const char* msg) {
  const uint32_t ds = millis() / 100;  // uptime in deciseconds
  snprintf(s_log[s_log_head], LOG_W, "%6lu.%lus [%s] %s",
           (unsigned long)(ds / 10), (unsigned long)(ds % 10),
           tag ? tag : "", msg ? msg : "");
  s_log_head = (uint8_t)((s_log_head + 1) % LOG_LINES);
  if (s_log_n < LOG_LINES) s_log_n++;
}

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

// The television surface: the same /api/glass snapshot, laid out for a
// 10-foot glance instead of a phone in the hand. A TV on the home WiFi
// pointed at http://<canary>.local/tv is a conformant Open Ambient Security
// Display with no hub in the loop. Self-contained, same LAN promise as the
// mirror. See tv/index.html (its source) and docs/hardware/tv_display_design.md.
void handle_tv() {
  s_server->send_P(200, "text/html", TV_HTML);
}

void handle_glass() {
  // 16 witnesses (~90 B each) + the voiced/palette head (~420 B) peaked
  // near the old 2048 — headroom so bappend's clamp (safe but truncating)
  // never has to cut a document the mirror then fails to parse.
  static char body[2688];
  size_t o = 0;
  const size_t C = sizeof(body);
#if defined(CD_FLAVOR_WATCH)
  const char* flavor = "watch";
#elif defined(CD_NIGHTLIGHT)
  const char* flavor = "nightlight";
#elif defined(CD_FLAVOR_NIGHTSTAND)
  const char* flavor = "nightstand";
#else
  const char* flavor = "dash";
#endif
  // The mirror speaks in the wall's voice: the active Character's calm
  // words ride the snapshot (static ASCII table strings — JSON-safe by
  // construction) so glass and phone can never disagree on register.
  // Trouble words are NOT here: the mirror derives those from `worst`
  // exactly like the wall does, from the same invariant vocabulary.
  const auto& voice = canary::ui::active_voice();
  // Day-look palette parity (Character wave 4): the mirror re-skins its
  // page in the wall's chosen Character — ground/tiers from the def, the
  // Character's own day semantic set (Almanac's paper stops belong with
  // its paper ground). The wall's night is sent as the `night` flag and
  // the mirror keeps its own warm-dim night emulation on top, exactly as
  // it did when the mirror knew only Quiet Glass.
  const auto& cdef = canary::ui::active_character_def();
  o = bappend(body, C, o,
              "{\"flavor\":\"%s\",\"night\":%d,\"time_valid\":%d,"
              "\"hh\":%d,\"mm\":%d,\"wifi\":%d,\"hub\":%d,\"bird\":%u,"
              "\"worst\":%u,\"acked\":%d,\"aq\":\"%s\",\"aql\":\"%s\","
              "\"pal\":{\"bg\":\"%06lX\",\"cd\":\"%06lX\",\"ed\":\"%06lX\","
              "\"tx\":\"%06lX\",\"mu\":\"%06lX\",\"ok\":\"%06lX\","
              "\"wa\":\"%06lX\",\"al\":\"%06lX\",\"si\":\"%06lX\"},"
              "\"witnesses\":[",
              flavor, s_snap.night, s_snap.time_valid, s_snap.clock_hh,
              s_snap.clock_mm, s_snap.wifi_ok, s_snap.mqtt_ok, s_snap.bird,
              s_snap.worst, s_snap.acked, voice.all_quiet,
              voice.all_quiet_low, (unsigned long)cdef.pal.bg,
              (unsigned long)cdef.pal.surface, (unsigned long)cdef.pal.edge,
              (unsigned long)cdef.pal.text, (unsigned long)cdef.pal.muted,
              (unsigned long)cdef.sem.ok, (unsigned long)cdef.sem.warn,
              (unsigned long)cdef.sem.alert, (unsigned long)cdef.sem.signed_);
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
  char body[704];
  size_t o = (size_t)snprintf(
      body, sizeof(body),
      "{\"day_pct\":%u,\"night_screen\":%u,\"red_shift\":%u,"
      "\"peek_s\":%u,\"night_start_hh\":%u,\"night_end_hh\":%u,"
      "\"night_step\":%d,\"night_steps\":%d",
      gs.day_pct, gs.night_screen, gs.red_shift, gs.peek_s,
      gs.night_start_hh, gs.night_end_hh,
      canary::glass::night_duty_step(floor_duty_now(), gs.night_duty),
      canary::glass::NIGHT_STEPS);
#ifdef CD_NIGHTLIGHT
  // The nightlight's own knobs, plus the scene catalog BY NAME — the app
  // renders the device's own list, so a new scene in the look engine shows
  // up on the phone with no app update (the device describes, the app
  // renders). lamp_pct is the drawn lamp strength; the 50% backlight duty
  // ceiling (CD_BL_MAX_PCT) is enforced in the HAL underneath all of this.
  {
    auto& lamp = canary::care::lantern();
    o += (size_t)snprintf(
        body + o, sizeof(body) - o,
        ",\"lamp_scene\":%u,\"lamp_auto\":%u,\"lamp_pct\":%u,"
        "\"lamp_max_duty_pct\":%d,\"clock_12h\":%u,"
        "\"orientation\":%u,\"auto_rotate\":%u,\"scenes\":[",
        lamp.scene(), lamp.auto_mode(),
        (unsigned)(((uint16_t)canary::care::nightlight_lamp_bri() * 100 + 127) / 255),
        CD_BL_MAX_PCT, canary::care::nightlight_clock_12h() ? 1u : 0u,
        canary::care::nightlight_rotation(),
        canary::care::nightlight_auto_rotate() ? 1u : 0u);
    for (uint8_t i = 0; i < canary::color::kSceneCount && o < sizeof(body); i++) {
      o += (size_t)snprintf(body + o, sizeof(body) - o, "%s\"%s\"",
                            i ? "," : "", canary::color::kScenes[i].name);
    }
    if (o < sizeof(body))
      o += (size_t)snprintf(body + o, sizeof(body) - o, "]");
  }
#endif
  if (o < sizeof(body)) snprintf(body + o, sizeof(body) - o, "}");
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
  // Two stored modes only (glow / off) — what "off" does on tap (peek vs
  // wake) is per-flavor behavior, not a third value (review catch: a 2
  // would be silently sanitized back to glow).
  else if (k == "night_screen" && v >= 0 && v <= 1) gs.night_screen = (uint8_t)v;
  else if (k == "red_shift" && (v == 0 || v == 1)) gs.red_shift = (uint8_t)v;
  else if (k == "peek_s" && (v == 3 || v == 5 || v == 10)) gs.peek_s = (uint8_t)v;
  else if (k == "night_start_hh" && v >= 0 && v <= 23) gs.night_start_hh = (uint8_t)v;
  else if (k == "night_end_hh" && v >= 0 && v <= 23) gs.night_end_hh = (uint8_t)v;
  else if (k == "night_step" && v >= 1 && v <= canary::glass::NIGHT_STEPS)
    gs.night_duty = canary::glass::night_step_duty(floor_duty_now(), (int)v);
#ifdef CD_NIGHTLIGHT
  // The nightlight's knobs persist through their own stores (lantern prefs
  // NVS / the nightlight glue) — the glass settings blob is left alone, so
  // the early-return below must not mark it dirty for these.
  else if (k == "lamp_scene" && v >= 0 && v < canary::color::kSceneCount) {
    auto& lamp = canary::care::lantern();
    lamp.configure((uint8_t)v, lamp.minutes(), lamp.auto_mode());
    canary::care::lantern_prefs_changed();
    canary::fleet::the_fleet().mark_dirty();
    s_server->send(200, "application/json", "{\"ok\":true}");
    return;
  } else if (k == "lamp_auto" && (v == 0 || v == 1)) {
    auto& lamp = canary::care::lantern();
    lamp.configure(lamp.scene(), lamp.minutes(), (uint8_t)v);
    canary::care::lantern_prefs_changed();
    canary::fleet::the_fleet().mark_dirty();
    s_server->send(200, "application/json", "{\"ok\":true}");
    return;
  } else if (k == "lamp_pct" && v >= 10 && v <= 100) {
    canary::care::nightlight_set_lamp_bri((uint8_t)((v * 255) / 100));
    canary::fleet::the_fleet().mark_dirty();
    s_server->send(200, "application/json", "{\"ok\":true}");
    return;
  } else if (k == "clock_12h" && (v == 0 || v == 1)) {
    canary::care::nightlight_set_clock_12h(v == 1);
    canary::fleet::the_fleet().mark_dirty();
    s_server->send(200, "application/json", "{\"ok\":true}");
    return;
  } else if (k == "orientation" && v >= 0 && v <= 3) {
    // A hand on the dial parks AUTO (the toggle brings it back). The
    // rotation itself is a mailbox: main.cpp's loop applies it on the
    // single path (panel + LVGL + rebuild + tumble) — a web handler must
    // never rebuild the face from inside a request.
    canary::care::nightlight_set_auto_rotate(false);
    canary::care::nightlight_request_rotation((uint8_t)v);
    s_server->send(200, "application/json", "{\"ok\":true}");
    return;
  } else if (k == "auto_rotate" && (v == 0 || v == 1)) {
    canary::care::nightlight_set_auto_rotate(v == 1);
    s_server->send(200, "application/json", "{\"ok\":true}");
    return;
  }
#endif
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

// The receipt: what this unit IS — chip, memory, radio, firmware, and the
// capabilities this build carries. Everything the boot banner knows, on
// the phone.
void handle_device() {
  static char body[1024];
  size_t o = 0;
  const size_t C = sizeof(body);
#if defined(CD_FLAVOR_WATCH)
  const char* flavor = "watch";
#elif defined(CD_NIGHTLIGHT)
  const char* flavor = "nightlight";
#elif defined(CD_FLAVOR_NIGHTSTAND)
  const char* flavor = "nightstand";
#else
  const char* flavor = "dash";
#endif
  o = bappend(body, C, o,
              "{\"fw\":\"%s\",\"flavor\":\"%s\",\"chip\":\"%s r%d\","
              "\"cores\":%d,\"mhz\":%lu,\"flash_mb\":%lu,\"psram_kb\":%lu,"
              "\"heap_kb\":%lu,\"heap_min_kb\":%lu,\"up_s\":%lu,",
              CANARY_FW_VERSION, flavor, ESP.getChipModel(),
              ESP.getChipRevision(), ESP.getChipCores(),
              (unsigned long)ESP.getCpuFreqMHz(),
              (unsigned long)(ESP.getFlashChipSize() >> 20),
              (unsigned long)(ESP.getPsramSize() >> 10),
              (unsigned long)(ESP.getFreeHeap() >> 10),
              (unsigned long)(ESP.getMinFreeHeap() >> 10),
              (unsigned long)(millis() / 1000));
  o = bappend(body, C, o, "\"id\":");
  o = bappend_jstr(body, C, o, canary::cfg::get().device_id);
  o = bappend(body, C, o, ",\"ssid\":");
  o = bappend_jstr(body, C, o, wifi_connected() ? WiFi.SSID().c_str() : "");
  o = bappend(body, C, o, ",\"ip\":\"%s\",\"signal\":%d,\"caps\":[",
              wifi_connected() ? WiFi.localIP().toString().c_str() : "",
              wifi_connected() ? (int)WiFi.RSSI() : 0);
  // Capabilities as compiled into THIS build — the honest feature list.
  const char* caps[] = {
    "live glass mirror",
#if defined(FEATURE_CARE) && FEATURE_CARE
    "care + wellbeing",
#endif
#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
    "proof-carrying history",
#endif
#if defined(FEATURE_MDNS_DISCOVERY) && FEATURE_MDNS_DISCOVERY
    "finds its fleet by itself",
#endif
#if defined(FEATURE_QR_COMMISSION) && FEATURE_QR_COMMISSION
    "QR canary onboarding",
#endif
#if defined(FEATURE_SNTP) && FEATURE_SNTP
    "atomic-clock time (two sources)",
#endif
    "living canary mood engine",
  };
  for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); ++i) {
    if (i) o = bappend(body, C, o, ",");
    o = bappend_jstr(body, C, o, caps[i]);
  }
  o = bappend(body, C, o, "]}");
  s_server->send(200, "application/json", body);
}

// The browser serial monitor: the same lines the USB cable would show.
void handle_log() {
  static char body[LOG_LINES * LOG_W];
  size_t o = 0;
  for (uint8_t i = 0; i < s_log_n; ++i) {
    const uint8_t idx =
        (uint8_t)((s_log_head + LOG_LINES - s_log_n + i) % LOG_LINES);
    o = bappend(body, sizeof(body), o, "%s\n", s_log[idx]);
  }
  s_server->send(200, "text/plain", body);
}

// GET /api/fleet — the coarse, UNAUTHENTICATED fleet presence/health contract
// (tvos/discovery/DISCOVERY.md), the same shape canary-wap answers and the
// Witness Wall emulator + Flasher's post-flash LAN discovery read. The wire
// shape is built by the ONE shared builder (fleet_selfreport.h in
// firmware/common, host-tested), so every networked board answers byte-for-byte
// identically — across BOTH server styles (this Arduino WebServer and
// canary-wap's esp_http_server). A display keeps no witness chain of its own
// (it renders the fleet's), so it honestly reports chain "unknown".
void handle_fleet() {
  const auto& cfg = canary::cfg::get();
  FleetSelfDevice self{};
  self.name         = (cfg.device_id[0]) ? cfg.device_id : "Canary";
#ifdef CD_NIGHTLIGHT
  // The nightlight self-reports as WHAT IT IS ("canary-nightlight", the
  // same dt its mDNS TXT carries), so the iPhone's LAN self-report path
  // types it correctly and shows the Nightlight settings card (Codex P2 on
  // this PR). The other display flavors keep the family string below —
  // their CD_DEVICE_TYPEs predate the wire and the apps type them off it.
  self.product      = DEVICE_TYPE;
#else
  self.product      = "canary-display";
#endif
  self.online       = 1;   // we are answering this request, so we are up
  self.chain_ok     = 0;   // a display holds no witness chain of its own
  self.chain_height = -1;  // omit chain_height
  // Sized by the shared macro for the worst case: device_id[48] of
  // all-escaping bytes expands 6x and is written twice (kernel + name) — a
  // smaller fixed buffer would truncate an accepted name into invalid JSON
  // served with a 200 (Codex P2 on #1226).
  char body[FLEET_SELFREPORT_BODY_CAP(sizeof(cfg.device_id), 16)];
  fleet_selfreport_build(body, sizeof(body), &self);
  s_server->sendHeader("Access-Control-Allow-Origin", "*");
  s_server->send(200, "application/json", body);
}

// OPTIONS /api/fleet — CORS preflight (DISCOVERY.md). A simple GET doesn't
// trigger one, but answering keeps stricter cross-origin clients happy.
void handle_fleet_options() {
  s_server->sendHeader("Access-Control-Allow-Origin", "*");
  s_server->sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  s_server->send(204, "text/plain", "");
}

void glass_web_init() {
  if (s_server) return;
  canary::g_log_sink = log_capture;  // browser serial monitor from here on
  s_server = new WebServer(80);
  s_server->on("/", HTTP_GET, handle_root);
  s_server->on("/tv", HTTP_GET, handle_tv);
  s_server->on("/api/glass", HTTP_GET, handle_glass);
  s_server->on("/api/fleet", HTTP_GET, handle_fleet);
  s_server->on("/api/fleet", HTTP_OPTIONS, handle_fleet_options);
  s_server->on("/api/device", HTTP_GET, handle_device);
  s_server->on("/api/log", HTTP_GET, handle_log);
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
