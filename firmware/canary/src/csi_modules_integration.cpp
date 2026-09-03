/*
 * SecuraCV Canary — CSI module pipeline integration (PIO build)
 *
 * Includes ONLY the common library headers so the local
 * `csi_features_t` (declared in `securacv_csi.h`) and the common
 * `csi_features_t` (declared in `csi_types.h`) never collide in the
 * same translation unit. The bridge takes a `void*` from main.cpp's
 * features callback; the runtime size check below verifies the two
 * structs stay byte-compatible.
 */

#include "csi_modules_integration.h"

/* Common CSI library — module pipeline + chokepoint + bundler. */
#include "csi_types.h"
#include "csi_module.h"
#include "csi_event.h"

/* v1 modules — same set the canary-wap Arduino build registers. */
#include "core_presence.h"
#include "core_breathing.h"
#include "core_activity_ribbon.h"
#include "meta_daily_summary.h"
#include "anomaly_baseline.h"
#include "wifi_channel_activity.h"
#include "core_multilink_fusion.h"
#include "meta_empty_room_baseline.h"
#include "tamper_events_module.h"

#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
#include "ble_scout.h"
#endif

/* The Scout broadcast hook calls into mesh_session::send_beacon_event
 * when both FEATURE_BLE_SCAN and FEATURE_MESH_NETWORK are enabled at
 * compile time. Pulling in the mesh_session header unconditionally
 * would force the LDF chain to include the securacv_mesh library in
 * every env — keep it inside the #if so dev/release/minimal builds
 * stay lean. */
#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN \
    && defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
#include "mesh_session.h"
#include "mesh_beacon.h"
#endif

/* PR 4b — channel-hop coordinator. Needs mesh_session (for
 * send_channel_lock + set_channel_lock_handler), csi_hal (for
 * set_channel_lock on receive), and the channel-hop wire format +
 * HopTracker. Gated on just FEATURE_MESH_NETWORK. */
#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
#if !(defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN)
#include "mesh_session.h"
#endif
#include "mesh_channel_hop.h"
#include "mesh_hub_election.h"
#include "mesh_state.h"      /* save/load elected-hub fingerprint (PR-3) */
#include "csi_hal.h"
#endif

#include "csi_hal.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN \
    && defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#endif

namespace {

bool s_initialized = false;

/* ──────────────────────────────────────────────────────────────────────────
 * SETTINGS — NVS-backed, mapped through the same short-key convention the
 * canary-wap Arduino build uses. ESP32 Preferences imposes a 15-char key
 * limit, so module-style "core.presence.preset" maps to "cp.preset" etc.
 * Keeping the same NVS layout means a device flashed with canary-wap and
 * later re-flashed with canary PIO retains its tunables.
 * ────────────────────────────────────────────────────────────────────────── */

constexpr const char* SETTINGS_NS = "csi";

struct SettingKey {
  const char* full;
  const char* nvs;
};

const SettingKey SETTING_KEYS[] = {
  /* core.presence */
  { "core.presence.pet_mode",            "cp.pet_mode"   },
  { "core.presence.preset",              "cp.preset"     },
  { "core.presence.sensitivity",         "cp.sens"       },
  { "core.presence.motion_threshold",    "cp.mt"         },
  { "core.presence.active_threshold",    "cp.at"         },
  { "core.presence.breathing_threshold", "cp.bt"         },
  { "core.presence.pet_mode_seconds",    "cp.ps"         },
  /* shimmer filter */
  { "core.presence.shimmer_rssi_swing",  "cp.srs"        },
  { "core.presence.shimmer_doppler_floor","cp.sdf"       },
  { "core.presence.shimmer_enabled",     "cp.se"         },
  /* core.breathing */
  { "core.breathing.lock_threshold",     "cb.lt"         },
  { "core.breathing.confirm_seconds",    "cb.cs"         },
  /* core.quiet_hours */
  { "core.quiet_hours.enabled",          "qh.en"         },
  { "core.quiet_hours.start_min",        "qh.start"      },
  { "core.quiet_hours.end_min",          "qh.end"        },
  /* anomaly.baseline */
  { "anomaly.baseline.spike_ratio",      "ab.sr"         },
  { "anomaly.baseline.min_motion",       "ab.mm"         },
  { "anomaly.baseline.min_breathing",    "ab.mb"         },
  { "anomaly.baseline.cooldown_sec",     "ab.cd"         },
};

const char* nvs_key_for(const char* full_key) {
  if (!full_key) return nullptr;
  for (const SettingKey& k : SETTING_KEYS) {
    if (strcmp(k.full, full_key) == 0) return k.nvs;
  }
  return nullptr;
}

#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
static void format_fingerprint(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN],
                               char out[2 * mesh_crypto::FINGERPRINT_LEN + 1]) {
  // NB: not named HEX — Arduino's Print.h defines `#define HEX 16` (the
  // numeric base constant), which is pulled in transitively on the core-3.x
  // BLE build envs and would clobber a local identifier called HEX.
  static const char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < mesh_crypto::FINGERPRINT_LEN; ++i) {
    out[2 * i]     = HEX_DIGITS[(fp[i] >> 4) & 0xF];
    out[2 * i + 1] = HEX_DIGITS[ fp[i]       & 0xF];
  }
  out[2 * mesh_crypto::FINGERPRINT_LEN] = '\0';
}
#endif

