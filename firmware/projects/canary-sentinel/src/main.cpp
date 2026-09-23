/*
  SecuraCV Canary Sentinel — Multi-Sensor Fusion Guardian (Phase 1a)
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
  engine, and signs + publishes the coarse result.

  PHASE STATUS. Phase 0 composed the sensing + fusion core (host-tested).
  Phase 1a (this file) wires the network/witness path — canary-sense's stack,
  carried in src/net + src/runtime_config.cpp + src/diagnostics.cpp and pinned
  to canary-sense by firmware/scripts/check_sentinel_net_sync.sh: NVS-backed
  runtime config, supervised WiFi STA with the shared setup portal, MQTT with
  LWT + Home Assistant discovery, the Ed25519 witness chain over the
  `sentinel` v1 canonical, signed pull-OTA, the _securacv._tcp mDNS advert
  and heap-health diagnostics. It is compile-gated in CI (firmware.yml's
  PlatformIO leg) and NOT released: nothing here has run on a bench yet (the
  project README's checklist). Phase 1b — the onboard-radio channels
  (WiFi-RF, WiFi-CSI, BLE) — is bench-bound and NOT built; their adapter call
  sites below stay comments. Nothing here fabricates a channel it does not
  have: a channel that is not wired simply never votes (requirement R6).

  PRIVACY CHOKEPOINT (design doc §5, requirement R4). Only the coarse
  FusionResult ever leaves this file: an ordinal level, a 0..100 confidence, a
  0..100 anomaly score, a 0/1/2+ occupant bucket, a near/mid/far band, and
  which modality CLASSES corroborated. Raw centimeters, per-target data, MACs
  and imagery are read, used to form a Vote, and dropped here. They never
  cross emit_claim().
*/

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <esp_random.h>  // esp_random() for reconnect jitter

// Board pin map (boards/<board>/pins via -I). Pin numbers ONLY here.
#include "pins.h"

// Project composition: active preset (SENT_* + FEATURE_*) + housekeeping and
// network constants.
#include "canary/config.h"
#include "canary/sentinel_requirements.h"
#include "canary/version.h"
#include "canary/log.h"
#include "canary/topics.h"
#include "canary/types.h"
#include "canary/runtime_config.h"
#include "canary/diagnostics.h"
#include "canary/witness.h"
#include "canary/net/wifi_mgr.h"
#include "canary/net/mdns_mgr.h"
#include "canary/net/mqtt_mgr.h"
#include "canary/net/ota_mgr.h"

// Board-agnostic fusion brain + channel adapters (firmware/common via -I).
#include "canary/sentinel_config.h"        // build_fusion_config() from the preset
#include "fusion/sentinel_fusion.h"
#include "fusion/sentinel_channels.h"

// Shared, board-agnostic modules (reached via -I .../common).
#include "boot/boot_banner.h"
#include "identity/device_pseudonym.h"  // salted, MAC-free device handle (Invariant III)

// Concrete drivers for the always-cheap channels (same as canary-sense).
#if defined(FEATURE_MMWAVE_RADAR) && FEATURE_MMWAVE_RADAR
#include "sensors/mmwave_mr60/mr60_uart.h"
#endif
#if defined(FEATURE_AMBIENT_LIGHT) && FEATURE_AMBIENT_LIGHT
#include "sensors/bh1750/bh1750.h"
#endif
#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
#include <esp_idf_version.h>
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

static Topics TOPICS;
static SentinelSnapshot g_snap;
static char g_last_event[32] = "boot";
static char g_modalities[80] = "none";
static bool g_state_dirty = false;
static uint32_t g_last_heartbeat_ms = 0;
static uint32_t g_last_health_ms = 0;

// Broker reconnect schedule (canary-sense's supervisor): a broker outage must
// never pin the loop — the guardian keeps sensing and each bounded connect
// attempt happens at most once per backoff window.
static uint32_t g_mqtt_next_attempt_ms = 0;
static uint32_t g_mqtt_attempts = 0;

// Broker gossip bookkeeping: advertise the working broker on the fleet
// advert while the link is up, tombstone it the moment it drops
// (ground-truth-only referral — same contract as canary-sense).
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
// The privacy chokepoint: FusionResult -> the coarse wire vocabulary. This is
// the FULL vocabulary that ever leaves the device about what it sensed.
// ----------------------------------------------------------------------------

