// Debug mode runtime (see include/canary/mode/debug_mode.h). The whole TU is
// empty without FEATURE_DEBUG_MODE, so default builds stay byte-identical.

#include "config.h"

#if defined(FEATURE_DEBUG_MODE) && FEATURE_DEBUG_MODE

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <lvgl.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "pins.h"
#include "debug_mode.h"
#if defined(FEATURE_MIC_ALARM) && FEATURE_MIC_ALARM && \
    defined(HAS_MICROPHONE) && HAS_MICROPHONE
#include "mic_alarm.h"
#endif
#include "mode_glue.h"
#include "diagnostics.h"
#include "fleet_instance.h"
#include "display.h"
#include "mqtt_mgr.h"
#include "runtime_config.h"
#include "topics.h"
#include "lvgl_port.h"
#include "theme.h"
#include "version.h"

#include "boot_banner.h"

namespace canary {
namespace mode {

namespace {

// The bench's local Quiet-Glass palette (playground_ui.cpp precedent):
// diagnostics must render identically whatever Character is saved.
lv_color_t c_bg()    { return lv_color_hex(0x000000); }
lv_color_t c_text()  { return lv_color_hex(0xEDEDED); }
lv_color_t c_muted() { return lv_color_hex(0x9A9A9A); }
lv_color_t c_warn()  { return lv_color_hex(0xFFB300); }
lv_color_t c_ok()    { return lv_color_hex(0x4CAF50); }

#ifdef CD_FLAVOR_WATCH
constexpr int LINE_CAP = 9;
constexpr int LINE_X = 26, LINE_Y0 = 44, LINE_STEP = 20;
constexpr int NEXT_STRIP_H = 40;
#else
constexpr int LINE_CAP = 16;
constexpr int LINE_X = 14, LINE_Y0 = 48, LINE_STEP = 26;
constexpr int NEXT_STRIP_H = 40;
#endif

enum Page : uint8_t { PG_SYSTEM = 0, PG_FLEET, PG_EVENTS, PG_TOUCH, PG_BUS,
                      PG_COUNT };

const char* page_name(uint8_t p) {
  switch (p) {
    case PG_SYSTEM: return "system + link";
    case PG_FLEET:  return "fleet, raw";
    case PG_EVENTS: return "events";
    case PG_TOUCH:  return "touch test";
    case PG_BUS:    return "i2c bus";
  }
  return "?";
}

bool      s_display_ok = false;
uint8_t   s_page = PG_SYSTEM;
lv_obj_t* s_title = nullptr;
lv_obj_t* s_lines[LINE_CAP] = {nullptr};
lv_obj_t* s_touch_dot = nullptr;

Topics    s_topics;
bool      s_mqtt_inited = false;
uint32_t  s_mqtt_next_ms = 0;
uint32_t  s_hb_next_ms = 0;

bool     s_touch_down = false;
uint32_t s_touch_down_ms = 0;
int16_t  s_tx = 0, s_ty = 0;

uint32_t s_last_ui_ms = 0;
uint32_t s_last_snap_ms = 0;
uint32_t s_last_scan_ms = 0;
uint8_t  s_bus_addrs[16] = {0};
uint8_t  s_bus_count = 0;

const char* i2c_name(uint8_t a) {
  switch (a) {
    case 0x10: return "VEML7700";
    case 0x14: case 0x5D: return "GT911";
    case 0x15: return "CST816S";
    case 0x23: case 0x24: case 0x26: case 0x38: return "CH422G";
    case 0x29: return "VL53L0X";
    case 0x51: return "PCF8563";
    case 0x5A: return "MPR121";
    default:   return "";
  }
}

const char* reset_name(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_SW:       return "sw";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int-wdt";
    case ESP_RST_TASK_WDT: return "task-wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP:return "deepsleep";
    default:               return "other";
  }
}

void set_line(int i, const char* txt, lv_color_t col) {
  if (i < 0 || i >= LINE_CAP || !s_lines[i]) return;
  lv_obj_set_style_text_color(s_lines[i], col, 0);
  lv_label_set_text(s_lines[i], txt);
}

void clear_lines(int from) {
  for (int i = from; i < LINE_CAP; i++) {
    if (s_lines[i]) lv_label_set_text(s_lines[i], "");
  }
}

void build_face() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, c_bg(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clean(scr);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  s_title = lv_label_create(scr);
  lv_obj_set_style_text_font(s_title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_title, c_warn(), 0);
#ifdef CD_FLAVOR_WATCH
  lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 14);
#else
  lv_obj_set_pos(s_title, LINE_X, 12);
#endif

