/*
  SecuraCV Canary Sentinel — Multi-Sensor Fusion Guardian (Phase 0)
  ----------------------------------------------------------------------
  (c) 2026 Errer Labs / SecuraCV
  errerlabs.com | securacv.com
  GitHub: https://github.com/kmay89/securaCV

  License: Apache-2.0 (repository license).

  WHAT THIS IS. The doorway/window guardian: it fuses physically INDEPENDENT
  sensing channels (PIR heat, 60GHz radar, WiFi CSI, WiFi/BLE device counting,
  ambient light, and — Heavy tier — door-contact/tamper/vision) into one
  debounced, privacy-preserving people-detection decision. The decision core is
  the board-agnostic, host-tested securacv::fusion engine
  (firmware/common/fusion); this file is the composition layer that reads the
  sensors this board actually has, adapts each to a coarse Vote, feeds the
  engine, and (Phase 1) signs + publishes the coarse result.

  PHASE STATUS. Phase 0 composes the sensing + fusion core and emits coarse
  transitions to the console — the novel part, fully host-tested. The signed
  witness + MQTT/HA + pull-OTA network path is the same proven stack as
  canary-sense and is wired in Phase 1 (see the project README checklist); its
  call site is emit_claim() below. Nothing here fabricates a network it cannot
  yet stand up — it degrades honestly (requirement R6). The onboard-radio
  channels (WiFi-RF/CSI, BLE) are likewise Phase-1 wiring; their adapter call
  sites are shown so the composition is complete and reviewable.

  PRIVACY CHOKEPOINT (design doc §5, requirement R4). Only the coarse
  FusionResult ever leaves this file: an ordinal level, a 0..100 confidence, a
  0/1/2+ occupant bucket, a near/mid/far band, and which modality CLASSES
  corroborated. Raw centimeters, per-target data, MACs and imagery are read,
  used to form a Vote, and dropped here. They never cross emit_claim().
*/

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

// Board pin map (boards/<board>/pins via -I). Pin numbers ONLY here.
#include "pins.h"

// Project composition: active preset (SENT_* + FEATURE_*) + housekeeping consts.
#include "canary/config.h"
#include "canary/sentinel_requirements.h"

// Board-agnostic fusion brain + channel adapters (firmware/common via -I).
#include "canary/sentinel_config.h"        // build_fusion_config() from the preset
#include "fusion/sentinel_fusion.h"
#include "fusion/sentinel_channels.h"

// Concrete drivers for the always-cheap channels (same as canary-sense).
#if defined(FEATURE_MMWAVE_RADAR) && FEATURE_MMWAVE_RADAR
#include "sensors/mmwave_mr60/mr60_uart.h"
#endif
#if defined(FEATURE_AMBIENT_LIGHT) && FEATURE_AMBIENT_LIGHT
#include "sensors/bh1750/bh1750.h"
#endif
#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
#include <esp_task_wdt.h>
#endif

using namespace securacv::fusion;
namespace chan = securacv::fusion::channels;

// ----------------------------------------------------------------------------
// Module instances
// ----------------------------------------------------------------------------
static FusionEngine g_engine;   // configured from the preset in setup()

#if defined(FEATURE_MMWAVE_RADAR) && FEATURE_MMWAVE_RADAR
static HardwareSerial RadarSerial(RADAR_UART_NUM);
static securacv::mmwave::FrameParser g_radar_parser;
static bool     g_radar_seen = false;
static uint32_t g_last_radar_frame_ms = 0;
#endif
#if defined(FEATURE_AMBIENT_LIGHT) && FEATURE_AMBIENT_LIGHT
static securacv::sensors::BH1750 g_lux;
static float g_lux_baseline = -1.0f;
#endif

// PIR settle tail: how long after the last motion edge PIR keeps voting Weak.
static constexpr uint32_t PIR_SETTLE_MS = 1500;
static uint32_t g_pir_last_edge_ms = 0;

// ----------------------------------------------------------------------------
// The privacy chokepoint: the ONLY function allowed to externalise a decision.
// Phase 0 prints the coarse result; Phase 1 signs it over the `sentinel` v1
// canonical (common/identity) and publishes retained MQTT + HA discovery, the
// same path canary-sense proves. Everything it receives is already coarse.
// ----------------------------------------------------------------------------
static void emit_claim(const FusionResult& r) {
  Serial.printf("[sentinel] level=%s conf=%u anomaly=%u occ=%s range=%s "
                "modalities=%u/0x%02x%s\n",
                level_name(r.level), r.confidence, r.anomaly,
                occupancy_name(r.occupancy), range_band_name(r.range),
                r.strong_modalities, r.modality_bits,
                r.denied_any ? " DENIED" : "");
  // Phase 1: witness_sign(sentinel_canonical(r)); mqtt_publish(events/state);
}

// ----------------------------------------------------------------------------
// Per-channel reads -> Vote -> engine.observe(). Each is compiled in only when
// its FEATURE_* flag is set, so a Lite board never references a radar it lacks.
// A channel only re-observes on its own cadence; between observations its last
// vote decays via the engine's per-channel stale_ms.
// ----------------------------------------------------------------------------
static void read_pir(uint32_t now) {
#if defined(FEATURE_PIR) && FEATURE_PIR
  const bool motion = digitalRead(PIR_PIN) == PIR_ACTIVE_LEVEL;
  if (motion) g_pir_last_edge_ms = now;
  const uint32_t since = now - g_pir_last_edge_ms;
  g_engine.observe(Channel::Pir, chan::pir_vote(motion, since, PIR_SETTLE_MS),
                   /*quality=*/95, now);
#else
  (void)now;
#endif
}

