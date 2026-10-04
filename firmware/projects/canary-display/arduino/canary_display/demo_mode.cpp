// Demo mode runtime (see include/canary/mode/demo_mode.h). The whole TU is
// empty without FEATURE_DEMO_MODE, so default builds stay byte-identical.

#include "canary/config.h"

#if defined(FEATURE_DEMO_MODE) && FEATURE_DEMO_MODE

#include <Arduino.h>
#include <lvgl.h>

#include "canary/mode/demo_mode.h"
#include "canary/mode/demo_script.h"
#include "canary/mode/mode_glue.h"
#include "canary/fleet/fleet_instance.h"
#include "canary/glass_settings.h"
#include "canary/hal/display.h"
#include "canary/ui/lvgl_port.h"
#include "canary/ui/character.h"
#include "canary/care/bird_glue.h"
#ifdef CD_FLAVOR_WATCH
#include "canary/ui/glance_ui.h"
#endif
#ifdef CD_FLAVOR_DASH
#include "canary/ui/dash_ui.h"
#endif

#include "boot/boot_banner.h"
#include "canary/version.h"

namespace canary {
namespace mode {

namespace {

constexpr uint32_t UI_EVERY_MS = 200;  // matches the fleet face's dash tick

bool     s_display_ok = false;
uint16_t s_prev_loop_s = 0;  // loop-local seconds, last processed position
bool     s_touch_down = false;
bool     s_longpress_fired = false;
uint32_t s_touch_down_ms = 0;
int16_t  s_tx = 0, s_ty = 0;
uint32_t s_last_ui_ms = 0;

// The hold-to-ack ring is part of the story: same sweep, same timing as
// the fleet face (MOTION_ACK_MS == CD_LONGPRESS_MS).
void ui_ack_hold(bool active) {
  if (!s_display_ok) return;
#ifdef CD_FLAVOR_WATCH
  canary::ui::glance_ui_ack_hold(active);
#endif
#ifdef CD_FLAVOR_DASH
  canary::ui::dash_ui_ack_hold(active);
#endif
}

#ifdef CD_FLAVOR_WATCH
int s_page = 0;
#endif

// The honesty rail: an unmissable chip on LVGL's top layer, above whatever
// face is showing — a demo unit can never pass for a live fleet.
void make_demo_chip() {
  lv_obj_t* chip = lv_label_create(lv_layer_top());
  lv_obj_set_style_text_font(chip, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(chip, lv_color_hex(0x111111), 0);
  lv_obj_set_style_bg_color(chip, lv_color_hex(0xFFB300), 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(chip, 8, 0);
  lv_obj_set_style_pad_ver(chip, 3, 0);
  lv_obj_set_style_radius(chip, 9, 0);
#ifdef CD_FLAVOR_WATCH
  lv_label_set_text(chip, "DEMO");  // 240 px round glass: the word alone
  lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, 6);
#else
  lv_label_set_text(chip, "DEMO  •  hold 3s to exit");
  lv_obj_align(chip, LV_ALIGN_BOTTOM_RIGHT, -10, -8);
#endif
}

// Seed the cast: names, rooms, liveness, battery — through the same ingest
// calls the MQTT dispatcher uses. Badges stay Unknown/Unsigned on purpose:
// synthetic events genuinely carry no signature, and the verified ✓ is
// never faked, even here (especially here).
void seed_cast(uint32_t now) {
  auto& fleet = canary::fleet::the_fleet();
  for (uint8_t i = 0; i < DEMO_CAST_COUNT; i++) {
    const DemoWitness& w = DEMO_CAST[i];
    fleet.on_status(w.id, "canary", /*online=*/true, w.battery, now);
    fleet.on_meta(w.id, w.name, w.room, now);
    fleet.on_health(w.id, w.battery, /*battery_present=*/true,
                    CANARY_FW_VERSION, now);
    // Dress the detail lines the way a lived-in fleet looks: believable
    // signal strengths, and room comfort on the indoor pair (the care
    // wave's comfort words render from exactly this ingest).
    fleet.on_rssi(w.id, -48 - (int)i * 9, now);
  }
  fleet.on_comfort(DEMO_CAST[1].id, 215, true, 44, now);  // kitchen 21.5 C
  fleet.on_comfort(DEMO_CAST[3].id, 226, true, 47, now);  // nursery 22.6 C
  fleet.mark_dirty();
}

// Walk the storyline: fire every beat in (prev, now] on the looping clock.
// Tamper beats also raise the witness tamper flag (that chip is fed by the
// tamper topic on the real wire), and "cleared" lowers it again.
void step_story(uint32_t now) {
  const uint16_t now_s = (uint16_t)((now / 1000UL) % DEMO_LOOP_S);
  if (now_s == s_prev_loop_s) return;
  uint16_t fired[DEMO_BEAT_COUNT];
  const uint8_t n =
      demo_beats_between(s_prev_loop_s, now_s, fired, DEMO_BEAT_COUNT);
  s_prev_loop_s = now_s;
  if (n == 0) return;
  auto& fleet = canary::fleet::the_fleet();
  for (uint8_t i = 0; i < n; i++) {
    const DemoBeat& b = DEMO_BEATS[fired[i]];
    const char* id = DEMO_CAST[b.witness].id;
    if (b.intent == canary::fleet::Sev::Tamper) {
      fleet.on_tamper(id, /*active=*/true, "contact", now);
    } else if (strcmp(b.event, "cleared") == 0) {
      fleet.on_tamper(id, /*active=*/false, "", now);
    }
    fleet.on_event(id, b.event, /*signed_flag=*/false, now);
    Serial.printf("DM1 %lu EVT %s %s\r\n", (unsigned long)now, id, b.event);
  }
}

void render(uint32_t now) {
  if (!s_display_ok) return;
  auto& fleet = canary::fleet::the_fleet();
  // The mood engine is part of the show — the living canary reacts to the
  // storyline exactly as it would to a real day (no valid clock in demo:
  // pass yday-unknown, day mode).
  const canary::ui::CanaryMood bird =
      canary::care::bird_mood_tick(now, /*night=*/false,
                                   /*yday_valid=*/false, -1);
#ifdef CD_FLAVOR_WATCH
  canary::ui::GlanceState st;
  st.page = s_page;
  st.night = false;
  // The face renders the healthy look — the links it would report on don't
  // exist in this gear, and the DEMO chip owns that honesty.
  st.wifi_ok = true;
  st.mqtt_ok = true;
  st.acked = fleet.ack_active(now);
  st.time_valid = false;
  st.bird = bird;
  canary::ui::glance_ui_update(fleet, now, st);
#endif
#ifdef CD_FLAVOR_DASH
  canary::ui::DashState st;
  st.night = false;
  st.wifi_ok = true;
  st.mqtt_ok = true;
  st.acked = fleet.ack_active(now);
  st.time_valid = false;
  st.bird = bird;
  canary::ui::dash_ui_update(fleet, now, st);
#endif
}

}  // namespace

void demo_mode_setup() {
  boot_line("              .--------.");
  boot_line("              |  o  o  |        DEMO MODE (scripted fleet)");
  boot_line("              '--------'        no WiFi - no MQTT - no OTA");
  boot_separator();
  boot_kvf("Story", "%u beats over %u s, looping",
           (unsigned)DEMO_BEAT_COUNT, (unsigned)DEMO_LOOP_S);
  boot_kv("Exit",  "hold the glass 3 s");
  boot_blank();

  // The saved Character dresses the demo too — the show should look like
  // the unit the household actually configured.
  canary::glass::settings_init();
  canary::ui::character_apply(
      (canary::ui::Character)canary::glass::settings().character);
  canary::ui::character_set_night(false);

  s_display_ok = canary::hal::display_init();
  if (s_display_ok) s_display_ok = canary::ui::lvgl_port_init();

  const uint32_t now = millis();
  seed_cast(now);
  s_prev_loop_s = (uint16_t)((now / 1000UL) % DEMO_LOOP_S);

  if (s_display_ok) {
#ifdef CD_FLAVOR_WATCH
    canary::ui::glance_ui_create();
#endif
#ifdef CD_FLAVOR_DASH
    canary::ui::dash_ui_create();
#endif
    make_demo_chip();
    render(now);
    lv_timer_handler();
    canary::hal::backlight_set(255);  // showtime: glass always on
  }

  Serial.printf("DM1 HELLO fw=%s loop_s=%u\r\n", CANARY_FW_VERSION,
                (unsigned)DEMO_LOOP_S);
  boot_scene_ready(
      "Demo up. A scripted household is playing on the real faces;",
      "nothing is transmitted and nothing here is a real witness.",
      NULL);
}

void demo_mode_loop() {
  const uint32_t now = millis();
  auto& fleet = canary::fleet::the_fleet();

  // ── Touch: tap = face nav, long-press = acknowledge (with the ring
  //    sweeping closed exactly as the fleet face does), 3 s = exit ──
  const auto ts = canary::hal::touch_read();
  if (ts.touched && !s_touch_down) {
    s_touch_down = true;
    s_touch_down_ms = now;
    s_tx = ts.x;
    s_ty = ts.y;
    s_longpress_fired = false;
    ui_ack_hold(true);  // the sweep starts; a tap only ever shows a sliver
  } else if (ts.touched && s_touch_down && !s_longpress_fired &&
             (int32_t)(now - s_touch_down_ms) >= (int32_t)CD_LONGPRESS_MS) {
    // Fires DURING the hold, like the product: the ring closes and the
    // alarm beat quiets in the same instant.
    s_longpress_fired = true;
    ui_ack_hold(false);
    fleet.acknowledge_by(now, "demo");
  } else if (!ts.touched && s_touch_down) {
    s_touch_down = false;
    ui_ack_hold(false);
    const uint32_t held = now - s_touch_down_ms;
    if (held >= 3000) {
      mode_exit_to_fleet();  // does not return
    } else if (!s_longpress_fired && held < 600 && s_display_ok) {
#ifdef CD_FLAVOR_WATCH
      s_page = (s_page + 1) % canary::ui::glance_page_count();
      fleet.mark_dirty();
#endif
#ifdef CD_FLAVOR_DASH
      canary::ui::dash_ui_handle_tap(s_tx, s_ty);
#endif
    }
  }

  step_story(now);
  fleet.tick(now);

  if (s_display_ok &&
      (fleet.take_dirty() || (now - s_last_ui_ms) >= UI_EVERY_MS)) {
    s_last_ui_ms = now;
    render(now);
  }
  if (s_display_ok) lv_timer_handler();
  delay(5);
}

}  // namespace mode
}  // namespace canary

#endif  // FEATURE_DEMO_MODE
