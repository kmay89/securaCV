/*
  SecuraCV Canary Sense — 60GHz mmWave Radar Witness Firmware (Phase 2)
  --------------------------------------------------------------------
  (c) 2026 Errer Labs / SecuraCV
  errerlabs.com | securacv.com
  GitHub: https://github.com/kmay89/securaCV

  License: Apache-2.0 (use repository license unless otherwise specified).

  PHASE 2: the radar witness publishes. On top of the Phase 0 sensing core
  (frame parser + stall-safe presence/vitals FSMs) this composes the same
  network stack as canary-vision: NVS-backed runtime config, supervised WiFi
  STA (exponential backoff + outage reboot), MQTT with LWT + Home Assistant
  discovery, the signed pull-OTA engine (firmware/common/ota) with HA update
  entity, and heap-health diagnostics.

  Privacy chokepoint (design doc §2): only coarsened claims leave this file —
  presence as a debounced state, occupant count as a 0/1/2+ bucket, distance
  as a near/mid/far band. Raw centimeters, per-target data and vitals phases
  are read and dropped here. Vitals (wellbeing builds) ride the state channel
  as a binary lock plus P1-gated BPM numerics and NEVER appear in events.

  One documented exception: the USB serial tuning console's `raw on` bench
  mode may echo the radar's raw distance/BPM scalars to the ATTENDED local
  console (never the network) so the flasher's tuning bench can calibrate
  the band edges. It is off by default, never persisted, and dies with the
  session — the network-facing vocabulary above is unaffected.
*/

#include <Arduino.h>
#include <Wire.h>

// Board pin map (boards/xiao-esp32c6-mr60/pins via -I). Pin numbers ONLY here.
#include "pins.h"

// Project composition header: flavor config (CS_*) + net/OTA/diag constants.
#include "canary/config.h"
// FEATURE_* vs HAS_* compile-time cross-check (needs pins.h + config above).
#include "core/feature_sanity.h"
#include "canary/version.h"
#include "canary/log.h"
#include "canary/topics.h"
#include "canary/types.h"
#include "canary/runtime_config.h"
#include "canary/sense_config.h"
#include "canary/diagnostics.h"
#include "canary/witness.h"
#include "canary/net/wifi_mgr.h"
#include "network/setup_portal.h"  // shared recovery portal (common/) — radio-contention query
#include "canary/net/mdns_mgr.h"
#include "canary/net/mqtt_mgr.h"
#include "canary/net/ota_mgr.h"
#if defined(FEATURE_FLEET_BEACON) && FEATURE_FLEET_BEACON
#include "canary/net/fleet_beacon_adv.h"  // advertise-only BLE presence beacon
#endif
#if defined(FEATURE_FLEET_ROSTER) && FEATURE_FLEET_ROSTER
#include "canary/net/fleet_roster_scan.h" // RX twin: track the other Canaries
#endif
#include <esp_random.h>  // esp_random() for reconnect jitter

#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
#include <esp_task_wdt.h>
#endif

// Shared, board-agnostic modules (reached via -I .../common).
#include "boot/boot_banner.h"
#include "console/tuning_console.h"     // USB serial tuning console (bench)
#include "identity/device_pseudonym.h"  // salted, MAC-free device handle (Invariant III)
#include "sensors/mmwave_mr60/mr60_uart.h"
#include "sensors/mmwave_mr60/mr60_presence.h"
#ifdef CANARY_SENSE_VITALS
#include "sensors/mmwave_mr60/mr60_vitals.h"
#endif
#if defined(FEATURE_AMBIENT_LIGHT) && FEATURE_AMBIENT_LIGHT
#include "sensors/bh1750/bh1750.h"
#endif

using securacv::mmwave::CountBucket;
using securacv::mmwave::Frame;
using securacv::mmwave::FrameParser;
using securacv::mmwave::Presence;
using securacv::mmwave::PresenceConfig;
using securacv::mmwave::PresenceEvent;
using securacv::mmwave::PresenceFSM;
using securacv::mmwave::RangeBand;

// ----------------------------------------------------------------------------
// Module instances
// ----------------------------------------------------------------------------

// Radar link: UART1 on the host (UART0 stays on the USB-CDC console).
static HardwareSerial RadarSerial(RADAR_UART_NUM);
static FrameParser g_parser;

static PresenceConfig make_presence_config() {
  // NVS-backed reflexes (sense_config): the CS_* compile-time values seed
  // the first boot; HA's number entities and the flasher tune them after.
  const auto& s = canary::cfg::sense();
  PresenceConfig c;
  c.present_debounce_ms = s.present_debounce_ms;
  c.clear_timeout_ms    = s.clear_timeout_ms;
  c.stall_timeout_ms    = s.stall_timeout_ms;
  c.near_cm             = (uint16_t)s.near_cm;
  c.mid_cm              = (uint16_t)s.mid_cm;
  return c;
}

// Constructed with placeholder defaults ON PURPOSE: make_presence_config()
// reads NVS (sense_config), which must not run during static init. setup()
// reconfigures with the real (NVS-backed) reflexes before the first tick.
static PresenceFSM g_presence(PresenceConfig{});

static void poll_sense_cfg_commands(uint32_t now);

#ifdef CANARY_SENSE_VITALS
using securacv::mmwave::VitalsConfig;
using securacv::mmwave::VitalsFSM;
using securacv::mmwave::VitalsLock;