#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN \
    && defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
/* ──────────────────────────────────────────────────────────────────────────
 * BLE SCOUT ↔ MESH GLUE (PR 5c integration)
 *
 * Connects the three pieces shipped over PRs 5c-1..5c-4:
 *   1. ble_scout's emit_arrived/departed transition fires the broadcast
 *      hook (PR 5c-1).
 *   2. The hook forwards to mesh_session::send_beacon_event, which builds
 *      and signs a BEACON_EVENT envelope (PR 5c-3) carrying the wire
 *      format from mesh_beacon (PR 5c-2).
 *   3. On the receive side, mesh_session::on_opera_frame routes inbound
 *      BEACON_EVENT frames into the handler installed via
 *      set_beacon_event_handler — peer table lookup + signature verify
 *      + replay defense (PR 5c-4).
 *
 * The forwarder is intentionally minimal: mesh_session::send_beacon_event
 * already short-circuits if !has_opera_secret() or the session isn't
 * running, so we don't duplicate the precondition checks here. The
 * receive handler likewise just logs — the deeper "what does the Hub
 * DO with a beacon_event" question is PR 4b's territory.
 *
 * NOTE: mesh_session::init() / ::start() are now called from main.cpp
 * setup() (see main.cpp:542-546, guarded by FEATURE_MESH_NETWORK) with
 * the correct boot ordering against WiFi for ESP-NOW to bind, and
 * mesh_transport::process() / mesh_session::process() run each loop
 * iteration (main.cpp:1045-1046). The callbacks registered below are
 * therefore live end-to-end when FEATURE_MESH_NETWORK=1: the send-side
 * broadcasts and the receive-side dispatches inbound frames. */

/* FreeRTOS queue that marshals beacon events from the (possibly
 * cross-task) ble_scout broadcast callback into the main loop,
 * where mesh_session::send_beacon_event is allowed to run per
 * PR 5c-3's threading contract.
 *
 * Producer: on_scout_beacon_event_outbound runs from EITHER the
 * main loop (ble_scout_tick → emit_departed) OR the NimBLE host
 * task (ble_scout_on_advert → emit_arrived). That is MPSC by
 * construction. xQueueSend is MPSC-safe (acquires the queue's
 * internal lock), so two concurrent producers from different tasks
 * cannot race — unlike the previous hand-rolled atomic ring, which
 * was a correct SPSC primitive but wrong for the declared usage.
 *
 * Consumer: drain_outbound_beacon_queue() runs at the top of
 * securacv_csi_modules_feed (called from main loop on every CSI
 * window). It pops every queued event and invokes mesh_session::
 * send_beacon_event under the right task context. */
struct OutboundBeaconEvent {
  bool                       arrived;
  char                       label[mesh_beacon::MAX_LABEL_BYTES + 1];
};
constexpr size_t  OUTBOUND_QUEUE_CAP = 8;
static QueueHandle_t s_outbound_queue = nullptr;