// Modality CLASS names for the published bitmask, in securacv::fusion::
// Modality bit order — classes, never channels, devices or addresses.
static const char* const MODALITY_NAMES[] = {
  "thermal", "radio_reflection", "channel_perturb",
  "carried_radio", "optical", "mechanical",
};
static_assert(sizeof(MODALITY_NAMES) / sizeof(MODALITY_NAMES[0]) ==
                  static_cast<size_t>(Modality::kCount),
              "one wire name per fusion modality class");

static const char* modality_list(uint8_t bits) {
  size_t n = 0;
  g_modalities[0] = '\0';
  for (size_t i = 0; i < static_cast<size_t>(Modality::kCount); i++) {
    if (!(bits & (1u << i))) continue;
    const int w = snprintf(g_modalities + n, sizeof(g_modalities) - n, "%s%s",
                           n ? "," : "", MODALITY_NAMES[i]);
    if (w < 0 || (size_t)w >= sizeof(g_modalities) - n) break;
    n += (size_t)w;
  }
  if (n == 0) snprintf(g_modalities, sizeof(g_modalities), "none");
  return g_modalities;
}

static void set_last_event(const char* e) {
  strncpy(g_last_event, e ? e : "boot", sizeof(g_last_event) - 1);
  g_last_event[sizeof(g_last_event) - 1] = '\0';
  g_snap.last_event = g_last_event;
}

static void refresh_snapshot(const FusionResult& r, uint32_t now_ms) {
  g_snap.claim.level         = level_name(r.level);
  g_snap.claim.confidence    = r.confidence;
  g_snap.claim.anomaly       = r.anomaly;
  g_snap.claim.occupancy     = occupancy_name(r.occupancy);
  g_snap.claim.range         = range_band_name(r.range);
  g_snap.claim.modality_bits = r.modality_bits;
  g_snap.present   = (r.level == Level::Present || r.level == Level::Confirmed ||
                      r.level == Level::Loiter);
  g_snap.anomalous = (r.level == Level::Anomaly);
  g_snap.denied_any        = r.denied_any;
  g_snap.strong_modalities = r.strong_modalities;
  g_snap.modalities = modality_list(r.modality_bits);
  g_snap.uptime_s = now_ms / 1000;
  g_snap.ts_ms    = now_ms;
}

static void publish_state_now(uint32_t now_ms) {
  refresh_snapshot(g_engine.last(), now_ms);
  canary::net::publish_state_retained(TOPICS, g_snap);
  g_state_dirty = false;
}

