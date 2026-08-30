/**
 * @file csi_integration.cpp
 * @brief Host-side wiring of the SecuraCV CSI library into canary-wap.
 *
 * Responsibilities:
 *   1. Register the four v1 modules (core.presence, core.breathing,
 *      core.activity_ribbon, meta.daily_summary) at boot.
 *   2. Initialize csi_hal and route every CSI features window through
 *      csi_module_tick_all() AND (optionally) the legacy
 *      rf_presence::feed_csi_window() path.
 *   3. Maintain an in-memory snapshot of the most recently committed event
 *      via the strong override of csi_event_on_committed().
 *   4. Serve four HTTP endpoints:
 *        GET  /api/csi/stream     polling-friendly snapshot @ 1 Hz cadence
 *        GET  /api/csi/window     latest 32-dim feature window (P2-gated)
 *        GET  /api/events/today   list of today's committed events
 *        POST /api/events/dismiss local-only "that was nothing" feedback
 *
 * Why polling and not Server-Sent Events?
 *   ESP-IDF httpd holds a worker per request until the handler returns.
 *   True long-lived SSE requires the async-handler API and validation on
 *   actual hardware before we can confidently land it; polling at the
 *   library's natural 1-Hz cadence covers the v1 needs (live orb, Today
 *   sheet, Python listener) without that risk. SSE upgrade is tracked as
 *   a Phase 4 follow-up.
 *
 * Witness-chain integration is wired below: the strong override of
 * csi_event_commit_witness routes every committed P0/P1 event through
 * canary_wap.ino's create_witness_record path so the event is
 * Ed25519-signed and hash-chained. P2 never reaches that hook (the
 * chokepoint gates it).
 */

#include "csi_integration.h"
#include "tune_ui.h"
#include "csi_mqtt.h"             // optional MQTT bridge (publishes events)
#include "csi_event_log.h"        // SD-backed event persistence + backfill
#include "api_auth.h"             // api_auth_check() — Bearer token gate

#include <Arduino.h>
#include <Preferences.h>          // NVS-backed settings store
#include <esp_http_server.h>
#include <esp_random.h>           // esp_fill_random() — pairing token entropy
#include <string.h>
#include <stdlib.h>

#include <csi_hal.h>
#include <csi_features.h>
#include <csi_probe.h>
#include <csi_types.h>
#include <csi_module.h>
#include <csi_event.h>
#include <csi_bundler.h>          // snapshot_open() — live rows for /api/events/today

/* The four v1 modules ship with the library. After the Phase-4 flattening
 * (see commit notes) these live at the library root rather than in a
 * modules/ subdir, so that arduino-cli's library-1.5 root-only compile
 * picks up their .cpp files without needing a src/ tree. */
#include <core_presence.h>
#include <core_breathing.h>
#include <core_activity_ribbon.h>
#include <core_multilink_fusion.h>
#include <meta_daily_summary.h>
#include <meta_quiet_hours.h>
#include <meta_empty_room_baseline.h>
#include <anomaly_baseline.h>
#include <wifi_channel_activity.h>
#include <ble_events_module.h>
#include "acoustic_events_module.h"

#include "build_config.h"
#if FEATURE_VAULT_SNAPSHOT
#include "vault_events_module.h"
#include "tamper_events_module.h"
#endif
#if FEATURE_BLE_SCAN
#include "ble_scout.h"
#endif

/* PR 5c integration — wire the Scout broadcast hook to the mesh
 * sender, and install a receiver handler for inbound BEACON_EVENT
 * frames. mesh_network.h owns both APIs; only pulled in when both
 * feature flags are live so dev/minimal builds stay lean. */
#if FEATURE_BLE_SCAN && FEATURE_MESH_NETWORK
#include "mesh_network.h"
#include "mesh_beacon.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#endif

/* PR 4b integration — channel-hop coordinator. Needs mesh_network
 * (for send_channel_lock + set_channel_lock_handler), csi_hal (for
 * set_channel_lock on receive), airtime_governor (for utilization),
 * and the channel-hop wire format + HopTracker. Gated on just
 * FEATURE_MESH_NETWORK — no BLE dependency. */
#if FEATURE_MESH_NETWORK
#if !(FEATURE_BLE_SCAN)
#include "mesh_network.h"
#endif
#include "mesh_channel_hop.h"
#include "mesh_hub_election.h"
#include "airtime_governor.h"
#include <csi_hal.h>
#include <csi_probe.h>
#include <core_multilink_fusion.h>
#endif

namespace {

bool                                    g_initialized        = false;
csi_integration::legacy_features_hook_t g_legacy_hook        = nullptr;
csi_features_t                          g_latest_window      = {};
bool                                    g_have_latest_window = false;
uint32_t                                g_stream_started_ms  = 0;
uint32_t                                s_watchdog_consecutive = 0;
/* True iff csi_hal::init() succeeded during csi_integration::init().
 * When false the HTTP routes are still registered (so the dashboard
 * doesn't 404) but handle_stream returns a "sensing_unavailable"
 * payload so the user sees a clear error instead of a stuck orb. */
bool                                    g_hal_ready          = false;
/* Bearer token expected on all CSI HTTP requests. Pointer into the
 * caller's storage (g_device.api_token_str) — never freed. */
const char*                             g_api_token          = nullptr;
/* Defined with the probe pump further down; handle_stream's supply
 * diagnostics need it first. */
bool probe_running();

/* Forward decl of the file-scope cv_session_validate trampoline (defined
 * just below the anonymous namespace, near session_validate_cookie). The
 * macro CSI_AUTH_OR_RETURN expands inside handlers that live in this
 * anonymous namespace, but unqualified name lookup falls through to the
 * enclosing global scope, so this declaration is found and the linker
 * resolves it to the file-scope definition. We can't write
 * `namespace csi_integration { ... }` HERE because that would create a
 * nested namespace <anonymous>::csi_integration distinct from the
 * file-scope one (different mangled names → linker error). */
}  /* namespace (anonymous) — close briefly to put the decl at file scope */

bool cv_session_validate(httpd_req_t* req);

namespace {  /* re-open anonymous so the rest of the original block continues */

/* Auth guard: drop into every handler at the very top. Mirrors the
 * handle_*_auth pattern used by /api/status, /api/chain, etc. The
 * macro form keeps the per-handler diff to one line.
 *
 * Two valid auth paths:
 *   - cv_session cookie (HttpOnly, SameSite=Strict, set by handle_ui
 *     after a one-shot pair-token URL hand-off). This is the dashboard's
 *     normal route — browsers send the cookie automatically with every
 *     /api/* fetch, so the token never appears in HTML source for any
 *     in-page script (or page-source viewer) to harvest.
 *   - Bearer header carrying the device's persistent api_token. Reserved
 *     for tooling: the Python listener, the canary-vision fleet UI, and
 *     similar. Same token surface as /api/status, /api/chain, etc.
 *
 * Failure paths:
 *   - g_api_token unset (init() refuses null tokens, so structurally
 *     unreachable today; defensive 503 protects future refactors that
 *     might register a route before init() completes).
 *   - Neither cookie nor Bearer valid → 401 + WWW-Authenticate. The
 *     dashboard at / catches this and re-routes through the pair flow. */
#define CSI_AUTH_OR_RETURN(req)                                       \
  do {                                                                \
    if (!g_api_token) {                                               \
      httpd_resp_set_status((req), "503 Service Unavailable");        \
      httpd_resp_set_type((req), "application/json");                 \
      httpd_resp_sendstr((req),                                       \
        "{\"error\":\"auth_unconfigured\","                           \
         "\"hint\":\"csi_integration::init never received an "        \
                   "api_token\"}");                                   \
      return ESP_OK;                                                  \
    }                                                                 \
    if (cv_session_validate((req))) break;                            \
    if (api_auth_check_optional((req), g_api_token)) break;           \
    httpd_resp_set_status((req), "401 Unauthorized");                 \
    httpd_resp_set_type((req), "application/json");                   \
    httpd_resp_set_hdr((req), "WWW-Authenticate",                     \
                       "Bearer realm=\"securacv\"");                  \
    httpd_resp_sendstr((req),                                         \
      "{\"error\":\"unauthorized\","                                  \
       "\"hint\":\"open / on the canary AP to pair\"}");              \
    return ESP_OK;                                                    \
  } while (0)

/* Privacy Budget byte counter. Increments only when host code calls
 * csi_integration::add_outbound_bytes() — i.e. when bytes go to a
 * destination outside the user's immediate network. The dashboard
 * polls this via GET /api/privacy-budget. Resets at boot for now;
 * a future wall-clock-aware reset hooks the same place. */
uint32_t                                g_outbound_bytes      = 0;

/* Indices into csi_features_t::v for the two bands the snapshot fallback
 * surfaces. Layout is documented in csi_features.h:11-18:
 *   v[0..7]   amplitude variance
 *   v[8..11]  phase-Doppler  (4 bands → motion)
 *   v[12..19] breathing FFT  (8 bins  → micro-motion / breath rhythm)
 *   v[20..23] RSSI stats
 *   v[24..31] frame health + reserved
 * Mirrored, deliberately by-value, in core_presence.cpp / core_breathing.cpp. */
constexpr int IDX_DOPPLER_BASE   = 8;
constexpr int IDX_DOPPLER_COUNT  = 4;
constexpr int IDX_BREATHING_BASE = 12;
constexpr int IDX_BREATHING_COUNT = 8;

/* ──────────────────────────────────────────────────────────────────────────
 * MOST-RECENT-EVENT SNAPSHOT
 *
 * Updated by the strong override of csi_event_on_committed() so the
 * /api/csi/stream snapshot endpoint always has the freshest published
 * state without needing to walk the ring.
 * ────────────────────────────────────────────────────────────────────────── */

struct Snapshot {
  bool                  valid;
  uint32_t              event_id;
  uint32_t              committed_ms;     /* monotonic; converted to relative t */
  csi_event_category_t  category;
  csi_privacy_class_t   privacy;
  csi_event_values_t    values;
  char                  module_id[CSI_EVENT_NAME_MAX];
  char                  type_name[CSI_EVENT_NAME_MAX];
};

Snapshot g_snapshot = {};

/* ──────────────────────────────────────────────────────────────────────────
 * THRESHOLD CALIBRATION
 *
 * The default core.presence thresholds (motion=35, active=75, breathing=30)
 * are tuned for a "typical" mid-noise apartment. Users in quieter RF
 * environments (rural homes, single-occupant studios, screened rooms) see
 * the orb sit on "Sensing…" forever because their ambient never crosses
 * the floor. Calibration solves that by sampling the room with no person
 * moving for ~10 s, then proposing thresholds that sit a margin above
 * the observed ambient noise.
 *
 * The state machine is driven by on_csi_window: when state == RUNNING we
 * accumulate motion / breathing magnitudes, track the per-window max +
 * running sum, and after CALIB_WINDOWS samples compute proposed
 * thresholds. The dashboard polls /api/csi/calibrate/status to render the
 * progress bar and the proposed-vs-current diff, then POSTs to
 * /api/csi/calibrate/apply to persist the proposal.
 *
 * Layout note: the indices below mirror the ones used by handle_stream
 * (IDX_DOPPLER_BASE / IDX_BREATHING_BASE) but the calibration uses the
 * same reduce-to-magnitude transform as core_presence.cpp so the
 * proposed thresholds compare apples-to-apples with what the module
 * actually sees at runtime. ────────────────────────────────────────── */

constexpr uint32_t CALIB_WINDOWS    = 10;     /* ~10 s at the 1 Hz library rate */
constexpr uint32_t CALIB_TIMEOUT_MS = 30UL * 1000UL;
/* Margin added above observed ambient max. Big enough that breath-of-pet
 * RF flicker doesn't sit at exactly the threshold; small enough that a
 * truly quiet room ends up with very sensitive thresholds. The "safe
 * floor" lower-bound (5) and upper-bound (120) match the same clamp the
 * Tuning Lab + module init path apply, so a calibration result is always
 * a valid coefficient value out of the box. */
constexpr int32_t  CALIB_MARGIN     = 10;
constexpr int32_t  CALIB_FLOOR      = 5;
constexpr int32_t  CALIB_CEILING    = 120;

enum CalibState : uint8_t {
  CALIB_IDLE = 0,
  CALIB_RUNNING,
  CALIB_READY,
  CALIB_TIMED_OUT,   /* Sampler started but no windows arrived (HAL stalled). */
};

struct Calibration {
  CalibState state;
  uint32_t   started_ms;
  uint32_t   samples;            /* count of windows accumulated */
  uint8_t    max_motion;         /* observed ambient peak */
  uint8_t    max_breathing;
  uint32_t   sum_motion;         /* for the dashboard's "average" readout */
  uint32_t   sum_breathing;
  /* Proposed thresholds, populated when state == CALIB_READY. */
  uint8_t    proposed_motion;
  uint8_t    proposed_active;
  uint8_t    proposed_breathing;
};

Calibration g_calibration = {};

uint8_t calib_clamp(int32_t v) {
  if (v < CALIB_FLOOR)   return (uint8_t)CALIB_FLOOR;
  if (v > CALIB_CEILING) return (uint8_t)CALIB_CEILING;
  return (uint8_t)v;
}

/* Same reduce-to-magnitude as core_presence.cpp::reduce_magnitude.
 * Keeping the math local avoids a cross-TU dependency on the module's
 * internals — if the module changes its v[] layout, the constants
 * IDX_DOPPLER_BASE etc. above already need updating in lockstep, so
 * this helper isn't gaining a coupling we don't already have. */
uint8_t calib_reduce(const int8_t* v, int from, int count) {
  int32_t sum = 0;
  for (int i = from; i < from + count; ++i) {
    int8_t s = v[i];
    sum += (s < 0) ? -(int32_t)s : (int32_t)s;
  }
  if (count <= 0) return 0;
  int32_t avg = sum / count;
  if (avg > 127) avg = 127;
  return (uint8_t)avg;
}

void calibration_finalize() {
  /* Proposal: ambient_max + CALIB_MARGIN, clamped to the same envelope
   * the Tuning Lab uses. The active threshold sits 40 above motion
   * (matches the +40 offset the "balanced" preset uses internally). */
  const int32_t prop_motion =
      (int32_t)g_calibration.max_motion + CALIB_MARGIN;
  const int32_t prop_active = prop_motion + 40;
  const int32_t prop_breath =
      (int32_t)g_calibration.max_breathing + CALIB_MARGIN;
  g_calibration.proposed_motion    = calib_clamp(prop_motion);
  g_calibration.proposed_active    = calib_clamp(prop_active);
  g_calibration.proposed_breathing = calib_clamp(prop_breath);
  g_calibration.state              = CALIB_READY;
}

/* Timeout accounting, separated from accumulation so it can run for
 * EVERY finalized window — including starved (<2 frame) ones that the
 * honesty gate keeps away from calibration_observe. Without this split
 * a frame-starved install left /api/csi/calibrate/status "running"
 * forever instead of reporting timed_out. */
void calibration_tick_timeout() {
  if (g_calibration.state != CALIB_RUNNING) return;
  if ((millis() - g_calibration.started_ms) >= CALIB_TIMEOUT_MS &&
      g_calibration.samples == 0) {
    g_calibration.state = CALIB_TIMED_OUT;
  }
}

void calibration_observe(const csi_features_t* features) {
  if (g_calibration.state != CALIB_RUNNING) return;
  /* Hard timeout in case the HAL stalls mid-calibration — without this
   * the dashboard would just spin forever on /status. */
  if ((millis() - g_calibration.started_ms) >= CALIB_TIMEOUT_MS &&
      g_calibration.samples == 0) {
    g_calibration.state = CALIB_TIMED_OUT;
    return;
  }
  /* IDX_DOPPLER_BASE/IDX_BREATHING_BASE are anonymous-namespace
   * constants further up; reuse them here. */
  const uint8_t m = calib_reduce(features->v,
      IDX_DOPPLER_BASE, IDX_DOPPLER_COUNT);
  const uint8_t b = calib_reduce(features->v,
      IDX_BREATHING_BASE, IDX_BREATHING_COUNT);
  if (m > g_calibration.max_motion)    g_calibration.max_motion    = m;
  if (b > g_calibration.max_breathing) g_calibration.max_breathing = b;
  g_calibration.sum_motion    += m;
  g_calibration.sum_breathing += b;
  g_calibration.samples++;
  if (g_calibration.samples >= CALIB_WINDOWS) calibration_finalize();
}

/* ──────────────────────────────────────────────────────────────────────────
 * CSI features callback — drives the module pipeline + legacy fusion.
 * ────────────────────────────────────────────────────────────────────────── */

void on_csi_window(const csi_features_t* features, void* /*user*/) {
  if (!features) return;
  s_watchdog_consecutive = 0;
  g_latest_window      = *features;
  g_have_latest_window = true;
  /* HONESTY GATE: a window with (almost) no frames is "no data", not
   * "empty room". Ticking the modules with an all-zeros vector would
   * advance core.presence toward a confident "empty" claim it cannot
   * back — with no home-WiFi beacons and no peer Canary probing, the
   * device would report every room as confidently empty forever. Skip
   * the pipeline; the dashboard's supply chip explains the starvation
   * (the stream endpoint carries fps + silent_ms). Calibration also
   * must not learn from starved windows — but its TIMEOUT accounting
   * must still tick, or a starved install would leave the calibrate
   * status "running" forever. */
  calibration_tick_timeout();
  if (features->frames_in_window < 2) return;
  /* Calibration runs in parallel with the normal module pipeline so a
   * user can hit Calibrate without disrupting live presence updates;
   * calibration_observe is a fast accumulator, no allocation. */
  calibration_observe(features);
  csi_module_tick_all(features);
  if (g_legacy_hook) g_legacy_hook(features);
}

/* ──────────────────────────────────────────────────────────────────────────
 * SETTINGS — NVS-backed module settings
 *
 * The library declares csi_module_settings_int/bool/float with weak
 * default symbols that just return the supplied default. We override
 * them here with a thin Preferences-backed reader so the dashboard's
 * Pet Mode toggle (and future preset / sensitivity controls) actually
 * change what the modules do at run-time.
 *
 * NVS key length limit is 15 chars, so we shorten the dotted module
 * keys to a stable abbreviation:
 *
 *   core.presence.pet_mode -> cp.pet_mode   (cp + dot + 8 = 11)
 *   core.presence.motion_threshold -> cp.mt (still valid, mapped below)
 *   ...
 *
 * The dashboard speaks in dotted keys; this map is the only place that
 * knows about the abbreviation, so future setting-key additions touch
 * one table.
 *
 * Defined here (above HTTP HANDLERS) so the GET / POST handlers below
 * can reference SETTINGS_NS, nvs_key_for(), and reinit_module() without
 * forward declarations.
 * ────────────────────────────────────────────────────────────────────────── */

constexpr const char* SETTINGS_NS = "csi";

struct SettingKey {
  const char* full;   // "core.presence.pet_mode"
  const char* nvs;    // "cp.pet_mode" — must be ≤ 15 chars
};
const SettingKey SETTING_KEYS[] = {
  { "core.presence.pet_mode",            "cp.pet_mode"   },
  /* Tier-3 dashboard surface: preset (0=sensitive, 1=balanced,
   * 2=quiet) + sensitivity slider (0..100). Module reads these and
   * computes the three thresholds below; users who change the
   * dashboard's preset / slider land here. */
  { "core.presence.preset",              "cp.preset"     },
  { "core.presence.sensitivity",         "cp.sens"       },
  /* Per-coefficient overrides (Tuning Lab path, Tier 4): if any of
   * these are explicitly set in NVS they win over the preset
   * baseline. Default value supplied at init() is the
   * preset+sensitivity-derived baseline so common-case users
   * never trip these. */
  { "core.presence.motion_threshold",    "cp.mt"         },
  { "core.presence.active_threshold",    "cp.at"         },
  { "core.presence.breathing_threshold", "cp.bt"         },
  { "core.presence.pet_mode_seconds",    "cp.ps"         },
  /* shimmer filter */
  { "core.presence.shimmer_rssi_swing",  "cp.srs"        },
  { "core.presence.shimmer_doppler_floor","cp.sdf"       },
  { "core.presence.shimmer_enabled",     "cp.se"         },
  { "core.breathing.lock_threshold",     "cb.lt"         },
  { "core.breathing.confirm_seconds",    "cb.cs"         },
  /* Quiet Hours — a single time range (minutes-of-day, 0..1439) that
   * the dashboard renders as dimmed ribbon cells and that future
   * notification / anomaly modules can consult to suppress alerts.
   * The setting is forward-compat scaffolding for PR 7 and beyond;
   * today its only visible effect is the dimmed ribbon. */
  { "core.quiet_hours.enabled",          "qh.en"         },
  { "core.quiet_hours.start_min",        "qh.start"      },
  { "core.quiet_hours.end_min",          "qh.end"        },
  /* Privacy ceiling. P0 (default, anti-snitch) blocks anything more
   * detailed than coarse state names. P1 lets per-event scores leave
   * the device. P2 unlocks the raw 32-dim feature window and the
   * Tuning Lab. Stored as int (0/1/2) so the apply_*_from_nvs helper
   * can use Preferences::getInt with a sane fallback. */
  { "core.privacy_ceiling",              "cp.pc"         },
  /* Anomaly baseline — out-of-pattern detector tunables. Defaults
   * cover a quiet home; Tuning Lab (PR 10) exposes them as sliders. */
  { "anomaly.baseline.spike_ratio",      "ab.sr"         },
  { "anomaly.baseline.min_motion",       "ab.mm"         },
  { "anomaly.baseline.min_breathing",    "ab.mb"         },
  { "anomaly.baseline.cooldown_sec",     "ab.cd"         },
};

const char* nvs_key_for(const char* full_key) {
  for (const SettingKey& k : SETTING_KEYS) {
    if (strcmp(k.full, full_key) == 0) return k.nvs;
  }
  return nullptr;
}

/* Reinit modules whose settings changed. Cheap — modules are stateless
 * apart from a few static counters that init() resets, and there are
 * only four registered. Called once after each /api/settings POST. */
void reinit_module(const char* module_id) {
  const csi_module_t* m = csi_module_find(module_id);
  if (!m) return;
  if (m->deinit) m->deinit();
  if (m->init)   m->init(nullptr);
}

/* Read the persisted Quiet Hours range from NVS and push it into the
 * chokepoint. Called both at boot (register_v1_modules) and on
 * /api/settings POST. Defaults match the dashboard's UI defaults
 * (23:00 → 07:00) so a never-set device is congruent with what a
 * fresh installer sees. The chokepoint setter is a pure state update
 * — held-summary flushing happens on the next emit, not here. */
void apply_quiet_hours_from_nvs() {
  Preferences qprefs;
  if (!qprefs.begin(SETTINGS_NS, /*readOnly=*/true)) return;
  const bool    qh_en    = qprefs.getBool("qh.en",    false);
  const int32_t qh_start = qprefs.getInt ("qh.start", 23 * 60);
  const int32_t qh_end   = qprefs.getInt ("qh.end",    7 * 60);
  qprefs.end();
  csi_event_set_quiet_window((uint16_t)qh_start, (uint16_t)qh_end, qh_en);
}

/* Restore the persisted privacy ceiling at boot. Without this every
 * reboot reverts to P0 and the user has to re-consent to P1/P2 every
 * power cycle, which made the Tuning Lab effectively unreachable.
 * Default is P0 (privacy-first) — any out-of-range value falls back to
 * P0 rather than silently elevating to a more permissive level. */
void apply_privacy_ceiling_from_nvs() {
  Preferences pprefs;
  if (!pprefs.begin(SETTINGS_NS, /*readOnly=*/true)) return;
  const int32_t raw = pprefs.getInt("cp.pc", (int32_t)CSI_PRIVACY_P0);
  pprefs.end();
  csi_privacy_class_t ceiling;
  switch (raw) {
    case (int32_t)CSI_PRIVACY_P1: ceiling = CSI_PRIVACY_P1; break;
    case (int32_t)CSI_PRIVACY_P2: ceiling = CSI_PRIVACY_P2; break;
    default:                      ceiling = CSI_PRIVACY_P0; break;
  }
  csi_event_set_privacy_ceiling(ceiling);
}

/* ──────────────────────────────────────────────────────────────────────────
 * EVENT-ID FLOOR — NVS persistence
 *
 * Lifts the cross-reboot collision in event_id allocation. csi_event
 * starts from g_next_event_id = 1 every boot, so a previous-boot id=50
 * and a current-boot id=50 are indistinguishable to anything that
 * tracks ids — most notably csi_mqtt's reconnect-backfill watermark.
 * PR #395 worked around it by clearing the SD log on cold boot. This
 * commit removes that workaround by persisting the allocator's next-
 * id to NVS and restoring at boot.
 *
 * Persist cadence: every CSI_ID_PERSIST_STRIDE allocations we write
 * "current next_id + STRIDE" to NVS. After a reboot we restore from
 * that persisted value, then continue from there. Worst case we skip
 * up to STRIDE ids (never reuse one), and NVS write traffic stays
 * bounded — at the per-module hourly ceiling (~6 events/hour) and
 * STRIDE=10 we churn ~14 NVS writes/day, well inside the cell wear
 * budget. ────────────────────────────────────────────────────────── */

constexpr const char*    NVS_KEY_EVENT_ID = "ev.next";
constexpr uint32_t       CSI_ID_PERSIST_STRIDE = 10;
uint32_t                 g_id_persisted_at = 0;

void apply_event_id_floor_from_nvs() {
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/true)) return;
  const uint32_t persisted = (uint32_t)prefs.getULong(NVS_KEY_EVENT_ID, 0);
  prefs.end();
  if (persisted > 0) {
    csi_event_set_event_id_floor(persisted);
    g_id_persisted_at = persisted;
  }
}