static void on_scout_beacon_event_outbound(bool arrived, const char* label) {
  /* Producer — main loop OR NimBLE host task. xQueueSend is safe
   * under concurrent producers and from any task context (not from
   * ISR; the NimBLE host task is a task, not an ISR). Timeout 0 =
   * non-blocking drop when the queue is full — bounded loss
   * (ble_scout's chokepoint already caps emissions at 24/hr per
   * beacon, so 8 in-flight is generous). */
  QueueHandle_t q = __atomic_load_n(&s_outbound_queue, __ATOMIC_ACQUIRE);
  if (q == nullptr) return;
  OutboundBeaconEvent ev = {};
  ev.arrived = arrived;
  if (label != nullptr) {
    strncpy(ev.label, label, sizeof(ev.label) - 1);
    ev.label[sizeof(ev.label) - 1] = '\0';
  } else {
    ev.label[0] = '\0';
  }
  (void)xQueueSend(q, &ev, 0);
}

static void drain_outbound_beacon_queue() {
  /* Consumer — main loop only. */
  QueueHandle_t q = __atomic_load_n(&s_outbound_queue, __ATOMIC_ACQUIRE);
  if (q == nullptr) return;
  OutboundBeaconEvent ev;
  while (xQueueReceive(q, &ev, 0) == pdTRUE) {
    mesh_session::send_beacon_event(
        ev.arrived ? mesh_beacon::BeaconState::ARRIVED
                     : mesh_beacon::BeaconState::DEPARTED,
        ev.label,
        (uint32_t)millis());
  }
}

static void on_peer_beacon_event_inbound(
    const uint8_t              sender_fp[mesh_crypto::FINGERPRINT_LEN],
    mesh_beacon::BeaconState   state,
    const char*                label) {
  char fp_hex[2 * mesh_crypto::FINGERPRINT_LEN + 1];
  format_fingerprint(sender_fp, fp_hex);
  Serial.printf("[ble.scout.peer] fp=%s state=%s label=\"%s\"\n",
                fp_hex,
                state == mesh_beacon::BeaconState::ARRIVED ? "arrived"
                : state == mesh_beacon::BeaconState::DEPARTED ? "departed"
                : "?",
                label ? label : "");
}
#endif  /* FEATURE_BLE_SCAN && FEATURE_MESH_NETWORK */

#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
/* ──────────────────────────────────────────────────────────────────────────
 * CHANNEL-HOP COORDINATOR (PR 4b integration)
 *
 * Hub side: tick the HopTracker every main-loop pass with the current
 * airtime utilization. When utilization exceeds 50% for 60 s, select
 * the next non-overlapping channel and broadcast CHANNEL_LOCK.
 *
 * Peer side: on receiving CHANNEL_LOCK, apply the proposed channel
 * via csi_hal::set_channel_lock and log.
 *
 * The PIO build doesn't have airtime_governor — tick with 0 so the
 * tracker never fires on its own. The receive side still works: a
 * canary-wap Hub can coordinate PIO peers.
 * ────────────────────────────────────────────────────────────────────────── */

static mesh_channel_hop::HopTracker s_hop_tracker =
    mesh_channel_hop::make_tracker(5000, 60000, 120000);

static void channel_hop_tick(uint32_t now_ms) {
  /* PIO build has no airtime_governor — pass 0 so the Hub-side
   * proposal logic is inert. PIO nodes act as peers that follow
   * channel locks from a canary-wap Hub. */
  if (!mesh_channel_hop::tick(s_hop_tracker, now_ms, 0)) return;
  /* If the tracker fires (impossible with util=0 but defensive): */
  mesh_channel_hop::reset(s_hop_tracker, now_ms);
}

static void on_peer_channel_lock(
    const uint8_t              sender_fp[mesh_crypto::FINGERPRINT_LEN],
    uint8_t                    channel,
    mesh_channel_hop::Reason   reason) {
  csi_hal::set_channel_lock(channel);

  char fp_hex[2 * mesh_crypto::FINGERPRINT_LEN + 1];
  format_fingerprint(sender_fp, fp_hex);

  Serial.printf("[mesh.channel] lock ch=%u reason=%u from fp=%s\n",
                channel, (unsigned)reason, fp_hex);
}