// Witness events: fused LEVEL transitions only, over the coarse vocabulary.
// The only time field is uptime coarsened to the project's 10-minute buckets —
// a precise per-event timestamp would hand the event stream exact activity
// timing, against the metadata-minimization invariant; `seq` preserves order.
//
// Every transition advances the Ed25519-anchored hash chain and is signed over
// the v1 `sentinel` canonical (canary/witness.h) — with or without a broker.
// The MQTT publish (event + refreshed retained chain head) happens only while
// connected; an offline gap shows up as a jump in seq / chain length rather
// than lost tamper evidence. Same shape as canary-sense's record_event_now.
static void emit_claim(const FusionResult& r, uint32_t now_ms) {
  Serial.printf("[sentinel] level=%s conf=%u anomaly=%u occ=%s range=%s "
                "modalities=%u/0x%02x%s\n",
                level_name(r.level), r.confidence, r.anomaly,
                occupancy_name(r.occupancy), range_band_name(r.range),
                r.strong_modalities, r.modality_bits,
                r.denied_any ? " DENIED" : "");

  static uint32_t fallback_seq = 0;
  refresh_snapshot(r, now_ms);
  set_last_event("level_changed");
  g_snap.claim.event = "level_changed";
  g_state_dirty = true;

  const uint32_t bucket_uptime_s = (now_ms / 1000UL / 600UL) * 600UL;
  // seq rides the NVS-persisted chain length so it stays monotonic across
  // reboots (a keyless boot falls back to a session counter).
  const uint32_t seq = canary::witness::ready()
                           ? canary::witness::chain_length() + 1
                           : ++fallback_seq;

  // Chain first: the witness record exists regardless of connectivity.
  canary::witness::chain_advance(seq, g_snap.claim, bucket_uptime_s);

  if (!canary::net::mqtt_connected()) return;

  // Envelope max is 142 bytes incl. NUL; the headroom keeps a future
  // constant bump from silently publishing unsigned events through the
  // fail-closed truncation path.
  char sig_env[160] = "";
  const bool signed_ok = canary::witness::sign_event_envelope(
      seq, g_snap.claim, bucket_uptime_s, sig_env, sizeof(sig_env));

  char msg[640];
  const int n = snprintf(msg, sizeof(msg),
           "{"
           "\"device_id\":\"%s\","
           "\"device_type\":\"%s\","
           "\"event\":\"%s\","
           "\"seq\":%lu,"
           "\"bucket_uptime_s\":%lu,"
           "\"level\":\"%s\","
           "\"confidence\":%u,"
           "\"anomaly\":%u,"
           "\"occupancy\":\"%s\","
           "\"range\":\"%s\","
           "\"modality_bits\":%u,"
           "\"signed\":%s"
           "%s"
           "}",
           canary::cfg::get().device_id, DEVICE_TYPE,
           g_snap.claim.event,
           (unsigned long)seq,
           (unsigned long)bucket_uptime_s,
           g_snap.claim.level,
           (unsigned)g_snap.claim.confidence,
           (unsigned)g_snap.claim.anomaly,
           g_snap.claim.occupancy,
           g_snap.claim.range,
           (unsigned)g_snap.claim.modality_bits,
           signed_ok ? "true" : "false",
           sig_env);
  if (n <= 0 || (size_t)n >= sizeof(msg)) return;

  canary::net::publish_event(TOPICS, msg);
  canary::net::publish_chain_retained(TOPICS);
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

// PHASE 1b — NOT BUILT. WiFi-RF, BLE and CSI ride the onboard radio and share
// the same privacy-preserving, MAC-free counting/feature paths canary-wap
// proves (canary-wap's rf_presence, common/bluetooth, common/csi). Whether
// they coexist on the C6's one radio with the STA link this phase brings up
// is the bench question that gates them (README checklist: "CSI + RF + BLE
// coexistence"), and the CSI HAL on the C6 is unproven. Until then these
// channels never vote — the engine treats a channel that never reports as
// quiet, not denied. Their call sites, for when 1b lands:
//
//   g_engine.observe(Channel::WifiRf, chan::count_vote(wifi_devs, 1, 3), 100, now);
//   g_engine.observe(Channel::Ble,    chan::count_vote(ble_devs, 1, 3),  100, now);
//   g_engine.observe(Channel::WifiCsi, chan::csi_vote(confirmed, observed), 85, now);

// One sensing pass: read the channels this board has, fuse on the engine's
// cadence, and take any level transition through the chokepoint. Runs every
// loop() pass and — through wifi_init_or_reboot's idle poll — while the boot
// join is still in flight, so the guardian witnesses from its first second.
static void sense_tick(uint32_t now) {
  static uint32_t next_tick = 0;
  static uint32_t next_light = 0;

  // Radar/PIR are cheap and edge-sensitive: sample every pass.
  read_pir(now);
  read_radar(now);
  if ((int32_t)(now - next_light) >= 0) {
    read_light(now);
    next_light = now + LIGHT_SAMPLE_MS;
  }

  if ((int32_t)(now - next_tick) >= 0) {
    next_tick = now + FUSION_TICK_MS;
    const FusionResult r = g_engine.evaluate(now);
    if (r.changed) emit_claim(r, now);
  }
}

// ----------------------------------------------------------------------------
// Serial boot output redirect (USB console).
// ----------------------------------------------------------------------------
static void sentinel_serial_write(const char* str) {
  Serial.print(str);
}

// ----------------------------------------------------------------------------
// Arduino entry points
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(600);

  boot_set_output(sentinel_serial_write);

  // Privacy (Invariant III): never surface the raw MAC. The stable device
  // handle is the salted, MAC-free pseudonym shown as "Hardware ID" below;
  // mac_address is left null so the boot banner skips the MAC line.
  boot_info_t bi = {};
  bi.product_name  = "SecuraCV Canary Sentinel";
  bi.fw_version    = CANARY_FW_VERSION;
  bi.build_date    = __DATE__;
  bi.build_time    = __TIME__;
  bi.device_type   = DEVICE_TYPE;
  bi.model         = MODEL;
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
  boot_separator();
  boot_kvf("Sentinel", "tier=%s  preset device=%s", TIER, DEVICE_ID);
  boot_kv("Phase", "1a: witness + MQTT/HA + pull-OTA (onboard-radio channels: 1b, not built)");
  boot_blank();

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

  TOPICS = build_topics(canary::cfg::get().device_id);

  // Witness identity + hash chain (NVS-backed Ed25519; see witness.h).
  // Before the network: a keyless boot must be loud in the boot log, and
  // the chain must be ready before the first level transition.
  canary::witness::init();
  boot_line("[sentinel] fusion armed");

  // The idle poll keeps the guardian sensing (and chaining its transitions)
  // through the blocking boot connect, and an unprovisioned board raises the
  // shared setup portal instead of joining a placeholder SSID.
  canary::net::wifi_init_or_reboot([]() { sense_tick(canary::ms_now()); });

  // Fleet LAN presence: the _securacv._tcp advert that lets the companion
  // app, canary-display and sibling Canaries find and label this witness.
  // Failure is non-fatal — MQTT/HA never depends on it.
  canary::net::mdns_init();

  // Seed the heap-health snapshot so the first status publish carries real
  // numbers instead of zeros.
  canary::diag::loop(canary::ms_now());

  // Confirm (or roll back) a freshly installed image now — before anything
  // that can block on external services. See ota_mgr.h.
  canary::net::ota_boot_validate();

  canary::net::mqtt_init(TOPICS);

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
  // success). A broker that is down at boot must not block the guardian:
  // sensing continues regardless and loop()'s backoff supervisor keeps trying.
  if (!mqtt_supervise(canary::ms_now())) {
    canary::log_line("MQTT", "Broker unreachable — sensing continues; retrying in loop().");
  }

  // Signed pull-OTA: arm the engine (validation already ran right after
  // WiFi). Daily jittered checks; HA's Install button and auto-update
  // switch are drained in loop().
  canary::net::ota_init(TOPICS);

  set_last_event("boot");
  publish_state_now(canary::ms_now());
  g_last_heartbeat_ms = canary::ms_now();

#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
  // Task watchdog, armed LAST so the blocking boot phases above (WiFi connect
  // up to 30 s) can't trip it. The timeout must exceed loop()'s worst bounded
  // block (one MQTT connect attempt against a dead broker). Dual API: the
  // Standard/Heavy C6 envs are on arduino-esp32 3.x (ESP-IDF 5, struct
  // config); the Lite C3 env is on 2.0.17 (ESP-IDF 4.4, scalar arguments) —
  // the same guard canary-vision uses.
  {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_task_wdt_config_t wdt_config = {
      .timeout_ms = WATCHDOG_TIMEOUT_SEC * 1000,
      .idle_core_mask = (1 << 0),   // XIAO ESP32-C6 / C3: single core
      .trigger_panic = true
    };
    esp_err_t wdt_err = esp_task_wdt_reconfigure(&wdt_config);
    if (wdt_err == ESP_ERR_INVALID_STATE) {
      esp_task_wdt_init(&wdt_config);
    }
#else
    esp_task_wdt_init(WATCHDOG_TIMEOUT_SEC, /*panic=*/true);
#endif
    esp_task_wdt_add(NULL);
    boot_kvf("Watchdog", "%lu s timeout", (unsigned long)WATCHDOG_TIMEOUT_SEC);
  }
#endif

  boot_scene_ready(
      "It will fuse independent sensing channels at this doorway",
      "and publish signed coarse claims via MQTT to Home Assistant.",
      NULL);
}