static VitalsConfig make_vitals_config() {
  // Fully NVS-backed since the tuning console: the CS_* band macros only
  // seed sense_config's first boot (see sense_config.cpp).
  const auto& s = canary::cfg::sense();
  VitalsConfig c;
  c.lock_confirm_ms = s.vitals_lock_ms;
  c.lock_lost_ms    = s.vitals_lost_ms;
  c.breath_min_bpm  = (uint16_t)s.breath_min_bpm;
  c.breath_max_bpm  = (uint16_t)s.breath_max_bpm;
  c.heart_min_bpm   = (uint16_t)s.heart_min_bpm;
  c.heart_max_bpm   = (uint16_t)s.heart_max_bpm;
  return c;
}

static VitalsFSM g_vitals(VitalsConfig{});  // real config lands in setup() (see g_presence)
#endif

#if defined(FEATURE_AMBIENT_LIGHT) && FEATURE_AMBIENT_LIGHT
static securacv::sensors::BH1750 g_lux;
#endif

static Topics TOPICS;
static SenseSnapshot g_snap;

static char g_last_event[32] = "boot";
static uint32_t g_last_heartbeat_ms = 0;
static uint32_t g_last_health_ms = 0;
static uint32_t g_last_lux_ms = 0;
static bool g_state_dirty = false;

// Broker reconnect schedule (mirrors the WiFi supervisor's backoff): a broker
// outage must never pin the loop — the radar witness keeps sensing and each
// bounded connect attempt happens at most once per backoff window.
static uint32_t g_mqtt_next_attempt_ms = 0;
static uint32_t g_mqtt_attempts = 0;

// Broker gossip bookkeeping: advertise the working broker on the fleet
// advert while the link is up, tombstone it the moment it drops
// (ground-truth-only referral — same contract as canary-display/wap).
static bool g_broker_gossiped = false;

static bool mqtt_supervise(uint32_t now) {
  if (canary::net::mqtt_connected()) {
    g_mqtt_attempts = 0;
    return true;
  }
  if (g_broker_gossiped) {
    canary::net::mdns_clear_broker();
    g_broker_gossiped = false;
  }
  if (!canary::net::wifi_connected()) return false;  // wifi_loop owns this
  if ((int32_t)(now - g_mqtt_next_attempt_ms) < 0) return false;

  if (canary::net::mqtt_connect_attempt()) {
    g_mqtt_attempts = 0;
    canary::net::mdns_advertise_broker(canary::cfg::get().mqtt_host,
                                       canary::cfg::get().mqtt_port);
    g_broker_gossiped = true;
    canary::net::publish_status_retained(TOPICS, "online");
    // Trust surface: health carries the pubkey HA TOFU-pins on; the
    // retained chain head lets HA verify continuity immediately.
    canary::net::publish_health_retained(TOPICS);
    canary::net::publish_chain_retained(TOPICS);
    g_last_health_ms = now;
    return true;
  }

  // Exponential backoff: 2 s -> 4 s -> 8 s -> 16 s -> 30 s cap.
  uint32_t attempt = g_mqtt_attempts;
  if (attempt > 4) attempt = 4;
  uint32_t backoff_ms = 2000UL << attempt;
  if (backoff_ms > 30000UL) backoff_ms = 30000UL;
  // Fleet reconnect jitter: up to a fraction of the backoff so brokers don't
  // see a thundering herd.
  backoff_ms += esp_random() % (backoff_ms / 4 + 1);
  g_mqtt_attempts++;
  g_mqtt_next_attempt_ms = now + backoff_ms;
  return false;
}

// ----------------------------------------------------------------------------
// Privacy chokepoint: enum -> coarse wire strings. This is the FULL vocabulary
// that ever leaves the device about what the radar saw.
// ----------------------------------------------------------------------------

static const char* presence_str(Presence p) {
  switch (p) {
    case Presence::Present: return "present";
    case Presence::Clear:   return "clear";
    default:                return "unknown";
  }
}

static const char* count_str(CountBucket c) {
  switch (c) {
    case CountBucket::One:     return "1";
    case CountBucket::TwoPlus: return "2+";
    default:                   return "0";
  }
}

static const char* range_str(RangeBand r) {
  switch (r) {
    case RangeBand::Near: return "near";
    case RangeBand::Mid:  return "mid";
    case RangeBand::Far:  return "far";
    default:              return "unknown";
  }
}

// ----------------------------------------------------------------------------
// Status LED (WS2812). Color encodes presence state at a glance.
// ----------------------------------------------------------------------------

static void led_show(uint8_t r, uint8_t g, uint8_t b) {
#if defined(FEATURE_STATUS_LED) && FEATURE_STATUS_LED
  // rmtWrite-free single-pixel path via the core's rgbLedWrite() helper
  // (Arduino-ESP32 3.x name; neopixelWrite() is its deprecated alias).
  rgbLedWrite(LED_WS2812_PIN, r, g, b);
#else
  (void)r; (void)g; (void)b;
#endif
}

// Identify (the wizard's "which device is which" moment): a 10 s white
// flash driven from HA's identify button or the companion app. While the
// window is open, identify owns the LED; the presence color is restored
// from g_led_presence when it closes.
static uint32_t g_identify_until_ms = 0;  // 0 = idle
static Presence g_led_presence = Presence::Unknown;

static void led_for_presence(Presence p) {
  g_led_presence = p;
  if (g_identify_until_ms) return;  // identify owns the LED until it ends
  switch (p) {
    case Presence::Present: led_show(0, 24, 0);  break;  // green: someone here
    case Presence::Clear:   led_show(0, 0, 16);  break;  // blue: clear
    case Presence::Unknown:                              // fallthrough
    default:                led_show(24, 8, 0);  break;  // amber: no radar data
  }
}