/* ──────────────────────────────────────────────────────────────────────────
 * ELECTED-HUB STATE (PR-3)
 *
 * Tracks the most recently elected coordinator fingerprint in RAM and
 * mirrors it to NVS (mesh_state) so failover continuity survives a
 * reboot. The RAM copy also de-dups NVS writes: an election broadcast
 * naming the same Hub we already recorded is a no-op for flash, which
 * matters because elections can re-fire on every peer state change.
 *
 * Consumer note: the elected-Hub identity is currently surfaced via the
 * boot log + this persisted record. Acting on it (HubMonitor instance,
 * coordinator role adoption) is the remaining piece of full hub failover
 * in the PIO build, tracked separately — see docs/mesh_esp_now_evaluation.md.
 * ────────────────────────────────────────────────────────────────────────── */

static uint8_t s_current_hub_fp[mesh_crypto::FINGERPRINT_LEN] = {0};
static bool    s_has_current_hub = false;

/* Load any persisted elected-Hub fingerprint into the RAM cache. Called
 * once from init so a post-reboot node starts with the last-known
 * coordinator instead of an empty slot. */
static void restore_elected_hub_from_nvs() {
  uint8_t fp[mesh_crypto::FINGERPRINT_LEN];
  if (mesh_state::load_elected_hub(fp)) {
    memcpy(s_current_hub_fp, fp, sizeof(s_current_hub_fp));
    s_has_current_hub = true;
    char fp_hex[2 * mesh_crypto::FINGERPRINT_LEN + 1];
    format_fingerprint(s_current_hub_fp, fp_hex);
    Serial.printf("[mesh.election] restored elected hub fp=%s from NVS\n",
                  fp_hex);
  }
}

static void on_peer_hub_election(
    const uint8_t              sender_fp[mesh_crypto::FINGERPRINT_LEN],
    mesh_hub_election::Event   event,
    const uint8_t              elected_fp[mesh_crypto::FINGERPRINT_LEN]) {
  char sender_hex[2 * mesh_crypto::FINGERPRINT_LEN + 1];
  char elected_hex[2 * mesh_crypto::FINGERPRINT_LEN + 1];
  format_fingerprint(sender_fp, sender_hex);
  format_fingerprint(elected_fp, elected_hex);

  Serial.printf("[mesh.election] %s from fp=%s elected=%s\n",
                event == mesh_hub_election::Event::HUB_ELECTED ? "elected"
                : event == mesh_hub_election::Event::HUB_ABSENT ? "absent"
                : "?",
                sender_hex, elected_hex);

  /* Persist the result of a completed election. HUB_ABSENT is the
   * "I no longer hear the Hub" signal that opens an election — the
   * winner isn't settled yet — so we only record HUB_ELECTED. */
  if (event != mesh_hub_election::Event::HUB_ELECTED) return;

  /* s_current_hub_fp mirrors what is actually persisted in NVS, so the
   * de-dup below skips re-writing a hub we've already stored. It is
   * updated ONLY after a successful save: if a save fails (transient
   * NVS fault, or flash encryption disabled), the cache stays stale so
   * the next identical HUB_ELECTED frame retries the write rather than
   * silently short-circuiting and never persisting at all. */
  if (s_has_current_hub &&
      mesh_hub_election::compare_fingerprints(s_current_hub_fp,
                                              elected_fp) == 0) {
    return;  /* already persisted this winner — skip the NVS write */
  }

  if (mesh_state::save_elected_hub(elected_fp)) {
    memcpy(s_current_hub_fp, elected_fp, sizeof(s_current_hub_fp));
    s_has_current_hub = true;
  } else {
    /* Non-fatal: failover still works from the live election broadcast;
     * we just won't have the hint cached across the next reboot (e.g.
     * flash encryption disabled on a dev board). Leave the cache
     * untouched so a later election re-attempts the persist. */
    Serial.println("[mesh.election] note: elected hub not persisted "
                   "(NVS write refused or failed)");
  }
}
#endif  /* FEATURE_MESH_NETWORK */

/* ──────────────────────────────────────────────────────────────────────────
 * CSI WATCHDOG CALLBACK
 *
 * csi_hal's watchdog detects 0 frames for 5 s and toggles the CSI rx
 * callback as a gentle recovery. This callback logs the event and
 * escalates to a full csi_hal::stop()/start() after 3 consecutive
 * failures.
 * ────────────────────────────────────────────────────────────────────────── */

constexpr uint32_t WATCHDOG_ESCALATE_AFTER = 3;
static uint32_t s_watchdog_consecutive = 0;

