/*
  SecuraCV Canary Display — Fleet Status Display Firmware (v0.1)
  --------------------------------------------------------------
  (c) 2026 Errer Labs / SecuraCV
  errerlabs.com | securacv.com
  GitHub: https://github.com/kmay89/securaCV

  License: Apache-2.0 (use repository license unless otherwise specified).

  "A Canary that shows instead of senses." Two flavors of the same app:

    watch  XIAO ESP32-S3 + Seeed Round Display — bedside/desk glance puck
    dash   Waveshare ESP32-S3-Touch-LCD-4.3   — front-door/kitchen dashboard

  The display subscribes to the fleet's MQTT topics
  (securacv/+/{status,availability,health,events,tamper,chain,state}),
  keeps a fleet model, TOFU-pins each witness pubkey from its retained
  health payload, verifies signed chain heads with on-device Ed25519, and
  renders the result — timely and relevant, no phone in the loop.

  Honesty rules this firmware enforces on itself (display_ux_design.md):
    - "Verified" appears only after a real Ed25519 verify against the pin.
    - Link loss is a first-class alarm (baby-monitor semantics): a silent
      witness goes amber then red on deadlines, and a dead WiFi/broker link
      is banner-visible — silence is never rendered as safety.
    - Acknowledge (long-press) quiets emphasis but leaves a residual chip
      until the underlying condition clears (Nest pattern). Tamper cannot
      be dismissed, only quieted.
    - It witnesses nothing itself: no camera, no microphone, no event
      publishing — its own MQTT voice is a liveness heartbeat + OTA entity.

  ⚠️ DEV STATUS: compile/CI-verified; NOT yet validated on bench hardware
     (matching the enclosures' v0.1-dev status). Pin maps carry VERIFY
     notes where vendor docs are thin.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

// Board pin map (boards/<board>/pins via -I). Pin numbers ONLY there.
#include "pins.h"

// Project composition header: flavor config (CD_*) + net/OTA/diag constants.
#include "canary/config.h"
// FEATURE_* vs HAS_* compile-time cross-check (needs pins.h + config above).
#include "core/feature_sanity.h"
#include "canary/version.h"
#include "canary/log.h"
#include "canary/topics.h"
#include "canary/runtime_config.h"
#include "canary/diagnostics.h"
#include "canary/trust.h"
#include "canary/fleet/fleet_instance.h"
#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
#include "canary/fleet/journal_instance.h"
#endif
#include "canary/net/wifi_mgr.h"
#include "canary/net/tz_auto.h"
#include "canary/net/glass_web.h"
#include "canary/net/mqtt_mgr.h"
#include "canary/net/ota_mgr.h"
#if defined(FEATURE_MDNS_DISCOVERY) && FEATURE_MDNS_DISCOVERY
#include "canary/net/discovery.h"
#endif
#if defined(FEATURE_CHIRP_SCAN) && FEATURE_CHIRP_SCAN
#include "canary/net/chirp_scan.h"
#endif
#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
#include "canary/io/field_io.h"   // 4.3B isolated DI/DO -> events + siren
#endif
#if defined(FEATURE_MIC_ALARM) && FEATURE_MIC_ALARM && \
    defined(HAS_MICROPHONE) && HAS_MICROPHONE
#include "canary/io/mic_alarm.h"  // 4.3C: acoustic alarm patterns -> events
#endif
#if defined(FEATURE_RTC) && FEATURE_RTC
#include "canary/io/rtc.h"        // PCF8563 trusted time (probe -> seed/mirror)
#endif
#if defined(FEATURE_ESPNOW) && FEATURE_ESPNOW
#include "canary/net/espnow_peer.h"  // router-independent peer presence (rx-only)
#endif
#if defined(FEATURE_FLEET_LINK) && FEATURE_FLEET_LINK
#include "canary/net/fleet_link.h"
#endif
#if defined(FEATURE_ONBOARDING) && FEATURE_ONBOARDING
#include "canary/net/provision.h"
#endif
#if defined(FEATURE_CARE) && FEATURE_CARE
#include "canary/care/care_glue.h"
#include "canary/fleet/mute_store.h"
#endif
#if defined(FEATURE_WAKE_ALARM) && FEATURE_WAKE_ALARM
#include "canary/care/wake_glue.h"
#endif
#include "canary/care/bird_glue.h"
#include "canary/hal/display.h"
#include "canary/hal/chime.h"
#include "canary/hal/core_compat.h"
#include "canary/glass_settings.h"
#if ((defined(FEATURE_PLAYGROUND) && FEATURE_PLAYGROUND) ||   \
     (defined(FEATURE_DEVMODE) && FEATURE_DEVMODE) ||         \
     (defined(FEATURE_DEMO_MODE) && FEATURE_DEMO_MODE) ||     \
     (defined(FEATURE_DEBUG_MODE) && FEATURE_DEBUG_MODE) ||   \
     (defined(FEATURE_ARCADE) && FEATURE_ARCADE))
// Mode system (docs/hardware/display_modes.md): this build carries at least
// one non-fleet gear (bench / demo / debug / arcade). Which gear a boot
// lands in is decided ONCE by the host-tested registry (NVS token, legacy
// devmode migration, fail-safe to Fleet); the resolved gear is cached here
// so loop() never re-reads NVS.
#define CD_MODES_COMPILED 1
#include "canary/mode/mode_glue.h"
static canary::mode::Mode s_active_mode = canary::mode::Mode::Fleet;
#endif

#include <lvgl.h>
#include "canary/ui/lvgl_port.h"
#include "canary/ui/character.h"
#include "canary/ui/splash.h"
#include "canary/ui/settings_ui.h"
#include "canary/ui/commission_ui.h"
#ifdef CD_FLAVOR_WATCH
#include "canary/ui/glance_ui.h"
#endif
#ifdef CD_FLAVOR_DASH
#include "canary/ui/dash_ui.h"
#endif
#ifdef CD_FLAVOR_NIGHTSTAND
#include "canary/ui/portrait_ui.h"
#endif
#if defined(FEATURE_AMBIENT_LED) && FEATURE_AMBIENT_LED
#include "canary/hal/ambient_led.h"  // WS2812 across-room state beacon
#endif

#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
#include <esp_task_wdt.h>
#endif

// Shared, board-agnostic modules (reached via -I .../common).
#include "boot/boot_banner.h"
#include "identity/device_pseudonym.h"  // salted, MAC-free device handle (Invariant III)

// LVGL renders from loop(), and Arduino's default 8 KiB loopTask stack is
// not enough for LVGL 9's renderer (its layout/draw recursion overflowed
// the stack canary on the dash's first bench boot — nested flex containers
// go several frames deeper per level than the 8.4 renderer did). 24 KiB
// buys the deepest face (dash) comfortable margin; the macro has existed
// since arduino-esp32 2.0.6, so both supported core lines honor it.
#ifdef SET_LOOP_TASK_STACK_SIZE
SET_LOOP_TASK_STACK_SIZE(24 * 1024);
#endif

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------

static Topics TOPICS;

static bool     g_display_ok = false;
static uint32_t g_last_heartbeat_ms = 0;
static uint32_t g_last_health_ms = 0;
static uint32_t g_last_render_ms = 0;
static uint32_t g_last_diag_ms = 0;

// Touch gesture tracking (tap vs long-press). The hold-to-ack ring starts
// the moment a finger lands: a quick tap flashes a sliver of arc — a quiet
// hint that holding does more — and a full hold sweeps it closed exactly
// when the ack fires (MOTION_ACK_MS == CD_LONGPRESS_MS).
static bool     g_touch_down = false;
static uint32_t g_touch_down_ms = 0;
static bool     g_longpress_fired = false;
static int16_t  g_touch_x = 0;      // press coordinates (dash tap routing)
static int16_t  g_touch_y = 0;

// Wake window: touch — or a fresh Notice+ fleet event (presence-wake,
// spec §3) — promotes the illumination ladder to Active for a while.
static uint32_t g_wake_until_ms = 0;
static uint32_t g_last_wake_event_ms = 0;

#ifdef CD_FLAVOR_WATCH
static int      g_page = 0;
static uint32_t g_page_touched_ms = 0;   // auto-return to overview
#endif

// Broker reconnect schedule (mirrors the WiFi supervisor's backoff): a broker
// outage must never pin the loop — the display keeps rendering last-known
// state (clearly bannered) and each bounded connect attempt happens at most
// once per backoff window.
static uint32_t g_mqtt_next_attempt_ms = 0;
static uint32_t g_mqtt_attempts = 0;

// Broker-outage clock for the fleet-rediscovery rebind (wifi up, broker
// dark). Zero = link healthy or wifi down.
static uint32_t g_mqtt_down_since_ms = 0;

static bool mqtt_supervise(uint32_t now) {
  if (canary::net::mqtt_connected()) {
    g_mqtt_attempts = 0;
    g_mqtt_down_since_ms = 0;
    return true;
  }
  if (!canary::net::wifi_connected()) {
    g_mqtt_down_since_ms = 0;  // wifi_loop owns this outage
    return false;
  }
  if (g_mqtt_down_since_ms == 0) {
    g_mqtt_down_since_ms = now;
#if defined(FEATURE_MDNS_DISCOVERY) && FEATURE_MDNS_DISCOVERY
    // Retract our referral the moment the link drops: gossip is ground
    // truth in both directions, or a dead/moved broker keeps re-seeding
    // every rediscovery on the LAN (review catch).
    canary::net::discovery_clear_broker();
#endif
  }
  if ((int32_t)(now - g_mqtt_next_attempt_ms) < 0) return false;

  if (canary::net::mqtt_connect_attempt()) {
    g_mqtt_attempts = 0;
    g_mqtt_down_since_ms = 0;
    canary::net::publish_health_retained(TOPICS);
    g_last_health_ms = now;
#if defined(FEATURE_MDNS_DISCOVERY) && FEATURE_MDNS_DISCOVERY
    // Gossip only ground truth: we are connected to this broker right now,
    // so the next device to join the WiFi can just ask the fleet.
    canary::net::discovery_advertise_broker(canary::net::mqtt_broker_host(),
                                            canary::net::mqtt_broker_port());
#endif
    canary::fleet::the_fleet().mark_dirty();
    return true;
  }

#if defined(FEATURE_MDNS_DISCOVERY) && FEATURE_MDNS_DISCOVERY
  // Self-healing rebind: WiFi is fine but the broker has been dark past
  // the deadline — the classic cause is the broker host taking a new DHCP
  // lease. Ask the fleet (bounded ~3-6 s mDNS query; the link is already
  // down, so the stall costs nothing), adopt any referral, and let the
  // normal backoff reconnect. Re-asks once per deadline window.
  if ((int32_t)(now - g_mqtt_down_since_ms) >= (int32_t)BROKER_REDISCOVER_MS) {
    g_mqtt_down_since_ms = now;
    char host[64];
    uint16_t port = 1883;
    if (canary::net::discovery_find_broker(host, sizeof(host), &port)) {
      if (strcmp(host, canary::net::mqtt_broker_host()) != 0 ||
          port != canary::net::mqtt_broker_port()) {
        canary::net::mqtt_set_broker(host, port);
        g_mqtt_next_attempt_ms = now;  // try the referral immediately
        g_mqtt_attempts = 0;
      }
    }
  }
#endif

  // Exponential backoff: 2 s -> 4 s -> 8 s -> 16 s -> 30 s cap.
  uint32_t attempt = g_mqtt_attempts;
  if (attempt > 4) attempt = 4;
  uint32_t backoff_ms = 2000UL << attempt;
  if (backoff_ms > 30000UL) backoff_ms = 30000UL;
  g_mqtt_attempts++;
  g_mqtt_next_attempt_ms = now + backoff_ms;
  return false;
}

// ----------------------------------------------------------------------------
// Clock / quiet hours (SNTP; the watch's PCF8563 RTC is a follow-up)
// ----------------------------------------------------------------------------

static bool local_time(int* hh, int* mm, int* yday = nullptr) {
  time_t t = time(nullptr);
  if (t < 1700000000) return false;  // clock not synced yet
  struct tm lt;
  localtime_r(&t, &lt);
  if (hh) *hh = lt.tm_hour;
  if (mm) *mm = lt.tm_min;
  if (yday) *yday = lt.tm_yday;
  return true;
}

static bool in_quiet_hours() {
  int hh = 0;
  if (!local_time(&hh, nullptr)) return false;  // unknown time = day mode
  // Runtime schedule (settings wave); CD_QUIET_* are the first-boot seeds.
  const auto& gs = canary::glass::settings();
  return canary::glass::hours_in_window(hh, gs.night_start_hh, gs.night_end_hh);
}

// Wake-window length: the day window is generous; a night peek is short and
// user-tunable (settings wave, the Hatch tap-to-peek pattern).
static uint32_t wake_window_ms(bool night) {
  if (!night) return CD_TOUCH_WAKE_MS;
  return (uint32_t)canary::glass::settings().peek_s * 1000UL;
}

// ----------------------------------------------------------------------------
// Brightness policy
// ----------------------------------------------------------------------------
//
// Day: full. Quiet hours: near-dark floor (watch, PWM) / off (dash, binary
// backlight) — EXCEPT that an unacked Alert/Tamper overrides the night
// floor: the one thing a bedside security glance must never do is sleep
// through a tamper. Touch opens a full-brightness wake window.

// Illumination ladder (spec §3): Active (touch / fresh event / unacked
// alert) > Ambient (idle daytime) > Night (quiet hours floor). G5 stands:
// nothing but an unacked Alert/Tamper ever overrides the Night floor.
static void apply_brightness(uint32_t now, bool night) {
  using canary::fleet::Sev;
  namespace glass = canary::glass;
  // A live brightness editor / black-point wizard IS the brightness policy
  // while it's open (The Screen Is the Preview) — stand down until it exits.
  if (canary::ui::settings_ui_owns_backlight()) return;
  // A commissioning code needs a bright glass: a camera lens is squinting
  // at it from a hand-width away.
  if (canary::ui::commission_ui_active()) {
    canary::hal::backlight_set(CD_BRIGHT_DAY);
    return;
  }
  auto& fleet = canary::fleet::the_fleet();
  const bool wake = (int32_t)(now - g_wake_until_ms) < 0;
  const bool urgent = fleet.worst(now) >= Sev::Alert && !fleet.ack_active(now);

#if defined(FEATURE_WAKE_ALARM) && FEATURE_WAKE_ALARM
  // Sunrise ramp override: dawn outranks the ladder, never dims it.
  const int wl = canary::care::wake_alarm_backlight();
#else
  const int wl = -1;
#endif

  uint8_t level;
  if (urgent) {
    level = glass::day_level();
  } else if (wake) {
    // Nightstand finding: a 3 a.m. time-check must not blast day
    // brightness into dark-adapted eyes — night wakes peek dim.
    level = night ? CD_BRIGHT_PEEK : glass::day_level();
  } else if (!night) {
    level = glass::ambient_level();
  } else {
    // Steady night: the runtime "screen at night" choice, on the fine
    // 13-bit night profile so the calibrated floor is actually reachable.
    if (wl > 0) {
      canary::hal::backlight_set((uint8_t)wl);  // dawn is already rising
      return;
    }
    bool dark_ok = false;
    if (glass::settings().night_screen == glass::NIGHT_SCREEN_OFF) {
      // Going dark is a choice, but honesty holds the veto: any Warn+
      // condition or a dead link keeps the night glow — silence is never
      // rendered as safety. A NEVER-CONFIGURED hub is the true standalone
      // signal (an empty fleet is also what a configured display sees
      // rebooting mid-outage, and THAT display must keep the honest glow).
      const bool links_ok =
          canary::net::wifi_connected() && canary::net::mqtt_connected();
      dark_ok = canary::net::mqtt_broker_is_placeholder() ||
                (links_ok && fleet.worst(now) < Sev::Warn);
    }
    canary::hal::backlight_night_set(dark_ok ? 0
                                             : glass::night_duty_effective());
    return;
  }
  if (wl > (int)level) level = (uint8_t)wl;
  canary::hal::backlight_set(level);
}

// ----------------------------------------------------------------------------
// Touch: tap = wake/page, long-press = acknowledge
// ----------------------------------------------------------------------------

static void ui_ack_hold(bool active) {
  if (!g_display_ok) return;
#ifdef CD_FLAVOR_WATCH
  canary::ui::glance_ui_ack_hold(active);
#endif
#ifdef CD_FLAVOR_DASH
  canary::ui::dash_ui_ack_hold(active);
#endif
#ifdef CD_FLAVOR_NIGHTSTAND
  canary::ui::portrait_ui_ack_hold(active);
#endif
}

#if defined(FEATURE_CARE) && FEATURE_CARE
// Mute toggle for one witness ("act on what you're looking at"): quiet
// until morning (next CD_QUIET_END_HOUR) with the promise persisted to NVS;
// clockless fallback is 8 h, this boot only. Un-muting clears both.
static void toggle_mute(const canary::fleet::Witness& w, uint32_t now) {
  auto& fleet = canary::fleet::the_fleet();
  if (canary::fleet::Fleet::mute_active(w, now)) {
    fleet.set_mute(w.id, false, 0);
    canary::fleet::mute_store_put(w.id, 0);
    boot_line("[input] long-press -> unmute witness");
#if defined(FEATURE_CHIME) && FEATURE_CHIME
    canary::hal::voice_play(canary::hal::Voice::MuteOff);  // rising: back on
#endif
    return;
  }
  const uint32_t until_epoch = canary::care::mute_until_morning_epoch();
  uint32_t until_ms;
  const uint32_t now_epoch = (uint32_t)time(nullptr);
  if (until_epoch > now_epoch) {
    // Strictly-greater guard: an SNTP step between the two time() reads
    // must not underflow into a ~49-day mute (review catch).
    until_ms = now + (until_epoch - now_epoch) * 1000UL;
    canary::fleet::mute_store_put(w.id, until_epoch);
  } else {
    until_ms = now + 8UL * 3600UL * 1000UL;  // no/odd clock: 8 h, unpersisted
  }
  fleet.set_mute(w.id, true, until_ms);
  boot_line("[input] long-press -> mute witness until morning");
#if defined(FEATURE_CHIME) && FEATURE_CHIME
  canary::hal::voice_play(canary::hal::Voice::MuteOn);  // falling: going quiet
#endif
}
#endif

static void handle_touch(uint32_t now) {
  const auto s = canary::hal::touch_read();
  auto& fleet = canary::fleet::the_fleet();

  // If a modal surface (settings / commissioning) closed out from under a
  // held finger (urgent close on a live alert), the remainder of that touch
  // must be swallowed — otherwise the base face sees the same held finger
  // age past the long-press deadline and fires an acknowledge/mute the user
  // never made (review catch). Marking the long-press as already-fired
  // parks both the hold action and the release tap.
  const bool modal_settings = canary::ui::settings_ui_active();
  const bool modal_commission = canary::ui::commission_ui_active();
  static bool s_modal_had_touch = false;
  if (s_modal_had_touch && !modal_settings && !modal_commission) {
    if (g_touch_down) g_longpress_fired = true;
    s_modal_had_touch = false;
  }

  // While a modal surface is open it owns every gesture: taps route to its
  // zones, long-press is the quick way out, and the face's page/ack/mute
  // gestures stay parked. The wake window is pinned so the glass never
  // dims mid-adjustment.
  if (modal_settings || modal_commission) {
    s_modal_had_touch = true;
    g_wake_until_ms = now + CD_TOUCH_WAKE_MS;
    if (s.touched && !g_touch_down) {
      g_touch_down = true;
      g_touch_down_ms = now;
      g_touch_x = s.x;
      g_touch_y = s.y;
      g_longpress_fired = false;
    } else if (s.touched && g_touch_down && !g_longpress_fired &&
               (int32_t)(now - g_touch_down_ms) >= (int32_t)CD_LONGPRESS_MS) {
      g_longpress_fired = true;
      if (modal_settings) canary::ui::settings_ui_close();
      else canary::ui::commission_ui_close();
    } else if (!s.touched && g_touch_down) {
      g_touch_down = false;
      if (!g_longpress_fired) {
        // Re-check which surface is live: a settings tap may have handed
        // off to commissioning within this very gesture.
        if (canary::ui::settings_ui_active())
          canary::ui::settings_ui_handle_tap(g_touch_x, g_touch_y);
        else if (canary::ui::commission_ui_active())
          canary::ui::commission_ui_handle_tap(g_touch_x, g_touch_y);
      }
    }
    return;
  }

#if defined(CD_FLAVOR_DASH) && defined(FEATURE_CARE) && FEATURE_CARE
  // Cleaning mode (transparency sheet -> "wipe the glass"): a wall panel
  // must survive a wet cloth without acking an alarm. Swallow everything
  // until the lockout ends.
  if (canary::ui::dash_ui_touch_locked(now)) {
    if (g_touch_down) {
      g_touch_down = false;
      g_longpress_fired = false;
      ui_ack_hold(false);
    }
    return;
  }
#endif

  if (s.touched && !g_touch_down) {
    g_touch_down = true;
    g_touch_down_ms = now;
    g_touch_x = s.x;
    g_touch_y = s.y;
    g_longpress_fired = false;
    ui_ack_hold(true);   // sweep starts; a tap only ever shows a sliver
    // Touch-startle: the bird notices you. The mark rations these and
    // refuses them while hidden or asleep, so this stays a cheap poke.
    canary::ui::canary_mark_react(canary::ui::CanaryReact::Startle);
    return;
  }

  if (s.touched && g_touch_down && !g_longpress_fired &&
      (int32_t)(now - g_touch_down_ms) >= (int32_t)CD_LONGPRESS_MS) {
    g_longpress_fired = true;
    ui_ack_hold(false);
    g_wake_until_ms = now + CD_TOUCH_WAKE_MS;

#ifdef CD_FLAVOR_WATCH
    // Settings doorway (settings wave): a long-press on its page opens the
    // settings surface — never the ack, never a mute.
    if (g_page == canary::ui::glance_settings_page()) {
      canary::ui::settings_ui_open();
      return;
    }
    // Empty-nest doorway (onboarding wave): with no canaries there is
    // nothing to acknowledge, so a long-press on the hero page mints the
    // add-a-canary code — the hero sub-line invites exactly this.
    if (g_page == 0 && fleet.count() == 0) {
      canary::ui::commission_ui_open();
      return;
    }
#endif

#if defined(FEATURE_CARE) && FEATURE_CARE
    // Long-press routes by what the finger is ON (the panel "bypass"
    // pattern, made deliberate): a witness in view -> mute/unmute THAT
    // witness; anywhere else -> the household acknowledge.
#ifdef CD_FLAVOR_WATCH
    if (g_page >= 1 && g_page <= fleet.count()) {
      const auto* w = fleet.at(g_page - 1);
      if (w) { toggle_mute(*w, now); return; }
    }
#endif
#ifdef CD_FLAVOR_DASH
    {
      const int card = canary::ui::dash_ui_card_at(g_touch_x, g_touch_y);
      if (card >= 0) {
        const auto* w = fleet.at(card);
        if (w) { toggle_mute(*w, now); return; }
      }
    }
#endif
#endif  // FEATURE_CARE

    // Long-press: acknowledge. Quiet, deliberate, works half-asleep.
    fleet.acknowledge_by(now, canary::cfg::get().device_id);
#if defined(FEATURE_CHIME) && FEATURE_CHIME
    canary::hal::voice_play(canary::hal::Voice::AckConfirm);  // a warm "got it"
#endif
#if defined(FEATURE_ACK_SYNC) && FEATURE_ACK_SYNC
    // Household ack-sync (spec §2): tell the sibling displays — only with
    // a real clock (no guessed timestamps on the wire).
    const time_t epoch = time(nullptr);
    if (epoch > 1700000000) canary::net::publish_fleet_ack((uint32_t)epoch);
#endif
    boot_line("[input] long-press -> acknowledge");
    return;
  }

  if (!s.touched && g_touch_down) {
    g_touch_down = false;
    ui_ack_hold(false);
    if (g_longpress_fired) return;
    // Tap. First tap in the dark only wakes; a lit tap navigates. A night
    // wake is a short peek (user-tunable), a day wake the full window.
    const bool night_now = in_quiet_hours();
    const bool was_awake = (int32_t)(now - g_wake_until_ms) < 0 || !night_now;
    g_wake_until_ms = now + (was_awake ? CD_TOUCH_WAKE_MS
                                       : wake_window_ms(night_now));
#if defined(FEATURE_WAKE_ALARM) && FEATURE_WAKE_ALARM
    // A live wake alarm owns the tap: dismiss, light the peek, done.
    if (canary::care::wake_alarm_tap()) {
      fleet.mark_dirty();
      return;
    }
#endif
#ifdef CD_FLAVOR_WATCH
    if (was_awake) {
      g_page = (g_page + 1) % canary::ui::glance_page_count();
      g_page_touched_ms = now;
#if defined(FEATURE_CHIME) && FEATURE_CHIME
      canary::hal::voice_play(canary::hal::Voice::PageTurn);  // a light flip
#endif
#if defined(FEATURE_FLEET_LINK) && FEATURE_FLEET_LINK
      // Tapping to a witness detail page queues an off-grid GATT status pull
      // for that WAP (no-op online; the loop only acts while broker-down).
      if (g_page >= 1 && g_page <= fleet.count()) {
        const auto* w = fleet.at(g_page - 1);
        if (w && w->fp[0]) canary::net::fleet_link_request(w->fp);
      }
#endif
    } else {
      // Glance-first wake: waking from the dark always lands on the one
      // big fact (the halo hero), never mid-rotation on a detail page —
      // the distance-tiered pattern, keyed on the wake edge.
      g_page = 0;
    }
#endif
#ifdef CD_FLAVOR_DASH
    // Proof-on-Glass (spec §1): a lit tap on a witness card opens its
    // proof sheet; a tap on an open sheet closes it.
    if (was_awake && g_display_ok) {
      canary::ui::dash_ui_handle_tap(g_touch_x, g_touch_y);
#if defined(FEATURE_FLEET_LINK) && FEATURE_FLEET_LINK
      // A lit tap on a witness card queues an off-grid GATT status pull for
      // that WAP (no-op online; the loop only acts while broker-down).
      const int fl_card = canary::ui::dash_ui_card_at(g_touch_x, g_touch_y);
      if (fl_card >= 0) {
        const auto* w = fleet.at(fl_card);
        if (w && w->fp[0]) canary::net::fleet_link_request(w->fp);
      }
#endif
    }
#endif
    fleet.mark_dirty();
  }
}

// ----------------------------------------------------------------------------
// Render
// ----------------------------------------------------------------------------

static void render(uint32_t now) {
  if (!g_display_ok) return;
  auto& fleet = canary::fleet::the_fleet();
  const bool night = in_quiet_hours();
  // Night outranks the Character (wave 3): the MODE — not the red-look
  // preference — forces the uniform dark ground at the theme choke
  // point, so the light Almanac can never glow in a bedroom. Set before
  // anything below reads a col_* accessor.
  canary::ui::character_set_night(night);
  // A live screen wears the ground that was true when it was BUILT; when
  // the effective ground flips — quiet hours begin/end, or a new
  // Character was picked — rebuild the face in place so the floor
  // actually changes (review catch: without this, a cream Almanac stayed
  // cream after dark until reboot). Guarded to when the face owns the
  // glass; while settings/commissioning are up the flip waits and lands
  // right after they close. Onboarding needs no guard: a flip requires a
  // valid clock or the picker, and neither exists before provisioning.
  // Open face modals close with the rebuild — a ground change is a scene
  // change.
  {
    static bool s_ground_night = canary::ui::character_night();
    static canary::ui::Character s_ground_char = canary::ui::active_character();
    if ((canary::ui::character_night() != s_ground_night ||
         canary::ui::active_character() != s_ground_char) &&
        !canary::ui::settings_ui_active() &&
        !canary::ui::commission_ui_active()) {
      s_ground_night = canary::ui::character_night();
      s_ground_char = canary::ui::active_character();
      lv_obj_clean(lv_scr_act());
#ifdef CD_FLAVOR_WATCH
      canary::ui::glance_ui_create();
#endif
#ifdef CD_FLAVOR_DASH
      canary::ui::dash_ui_create();
#endif
#ifdef CD_FLAVOR_NIGHTSTAND
      canary::ui::portrait_ui_create();
#endif
    }
  }
  // "Night look" (settings wave): the red-shifted palette is the default
  // night face, but it's a preference — with it off, night keeps the day
  // palette at the calibrated night glow. Brightness policy runs on the
  // schedule either way.
  const bool night_look = night && canary::glass::settings().red_shift != 0;
  // Living canary: one face per pass from the mood engine; the UIs decide
  // whether the current page offers a perch.
  int yday = -1;
  local_time(nullptr, nullptr, &yday);
  const canary::ui::CanaryMood bird =
      canary::care::bird_mood_tick(now, night, yday >= 0, yday);

  // Feed the web mirror the same state the glass is about to draw — the
  // phone view can never disagree with the wall.
  {
    int mhh = 0, mmm = 0;
    const bool mtv = local_time(&mhh, &mmm);
    canary::net::glass_web_publish(fleet, now, night_look, mtv, mhh, mmm,
                                   canary::net::wifi_connected(),
                                   canary::net::mqtt_connected(), bird);
  }

#ifdef CD_FLAVOR_WATCH
  // Auto-return to the overview page after idle.
  if (g_page != 0 && (int32_t)(now - g_page_touched_ms) >= 20000) g_page = 0;

  canary::ui::GlanceState st;
  st.page = g_page;
  st.night = night_look;
  st.wifi_ok = canary::net::wifi_connected();
  st.mqtt_ok = canary::net::mqtt_connected();
  st.acked = fleet.ack_active(now);
  st.time_valid = local_time(&st.clock_hh, &st.clock_mm);
  st.bird = bird;
  canary::ui::glance_ui_update(fleet, now, st);
#endif
#ifdef CD_FLAVOR_DASH
  canary::ui::DashState st;
  st.night = night_look;
  st.wifi_ok = canary::net::wifi_connected();
  st.mqtt_ok = canary::net::mqtt_connected();
  st.acked = fleet.ack_active(now);
  st.time_valid = local_time(&st.clock_hh, &st.clock_mm);
  st.bird = bird;
  canary::ui::dash_ui_update(fleet, now, st);
#endif
#ifdef CD_FLAVOR_NIGHTSTAND
  canary::ui::PortraitState st;
  st.night = night_look;
  st.wifi_ok = canary::net::wifi_connected();
  st.mqtt_ok = canary::net::mqtt_connected();
  st.acked = fleet.ack_active(now);
  st.time_valid = local_time(&st.clock_hh, &st.clock_mm);
  st.bird = bird;
  canary::ui::portrait_ui_update(fleet, now, st);
#endif

  apply_brightness(now, night);
}

// ----------------------------------------------------------------------------
// Boot output redirect (USB-CDC console)
// ----------------------------------------------------------------------------

static void display_serial_write(const char* str) {
  Serial.print(str);
}

void setup() {
#if defined(FEATURE_AMBIENT_LED) && FEATURE_AMBIENT_LED
  // First visible sign of life: the behind-the-glass beacon lights bright
  // canary yellow before ANYTHING else — before serial settles, before the
  // panel initializes — so power-on is answered instantly. The first render
  // tick replaces it with the real state (or honest darkness).
  canary::hal::ambient_led_init();
#endif
  Serial.begin(115200);
  delay(600);

  boot_set_output(display_serial_write);

  // Privacy (Invariant III): never surface the raw MAC. The stable device
  // handle is the salted, MAC-free pseudonym shown as "Hardware ID" below.
  boot_info_t bi = {};
  bi.product_name  = "SecuraCV Canary Display";
  bi.fw_version    = CANARY_FW_VERSION;
  bi.build_date    = __DATE__;
  bi.build_time    = __TIME__;
  bi.device_type   = CD_DEVICE_TYPE;
  bi.model         = CD_MODEL;
  bi.board_name    = BOARD_NAME;
  bi.chip_model    = ESP.getChipModel();
  bi.chip_revision = (uint8_t)ESP.getChipRevision();
  bi.cpu_freq_mhz  = (uint16_t)ESP.getCpuFreqMHz();
  bi.cpu_cores     = (uint8_t)ESP.getChipCores();
  bi.flash_mb      = (uint32_t)(ESP.getFlashChipSize() / (1024 * 1024));
  bi.heap_free_kb  = (uint32_t)(ESP.getFreeHeap() / 1024);
  bi.sdk_version   = ESP.getSdkVersion();

  boot_scene_banner(&bi);
  boot_scene_hardware(&bi);

#ifdef CD_MODES_COMPILED
  // Non-fleet gears (docs/hardware/display_modes.md): resolve this boot's
  // gear from the NVS latch — the dedicated bench env always boots the
  // bench, an unknown/uncarried token fails safe to the fleet face, and the
  // legacy "devmode" bool still lands in the bench it asked for. A non-fleet
  // gear owns the device from here; per-mode policy (mode_registry.h,
  // host-tested) pins what it may touch — only debug brings the network up,
  // none take OTA, none arm the watchdog. Everything below this block is
  // fleet-only bring-up.
  s_active_mode = canary::mode::boot_mode();
  if (s_active_mode != canary::mode::Mode::Fleet) {
    canary::mode::mode_enter(s_active_mode);
    return;
  }
#endif

  // Display-specific boot scene.
  boot_line("              .--------.");
  boot_line("              |  o  o  |        The fleet's face:");
  boot_line("              |  ----  |        it shows, it never watches.");
  boot_line("              '--------'");
  boot_separator();
#ifdef CD_FLAVOR_WATCH
  boot_kv("Glass",   "GC9A01 1.28\" 240x240 round + CST816S touch");
  boot_kvf("SPI",    "SCK=%d MOSI=%d CS=%d DC=%d BL=%d(PWM)",
           TFT_PIN_SCK, TFT_PIN_MOSI, TFT_PIN_CS, TFT_PIN_DC, TFT_PIN_BL);
#endif
#ifdef CD_FLAVOR_DASH
  boot_kv("Glass",   "800x480 RGB565 parallel + GT911 touch (CH422G expander)");
  boot_kvf("RGB",    "DE=%d VS=%d HS=%d PCLK=%d @ %d Hz",
           LCD_PIN_DE, LCD_PIN_VSYNC, LCD_PIN_HSYNC, LCD_PIN_PCLK, LCD_PCLK_HZ);
#endif
#ifdef CD_FLAVOR_NIGHTSTAND
  boot_kv("Glass",   "ST7789 1.47\" 172x320 portrait SPI (no touch)");
  boot_kvf("SPI",    "SCK=%d MOSI=%d CS=%d DC=%d BL=%d(PWM) off=%d",
           TFT_PIN_SCK, TFT_PIN_MOSI, TFT_PIN_CS, TFT_PIN_DC, TFT_PIN_BL,
           TFT_COL_OFFSET);
#if defined(FEATURE_AMBIENT_LED) && FEATURE_AMBIENT_LED
  boot_kvf("Beacon", "WS2812 x%d on GPIO%d (RMT) — the state channel",
           RGBLED_COUNT, RGBLED_PIN);
#endif
#endif
#if !defined(CD_FLAVOR_NIGHTSTAND)
  // The 1.47" nightstand boards have no touch controller (TOUCH_I2C_ADDR is
  // not defined on those pin maps) — the I2C header there is sensor-only.
  boot_kvf("Touch",  "I2C SDA=%d SCL=%d  addr 0x%02X",
           I2C_PIN_SDA, I2C_PIN_SCL, TOUCH_I2C_ADDR);
#endif
  boot_kvf("Fleet",  "up to %d witnesses, stale %lus, lost %lus",
           CD_FLEET_MAX_DEVICES,
           (unsigned long)(CD_STALE_AFTER_MS / 1000),
           (unsigned long)(CD_LOST_AFTER_MS / 1000));
  // Runtime screen settings (settings wave): load before anything reads
  // quiet hours or brightness. Compile-time CD_* values are first-boot
  // seeds; the on-glass settings surface owns them from here.
  canary::glass::settings_init();
  // Character wave: the glass wakes up already wearing the saved look —
  // theme.h reads the active Character, and nothing has drawn yet (a
  // stale or corrupt blob degraded to Quiet Glass just above; the picker
  // re-applies on change).
  canary::ui::character_apply(
      (canary::ui::Character)canary::glass::settings().character);
  // Seed the night flag before anything paints (splash reads col_bg):
  // usually "day" this early — the clock is rarely valid before SNTP —
  // and the render tick re-evaluates every pass from here on.
  canary::ui::character_set_night(in_quiet_hours());
  boot_kvf("Quiet",  "%02d:00-%02d:00 local (%s)",
           canary::glass::settings().night_start_hh,
           canary::glass::settings().night_end_hh, CD_TZ);
  boot_blank();

  // Trust store before the network: retained chain payloads replay the
  // moment the broker accepts our subscriptions.
  canary::trust::init();

#if defined(FEATURE_WAKE_ALARM) && FEATURE_WAKE_ALARM
  // OUTSIDE the chime gate on purpose (review catch): the sunrise ramp is
  // the alarm's visual half and must restore even on a silent-piezo build
  // — chime_play() itself no-ops when the pin was never initialized.
  canary::care::wake_alarm_init();  // restore a persisted alarm — it must
                                    // survive a power blip and still fire
#endif

  // Glass before the network too — a display that boots into a visible
  // "listening" state beats a black disc while WiFi retries.
  g_display_ok = canary::hal::display_init();
  if (g_display_ok) g_display_ok = canary::ui::lvgl_port_init();

#if defined(FEATURE_CHIME) && FEATURE_CHIME
  // Canary Voice (spec §5): sound the boot signature — "the canary wakes" —
  // right here, as the glass comes up. Init needs only the pin; the engine TU
  // is always compiled for CI coverage even when the pad is unpopulated.
  canary::hal::chime_init(BUZZER_PIN);
#if defined(FEATURE_RTC) && FEATURE_RTC
  // Seed trusted wall time from the battery-backed RTC now (Wire is up after
  // display_init) so the chime's quiet-hours attenuation is right on a cold
  // reboot, before SNTP. Idempotent — the later rtc_begin() re-probes and
  // no-ops once the clock is valid.
  canary::io::rtc_begin();
#endif
  // Quiet-hours volume: use real wall time when we have it; if it's still
  // unknown (no / unreliable RTC), err quiet so a 3am reboot never chimes loud.
  int boot_hh;
  const bool boot_time_known = local_time(&boot_hh, nullptr);
  canary::hal::voice_set_night(boot_time_known ? in_quiet_hours() : true);
  canary::hal::voice_play(canary::hal::Voice::Boot);
  // Render it synchronously, at power-on. The steady-state loop that normally
  // pumps voice_loop() only runs AFTER the blocking splash, provisioning (which
  // can wait indefinitely on first setup) and Wi-Fi-or-reboot phases — so a
  // latched-only phrase would play seconds late or, on first setup / a no-Wi-Fi
  // reboot, never. Bounded (the phrase is ~330 ms) so a stuck note can't hang boot.
  {
    constexpr uint32_t kBootChimeMaxMs = 700;
    const uint32_t t0 = millis();
    while (canary::hal::voice_active() &&
           (uint32_t)(millis() - t0) < kBootChimeMaxMs) {
      canary::hal::chime_loop(millis());
      delay(1);
    }
    // Silence on the way out: a normal finish already did, but a deadline exit
    // (scheduler stall) must not leave a note sounding through the blocking
    // splash / provisioning that follow.
    canary::hal::voice_stop();
  }
#endif

  if (g_display_ok) {
    // Boot splash, then face, then the curtain lift. splash_play() ends by
    // dropping an opaque curtain over the glass instead of fading into the
    // default screen (whose unstyled bright frame WAS the "flash"); the
    // face builds and paints beneath it, and splash_reveal() wipes the
    // curtain up off a fully-drawn face — an arcade curtain, not a blink.
    canary::ui::splash_play(1700);
#ifdef CD_FLAVOR_WATCH
    canary::ui::glance_ui_create();
#endif
#ifdef CD_FLAVOR_DASH
    canary::ui::dash_ui_create();
#endif
#ifdef CD_FLAVOR_NIGHTSTAND
    canary::ui::portrait_ui_create();
#endif
    render(canary::ms_now());
    lv_timer_handler();
    canary::ui::splash_reveal();
    canary::hal::backlight_set(CD_BRIGHT_DAY);
  }

#if defined(FEATURE_ONBOARDING) && FEATURE_ONBOARDING
  // First boot (placeholder WiFi credentials): the glass becomes the setup
  // guide — SoftAP + join QR + captive portal — instead of reboot-looping
  // against a network that was never configured. Blocking modal phase; runs
  // BEFORE the watchdog is armed (same class as the WiFi boot connect) and
  // returns with the STA associated and credentials persisted.
  if (canary::net::provision_needed()) canary::net::provision_run(g_display_ok);
#endif

  TOPICS = build_topics(canary::cfg::get().device_id);

  canary::net::wifi_init_or_reboot();

#if defined(FEATURE_MDNS_DISCOVERY) && FEATURE_MDNS_DISCOVERY
  canary::net::discovery_init(canary::cfg::get().device_id, DEVICE_TYPE,
                              "display");
#endif

#if defined(FEATURE_SNTP) && FEATURE_SNTP
  // Wall time = UTC (SNTP, two independent sources) + a timezone rule.
  // The rule comes from NVS if a previous boot learned it from the web
  // (tz_auto — DST included), else the CD_TZ seed (UTC0 unless secrets.h
  // set a real one). The clock read "00:00" at 8 PM on the bench because
  // the seed was honest UTC — right instant, wrong wall.
  {
    char tz[48];
    canary::net::tz_boot_string(CD_TZ, tz, sizeof(tz));
    configTzTime(tz, "pool.ntp.org", "time.nist.gov");
  }
#endif

#if defined(FEATURE_RTC) && FEATURE_RTC
  // Trusted time: if a PCF8563 is populated on the shared I2C bus, seed the
  // clock from it now (so timestamps are trustworthy before/without SNTP); the
  // loop mirrors NTP back to it once the network gives a real wall time.
  canary::io::rtc_begin();
#endif

#if defined(FEATURE_ESPNOW) && FEATURE_ESPNOW
  // Router-independent peer presence: bring up the ESP-NOW listener now that the
  // WiFi driver is started. Receive-only — the dash observes peer beacons off
  // the raw channel when the AP/broker is down; it never transmits or signs.
  canary::net::espnow_begin();
#endif

  // The display's own web page — live mirror, help, master settings.
  // Started AFTER provisioning (the portal owned :80 during setup) and
  // independent of the panel: a display with broken glass still mirrors.
  canary::net::glass_web_init();

#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
  // Boot the time machine (spec 7): reload recent proof-carrying history (when
  // persistence is enabled) and wire the fleet's event sink so every new event
  // is journaled. Must precede the first MQTT ingest below so nothing is
  // missed; degrades to RAM-only if the filesystem won't mount.
  canary::fleet::journal_begin();
#endif

#if defined(FEATURE_CARE) && FEATURE_CARE
  // Care wave: restore the learned rhythm baseline from NVS (mutes re-apply
  // later, from care_loop, once SNTP gives both clocks).
  canary::care::care_begin();
#endif

#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
  // 4.3B field I/O: isolated contacts -> unsigned local events, siren on
  // unacked alerts. Reports under this dash's own id; never signs.
  canary::io::field_io_begin(canary::cfg::get().device_id);
#endif

#if defined(FEATURE_MIC_ALARM) && FEATURE_MIC_ALARM && \
    defined(HAS_MICROPHONE) && HAS_MICROPHONE
  // 4.3C acoustic alarm listener (display_mic_variant.md): smoke-T3/CO-T4
  // cadences -> unsigned local events. OFF by default; the amber MIC chip
  // is lit exactly while the capture driver runs; disarm uninstalls the
  // driver (the hard mute). Reports under this dash's own id; never signs,
  // never records, never streams.
  canary::io::mic_begin(canary::cfg::get().device_id, g_display_ok);
#endif

  // Seed the heap-health snapshot so the first status publish carries real
  // numbers instead of zeros.
  canary::diag::loop(canary::ms_now());

  // Confirm (or roll back) a freshly installed image now — before anything
  // that can block on external services. See ota_mgr.h.
  canary::net::ota_boot_validate();

  canary::net::mqtt_init(TOPICS);

#if defined(FEATURE_MDNS_DISCOVERY) && FEATURE_MDNS_DISCOVERY
  // Zero-config join: no broker was provisioned (fresh flash with the
  // stock secrets template) — ask the fleet before the first connect
  // attempt. One hand-provisioned device on the LAN makes every later
  // one plug-and-play; the referral persists to NVS via mqtt_set_broker.
  if (canary::net::mqtt_broker_is_placeholder()) {
    boot_kv("Broker", "unconfigured — asking the fleet (mDNS)");
    char host[64];
    uint16_t port = 1883;
    if (canary::net::discovery_find_broker(host, sizeof(host), &port)) {
      canary::net::mqtt_set_broker(host, port);
      boot_kvf("Broker", "fleet referral: %s:%u", host, (unsigned)port);
    } else {
      boot_kv("Broker", "no referral yet — will keep asking from loop()");
    }
  }
#endif

  boot_line("              .--------.  ))");
  boot_line("              |  o  o  |  ))    Connecting to MQTT...");
  boot_line("              '--------'");
  boot_separator();
  boot_kv("Device ID", canary::cfg::get().device_id);
  char devid_hex[device_pseudonym::HEX_LEN + 1] = {0};
  if (device_pseudonym::device_id_hex(devid_hex, sizeof(devid_hex))) {
    boot_kv("Hardware ID", devid_hex);  // salted pseudonym, not the raw MAC
  }
  boot_kvf("Heartbeat", "every %lu ms", (unsigned long)HEARTBEAT_MS);
  boot_blank();

  // ONE bounded connect attempt. A broker that is down at boot must not
  // block the display: it renders "broker down" and loop()'s backoff
  // supervisor keeps trying.
  if (!mqtt_supervise(canary::ms_now())) {
    canary::log_line("MQTT", "Broker unreachable — rendering anyway; retrying in loop().");
  }

  // Signed pull-OTA: arm the engine (validation already ran right after
  // WiFi). Daily jittered checks; HA's Install button and auto-update
  // switch are drained in loop().
  canary::net::ota_init(TOPICS);

#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
  // Task watchdog (arduino-esp32 2.0.x / IDF4 API on the S3 envs) — armed
  // LAST so the blocking boot phases above (WiFi connect up to 30 s) can't
  // trip it. Timeout must exceed loop()'s worst bounded block (one MQTT
  // connect attempt against a dead broker).
  canary::hal::cc_task_wdt_arm(CD_WATCHDOG_TIMEOUT_SEC);
  boot_kvf("Watchdog", "%lu s timeout", (unsigned long)CD_WATCHDOG_TIMEOUT_SEC);
#endif

  boot_scene_ready(
      "It will watch the witnesses so you don't have to,",
      "verify their chains on its own silicon, and never phone home.",
      NULL);

  render(canary::ms_now());
}

void loop() {
#ifdef CD_MODES_COMPILED
  if (s_active_mode != canary::mode::Mode::Fleet) {
    canary::mode::mode_loop_step(s_active_mode);
    return;
  }
#endif

#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
  esp_task_wdt_reset();
#endif

  const uint32_t now = canary::ms_now();
  auto& fleet = canary::fleet::the_fleet();

  // ── Input + model time first: the face stays honest with or without a
  //    network (staleness deadlines run locally) ──
  handle_touch(now);
  fleet.tick(now);

  // Settings wave: debounced flash commit + modal-surface housekeeping
  // (idle close, live wizard clock, commissioning countdown/celebration,
  // instant close on a real alarm).
  canary::glass::settings_loop(now);
#if defined(FEATURE_SNTP) && FEATURE_SNTP
  canary::net::tz_auto_tick(now);  // learn the wall-clock zone once online
#endif
  canary::net::glass_web_tick(now);  // serve the phone mirror
  {
    using canary::fleet::Sev;
    const bool urgent =
        fleet.worst(now) >= Sev::Alert && !fleet.ack_active(now);
    canary::ui::settings_ui_tick(now, urgent);
    canary::ui::commission_ui_tick(now, fleet.count(), urgent);
  }

#if defined(FEATURE_PRESENCE_WAKE) && FEATURE_PRESENCE_WAKE
  // Presence-wake (spec §3): a fresh Notice+ event promotes Ambient to
  // Active — the hallway canary lights the display before you reach it.
  // Never during quiet hours below Alert (G5); apply_brightness enforces
  // the Night floor regardless of this wake window.
  {
    const auto* e = fleet.event_at(0);
    if (e && !in_quiet_hours() &&
        e->sev >= canary::fleet::Sev::Notice &&
        (int32_t)(now - e->at_ms) < (int32_t)CD_PRESENCE_WAKE_MS &&
        e->at_ms != g_last_wake_event_ms) {
      g_last_wake_event_ms = e->at_ms;
      g_wake_until_ms = now + CD_TOUCH_WAKE_MS;
    }
  }
#endif

#if !(defined(FEATURE_CARE) && FEATURE_CARE)
#if defined(FEATURE_CHIME) && FEATURE_CHIME
  // Sound identity (spec §5) — legacy inline grammar, only compiled when
  // the care wave is off (FEATURE_CARE owns the chime via its attention
  // policy: same tiers, plus night suppression + ramp + morning summary).
  {
    using canary::fleet::Sev;
    static Sev s_prev_worst = Sev::Ok;
    static uint32_t s_last_voice_ms = 0;
    const Sev worst = fleet.worst(now);
    const bool acked = fleet.ack_active(now);
    const bool night = in_quiet_hours();
    if (worst >= Sev::Alert && !acked) {
      // Tier 1 is the one sound allowed to break quiet hours; it re-voices
      // until acknowledged.
      if (s_prev_worst < Sev::Alert ||
          (int32_t)(now - s_last_voice_ms) >= (int32_t)CD_CHIME_REVOICE_MS) {
        canary::hal::chime_play(canary::hal::Chime::Tier1Alarm);
        s_last_voice_ms = now;
      }
    } else if (worst == Sev::Warn && s_prev_worst < Sev::Warn && !night) {
      canary::hal::chime_play(canary::hal::Chime::Tier2Warn);
      s_last_voice_ms = now;
    } else if (worst <= Sev::Notice && s_prev_worst >= Sev::Warn && !night) {
      // Resolution deserves a sound — falling tone, exactly once.
      canary::hal::chime_play(canary::hal::Chime::AllClear);
    }
    s_prev_worst = worst;
  }
  canary::hal::chime_loop(now);
#if defined(FEATURE_WAKE_ALARM) && FEATURE_WAKE_ALARM
  canary::care::wake_alarm_loop(now);
#endif
#endif
#endif  // !FEATURE_CARE

  // ── Network supervision ──
  canary::net::wifi_loop(now);
  if ((int32_t)(now - g_last_diag_ms) >= 1000) {
    g_last_diag_ms = now;
    canary::diag::loop(now);
  }

  const bool broker = mqtt_supervise(now);

#if defined(FEATURE_CHIME) && FEATURE_CHIME
  // Keep the Voice loudness model in step with quiet hours every pass — the
  // model silences the gentle categories at night and only lets Wake/Alert
  // through (voice_score.h::voice_peak_duty). Cheap; in_quiet_hours() is
  // already read several times a loop.
  canary::hal::voice_set_night(in_quiet_hours());
#endif

#if defined(FEATURE_CARE) && FEATURE_CARE
  // Care wave (display_care_wave.md): the attention policy drives the chime
  // (night-silent maintenance + ramp), harvests the overnight ledger, fires
  // escalation-on-no-ack, applies persisted mutes once the clock is valid,
  // and feeds the rhythm baseline.
  {
    int hh = 0, mm = 0, doy = -1;
    const bool tv = local_time(&hh, &mm, &doy);
    canary::care::care_loop(now, in_quiet_hours(), broker, tv, hh, mm, doy);
  }
#endif

#if defined(FEATURE_CHIRP_SCAN) && FEATURE_CHIRP_SCAN
  // Off-grid fallback (spec §6): while the broker is dark and WiFi may be
  // too, passive BLE bursts keep tamper/liveness flowing to the glass. The
  // module itself stops scanning the moment the broker is back.
  canary::net::chirp_scan_loop(now, !broker, canary::net::wifi_connected());
#endif

#if defined(FEATURE_ESPNOW) && FEATURE_ESPNOW
  // Drain any peer presence beacons the ESP-NOW listener captured into the
  // fleet model (unsigned observation, same footing as the BLE beacon).
  canary::net::espnow_loop(now);
#endif

#if defined(FEATURE_MDNS_DISCOVERY) && FEATURE_MDNS_DISCOVERY
  // Broker-less fleet enumeration (WiFi analog of the BLE presence beacon):
  // once every device is on the home WiFi, browse the fleet's mDNS adverts
  // directly and show every nearby Canary even with NO broker/Home Assistant.
  // Gated on the broker being DOWN — when it's up, MQTT is the richer source,
  // so skip the mDNS enumeration to avoid churn. Self-rate-limits (~20 s) and
  // only actually queries when due; the query blocks ~3 s inside ESPmDNS.
  if (!broker) canary::net::discovery_scan_witnesses(now);
#endif

#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO
  // Poll the isolated contacts into events and drive the siren output. Cheap
  // and self-throttled; 4.3B only.
  canary::io::field_io_loop(now);
#endif

#if defined(FEATURE_MIC_ALARM) && FEATURE_MIC_ALARM && \
    defined(HAS_MICROPHONE) && HAS_MICROPHONE
  // Drain mic frames into the cadence detector (scalars only — samples are
  // zeroed inside the module). Self-throttled; 4.3C only.
  canary::io::mic_loop(now);
  // Opt-in "wake on a sound": a loud onset (a door close) lights a dark dash,
  // the same wake a touch or a presence event gives it. The mic layer only
  // raises the request while it's actually listening; we turn it into a wake
  // window here so the display keeps a single owner of that state.
  if (canary::io::mic_take_wake_request()) {
    const bool night = in_quiet_hours();
    g_wake_until_ms = now + wake_window_ms(night);
    g_last_wake_event_ms = now;
  }
#endif

#if defined(FEATURE_RTC) && FEATURE_RTC
  // Mirror NTP back to the PCF8563 once the clock is real. Self-throttled.
  canary::io::rtc_loop(now);
#endif

#if defined(FEATURE_FLEET_LINK) && FEATURE_FLEET_LINK
  // Layer 3: on-demand GATT pull of a WAP's live status (a display tap queues
  // the request via fleet_link_request; this drives the bounded connect+read
  // while off-grid). Runs after the passive listener so it can stop that scan
  // before driving the shared radio as a central.
  canary::net::fleet_link_loop(now, !broker, canary::net::wifi_connected());
#endif

  // Time machine v1 (spec §7): keep the model's wall-hour current so new
  // events bin into the rolling 24 h story.
  {
    int hh = -1;
    local_time(&hh, nullptr);
    fleet.set_wall_hour(hh);
  }

  if (broker) {
    canary::net::mqtt_loop();
    canary::net::ota_loop(now);

    if ((int32_t)(now - g_last_heartbeat_ms) >=
        (int32_t)(HEARTBEAT_MS * canary::diag::period_scale())) {
      g_last_heartbeat_ms = now;
      canary::net::publish_status_retained(TOPICS, "online");
    }
    if ((int32_t)(now - g_last_health_ms) >= (int32_t)HEALTH_PUBLISH_MS) {
      g_last_health_ms = now;
      canary::net::publish_health_retained(TOPICS);
    }
  }

  // ── Content refresh: promptly on model change (frame-rate capped), and
  //    at a slow steady tick so clocks/ages/staleness colors move even when
  //    the wire is quiet. LVGL itself repaints + animates from
  //    lv_timer_handler() every pass — updates here only change WHAT is
  //    shown, never how often pixels move ──
  static bool s_render_pending = false;
  if (fleet.take_dirty()) s_render_pending = true;
  const int32_t since_render = (int32_t)(now - g_last_render_ms);
  if ((s_render_pending && since_render >= (int32_t)CD_UI_FRAME_MS) ||
      since_render >= 1000) {
    s_render_pending = false;
    g_last_render_ms = now;
    render(now);
  }

#if defined(FEATURE_AMBIENT_LED) && FEATURE_AMBIENT_LED
  // The ambient beacon breathes every loop pass (not just on render ticks) so
  // the point of light animates smoothly. The honest rule holds: night +
  // all-quiet + healthy links -> dark (a never-configured hub counts as the
  // standalone all-quiet case, exactly like apply_brightness's night floor).
  {
    using canary::fleet::Sev;
    const bool led_night = in_quiet_hours();
    const Sev led_worst = fleet.worst(now);
    const bool links_ok =
        canary::net::wifi_connected() && canary::net::mqtt_connected();
    const bool safe_dark =
        led_night && (canary::net::mqtt_broker_is_placeholder() ||
                      (links_ok && led_worst < Sev::Warn));
    canary::hal::ambient_led_tick(now, led_worst, led_night, safe_dark);
  }
#endif

  if (g_display_ok) lv_timer_handler();

  delay(5);
}