void loop() {
#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
  esp_task_wdt_reset();
#endif

  const uint32_t now = canary::ms_now();

  // ── Sensing first: the guardian keeps observing with or without a network ──
  sense_tick(now);

  // ── Network supervision ──
  canary::net::wifi_loop(now);
  canary::net::mdns_loop(now);  // drain deferred re-announce (event task latches only)
  canary::diag::loop(now);

  // Bounded, backoff-scheduled broker supervision: while the broker is
  // unreachable the guardian keeps sensing and chaining (sense_tick above)
  // and simply skips the publish phase.
  if (!mqtt_supervise(now)) {
    delay(5);
    return;
  }

  canary::net::mqtt_loop();
  canary::net::ota_loop(now);

  if (g_state_dirty) {
    publish_state_now(now);
  }

  // Periodic health refresh (retained pubkey/uptime/heap for HA's trust
  // store and diagnostics).
  if ((int32_t)(now - g_last_health_ms) >= (int32_t)HEALTH_PUBLISH_MS) {
    g_last_health_ms = now;
    canary::net::publish_health_retained(TOPICS);
  }

  // Heartbeat + state refresh. Under heap pressure the diagnostics ladder
  // stretches the cadence (2x at critical, 5x at emergency).
  const uint32_t heartbeat_ms = HEARTBEAT_MS * canary::diag::period_scale();
  if ((int32_t)(now - g_last_heartbeat_ms) >= (int32_t)heartbeat_ms) {
    g_last_heartbeat_ms = now;
    refresh_snapshot(g_engine.last(), now);
    canary::net::publish_heartbeat(TOPICS, g_snap);
    publish_state_now(now);
  }

  delay(5);
}