static void on_csi_watchdog(uint32_t silent_ms, uint32_t attempt) {
  ++s_watchdog_consecutive;
  Serial.printf("[csi.watchdog] silent %ums, attempt %u (consecutive %u)\n",
                (unsigned)silent_ms, (unsigned)attempt,
                (unsigned)s_watchdog_consecutive);

  if (s_watchdog_consecutive >= WATCHDOG_ESCALATE_AFTER) {
    Serial.printf("[csi.watchdog] escalating: csi_hal stop/start\n");
    csi_hal::stop();
    csi_hal::start();
    s_watchdog_consecutive = 0;
  }
}

}  /* namespace */

/* ──────────────────────────────────────────────────────────────────────────
 * STRONG OVERRIDES — csi_module_settings_*
 *
 * The library declares each accessor `__attribute__((weak))` returning the
 * caller's default. Here we look up the canonical full key, map to the
 * short NVS key, and read the persisted value. Read-only Preferences
 * handles are opened per call — settings reads are infrequent (boot +
 * post-POST reinit) so the small open/close cost is fine.
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" int32_t csi_module_settings_int(const csi_module_settings_t*,
                                           const char* key,
                                           int32_t default_value) {
  if (!key) return default_value;
  const char* nvs_key = nvs_key_for(key);
  if (!nvs_key) return default_value;
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/true)) return default_value;
  int32_t v = prefs.getInt(nvs_key, default_value);
  prefs.end();
  return v;
}

extern "C" bool csi_module_settings_bool(const csi_module_settings_t*,
                                         const char* key,
                                         bool default_value) {
  if (!key) return default_value;
  const char* nvs_key = nvs_key_for(key);
  if (!nvs_key) return default_value;
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/true)) return default_value;
  bool v = prefs.getBool(nvs_key, default_value);
  prefs.end();
  return v;
}

extern "C" float csi_module_settings_float(const csi_module_settings_t*,
                                           const char* key,
                                           float default_value) {
  if (!key) return default_value;
  const char* nvs_key = nvs_key_for(key);
  if (!nvs_key) return default_value;
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/true)) return default_value;
  float v = prefs.getFloat(nvs_key, default_value);
  prefs.end();
  return v;
}

/* ──────────────────────────────────────────────────────────────────────────
 * PUBLIC BRIDGE
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" bool securacv_csi_modules_init(void) {
  if (s_initialized) {
    /* Re-running init re-reads NVS — it's a deliberate path used after a
     * /api/settings POST. The module dispatcher tolerates a second
     * register_module() of the same id by replacing the prior entry. */
  }

  csi_event_set_privacy_ceiling(CSI_PRIVACY_P0);

  csi_module_register(core_presence_module());
  csi_module_register(core_breathing_module());
  csi_module_register(core_activity_ribbon_module());
  csi_module_register(meta_daily_summary_module());
  csi_module_register(anomaly_baseline_module());
  /* Ambient, unattributed "the airwaves near me just got busy" glow — reads
   * only identity-free CSI aggregates, emits CSI_CATEGORY_AMBIENT (never
   * persisted; live UI only). See wifi_channel_activity.{h,cpp} and
   * spec/canary_free_signals_v0.md §3.2 + Invariants A/E/F. */
  csi_module_register(wifi_channel_activity_module());
  /* Multi-link motion confirmation (PR 3) — promotes single-link
   * "observed" to multi-link "confirmed" when ≥2 links agree within
   * a 3-second window. PR 3 added the module but never wired it; this
   * PR closes that gap alongside meta.empty_room_baseline. */
  csi_module_register(core_multilink_fusion_module());
  /* Scheduled 10-min empty-room baseline calibration (PR 4a). */
  csi_module_register(meta_empty_room_baseline_module());
  /* system.integrity — per-kind tamper narration (watchdog / power_loss /
   * unexpected_reboot) through the chokepoint, same module the canary-wap
   * sketch registers. Unconditional: every board has a reset reason, and
   * a board without an SD state machine simply never sees SD transitions.
   * Fed from main.cpp's loop() via securacv_csi_modules_tamper_watch(). */
  csi_module_register(tamper_events_module());