// One [CFG] block for every applied reflex change, whichever transport
// carried it (HA number entity or the serial tuning console) — the bench
// and the boot log read the same shape.
static void log_cfg_applied() {
  const auto& s = canary::cfg::sense();
  canary::log_header("CFG");
  canary::dbg_serial().printf(
      "Radar reflexes: debounce=%lums clear=%lums stall=%lums near=%lucm "
      "mid=%lucm vlock=%lums vlost=%lums breath=%lu..%lubpm heart=%lu..%lubpm\n",
      (unsigned long)s.present_debounce_ms, (unsigned long)s.clear_timeout_ms,
      (unsigned long)s.stall_timeout_ms, (unsigned long)s.near_cm,
      (unsigned long)s.mid_cm, (unsigned long)s.vitals_lock_ms,
      (unsigned long)s.vitals_lost_ms, (unsigned long)s.breath_min_bpm,
      (unsigned long)s.breath_max_bpm, (unsigned long)s.heart_min_bpm,
      (unsigned long)s.heart_max_bpm);
}

// ── runtime radar reflexes: drain → clamp → apply → republish ───────────────
static void poll_sense_cfg_commands(uint32_t now) {
  using namespace canary::net;
  long v;
  bool any_cmd = false, changed = false, vitals_changed = false;

  if ((v = take_pending_cfg_debounce()) >= 0) {
    any_cmd = true;
    changed |= canary::cfg::sense_set_present_debounce_ms((uint32_t)v);
  }
  if ((v = take_pending_cfg_clear()) >= 0) {
    any_cmd = true;
    changed |= canary::cfg::sense_set_clear_timeout_ms((uint32_t)v);
  }
  if ((v = take_pending_cfg_stall()) >= 0) {
    any_cmd = true;
    changed |= canary::cfg::sense_set_stall_timeout_ms((uint32_t)v);
  }
  if ((v = take_pending_cfg_near()) >= 0) {
    any_cmd = true;
    changed |= canary::cfg::sense_set_near_cm((uint32_t)v);
  }
  if ((v = take_pending_cfg_mid()) >= 0) {
    any_cmd = true;
    changed |= canary::cfg::sense_set_mid_cm((uint32_t)v);
  }
  if ((v = take_pending_cfg_vlock()) >= 0) {
    any_cmd = true;
    vitals_changed |= canary::cfg::sense_set_vitals_lock_ms((uint32_t)v);
  }
  if ((v = take_pending_cfg_vlost()) >= 0) {
    any_cmd = true;
    vitals_changed |= canary::cfg::sense_set_vitals_lost_ms((uint32_t)v);
  }
  if ((v = take_pending_cfg_bmin()) >= 0) {
    any_cmd = true;
    vitals_changed |= canary::cfg::sense_set_breath_min_bpm((uint32_t)v);
  }
  if ((v = take_pending_cfg_bmax()) >= 0) {
    any_cmd = true;
    vitals_changed |= canary::cfg::sense_set_breath_max_bpm((uint32_t)v);
  }
  if ((v = take_pending_cfg_hmin()) >= 0) {
    any_cmd = true;
    vitals_changed |= canary::cfg::sense_set_heart_min_bpm((uint32_t)v);
  }
  if ((v = take_pending_cfg_hmax()) >= 0) {
    any_cmd = true;
    vitals_changed |= canary::cfg::sense_set_heart_max_bpm((uint32_t)v);
  }

  if (changed) {
    g_presence.reconfigure(make_presence_config(), now);
  }
#ifdef CANARY_SENSE_VITALS
  if (vitals_changed) {
    g_vitals.reconfigure(make_vitals_config(), now);
  }
#endif
  if (changed || vitals_changed) log_cfg_applied();
  if (any_cmd) publish_sense_cfg_retained(TOPICS);
}

static void identify_start(uint32_t now) {
  g_identify_until_ms = now + 10000UL;
  if (g_identify_until_ms == 0) g_identify_until_ms = 1;  // wrap guard
  canary::net::publish_identify_echo(TOPICS, true);
  boot_line("[identify] flashing LED for 10 s");
}

static void identify_tick(uint32_t now) {
  if (!g_identify_until_ms) return;
  if ((int32_t)(now - g_identify_until_ms) >= 0) {
    g_identify_until_ms = 0;
    canary::net::publish_identify_echo(TOPICS, false);
    led_for_presence(g_led_presence);  // hand the LED back to presence
    return;
  }
  // 2 Hz white flash — unmistakable against the steady presence colors.
  const bool on = ((now / 250) & 1) == 0;
  led_show(on ? 48 : 0, on ? 48 : 0, on ? 48 : 0);
}

// ----------------------------------------------------------------------------
// Serial boot output redirect (USB-CDC console).
// ----------------------------------------------------------------------------

static void sense_serial_write(const char* str) {
  Serial.print(str);
}

// ----------------------------------------------------------------------------
// USB serial tuning console — the flasher's post-flash tuning bench.
//
// Every runtime knob, over the same clamping setters HA uses, plus a periodic
// `[radar]` line so the console always shows what the radar sees. Works with
// no WiFi and no broker: it is serviced BEFORE the network phase of loop().
// Type `help` in any 115200-baud monitor.
// ----------------------------------------------------------------------------