  for (int i = 0; i < LINE_CAP; i++) {
    s_lines[i] = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lines[i], &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lines[i], c_text(), 0);
    lv_obj_set_pos(s_lines[i], LINE_X, LINE_Y0 + i * LINE_STEP);
    lv_label_set_text(s_lines[i], "");
  }

  // Touch-test marker: parked off-screen until the touch page shows it.
  s_touch_dot = lv_obj_create(scr);
  lv_obj_set_size(s_touch_dot, 18, 18);
  lv_obj_set_style_radius(s_touch_dot, 9, 0);
  lv_obj_set_style_bg_color(s_touch_dot, c_warn(), 0);
  lv_obj_set_style_bg_opa(s_touch_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_touch_dot, 0, 0);
  lv_obj_add_flag(s_touch_dot, LV_OBJ_FLAG_HIDDEN);
}

void title_update() {
  if (!s_title) return;
  lv_label_set_text_fmt(s_title, "DEBUG %d/%d  •  %s  •  tap top = next",
                        s_page + 1, PG_COUNT, page_name(s_page));
}

void i2c_scan() {
  s_bus_count = 0;
  for (uint8_t a = 1; a < 127 && s_bus_count < sizeof(s_bus_addrs); a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) s_bus_addrs[s_bus_count++] = a;
  }
}

