// src/net/glass_web.cpp — the display's own web page. See glass_web.h.
#include <config.h>
#include <Arduino.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <string.h>

#include <WiFi.h>

#include "canary/net/glass_web.h"
#include "canary/net/wifi_mgr.h"
#include "canary/net/tz_auto.h"
#include "canary/net/mqtt_mgr.h"   // hub state for the /api/fleet self-report
#include "canary/net/mirror_html.h"
#include "canary/net/tv_html.h"
#include "canary/ui/character.h"
#include "canary/ui/clock_styles.h"
#if defined(CD_FLAVOR_DASH) && defined(FEATURE_STANDALONE_WEATHER) && \
    FEATURE_STANDALONE_WEATHER && defined(FEATURE_HUB_WEATHER) && \
    FEATURE_HUB_WEATHER
#define GW_WX 1
#include "canary/net/wx_direct.h"
#endif
#include "canary/glass_settings.h"
#include "canary/runtime_config.h"
#include "canary/version.h"
#include "canary/log.h"
#include "fleet_selfreport/fleet_selfreport.h"  // shared /api/fleet body builder
#include "pins.h"  // CANARY_FIGURE_HARDWARE — which board this is (board -I path)
#ifdef CD_NIGHTLIGHT
// The nightlight's /api/settings extras: the lamp (lantern prefs), the
// nightlight glue prefs, and the scene catalog served by name.
#include "canary/care/lantern.h"
#include "canary/care/nightlight_glue.h"
#include "canary/fleet/fleet_instance.h"
#include "color/look_engine.h"
// The shared look controls — the color wheel writes here, and /api/settings
// reads the live value back so the app never has to remember what it sent.
#include "canary/ui/look_state.h"
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
  char tz[48];
  canary::net::tz_current(tz, sizeof(tz));
  char body[1280];
  size_t o = (size_t)snprintf(
      body, sizeof(body),
      "{\"day_pct\":%u,\"night_screen\":%u,\"red_shift\":%u,"
      "\"peek_s\":%u,\"night_start_hh\":%u,\"night_end_hh\":%u,"
      "\"night_step\":%d,\"night_steps\":%d",
      gs.day_pct, gs.night_screen, gs.red_shift, gs.peek_s,
      gs.night_start_hh, gs.night_end_hh,
      canary::glass::night_duty_step(floor_duty_now(), gs.night_duty),
      canary::glass::NIGHT_STEPS);
  // The zone is operator-supplied text, so it leaves through the same escaper
  // the MQTT-sourced names use. The setter refuses anything unprintable, but
  // a value stored by an older build must not be able to break the document
  // this page parses — an unparseable /api/settings would take the settings
  // UI down until someone reflashed.
  o = bappend(body, sizeof(body), o, ",\"tz\":");
  o = bappend_jstr(body, sizeof(body), o, tz);
#ifdef CD_FLAVOR_DASH
  // THE BRIGHTNESS KNOB THAT WORKS ON THIS GLASS.
  //
  // `day_pct` above scales what backlight_set() is given — and on this panel
  // the backlight is a CH422G expander line, which is BINARY. Any nonzero
  // level is simply "on". So day_pct has always been served here, has always
  // been settable from the app, and has always done nothing you can see: the
  // owner drags a brightness slider on their phone and the screen in front of
  // them does not change. That is not a bug in the app; it is this endpoint
  // never having offered the control that does work.
  //
  // On a binary-backlight board the sustained brightness is `bright_pct`, a
  // RENDERED dim — a black scrim over the glass (lvgl_port_set_dim), applied
  // every frame by the main loop. It was reachable only from the on-glass
  // settings menu, so the phone had no way to write it.
  //
  // Sent only on the boards that HAVE it, exactly like the lamp block below:
  // its presence is how the app knows this glass dims by scrim rather than by
  // backlight, so it can offer one honest brightness control instead of two
  // that disagree. A display with a real dimmable backlight omits it and
  // keeps day_pct as the brightness knob.
  o += (size_t)snprintf(
      body + o, sizeof(body) - o,
      ",\"bright_pct\":%u,\"bright_min_pct\":%u",
      gs.bright_pct, canary::glass::BRIGHT_PCT_MIN);
  // THE LOOK OF THE GLASS, FROM THE PHONE. The Character (the curated
  // face/color ring) and the clock style were on-glass-only settings; the
  // app the owner actually holds could not change either. Served BY NAME —
  // the device describes, the app renders (the same contract the nightlight
  // scene catalog keeps) — so a new Character or clock face shows up on the
  // phone with no app update. `orientation` mirrors the on-glass editor for
  // the same reason; it shares the nightlight's key on purpose (one word,
  // one meaning, and the two flavors never serve it twice).
  o += (size_t)snprintf(
      body + o, sizeof(body) - o,
      ",\"orientation\":%u,\"character\":%u,\"clock_style\":%u,"
      "\"characters\":[",
      gs.rotation, gs.character, gs.clock_style);
  for (uint8_t i = 0; i < canary::ui::character_count() && o < sizeof(body);
       i++) {
    o += (size_t)snprintf(
        body + o, sizeof(body) - o, "%s\"%s\"", i ? "," : "",
        canary::ui::character_name((canary::ui::Character)i));
  }
  if (o < sizeof(body))
    o += (size_t)snprintf(body + o, sizeof(body) - o, "],\"clock_styles\":[");
  for (uint8_t i = 0; i < canary::ui::clock_style_count() && o < sizeof(body);
       i++) {
    o += (size_t)snprintf(body + o, sizeof(body) - o, "%s\"%s\"",
                          i ? "," : "", canary::ui::clock_style_name(i));
  }
  if (o < sizeof(body))
    o += (size_t)snprintf(body + o, sizeof(body) - o, "]");