static uint32_t kget_debounce() { return canary::cfg::sense().present_debounce_ms; }
static uint32_t kget_clear()    { return canary::cfg::sense().clear_timeout_ms; }
static uint32_t kget_stall()    { return canary::cfg::sense().stall_timeout_ms; }
static uint32_t kget_near()     { return canary::cfg::sense().near_cm; }
static uint32_t kget_mid()      { return canary::cfg::sense().mid_cm; }
#ifdef CANARY_SENSE_VITALS
static uint32_t kget_vlock()    { return canary::cfg::sense().vitals_lock_ms; }
static uint32_t kget_vlost()    { return canary::cfg::sense().vitals_lost_ms; }
static uint32_t kget_bmin()     { return canary::cfg::sense().breath_min_bpm; }
static uint32_t kget_bmax()     { return canary::cfg::sense().breath_max_bpm; }
static uint32_t kget_hmin()     { return canary::cfg::sense().heart_min_bpm; }
static uint32_t kget_hmax()     { return canary::cfg::sense().heart_max_bpm; }
#endif

// The knob vocabulary matches the cfg/* MQTT topic tails and the flasher's
// sns_* NVS keys, so every surface speaks the same names.
static const securacv::console::Knob SENSE_KNOBS[] = {
  {"debounce", "ms", "sustained target before 'present'",
   canary::cfg::SENSE_DEBOUNCE_MS_LO, canary::cfg::SENSE_DEBOUNCE_MS_HI,
   CS_PRESENT_DEBOUNCE_MS, kget_debounce, canary::cfg::sense_set_present_debounce_ms},
  {"clear", "ms", "no target before 'clear'",
   canary::cfg::SENSE_CLEAR_MS_LO, canary::cfg::SENSE_CLEAR_MS_HI,
   CS_CLEAR_TIMEOUT_MS, kget_clear, canary::cfg::sense_set_clear_timeout_ms},
  {"stall", "ms", "radar silence before 'unknown'",
   canary::cfg::SENSE_STALL_MS_LO, canary::cfg::SENSE_STALL_MS_HI,
   CS_RADAR_STALL_MS, kget_stall, canary::cfg::sense_set_stall_timeout_ms},
  {"near", "cm", "near band edge",
   canary::cfg::SENSE_NEAR_CM_LO, canary::cfg::SENSE_NEAR_CM_HI,
   CS_RANGE_NEAR_CM, kget_near, canary::cfg::sense_set_near_cm},
  {"mid", "cm", "mid band edge (beyond = far)",
   canary::cfg::SENSE_MID_CM_LO, canary::cfg::SENSE_MID_CM_HI,
   CS_RANGE_MID_CM, kget_mid, canary::cfg::sense_set_mid_cm},
#ifdef CANARY_SENSE_VITALS
  {"vlock", "ms", "sustained vitals before 'locked'",
   canary::cfg::SENSE_VLOCK_MS_LO, canary::cfg::SENSE_VLOCK_MS_HI,
   CS_VITALS_LOCK_MS, kget_vlock, canary::cfg::sense_set_vitals_lock_ms},
  {"vlost", "ms", "no vitals before 'lost'",
   canary::cfg::SENSE_VLOST_MS_LO, canary::cfg::SENSE_VLOST_MS_HI,
   CS_VITALS_LOST_MS, kget_vlost, canary::cfg::sense_set_vitals_lost_ms},
  {"breath_min", "bpm", "reject breathing below this",
   canary::cfg::SENSE_BREATH_MIN_LO, canary::cfg::SENSE_BREATH_MIN_HI,
   CS_BREATH_MIN_BPM, kget_bmin, canary::cfg::sense_set_breath_min_bpm},
  {"breath_max", "bpm", "reject breathing above this",
   canary::cfg::SENSE_BREATH_MAX_LO, canary::cfg::SENSE_BREATH_MAX_HI,
   CS_BREATH_MAX_BPM, kget_bmax, canary::cfg::sense_set_breath_max_bpm},
  {"heart_min", "bpm", "reject heart rate below this",
   canary::cfg::SENSE_HEART_MIN_LO, canary::cfg::SENSE_HEART_MIN_HI,
   CS_HEART_MIN_BPM, kget_hmin, canary::cfg::sense_set_heart_min_bpm},
  {"heart_max", "bpm", "reject heart rate above this",
   canary::cfg::SENSE_HEART_MAX_LO, canary::cfg::SENSE_HEART_MAX_HI,
   CS_HEART_MAX_BPM, kget_hmax, canary::cfg::sense_set_heart_max_bpm},
#endif
};

static securacv::console::TuningConsole g_console;

// Latest raw parser aggregate — only ever printed by the stream's opt-in
// `raw on` bench mode (see the privacy note in the file header).
static Frame g_last_raw;
static uint32_t g_last_stream_ms = 0;

// The periodic "what the radar sees" line. Coarse vocabulary by default
// (identical to what MQTT publishes); `raw on` appends the bench scalars.
static void tuning_stream_tick(uint32_t now) {
  const uint32_t period = g_console.stream_period_ms();
  if (!period) return;
  if ((int32_t)(now - g_last_stream_ms) < (int32_t)period) return;
  g_last_stream_ms = now;

  char line[192];
  size_t n = (size_t)snprintf(line, sizeof(line),
      "[radar] state=%s count=%s range=%s",
      presence_str(g_presence.state()), count_str(g_presence.count()),
      range_str(g_presence.range()));
#ifdef CANARY_SENSE_VITALS
  if (n < sizeof(line)) {
    const char* lock = "unknown";
    if (g_vitals.lock() == VitalsLock::Locked)    lock = "locked";
    else if (g_vitals.lock() == VitalsLock::Lost) lock = "lost";
    n += (size_t)snprintf(line + n, sizeof(line) - n,
        " lock=%s breath=%u heart=%u",
        lock, (unsigned)g_snap.breath_bpm, (unsigned)g_snap.heart_bpm);
  }
#endif
  if (g_console.raw_enabled() && n < sizeof(line)) {
    n += (size_t)snprintf(line + n, sizeof(line) - n,
        " raw_dist=%ucm raw_count=%u raw_breath=%u raw_heart=%u",
        (unsigned)g_last_raw.distance_cm, (unsigned)g_last_raw.target_count,
        (unsigned)g_last_raw.breath_rate, (unsigned)g_last_raw.heart_rate);
  }
  if (n < sizeof(line)) {
    snprintf(line + n, sizeof(line) - n, " errs=%lu",
             (unsigned long)g_parser.error_count());
  }
  boot_line(line);
}