void page_update(uint32_t now) {
  char b[96];
  int i = 0;
  const auto& fleet = canary::fleet::the_fleet();
  const auto& d = canary::diag::get();

  if (s_touch_dot) {
    if (s_page == PG_TOUCH) lv_obj_clear_flag(s_touch_dot, LV_OBJ_FLAG_HIDDEN);
    else                    lv_obj_add_flag(s_touch_dot, LV_OBJ_FLAG_HIDDEN);
  }

  switch (s_page) {
    case PG_SYSTEM: {
      snprintf(b, sizeof(b), "fw %s  •  %s", CANARY_FW_VERSION, BOARD_NAME);
      set_line(i++, b, c_text());
      const esp_partition_t* part = esp_ota_get_running_partition();
      snprintf(b, sizeof(b), "uptime %lus  reset %s  slot %s",
               (unsigned long)(now / 1000UL), reset_name(esp_reset_reason()),
               part ? part->label : "?");
      set_line(i++, b, c_muted());
      snprintf(b, sizeof(b), "heap %luk  min %luk  block %luk  (%s)",
               (unsigned long)(d.free_heap / 1024),
               (unsigned long)(d.min_heap / 1024),
               (unsigned long)(d.largest_block / 1024),
               canary::diag::level_name(d.level));
      set_line(i++, b, d.level == canary::diag::Level::Normal ? c_ok()
                                                              : c_warn());
      const bool up = WiFi.status() == WL_CONNECTED;
      // Support-by-photo needs the SSID; the password never renders.
      snprintf(b, sizeof(b), "wifi %s  \"%s\"", up ? "up" : "down",
               canary::cfg::get().wifi_ssid);
      set_line(i++, b, up ? c_ok() : c_warn());
      if (up) {
        snprintf(b, sizeof(b), "ip %s  rssi %d dBm  ch %d",
                 WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(),
                 (int)WiFi.channel());
        set_line(i++, b, c_muted());
      }
      const bool broker = canary::net::mqtt_connected();
      snprintf(b, sizeof(b), "broker %s  %s:%u", broker ? "up" : "down",
               canary::net::mqtt_broker_host(),
               (unsigned)canary::net::mqtt_broker_port());
      set_line(i++, b, broker ? c_ok() : c_warn());
      snprintf(b, sizeof(b), "device  %s", canary::cfg::get().device_id);
      set_line(i++, b, c_muted());
#if defined(FEATURE_MIC_ALARM) && FEATURE_MIC_ALARM && \
    defined(HAS_MICROPHONE) && HAS_MICROPHONE
      // The always-know contract, restated where diagnosticians look.
      snprintf(b, sizeof(b), "mic  %s  rms %u",
               !canary::io::mic_pins_ok() ? "pins unset (VERIFY)"
               : canary::io::mic_listening() ? "LISTENING" : "off",
               (unsigned)canary::io::mic_level());
      set_line(i++, b, canary::io::mic_listening() ? c_warn() : c_muted());
#endif
      set_line(i++, "hold the glass 3 s to exit debug", c_muted());
      break;
    }
    case PG_FLEET: {
      snprintf(b, sizeof(b), "%d witnesses  •  worst %s%s", fleet.count(),
               canary::fleet::sev_name(fleet.worst(now)),
               fleet.ack_active(now) ? "  •  acked" : "");
      set_line(i++, b, c_text());
      for (int w = 0; w < fleet.count() && i < LINE_CAP; w++) {
        const auto* wit = fleet.at(w);
        if (!wit) break;
        const uint32_t age_s = (now - wit->last_seen_ms) / 1000UL;
        snprintf(b, sizeof(b), "%.14s %s %lus %s batt%d",
                 canary::fleet::Fleet::display_name(*wit),
                 canary::ui::link_label(wit->link), (unsigned long)age_s,
                 canary::ui::badge_text(wit->badge), (int)wit->battery_pct);
        set_line(i++, b, wit->link == canary::fleet::Link::Online ? c_text()
                                                                  : c_warn());
      }
      break;
    }
    case PG_EVENTS: {
      const int total = fleet.events_count();
      snprintf(b, sizeof(b), "%d events (newest first)", total);
      set_line(i++, b, c_text());
      for (int e = 0; e < total && i < LINE_CAP; e++) {
        const auto* row = fleet.event_at(e);
        if (!row) break;
        snprintf(b, sizeof(b), "-%lus %.10s %.24s [%s]%s",
                 (unsigned long)((now - row->at_ms) / 1000UL), row->device,
                 row->name, canary::fleet::sev_name(row->sev),
                 row->signed_flag ? " s" : "");
        set_line(i++, b, c_muted());
      }
      break;
    }
    case PG_TOUCH: {
      set_line(i++, "tap anywhere: the dot lands where the panel", c_muted());
      set_line(i++, "says your finger is. offset = calibration.", c_muted());
      snprintf(b, sizeof(b), "last  x=%d  y=%d", (int)s_tx, (int)s_ty);
      set_line(i++, b, c_text());
      break;
    }
    case PG_BUS: {
      snprintf(b, sizeof(b), "%u devices ACK on the shared bus",
               (unsigned)s_bus_count);
      set_line(i++, b, c_text());
      for (uint8_t a = 0; a < s_bus_count && i < LINE_CAP; a++) {
        snprintf(b, sizeof(b), "0x%02X  %s", s_bus_addrs[a],
                 i2c_name(s_bus_addrs[a]));
        set_line(i++, b, c_muted());
      }
      break;
    }
  }
  clear_lines(i);
  title_update();
}

void snapshot(uint32_t now) {
  const auto& d = canary::diag::get();
  Serial.printf(
      "DBG1 %lu SNAP heap=%lu min=%lu block=%lu wifi=%d rssi=%d mqtt=%d "
      "witnesses=%d worst=%s\r\n",
      (unsigned long)now, (unsigned long)d.free_heap,
      (unsigned long)d.min_heap, (unsigned long)d.largest_block,
      WiFi.status() == WL_CONNECTED ? 1 : 0, (int)WiFi.RSSI(),
      canary::net::mqtt_connected() ? 1 : 0,
      canary::fleet::the_fleet().count(),
      canary::fleet::sev_name(canary::fleet::the_fleet().worst(now)));
}

}  // namespace