#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
  /* BLE Scout — paired-beacon room-attribution (PR 5b). Lives in
   * the securacv_ble_scan library; only registered when the build
   * opts in via -DFEATURE_BLE_SCAN=1 (and pulls in NimBLE-Arduino
   * via the [env:full] lib_deps). The module's csi_event_decl_t
   * manifest constrains every emit to state_name/note/time_bucket
   * — no MAC or hashed_id ever lands in an event payload. */
  csi_module_register(ble_scout::ble_scout_module());
  /* Load the per-device key + start the NimBLE passive scan loop.
   * Safe to call even if NimBLE isn't actually available — the
   * scan-loop TU is an empty file in that case and ble_scout_init
   * still wires up the registry+tracker. */
  ble_scout::ble_scout_init();

#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
  /* PR 5c integration — wire the Scout broadcast hook to the mesh
   * sender, and install a receiver handler for inbound BEACON_EVENT
   * frames from peer Scouts. Create the FreeRTOS queue first so the
   * broadcast callback has somewhere to push to from the NimBLE host
   * task. Idempotent across re-init (securacv_csi_modules_init may
   * re-run after a /api/settings POST — keep the existing queue
   * rather than orphaning any in-flight events). */
  if (__atomic_load_n(&s_outbound_queue, __ATOMIC_ACQUIRE) == nullptr) {
    QueueHandle_t q = xQueueCreate(OUTBOUND_QUEUE_CAP,
                                   sizeof(OutboundBeaconEvent));
    QueueHandle_t expected = nullptr;
    if (q != nullptr && !__atomic_compare_exchange_n(
            &s_outbound_queue, &expected, q,
            false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
      vQueueDelete(q);
    }
  }
  ble_scout::set_broadcast_callback(&on_scout_beacon_event_outbound);
  mesh_session::set_beacon_event_handler(&on_peer_beacon_event_inbound);
#endif
#endif  /* FEATURE_BLE_SCAN */

#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
  mesh_session::set_channel_lock_handler(&on_peer_channel_lock);
  mesh_session::set_hub_election_handler(&on_peer_hub_election);
  /* Seed the elected-hub cache from NVS so a rebooted node already
   * knows the coordinator and de-dups the first redundant election. */
  restore_elected_hub_from_nvs();
#endif

  csi_hal::set_watchdog(csi_hal::WATCHDOG_DEFAULT_TIMEOUT_MS,
                        &on_csi_watchdog);

  s_initialized = true;
  return true;
}

extern "C" void securacv_csi_modules_feed(const void* features_blob) {
  if (!s_initialized || !features_blob) return;

  s_watchdog_consecutive = 0;

#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN \
    && defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
  /* Drain the outbound beacon queue BEFORE running modules so that
   * any events the previous main-loop pass enqueued (or that the
   * NimBLE host task enqueued asynchronously) get broadcast on the
   * same tick. The drain runs on the main loop — same task as the
   * CSI dispatcher — satisfying mesh_session::send_beacon_event's
   * threading contract. */
  drain_outbound_beacon_queue();
#endif

#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK
  channel_hop_tick((uint32_t)millis());
#endif

  /* Both csi_features_t structs are layout-identical (same field order,
   * same int8/uint16/uint8 widths). The library's csi_types.h
   * static-asserts the 32-byte vector width and we trust the canary HAL
   * to honor the documented contract — this cast is the bridge.
   *
   * The size assertion guarding this lives in main.cpp where both types
   * are visible (this TU only sees the common one). */
  const csi_features_t* f = static_cast<const csi_features_t*>(features_blob);
  csi_module_tick_all(f);
  /* Drain bundled events whose 10-minute window has elapsed. The
   * bundler buffers same-state observations and commits one row per
   * window; this call is what makes the long tail land. */
  csi_event_flush_bundles();
}

extern "C" void securacv_csi_modules_tamper_watch(int reset_was_crash,
                                                  int reset_was_watchdog,
                                                  int reset_was_brownout,
                                                  uint8_t sd_state) {
  /* Pass-through, deliberately NOT gated on s_initialized: the watcher is
   * designed to be called before registration (bounded retry inside the
   * module — an emit before the module exists is silently dropped by the
   * chokepoint), and gating here would eat the boot story on a build
   * where CSI init runs late or fails. */
  tamper_events_watch(reset_was_crash, reset_was_watchdog,
                      reset_was_brownout, sd_state);
}

extern "C" void securacv_csi_modules_deinit(void) {
  s_initialized = false;
}