void persist_event_id_floor(uint32_t next_id) {
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/false)) return;
  /* Persist next_id + STRIDE so a reboot between persists at most
   * skips STRIDE ids forward but never rewinds into the live range. */
  prefs.putULong(NVS_KEY_EVENT_ID, (unsigned long)(next_id + CSI_ID_PERSIST_STRIDE));
  prefs.end();
  g_id_persisted_at = next_id;
}

/* ──────────────────────────────────────────────────────────────────────────
 * HTTP HANDLERS
 * ────────────────────────────────────────────────────────────────────────── */

esp_err_t handle_stream(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  /* Polling-friendly snapshot. Returns the most recently committed event,
   * or an "ambient" record derived from the latest feature window if no
   * event has fired yet. The client reconnects once per second.
   *
   * The wire format mirrors what a future SSE upgrade will emit, so the
   * Python listener and the dashboard work unchanged when SSE lands. */
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  /* HAL never came up — be honest about it instead of returning the same
   * "sensing" fallback as a healthy-but-quiet device. The dashboard maps
   * status:"unavailable" to a clear "Sensing offline" plate. */
  if (!g_hal_ready) {
    const char* body = "{\"t\":0,\"status\":\"unavailable\","
                       "\"reason\":\"hal_init_failed\","
                       "\"category\":\"ambient\",\"state\":\"sensing\"}";
    httpd_resp_send(req, body, -1);
    return ESP_OK;
  }

  /* Signal-supply diagnostics, present in every stream response: the
   * dashboard's supply chip needs to distinguish "healthy but quiet room"
   * (fps fine, no motion) from "sensing starved of frames" (fps ≈ 0 —
   * no home WiFi beacons and no peer Canary probing). fps is the frame
   * count of the last finalized 1 s window ≡ frames/second. */
  char supply[96];
  {
    /* The never-got-a-frame sentinel UINT32_MAX intentionally prints as
     * -1 through the int32 cast; the dashboard treats any negative as
     * "no frames yet". */
    const uint32_t silent = csi_hal::get_ms_since_last_frame();
    snprintf(supply, sizeof(supply),
      "\"supply\":{\"fps\":%u,\"probe\":%s,\"silent_ms\":%d}",
      (unsigned)(g_have_latest_window ? g_latest_window.frames_in_window : 0),
      probe_running() ? "true" : "false",
      (int)(int32_t)silent);
  }

  char buf[768];
  if (g_snapshot.valid) {
    const Snapshot* s = &g_snapshot;
    const char* cat = (s->category == CSI_CATEGORY_AMBIENT) ? "ambient"
                    : (s->category == CSI_CATEGORY_ANOMALY) ? "anomaly" : "event";
    const char* priv = (s->privacy == CSI_PRIVACY_P0) ? "p0"
                     : (s->privacy == CSI_PRIVACY_P1) ? "p1" : "p2";
    snprintf(buf, sizeof(buf),
      "{"
        "\"t\":%lu,"
        "\"id\":%lu,"
        "\"module\":\"%s\","
        "\"type\":\"%s\","
        "\"category\":\"%s\","
        "\"privacy\":\"%s\","
        "\"state\":\"%s\","
        "\"confidence\":\"%s\","
        "\"motion\":%u,"
        "\"breathing\":%u,"
        "\"bpm\":%u,"
        "\"duration_sec\":%u,"
        "\"bundled\":%u,"
        "\"time_bucket\":%u,"
        "%s"
      "}",
      /* `t` is the relative seconds at which THIS event was committed, not
       * the time of the HTTP request — otherwise consecutive polls of the
       * same event_id would tick `t` upward and break client-side duration
       * math. */
      (unsigned long)((s->committed_ms - g_stream_started_ms) / 1000u),
      (unsigned long)s->event_id, s->module_id, s->type_name, cat, priv,
      s->values.state_name, s->values.confidence,
      (unsigned)s->values.motion_score,
      (unsigned)s->values.breathing_score,
      (unsigned)s->values.breathing_rate_bpm,
      (unsigned)s->values.duration_sec,
      (unsigned)s->values.bundled_count,
      (unsigned)s->values.time_bucket,
      supply
    );
  } else {
    /* No committed event yet — surface the latest raw window's two scalars
     * so the dashboard's orb has something honest to render at boot. */
    uint8_t motion = 0, breathing = 0;
    if (g_have_latest_window) {
      int32_t m = 0, b = 0;
      for (int i = IDX_DOPPLER_BASE;
           i < IDX_DOPPLER_BASE + IDX_DOPPLER_COUNT; ++i) {
        m += abs((int)g_latest_window.v[i]);
      }
      for (int i = IDX_BREATHING_BASE;
           i < IDX_BREATHING_BASE + IDX_BREATHING_COUNT; ++i) {
        b += abs((int)g_latest_window.v[i]);
      }
      const int32_t m_avg = m / IDX_DOPPLER_COUNT;
      const int32_t b_avg = b / IDX_BREATHING_COUNT;
      motion    = (uint8_t)(m_avg > 100 ? 100 : m_avg);
      breathing = (uint8_t)(b_avg > 100 ? 100 : b_avg);
    }
    snprintf(buf, sizeof(buf),
      "{\"t\":%lu,\"category\":\"ambient\",\"state\":\"sensing\","
       "\"confidence\":\"tentative\","
       "\"motion\":%u,\"breathing\":%u,%s}",
      (unsigned long)((millis() - g_stream_started_ms) / 1000u),
      (unsigned)motion, (unsigned)breathing, supply);
  }
  httpd_resp_send(req, buf, -1);
  return ESP_OK;
}

esp_err_t handle_window(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  /* Raw 32-dim feature vector. P2 only — the chokepoint enforces the
   * privacy ceiling, so unauthorized callers get a 403 with no data leak. */
  if (csi_event_get_privacy_ceiling() < CSI_PRIVACY_P2) {
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_send(req,
      "{\"error\":\"raw window requires P2 privacy ceiling\"}", -1);
    return ESP_OK;
  }
  if (!g_have_latest_window) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
  }
  char buf[768];
  int  off = snprintf(buf, sizeof(buf),
    "{\"frames\":%u,\"time_bucket\":%u,\"v\":[",
    (unsigned)g_latest_window.frames_in_window,
    (unsigned)g_latest_window.time_bucket);
  for (int i = 0; i < CSI_FEATURE_DIM; ++i) {
    off += snprintf(buf + off, sizeof(buf) - off, "%s%d",
                    i ? "," : "", (int)g_latest_window.v[i]);
    if (off >= (int)sizeof(buf) - 8) break;
  }
  snprintf(buf + off, sizeof(buf) - off, "]}");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, -1);
  return ESP_OK;
}

