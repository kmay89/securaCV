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
#include "csi_event_egress.h"     // committed events -> SD log + MQTT, in id order
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
#include <csi_traffic.h>
#include <csi_types.h>
#include <csi_module.h>
#include "csi_module_settings_nvs.h"  // the module settings' NVS rule, shared with the canary (F93)
#include "csi_settings_nvs.h"         // the modules' boot init (F93); stored Quiet Hours (F123, F128)
#include "csi_tune_lab.h"             // the Tuning Lab's knobs and its POST (F123, F128)
#include <csi_event.h>
#include "csi_event_id_floor.h"   // when to write the id floor (common/csi, host-tested)
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
#include "probe_airtime.h"         // probe sends reserve routine airtime

#include "build_config.h"
/* Unconditional, matching its unconditional registration below: every
 * profile and every board has a reset reason and an SD state machine.
 * (Its first cut sat inside the vault guard, which compiles only on
 * FULL+S3 — exactly the one config CI builds — so DEV/MINIMAL/C3 broke
 * invisibly. The adversarial review caught it; keep it out here.) */
#include "tamper_events_module.h"
#if FEATURE_VAULT_SNAPSHOT
#include "vault_events_module.h"
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
bool traffic_pinging();

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
 *   v[24..27] frame health
 *   v[28..29] wander / jitter (CSI_WANDER_JITTER builds only; else zero)
 *   v[30..31] reserved
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
 * The dashboard, the calibration and the Tuning Lab speak in dotted module
 * keys ("core.presence.pet_mode"); NVS keys are at most 15 characters, so
 * each is stored under a short key ("cp.pet_mode"). That map, and the read
 * rule the modules' csi_module_settings_* calls follow, are
 * csi_module_settings_nvs.h's: one table both trees read by, so a new
 * setting is a row there.
 *
 * Defined here (above HTTP HANDLERS) so the GET / POST handlers below
 * can reference SETTINGS_NS, nvs_key_for(), and reinit_module() without
 * forward declarations.
 * ────────────────────────────────────────────────────────────────────────── */

/* The module settings' namespace and key map are
 * csi_module_settings_nvs.h's, the one rule the canary reads them by too
 * (sweep F93). This TU's other "csi" keys (the time zone, the transmitter
 * filter, the event-id floor) share the namespace. The csi_module_settings_*
 * overrides that read them for the modules, and the modules' boot init,
 * are in csi_settings_nvs.cpp. */
constexpr const char* SETTINGS_NS = csi_module_settings_nvs::kNamespace;

using csi_module_settings_nvs::nvs_key_for;

/* Reinit a module whose settings changed: its init() re-reads them. Cheap
 * — modules are stateless apart from a few static counters that init()
 * resets. Called after a settings POST, a calibration apply or a Tuning
 * Lab change; the boot's init is csi_settings_nvs_init_modules() in init()
 * below. A NULL settings handle: each read opens NVS for itself. */
void reinit_module(const char* module_id) {
  const csi_module_t* m = csi_module_find(module_id);
  if (!m) return;
  if (m->deinit) m->deinit();
  if (m->init)   m->init(nullptr);
}

/* The stored Quiet Hours and their apply to the chokepoint
 * (apply_quiet_hours_from_nvs(), read_quiet_hours() and the one default
 * they share, 23:00 to 07:00, off) are csi_settings_nvs.cpp's, which a host
 * suite compiles (sweeps F123, F128). Called at boot (register_v1_modules),
 * on a Quiet Hours change through /api/settings, and by the Tuning Lab's
 * POST (tune_post()). */

/* Household time zone (repo sweep F28). NVS "csi"/"tz" holds the POSIX rule;
 * "tz.iana" the IANA name it was mapped from (for the dashboard to show), and
 * is removed when a rule is typed directly. Applied with setenv + tzset only —
 * configTzTime would also start SNTP, which this device deliberately lacks.
 * A missing or invalid stored value sets nothing: TZ stays unset = UTC. */
constexpr const char* NVS_KEY_TZ      = "tz";
constexpr const char* NVS_KEY_TZ_IANA = "tz.iana";

void apply_tz_rule(const char* rule) {
  setenv("TZ", rule, 1);
  tzset();
}