#endif
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
        "\"orientation\":%u,\"auto_rotate\":%u,"
        // The wheel and the timer. lamp_hue is -1 when a catalog scene is on,
        // so the app can show WHICH control is currently the answer instead
        // of guessing from a color it sent earlier.
        "\"lamp_hue\":%d,\"lamp_minutes\":%u,\"scenes\":[",
        lamp.scene(), lamp.auto_mode(),
        (unsigned)(((uint16_t)canary::care::nightlight_lamp_bri() * 100 + 127) / 255),
        CD_BL_MAX_PCT, canary::care::nightlight_clock_12h() ? 1u : 0u,
        canary::care::nightlight_rotation(),
        canary::care::nightlight_auto_rotate() ? 1u : 0u,
        (int)canary::ui::look_params().custom_hue,
        (unsigned)lamp.minutes());
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
#ifdef CD_FLAVOR_DASH
  // The rendered-dim brightness — see handle_settings_get. Stored and nothing
  // more: the main loop reads settings().bright_pct every frame and drives the
  // scrim from it, so the glass follows on the next tick without a web handler
  // ever touching LVGL from inside a request (the same discipline the
  // orientation mailbox keeps, and for the same reason).
  //
  // Clamped through the glass's own bright_pct_clamp rather than range-checked
  // here, so the phone and the panel can never disagree about what a given
  // number means. Any value in [50..100] is legal and renders exactly —
  // bright_scrim_opa is continuous, and the settings blob's sanitizer checks
  // the RANGE, not step membership. The on-glass stepper's tens are the
  // thumb's granularity, not the setting's, so a slider is free to send 63.
  else if (k == "bright_pct" && v >= 0 && v <= 100)
    gs.bright_pct = canary::glass::bright_pct_clamp((int)v);
  // The look knobs (see handle_settings_get). Stored and nothing more — the
  // render loop applies a Character/clock/rotation change on the loop task
  // through the exact paths an on-glass tap uses (character_apply + the
  // ground-flip rebuild; lvgl_port rotation). A web handler never touches
  // LVGL from inside a request — the same mailbox discipline as bright_pct.
  else if (k == "character" && v >= 0 && v < canary::ui::character_count())
    gs.character = (uint8_t)v;
  else if (k == "clock_style" && v >= 0 && v < canary::ui::clock_style_count())
    gs.clock_style = (uint8_t)v;
  else if (k == "orientation" && v >= 0 && v <= 3)
    gs.rotation = (uint8_t)v;
  else if (k == "clock_12h" && (v == 0 || v == 1))
    gs.clock_12h = (uint8_t)v;
#ifdef GW_WX
  else if (k == "wx_direct" && (v == 0 || v == 1))
    gs.wx_direct = (uint8_t)v;
  else if (k == "wx_loc") {
    // The coarse location arrives as ONE combined integer (wx_loc_encode in
    // glass_settings.h) so half a coordinate can never be stored. The app
    // coarsens to the 0.1-degree grid before encoding; anything outside the
    // grid is refused here, exactly like every other knob.
    int16_t la, lo;
    if (!canary::glass::wx_loc_decode(v, &la, &lo)) ok = false;
    else {
      gs.wx_lat10 = la;
      gs.wx_lon10 = lo;
      gs.wx_loc_set = 1;
    }
  }