esp_err_t handle_events_today(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  /* Walk the in-memory ring; emit at most 64 newest rows.
   *
   * The 64-row snapshot is ~7.5 KB (csi_event_record_t is ~120 B). A stack
   * buffer that large would blow the httpd task stack, but a plain static
   * array lands in internal DRAM .bss — the segment the FULL build was
   * overflowing (`region 'dram0_0_seg' overflowed by 64 bytes`). Park the
   * scratchpad in PSRAM instead (the XIAO ESP32-S3 ships 8 MB OPI PSRAM,
   * pinned on in sketch.yaml), falling back to internal RAM on parts without
   * PSRAM. Allocated once and retained for the process lifetime — function-
   * ally identical to the old static buffer, just no longer charged against
   * dram0_0_seg. */
  static constexpr size_t kEventRows = 64;
  static csi_event_record_t* buffer = nullptr;
  if (buffer == nullptr) {
    buffer = (csi_event_record_t*)ps_malloc(kEventRows * sizeof(csi_event_record_t));
    if (buffer == nullptr) {
      buffer = (csi_event_record_t*)malloc(kEventRows * sizeof(csi_event_record_t));
    }
    if (buffer == nullptr) {
      httpd_resp_set_status(req, "500 Internal Server Error");
      return httpd_resp_send(req, "{\"ok\":false,\"reason\":\"oom\"}", -1);
    }
  }
  size_t n = csi_event_recent(buffer, kEventRows);

  /* OPEN bundles ride the same response, ahead of the committed ring: an
   * alarm mid-bundle is the most current thing this endpoint knows, and
   * before this block it was invisible for up to the 10-minute window
   * (review on the first phone client). snapshot_open() hands back
   * consistent copies under the bundler's mutex — never live slots. */
  csi_event_record_t open_rows[8];
  const size_t nopen = csi_bundler_snapshot_open(
      open_rows, sizeof(open_rows) / sizeof(open_rows[0]));

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send_chunk(req, "{\"events\":[", 11);

  bool first = true;
  /* One serializer for both kinds of row: `open` is 1 while the bundle is
   * still collecting (live), 0 for a committed ring row (history). Every
   * key appears on every row — clients decode one shape. */
  auto emit_row = [&](const csi_event_record_t* r, unsigned open_flag) {
    if (r->event_id == 0) return;
    if (r->privacy > csi_event_get_privacy_ceiling()) return;
    char row[400];
    const char* cat = (r->category == CSI_CATEGORY_AMBIENT) ? "ambient"
                    : (r->category == CSI_CATEGORY_ANOMALY) ? "anomaly" : "event";
    const int len = snprintf(row, sizeof(row),
      "%s{"
        "\"id\":%lu,"
        "\"module\":\"%s\","
        "\"type\":\"%s\","
        "\"category\":\"%s\","
        "\"state\":\"%s\","
        "\"confidence\":\"%s\","
        "\"motion\":%u,"
        "\"breathing\":%u,"
        "\"bpm\":%u,"
        "\"duration_sec\":%u,"
        "\"bundled\":%u,"
        "\"time_bucket\":%u,"
        "\"dismissed\":%u,"
        "\"open\":%u"
      "}",
      first ? "" : ",",
      (unsigned long)r->event_id,
      r->module_id, r->type_name, cat,
      r->values.state_name, r->values.confidence,
      (unsigned)r->values.motion_score,
      (unsigned)r->values.breathing_score,
      (unsigned)r->values.breathing_rate_bpm,
      (unsigned)r->values.duration_sec,
      (unsigned)r->bundled_count,
      (unsigned)r->values.time_bucket,
      (unsigned)r->values.dismissed,
      open_flag);
    if (len > 0 && len < (int)sizeof(row)) {
      httpd_resp_send_chunk(req, row, len);
      first = false;
    }
  };

  for (size_t i = 0; i < nopen; ++i) emit_row(&open_rows[i], 1u);
  for (size_t i = 0; i < n; ++i)     emit_row(&buffer[i], 0u);

  httpd_resp_send_chunk(req, "]}", 2);
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

esp_err_t handle_events_dismiss(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  /* Body is small JSON: {"event_id": <number>}. We parse with a tiny
   * scanner to avoid pulling ArduinoJson into this TU. */
  char body[96];
  const int got = httpd_req_recv(req, body, sizeof(body) - 1);
  if (got <= 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"empty body\"}", -1);
    return ESP_OK;
  }
  body[got] = '\0';
  const char* k = strstr(body, "event_id");
  if (!k) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"missing event_id\"}", -1);
    return ESP_OK;
  }
  const char* digit = k;
  while (*digit && (*digit < '0' || *digit > '9')) digit++;
  const uint32_t event_id = (uint32_t)strtoul(digit, nullptr, 10);
  const bool ok = csi_event_dismiss(event_id);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}", -1);
  return ESP_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * /api/csi/calibrate/{start,status,apply}
 *
 * Threshold auto-calibration. The dashboard guides the user through a
 * ~10 s observation of an empty / still room, then proposes thresholds
 * a margin above the observed ambient. The user accepts or cancels.
 *
 * Wire shape:
 *   POST /api/csi/calibrate/start   →  {"ok":true,"duration_sec":10}
 *   GET  /api/csi/calibrate/status  →
 *     while running:
 *       {"state":"running","samples":N,"target":10}
 *     when done:
 *       {"state":"ready","samples":10,
 *        "max_motion":M,"max_breathing":B,
 *        "proposed":{"motion":X,"active":Y,"breathing":Z},
 *        "current":{"motion":X0,"active":Y0,"breathing":Z0}}
 *     timed out (HAL not running):
 *       {"state":"timed_out"}
 *     never started:
 *       {"state":"idle"}
 *   POST /api/csi/calibrate/apply   →  {"ok":true}  (writes NVS, reinits
 *                                                   core.presence)
 * ────────────────────────────────────────────────────────────────────────── */

esp_err_t handle_calibrate_start(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  /* Reset the accumulator and arm. Calling start() while a previous
   * run is RUNNING / READY is fine — the user re-clicked Calibrate. */
  g_calibration = {};
  g_calibration.state      = CALIB_RUNNING;
  g_calibration.started_ms = millis();
  httpd_resp_set_type(req, "application/json");
  char buf[64];
  snprintf(buf, sizeof(buf),
    "{\"ok\":true,\"duration_sec\":%lu}",
    (unsigned long)CALIB_WINDOWS);  /* 1 Hz library rate → seconds = windows */
  httpd_resp_send(req, buf, -1);
  return ESP_OK;
}

esp_err_t handle_calibrate_status(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  /* Read the current persisted thresholds so the dashboard can render
   * a "before / after" diff without an extra fetch. We read NVS rather
   * than the module's runtime state to match what the user would see
   * if they reopened the page (NVS is the source of truth across
   * reboots). */
  Preferences prefs;
  bool prefs_ok = prefs.begin(SETTINGS_NS, /*readOnly=*/true);
  const int32_t cur_motion =
      prefs_ok ? prefs.getInt("cp.mt", 35) : 35;
  const int32_t cur_active =
      prefs_ok ? prefs.getInt("cp.at", 75) : 75;
  const int32_t cur_breath =
      prefs_ok ? prefs.getInt("cp.bt", 30) : 30;
  if (prefs_ok) prefs.end();

  char buf[320];
  switch (g_calibration.state) {
    case CALIB_IDLE:
      snprintf(buf, sizeof(buf), "{\"state\":\"idle\"}");
      break;
    case CALIB_TIMED_OUT:
      snprintf(buf, sizeof(buf),
        "{\"state\":\"timed_out\","
         "\"hint\":\"sensing not running; check /api/status csi.running\"}");
      break;
    case CALIB_RUNNING:
      snprintf(buf, sizeof(buf),
        "{\"state\":\"running\",\"samples\":%lu,\"target\":%lu}",
        (unsigned long)g_calibration.samples,
        (unsigned long)CALIB_WINDOWS);
      break;
    case CALIB_READY:
    default:
      snprintf(buf, sizeof(buf),
        "{\"state\":\"ready\",\"samples\":%lu,"
         "\"max_motion\":%u,\"max_breathing\":%u,"
         "\"proposed\":{\"motion\":%u,\"active\":%u,\"breathing\":%u},"
         "\"current\":{\"motion\":%ld,\"active\":%ld,\"breathing\":%ld}}",
        (unsigned long)g_calibration.samples,
        (unsigned)g_calibration.max_motion,
        (unsigned)g_calibration.max_breathing,
        (unsigned)g_calibration.proposed_motion,
        (unsigned)g_calibration.proposed_active,
        (unsigned)g_calibration.proposed_breathing,
        (long)cur_motion, (long)cur_active, (long)cur_breath);
      break;
  }
  httpd_resp_send(req, buf, -1);
  return ESP_OK;
}

esp_err_t handle_calibrate_apply(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  httpd_resp_set_type(req, "application/json");

  /* Refuse if the most recent calibration didn't actually finish — we
   * don't want to silently apply stale or nonsense values. */
  if (g_calibration.state != CALIB_READY) {
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_send(req,
      "{\"ok\":false,\"reason\":\"no calibration ready; call /start first\"}",
      -1);
    return ESP_OK;
  }

  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/false)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"nvs unavailable\"}", -1);
    return ESP_OK;
  }
  prefs.putInt("cp.mt", (int32_t)g_calibration.proposed_motion);
  prefs.putInt("cp.at", (int32_t)g_calibration.proposed_active);
  prefs.putInt("cp.bt", (int32_t)g_calibration.proposed_breathing);
  prefs.end();

  /* Reinit core.presence so the new thresholds take effect on the next
   * tick — no reboot needed. Mirrors the reinit path in
   * handle_settings_post. */
  reinit_module("core.presence");

  /* Mark the calibration consumed so a subsequent /status returns idle
   * (avoids the dashboard showing the same proposal again). */
  g_calibration.state = CALIB_IDLE;

  httpd_resp_send(req, "{\"ok\":true}", -1);
  return ESP_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * /api/settings — GET reads NVS, POST writes NVS + reinits affected module
 *
 * Wire format (intentionally tiny, dashboard-friendly):
 *   GET  → {"pet_mode":true|false}
 *   POST {"pet_mode":true|false}  → 200 {"ok":true} after persisting
 *
 * Pet Mode is the only key on the wire today; preset / sensitivity-slider
 * round-trips will land in a follow-up that maps preset → motion/active/
 * breathing thresholds. The NVS schema (cp.pet_mode et al.) is already
 * defined in SETTING_KEYS, so future endpoint expansion is purely
 * additive.
 * ────────────────────────────────────────────────────────────────────────── */

esp_err_t handle_settings_get(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/true)) {
    /* Don't silently report defaults — the dashboard would reconcile
     * localStorage to those values and quietly clobber any choice the
     * user had previously made. Surface the unavailability so the
     * client skips reconciliation and keeps its current localStorage
     * source of truth. */
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"settings store unavailable\"}", -1);
    return ESP_OK;
  }
  const bool    pet_mode    = prefs.getBool("cp.pet_mode", false);
  const int32_t preset_idx  = prefs.getInt ("cp.preset",   1);   // default balanced
  const int32_t sensitivity = prefs.getInt ("cp.sens",     50);  // default neutral
  const bool    qh_enabled  = prefs.getBool("qh.en",       false);
  const int32_t qh_start    = prefs.getInt ("qh.start",    23 * 60);  // 11 PM default
  const int32_t qh_end      = prefs.getInt ("qh.end",       7 * 60);  //  7 AM default
  /* Privacy ceiling: persisted P0/P1/P2 choice (default P0 = anti-snitch).
   * Read separately from the in-memory chokepoint state (which apply_*
   * keeps in sync) so we always echo what's on disk, not what the
   * chokepoint thinks. */
  const int32_t privacy_raw = prefs.getInt("cp.pc", (int32_t)CSI_PRIVACY_P0);
  prefs.end();

  /* Map preset index back to a stable string for the dashboard. The
   * mapping is the only place this conversion lives — keep it in sync
   * with the parser in handle_settings_post and the switch in
   * core_presence.cpp's on_init. */
  const char* preset_str = (preset_idx == 0) ? "sensitive"
                         : (preset_idx == 2) ? "quiet" : "balanced";

  const char* privacy_str = (privacy_raw == (int32_t)CSI_PRIVACY_P2) ? "p2"
                          : (privacy_raw == (int32_t)CSI_PRIVACY_P1) ? "p1"
                          : "p0";

  char buf[320];
  snprintf(buf, sizeof(buf),
    "{\"pet_mode\":%s,\"preset\":\"%s\",\"sensitivity\":%ld,"
     "\"quiet_hours\":{\"enabled\":%s,\"start_min\":%ld,\"end_min\":%ld},"
     "\"privacy_ceiling\":\"%s\"}",
    pet_mode ? "true" : "false", preset_str, (long)sensitivity,
    qh_enabled ? "true" : "false", (long)qh_start, (long)qh_end,
    privacy_str);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, -1);
  return ESP_OK;
}

esp_err_t handle_settings_post(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  /* Body is small JSON. Recognized keys:
   *   "pet_mode":    true|false  → cp.pet_mode (bool)
   *   "preset":      "sensitive"|"balanced"|"quiet" → cp.preset (int 0..2)
   *   "sensitivity": 0..100      → cp.sens (int)
   * Hand-parse to keep ArduinoJson out of this TU. We search for the
   * QUOTED key in every case so a body like {"not_pet_mode": true}
   * doesn't accidentally match. Buffer sized for the full payload:
   *   pet_mode + preset + sensitivity + quiet_hours{enabled, start, end}
   * is ~130 chars; 256 leaves comfortable headroom for future keys. */
  char body[256];
  const int got = httpd_req_recv(req, body, sizeof(body) - 1);
  if (got <= 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"empty body\"}", -1);
    return ESP_OK;
  }
  body[got] = '\0';

  bool wrote_anything = false;
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/false)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"nvs unavailable\"}", -1);
    return ESP_OK;
  }

  /* "pet_mode": true|false */
  if (const char* k = strstr(body, "\"pet_mode\"")) {
    if (const char* v = strchr(k, ':')) {
      v++;
      while (*v == ' ' || *v == '\t' || *v == '"') v++;
      if (strncmp(v, "true", 4) == 0) {
        prefs.putBool("cp.pet_mode", true);  wrote_anything = true;
      } else if (strncmp(v, "false", 5) == 0) {
        prefs.putBool("cp.pet_mode", false); wrote_anything = true;
      }
    }
  }

  /* "preset": "sensitive" | "balanced" | "quiet". Stored as int 0/1/2
   * so core_presence.cpp's switch is fast and the NVS row is small. */
  if (const char* k = strstr(body, "\"preset\"")) {
    if (const char* v = strchr(k, ':')) {
      v++;
      while (*v == ' ' || *v == '\t' || *v == '"') v++;
      int32_t idx = -1;
      if      (strncmp(v, "sensitive", 9) == 0) idx = 0;
      else if (strncmp(v, "balanced",  8) == 0) idx = 1;
      else if (strncmp(v, "quiet",     5) == 0) idx = 2;
      if (idx >= 0) {
        prefs.putInt("cp.preset", idx);
        wrote_anything = true;
      }
    }
  }

  /* "sensitivity": 0..100 (clamped). Skip `"` too so a value sent as
   * a string ({"sensitivity":"75"}) parses the same as a bare number,
   * matching the pet_mode and preset parsers above. */
  if (const char* k = strstr(body, "\"sensitivity\"")) {
    if (const char* v = strchr(k, ':')) {
      v++;
      while (*v == ' ' || *v == '\t' || *v == '"') v++;
      char* end = nullptr;
      long n = strtol(v, &end, 10);
      if (end != v) {
        if (n < 0)   n = 0;
        if (n > 100) n = 100;
        prefs.putInt("cp.sens", (int32_t)n);
        wrote_anything = true;
      }
    }
  }

  /* "quiet_hours": {"enabled": true|false, "start_min": M, "end_min": M}
   *
   * The original implementation gated on "\"quiet_hours\"" at the top
   * level but then searched for "\"enabled\"" / "\"start_min\"" /
   * "\"end_min\"" from the start of the body — meaning a future
   * top-level `enabled` field (or any other object that happens to
   * contain `enabled`) could overwrite qh.en with the wrong value.
   *
   * Walk the brace pair of the quiet_hours object and search ONLY
   * within that span. We temporarily nul-terminate at the closing
   * brace so strstr can't see past it, then restore the byte. Body
   * is a local buffer; mutating it is fine. */
  bool qh_changed = false;
  if (char* qh_key = (char*)strstr(body, "\"quiet_hours\"")) {
    char* qh_open = strchr(qh_key, '{');
    if (qh_open) {
      int depth = 1;
      char* p = qh_open + 1;
      for (; *p; ++p) {
        if (*p == '{') depth++;
        else if (*p == '}') {
          if (--depth == 0) break;
        }
      }
      /* p now points at the matching close brace, or '\0' if malformed.
       * Either way, nul-terminate one past it so strstr sees only the
       * object's contents. Save the byte to restore after parsing. */
      char saved = *p;
      *p = '\0';

      if (const char* e = strstr(qh_open, "\"enabled\"")) {
        if (const char* v = strchr(e, ':')) {
          v++;
          while (*v == ' ' || *v == '\t' || *v == '"') v++;
          if (strncmp(v, "true", 4) == 0) {
            prefs.putBool("qh.en", true);  wrote_anything = true; qh_changed = true;
          } else if (strncmp(v, "false", 5) == 0) {
            prefs.putBool("qh.en", false); wrote_anything = true; qh_changed = true;
          }
        }
      }
      auto put_minute = [&](const char* tag, const char* nvs) {
        const char* k = strstr(qh_open, tag);
        if (!k) return;
        const char* v = strchr(k, ':');
        if (!v) return;
        v++;
        while (*v == ' ' || *v == '\t' || *v == '"') v++;
        char* vend = nullptr;
        long n = strtol(v, &vend, 10);
        if (vend == v) return;
        if (n < 0)    n = 0;
        if (n > 1439) n = 1439;
        prefs.putInt(nvs, (int32_t)n);
        wrote_anything = true;
        qh_changed = true;
      };
      put_minute("\"start_min\"", "qh.start");
      put_minute("\"end_min\"",   "qh.end");

      *p = saved;  /* restore for any later parsers and for cleanliness */
    }
  }

  /* "privacy_ceiling": "p0" | "p1" | "p2". Persisted as int 0/1/2 so
   * apply_privacy_ceiling_from_nvs() can compare against the
   * CSI_PRIVACY_* enum directly. Unrecognized values are ignored — the
   * existing persisted value (or P0 default) survives. */
  bool ceiling_changed = false;
  if (const char* k = strstr(body, "\"privacy_ceiling\"")) {
    if (const char* v = strchr(k, ':')) {
      v++;
      while (*v == ' ' || *v == '\t' || *v == '"') v++;
      int32_t val = -1;
      if      (strncmp(v, "p0", 2) == 0) val = (int32_t)CSI_PRIVACY_P0;
      else if (strncmp(v, "p1", 2) == 0) val = (int32_t)CSI_PRIVACY_P1;
      else if (strncmp(v, "p2", 2) == 0) val = (int32_t)CSI_PRIVACY_P2;
      if (val >= 0) {
        prefs.putInt("cp.pc", val);
        wrote_anything = true;
        ceiling_changed = true;
      }
    }
  }

  prefs.end();

  if (!wrote_anything) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"no recognized keys\"}", -1);
    return ESP_OK;
  }

  /* Re-init affected module so it picks up the new values on the next tick. */
  reinit_module("core.presence");

  /* Re-apply Quiet Hours to the chokepoint only when one of the three
   * qh.* keys actually changed. Skipping the call when nothing in that
   * subtree moved avoids a needless NVS read + chokepoint mutation
   * (which also triggers a held_summary flush on transition) on every
   * unrelated POST (pet_mode, sensitivity, privacy_ceiling, etc.). The
   * gate matches the same pattern used for the privacy ceiling below. */
  if (qh_changed) apply_quiet_hours_from_nvs();

  /* Re-apply privacy ceiling to the chokepoint so the next request to
   * /api/csi/window or /api/tune/* reflects the new ceiling without a
   * reboot. Cheap (single int compare + atomic store). */
  if (ceiling_changed) apply_privacy_ceiling_from_nvs();

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"ok\":true}", -1);
  return ESP_OK;
}