static void read_radar(uint32_t now) {
#if defined(FEATURE_MMWAVE_RADAR) && FEATURE_MMWAVE_RADAR
  using securacv::mmwave::Frame;
  using securacv::mmwave::FrameKind;

  while (RadarSerial.available()) {
    g_radar_parser.push(static_cast<uint8_t>(RadarSerial.read()));
  }

  bool got = false, present_now = false;
  for (Frame f = g_radar_parser.poll(); f.kind != FrameKind::None;
       f = g_radar_parser.poll()) {
    got = true;
    g_radar_seen = true;
    g_last_radar_frame_ms = now;
    if (f.kind == FrameKind::Presence) {
      present_now = f.has_target;
      // Coarse side-bands consumed here; the raw cm/count never leave this file.
      g_engine.set_range(chan::range_from_cm(f.distance_cm, SENT_RANGE_NEAR_CM,
                                             SENT_RANGE_MID_CM));
      g_engine.set_occupancy(chan::occupancy_from_count(f.target_count));
    }
  }

  const bool stalled = g_radar_seen && (now - g_last_radar_frame_ms) > SENT_STALE_RADAR;
  if (got) {
    g_engine.observe(Channel::Radar, chan::radar_vote(false, present_now, false),
                     /*quality=*/95, now);
  } else if (stalled) {
    // The radar UART should be speaking and isn't — evasion or fault: Denied.
    g_engine.observe(Channel::Radar, Vote::Denied, /*quality=*/100, now);
  }
  // else: no complete frame this loop and not yet stalled — leave the last
  // radar vote to decay naturally via the engine's stale window.
#else
  (void)now;
#endif
}

static void read_light(uint32_t now) {
#if defined(FEATURE_AMBIENT_LIGHT) && FEATURE_AMBIENT_LIGHT
  const float lux = g_lux.read_lux();
  if (lux < 0.0f) {
    // Sensor absent / read failed / saturated-dark: treat as blinded.
    g_engine.observe(Channel::Light, Vote::Denied, /*quality=*/100, now);
    return;
  }
  if (g_lux_baseline < 0.0f) g_lux_baseline = lux;
  const uint16_t delta = static_cast<uint16_t>(fabsf(lux - g_lux_baseline));
  // Slow-track the baseline so a sunset drift doesn't read as an event.
  g_lux_baseline += (lux - g_lux_baseline) * 0.02f;
  g_engine.observe(Channel::Light,
                   chan::light_vote(/*blinded=*/false, delta, /*weak_delta=*/50),
                   /*quality=*/80, now);
#else
  (void)now;
#endif
}

// WiFi-RF, BLE and CSI ride the onboard radio and share the same
// privacy-preserving, MAC-free counting/feature paths canary-wap proves
// (canary-wap's rf_presence, common/bluetooth, common/csi). Their scan/feature
// callbacks call the adapters below; the wiring is Phase 1 (see README). The
// call sites are shown so the composition is complete and reviewable:
//
//   g_engine.observe(Channel::WifiRf, chan::count_vote(wifi_devs, 1, 3), 100, now);
//   g_engine.observe(Channel::Ble,    chan::count_vote(ble_devs, 1, 3),  100, now);
//   g_engine.observe(Channel::WifiCsi, chan::csi_vote(confirmed, observed), 85, now);

// ----------------------------------------------------------------------------
// Arduino entry points
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.printf("\n[sentinel] %s\n[sentinel] tier=%s device=%s\n",
                MODEL, TIER, DEVICE_ID);

  g_engine.configure(canary::build_fusion_config());

#if defined(FEATURE_PIR) && FEATURE_PIR
  pinMode(PIR_PIN, PIR_INPUT_MODE);
#endif
#if defined(FEATURE_MMWAVE_RADAR) && FEATURE_MMWAVE_RADAR
  RadarSerial.begin(RADAR_UART_BAUD, SERIAL_8N1, RADAR_UART_RX, RADAR_UART_TX);
  g_last_radar_frame_ms = millis();
#endif
#if defined(FEATURE_AMBIENT_LIGHT) && FEATURE_AMBIENT_LIGHT
  Wire.begin(I2C_SDA, I2C_SCL);
  g_lux.begin(Wire, BH1750_ADDR);
#endif
#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
  esp_task_wdt_init(WATCHDOG_TIMEOUT_SEC, /*panic=*/true);
  esp_task_wdt_add(nullptr);
#endif

  Serial.println("[sentinel] fusion armed");
}

void loop() {
  const uint32_t now = millis();
  static uint32_t next_tick = 0;
  static uint32_t next_light = 0;

  // Radar/PIR are cheap and edge-sensitive: sample every loop.
  read_pir(now);
  read_radar(now);
  if (now >= next_light) { read_light(now); next_light = now + LIGHT_SAMPLE_MS; }

  if (now >= next_tick) {
    next_tick = now + FUSION_TICK_MS;
    const FusionResult r = g_engine.evaluate(now);
    if (r.changed) emit_claim(r);
  }

#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
  esp_task_wdt_reset();
#endif
}