void apply_tz_from_nvs() {
  Preferences tprefs;
  if (!csi_module_settings_nvs::begin_read_only(tprefs)) return;
  char rule[tz_rule::MAX_POSIX_LEN + 1] = {0};
  if (tprefs.isKey(NVS_KEY_TZ)) tprefs.getString(NVS_KEY_TZ, rule, sizeof(rule));
  tprefs.end();
  if (tz_rule::posix_valid(rule)) apply_tz_rule(rule);
}

/* Persist + apply. Returns the resolution; nothing is written unless OK. */
tz_rule::Resolve store_tz(const char* posix, const char* iana) {
  char rule[tz_rule::MAX_POSIX_LEN + 1] = {0};
  const tz_rule::Resolve r = tz_rule::resolve(posix, iana, rule);
  if (r != tz_rule::Resolve::OK) return r;
  Preferences tprefs;
  if (!tprefs.begin(SETTINGS_NS, /*readOnly=*/false)) return tz_rule::Resolve::BAD_RULE;
  tprefs.putString(NVS_KEY_TZ, rule);
  const bool typed = posix != nullptr && posix[0] != '\0';
  if (!typed && iana != nullptr && strlen(iana) <= tz_rule::MAX_IANA_LEN) {
    tprefs.putString(NVS_KEY_TZ_IANA, iana);
  } else if (tprefs.isKey(NVS_KEY_TZ_IANA)) {
    tprefs.remove(NVS_KEY_TZ_IANA);
  }
  tprefs.end();
  apply_tz_rule(rule);
  return tz_rule::Resolve::OK;
}

/* Forget the zone: both keys removed, TZ unset — UTC again, as before F28. */
tz_rule::Resolve clear_tz() {
  Preferences tprefs;
  if (!tprefs.begin(SETTINGS_NS, /*readOnly=*/false)) return tz_rule::Resolve::BAD_RULE;
  if (tprefs.isKey(NVS_KEY_TZ))      tprefs.remove(NVS_KEY_TZ);
  if (tprefs.isKey(NVS_KEY_TZ_IANA)) tprefs.remove(NVS_KEY_TZ_IANA);
  tprefs.end();
  unsetenv("TZ");
  tzset();
  return tz_rule::Resolve::OK;
}

/* The persisted privacy ceiling's reader, store and apply
 * (apply_privacy_ceiling_from_nvs(), at boot and after a POST stores it) are
 * csi_settings_nvs.cpp's, by the shared key map's core.privacy_ceiling row
 * (sweep F151). */

/* Transmitter filter (csi_hal.h): accept CSI frames only from the router
 * this station is associated with (and registered peer Canaries). On by
 * default; the /api/settings key "filter_foreign" turns it off for an
 * install whose frame supply is something else. Same namespace as the
 * module settings, own key (not a module tunable). */
constexpr const char* NVS_KEY_FILTER_FOREIGN = "csi.ff";

bool read_filter_foreign_from_nvs() {
  Preferences prefs;
  if (!csi_module_settings_nvs::begin_read_only(prefs)) return true;
  const bool on = prefs.getBool(NVS_KEY_FILTER_FOREIGN, true);
  prefs.end();
  return on;
}

void apply_filter_foreign_from_nvs() {
  csi_hal::set_filter_foreign(read_filter_foreign_from_nvs());
}

/* ──────────────────────────────────────────────────────────────────────────
 * EVENT-ID FLOOR — NVS persistence
 *
 * Lifts the cross-reboot collision in event_id allocation. csi_event
 * starts from g_next_event_id = 1 every boot, so a previous-boot id=50
 * and a current-boot id=50 are indistinguishable to anything that
 * tracks ids — most notably the events egress's reconnect-backfill
 * watermark (csi_event_egress.cpp).
 * PR #395 worked around it by clearing the SD log on cold boot. This
 * commit removes that workaround by persisting the allocator's next-
 * id to NVS and restoring at boot.
 *
 * Persist cadence: common/csi/src/csi_event_id_floor.h, shared with the
 * canary PIO tree and host-tested across modeled reboots.
 * g_id_floor_stored is the value NVS holds; an allocation at or past it
 * writes "id + STRIDE" before the id goes out, so NVS is always above
 * every id handed out. After a reboot we restore from that value, and
 * the boot's first allocation writes again. Worst case a reboot skips
 * up to STRIDE ids (never reuses one): the scheme this replaced wrote
 * only every STRIDE ids and reused the ids of any boot shorter than
 * that. NVS write traffic stays bounded: one write per boot, plus ~14/day
 * at the per-module hourly ceiling (~6 events/hour, STRIDE=10), well
 * inside the cell wear budget.
 *
 * One id space (backlog F46): the allocator starts at kIdSpaceBase, above
 * every id an older firmware handed out, so a floor an older firmware
 * persisted changes nothing. The restore is boot_floor(): it also holds the
 * floor at or above the events egress's delivery ceiling (csi_mqtt::
 * NVS_KEY_DELIVERED, in this same namespace), so a boot whose floor writes
 * failed while its ceiling writes did not never reissues an id Home
 * Assistant already has. No extra write: the boot's first allocation is
 * the write. g_id_floor_stored stays what NVS holds for the floor key. ── */

