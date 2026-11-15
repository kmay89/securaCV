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
  cross emit_claim(). Beside the claim, the retained health payload carries
  the radar UART's link health, as canary-sense's does (radar_link_health():
  link up or down, the age of the last frame, the parser's error count),
  never a reading. The device-details design classes that age and that count
  as activity, and its tier policy keeps activity off the broker; they reach
  the owner's broker here anyway, as a legacy exception: the same health
  object canary-sense already publishes, which that design's PR 7 asks this
  file to publish too. Home Assistant's diagnostics redaction does not list
  `last_frame_age_ms` yet; device-details PR 16 adds it.
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
#include <Preferences.h>
#include <WiFi.h>
#include "securacv_ota.h"                 // the engine's state word for /api/status
#include "network/node_http.h"            // GET /api/status + /api/fleet once online (common/)
#include "network/node_status.h"          // the one status body (common/)
#include "network/setup_portal.h"         // portal-up fact for the node surface
#include "io/reset_hold.h"                // the 10 s factory-reset hold (common/)
#include "identity/key_at_rest.h"         // the creds_at_rest word (common/)
#include "network/peer_link.h"            // the hub-less mesh: authenticated heartbeats, the coordinator (common/)
#include "network/mdns_txt.h"             // the coord TXT word
#include "identity/device_signature.h"    // fingerprint_hex() for the peer identity

// Board-agnostic fusion brain + channel adapters (firmware/common via -I).
#include "canary/sentinel_config.h"        // build_fusion_config() from the preset
#include "canary/sentinel_links.h"         // the radar and light links, one rule each
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
static canary::sentinel::RadarLink g_radar_link;  // the fusion vote and the health object read it
#endif
#if defined(FEATURE_AMBIENT_LIGHT) && FEATURE_AMBIENT_LIGHT
static securacv::sensors::BH1750 g_lux;
static canary::sentinel::LightChannel g_light;  // its lux baseline (canary/sentinel_links.h)
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

// The health payload's radar object, or nullptr while there is none to give
// (see radar_link_health(), below the radar reader whose state it reports).
static const canary::net::RadarLinkHealth* radar_link_health(uint32_t now_ms);

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
    canary::net::publish_health_retained(TOPICS, radar_link_health(now));
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

// Whether this build's light sensor did not answer at boot (the driver keeps
// begin()'s answer as present()). A build without the sensor has nothing to
// be absent. tests_host/test_sentinel_links.cpp holds this body to its source.
static bool light_absent_at_boot() {
#if defined(FEATURE_AMBIENT_LIGHT) && FEATURE_AMBIENT_LIGHT
  return !g_lux.present();
#else
  return false;
#endif
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
  // A channel voting Denied, or a light sensor absent since boot: an absent
  // sensor never votes, but it is never a silent gap either (R6).
  g_snap.denied_any        = canary::sentinel::channel_denied_reported(r.denied_any,
                                                                       light_absent_at_boot());
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
    canary::sentinel::radar_link_frame(g_radar_link, now);
    if (f.kind == FrameKind::Presence) {
      present_now = f.has_target;
      // Coarse side-bands consumed here; the raw cm/count never leave this file.
      g_engine.set_range(chan::range_from_cm(f.distance_cm, SENT_RANGE_NEAR_CM,
                                             SENT_RANGE_MID_CM));
      g_engine.set_occupancy(chan::occupancy_from_count(f.target_count));
    }
  }

  const bool stalled = canary::sentinel::radar_stalled(g_radar_link, now, SENT_STALE_RADAR);
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

// The radar object of the retained health payload: the wire contract Home
// Assistant's radar-link diagnostic reads (link_ok / last_frame_age_ms /
// frame_errors; RadarLinkHealth in canary/net/mqtt_mgr.h). It reports the
// link read_radar() above already judges, by the same rule
// (canary/sentinel_links.h), so once the radar has spoken the health object
// and the fusion vote agree: link_ok is false exactly when the vote is
// Denied. A radar silent since boot is the exception: it reads down here
// once judged, while its channel never votes, as an unwired one does. The
// publish was there, carried from canary-sense; nothing here passed it a
// radar, so a Standard build's health never carried one.
// tests_host/test_sentinel_links.cpp holds this body, and both publishes'
// use of it, to their source.
//
// nullptr while the link has not been judged yet (no frame decoded and the
// stall window not yet over since the UART opened), which HA keeps as
// "unknown" rather than "down", and always on a build with no radar (Lite),
// so HA grows no radar-link sensor for it.
static const canary::net::RadarLinkHealth* radar_link_health(uint32_t now_ms) {
#if defined(FEATURE_MMWAVE_RADAR) && FEATURE_MMWAVE_RADAR
  static canary::net::RadarLinkHealth h;
  if (!canary::sentinel::radar_link_judged(g_radar_link, now_ms, SENT_STALE_RADAR)) return nullptr;
  h.link_ok           = canary::sentinel::radar_link_ok(g_radar_link, now_ms, SENT_STALE_RADAR);
  h.last_frame_age_ms = canary::sentinel::radar_frame_age_ms(g_radar_link, now_ms);
  h.frame_errors      = g_radar_parser.error_count();
  return &h;
#else
  (void)now_ms;
  return nullptr;
#endif
}

static void read_light(uint32_t now) {
#if defined(FEATURE_AMBIENT_LIGHT) && FEATURE_AMBIENT_LIGHT
  // begin()'s answer at boot (the driver keeps it as present()) tells a unit
  // with no light sensor (no vote) from a sensor that stopped answering
  // (Denied, R2); the rule and the vote are canary/sentinel_links.h's, and
  // tests_host/test_sentinel_links.cpp holds this body to its source.
  const canary::sentinel::LightSample s =
      canary::sentinel::light_sample(g_light, g_lux.present(), g_lux.read_lux());
  if (s.votes) g_engine.observe(Channel::Light, s.vote, s.quality, now);
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

// Where this node stands with its hub: none (nobody pointed it at one), down
// (configured, unreachable), ok — three values, not a bool, because the first
// two need different people to act (fleet_selfreport.h's hub note).
static int hub_word() {
  if (!canary::cfg::mqtt_credentials_configured()) return NS_HUB_NONE;
  return canary::net::mqtt_connected() ? NS_HUB_OK : NS_HUB_DOWN;
}

static void fill_node_status(NodeStatus& st) {
  static char ip[16];
  static char tz[64];
  const auto& cfg = canary::cfg::get();
  const uint32_t now = canary::ms_now();
  st.node = DEVICE_TYPE;
#ifdef SECURACV_OTA_PRODUCT
  st.product = SECURACV_OTA_PRODUCT;
#endif
  st.fw = CANARY_FW_VERSION;
#ifdef CANARY_FIGURE_HARDWARE
  st.hardware = CANARY_FIGURE_HARDWARE;
#endif
  st.name = cfg.device_id;
  st.uptime_s = now / 1000u;
  if (canary::net::wifi_connected()) {
    snprintf(ip, sizeof(ip), "%s", WiFi.localIP().toString().c_str());
    st.ip = ip;
    st.rssi = canary::net::wifi_rssi();
  }
  st.onboarding = canary::net::onboard::state_label(canary::net::wifi_onboarding_state());
  st.setup_ap = canary::net::wifi_setup_ap_up() ? 1 : 0;
#if defined(FEATURE_IMPROV) && FEATURE_IMPROV
  st.ble_door = canary::net::improv_ble::setup_open() ? 1 : 0;
#endif
  st.saved_networks = canary::net::wifi_saved_networks();
  st.hub = hub_word();
#if defined(FEATURE_FLEET_ROSTER) && FEATURE_FLEET_ROSTER
  st.peers_seen = canary::net::fleet_roster_scan_peer_count();
#endif
  // The Witness Wall reads this node directly when it has polled /api/fleet
  // lately; otherwise the hub's roll-call carries it, or nothing does.
  st.wall = canary::net::node_http::wall_polling(now) ? "direct"
            : (st.hub == NS_HUB_OK ? "hub" : "none");
  st.wall_polled_s_ago = canary::net::node_http::fleet_polled_s_ago(now);
  // The mesh: on when a fleet key could be made, off on an open network; who
  // coordinates; peers = the most any band has heard.
  st.peer = canary::net::peer_link::armed() ? "on" : "off";
  {
    static char coord[17];
    if (canary::net::peer_link::coord_word(coord, sizeof(coord), now) > 0) st.coord = coord;
  }
  {
    const int mesh_peers = canary::net::peer_link::peers();
    if (mesh_peers > st.peers_seen) st.peers_seen = mesh_peers;
  }
  st.ota_state = securacv_ota_state_word(securacv_ota_get_state());
  const securacv_ota_manifest_t* m = securacv_ota_get_manifest();
  if (m != nullptr && m->version[0] && securacv_ota_update_available()) st.ota_target = m->version;
  st.ota_pending_verify = securacv_ota_pending_verify() ? 1 : 0;  // this boot's image is still unconfirmed (the dwell is running)
  // Honest posture: every shipped build keeps its secrets in plaintext NVS
  // (common/identity/key_at_rest.h Tier 0); the path up is NVS encryption
  // (the arduino-as-IDF-component migration), not the secure kit, whose flash
  // encryption does not cover NVS.
  st.creds_at_rest = key_at_rest::wire_label(key_at_rest::Tier::PlaintextNvs);
  st.clock = canary::time_sync::source_label(canary::net::wifi_clock_source());
  {
    Preferences prefs;
    if (prefs.begin("securacv", /*readOnly=*/true)) {
      snprintf(tz, sizeof(tz), "%s", prefs.getString("tz", "").c_str());
      prefs.end();
      if (tz[0]) st.tz = tz;
    }
  }
}

// The self row of GET /api/fleet — the roll-call the Witness Wall polls
// (tvos/discovery/DISCOVERY.md). Coarse: presence + health, never raw media.
static void fill_fleet_self(FleetSelfDevice& d) {
  const auto& cfg = canary::cfg::get();
  d.name = cfg.device_id;
  d.product = DEVICE_TYPE;
  d.online = 1;
  d.chain_ok = canary::witness::ready() ? 1 : 0;
  d.chain_height = canary::witness::ready() ? (int)canary::witness::chain_length() : -1;
#ifdef CANARY_FIGURE_HARDWARE
  d.hardware = CANARY_FIGURE_HARDWARE;
#endif
  d.hub = hub_word();
}

static const char* node_kernel_name() { return canary::cfg::get().device_id; }

static canary::net::node_http::Hooks node_http_hooks() {
  canary::net::node_http::Hooks h{};
  h.fill_status = fill_node_status;
  h.fill_fleet = fill_fleet_self;
  h.kernel_name = node_kernel_name;
  return h;
}


// ── The hub-less mesh (common/network/peer_link): the household fleet key,
// authenticated heartbeats on the beacon's multicast group, and the one
// deterministic coordinator every node computes the same way. docs/TRUST_MODEL.md.
static bool peer_hub_ok() { return canary::net::mqtt_connected(); }

static void peer_fill_heartbeat(canary::net::peer::Heartbeat& hb) {
  hb.onboarding = (uint8_t)canary::net::wifi_onboarding_state();
  hb.hub = (uint8_t)hub_word();
  hb.ota = (uint8_t)securacv_ota_get_state();
  hb.rssi = (int8_t)canary::net::wifi_rssi();
  const int saved = canary::net::wifi_saved_networks();
  hb.saved_networks = (uint8_t)(saved < 0 ? 0 : saved > 255 ? 255 : saved);
}

// The 16-hex fingerprint everything else uses → the 8 bytes a peer frame
// carries. A node without a key (witness init failed) gets all-zero bytes
// and still speaks, so the fleet can see it is up and keyless.
static void peer_fp_bytes(uint8_t out[canary::net::peer::FP_BYTES]) {
  memset(out, 0, canary::net::peer::FP_BYTES);
  const char* hex = device_signature::fingerprint_hex();
  if (hex == nullptr || strlen(hex) < 16) return;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  for (size_t i = 0; i < canary::net::peer::FP_BYTES; i++) {
    out[i] = (uint8_t)((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]));
  }
}

static void peer_link_setup() {
  canary::net::peer_link::Identity id{};
  peer_fp_bytes(id.fp);
  id.role = canary::net::peer::ROLE_WITNESS;
  id.caps = canary::net::mdns_txt::CAP_STATUS | canary::net::mdns_txt::CAP_FLEET |
            canary::net::mdns_txt::CAP_OTA | canary::net::mdns_txt::CAP_MQTT |
            canary::net::mdns_txt::CAP_PEER;
#if defined(FEATURE_IMPROV) && FEATURE_IMPROV
  id.caps |= canary::net::mdns_txt::CAP_BLE_DOOR;
#endif
#if defined(FEATURE_FLEET_BEACON) && FEATURE_FLEET_BEACON
  id.caps |= canary::net::mdns_txt::CAP_BEACON;
#endif
  canary::net::peer_link::Hooks hooks{};
  hooks.fill_heartbeat = peer_fill_heartbeat;
  hooks.hub_ok = peer_hub_ok;
  canary::net::peer_link::begin(id, hooks);
}

// Each pass: arm on (re)join with the network on the air (the key is derived
// from it, or the stored replacement wins), tick the socket, and keep the
// advert's `coord` word current (re-applied only when it changes).
static void peer_link_tick(uint32_t now) {
  static bool was_online = false;
  static uint32_t next_coord_ms = 0;
  const bool online = canary::net::wifi_connected();
  if (online && !was_online) {
    const char* ssid = nullptr;
    const char* pass = nullptr;
    // The witness key may have been established after peer_link_setup()
    // (vision inits it late in setup); take the fingerprint again now.
    uint8_t fp[canary::net::peer::FP_BYTES];
    peer_fp_bytes(fp);
    canary::net::peer_link::set_fp(fp);
    if (canary::net::wifi_current_network(&ssid, &pass)) canary::net::peer_link::arm(ssid, pass);
  }
  if (!online && was_online) canary::net::peer_link::disarm();
  was_online = online;
  canary::net::peer_link::tick(now, online);
  if ((int32_t)(now - next_coord_ms) >= 0) {
    next_coord_ms = now + 5000;
    char word[17];
    canary::net::peer_link::coord_word(word, sizeof(word), now);
    canary::net::mdns_set_coord(word);
  }
}

#if defined(BOOT_BUTTON_PIN) && (BOOT_BUTTON_PIN >= 0)
// The 10 s factory-reset hold (common/io/reset_hold.h): saved networks, the
// setup AP's key, the hub and the settings go; the witness identity and the
// chain stay (onboarding_fsm.h says why: every hub and display that pinned
// this key would otherwise see a stranger). The BOOT pin is a strapping pin,
// so it is only ever read.
static void forget_settings_keep_identity() {
  canary::net::wifi_forget_all();
  Preferences prefs;
  if (prefs.begin("securacv", /*readOnly=*/false)) {
    // "mqtt_tls" is canary::net::mqtt_tls::NVS_KEY_MODE (mqtt_transport.h).
    static const char* const kKeys[] = {"mqtt_host", "mqtt_port", "mqtt_user",
                                        "mqtt_pass", "mqtt_tls", "dev_name", "tz"};
    for (const char* k : kKeys) prefs.remove(k);
    prefs.end();
  }
}

static void reset_hold_tick(uint32_t now) {
  static canary::io::ResetHold hold;
  static bool pin_ready = false;
  if (!pin_ready) {
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    pin_ready = true;
  }
  switch (hold.step(digitalRead(BOOT_BUTTON_PIN) == BOOT_BUTTON_ACTIVE, now)) {
    case canary::io::HoldEvent::Warn:
      canary::log_line("RESET", "BOOT held 3 s - keep holding to 10 s to forget the saved networks, hub and settings (identity kept).");
      break;
    case canary::io::HoldEvent::Reset:
      // The erase happens NOW, while the hand is still on the button, so the
      // feedback lands; the restart waits for the release. BOOT is a
      // strapping pin (GPIO0 / GPIO9): a reset with it held low is the ROM's
      // download-mode gesture, and a node that reboots into the serial
      // bootloader instead of its setup network reads as bricked.
      canary::log_line("RESET", "BOOT held 10 s - factory reset: credentials and settings forgotten; the witness identity is kept. Let go to restart into setup.");
      forget_settings_keep_identity();
      break;
    case canary::io::HoldEvent::Released:
      if (hold.released_stage >= 2) {
        canary::log_line("RESET", "Restarting into setup.");
        delay(200);
        ESP.restart();
      }
      break;
    default:
      break;
  }
}
#endif

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
  canary::sentinel::radar_link_open(g_radar_link, millis());
#endif
#if defined(FEATURE_AMBIENT_LIGHT) && FEATURE_AMBIENT_LIGHT
  Wire.begin(I2C_SDA, I2C_SCL);
  // The POWER_ON acknowledgment is the BH1750's only presence probe. A unit
  // where nothing answers has no light sensor: its light channel never votes,
  // where it used to vote Denied on every read, and the state topic reports
  // it as a denied channel (canary/sentinel_links.h).
  if (g_lux.begin(Wire, BH1750_ADDR)) {
    boot_kv("BH1750", "ambient light sensor online");
  } else {
    boot_kv("BH1750", "not found: absent, never votes, reported as channel_denied");
  }
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
  // The node's LAN surface: GET /api/status + /api/fleet (common/network/node_http).
  canary::net::node_http::begin(node_http_hooks());
  peer_link_setup();   // the hub-less mesh (docs/TRUST_MODEL.md)

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
  // The node's LAN surface rides the link: up when online and the setup
  // portal (which owns port 80) is down, torn down otherwise.
  canary::net::node_http::loop(now, canary::net::wifi_connected(),
                               canary::net::setup_portal_active());
  peer_link_tick(now);
#if defined(BOOT_BUTTON_PIN) && (BOOT_BUTTON_PIN >= 0)
  reset_hold_tick(now);   // the 10 s factory-reset hold (identity kept)
#endif
  canary::net::mdns_loop(now);  // drain deferred re-announce (event task latches only)
  canary::diag::loop(now);

  // Confirm a new image only after a healthy minute — before the broker
  // early-return below, so it runs on EVERY pass. Behind it, a new image that
  // booted to a dark broker waited on the broker to confirm, and a restart
  // before the broker came back returned it to the previous firmware.
  // Healthy is the Wi-Fi station link, the same thing the boot probe asserts.
  canary::net::ota_health_tick(now, canary::net::wifi_connected());

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
    canary::net::publish_health_retained(TOPICS, radar_link_health(now));
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