// Drain typed bytes, apply any knob change to the live FSMs, keep the
// stream ticking. Runs before the network phase so the bench works with
// no WiFi, no broker, no HA.
static void tuning_console_tick(uint32_t now) {
  while (Serial.available() > 0) {
    g_console.feed((char)Serial.read());
  }
  if (g_console.take_changed()) {
    g_presence.reconfigure(make_presence_config(), now);
#ifdef CANARY_SENSE_VITALS
    g_vitals.reconfigure(make_vitals_config(), now);
#endif
    log_cfg_applied();
    // Keep HA's optimistic number entities honest when a bench and HA are
    // both attached; a no-op while the broker is away.
    canary::net::publish_sense_cfg_retained(TOPICS);
  }
  tuning_stream_tick(now);
}

// ----------------------------------------------------------------------------
// Publishing
// ----------------------------------------------------------------------------

static void set_last_event(const char* e) {
  strncpy(g_last_event, e ? e : "boot", sizeof(g_last_event) - 1);
  g_last_event[sizeof(g_last_event) - 1] = '\0';
  g_snap.last_event = g_last_event;
}

static void refresh_snapshot(uint32_t now_ms) {
  g_snap.presence  = presence_str(g_presence.state());
  g_snap.present   = (g_presence.state() == Presence::Present);
  g_snap.occupants = count_str(g_presence.count());
  g_snap.range     = range_str(g_presence.range());
  g_snap.radar_ok  = (g_presence.state() != Presence::Unknown);
  g_snap.frame_errors = g_parser.error_count();
  g_snap.uptime_s  = now_ms / 1000;
  g_snap.ts_ms     = now_ms;
}

static void publish_state_now(uint32_t now_ms) {
  refresh_snapshot(now_ms);
  canary::net::publish_state_retained(TOPICS, g_snap);
  g_state_dirty = false;
}

// Witness events: presence transitions and occupancy changes only, with the
// coarse bucket/band vocabulary. No distance, no vitals, ever. The only time
// field is uptime coarsened to the project's 10-minute buckets — a precise
// per-event timestamp would hand the event stream exact activity timing,
// against the metadata-minimization invariant; `seq` preserves ordering.
//
// Every witnessed transition advances the Ed25519-anchored hash chain and
// is signed over the v1 `sense` canonical (canary/witness.h) — with or
// without a broker. The MQTT publish (event + refreshed retained chain
// head) happens only while connected; an offline gap shows up as a jump
// in seq/chain length rather than lost tamper evidence.
static void record_event_now(const char* event_name, uint32_t now_ms) {
  static uint32_t fallback_seq = 0;
  refresh_snapshot(now_ms);

  const uint32_t bucket_uptime_s = (now_ms / 1000UL / 600UL) * 600UL;
  // seq rides the NVS-persisted chain length so it stays monotonic across
  // reboots (a keyless boot falls back to a session counter).
  const uint32_t seq = canary::witness::ready()
                           ? canary::witness::chain_length() + 1
                           : ++fallback_seq;

  // Chain first: the witness record exists regardless of connectivity.
  canary::witness::chain_advance(seq, event_name, g_snap.presence,
                                 g_snap.occupants, g_snap.range,
                                 bucket_uptime_s);

  if (!canary::net::mqtt_connected()) return;

  // Envelope max is 142 bytes incl. NUL; the headroom keeps a future
  // constant bump from silently publishing unsigned events through the
  // fail-closed truncation path.
  char sig_env[160] = "";
  const bool signed_ok = canary::witness::sign_event_envelope(
      seq, event_name, g_snap.presence, g_snap.occupants, g_snap.range,
      bucket_uptime_s, sig_env, sizeof(sig_env));

  char msg[640];
  const int n = snprintf(msg, sizeof(msg),
           "{"
           "\"device_id\":\"%s\","
           "\"device_type\":\"%s\","
           "\"event\":\"%s\","
           "\"seq\":%lu,"
           "\"bucket_uptime_s\":%lu,"
           "\"presence\":\"%s\","
           "\"occupants\":\"%s\","
           "\"range\":\"%s\","
           "\"signed\":%s"
           "%s"
           "}",
           canary::cfg::get().device_id, DEVICE_TYPE,
           event_name,
           (unsigned long)seq,
           (unsigned long)bucket_uptime_s,
           g_snap.presence,
           g_snap.occupants,
           g_snap.range,
           signed_ok ? "true" : "false",
           sig_env);
  if (n <= 0 || (size_t)n >= sizeof(msg)) return;

  canary::net::publish_event(TOPICS, msg);
  canary::net::publish_chain_retained(TOPICS);
}

// ----------------------------------------------------------------------------
// Drive the presence (and, when built, vitals) FSMs for one frame. Called once
// per decoded frame, and once with an empty frame when none arrived — the
// deadline check inside each tick() runs first, so a silent radar still
// advances the FSMs toward their stall deadlines.
// ----------------------------------------------------------------------------

