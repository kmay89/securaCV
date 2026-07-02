/*
  SecuraCV Canary Sense — 60GHz mmWave Radar Witness Firmware (Phase 0)
  --------------------------------------------------------------------
  (c) 2026 Errer Labs / SecuraCV
  errerlabs.com | securacv.com
  GitHub: https://github.com/kmay89/securaCV

  License: Apache-2.0 (use repository license unless otherwise specified).

  PHASE 0 "hello-witness" skeleton. Its job is to prove the ESP32-C6 toolchain
  (pioarduino core 3.x) and the board/config/common layering compile and link,
  BEFORE the radar kit arrives. It brings up pins + the radar UART, runs the
  presence FSM against whatever frames the (stubbed) parser yields, drives the
  WS2812 status LED, and prints a health heartbeat. Network / witness chain /
  OTA are wired in Phase 2 (see docs/canary_sense_mr60bha2_design.md §3), after
  the core 2.x/3.x audit of the common/ modules we plan to link.
*/

#include <Arduino.h>

// Board pin map (boards/xiao-esp32c6-mr60/pins via -I). Pin numbers ONLY here.
#include "pins.h"

// Build-time configuration (configs/canary-sense/<flavor> via -I). Flags ONLY.
#include "config.h"

// Shared, board-agnostic modules (reached via -I .../common).
#include "boot/boot_banner.h"
#include "sensors/mmwave_mr60/mr60_uart.h"
#include "sensors/mmwave_mr60/mr60_presence.h"
#ifdef CANARY_SENSE_VITALS
#include "sensors/mmwave_mr60/mr60_vitals.h"
#endif

using securacv::mmwave::Frame;
using securacv::mmwave::FrameParser;
using securacv::mmwave::Presence;
using securacv::mmwave::PresenceConfig;
using securacv::mmwave::PresenceEvent;
using securacv::mmwave::PresenceFSM;

// ----------------------------------------------------------------------------
// Module instances
// ----------------------------------------------------------------------------

// Radar link: UART1 on the host (UART0 stays on the USB-CDC console).
static HardwareSerial RadarSerial(RADAR_UART_NUM);
static FrameParser g_parser;

static PresenceConfig make_presence_config() {
  PresenceConfig c;
  c.present_debounce_ms = CS_PRESENT_DEBOUNCE_MS;
  c.clear_timeout_ms    = CS_CLEAR_TIMEOUT_MS;
  c.stall_timeout_ms    = CS_RADAR_STALL_MS;
  c.near_cm             = CS_RANGE_NEAR_CM;
  c.mid_cm              = CS_RANGE_MID_CM;
  return c;
}

static PresenceFSM g_presence(make_presence_config());

#ifdef CANARY_SENSE_VITALS
using securacv::mmwave::VitalsConfig;
using securacv::mmwave::VitalsFSM;

static VitalsConfig make_vitals_config() {
  VitalsConfig c;
  c.lock_confirm_ms = CS_VITALS_LOCK_MS;
  c.lock_lost_ms    = CS_VITALS_LOST_MS;
  c.breath_min_bpm  = CS_BREATH_MIN_BPM;
  c.breath_max_bpm  = CS_BREATH_MAX_BPM;
  c.heart_min_bpm   = CS_HEART_MIN_BPM;
  c.heart_max_bpm   = CS_HEART_MAX_BPM;
  return c;
}

static VitalsFSM g_vitals(make_vitals_config());
#endif

static uint32_t g_last_heartbeat_ms = 0;

// ----------------------------------------------------------------------------
// Status LED (WS2812). Uses the Arduino-ESP32 RMT helper so we pull in no extra
// library for a single pixel. Colour encodes presence state at a glance.
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

static void led_for_presence(Presence p) {
  switch (p) {
    case Presence::Present: led_show(0, 24, 0);  break;  // green: someone here
    case Presence::Clear:   led_show(0, 0, 16);  break;  // blue: clear
    case Presence::Unknown:                              // fallthrough
    default:                led_show(24, 8, 0);  break;  // amber: no radar data
  }
}

// ----------------------------------------------------------------------------
// Serial boot output redirect (USB-CDC console).
// ----------------------------------------------------------------------------

static void sense_serial_write(const char* str) {
  Serial.print(str);
}

// ----------------------------------------------------------------------------
// Drive the presence (and, when built, vitals) FSMs for one frame. Called once
// per decoded frame, and once with an empty frame when none arrived — the
// deadline check inside each tick() runs first, so a silent radar still
// advances the FSMs toward their stall/lost states.
// ----------------------------------------------------------------------------

static void drive_fsms(const Frame& frame, uint32_t now) {
  const PresenceEvent pev = g_presence.tick(frame, now);
  if (pev.state_changed) {
    led_for_presence(pev.state);
    const char* s = (pev.state == Presence::Present) ? "present"
                  : (pev.state == Presence::Clear)   ? "clear"
                                                     : "unknown";
    boot_linef("[presence] -> %s%s", s, pev.stalled ? " (radar stall)" : "");
  }

#ifdef CANARY_SENSE_VITALS
  // Vitals are suppressed unless exactly one target is present.
  const bool single_target =
      (pev.count == securacv::mmwave::CountBucket::One);
  const auto vev = g_vitals.tick(frame, single_target, now);
  if (vev.lock_changed) {
    const char* l = (vev.lock == securacv::mmwave::VitalsLock::Locked) ? "locked"
                  : "lost";
    boot_linef("[vitals] breathing %s%s", l, vev.stalled ? " (stall)" : "");
  }
#endif
}

void setup() {
  Serial.begin(115200);
  delay(600);

  boot_set_output(sense_serial_write);

  boot_info_t bi = {};
  bi.product_name  = "SecuraCV Canary Sense";
  bi.fw_version    = "0.0.0-phase0";
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
           (unsigned long)CS_PRESENT_DEBOUNCE_MS,
           (unsigned long)CS_CLEAR_TIMEOUT_MS,
           (unsigned long)CS_RADAR_STALL_MS);
  boot_blank();

  // Bring up the radar UART (host TX16 / RX17 per the kit reference wiring).
  RadarSerial.begin(RADAR_UART_BAUD, SERIAL_8N1, RADAR_UART_RX, RADAR_UART_TX);

  const uint32_t now = millis();
  g_parser.reset();
  g_presence.reset(now);
#ifdef CANARY_SENSE_VITALS
  g_vitals.reset(now);
#endif
  g_last_heartbeat_ms = now;

  led_for_presence(Presence::Unknown);

  boot_scene_ready(
      "It will witness presence over 60GHz radar",
      "and publish signed claims via MQTT (Phase 2).",
      NULL);
}

void loop() {
  const uint32_t now = millis();

  // Pump any received radar bytes into the frame parser.
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
    drive_fsms(frame, now);
    any_frame = true;
  }
  if (!any_frame) {
    drive_fsms(Frame(), now);
  }

  // Health heartbeat (HEALTH_CAT_SENSOR territory in Phase 2; a serial line for
  // now so CI/bench can see the loop is alive).
  if ((int32_t)(now - g_last_heartbeat_ms) >= (int32_t)CS_HEARTBEAT_MS) {
    g_last_heartbeat_ms = now;
    boot_linef("[health] up %lus  heap %luKB  frame_errs %lu",
               (unsigned long)(now / 1000),
               (unsigned long)(ESP.getFreeHeap() / 1024),
               (unsigned long)g_parser.error_count());
  }

  delay(5);
}