constexpr const char*    NVS_KEY_EVENT_ID = "ev.next";
uint32_t                 g_id_floor_stored = 0;

/* True when NVS was read (the floor is now what it holds, or none was ever
 * stored); false when the namespace could not be opened, or (the first boot
 * after an NVS erase) does not exist yet: read_event_id_floor_rows() asks
 * quietly (sweep F150). This is the boot's first read of the namespace. */
bool apply_event_id_floor_from_nvs() {
  uint32_t persisted = 0;
  uint32_t delivered = 0;
  if (!read_event_id_floor_rows(NVS_KEY_EVENT_ID, csi_mqtt::NVS_KEY_DELIVERED, &persisted, &delivered)) {
    return false;
  }
  csi_event_set_event_id_floor(csi_event_id_floor::boot_floor(persisted, delivered));
  if (persisted > 0) g_id_floor_stored = persisted;
  return true;
}

void persist_event_id_floor(uint32_t new_id) {
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/false)) return;  // retried next id
  const uint32_t next_floor = csi_event_id_floor::floor_for(new_id);
  const bool wrote = prefs.putULong(NVS_KEY_EVENT_ID, (unsigned long)next_floor) > 0;
  prefs.end();
  if (wrote) g_id_floor_stored = next_floor;
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
  char supply[112];
  {
    /* The never-got-a-frame sentinel UINT32_MAX intentionally prints as
     * -1 through the int32 cast; the dashboard treats any negative as
     * "no frames yet". */
    const uint32_t silent = csi_hal::get_ms_since_last_frame();
    snprintf(supply, sizeof(supply),
      "\"supply\":{\"fps\":%u,\"probe\":%s,\"ping\":%s,\"silent_ms\":%d}",
      (unsigned)(g_have_latest_window ? g_latest_window.frames_in_window : 0),
      probe_running() ? "true" : "false",
      traffic_pinging() ? "true" : "false",
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
      /* Breathing is the PEAK bin, via the reducer core.presence and
       * anomaly.baseline share (csi_types.h): a clean breath is one bin
       * ≈40 and seven ≈0, so the 8-bin mean under-read the orb by ~8×. */
      (void)b;
      const int32_t m_avg = m / IDX_DOPPLER_COUNT;
      const int32_t b_avg = (int32_t)csi_breathing_peak(g_latest_window.v);
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

  /* The present tense, explicitly. Tamper rows are sealed-and-closed the
   * moment they commit (durability over bundling), so a client can no
   * longer read "still standing" off an open bundle — this envelope field
   * carries the standing condition instead. Absent means the device has
   * nothing to confess (never an empty object): boot kinds stand for the
   * boot, SD kinds clear on recovery (tamper_events_module.h). */
  const char* active = tamper_events_active_kind();
  if (active && active[0]) {
    char tail[64];
    const int tn = snprintf(tail, sizeof(tail),
                            "],\"tamper\":{\"kind\":\"%s\"}}", active);
    if (tn > 0 && (size_t)tn < sizeof(tail)) {
      httpd_resp_send_chunk(req, tail, tn);
    } else {
      httpd_resp_send_chunk(req, "]}", 2);
    }
  } else {
    httpd_resp_send_chunk(req, "]}", 2);
  }
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
  /* Persist it (written on the loop task by flush_dismissals), so the
   * reboot refill does not bring the event back undismissed. */
  if (ok && !csi_event_log::queue_dismissal(event_id)) {
    Serial.println("[EVT-LOG] dismissal queue full - this dismissal holds until reboot");
  }
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
  bool prefs_ok = csi_module_settings_nvs::begin_read_only(prefs);
  /* Through the key map, the rows core.presence reads (sweep F151); with
   * NVS not open, the reader's own defaults. */
  PresenceThresholds current = presence_threshold_defaults();
  if (prefs_ok) {
    current = read_presence_thresholds(prefs);
    prefs.end();
  }
  const int32_t cur_motion = current.motion;
  const int32_t cur_active = current.active;
  const int32_t cur_breath = current.breathing;

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
  /* Stored by the key map, on the rows core.presence's init() reads
   * (store_presence_thresholds(), sweep F151). */
  PresenceThresholds proposed;
  proposed.motion    = (int32_t)g_calibration.proposed_motion;
  proposed.active    = (int32_t)g_calibration.proposed_active;
  proposed.breathing = (int32_t)g_calibration.proposed_breathing;
  (void)store_presence_thresholds(prefs, proposed);
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
 * defined in csi_module_settings_nvs.h's key map, so future endpoint
 * expansion is purely additive.
 * ────────────────────────────────────────────────────────────────────────── */

esp_err_t handle_settings_get(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  Preferences prefs;
  if (!csi_module_settings_nvs::begin_read_only(prefs)) {
    /* Don't silently report defaults — the dashboard would reconcile
     * localStorage to those values and quietly clobber any choice the
     * user had previously made. Surface the unavailability so the
     * client skips reconciliation and keeps its current localStorage
     * source of truth. */
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"settings store unavailable\"}", -1);
    return ESP_OK;
  }
  /* core.presence's pet mode, preset and sensitivity, through the key map
   * the module reads them by, with its defaults (sweep F151). */
  const PresenceSettings presence = read_presence_settings(prefs);
  const bool    pet_mode    = presence.pet_mode;
  const int32_t preset_idx  = presence.preset;
  const int32_t sensitivity = presence.sensitivity;
  /* Quiet Hours through the one reader, with the one default the
   * chokepoint and the Tuning Lab use too (sweep F123). */
  const QuietHours qh       = read_quiet_hours(prefs);
  /* Privacy ceiling: persisted P0/P1/P2 choice (default P0 = anti-snitch).
   * Read separately from the in-memory chokepoint state (which apply_*
   * keeps in sync) so we always echo what's on disk, not what the
   * chokepoint thinks. */
  const int32_t privacy_raw = read_privacy_ceiling(prefs);
  /* Transmitter filter (default on). */
  const bool    filter_foreign = prefs.getBool(NVS_KEY_FILTER_FOREIGN, true);
  /* Household time zone (F28): "" while unset (the device keeps UTC). Both
   * values passed posix_valid / the IANA table on the way in, so they
   * carry no quote or backslash and print into the JSON as-is. */
  char tz[tz_rule::MAX_POSIX_LEN + 1] = {0};
  char tz_iana[tz_rule::MAX_IANA_LEN + 1] = {0};
  if (prefs.isKey(NVS_KEY_TZ))      prefs.getString(NVS_KEY_TZ, tz, sizeof(tz));
  if (prefs.isKey(NVS_KEY_TZ_IANA)) prefs.getString(NVS_KEY_TZ_IANA, tz_iana, sizeof(tz_iana));
  if (!tz_rule::posix_valid(tz)) tz[0] = '\0';
  if (tz_rule::posix_for_iana(tz_iana) == nullptr) tz_iana[0] = '\0';
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

  char buf[448];
  snprintf(buf, sizeof(buf),
    "{\"pet_mode\":%s,\"preset\":\"%s\",\"sensitivity\":%ld,"
     "\"quiet_hours\":{\"enabled\":%s,\"start_min\":%ld,\"end_min\":%ld},"
     "\"privacy_ceiling\":\"%s\",\"filter_foreign\":%s,"
     "\"tz\":\"%s\",\"tz_iana\":\"%s\"}",
    pet_mode ? "true" : "false", preset_str, (long)sensitivity,
    qh.enabled ? "true" : "false", (long)qh.start_min, (long)qh.end_min,
    privacy_str, filter_foreign ? "true" : "false", tz, tz_iana);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, -1);
  return ESP_OK;
}