static void drive_fsms(const Frame& frame, uint32_t now) {
  const PresenceEvent pev = g_presence.tick(frame, now);

  // One compact, machine-parseable line on any sensing change — the browser
  // flasher's live radar bench reads it off the attended USB console (the
  // same coarse fields MQTT publishes: state, count bucket, range band —
  // never raw centimeters). Cheap (change-gated) and greppable.
  {
    static Presence    last_state = Presence::Unknown;
    static CountBucket last_count = CountBucket::Zero;
    static RangeBand   last_range = RangeBand::Unknown;
    const RangeBand range_now = g_presence.range();
    if (pev.state_changed || pev.count_changed || range_now != last_range) {
      last_state = g_presence.state();
      last_count = g_presence.count();
      last_range = range_now;
      boot_linef("[sense] %s count=%s range=%s",
                 presence_str(last_state), count_str(last_count),
                 range_str(last_range));
    }
  }

  if (pev.state_changed) {
    led_for_presence(pev.state);
    const char* s = presence_str(pev.state);
    boot_linef("[presence] -> %s%s", s, pev.stalled ? " (radar stall)" : "");

    if (pev.state == Presence::Present) {
      set_last_event("presence_detected");
      record_event_now("presence_detected", now);
    } else if (pev.state == Presence::Clear) {
      set_last_event("presence_cleared");
      record_event_now("presence_cleared", now);
    }
    // Unknown (stall) is radar-link health, not a witness event: it surfaces
    // through the radar_link problem sensor + health heartbeat instead.
    g_state_dirty = true;
  }

  if (pev.count_changed) {
    // Occupancy bucket moved (0/1/2+). Only newsworthy while someone is here.
    if (g_presence.state() == Presence::Present) {
      set_last_event("occupancy_changed");
      record_event_now("occupancy_changed", now);
    }
    g_state_dirty = true;
  }

#ifdef CANARY_SENSE_VITALS
  // Vitals are suppressed unless exactly one target is present.
  const bool single_target = (pev.count == CountBucket::One);
  const auto vev = g_vitals.tick(frame, single_target, now);

  const bool locked = (g_vitals.lock() == VitalsLock::Locked);
  if (vev.lock_changed) {
    boot_linef("[vitals] breathing %s%s", locked ? "locked" : "lost",
               vev.stalled ? " (stall)" : "");
    g_state_dirty = true;
  }
  if (g_snap.breathing_locked != locked ||
      g_snap.bpm_valid != vev.bpm_valid ||
      (vev.bpm_valid && (g_snap.breath_bpm != vev.breath_bpm ||
                         g_snap.heart_bpm != vev.heart_bpm))) {
    g_snap.breathing_locked = locked;
    g_snap.bpm_valid  = vev.bpm_valid;
    g_snap.breath_bpm = vev.breath_bpm;
    g_snap.heart_bpm  = vev.heart_bpm;
    g_state_dirty = true;
    // The bench line for the wellbeing wow — rate-limited so a jittery lock
    // can't flood the console; same P1-gated numerics MQTT carries.
    static uint32_t last_bpm_line_ms = 0;
    if (vev.bpm_valid && (int32_t)(now - last_bpm_line_ms) >= 1000) {
      last_bpm_line_ms = now;
      boot_linef("[vitals] breath=%u heart=%u bpm",
                 (unsigned)vev.breath_bpm, (unsigned)vev.heart_bpm);
    }
  }
#endif
}

void setup() {
  Serial.begin(115200);
  delay(600);

  boot_set_output(sense_serial_write);

  // Privacy (Invariant III): never surface the raw MAC. The stable device
  // handle is the salted, MAC-free pseudonym shown as "Hardware ID" below;
  // mac_address is left null so the boot banner skips the MAC line.
  boot_info_t bi = {};
  bi.product_name  = "SecuraCV Canary Sense";
  bi.fw_version    = CANARY_FW_VERSION;
  bi.build_date    = __DATE__;
  bi.build_time    = __TIME__;
  bi.device_type   = CS_DEVICE_TYPE;
  bi.model         = CS_MODEL;
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

  // Radar-specific boot scene.
  boot_line("              .   .   .");
  boot_line("           .  ((( o )))  .      Who is in the room?");
  boot_line("              '   '   '");
  boot_separator();
  boot_kv("Sensor",  "MR60BHA2 60GHz FMCW radar (UART)");
  boot_kvf("Radar",  "UART%d  TX=%d RX=%d  @ %lu 8N1",
           RADAR_UART_NUM, RADAR_UART_TX, RADAR_UART_RX,
           (unsigned long)RADAR_UART_BAUD);
  boot_kvf("Lux",    "BH1750 I2C  SDA=%d SCL=%d  addr 0x%02X",
           I2C_PIN_SDA, I2C_PIN_SCL, BH1750_I2C_ADDR);
  boot_kvf("LED",    "WS2812 on GPIO%d", LED_WS2812_PIN);
#if defined(FEATURE_VITALS) && FEATURE_VITALS
  boot_kv("Vitals",  "ENABLED (P1-gated wellbeing channel)");
#else
  boot_kv("Vitals",  "disabled (presence-only build)");
#endif
  boot_kvf("Present", "%lu ms debounce, %lu ms clear, %lu ms stall",
           (unsigned long)canary::cfg::sense().present_debounce_ms,
           (unsigned long)canary::cfg::sense().clear_timeout_ms,
           (unsigned long)canary::cfg::sense().stall_timeout_ms);
  boot_kv("Tuning", "serial console ready — type 'help' (115200 8N1)");
  boot_blank();

  // Serial tuning console: every knob above is live from this moment —
  // before WiFi, before the broker — so the flasher's bench can start
  // tuning the instant the flash finishes.
  g_console.begin(SENSE_KNOBS, sizeof(SENSE_KNOBS) / sizeof(SENSE_KNOBS[0]),
                  sense_serial_write);

  // Bring up the radar UART (host TX16 / RX17 per the kit reference wiring).
  RadarSerial.begin(RADAR_UART_BAUD, SERIAL_8N1, RADAR_UART_RX, RADAR_UART_TX);

  const uint32_t boot_ms = millis();
  g_parser.reset();
  g_presence.reconfigure(make_presence_config(), boot_ms);
#ifdef CANARY_SENSE_VITALS
  g_vitals.reconfigure(make_vitals_config(), boot_ms);
#endif
  g_last_heartbeat_ms = boot_ms;

  led_for_presence(Presence::Unknown);

#if defined(FEATURE_AMBIENT_LIGHT) && FEATURE_AMBIENT_LIGHT
  Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL, I2C_FREQ_DEFAULT);
  if (g_lux.begin(Wire, BH1750_I2C_ADDR)) {
    boot_kv("BH1750", "ambient light sensor online");
  } else {
    boot_kv("BH1750", "not found — illuminance disabled");
  }