esp_err_t handle_privacy_budget(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  /* Returns the literal outbound-byte count plus the current privacy
   * ceiling so the dashboard can warm-tint the pill when the user has
   * raised the ceiling above P0. ceiling=p0 + bytes=0 → cool pill;
   * any change → warm pill. Cheap: a single 32-bit read and a
   * three-letter switch.
   *
   * `wired` reflects whether any off-device export path actually feeds
   * the byte counter. The MQTT bridge in csi_mqtt.cpp now calls
   * add_outbound_bytes() on every successful publish, so the counter
   * is structurally honest as soon as the broker is configured. SD /
   * BLE export retrofits are a future commit. The dashboard reads
   * `wired` and either hides the pill or shows "not yet measured" —
   * we're now in the "yes, measured" branch.
   *
   * Cache-Control: no-store. The whole point of the pill is "what is
   * the device sending right now" — a cached zero would lie. */
  const csi_privacy_class_t ceiling = csi_event_get_privacy_ceiling();
  const char* ceiling_str = (ceiling == CSI_PRIVACY_P0) ? "p0"
                          : (ceiling == CSI_PRIVACY_P1) ? "p1" : "p2";

  /* Atomic load — the counter is updated lock-free from any export
   * path; see add_outbound_bytes() above for the threading rationale. */
  const uint32_t bytes = __atomic_load_n(&g_outbound_bytes, __ATOMIC_RELAXED);
  char buf[128];
  snprintf(buf, sizeof(buf),
    "{\"bytes_today\":%lu,\"ceiling\":\"%s\","
     "\"since_ms\":%lu,\"wired\":true}",
    (unsigned long)bytes,
    ceiling_str,
    (unsigned long)(millis() - g_stream_started_ms));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, buf, -1);
  return ESP_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * PWA assets — /manifest.webmanifest + /sw.js
 *
 * The companion PWA at /companion ships its own SW scoped to /companion.
 * The headline Sensing dashboard at / didn't have a PWA layer, so
 * "Add to Home Screen" landed on a generic browser bookmark with no
 * offline shell. This pair gives the dashboard a proper PWA identity:
 * an install promptable manifest and a tiny SW that caches the shell.
 *
 * The SW uses a network-first strategy for the cached URLs so live
 * dashboard updates land whenever WiFi is reachable; cache fallback
 * only when offline. Live API routes (/api/csi/stream, etc.) are
 * deliberately NOT in the precache list — they always hit the device.
 *
 * The icon is rendered inline as an SVG data URI so we don't need a
 * separate /icon.png route. Apple/Android home-screen icons accept
 * SVG; the orb-style gradient circle matches the dashboard's hero
 * widget.
 * ────────────────────────────────────────────────────────────────────────── */

const char SENSE_MANIFEST_JSON[] PROGMEM =
  "{"
    "\"name\":\"SecuraCV Canary\","
    "\"short_name\":\"Canary\","
    "\"start_url\":\"/\","
    "\"scope\":\"/\","
    "\"display\":\"standalone\","
    "\"background_color\":\"#fffbec\","
    "\"theme_color\":\"#f0c319\","
    "\"description\":\"Camera-free sensing dashboard.\","
    "\"icons\":["
      "{"
        "\"src\":\"data:image/svg+xml;utf8,"
          "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 256 256'>"
          "<defs><radialGradient id='g' cx='40%25' cy='35%25' r='65%25'>"
          "<stop offset='0%25' stop-color='%23fff3b0'/>"
          "<stop offset='55%25' stop-color='%23f0c319'/>"
          "<stop offset='100%25' stop-color='%23a07a08'/>"
          "</radialGradient></defs>"
          "<circle cx='128' cy='128' r='118' fill='url(%23g)'/>"
          "</svg>\","
        "\"sizes\":\"any\","
        "\"type\":\"image/svg+xml\","
        "\"purpose\":\"any maskable\""
      "}"
    "]"
  "}";

const char SENSE_SW_JS[] PROGMEM =
  "const CACHE='securacv-sense-v1';\n"
  "const URLS=['/','/manifest.webmanifest'];\n"
  "self.addEventListener('install',e=>{e.waitUntil(caches.open(CACHE).then(c=>c.addAll(URLS)));self.skipWaiting();});\n"
  "self.addEventListener('activate',e=>{e.waitUntil(caches.keys().then(keys=>Promise.all(keys.filter(k=>k!==CACHE).map(k=>caches.delete(k)))).then(()=>self.clients.claim()));});\n"
  "self.addEventListener('fetch',e=>{\n"
  "  if(e.request.method!=='GET')return;\n"
  "  const u=new URL(e.request.url);\n"
  "  /* Live data endpoints always hit the network — never cache. */\n"
  "  if(u.pathname.startsWith('/api/'))return;\n"
  "  const wantsCache=URLS.some(p=>u.pathname===p);\n"
  "  if(!wantsCache)return; /* pass-through for everything outside our shell */\n"
  "  e.respondWith(fetch(e.request).then(r=>{\n"
  "    if(r&&r.ok){const copy=r.clone();caches.open(CACHE).then(c=>c.put(e.request,copy));}\n"
  "    return r;\n"
  "  }).catch(()=>caches.match(e.request)));\n"
  "});\n";

esp_err_t handle_sense_manifest(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/manifest+json");
  /* The shell rarely changes within a session — let the browser cache
   * the manifest itself for an hour. The SW separately revalidates the
   * shell's HTML on each visit. */
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
  return httpd_resp_send(req, SENSE_MANIFEST_JSON, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handle_sense_sw(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/javascript");
  /* The Service-Worker-Allowed header lets the SW take a wider scope
   * than its own URL when the page registers it with `scope: '/'`.
   * The SW lives at the root so this is decorative for now, but
   * keeping it makes future relocations harmless. */
  httpd_resp_set_hdr(req, "Service-Worker-Allowed", "/");
  /* SW updates need to bypass HTTP cache so a new version of this
   * string activates on next install. The browser still caches the
   * SW for ~24h max regardless of headers; this is the lower bound. */
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, SENSE_SW_JS, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handle_sense_page(httpd_req_t* req) {
  /* /sense is the legacy alias from Phase-3 staging. The canonical
   * landing route is now / (handle_ui in canary_wap.ino), which injects
   * the Bearer token into the dashboard HTML at request time. Issuing
   * the same HTML here would skip that injection and the dashboard's
   * fetch() calls would all 401. A 301 to / keeps old links working
   * AND ensures the user lands on the token-bearing variant. */
  httpd_resp_set_status(req, "301 Moved Permanently");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_send(req, nullptr, 0);
  return ESP_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * TUNING LAB (Pillar D / Tier 4 #10)
 *
 * Hidden P2 surface at /tune. Lists every NVS-backed coefficient in
 * SETTING_KEYS as a labeled slider with min, max, default. Save/Load
 * preset writes/reads a local JSON bundle (no network egress) so a
 * tinkerer can ship a baseline between devices or back up before
 * experiments.
 *
 * Why a separate metadata table next to SETTING_KEYS?
 *   SETTING_KEYS only knows the (full_key, nvs_key) pair — it can't
 *   render a slider on its own. The metadata below adds the bits the
 *   UI needs (label, kind, range, default) and the bit the POST
 *   handler needs (which module to reinit). Co-locating these two
 *   tables keeps the abbreviation map small while still making "add
 *   a new coefficient" a one-place change.
 * ────────────────────────────────────────────────────────────────────────── */

enum TuneKind { TK_INT, TK_BOOL, TK_MINUTES };

struct TuneCoeff {
  const char* full_key;       /* e.g. "core.presence.preset" */
  const char* group;          /* "core.presence" */
  const char* label;          /* short human label for the slider */
  TuneKind    kind;           /* INT | BOOL | MINUTES (HH:MM render) */
  int32_t     min_v;
  int32_t     max_v;
  int32_t     default_v;
  const char* reinit_module;  /* module id to reinit on change ("" = none) */
};

const TuneCoeff TUNE_COEFFS[] = {
  /* Presence — preset (0=sensitive,1=balanced,2=quiet) + sensitivity slider
   * map onto the three direct thresholds; exposing all five lets a
   * tuner pin individual values without the preset overriding them. */
  { "core.presence.preset",              "core.presence",  "Preset (0=sensitive 1=balanced 2=quiet)", TK_INT,     0,    2,    1,  "core.presence" },
  { "core.presence.sensitivity",         "core.presence",  "Sensitivity (0..100)",                     TK_INT,     0,    100,  50, "core.presence" },
  { "core.presence.motion_threshold",    "core.presence",  "Motion threshold",                          TK_INT,     5,    120,  35, "core.presence" },
  { "core.presence.active_threshold",    "core.presence",  "Active threshold",                          TK_INT,     5,    120,  75, "core.presence" },
  { "core.presence.breathing_threshold", "core.presence",  "Breathing threshold",                       TK_INT,     5,    120,  30, "core.presence" },
  { "core.presence.pet_mode",            "core.presence",  "Pet mode",                                  TK_BOOL,    0,    1,    0,  "core.presence" },
  { "core.presence.pet_mode_seconds",    "core.presence",  "Pet-mode confirm window (sec)",             TK_INT,     5,    120,  30, "core.presence" },
  /* Multipath shimmer rejection — large RSSI swing without Doppler is
   * reflection noise, not motion. Defaults mirror core_presence.cpp. */
  { "core.presence.shimmer_enabled",     "core.presence",  "Shimmer rejection enabled",                 TK_BOOL,    0,    1,    1,  "core.presence" },
  { "core.presence.shimmer_rssi_swing",  "core.presence",  "Shimmer RSSI swing threshold (dB)",         TK_INT,     1,    50,   8,  "core.presence" },
  { "core.presence.shimmer_doppler_floor","core.presence", "Shimmer Doppler floor",                     TK_INT,     1,    120,  30, "core.presence" },

  /* Breathing — Goertzel band lock parameters. */
  { "core.breathing.lock_threshold",     "core.breathing", "Lock threshold",                            TK_INT,     5,    120,  30, "core.breathing" },
  { "core.breathing.confirm_seconds",    "core.breathing", "Confirm window (sec)",                      TK_INT,     5,    60,   20, "core.breathing" },

  /* Quiet hours — minutes-of-day window the dashboard dims and future
   * notification paths can suppress against. */
  { "core.quiet_hours.enabled",          "core.quiet_hours","Enabled",                                  TK_BOOL,    0,    1,    0,  "" },
  { "core.quiet_hours.start_min",        "core.quiet_hours","Start",                                    TK_MINUTES, 0,    1439, 0,  "" },
  { "core.quiet_hours.end_min",          "core.quiet_hours","End",                                      TK_MINUTES, 0,    1439, 480,"" },

  /* Anomaly baseline — out-of-pattern detector envelope. The runtime
   * clamps these inside the module on read; the UI mirrors the same
   * envelope so a tuner can't accidentally pick a value the runtime
   * will silently round off. */
  { "anomaly.baseline.spike_ratio",      "anomaly.baseline","Spike ratio (× baseline, 100 = 1.0×)",    TK_INT,     110,  1000, 250,"anomaly.baseline" },
  { "anomaly.baseline.min_motion",       "anomaly.baseline","Motion floor",                            TK_INT,     1,    100,  60, "anomaly.baseline" },
  { "anomaly.baseline.min_breathing",    "anomaly.baseline","Breathing floor",                         TK_INT,     1,    100,  50, "anomaly.baseline" },
  { "anomaly.baseline.cooldown_sec",     "anomaly.baseline","Per-channel cooldown (sec)",              TK_INT,     30,   3600, 600,"anomaly.baseline" },
};

const TuneCoeff* tune_coeff_for(const char* full_key) {
  if (!full_key) return nullptr;
  for (const TuneCoeff& c : TUNE_COEFFS) {
    if (strcmp(c.full_key, full_key) == 0) return &c;
  }
  return nullptr;
}

int32_t tune_clamp(const TuneCoeff& c, int32_t v) {
  if (v < c.min_v) return c.min_v;
  if (v > c.max_v) return c.max_v;
  return v;
}

/* Read the persisted value for one coefficient, or fall back to its
 * declared default. The declared default mirrors what each module
 * passes as its `csi_module_settings_int default` argument; if a value
 * has never been written, GET should still return that exact default
 * so the slider position matches what the module would actually use.
 *
 * Defensive guard: if a TuneCoeff is ever added without a matching
 * SETTING_KEYS row, nvs_key_for() returns nullptr and we fall back to
 * the declared default rather than passing NULL into Preferences. */
int32_t tune_read_value(Preferences& prefs, const TuneCoeff& c) {
  const char* nvs = nvs_key_for(c.full_key);
  if (!nvs) return c.default_v;
  if (c.kind == TK_BOOL) {
    return prefs.getBool(nvs, c.default_v != 0) ? 1 : 0;
  }
  return prefs.getInt(nvs, c.default_v);
}

void tune_write_value(Preferences& prefs, const TuneCoeff& c, int32_t v) {
  const char* nvs = nvs_key_for(c.full_key);
  if (!nvs) return;
  v = tune_clamp(c, v);
  if (c.kind == TK_BOOL) {
    prefs.putBool(nvs, v != 0);
  } else {
    prefs.putInt(nvs, v);
  }
}

esp_err_t handle_tune_page(httpd_req_t* req) {
  /* P2 surface. The page is a top-level navigation so we can't return
   * a 401 — the browser would just show its default error page. Instead:
   * if the visitor has a valid cv_session cookie, serve the Tuning Lab
   * directly (its in-page fetches authenticate via the same cookie,
   * sent automatically by the browser). If not, redirect to / so the
   * pair landing kicks in; the user can long-press the device chip in
   * the dashboard topbar to come back here once paired. */
  if (!csi_integration::session_validate_cookie(req)) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
  }
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, TUNE_UI_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handle_tune_get_coefficients(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  Preferences prefs;
  bool prefs_ok = prefs.begin(SETTINGS_NS, /*readOnly=*/true);

  /* Stream out one big JSON object; chunked send keeps RAM bounded
   * even as the table grows past the 16-coefficient v1 set. */
  httpd_resp_send_chunk(req, "{\"coefficients\":[", -1);
  bool first = true;
  for (const TuneCoeff& c : TUNE_COEFFS) {
    int32_t v = prefs_ok ? tune_read_value(prefs, c) : c.default_v;
    char buf[320];
    const char* kind_str = (c.kind == TK_BOOL) ? "bool"
                         : (c.kind == TK_MINUTES) ? "minutes" : "int";
    int n = snprintf(buf, sizeof(buf),
      "%s{\"full_key\":\"%s\",\"group\":\"%s\",\"label\":\"%s\",\"kind\":\"%s\","
      "\"min\":%ld,\"max\":%ld,\"default\":%ld,\"value\":%ld,\"step\":1}",
      first ? "" : ",",
      c.full_key, c.group, c.label, kind_str,
      (long)c.min_v, (long)c.max_v, (long)c.default_v, (long)v);
    if (n > 0) httpd_resp_send_chunk(req, buf, n);
    first = false;
  }
  httpd_resp_send_chunk(req, "]}", -1);
  httpd_resp_send_chunk(req, nullptr, 0);  /* end-of-chunks */
  if (prefs_ok) prefs.end();
  return ESP_OK;
}

/* Find one or more "key":value pairs in the body and write each. The
 * parser is intentionally minimal — it walks the body looking for
 * keys we recognize from TUNE_COEFFS and a numeric or true/false RHS.
 * Any unrecognized key is silently ignored (P2; tinkerers are not
 * expected to need detailed feedback on typos). */
esp_err_t handle_tune_post_coefficients(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  httpd_resp_set_type(req, "application/json");

  size_t total = req->content_len;
  if (total == 0 || total > 4096) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "{\"ok\":false,\"reason\":\"empty or too large\"}", -1);
  }
  char* body = (char*)malloc(total + 1);
  if (!body) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "{\"ok\":false,\"reason\":\"oom\"}", -1);
  }
  size_t read = 0;
  while (read < total) {
    int n = httpd_req_recv(req, body + read, total - read);
    if (n <= 0) { free(body); return ESP_FAIL; }
    read += (size_t)n;
  }
  body[total] = '\0';

  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/false)) {
    free(body);
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "{\"ok\":false,\"reason\":\"nvs unavailable\"}", -1);
  }

  /* Track which modules we need to reinit. A small fixed set keeps
   * us from reinit-spamming when one POST changes several coefficients
   * that all live under the same module. */
  bool reinit_presence  = false;
  bool reinit_breathing = false;
  bool reinit_anomaly   = false;
  int  changed = 0;

  for (const TuneCoeff& c : TUNE_COEFFS) {
    /* Locate "<full_key>" in the body, then walk to the colon and
     * the value. We require the surrounding quotes so that
     * "core.presence.pet_mode" doesn't accidentally match
     * "not_pet_mode" or similar substrings. */
    char needle[80];
    int nl = snprintf(needle, sizeof(needle), "\"%s\"", c.full_key);
    if (nl <= 0 || nl >= (int)sizeof(needle)) continue;
    const char* p = strstr(body, needle);
    if (!p) continue;
    p += nl;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') continue;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    int32_t v;
    if (c.kind == TK_BOOL) {
      if      (strncmp(p, "true",  4) == 0) v = 1;
      else if (strncmp(p, "false", 5) == 0) v = 0;
      else if (strncmp(p, "1",     1) == 0) v = 1;
      else if (strncmp(p, "0",     1) == 0) v = 0;
      else continue;
    } else {
      char* end = nullptr;
      long n = strtol(p, &end, 10);
      if (end == p) continue;
      v = (int32_t)n;
    }

    tune_write_value(prefs, c, v);
    changed++;
    if      (strcmp(c.reinit_module, "core.presence")    == 0) reinit_presence  = true;
    else if (strcmp(c.reinit_module, "core.breathing")   == 0) reinit_breathing = true;
    else if (strcmp(c.reinit_module, "anomaly.baseline") == 0) reinit_anomaly   = true;
  }
  prefs.end();
  free(body);

  if (changed == 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "{\"ok\":false,\"reason\":\"no recognized keys\"}", -1);
  }

  if (reinit_presence)  reinit_module("core.presence");
  if (reinit_breathing) reinit_module("core.breathing");
  if (reinit_anomaly)   reinit_module("anomaly.baseline");

  char ok[48];
  snprintf(ok, sizeof(ok), "{\"ok\":true,\"changed\":%d}", changed);
  return httpd_resp_send(req, ok, -1);
}