esp_err_t handle_settings_post(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  /* Body is small JSON. Recognized keys (each stored under the shared key
   * map's row for its dotted key, csi_module_settings_nvs.h):
   *   "pet_mode":    true|false  → core.presence.pet_mode (bool)
   *   "preset":      "sensitive"|"balanced"|"quiet" → core.presence.preset (int 0..2)
   *   "sensitivity": 0..100      → core.presence.sensitivity (int)
   * Hand-parse to keep ArduinoJson out of this TU. We search for the
   * QUOTED key in every case so a body like {"not_pet_mode": true}
   * doesn't accidentally match. Buffer sized for the full payload:
   *   pet_mode + preset + sensitivity + quiet_hours{enabled, start, end}
   * is ~130 chars; 384 leaves room for the household time zone (F28:
   * "tz" up to 47 chars, "tz_iana" up to 47) on top. */
  char body[384];
  const int got = httpd_req_recv(req, body, sizeof(body) - 1);
  if (got <= 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"empty body\"}", -1);
    return ESP_OK;
  }
  body[got] = '\0';

  bool wrote_anything = false;

  /* "tz": a POSIX rule, or "tz_iana": an IANA zone the shared table maps
   * (repo sweep F28); "tz":"" alone clears the zone (back to UTC). Handled
   * FIRST and all-or-nothing: an unknown zone or an invalid rule is refused
   * by name before any other key in the body is written, so a 400 never
   * leaves half a settings change on disk. */
  if (strstr(body, "\"tz\"") != nullptr || strstr(body, "\"tz_iana\"") != nullptr) {
    char tz[tz_rule::MAX_POSIX_LEN + 1] = {0};
    char tz_iana[tz_rule::MAX_IANA_LEN + 1] = {0};
    const bool tz_key   = strstr(body, "\"tz\"") != nullptr;
    const bool iana_key = strstr(body, "\"tz_iana\"") != nullptr;
    if ((tz_key && !tz_rule::json_string_field(body, "\"tz\"", tz, sizeof(tz))) ||
        (iana_key && !tz_rule::json_string_field(body, "\"tz_iana\"", tz_iana, sizeof(tz_iana)))) {
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_send(req, "{\"ok\":false,\"reason\":\"bad time zone\"}", -1);
      return ESP_OK;
    }
    const tz_rule::Resolve r = (tz_key && tz[0] == '\0' && tz_iana[0] == '\0')
                                   ? clear_tz()
                                   : store_tz(tz, tz_iana);
    if (r == tz_rule::Resolve::UNKNOWN_ZONE) {
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_send(req, "{\"ok\":false,\"reason\":\"unknown zone\"}", -1);
      return ESP_OK;
    }
    if (r != tz_rule::Resolve::OK) {
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_send(req, "{\"ok\":false,\"reason\":\"bad time zone\"}", -1);
      return ESP_OK;
    }
    wrote_anything = true;
  }

  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/false)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"nvs unavailable\"}", -1);
    return ESP_OK;
  }

  /* "pet_mode", "preset" and "sensitivity": core.presence's own rows,
   * stored by store_presence_from_settings() (csi_settings_nvs.cpp) through
   * the shared key map its init() reads by (sweep F151). They were written
   * here by literal key, which no host suite compiled; test_wap_tune_lab.cpp
   * runs the store and its source pins hold this handler to it. */
  if (store_presence_from_settings(prefs, body)) wrote_anything = true;

  /* "quiet_hours": {"enabled": true|false, "start_min": M, "end_min": M},
   * stored by store_quiet_hours_from_settings() (csi_settings_nvs.cpp):
   * only the object's own fields, on the rows read_quiet_hours() reads,
   * through the shared key map. It is host-tested there with the reader
   * and the apply (test_wap_tune_lab.cpp, whose source pins hold this
   * handler to the call and to the apply below). */
  const bool qh_changed = store_quiet_hours_from_settings(prefs, body);
  if (qh_changed) wrote_anything = true;

  /* "privacy_ceiling": "p0" | "p1" | "p2", stored by
   * store_privacy_ceiling_from_settings() through the key map's
   * core.privacy_ceiling row, the one apply_privacy_ceiling_from_nvs() and
   * the GET above read (sweep F151). Unrecognized values store nothing. */
  const bool ceiling_changed = store_privacy_ceiling_from_settings(prefs, body);
  if (ceiling_changed) wrote_anything = true;

  /* "filter_foreign": true|false → csi.ff (bool). Applied to the HAL below
   * so the next frame sees it; no reboot. */
  bool filter_changed = false;
  if (const char* k = strstr(body, "\"filter_foreign\"")) {
    if (const char* v = strchr(k, ':')) {
      v++;
      while (*v == ' ' || *v == '\t' || *v == '"') v++;
      if (strncmp(v, "true", 4) == 0) {
        prefs.putBool(NVS_KEY_FILTER_FOREIGN, true);  wrote_anything = true; filter_changed = true;
      } else if (strncmp(v, "false", 5) == 0) {
        prefs.putBool(NVS_KEY_FILTER_FOREIGN, false); wrote_anything = true; filter_changed = true;
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

  if (filter_changed) apply_filter_foreign_from_nvs();

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
 * Hidden P2 surface at /tune. Lists every NVS-backed coefficient in the
 * shared key map (csi_module_settings_nvs.h) as a labeled slider with min,
 * max, default. Save/Load
 * preset writes/reads a local JSON bundle (no network egress) so a
 * tinkerer can ship a baseline between devices or back up before
 * experiments.
 *
 * The knobs' table (TUNE_COEFFS: label, kind, range, default, what a change
 * applies) and the POST's store-and-apply (tune_post()) are
 * csi_tune_lab.cpp's, which a host suite compiles (sweeps F123, F128);
 * these handlers are the HTTP around them.
 * ────────────────────────────────────────────────────────────────────────── */

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
  bool prefs_ok = csi_module_settings_nvs::begin_read_only(prefs);

  /* Stream out one big JSON object; chunked send keeps RAM bounded
   * even as the table grows past the 16-coefficient v1 set. */
  httpd_resp_send_chunk(req, "{\"coefficients\":[", -1);
  bool first = true;
  for (size_t i = 0; i < TUNE_COEFF_COUNT; ++i) {
    const TuneCoeff& c = TUNE_COEFFS[i];
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

/* POST /api/tune/coefficients (and /api/tune/preset, below): the body's
 * "<full_key>": value pairs are stored and applied by tune_post()
 * (csi_tune_lab.cpp): each module touched re-runs its init() through
 * reinit_module(), and a Quiet Hours knob re-applies the stored window to
 * the chokepoint (sweep F128), here on the HTTP server task as
 * /api/settings applies its own changes. Any unrecognized key is silently
 * ignored (P2; tinkerers are not expected to need detailed feedback on
 * typos). */
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

  const TunePost post = tune_post(body, reinit_module);
  free(body);

  if (!post.nvs_ok) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "{\"ok\":false,\"reason\":\"nvs unavailable\"}", -1);
  }
  if (post.changed == 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "{\"ok\":false,\"reason\":\"no recognized keys\"}", -1);
  }

  char ok[48];
  snprintf(ok, sizeof(ok), "{\"ok\":true,\"changed\":%d}", post.changed);
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
  bool prefs_ok = csi_module_settings_nvs::begin_read_only(prefs);

  httpd_resp_send_chunk(req, "{", -1);
  bool first = true;
  for (size_t i = 0; i < TUNE_COEFF_COUNT; ++i) {
    const TuneCoeff& c = TUNE_COEFFS[i];
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
 * Airtime: by the governor's estimate at ESP-NOW's default 1 Mbps
 * long-preamble rate, one framed probe frame (16 B payload + the ~59 B
 * framing allowance the governor adds) is 792 us (192 us + 8 us a byte, as
 * csi_probe.h's hand math says), so the 10 Hz idle broadcast is ~0.8 % of
 * the window — real, not negligible, and unicast fan-out to a filled peer
 * table would ask for far more. Every send therefore reserves against the
 * airtime governor's 2 % routine cap first (Config::airtime_gate below,
 * probe_airtime.h): the probe shares one budget with mesh
 * heartbeats/gossip and chirp presence, it starts no frame once the window
 * reads 1.60 % so those keep their room, and a saturated window skips
 * probe slots instead of degrading the user's WiFi.
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
    /* Probe frames are routine traffic, never forced: the 1.60 % ceiling
     * and the mesh-less governor bring-up are in probe_airtime.h, and the
     * governor adds the framing to the cost (host-tested by
     * test_csi_probe_airtime, which also pins these two lines). */
    probe_airtime::ensure_governor();
    pc.airtime_gate = probe_airtime::reserve_probe_frame;
    if (!csi_probe::init(pc)) return;   /* ESP-NOW not ready — retry */
    csi_probe::start();
    /* Transmitter filter: frames from registered peer Canaries are the
     * other legitimate link. The hook compares against the probe layer's
     * own RAM table in place — the HAL keeps no copy (csi_hal.h). Today
     * the WAP registers no peers (broadcast-only probe), so this is the
     * wiring for when the mesh layer fills that table. */
    csi_hal::set_peer_filter([](const uint8_t* mac) {
      return csi_probe::has_peer(mac);
    });
    s_probe_up = true;
    Serial.printf("[CSI] active probe up — %u Hz ESP-NOW broadcast "
                  "(peer Canaries sense off these frames)\n",
                  (unsigned)CSI_PROBE_BROADCAST_HZ);
    /* airtime_governor.cpp cannot log (host-compiled); its callers do. */
    if (!airtime_governor::ring_ok()) {
      Serial.printf("[CSI] airtime ring alloc failed — probe sends are "
                    "not governed\n");
    }
  } else if (csi_hal::is_running()) {
    /* Only spend TX airtime while local sensing runs — if csi_hal is
     * stopped (power policy, watchdog restart window) the radio budget
     * shouldn't go to lighting up the neighbors. */
    csi_probe::process();
  }
}