#endif

  TOPICS = build_topics(canary::cfg::get().device_id);

  // Witness identity + hash chain (NVS-backed Ed25519; see witness.h).
  // Before the network: a keyless boot must be loud in the boot log, and
  // the chain must be ready before the first presence transition.
  canary::witness::init();

  // The idle poll keeps the tuning console answering (and its [radar]
  // stream ticking) through the blocking boot connect — without it, a
  // bench board with no provisioned WiFi would spin here for the boot
  // timeout and reboot, starving loop() so the console never speaks.
  canary::net::wifi_init_or_reboot([]() {
    tuning_console_tick(canary::ms_now());
  });

  // Fleet LAN presence: the _securacv._tcp advert (device_id/name/host/
  // fw/model/dt/role TXT) that lets the companion app, canary-display,
  // and sibling Canaries find and label this witness. Failure is
  // non-fatal — MQTT/HA never depends on it.
  canary::net::mdns_init();

#if defined(FEATURE_FLEET_BEACON) && FEATURE_FLEET_BEACON
  // Fleet-link BLE presence beacon: WiFi STA is up (they coexist on the C6
  // shared radio) and the witness fingerprint was established above, so a
  // canary-display can find this witness directly over BLE — broker-free and
  // WiFi-free. Fail-safe: a stack that can't come up degrades to a no-op.
  canary::net::fleet_beacon_begin(canary::ms_now());
#endif

  // Seed the heap-health snapshot so the first status publish carries real
  // numbers instead of zeros.
  canary::diag::loop(canary::ms_now());

  // Confirm (or roll back) a freshly installed image now — before anything
  // that can block on external services. See ota_mgr.h.
  canary::net::ota_boot_validate();

  canary::net::mqtt_init(TOPICS);

  // MQTT connection scene.
  boot_line("              .   .   .  ))");
  boot_line("           .  ((( o )))  ))     Connecting to MQTT...");
  boot_line("              '   '   '");
  boot_separator();
  boot_kv("Device ID", canary::cfg::get().device_id);
  char devid_hex[device_pseudonym::HEX_LEN + 1] = {0};
  if (device_pseudonym::device_id_hex(devid_hex, sizeof(devid_hex))) {
    boot_kv("Hardware ID", devid_hex);  // salted pseudonym, not the raw MAC
  }
  boot_kvf("Heartbeat", "every %lu ms", (unsigned long)HEARTBEAT_MS);
  boot_kv("HA prefix", HA_DISCOVERY_PREFIX);
  boot_blank();

  // ONE bounded connect attempt (it publishes status + HA discovery on
  // success). A broker that is down at boot must not block the witness:
  // sensing starts regardless and loop()'s backoff supervisor keeps trying.
  if (!mqtt_supervise(canary::ms_now())) {
    canary::log_line("MQTT", "Broker unreachable — sensing starts anyway; retrying in loop().");
  }

  // Signed pull-OTA: arm the engine (validation already ran right after
  // WiFi). Daily jittered checks; HA's Install button and auto-update
  // switch are drained in loop().
  canary::net::ota_init(TOPICS);

  set_last_event("boot");
  publish_state_now(canary::ms_now());

#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
  // Task watchdog — same IDF5 config canary-wap uses, armed LAST so the
  // blocking boot phases above (WiFi connect up to 30 s) can't trip it.
  // The timeout must exceed loop()'s worst bounded block (one MQTT
  // connect attempt against a dead broker).
  {
    esp_task_wdt_config_t wdt_config = {
      .timeout_ms = CS_WATCHDOG_TIMEOUT_SEC * 1000,
      .idle_core_mask = (1 << 0),   // ESP32-C6: single core
      .trigger_panic = true
    };
    esp_err_t wdt_err = esp_task_wdt_reconfigure(&wdt_config);
    if (wdt_err == ESP_ERR_INVALID_STATE) {
      esp_task_wdt_init(&wdt_config);
    }
    esp_task_wdt_add(NULL);
    boot_kvf("Watchdog", "%lu s timeout", (unsigned long)CS_WATCHDOG_TIMEOUT_SEC);
  }
#endif

  boot_scene_ready(
      "It will witness presence over 60GHz radar",
      "and publish signed coarse claims via MQTT to Home Assistant.",
      NULL);
}