esp_err_t handle_tune_get_preset(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  /* Preset bundle: a flat JSON object mapping each coefficient's full
   * key to its current value. Identical shape to what POST consumes,
   * so a Save→Load round-trip is the identity. The bundle is local to
   * the device's filesystem of the user's browser; no network egress.
   *
   * Privacy: nothing in here ties to identity, but it does reveal a
   * tuner's calibration. Treated as P2 (developer only) — the dashboard
   * gates this surface behind the long-press affordance. */
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"tuning-preset.json\"");

  Preferences prefs;
  bool prefs_ok = prefs.begin(SETTINGS_NS, /*readOnly=*/true);

  httpd_resp_send_chunk(req, "{", -1);
  bool first = true;
  for (const TuneCoeff& c : TUNE_COEFFS) {
    int32_t v = prefs_ok ? tune_read_value(prefs, c) : c.default_v;
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "%s\"%s\":%ld",
                     first ? "" : ",", c.full_key, (long)v);
    if (n > 0) httpd_resp_send_chunk(req, buf, n);
    first = false;
  }
  httpd_resp_send_chunk(req, "}", -1);
  httpd_resp_send_chunk(req, nullptr, 0);
  if (prefs_ok) prefs.end();
  return ESP_OK;
}

esp_err_t handle_tune_post_preset(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  /* The preset bundle uses the same key/value shape as
   * handle_tune_post_coefficients, so we can just re-use that
   * handler — it walks the body looking for known keys and writes
   * each. The only difference is a preset typically carries every
   * coefficient at once. */
  return handle_tune_post_coefficients(req);
}

/* ──────────────────────────────────────────────────────────────────────────
 * PAIRING TOKEN STORE — Tier 5 #11
 *
 * RAM-only ring of one-shot tokens for the captive-portal QR onboarding
 * flow. The captive-portal handler bakes the token into the QR; the
 * companion PWA validates / consumes it before showing the WiFi
 * credentials form.
 *
 * Slots: small fixed pool. Each slot tracks 32 random bytes, the
 * issuance time, and a "used" flag. Eviction prefers (a) used slots,
 * (b) expired slots, (c) the oldest unused slot. That last clause means
 * a determined attacker can churn out token issuances and force the
 * eviction of a token a real user is mid-onboarding with — but the
 * legitimate user is already on the device's AP at that point, and
 * the PWA simply re-fetches /api/pair/token if validation fails. No
 * security regression vs. the older 302-redirect design.
 * ────────────────────────────────────────────────────────────────────────── */

constexpr size_t   PAIR_SLOTS        = 4;
constexpr uint32_t PAIR_TTL_MS       = 10UL * 60UL * 1000UL;  /* 10 min */
constexpr size_t   PAIR_TOK_BYTES    = 32;                    /* 256-bit entropy */
constexpr size_t   PAIR_TOK_HEX_LEN  = PAIR_TOK_BYTES * 2;    /* 64 hex chars */

struct PairSlot {
  bool     used;
  bool     active;
  uint32_t issued_ms;
  uint8_t  token[PAIR_TOK_BYTES];
};
PairSlot g_pair_slots[PAIR_SLOTS] = {};

/* hex_encode lives in the public csi_integration namespace (see the
 * definition near the bottom of this file) so csi_mqtt and any future
 * export path share one canonical encoder. The internal callers below
 * reach it via unqualified lookup since they're already inside
 * namespace csi_integration. */

bool hex_decode_to(const char* hex, uint8_t* out, size_t out_len) {
  if (!hex || strlen(hex) != out_len * 2) return false;
  for (size_t i = 0; i < out_len; ++i) {
    char hi = hex[2*i], lo = hex[2*i+1];
    auto val = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return 10 + c - 'a';
      if (c >= 'A' && c <= 'F') return 10 + c - 'A';
      return -1;
    };
    int hi_v = val(hi), lo_v = val(lo);
    if (hi_v < 0 || lo_v < 0) return false;
    out[i] = (uint8_t)((hi_v << 4) | lo_v);
  }
  return true;
}

/* Constant-time compare so a timing oracle can't tell us what's wrong. */
bool ct_eq(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; ++i) d |= a[i] ^ b[i];
  return d == 0;
}

PairSlot* pick_slot_for_issuance() {
  const uint32_t now = millis();
  /* Pass 1: prefer a used or expired slot. */
  for (size_t i = 0; i < PAIR_SLOTS; ++i) {
    PairSlot& s = g_pair_slots[i];
    if (!s.active || s.used || (now - s.issued_ms) >= PAIR_TTL_MS) return &s;
  }
  /* Pass 2: evict the oldest active-and-unused slot. */
  size_t oldest = 0;
  for (size_t i = 1; i < PAIR_SLOTS; ++i) {
    if ((now - g_pair_slots[i].issued_ms) > (now - g_pair_slots[oldest].issued_ms)) {
      oldest = i;
    }
  }
  return &g_pair_slots[oldest];
}

PairSlot* find_slot(const uint8_t* token) {
  const uint32_t now = millis();
  for (size_t i = 0; i < PAIR_SLOTS; ++i) {
    PairSlot& s = g_pair_slots[i];
    if (!s.active || s.used) continue;
    if ((now - s.issued_ms) >= PAIR_TTL_MS) continue;
    if (ct_eq(s.token, token, PAIR_TOK_BYTES)) return &s;
  }
  return nullptr;
}

/* ──────────────────────────────────────────────────────────────────────────
 * SESSION COOKIE STORE
 *
 * Issued on successful one-shot pair-token consumption (handle_ui's
 * /?cv_pair=<hex> branch). Replaces the previous design where the device's
 * Bearer api_token was injected into dashboard HTML — that approach made
 * the token harvestable by anyone on the SoftAP who could `view-source`
 * on / (per pull-request review #392 r3213361582).
 *
 * Each cookie is HttpOnly + SameSite=Strict, so JS can't read it (no XSS
 * exfil) and cross-origin requests can't forge it (no CSRF). Cookie body
 * is 32 bytes hex-encoded — 256-bit entropy, indistinguishable from
 * random by anything an in-page script could observe.
 *
 * 8 slots is enough for a household worth of phones / laptops / tablets
 * pairing concurrently. 24 h TTL matches typical "remember me" UX and
 * caps the post-compromise window without forcing daily re-pairing.
 *
 * Threading: HTTP handlers serialize behind one ESP-IDF httpd worker, so
 * no portMUX needed; matches the pair-slot store above. ────────────── */

constexpr size_t   SESSION_SLOTS    = 8;
constexpr uint32_t SESSION_TTL_MS   = 24UL * 60UL * 60UL * 1000UL;  /* 24 h */
constexpr size_t   SESSION_TOK_BYTES   = 32;                     /* 256-bit entropy */
constexpr size_t   SESSION_TOK_HEX_LEN = SESSION_TOK_BYTES * 2;  /* 64 hex chars */

struct SessionSlot {
  bool     active;
  uint32_t issued_ms;
  uint8_t  token[SESSION_TOK_BYTES];
};
SessionSlot g_session_slots[SESSION_SLOTS] = {};

SessionSlot* pick_session_slot_for_issuance() {
  const uint32_t now = millis();
  /* Pass 1: prefer an empty or expired slot. */
  for (size_t i = 0; i < SESSION_SLOTS; ++i) {
    SessionSlot& s = g_session_slots[i];
    if (!s.active || (now - s.issued_ms) >= SESSION_TTL_MS) return &s;
  }
  /* Pass 2: evict the oldest active slot. Same trade-off as the pair
   * store — a determined attacker can churn issuances and bump a real
   * user's session, but the legitimate user is on the AP and can re-pair
   * by tapping the QR / "Open dashboard" link again. */
  size_t oldest = 0;
  for (size_t i = 1; i < SESSION_SLOTS; ++i) {
    if ((now - g_session_slots[i].issued_ms) >
        (now - g_session_slots[oldest].issued_ms)) {
      oldest = i;
    }
  }
  return &g_session_slots[oldest];
}

bool find_valid_session(const uint8_t* token) {
  const uint32_t now = millis();
  for (size_t i = 0; i < SESSION_SLOTS; ++i) {
    SessionSlot& s = g_session_slots[i];
    if (!s.active) continue;
    if ((now - s.issued_ms) >= SESSION_TTL_MS) continue;
    if (ct_eq(s.token, token, SESSION_TOK_BYTES)) return true;
  }
  return false;
}

/* Read the cv_session cookie out of the Cookie request header.
 * The Cookie header is a single string of "name=value; name=value; ..."
 * pairs. We scan for "cv_session=" and copy out exactly SESSION_TOK_HEX_LEN
 * bytes after it. Returns false on any parse failure (header missing,
 * cookie absent, hex too short / too long). Never sends a response. */
bool read_session_cookie_hex(httpd_req_t* req, char* hex_out) {
  if (!req || !hex_out) return false;
  const size_t hdr_len = httpd_req_get_hdr_value_len(req, "Cookie");
  if (hdr_len == 0 || hdr_len >= 512) return false;
  /* Stack-allocate; 512 cap is a comfortable ceiling for the small
   * cookie set this device uses. */
  char buf[512];
  if (httpd_req_get_hdr_value_str(req, "Cookie", buf, sizeof(buf)) != ESP_OK) {
    return false;
  }
  const char* k = strstr(buf, "cv_session=");
  if (!k) return false;
  k += 11;  /* len("cv_session=") */
  /* Ensure there are exactly SESSION_TOK_HEX_LEN hex chars and the
   * value is terminated by ';' or end-of-string. Anything else is
   * a malformed cookie and we reject it. */
  for (size_t i = 0; i < SESSION_TOK_HEX_LEN; ++i) {
    const char c = k[i];
    const bool is_hex = (c >= '0' && c <= '9') ||
                        (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!is_hex) return false;
    hex_out[i] = c;
  }
  hex_out[SESSION_TOK_HEX_LEN] = '\0';
  /* The next char must be the cookie-pair terminator. */
  const char tail = k[SESSION_TOK_HEX_LEN];
  return (tail == '\0' || tail == ';' || tail == ' ');
}

}  /* namespace (anonymous) */

namespace csi_integration {

bool session_validate_cookie(httpd_req_t* req) {
  char hex[SESSION_TOK_HEX_LEN + 1];
  if (!read_session_cookie_hex(req, hex)) return false;
  uint8_t raw[SESSION_TOK_BYTES];
  if (!hex_decode_to(hex, raw, SESSION_TOK_BYTES)) return false;
  return find_valid_session(raw);
}

}  /* namespace csi_integration */

/* File-scope trampoline forwarded to from the anonymous-namespace
 * cv_session_validate forward decl (used by CSI_AUTH_OR_RETURN). Letting
 * the macro call the public API directly would require putting the
 * forward decl inside namespace csi_integration { ... } at file scope —
 * harmless but noisier than this single-line bridge. */
bool cv_session_validate(httpd_req_t* req) {
  return csi_integration::session_validate_cookie(req);
}