bool probe_running() { return csi_probe::is_running() && !csi_probe::is_paused(); }

/* ──────────────────────────────────────────────────────────────────────────
 * ROUTER ECHO SUPPLY
 *
 * probe_pump() lights up OTHER Canaries; a device cannot receive its own
 * transmissions, so a solo Canary on home Wi-Fi still lived on the AP's
 * ~10 Hz beacons and every window read "degraded". csi_traffic pings the
 * gateway at the HAL's target frame rate — each echo reply is a frame
 * addressed to this station, so the reply rate IS the CSI frame rate (the
 * same remedy espressif/esp-csi ships in its router-driven examples).
 * 20 Hz × ~70 B is ~1.4 kB/s. It runs only while csi_hal is running AND the
 * power gate allows CSI; the module polls the STA netif for link state and
 * gateway itself, so nothing here touches association or channel.
 * ────────────────────────────────────────────────────────────────────────── */
constexpr uint16_t CSI_TRAFFIC_PING_HZ = 20;

void traffic_pump(bool run_csi) {
  static bool s_traffic_init = false;
  if (!g_hal_ready) return;
  if (!s_traffic_init) {
    csi_traffic::TrafficConfig tc = csi_traffic::TrafficConfig::defaults();
    tc.rate_hz = CSI_TRAFFIC_PING_HZ;
    csi_traffic::init(tc);
    s_traffic_init = true;
  }
  csi_traffic::set_enabled(run_csi && csi_hal::is_running());
  csi_traffic::process();
}