void loop() {
#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
  esp_task_wdt_reset();
#endif

  const uint32_t now = canary::ms_now();

  // ── Sensing first: the witness keeps observing with or without a network ──
  while (RadarSerial.available() > 0) {
    g_parser.push((uint8_t)RadarSerial.read());
  }

  // Drain every frame the parser decoded this loop, advancing the FSMs for
  // each. If none arrived, tick once with an empty frame so the deadline
  // checks still run — a silent radar drives presence to Unknown (and, when
  // built, vitals to Lost) instead of freezing on the last good frame.
  bool any_frame = false;
  for (Frame frame = g_parser.poll(); frame.kind != securacv::mmwave::FrameKind::None;
       frame = g_parser.poll()) {
    g_last_raw = frame;  // bench-only echo; see the raw-mode privacy note
    drive_fsms(frame, now);
    any_frame = true;
  }
  if (!any_frame) {
    drive_fsms(Frame(), now);
  }

  // Serial tuning console + [radar] stream — before the network phase, so
  // the post-flash bench works with no WiFi, no broker, no HA.
  tuning_console_tick(now);

#if defined(FEATURE_AMBIENT_LIGHT) && FEATURE_AMBIENT_LIGHT
  if (g_lux.present() && (int32_t)(now - g_last_lux_ms) >= (int32_t)LUX_SAMPLE_MS) {
    g_last_lux_ms = now;
    const float lux = g_lux.read_lux();
    // Publish on meaningful change only (>= 5 lx or presence of a reading
    // where there was none) — the lux channel corroborates tamper, it isn't
    // a light meter.
    if (lux >= 0) {
      if (g_snap.lux < 0 || fabsf(lux - g_snap.lux) >= 5.0f) {
        g_snap.lux = lux;
        g_state_dirty = true;
      }
    } else if (g_snap.lux >= 0) {
      // Read failure on a tamper-corroboration channel must be visible:
      // revert to null in HA rather than freezing the last good reading.
      g_snap.lux = -1.0f;
      g_state_dirty = true;
    }
  }
#endif

  // Identify blink window: drive it every pass, broker or no broker —
  // the LED must keep flashing through a mid-identify outage.
  identify_tick(now);

  // ── Network supervision ──
  canary::net::wifi_loop(now);
  canary::net::mdns_loop(now);  // drain deferred re-announce (event task latches only)
  canary::diag::loop(now);

#if defined(FEATURE_FLEET_BEACON) && FEATURE_FLEET_BEACON
  // Refresh the BLE presence beacon every pass (internally rate-limited to
  // ~5 s). Placed before the broker early-return below so it keeps advertising
  // through an MQTT outage — that broker-free reach is the point.
  canary::net::fleet_beacon_tick(now);
#endif

#if defined(FEATURE_FLEET_ROSTER) && FEATURE_FLEET_ROSTER
  // Low-duty passive scan that hears the OTHER Canaries and keeps this
  // witness's own fleet roster (last-heartbeat + status). Broker-independent
  // like the beacon; also before the broker early-return so it keeps tracking
  // peers through an MQTT/WiFi outage (continuous scan when fully off-grid).
  // wifi_provisioned: real state now that an unseeded board raises the shared
  // setup portal instead of joining a placeholder forever. While the portal
  // owns the radio (SoftAP + candidate joins) the scan must treat it as
  // contended, exactly like an in-flight join — hence the OR.
  canary::net::fleet_roster_scan_tick(
      now, canary::net::wifi_connected(),
      /*wifi_provisioned=*/canary::net::wifi_configured() ||
          canary::net::setup_portal_active());
#endif

  // Bounded, backoff-scheduled broker supervision: while the broker is
  // unreachable the witness keeps sensing (we already drove the FSMs above)
  // and simply skips the publish phase.
  if (!mqtt_supervise(now)) {
    delay(5);
    return;
  }

  canary::net::mqtt_loop();
  canary::net::ota_loop(now);

  // Identify: drain the button press and open the blink window.
  if (canary::net::take_pending_identify()) identify_start(now);

  // Runtime radar reflexes: drain HA's number writes, apply through the
  // clamping setters, and — on any actual change — swap the new thresholds
  // into the live FSMs. State resets there by design (deadlines measured
  // under the old config would be misleading) and rebuilds within one
  // debounce window. Republish on every inbound command, even a clamped-to-
  // same one, so HA's optimistic number display self-corrects.
  poll_sense_cfg_commands(now);

  if (g_state_dirty) {
    publish_state_now(now);
  }

  // Periodic health refresh (retained pubkey/uptime/heap for HA's trust
  // store and diagnostics).
  if ((int32_t)(now - g_last_health_ms) >= (int32_t)HEALTH_PUBLISH_MS) {
    g_last_health_ms = now;
    canary::net::publish_health_retained(TOPICS);
  }

  // Health heartbeat. Under heap pressure the diagnostics ladder stretches
  // the cadence (2x at critical, 5x at emergency).
  const uint32_t heartbeat_ms = HEARTBEAT_MS * canary::diag::period_scale();
  if ((int32_t)(now - g_last_heartbeat_ms) >= (int32_t)heartbeat_ms) {
    g_last_heartbeat_ms = now;
    refresh_snapshot(now);
    canary::net::publish_heartbeat(TOPICS, g_snap);
    publish_state_now(now);
    boot_linef("[health] up %lus  heap %luKB  frame_errs %lu",
               (unsigned long)(now / 1000),
               (unsigned long)(ESP.getFreeHeap() / 1024),
               (unsigned long)g_parser.error_count());
  }

  delay(5);
}
