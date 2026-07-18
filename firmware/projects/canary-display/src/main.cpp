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
#include "canary/net/mqtt_mgr.h"
#include "canary/net/ota_mgr.h"
#if defined(FEATURE_MDNS_DISCOVERY) && FEATURE_MDNS_DISCOVERY
#include "canary/net/discovery.h"
#endif
#if defined(FEATURE_CHIRP_SCAN) && FEATURE_CHIRP_SCAN
#include "canary/net/chirp_scan.h"
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
#include "canary/hal/display.h"
#include "canary/hal/chime.h"
#include "canary/hal/core_compat.h"

#include <lvgl.h>
#include "canary/ui/lvgl_port.h"
#ifdef CD_FLAVOR_WATCH
#include "canary/ui/glance_ui.h"
#endif
#ifdef CD_FLAVOR_DASH
#include "canary/ui/dash_ui.h"
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
  if (CD_QUIET_START_HOUR == CD_QUIET_END_HOUR) return false;
  if (CD_QUIET_START_HOUR < CD_QUIET_END_HOUR)
    return hh >= CD_QUIET_START_HOUR && hh < CD_QUIET_END_HOUR;
  return hh >= CD_QUIET_START_HOUR || hh < CD_QUIET_END_HOUR;  // wraps midnight
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
  auto& fleet = canary::fleet::the_fleet();
  const bool wake = (int32_t)(now - g_wake_until_ms) < 0;
  const bool urgent = fleet.worst(now) >= Sev::Alert && !fleet.ack_active(now);

  uint8_t level;
  if (urgent) {
    level = CD_BRIGHT_DAY;
  } else if (wake) {
    // Nightstand finding: a 3 a.m. time-check must not blast day
    // brightness into dark-adapted eyes — night wakes peek dim.
    level = night ? CD_BRIGHT_PEEK : CD_BRIGHT_DAY;
  } else if (!night) {
    level = CD_BRIGHT_AMBIENT;
  } else {
#if defined(FEATURE_NIGHT_BLACKOUT) && FEATURE_NIGHT_BLACKOUT
    // True darkness by default (the #1 bedside-display complaint is "still
    // too bright"). Honesty holds the veto: any Warn+ condition or a dead
    // link keeps the night glow — silence is never rendered as safety.
    // A NEVER-CONFIGURED hub is the true standalone signal (review catch:
    // an empty fleet is also what a configured display sees rebooting
    // mid-outage, and THAT display must keep the honest glow). A display
    // that never had a hub guards nothing — it earns its dark room.
    const bool links_ok =
        canary::net::wifi_connected() && canary::net::mqtt_connected();
    if (canary::net::mqtt_broker_is_placeholder() ||
        (links_ok && fleet.worst(now) < Sev::Warn)) {
      level = 0;
    } else {
      level = CD_BRIGHT_NIGHT;
    }
#else
    level = CD_BRIGHT_NIGHT;
#endif
  }
#if defined(FEATURE_WAKE_ALARM) && FEATURE_WAKE_ALARM
  // Sunrise ramp override: dawn outranks the ladder, never dims it.
  const int wl = canary::care::wake_alarm_backlight();
  if (wl > (int)level) level = (uint8_t)wl;
#endif
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
}
#endif

static void handle_touch(uint32_t now) {
  const auto s = canary::hal::touch_read();
  auto& fleet = canary::fleet::the_fleet();

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
    return;
  }

  if (s.touched && g_touch_down && !g_longpress_fired &&
      (int32_t)(now - g_touch_down_ms) >= (int32_t)CD_LONGPRESS_MS) {
    g_longpress_fired = true;
    ui_ack_hold(false);
    g_wake_until_ms = now + CD_TOUCH_WAKE_MS;

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
    // Tap. First tap in the dark only wakes; a lit tap navigates.
    const bool was_awake = (int32_t)(now - g_wake_until_ms) < 0 || !in_quiet_hours();
    g_wake_until_ms = now + CD_TOUCH_WAKE_MS;
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

#ifdef CD_FLAVOR_WATCH
  // Auto-return to the overview page after idle.
  if (g_page != 0 && (int32_t)(now - g_page_touched_ms) >= 20000) g_page = 0;

  canary::ui::GlanceState st;
  st.page = g_page;
  st.night = night;
  st.wifi_ok = canary::net::wifi_connected();
  st.mqtt_ok = canary::net::mqtt_connected();
  st.acked = fleet.ack_active(now);
  st.time_valid = local_time(&st.clock_hh, &st.clock_mm);
  canary::ui::glance_ui_update(fleet, now, st);
#endif
#ifdef CD_FLAVOR_DASH
  canary::ui::DashState st;
  st.night = night;
  st.wifi_ok = canary::net::wifi_connected();
  st.mqtt_ok = canary::net::mqtt_connected();
  st.acked = fleet.ack_active(now);
  st.time_valid = local_time(&st.clock_hh, &st.clock_mm);
  canary::ui::dash_ui_update(fleet, now, st);
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
  boot_kvf("Touch",  "I2C SDA=%d SCL=%d  addr 0x%02X",
           I2C_PIN_SDA, I2C_PIN_SCL, TOUCH_I2C_ADDR);
  boot_kvf("Fleet",  "up to %d witnesses, stale %lus, lost %lus",
           CD_FLEET_MAX_DEVICES,
           (unsigned long)(CD_STALE_AFTER_MS / 1000),
           (unsigned long)(CD_LOST_AFTER_MS / 1000));
  boot_kvf("Quiet",  "%02d:00-%02d:00 local (%s)",
           CD_QUIET_START_HOUR, CD_QUIET_END_HOUR, CD_TZ);
  boot_blank();

  // Trust store before the network: retained chain payloads replay the
  // moment the broker accepts our subscriptions.
  canary::trust::init();

#if defined(FEATURE_CHIME) && FEATURE_CHIME
  // Chime engine (spec §5) — only ever initialized when the piezo pad is
  // populated; the engine TU itself is always compiled for CI coverage.
  canary::hal::chime_init(BUZZER_PIN);
#endif
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
  if (g_display_ok) {
#ifdef CD_FLAVOR_WATCH
    canary::ui::glance_ui_create();
#endif
#ifdef CD_FLAVOR_DASH
    canary::ui::dash_ui_create();
#endif
    render(canary::ms_now());
    lv_timer_handler();
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
  configTzTime(CD_TZ, "pool.ntp.org", "time.nist.gov");
#endif

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
#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
  esp_task_wdt_reset();
#endif

  const uint32_t now = canary::ms_now();
  auto& fleet = canary::fleet::the_fleet();

  // ── Input + model time first: the face stays honest with or without a
  //    network (staleness deadlines run locally) ──
  handle_touch(now);
  fleet.tick(now);

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
  canary::net::chirp_scan_loop(now, !broker);
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
  if (g_display_ok) lv_timer_handler();

  delay(5);
}