bool traffic_pinging() { return csi_traffic::is_pinging(); }

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

/* The csi_module_settings_* overrides (NVS-backed, by
 * csi_module_settings_nvs.h's rule) are in csi_settings_nvs.cpp, a
 * TU the host tests compile (sweep F93). */

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

  /* The events egress (csi_event_egress.h): the MQTT `events` / `tamper`
   * publishes and the SD event log. Same privacy ceiling already gated the
   * snapshot write above, so we forward whatever we accepted into
   * g_snapshot — no new chokepoint to keep in sync. */
  csi_event_egress::on_committed(event_id, s->module_id, s->type_name, category, privacy, values);
}

/* ──────────────────────────────────────────────────────────────────────────
 * STRONG OVERRIDE — csi_event_on_id_advance
 *
 * Fires on every event-id allocation. We throttle-persist the floor to
 * NVS (csi_event_id_floor.h's STRIDE) so a subsequent boot can resume
 * from "persisted + safety_margin" via apply_event_id_floor_from_nvs.
 * Without this, a reboot restarts the allocator at kIdSpaceBase (backlog
 * F46) and hands out ids an earlier boot already used, so csi_mqtt's
 * reconnect-backfill watermark could not tell previous-boot events from
 * current-boot ones. ──────────────────────────── */