namespace csi_integration {

bool session_issue(char* hex_out, size_t out_cap) {
  if (!hex_out || out_cap < SESSION_TOK_HEX_LEN + 1) return false;
  SessionSlot* s = pick_session_slot_for_issuance();
  if (!s) return false;
  esp_fill_random(s->token, SESSION_TOK_BYTES);
  s->issued_ms = millis();
  s->active    = true;
  hex_encode(s->token, SESSION_TOK_BYTES, hex_out);
  return true;
}

/* Static so the asset lives in flash (PROGMEM-style on ESP32) and isn't
 * counted toward heap. Two %s slots: pair-token hex × 2 (one for the
 * <a href> and one for the on-screen URL the user can hand-type or
 * scan from a printed QR if their captive portal is uncooperative).
 *
 * Microcopy doctrine matches the headline dashboard's COPY object:
 * plain words, no jargon ("pair", "Bearer", "session" do not appear),
 * grade ≤6th to clear the FKGL CI gate. */
static const char SENSE_PAIR_LANDING_TMPL[] PROGMEM =
  "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
  "<title>Canary &middot; Welcome</title>"
  "<style>"
    "body{font-family:system-ui,-apple-system,sans-serif;max-width:420px;"
      "margin:60px auto;padding:24px;text-align:center;"
      "background:#fffbec;color:#1a1605;line-height:1.5;}"
    "h1{font-weight:500;font-size:28px;margin-bottom:8px;}"
    "p{margin:14px 0;color:#3a311e;}"
    "a.enter{display:inline-block;margin:24px 0 8px;padding:14px 36px;"
      "background:#f0c319;color:#1a1605;border-radius:14px;"
      "text-decoration:none;font-weight:500;font-size:17px;}"
    "a.enter:active{transform:translateY(1px);}"
    ".note{color:#6b6049;font-size:13px;margin-top:24px;}"
    ".url{font-family:ui-monospace,Menlo,monospace;font-size:11px;"
      "word-break:break-all;color:#6b6049;background:#f3ecd0;"
      "padding:10px;border-radius:8px;margin-top:8px;}"
    "@media (prefers-color-scheme:dark){"
      "body{background:#1a1605;color:#fffbec;}"
      "p{color:#cfc6ad;}"
      ".note,.url{color:#a89e85;}"
      ".url{background:#2a2310;}"
    "}"
  "</style></head><body>"
  "<h1>Welcome to your Canary</h1>"
  "<p>Open the dashboard to start sensing.</p>"
  "<a class=\"enter\" href=\"/?cv_pair=%s\">Open dashboard</a>"
  "<p class=\"note\">If the button does not work, paste this on the same network:</p>"
  "<div class=\"url\">http://192.168.4.1/?cv_pair=%s</div>"
  "<p class=\"note\">This link is good for 10 minutes and works one time.</p>"
  "</body></html>";

bool send_pair_landing(httpd_req_t* req) {
  if (!req) return false;
  char pair_hex[PAIR_TOK_HEX_LEN + 1];
  if (!pair_token_issue(pair_hex, sizeof(pair_hex))) {
    /* All slots taken AND none expirable — should be vanishingly rare.
     * Surface a 503 with a friendly note rather than serving a dead
     * landing page. */
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
      "<!doctype html><meta charset=\"utf-8\"><title>Canary</title>"
      "<p style=\"font-family:system-ui;text-align:center;margin-top:80px\">"
      "Too many people pairing right now. Please try again in a minute.</p>");
    return true;
  }
  /* Two %s + the template glue. The template is ~1.3 KB, the two hex
   * tokens add 128 bytes; 2 KB is comfortable. */
  char body[2048];
  const int n = snprintf(body, sizeof(body),
                         SENSE_PAIR_LANDING_TMPL, pair_hex, pair_hex);
  if (n <= 0 || (size_t)n >= sizeof(body)) return false;
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body, n) == ESP_OK;
}

}  /* namespace csi_integration */

namespace {

#if FEATURE_BLE_SCAN && FEATURE_MESH_NETWORK
/* ──────────────────────────────────────────────────────────────────────────
 * BLE SCOUT ↔ MESH GLUE (canary-wap parity for PIO PR #476)
 *
 * Connects the three pieces shipped over the PR 5c series, ported to
 * canary-wap:
 *   1. ble_scout's emit_arrived/departed transition fires the broadcast
 *      hook installed below.
 *   2. The hook forwards to mesh_network::send_beacon_event, which builds
 *      and signs a BEACON_EVENT envelope carrying the wire format from
 *      mesh_beacon.
 *   3. On the receive side, mesh_network::handle_received_message routes
 *      verified MSG_BEACON_EVENT frames into the handler installed via
 *      set_beacon_event_handler — peer table lookup + signature verify
 *      + replay defense.
 *
 * The forwarder is intentionally minimal: mesh_network::send_beacon_event
 * already short-circuits if no opera_secret is loaded or no peers are
 * connected, so we don't duplicate the precondition checks here. The
 * receive handler likewise just logs — the deeper "what does the Hub
 * DO with a beacon_event" question is PR 4b's territory.
 *
 * Threading: ble_scout's broadcast callback runs from EITHER the main
 * loop (ble_scout_tick → emit_departed) OR the NimBLE host task
 * (ble_scout_on_advert → emit_arrived) — see ble_scout.h THREADING
 * contract. That is multi-producer-single-consumer (MPSC) by
 * construction, not SPSC. A FreeRTOS queue is MPSC-safe by design
 * (xQueueSend acquires the queue's internal lock), so use it as the
 * marshaling primitive instead of a hand-rolled atomic ring — the
 * latter would race on `head` between the two producer tasks. */
struct OutboundBeaconEvent {
  bool arrived;
  char label[mesh_beacon::MAX_LABEL_BYTES + 1];
};
constexpr size_t  OUTBOUND_QUEUE_CAP = 8;
QueueHandle_t     s_outbound_queue   = nullptr;

void on_scout_beacon_event_outbound(bool arrived, const char* label) {
  /* Producer — main loop OR NimBLE host task. xQueueSend is safe under
   * concurrent producers and from any task context (not from ISR; the
   * NimBLE host task is a task, not an ISR). Timeout 0 = non-blocking
   * drop when the queue is full — bounded loss (ble_scout's chokepoint
   * already caps emissions at 24/hr per beacon, so 8 in-flight is
   * generous). */
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

void drain_outbound_beacon_queue() {
  /* Consumer — main loop only. */
  QueueHandle_t q = __atomic_load_n(&s_outbound_queue, __ATOMIC_ACQUIRE);
  if (q == nullptr) return;
  OutboundBeaconEvent ev;
  while (xQueueReceive(q, &ev, 0) == pdTRUE) {
    mesh_network::send_beacon_event(
        ev.arrived ? mesh_beacon::BeaconState::ARRIVED
                   : mesh_beacon::BeaconState::DEPARTED,
        ev.label);
  }
}

void on_peer_beacon_event_inbound(
    const uint8_t            sender_fp[mesh_network::FINGERPRINT_SIZE],
    mesh_beacon::BeaconState state,
    const char*              label) {
  /* Inbound events are rate-limited at the wire (Scout's chokepoint
   * 24/hr ceiling) so no extra rate gate here. Logging-only for now
   * — Hub-side aggregation lands with PR 4b. Uses the canonical
   * csi_integration::hex_encode helper rather than spawning a local
   * duplicate (per the docstring in csi_integration.h:210). */
  char fp_hex[2 * mesh_network::FINGERPRINT_SIZE + 1];
  csi_integration::hex_encode(sender_fp, mesh_network::FINGERPRINT_SIZE, fp_hex);

  Serial.printf("[ble.scout.peer] fp=%s state=%s label=\"%s\"\n",
                fp_hex,
                state == mesh_beacon::BeaconState::ARRIVED ? "arrived"
                : state == mesh_beacon::BeaconState::DEPARTED ? "departed"
                : "?",
                label ? label : "");
}
#endif  /* FEATURE_BLE_SCAN && FEATURE_MESH_NETWORK */

/* ──────────────────────────────────────────────────────────────────────────
 * CSI WATCHDOG CALLBACK
 *
 * csi_hal's watchdog detects 0 frames for 5 s and toggles the CSI rx
 * callback as a gentle recovery. This callback logs the event and
 * escalates to a full WiFi restart after 3 consecutive failures.
 * ────────────────────────────────────────────────────────────────────────── */

constexpr uint32_t WATCHDOG_ESCALATE_AFTER = 3;

/* ──────────────────────────────────────────────────────────────────────────
 * ACTIVE PROBE PUMP
 *
 * The CSI receiver only sees frames somebody transmits. On home WiFi the
 * AP's beacons supply ~10 Hz; on a Canary-only install (AP mode, no home
 * network) the air can be nearly silent and sensing starves. The probe
 * broadcasts a tiny ESP-NOW frame at CSI_PROBE_BROADCAST_HZ so every
 * OTHER Canary in range gets a deterministic frame supply — two devices
 * genuinely sense better together, each lighting up the other. (A device
 * cannot receive its own transmissions; a solo Canary still needs the
 * home AP's beacons — the dashboard's signal-supply chip says so.)
 *
 * Airtime: 10 Hz × ~40 B ESP-NOW broadcast ≈ 0.03 % of the channel —
 * far below the airtime governor's mesh thresholds.
 *
 * The probe shares ESP-NOW with the mesh when FEATURE_MESH_NETWORK is on
 * (csi_probe::init is idempotent against a prior esp_now_init) and brings
 * ESP-NOW up itself when the mesh is compiled out. Channel-hop desync
 * handling already pauses/resumes it (channel_recovery_tick). */
constexpr uint16_t CSI_PROBE_BROADCAST_HZ = 10;

void probe_pump() {
  static bool     s_probe_up = false;
  static uint32_t s_last_try_ms = 0;
  if (!g_hal_ready) return;
  if (!s_probe_up) {
    /* csi_hal running implies the WiFi driver is started, which is what
     * esp_now_init needs. Retry at 5 s until it sticks. */
    if (!csi_hal::is_running()) return;
    const uint32_t now = millis();
    if ((now - s_last_try_ms) < 5000u) return;
    s_last_try_ms = now;
    csi_probe::Config pc = csi_probe::Config::defaults();
    pc.broadcast_when_no_peers = true;
    pc.idle_rate_hz            = CSI_PROBE_BROADCAST_HZ;
    if (!csi_probe::init(pc)) return;   /* ESP-NOW not ready — retry */
    csi_probe::start();
    s_probe_up = true;
    Serial.printf("[CSI] active probe up — %u Hz ESP-NOW broadcast "
                  "(peer Canaries sense off these frames)\n",
                  (unsigned)CSI_PROBE_BROADCAST_HZ);
  } else if (csi_hal::is_running()) {
    /* Only spend TX airtime while local sensing runs — if csi_hal is
     * stopped (power policy, watchdog restart window) the radio budget
     * shouldn't go to lighting up the neighbors. */
    csi_probe::process();
  }
}

bool probe_running() { return csi_probe::is_running() && !csi_probe::is_paused(); }

void on_csi_watchdog(uint32_t silent_ms, uint32_t attempt) {
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

#if FEATURE_MESH_NETWORK
/* ──────────────────────────────────────────────────────────────────────────
 * CHANNEL-HOP COORDINATOR (PR 4b integration)
 *
 * Hub side: tick the HopTracker every main-loop pass with the current
 * airtime utilization from airtime_governor. When utilization exceeds
 * 50% for 60 s continuously, select the next non-overlapping channel
 * and broadcast CHANNEL_LOCK to all peers. Apply the channel lock
 * locally too (csi_hal::set_channel_lock) so Hub and peers converge.
 *
 * Peer side: on receiving CHANNEL_LOCK, apply the proposed channel
 * via csi_hal::set_channel_lock. Log the event.
 *
 * Both sides: the channel lock is advisory — if WiFi STA is associated
 * to an AP on a different channel, the AP wins. is_channel_in_sync()
 * reports the truth.
 * ────────────────────────────────────────────────────────────────────────── */

mesh_channel_hop::HopTracker s_hop_tracker =
    mesh_channel_hop::make_tracker(5000, 60000, 120000);

void channel_hop_tick(uint32_t now_ms) {
  uint16_t util = airtime_governor::airtime_pct_x100(now_ms);
  if (!mesh_channel_hop::tick(s_hop_tracker, now_ms, util)) return;

  uint8_t current = csi_hal::get_channel_lock();
  if (current == 0) current = csi_hal::get_observed_channel();
  uint8_t next = mesh_channel_hop::next_channel(current);
  if (next == 0) next = 6;

  size_t n = mesh_network::send_channel_lock(
      next, mesh_channel_hop::Reason::UTILIZATION);
  if (n == 0) return;

  csi_hal::set_channel_lock(next);
  mesh_channel_hop::reset(s_hop_tracker, now_ms);

  Serial.printf("[mesh.channel] hop %u→%u (util=%u.%02u%%, peers=%u)\n",
                current, next,
                (unsigned)(util / 100), (unsigned)(util % 100),
                (unsigned)n);
}

void on_peer_channel_lock(
    const uint8_t              sender_fp[mesh_network::FINGERPRINT_SIZE],
    uint8_t                    channel,
    mesh_channel_hop::Reason   reason) {
  csi_hal::set_channel_lock(channel);

  char fp_hex[2 * mesh_network::FINGERPRINT_SIZE + 1];
  csi_integration::hex_encode(sender_fp, mesh_network::FINGERPRINT_SIZE, fp_hex);

  Serial.printf("[mesh.channel] lock ch=%u reason=%u from fp=%s\n",
                channel, (unsigned)reason, fp_hex);
}

/* ──────────────────────────────────────────────────────────────────────────
 * AP ROAM / CHANNEL RECOVERY
 *
 * Detects when the observed CSI channel diverges from the pinned
 * channel lock (AP roam, DFS event, or neighbor interference).
 * Pauses probes, re-applies the channel lock to the new observed
 * channel, resumes probes, and logs. Checked every 5s from loop().
 * ────────────────────────────────────────────────────────────────────────── */

bool s_channel_desync_detected = false;

void channel_recovery_tick() {
  if (csi_hal::get_channel_lock() == 0) return;

  if (!csi_hal::is_channel_in_sync()) {
    if (!s_channel_desync_detected) {
      s_channel_desync_detected = true;
      csi_probe::set_paused(true);
      Serial.printf("[mesh.channel] desync: lock=%u observed=%u — probes paused\n",
                    csi_hal::get_channel_lock(),
                    csi_hal::get_observed_channel());
    }
    uint8_t observed = csi_hal::get_observed_channel();
    if (observed != 0 && observed != csi_hal::get_channel_lock()) {
      csi_hal::set_channel_lock(observed);
    }
  } else if (s_channel_desync_detected) {
    s_channel_desync_detected = false;
    csi_probe::set_paused(false);
    Serial.printf("[mesh.channel] resync: ch=%u — probes resumed\n",
                  csi_hal::get_observed_channel());
  }
}

/* ──────────────────────────────────────────────────────────────────────────
 * HUB FAILOVER ELECTION (PR 4c integration)
 *
 * Coordinator role is held by the live node with the lowest fingerprint.
 * On peer state changes (CONNECTED → OFFLINE), we re-evaluate who the
 * coordinator is. If we are the new coordinator (our fingerprint is
 * lowest among all CONNECTED peers + self), broadcast HUB_ELECTED.
 *
 * The coordinator runs channel_hop_tick (already wired above). Non-
 * coordinator nodes skip the Hub-side tick (they still respond to
 * CHANNEL_LOCK frames as peers).
 *
 * "Self fingerprint" is the first 8 bytes of the SHA-256 of our
 * Ed25519 pubkey — same derivation mesh_crypto uses. We compute it
 * once at init and cache it.
 * ────────────────────────────────────────────────────────────────────────── */

uint8_t  s_self_fp[mesh_hub_election::FINGERPRINT_LEN] = {0};
bool     s_self_fp_valid = false;
bool     s_is_coordinator = false;

void expire_offline_fusion_links() {
  for (uint8_t i = 0; i < mesh_network::get_peer_count(); ++i) {
    const mesh_network::OperaPeer* peer = mesh_network::get_peer(i);
    if (peer == nullptr) continue;
    if (peer->state == mesh_network::PEER_OFFLINE ||
        peer->state == mesh_network::PEER_REMOVED) {
      core_multilink_fusion_expire_link(peer->fingerprint);
    }
  }
}

void evaluate_coordinator() {
  if (!s_self_fp_valid) {
    s_self_fp_valid = mesh_network::get_self_fingerprint(s_self_fp);
    if (!s_self_fp_valid) return;
  }

  const uint8_t* lowest = s_self_fp;

  for (uint8_t i = 0; i < mesh_network::get_peer_count(); ++i) {
    const mesh_network::OperaPeer* peer = mesh_network::get_peer(i);
    if (peer == nullptr) continue;
    if (peer->state != mesh_network::PEER_CONNECTED &&
        peer->state != mesh_network::PEER_STALE &&
        peer->state != mesh_network::PEER_ALERT) continue;
    if (mesh_hub_election::compare_fingerprints(peer->fingerprint, lowest) < 0) {
      lowest = peer->fingerprint;
    }
  }

  bool was_coordinator = s_is_coordinator;
  s_is_coordinator = (mesh_hub_election::compare_fingerprints(lowest, s_self_fp) == 0);

  if (s_is_coordinator && !was_coordinator) {
    mesh_network::send_hub_election(
        mesh_hub_election::Event::HUB_ELECTED, s_self_fp);
    Serial.println("[mesh.election] promoted to coordinator (lowest fp)");
  } else if (!s_is_coordinator && was_coordinator) {
    Serial.println("[mesh.election] demoted from coordinator");
  }
}

void on_peer_hub_election(
    const uint8_t              sender_fp[mesh_network::FINGERPRINT_SIZE],
    mesh_hub_election::Event   event,
    const uint8_t              elected_fp[mesh_network::FINGERPRINT_SIZE]) {
  char sender_hex[2 * mesh_network::FINGERPRINT_SIZE + 1];
  char elected_hex[2 * mesh_network::FINGERPRINT_SIZE + 1];
  csi_integration::hex_encode(sender_fp, mesh_network::FINGERPRINT_SIZE, sender_hex);
  csi_integration::hex_encode(elected_fp, mesh_network::FINGERPRINT_SIZE, elected_hex);

  Serial.printf("[mesh.election] %s from fp=%s elected=%s\n",
                event == mesh_hub_election::Event::HUB_ELECTED ? "elected"
                : event == mesh_hub_election::Event::HUB_ABSENT ? "absent"
                : "?",
                sender_hex, elected_hex);

  evaluate_coordinator();
}
#endif  /* FEATURE_MESH_NETWORK */

esp_err_t handle_pair_token(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  /* Issues a fresh one-shot token. The captive-portal handler also calls
   * the C++ helper directly (it embeds the same token in the QR), but
   * having the route lets a manually-typed companion path or a future
   * mobile flow refresh on demand. */
  char hex[PAIR_TOK_HEX_LEN + 1];
  if (!csi_integration::pair_token_issue(hex, sizeof(hex))) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "{\"ok\":false}", -1);
  }
  /* 256 is comfortable for the current envelope (URL-encoded token is
   * 64 chars and the surrounding JSON is ~110 chars). We still pass
   * -1 (HTTPD_RESP_USE_STRLEN) so that future schema changes that
   * stretch this payload past the buffer get safely truncated by
   * snprintf and reflected by strlen — the alternative of using
   * snprintf's return value directly would trip a stack read overflow
   * on truncation. */
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"ok\":true,\"token\":\"%s\",\"expires_in_sec\":%lu,\"pair_url\":\"http://192.168.4.1/companion?token=%s\"}",
    hex, (unsigned long)(PAIR_TTL_MS / 1000UL), hex);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, buf, -1);
}