#endif
#endif
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
    // Choosing a scene puts the wheel away. Without this the chosen hue keeps
    // winning in current_look(), so the new scene is invisible and
    // /api/settings still reports the old color — two answers to "what color
    // is it?", which is the thing this pair of controls must never do.
    canary::ui::look_set_custom_hue(-1);
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
  } else if (k == "lamp_hue" && v >= -1 && v <= 359) {
    // The color wheel. -1 hands the glass back to the chosen scene, which is
    // how the app turns the wheel OFF without inventing a tenth scene.
    canary::ui::look_set_custom_hue((int16_t)v);
    canary::care::lantern_prefs_changed();
    canary::fleet::the_fleet().mark_dirty();
    s_server->send(200, "application/json", "{\"ok\":true}");
    return;
  } else if (k == "lamp_minutes" && v >= 1 && v <= 480) {
    // How long the lamp stays on. It was settable on the panel and by MQTT
    // but had no web key, so "rainbow for 15 minutes" could be asked for
    // everywhere except from the app the owner actually holds.
    //
    // The floor is 1, not 0: LanternModel clamps anything below a minute up
    // to one, so accepting 0 here would have promised an untimed lamp and
    // delivered a 60-second one. The device has no untimed state, and the
    // app must not invent one for it.
    auto& l = canary::care::lantern();
    l.configure(l.scene(), (uint16_t)v, l.auto_mode());
    canary::care::lantern_prefs_changed();
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

// POST /api/tz?v=EST5EDT,M3.2.0,M11.1.0 — the wall clock's zone.
//
// Separate from /api/set because that endpoint's contract is one INTEGER
// knob per request, and a POSIX TZ rule is a string. It gets its own route
// rather than a string escape hatch in the integer parser.
//
// Why it exists at all: OTA release binaries are generic (runtime_config.h),
// so they ship the CD_TZ default, which is a real zone (America/New_York)
// rather than UTC — right for one coast and wrong for everywhere else, so
// this route is how everywhere else fixes it. Before it existed, a household
// whose
// display showed the wrong hour had exactly one remedy — rebuild the
// firmware with its zone compiled in — which is not a thing a household
// does. The zone lands in the same NVS the learner writes, so it outlives
// reboots and future updates.
void handle_tz_set() {
  const String v = s_server->arg("v");
  // A POSIX TZ rule is printable ASCII and nothing else: letters, digits, and
  // + - : , . / < >. Refuse anything outside that rather than sanitize it,
  // because a "cleaned" zone is not the zone anyone meant. Control bytes are
  // the case that matters — this value is persisted to NVS and read back on
  // every boot, so a smuggled newline would be a durable break, not a
  // transient one. (The reader escapes it too; this is the other half.)
  bool printable = v.length() > 0;
  for (unsigned i = 0; printable && i < v.length(); i++) {
    const unsigned char c = (unsigned char)v[i];
    if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') printable = false;
  }
  if (!printable || !canary::net::tz_set_manual(v.c_str())) {
    s_server->send(400, "application/json", "{\"ok\":false}");
    return;
  }
  canary::log_line("TZ", "Timezone set by hand from the web page.");
  s_server->send(200, "application/json", "{\"ok\":true}");
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
  // EVERY display self-reports as WHAT IT IS — the same DEVICE_TYPE its mDNS
  // TXT already carries — not as the family string "canary-display".
  //
  // Only the nightlight used to do this, and the flattening cost the rest of
  // the line their picture. "canary-display" is deliberately unmapped in the
  // figure ledger (four different products wear that one type, so a figure
  // chosen from it would be a coin flip), so a Nightstand or a Dash answering
  // with the family string could never resolve to anything and drew the
  // generic marker on every surface — while the ledger held a drawing of it
  // the whole time. Publishing the real type also lets the apps offer the
  // screen controls this glass actually serves.
  //
  // A reader that has never heard of this particular type still degrades to
  // the coarse family (the apps fold the display line themselves), so an
  // older app meets a newer flavor and sees a display, not nothing.
  self.product      = DEVICE_TYPE;
  self.online       = 1;   // we are answering this request, so we are up
  self.chain_ok     = 0;   // a display holds no witness chain of its own
  self.chain_height = -1;  // omit chain_height
  // And the board, which is exact about the SHAPE where the type cannot be:
  // the 7" glass is both a Dash 7 and a Nightstand 7, so its product names
  // one of two products while its board names the one thing to draw.
#ifdef CANARY_FIGURE_HARDWARE
  self.hardware     = CANARY_FIGURE_HARDWARE;
#endif
  // And where this display stands with its hub, so the owner can be TOLD they
  // need one instead of discovering it by noticing nothing works. A display
  // with a placeholder broker is not broken and not misconfigured — it is a
  // device nobody has pointed at a hub yet, which is a different sentence and
  // a different fix from a hub that is simply down.
  self.hub          = canary::net::mqtt_broker_is_placeholder() ? FSR_HUB_NONE
                      : canary::net::mqtt_connected()           ? FSR_HUB_OK
                                                                : FSR_HUB_DOWN;
  // Sized by the shared macro for the worst case: device_id[48] of
  // all-escaping bytes expands 6x and is written twice (kernel + name) — a
  // smaller fixed buffer would truncate an accepted name into invalid JSON
  // served with a 200 (Codex P2 on #1226).
  //
  // The product bound is 24, not the 16 it used to be. 16 was already under
  // the truth — the nightlight has answered "canary-nightlight" (17) since it
  // started reporting its real type — and now every flavor reports one, the
  // longest being "canary-nightstand7" (18). The macro's promise is that
  // truncation is impossible BY CONSTRUCTION, and a bound smaller than the
  // string it bounds only kept that promise by accident of headroom.
  char body[FLEET_SELFREPORT_BODY_CAP(sizeof(cfg.device_id), 24)];
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
  s_server->on("/api/tz", HTTP_POST, handle_tz_set);
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