extern "C" void csi_event_on_id_advance(uint32_t new_id) {
  /* Cheap gate so we don't hit NVS on every event: one write per boot
   * plus one per STRIDE ids (csi_event_id_floor.h). Worst-case loss is
   * STRIDE ids on a hard reset, and none is ever reused. */
  if (!csi_event_id_floor::must_persist(g_id_floor_stored, new_id)) return;
  persist_event_id_floor(new_id);
}

/* ──────────────────────────────────────────────────────────────────────────
 * PUBLIC API
 * ────────────────────────────────────────────────────────────────────────── */

namespace csi_integration {

void set_legacy_features_hook(legacy_features_hook_t hook) {
  g_legacy_hook = hook;
}

void apply_timezone_from_nvs() {
  apply_tz_from_nvs();
}

tz_rule::Resolve set_timezone(const char* posix, const char* iana) {
  return store_tz(posix, iana);
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

  /* Restore the event-id floor from NVS so allocations stay globally
   * monotone across reboots. Done FIRST, before the modules register
   * (backlog F83), as the canary restores its floor in
   * csi_event_egress_begin() before its modules: a module may emit while it
   * registers (ble_scout_init() emits initialized("failed") when its key
   * store fails), and a commit there, with g_id_floor_stored still 0, would
   * allocate from kIdSpaceBase and write that floor over the persisted one
   * before this read it. Today that emit is state-bearing, so it only opens
   * a bundle and commits later, after this restore; this order does not
   * lean on that. With the floor restored, the events egress's backfill
   * watermark stays sound and csi_event_log no longer needs to wipe the
   * on-disk log on cold boot to avoid id collisions. */
  const bool floor_restored = apply_event_id_floor_from_nvs();

  /* The committed-event egress (csi_event_egress.h): its queue, and its
   * delivery watermark restored from the NVS ceiling with the floor just
   * restored (it reads event_id_floor_stored). After the floor, never
   * before: on the first boot of this firmware (no ceiling yet) the
   * restore treats every id below the floor as delivered (an earlier image
   * may have published it), and with no floor it would replay the card's
   * whole log into Home Assistant's replay gate. Before the modules
   * register, so any row they commit finds the queue there. */
  csi_event_egress::begin();

  register_v1_modules();

  /* Run every registered module's init() once, with its stored settings
   * (sweep F93). Registration alone initializes nothing, so until this
   * call every saved preset, threshold, pet mode and anomaly cooldown
   * applied only after a settings change in the same boot. After the floor
   * and the egress (no init() emits, and one that did would allocate from
   * the restored floor) and the modules; before the HAL installs the
   * features callback, the first tick (csi_module_tick_all ticks no module
   * before its init). One read-only NVS handle serves every init()
   * (csi_settings_nvs.cpp). check_wap_event_egress.py's rule 3 holds the
   * order. */
  csi_settings_nvs_init_modules();

  /* Restore persisted privacy ceiling (defaults to P0 — privacy-first).
   * Done before HAL start so the very first /api/csi/window request after
   * boot honors the user's prior choice rather than always 403'ing. */
  apply_privacy_ceiling_from_nvs();

  /* Refill the Today ring from the SD event log's tail. Needs the ceiling
   * and the floor above (csi_event_inject refuses a row this boot could
   * still allocate, and a type above the ceiling), so the load is armed
   * only once NVS has been read; before that (and on a boot where this
   * init never runs, e.g. the AP failed) load_into_ring() does nothing and
   * does not latch. Runs before the HAL so no live event can commit first.
   * No card yet: the loop's mount transition in canary_wap.ino calls it
   * again, and it runs once. */
  if (floor_restored) {
    csi_event_log::arm_load();
  } else {
    Serial.println("[EVT-LOG] event-id floor not readable from NVS - the log is not reloaded this boot");
  }
  (void)csi_event_log::load_into_ring();

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
  /* Transmitter filter: persisted choice, default on. The HAL learns the
   * associated BSSID itself from process(); the STA got-IP handler in
   * canary_wap.ino calls on_wifi_sta_connected() to make that immediate. */
  cfg.filter_foreign    = read_filter_foreign_from_nvs();
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

  /* Write any dismissal the HTTP handler queued, on this task, where every
   * other write to the SD event log happens. */
  (void)csi_event_log::flush_dismissals();

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
  /* The echo supply is pumped BEFORE the gate so it can also stop when
   * CSI is gated off; it stays idle while run_csi is false. */
  traffic_pump(run_csi);

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

uint32_t event_id_floor_stored() {
  return g_id_floor_stored;
}

bool csi_get_stats(csi_stats_t* out) {
  if (!out) return false;
  return csi_hal::get_stats(out);
}

bool csi_filter_foreign() { return csi_hal::get_filter_foreign(); }

bool csi_filter_armed() {
  /* "Comparing" needs both: the setting on and a BSSID to compare against.
   * has_associated_bssid() alone stays true after the setting is turned off
   * (the learned BSSID is kept), which would read as armed while every frame
   * passes. */
  return csi_hal::get_filter_foreign() && csi_hal::has_associated_bssid();
}

void on_wifi_sta_connected() {
  /* Runs on the Arduino Wi-Fi event task: only flag it; csi_hal::process()
   * does the driver call on the main loop. */
  if (g_hal_ready) csi_hal::request_bssid_refresh();
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