/* ──────────────────────────────────────────────────────────────────────────
 * MODULE REGISTRATION
 * ────────────────────────────────────────────────────────────────────────── */

void register_v1_modules() {
  /* Each module is wrapped in a CSI_DISABLE_MODULE_<id> guard so a
   * build with -DCSI_DISABLE_MODULE_<id>=1 still links cleanly. The
   * .github/workflows/csi_module_disable_matrix.yml job exercises each
   * single-disable variant on every PR — this guards the plan's
   * promise that "Disabling any single module via build flag still
   * produces a working firmware." */
#ifndef CSI_DISABLE_MODULE_CORE_PRESENCE
  csi_module_register(core_presence_module());
#endif
#ifndef CSI_DISABLE_MODULE_CORE_BREATHING
  csi_module_register(core_breathing_module());
#endif
#ifndef CSI_DISABLE_MODULE_CORE_ACTIVITY_RIBBON
  csi_module_register(core_activity_ribbon_module());
#endif
#ifndef CSI_DISABLE_MODULE_META_DAILY_SUMMARY
  csi_module_register(meta_daily_summary_module());
#endif
  /* meta.quiet_hours holds the manifest entry for the held_summary row
   * the chokepoint synthesises at the moment a configured Quiet Hours
   * window closes. Registering the module is what lets that synthetic
   * emit pass the chokepoint's allow-list check. Not part of the disable
   * matrix — gating Quiet Hours via build flag is a higher-layer feature
   * concern, and the chokepoint's setter is the runtime kill switch. */
  csi_module_register(meta_quiet_hours_module());
  /* Tier 3 #7: baseline-aware anomaly detector. P0, no identity, just
   * "this room rarely looks like that." Watches the same features
   * stream the four core modules see. */
#ifndef CSI_DISABLE_MODULE_ANOMALY_BASELINE
  csi_module_register(anomaly_baseline_module());
#endif

  /* wifi.channel_activity — ambient, unattributed "airwaves got busy" glow.
   * Identity-free CSI aggregates only; emits CSI_CATEGORY_AMBIENT (never
   * persisted, live UI only). See spec/canary_free_signals_v0.md Invariants
   * A/E/F. */
#ifndef CSI_DISABLE_MODULE_WIFI_CHANNEL_ACTIVITY
  csi_module_register(wifi_channel_activity_module());
#endif

  /* PR 3 — core.multilink_fusion: 2-link motion-confirmation gate.
   * Promotes single-link "observed" to multi-link "confirmed" when ≥2
   * links agree within a 3-second window. The .h/.cpp shipped in PR
   * #456 but the registration was missed in this build until now; the
   * matching gap in firmware/canary/src/csi_modules_integration.cpp
   * was closed in PR #458. */
#ifndef CSI_DISABLE_MODULE_CORE_MULTILINK_FUSION
  csi_module_register(core_multilink_fusion_module());
#endif

  /* PR 4a — meta.empty_room_baseline: scheduled 10-minute "empty
   * room" mean calibration, triggered at pairing-complete and during
   * quiet-hours. Same registration gap as core_multilink_fusion above. */
#ifndef CSI_DISABLE_MODULE_META_EMPTY_ROOM_BASELINE
  csi_module_register(meta_empty_room_baseline_module());
#endif

  /* spec/event_contract.md §10: BLE Discovery semantic events. The
   * module exists so any BLE → witness-chain emit MUST go through the
   * chokepoint, where the per-event allow-list strips fields that
   * carry MAC addresses, RSSI at tracking precision, or stable
   * hardware identifiers. Helpers in ble_events_module.h are the only
   * legitimate BLE→witness path going forward. Not part of the
   * disable matrix today — the BLE stack itself is a feature flag
   * (FEATURE_BLE_DISCOVERY) controlled at a higher layer. */
  csi_module_register(ble_events_module());

#if FEATURE_ACOUSTIC_EVENTS
  /* PDM-microphone acoustic detections (smoke/CO/knock/doorbell/glass
   * + mic mute toggles). Same chokepoint rationale as ble.events: the
   * per-event allow-list constrains every emit to a state tag, a
   * confidence word, and the time bucket — no audio content exists to
   * leak. Gated on the same flag that compiles the detector itself. */
  csi_module_register(acoustic_events_module());
#endif

#if FEATURE_VAULT_SNAPSHOT
  /* Sealed-snapshot vault lifecycle. The allow-list is the whole privacy
   * story: a frame_sealed event may carry the trigger tag (state_name),
   * the ciphertext SHA-256 prefix (note — integrity data only), and the
   * coarse time bucket. Image bytes structurally cannot cross the
   * chokepoint; the frame itself exists only as an encrypted .svlt on SD
   * that this device cannot decrypt (vault_snapshot.h). */
  csi_module_register(vault_events_module());
#endif

  /* The WAP's own integrity story (system.integrity). Unconditional, like
   * ble.events: every WAP has a reset reason and an SD state machine, and
   * the module's doctrine (tamper_events_module.h) already restricts it to
   * the kinds this hardware can truly detect. The allow-list constrains
   * every emit to a const.py vocabulary word (state_name) + time bucket. */
  csi_module_register(tamper_events_module());

#if FEATURE_BLE_SCAN
  /* BLE Scout — paired-beacon room-attribution (PR 5b ported to
   * canary-wap). Gated behind FEATURE_BLE_SCAN so the build cost
   * (NimBLE passive scan loop + per-device NVS key) is opt-in. The
   * module's csi_event_decl_t manifest constrains every emit to
   * state_name/note/time_bucket — no MAC or hashed_id ever lands in
   * an event payload. */
  csi_module_register(ble_scout::ble_scout_module());
  /* Load the per-device key + start the NimBLE passive scan loop.
   * Idempotent — safe even if the NimBLE stack isn't initialized yet
   * (the scan-loop TU is empty in builds without NimBLEDevice.h). */
  ble_scout::ble_scout_init();

#if FEATURE_MESH_NETWORK
  /* Wire the Scout broadcast hook into the mesh, and install a
   * receiver for inbound BEACON_EVENT frames. mesh_network's pairing
   * + opera-secret bootstrap is already brought up by canary_wap.ino
   * setup() — the moment a peer is paired, send_beacon_event lights
   * up end-to-end. Until then send_beacon_event returns 0 (no peers
   * or no opera_secret) and the handler is dormant.
   *
   * Create the FreeRTOS queue first so the broadcast callback has
   * somewhere to push to from the NimBLE host task. Idempotent across
   * re-init (the function is documented as safely re-entrant after a
   * /api/settings POST — keep the existing queue rather than orphaning
   * any in-flight events). */
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
  mesh_network::set_beacon_event_handler(&on_peer_beacon_event_inbound);
#endif
#endif

#if FEATURE_MESH_NETWORK
  /* PR 4b — install the channel-lock receiver. When a peer (Hub)
   * broadcasts CHANNEL_LOCK, on_peer_channel_lock applies the
   * proposed channel via csi_hal::set_channel_lock(). The Hub-side
   * tick (channel_hop_tick) runs from loop() below. */
  mesh_network::set_channel_lock_handler(&on_peer_channel_lock);
  mesh_network::set_hub_election_handler(&on_peer_hub_election);
#endif

  /* Wire the persisted Quiet Hours range into the chokepoint. The
   * dashboard's settings panel writes qh.en / qh.start / qh.end via
   * /api/settings POST; rebooting the device picks them back up. */
  apply_quiet_hours_from_nvs();

  csi_hal::set_watchdog(csi_hal::WATCHDOG_DEFAULT_TIMEOUT_MS,
                        &on_csi_watchdog);
}

}  /* namespace */

/* ──────────────────────────────────────────────────────────────────────────
 * STRONG OVERRIDE — csi_event_on_committed
 *
 * The library declares this hook __attribute__((weak)) so the standalone
 * build links cleanly. Here we provide the strong implementation that
 * records the snapshot for /api/csi/stream.
 * ────────────────────────────────────────────────────────────────────────── */

/* ──────────────────────────────────────────────────────────────────────────
 * STRONG OVERRIDES — csi_module_settings_*
 *
 * The library's weak defaults return whatever default the caller passes;
 * here we look up the canonical full key, map to the short NVS key, and
 * read the persisted value. Falls back to the caller's default when the
 * key is absent or this is the first boot.
 *
 * Read-only Preferences handle is opened per call. Settings reads are
 * infrequent (boot + post-POST reinit), so the small open/close cost
 * is fine and avoids holding an NVS handle across the firmware lifetime.
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
 * STRONG OVERRIDE — csi_event_commit_witness
 *
 * The library declares this hook with a weak no-op default in
 * firmware/common/csi/src/csi_event.cpp so the standalone build links
 * cleanly. Here in the canary-wap host we route every committed P0/P1
 * event into the existing witness chain via the public bridge defined in
 * canary_wap.ino's create_witness_record path. P2 never reaches us — the
 * chokepoint already gates that.
 *
 * The bridge function is defined in canary_wap.ino as extern "C"; we
 * forward-declare it here (the .ino doesn't ship a header). Ed25519
 * signing, hash-chaining, and SD persistence all happen inside the
 * existing create_witness_record + persist_chain_state pipeline; this
 * override is just the glue.
 *
 * Best-effort: a witness-chain failure (e.g. signing self-test broken,
 * SD full) doesn't bubble back to the caller — the in-memory ring + SSE
 * stream still update. This matches how create_witness_record's other
 * call sites in canary_wap.ino treat failures.
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" bool csi_witness_emit_event(const char* module_id,
                                       const char* type_name,
                                       uint8_t     category,
                                       const char* state_name,
                                       const char* confidence,
                                       uint8_t     motion_score,
                                       uint8_t     breathing_score,
                                       uint8_t     bpm,
                                       uint16_t    duration_sec,
                                       uint8_t     time_bucket);

extern "C" bool csi_event_commit_witness(uint32_t                  /*event_id*/,
                                         const char*               module_id,
                                         const char*               type_name,
                                         csi_event_category_t      category,
                                         const csi_event_values_t* values) {
  if (!values || !module_id || !type_name) return false;
  /* Only persist Event / Anomaly. Ambient never reaches us thanks to the
   * chokepoint, but defensive check keeps the contract local to this TU. */
  if (category == CSI_CATEGORY_AMBIENT) return false;

  return csi_witness_emit_event(
    module_id,
    type_name,
    (uint8_t)category,
    values->state_name,
    values->confidence,
    values->motion_score,
    values->breathing_score,
    values->breathing_rate_bpm,
    values->duration_sec,
    values->time_bucket);
}

extern "C" void csi_event_on_committed(uint32_t                  event_id,
                                       const char*               module_id,
                                       const char*               type_name,
                                       csi_event_category_t      category,
                                       csi_privacy_class_t       privacy,
                                       const csi_event_values_t* values) {
  if (!values) return;
  /* Don't expose P2 events on the public stream unless the user has
   * explicitly raised the privacy ceiling. The chokepoint already respects
   * this for emit; a defensive check here keeps the wire surface obvious. */
  if (privacy > csi_event_get_privacy_ceiling()) return;

  Snapshot* s = &g_snapshot;
  s->valid        = true;
  s->event_id     = event_id;
  s->committed_ms = millis();
  s->category     = category;
  s->privacy      = privacy;
  s->values       = *values;
  strncpy(s->module_id, module_id ? module_id : "?", CSI_EVENT_NAME_MAX - 1);
  strncpy(s->type_name, type_name ? type_name : "?", CSI_EVENT_NAME_MAX - 1);
  s->module_id[CSI_EVENT_NAME_MAX - 1] = '\0';
  s->type_name[CSI_EVENT_NAME_MAX - 1] = '\0';

  /* Forward to MQTT (no-op when the bridge is disabled or the broker
   * is unreachable). Same privacy ceiling already gated the snapshot
   * write above, so we forward whatever we accepted into g_snapshot
   * — no new chokepoint to keep in sync. event_id flows through so
   * csi_mqtt can track the high-water-mark for backfill on reconnect. */
  csi_mqtt::publish_event(event_id, s->module_id, s->type_name, category, privacy, values);

  /* Persist to SD so today's history survives a reboot AND so the
   * MQTT bridge can backfill HA after an outage (csi_event_log
   * iterate_since walks the same file). The full record (including
   * first_seen_ms / last_seen_ms / bundled_count, which the bundler
   * filled in inside the ring) lives in the in-memory ring; pull a
   * copy via csi_event_find so the on-disk row matches what
   * csi_event_recent would return. */
  csi_event_record_t persist_rec;
  if (csi_event_find(event_id, &persist_rec)) {
    csi_event_log::append(&persist_rec);
  }
}

/* ──────────────────────────────────────────────────────────────────────────
 * STRONG OVERRIDE — csi_event_on_id_advance
 *
 * Fires on every event-id allocation. We throttle-persist the next-id
 * to NVS every CSI_ID_PERSIST_STRIDE advances so a subsequent boot can
 * resume from "persisted + safety_margin" via apply_event_id_floor_from_nvs.
 * Without this, a reboot resets g_next_event_id to 1 and csi_mqtt's
 * reconnect-backfill watermark loses the ability to disambiguate
 * previous-boot vs current-boot events. ──────────────────────────── */

extern "C" void csi_event_on_id_advance(uint32_t new_id) {
  /* Cheap modulo gate so we don't hit NVS on every event. STRIDE=10
   * means worst-case loss is 10 ids on a hard reset; NVS writes stay
   * around ~14/day at the per-module hourly ceiling, well inside the
   * cell wear budget. */
  if (new_id < g_id_persisted_at + CSI_ID_PERSIST_STRIDE) return;
  persist_event_id_floor(new_id);
}

/* ──────────────────────────────────────────────────────────────────────────
 * PUBLIC API
 * ────────────────────────────────────────────────────────────────────────── */