void debug_mode_setup() {
  boot_line("              .--------.");
  boot_line("              |  ?  ?  |        DEBUG MODE (diagnostics)");
  boot_line("              '--------'        network UP - no OTA - read-mostly");
  boot_separator();
  boot_kv("Pages", "system+link / fleet / events / touch / i2c");
  boot_kv("Exit",  "hold the glass 3 s");
  boot_blank();

  s_display_ok = canary::hal::display_init();
  if (s_display_ok) s_display_ok = canary::ui::lvgl_port_init();
  if (s_display_ok) {
    build_face();
    page_update(millis());
    lv_timer_handler();
    canary::hal::backlight_set(255);  // a diagnosis session never dims
  }

  // Non-blocking WiFi: a dead AP is a FINDING this mode exists to show,
  // never a reboot loop — so no wifi_init_or_reboot() here.
  WiFi.mode(WIFI_STA);
  WiFi.begin(canary::cfg::get().wifi_ssid, canary::cfg::get().wifi_pass);

  s_topics = build_topics(canary::cfg::get().device_id);
  canary::net::mqtt_init(s_topics);
  s_mqtt_inited = true;

  Serial.printf("DBG1 HELLO fw=%s board=%s\r\n", CANARY_FW_VERSION, BOARD_ID);
  boot_scene_ready(
      "Debug up. Every page is self-labeled - a photo of the glass",
      "is a support report. DBG1 snapshots on this console, 1 Hz.",
      NULL);
}

void debug_mode_loop() {
  const uint32_t now = millis();
  canary::diag::loop(now);
  canary::fleet::the_fleet().tick(now);

  // ── Touch: top strip = next page, body = touch test, 3 s hold = exit ──
  const auto ts = canary::hal::touch_read();
  if (ts.touched && !s_touch_down) {
    s_touch_down = true;
    s_touch_down_ms = now;
  }
  if (ts.touched) {
    s_tx = ts.x;
    s_ty = ts.y;
    if (s_page == PG_TOUCH && s_display_ok && s_touch_dot) {
      lv_obj_clear_flag(s_touch_dot, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(s_touch_dot, s_tx - 9, s_ty - 9);
    }
  } else if (s_touch_down) {
    s_touch_down = false;
    const uint32_t held = now - s_touch_down_ms;
    if (held >= 3000) {
      mode_exit_to_fleet();  // does not return
    } else if (held < 600) {
      if (s_ty < NEXT_STRIP_H || s_page != PG_TOUCH) {
        s_page = (uint8_t)((s_page + 1) % PG_COUNT);
        if (s_page == PG_BUS) s_last_scan_ms = 0;  // fresh census on entry
      }
      if (s_display_ok) page_update(now);
    }
  }

  // ── Link supervision: bounded broker attempts on a fixed cadence ──
  if (WiFi.status() == WL_CONNECTED) {
    if (canary::net::mqtt_connected()) {
      canary::net::mqtt_loop();
      if ((int32_t)(now - s_hb_next_ms) >= 0) {
        s_hb_next_ms = now + HEARTBEAT_MS;
        canary::net::publish_status_retained(s_topics, "online");
      }
    } else if ((int32_t)(now - s_mqtt_next_ms) >= 0) {
      s_mqtt_next_ms = now + 5000;
      canary::net::mqtt_connect_attempt();
    }
  }

  // ── Periodic work: census (bus page only), serial snapshot, redraw ──
  if (s_page == PG_BUS && (s_last_scan_ms == 0 || now - s_last_scan_ms >= 3000)) {
    s_last_scan_ms = now;
    i2c_scan();
  }
  if (now - s_last_snap_ms >= 1000) {
    s_last_snap_ms = now;
    snapshot(now);
  }
  if (s_display_ok && now - s_last_ui_ms >= 500) {
    s_last_ui_ms = now;
    page_update(now);
  }
  if (s_display_ok) lv_timer_handler();
  delay(5);
}

}  // namespace mode
}  // namespace canary

#endif  // FEATURE_DEBUG_MODE