namespace csi_integration {

void set_legacy_features_hook(legacy_features_hook_t hook) {
  g_legacy_hook = hook;
}

unsigned int sse_client_count() {
  /* Polling currently — no persistent clients. Reserved for SSE upgrade. */
  return 0;
}

void add_outbound_bytes(uint32_t bytes) {
  /* Lock-free CAS loop. The function is documented as callable from
   * any host export path, including paths that run on different
   * FreeRTOS tasks (a future MQTT publisher on the WiFi task, an SD
   * exporter on the storage task, the HTTP task reading the counter
   * for /api/privacy-budget). A naive read-modify-write would lose
   * concurrent increments — the BLE export task's bytes could be
   * stomped by the MQTT task and the user would see an under-count,
   * which silently undermines the privacy promise the pill makes.
   *
   * GCC built-in atomics are available on ESP32's xtensa toolchain
   * with no extra header. Saturating add at UINT32_MAX rather than
   * wrap, since silently rolling back to 0 would lie. */
  uint32_t expected = __atomic_load_n(&g_outbound_bytes, __ATOMIC_RELAXED);
  uint32_t desired;
  do {
    desired = expected + bytes;
    if (desired < expected) desired = UINT32_MAX;  // overflow → saturate
  } while (!__atomic_compare_exchange_n(
      &g_outbound_bytes, &expected, desired,
      /*weak=*/false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

uint32_t outbound_bytes_today() {
  return __atomic_load_n(&g_outbound_bytes, __ATOMIC_RELAXED);
}

/* ──────────────────────────────────────────────────────────────────────────
 * PUBLIC TOKEN API (Tier 5 #11)
 * ────────────────────────────────────────────────────────────────────────── */

bool pair_token_issue(char* hex_out, size_t out_cap) {
  if (!hex_out || out_cap < PAIR_TOK_HEX_LEN + 1) return false;
  PairSlot* s = pick_slot_for_issuance();
  if (!s) return false;
  esp_fill_random(s->token, PAIR_TOK_BYTES);
  s->issued_ms = millis();
  s->used      = false;
  s->active    = true;
  hex_encode(s->token, PAIR_TOK_BYTES, hex_out);
  return true;
}

bool pair_token_valid(const char* hex) {
  if (!hex) return false;
  uint8_t raw[PAIR_TOK_BYTES];
  if (!hex_decode_to(hex, raw, PAIR_TOK_BYTES)) return false;
  return find_slot(raw) != nullptr;
}

bool pair_token_consume(const char* hex) {
  if (!hex) return false;
  uint8_t raw[PAIR_TOK_BYTES];
  if (!hex_decode_to(hex, raw, PAIR_TOK_BYTES)) return false;
  PairSlot* s = find_slot(raw);
  if (!s) return false;
  s->used = true;
  return true;
}

bool init(httpd_handle_t server, const char* api_token) {
  if (g_initialized) return true;
  if (!server || !api_token || !*api_token) return false;

  /* Stash the Bearer token before any handler can fire. CSI_AUTH_OR_RETURN
   * reads g_api_token; if it's null every handler 401s, which is the
   * correct fail-closed behavior. */
  g_api_token = api_token;

  register_v1_modules();

  /* Restore persisted privacy ceiling (defaults to P0 — privacy-first).
   * Done before HAL start so the very first /api/csi/window request after
   * boot honors the user's prior choice rather than always 403'ing. */
  apply_privacy_ceiling_from_nvs();

  /* Restore the event-id floor from NVS so allocations stay globally
   * monotone across reboots. Done before any module ticks (which can
   * call csi_event_emit and trigger an allocation) so the very first
   * post-reboot id starts at the persisted floor instead of 1. With
   * this, csi_mqtt's reconnect-backfill watermark stays sound and
   * csi_event_log no longer needs to wipe the on-disk log on cold
   * boot to avoid id collisions. */
  apply_event_id_floor_from_nvs();

  /* Bring up the CSI HAL. start() defers until WiFi is up; the deferred
   * retry is silent and handled by csi_hal::process().
   *
   * If init fails (chip lacks CSI, ESP-IDF build disabled it, etc.) we
   * still register the HTTP routes — handle_stream sniffs g_hal_ready
   * and returns a "sensing_unavailable" payload so the dashboard renders
   * a clear error state instead of 404'ing. */
  csi_hal::Config cfg = csi_hal::Config::defaults();
  cfg.bandwidth_mhz     = 20;
  cfg.max_frame_rate_hz = 20;
  g_hal_ready = csi_hal::init(cfg);
  if (g_hal_ready) {
    csi_set_features_callback(on_csi_window, nullptr);
    csi_hal::start();   /* may defer; that's fine */
  } else {
    Serial.println("[CSI] csi_hal::init failed; routes still registered, "
                   "stream will return status:unavailable");
  }

  g_stream_started_ms = millis();

  /* Register HTTP routes. Four endpoints; the route reservation in
   * canary_wap.ino's start_http_server() needs to budget for them. */
  static httpd_uri_t r_stream = {
    .uri = "/api/csi/stream", .method = HTTP_GET, .handler = handle_stream
  };
  httpd_register_uri_handler(server, &r_stream);

  static httpd_uri_t r_window = {
    .uri = "/api/csi/window", .method = HTTP_GET, .handler = handle_window
  };
  httpd_register_uri_handler(server, &r_window);

  static httpd_uri_t r_today = {
    .uri = "/api/events/today", .method = HTTP_GET, .handler = handle_events_today
  };
  httpd_register_uri_handler(server, &r_today);

  static httpd_uri_t r_dismiss = {
    .uri = "/api/events/dismiss", .method = HTTP_POST, .handler = handle_events_dismiss
  };
  httpd_register_uri_handler(server, &r_dismiss);

  /* /api/csi/calibrate/{start,status,apply} — threshold auto-calibration.
   * The dashboard guides the user through ~10 s of "stand still" sampling,
   * computes proposed thresholds a margin above the observed ambient,
   * then applies on accept. canary_wap.ino's start_http_server route
   * budget reserves three slots for these. */
  static httpd_uri_t r_calib_start = {
    .uri = "/api/csi/calibrate/start", .method = HTTP_POST, .handler = handle_calibrate_start
  };
  httpd_register_uri_handler(server, &r_calib_start);
  static httpd_uri_t r_calib_status = {
    .uri = "/api/csi/calibrate/status", .method = HTTP_GET, .handler = handle_calibrate_status
  };
  httpd_register_uri_handler(server, &r_calib_status);
  static httpd_uri_t r_calib_apply = {
    .uri = "/api/csi/calibrate/apply", .method = HTTP_POST, .handler = handle_calibrate_apply
  };
  httpd_register_uri_handler(server, &r_calib_apply);

  /* /api/mqtt/{config,test} + /mqtt — optional Home Assistant bridge.
   * The token-provider lambda lets csi_mqtt's HTTP handlers
   * authenticate against the same api_token the rest of the CSI
   * surface uses without csi_mqtt needing to know about g_device. */
  csi_mqtt::set_api_token_provider([]() -> const char* { return g_api_token; });
  static httpd_uri_t r_mqtt_cfg_get = {
    .uri = "/api/mqtt/config", .method = HTTP_GET, .handler = csi_mqtt::handle_config_get
  };
  httpd_register_uri_handler(server, &r_mqtt_cfg_get);
  static httpd_uri_t r_mqtt_cfg_post = {
    .uri = "/api/mqtt/config", .method = HTTP_POST, .handler = csi_mqtt::handle_config_post
  };
  httpd_register_uri_handler(server, &r_mqtt_cfg_post);
  static httpd_uri_t r_mqtt_test = {
    .uri = "/api/mqtt/test", .method = HTTP_POST, .handler = csi_mqtt::handle_test
  };
  httpd_register_uri_handler(server, &r_mqtt_test);
  static httpd_uri_t r_mqtt_ui = {
    .uri = "/mqtt", .method = HTTP_GET, .handler = csi_mqtt::handle_ui
  };
  httpd_register_uri_handler(server, &r_mqtt_ui);

  /* /sense is kept as an alias for the headline dashboard for backward
   * compatibility — the canonical landing route is now "/" (handled by
   * canary_wap.ino's handle_ui), and the legacy tabbed dashboard moved
   * to /admin. Any links the companion PWA or third-party tools may
   * have made during the Phase-3 staging period keep working. */
  static httpd_uri_t r_sense = {
    .uri = "/sense", .method = HTTP_GET, .handler = handle_sense_page
  };
  httpd_register_uri_handler(server, &r_sense);

  /* /api/settings — GET returns persisted module settings, POST writes
   * them and triggers a module reinit so the device responds immediately
   * to dashboard changes. Pet Mode is the only key on the wire today;
   * the NVS schema is set up for preset / sensitivity follow-up. */
  static httpd_uri_t r_settings_get = {
    .uri = "/api/settings", .method = HTTP_GET, .handler = handle_settings_get
  };
  httpd_register_uri_handler(server, &r_settings_get);
  static httpd_uri_t r_settings_post = {
    .uri = "/api/settings", .method = HTTP_POST, .handler = handle_settings_post
  };
  httpd_register_uri_handler(server, &r_settings_post);

  /* /api/privacy-budget — literal byte counter for outbound traffic.
   * 0 by default (the device is local-first); other code calls
   * csi_integration::add_outbound_bytes() when it sends data to a
   * destination outside the user's immediate network. */
  static httpd_uri_t r_privacy_budget = {
    .uri = "/api/privacy-budget", .method = HTTP_GET, .handler = handle_privacy_budget
  };
  httpd_register_uri_handler(server, &r_privacy_budget);

  /* Dashboard PWA shell — manifest + service worker. The SW is
   * scope-/ so it can intercept dashboard fetches; live API routes
   * are explicitly passed through inside the SW. */
  static httpd_uri_t r_manifest = {
    .uri = "/manifest.webmanifest", .method = HTTP_GET, .handler = handle_sense_manifest
  };
  httpd_register_uri_handler(server, &r_manifest);
  static httpd_uri_t r_sw = {
    .uri = "/sw.js", .method = HTTP_GET, .handler = handle_sense_sw
  };
  httpd_register_uri_handler(server, &r_sw);

  /* Tier 4 #10 — Tuning Lab. P2 surface; the route reservation in
   * canary_wap.ino's start_http_server() needs five extra slots for
   * the page + the two coefficient endpoints + the two preset
   * endpoints. */
  static httpd_uri_t r_tune_page = {
    .uri = "/tune", .method = HTTP_GET, .handler = handle_tune_page
  };
  httpd_register_uri_handler(server, &r_tune_page);
  static httpd_uri_t r_tune_get = {
    .uri = "/api/tune/coefficients", .method = HTTP_GET, .handler = handle_tune_get_coefficients
  };
  httpd_register_uri_handler(server, &r_tune_get);
  static httpd_uri_t r_tune_post = {
    .uri = "/api/tune/coefficients", .method = HTTP_POST, .handler = handle_tune_post_coefficients
  };
  httpd_register_uri_handler(server, &r_tune_post);
  static httpd_uri_t r_tune_preset_get = {
    .uri = "/api/tune/preset", .method = HTTP_GET, .handler = handle_tune_get_preset
  };
  httpd_register_uri_handler(server, &r_tune_preset_get);
  static httpd_uri_t r_tune_preset_post = {
    .uri = "/api/tune/preset", .method = HTTP_POST, .handler = handle_tune_post_preset
  };
  httpd_register_uri_handler(server, &r_tune_preset_post);

  /* Tier 5 #11 — pairing token issuance. The captive portal handler in
   * canary_wap.ino calls pair_token_issue() directly to bake the token
   * into the QR; this route is for the companion PWA to refresh a
   * token if the captive-portal copy expired before the user finished
   * entering credentials. */
  static httpd_uri_t r_pair_token = {
    .uri = "/api/pair/token", .method = HTTP_GET, .handler = handle_pair_token
  };
  httpd_register_uri_handler(server, &r_pair_token);

  g_initialized = true;
  Serial.printf("[CSI] integration ready: %u modules, 16 routes registered\n",
                (unsigned)csi_module_count());
  return true;
}

/* ──────────────────────────────────────────────────────────────────────────
 * MAIN-LOOP PUMP + BOOT SELF-TEST
 *
 * The host sketch calls csi_integration::loop() once per main-loop
 * iteration. We forward to csi_hal::process(), which drains the WiFi-task
 * SPSC ring, finalizes 1-Hz feature windows, dispatches each window through
 * on_csi_window() into the v1 module pipeline, and silently retries the
 * deferred CSI-enable sequence if start() was queued before WiFi came up.
 *
 * The self-test fires once, ~3 seconds after init() returned, and surfaces
 * a single line on Serial so a cabled installer immediately knows whether
 * the radio is producing CSI frames. This is intentionally not gated on a
 * dashboard / API call: the dashboard's "Sensing…" copy is correct UX for
 * an honest-but-quiet room, so it can't double as a "did it boot" signal.
 * ────────────────────────────────────────────────────────────────────────── */

void loop(bool run_csi) {
  if (!g_initialized) return;

  /* Close any bundle past its window or quiet gap NOW. The bundler only
   * expires on the next admissible emit, so a room that goes quiet right
   * after a state-bearing event would otherwise hold its last bundle
   * "open" indefinitely — and /api/events/today would keep calling it
   * current (review on the open-bundle serializer). Unconditional on
   * purpose: bundles opened before a power-gate pause must still commit
   * on time. Cheap — an 8-slot scan, closes only when overdue. */
  csi_bundler_tick();

#if FEATURE_BLE_SCAN && FEATURE_MESH_NETWORK
  /* Drain the outbound beacon queue first so events the previous tick
   * enqueued (or that the NimBLE host task enqueued asynchronously)
   * get broadcast on the same main-loop pass. Drain runs in main task
   * context — satisfies mesh_network::send_beacon_event's contract. */
  drain_outbound_beacon_queue();
#endif

#if FEATURE_MESH_NETWORK
  {
    static uint32_t s_last_election_eval_ms = 0;
    uint32_t now = millis();
    if ((int32_t)(now - s_last_election_eval_ms) >= 5000) {
      s_last_election_eval_ms = now;
      evaluate_coordinator();
      expire_offline_fusion_links();
      channel_recovery_tick();
    }
    if (s_is_coordinator) {
      channel_hop_tick(now);
    }
  }
#endif

  /* Round-two power gate: everything ABOVE (outbound beacon drain +
   * mesh coordinator/channel maintenance) always runs — mesh carries
   * inter-canary security alerts and must not pause on battery. Only the
   * CSI-specific work below is skipped when the policy disables CSI:
   * csi_hal::process() (the drain), probe_pump() (peer probing that
   * exists solely to elicit CSI frames — no probes needed when CSI is
   * off, and it saves the probe TX too), and the boot self-test (which
   * would otherwise false-alarm "0 frames" while CSI is intentionally
   * disabled — deferring it means it runs once CSI is actually active). */
  if (!run_csi) return;

  csi_hal::process();
  probe_pump();

  /* One-shot boot self-test. The 3-second window is long enough for
   * csi_hal's deferred-start retry (1 Hz) to converge AND for at least
   * one or two windows to finalize on a healthy radio (windows are 1 s
   * each), but short enough that an installer watching serial output
   * doesn't lose patience. */
  static bool s_boot_check_done = false;
  if (s_boot_check_done) return;

  const uint32_t now = millis();
  if ((now - g_stream_started_ms) < 3000u) return;

  s_boot_check_done = true;
  csi_stats_t stats = {};
  csi_hal::get_stats(&stats);
  if (!csi_hal::is_running()) {
    Serial.println("[CSI] STALLED: HAL not running after 3s — "
                   "WiFi never came up or chip lacks CSI");
  } else if (stats.frames_received == 0) {
    Serial.println("[CSI] STALLED: 0 frames received in 3s — "
                   "check antenna / WiFi mode");
  } else if (stats.frames_dropped_full > 0 && stats.windows_emitted == 0) {
    Serial.printf("[CSI] DROPS: %lu frames dropped (ring full), windows=0 — "
                  "main loop starved\n",
                  (unsigned long)stats.frames_dropped_full);
  } else {
    Serial.printf("[CSI] OK: %lu frames received, %lu windows emitted in 3s\n",
                  (unsigned long)stats.frames_received,
                  (unsigned long)stats.windows_emitted);
  }
}

/* ──────────────────────────────────────────────────────────────────────────
 * DIAGNOSTIC ACCESSORS — for /api/status
 * ────────────────────────────────────────────────────────────────────────── */

bool snapshot_valid() {
  return g_snapshot.valid;
}

bool csi_running() {
  return csi_hal::is_running();
}

bool csi_get_stats(csi_stats_t* out) {
  if (!out) return false;
  return csi_hal::get_stats(out);
}

/* Single source of truth for lowercase hex encoding. Moved out of the
 * anonymous namespace so csi_mqtt and other exporters can call it
 * with a qualified name, and so the internal session/pair-token
 * issuance paths and the new csi_mqtt::publish_chain converge on one
 * implementation (PR #394 review r3213674564). */
void hex_encode(const uint8_t* in, size_t len, char* out) {
  static const char* H = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    out[2*i  ] = H[(in[i] >> 4) & 0xF];
    out[2*i+1] = H[ in[i]       & 0xF];
  }
  out[2*len] = '\0';
}

}  /* namespace csi_integration */
