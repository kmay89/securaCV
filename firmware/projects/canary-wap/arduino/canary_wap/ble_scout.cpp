/*
 * SecuraCV Canary — BLE Scout integration module — Implementation
 *
 * Compiles on host (CSI_TEST_HOST_BUILD) — pure-logic glue only.
 * The NimBLE scan loop lives in ble_scout_nimble.cpp and is the only
 * TU that links against the Bluetooth stack.
 *
 * Privacy invariants enforced here:
 *   1. Raw 6-byte MAC enters ble_scout_on_advert() and ble_scout_pair()
 *      ONLY. It is hashed immediately and the raw bytes never reach
 *      the registry, the tracker, or any event field.
 *   2. Events emitted ("scout_initialized", "beacon_arrived",
 *      "beacon_departed") carry only the privacy-chokepoint's
 *      allow-listed fields. No hashed_id is published in v1 either —
 *      the label travels in the note field (sanitized ASCII at the
 *      chokepoint) and consumers correlate via label only.
 *
 * VENDORED COPY — intentional divergence from the canonical library
 * (firmware/canary/lib/securacv_ble_scan/src/ble_scout.cpp), normalized away by
 * firmware/scripts/check_ble_scan_sync.sh: this copy carries no fleet_roster_feed
 * wiring. The canonical module_tick() also expires the fleet roster (the Scout's
 * second consumer, tracking OTHER Canaries); the WAP tracks its fleet through
 * the mesh layer instead, so fleet_roster_feed / fleet_roster.h are not staged
 * here. See ble_scout_nimble.cpp's header for the full divergence note.
 */

#include "ble_scout.h"
#include "ble_scout_state.h"
#include "ble_scout_key.h"

#include "csi_event.h"
#include "csi_module.h"

#include <string.h>

#ifndef CSI_TEST_HOST_BUILD
  #include <Arduino.h>   /* millis() */
#endif

/* canary-wap defines FEATURE_BLE_SCAN in build_config.h, while the PIO
 * build supplies it via -D in platformio.ini's build_flags. Pull the
 * header in here when present so the gate below sees the same flag in
 * both builds — without this, the canary-wap FULL profile silently
 * compiled with BLE_SCOUT_HAS_NIMBLE=0 and never started the scan loop. */
#if defined(__has_include)
  #if __has_include("build_config.h")
    #include "build_config.h"
  #endif
#endif

/* Forward-declare the NimBLE scan-loop entry points. Defined in
 * ble_scout_nimble.cpp, which is an EMPTY translation unit unless
 * FEATURE_BLE_SCAN=1 AND NimBLEDevice.h is available. We gate the
 * call site with the same condition so dev/release builds (no NimBLE
 * in lib_deps) don't try to link against absent symbols. */
#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN \
    && !defined(CSI_TEST_HOST_BUILD) \
    && __has_include(<NimBLEDevice.h>)
  #define BLE_SCOUT_HAS_NIMBLE 1
  namespace ble_scout { bool nimble_scan_init(); bool nimble_scan_start(); }
#else
  #define BLE_SCOUT_HAS_NIMBLE 0
#endif

namespace ble_scout {

namespace {

bool                s_inited       = false;
bool                s_scan_started = false;   /* tracks NimBLE scan-loop state separately
                                               * so a one-time scan-start failure can
                                               * be retried on the next init() call */
bool                s_radio_allowed = false;  /* NimBLE bring-up latch. csi_integration
                                               * calls ble_scout_init() at web-server
                                               * start — INSIDE the provisioning join
                                               * window, before the network's memory
                                               * accounting settles, and even in safe
                                               * mode. Until the .ino's post-join-window
                                               * gate flips this, init() does state-only
                                               * setup (Phase 1) and leaves the radio +
                                               * its heap spend alone. Device builds
                                               * only — the host path ignores it. */
ble_scan::Registry  s_registry;
PresenceTracker     s_tracker;
beacon_event_broadcast_fn s_broadcast_cb = nullptr;

inline uint32_t now_ms_impl() {
#ifdef CSI_TEST_HOST_BUILD
  /* Host build: presence_on_tick is driven by an explicit
   * caller-supplied timestamp via ble_scout_tick(); this fallback
   * isn't reached in the host test path. */
  return 0;
#else
  return millis();
#endif
}

/* ──────────────────────────────────────────────────────────────────────────
 * Event emission helpers
 *
 * Note: hashed_id is NOT placed in the note field. We carry only the
 * user-supplied label (already sanitized at pair time) so the wire
 * format contains zero secret-derived bytes. This matches the design
 * doc's "events expose room-attribution, never beacon identity."
 * ────────────────────────────────────────────────────────────────────────── */

void copy_label_to_note(csi_event_values_t* v, const char* label) {
  if (label == nullptr || label[0] == '\0') return;
  v->present_fields |= CSI_FIELD_NOTE;
  strncpy(v->note, label, sizeof(v->note) - 1);
  v->note[sizeof(v->note) - 1] = '\0';
}

void emit_arrived(const char* label) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_TIME_BUCKET;
  strncpy(v.state_name, "arrived", sizeof(v.state_name) - 1);
  v.state_name[sizeof(v.state_name) - 1] = '\0';
  copy_label_to_note(&v, label);
  (void)csi_event_emit("ble.scout", "beacon_event", &v);
  /* Mesh broadcast (PR 5c). Runs AFTER the local emit so the broadcast
   * inherits any chokepoint rate-limiting / privacy filtering decisions
   * by construction — if the local event was rate-limited, the broadcast
   * still happens (mesh has its own rate budget), but both pull from the
   * same label which is already sanitized at pair time. */
  if (s_broadcast_cb != nullptr) {
    s_broadcast_cb(/*arrived=*/true, label != nullptr ? label : "");
  }
}

void emit_departed(const char* label) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_TIME_BUCKET;
  strncpy(v.state_name, "departed", sizeof(v.state_name) - 1);
  v.state_name[sizeof(v.state_name) - 1] = '\0';
  copy_label_to_note(&v, label);
  (void)csi_event_emit("ble.scout", "beacon_event", &v);
  if (s_broadcast_cb != nullptr) {
    s_broadcast_cb(/*arrived=*/false, label != nullptr ? label : "");
  }
}

void emit_initialized(const char* status) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_TIME_BUCKET;
  strncpy(v.state_name, status, sizeof(v.state_name) - 1);
  v.state_name[sizeof(v.state_name) - 1] = '\0';
  (void)csi_event_emit("ble.scout", "scout_initialized", &v);
}

/* ──────────────────────────────────────────────────────────────────────────
 * csi_module manifest — registered by csi_modules_integration.cpp
 * ────────────────────────────────────────────────────────────────────────── */

void module_init(const csi_module_settings_t* /*s*/) {
  /* ble_scout_init() is called separately from main.cpp (it needs to
   * happen after NVS + WiFi-radio are up; csi_modules_init() runs
   * earlier in the boot path). This callback is a no-op so the
   * registry runtime sees a well-formed module. */
}

void module_tick(const csi_features_t* /*f*/) {
  /* on_tick fires once per CSI window (~1 Hz). That's the same
   * cadence we want for the LOST_MS lapsed-beacon check. */
  if (!s_inited) return;
  ble_scout_tick(now_ms_impl());
}

const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */                "scout_initialized",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  6,
  },
  {
    /* type_name */                "beacon_event",
    /* allowed_fields */            CSI_FIELD_STATE_NAME    /* "arrived" | "departed" */
                                  | CSI_FIELD_NOTE          /* sanitized label */
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  24,                     /* 1/min per beacon, bundled */
  },
};

const csi_module_t MODULE = {
  /* id */                  "ble.scout",
  /* default_privacy */     CSI_PRIVACY_P0,
  /* events */              EVENTS,
  /* event_count */         sizeof(EVENTS) / sizeof(EVENTS[0]),
  /* init */                module_init,
  /* tick */                module_tick,
  /* on_event_dismissed */  nullptr,
  /* deinit */              nullptr,
};

}  /* namespace */

/* ──────────────────────────────────────────────────────────────────────────
 * PUBLIC API
 * ────────────────────────────────────────────────────────────────────────── */

void ble_scout_allow_radio() {
  s_radio_allowed = true;
}

bool ble_scout_init() {
  /* Phase 1: load the per-device key + bring up the in-RAM registry
   * and tracker. Idempotent — only runs once. */
  if (!s_inited) {
    if (!ble_scout_key_init()) {
      /* Key store failed — emit a one-shot init-failed event so the
       * dashboard / health log can surface it. */
      emit_initialized("failed");
      return false;
    }
    ble_scan::registry_init(&s_registry);
    presence_init(&s_tracker);
    s_inited = true;
  }

#if BLE_SCOUT_HAS_NIMBLE
  /* Phase 2: bring up the NimBLE passive scanner — but ONLY once the
   * .ino's post-join-window gate has allowed radio activity (see
   * s_radio_allowed above). While disallowed, return silently: this is
   * the expected deferred state at web-server start, not a failure, so
   * no "scan_unavailable" event. The gate calls ble_scout_init() again
   * after flipping the latch; s_inited makes Phase 1 idempotent.
   * Tracked separately via s_scan_started so a transient scan-start
   * failure can be retried by a later init() call. The "ok" event
   * fires only on the success transition; "scan_unavailable" fires on
   * each real failed attempt but the chokepoint ceiling (6/hour) keeps
   * the wire chatter bounded. */
  if (!s_scan_started && s_radio_allowed) {
    if (nimble_scan_init() && nimble_scan_start()) {
      s_scan_started = true;
      emit_initialized("ok");
    } else {
      emit_initialized("scan_unavailable");
    }
  }
#else
  /* Host build / FEATURE_BLE_SCAN=0 path: scan loop is absent. Emit
   * "ok" once (the s_scan_started latch keeps it from re-firing). */
  if (!s_scan_started) {
    s_scan_started = true;
    emit_initialized("ok");
  }
#endif
  return true;
}

bool ble_scout_pair(const uint8_t mac[ble_scan::MAC_LEN],
                    const char*   label) {
  if (!s_inited || mac == nullptr) return false;

  uint8_t hashed[ble_scan::HASHED_ID_LEN];
  if (!ble_scout_key_hash_beacon(mac, hashed)) return false;

  return ble_scan::registry_add(&s_registry, hashed, label);
}

bool ble_scout_unpair(const uint8_t hashed_id[ble_scan::HASHED_ID_LEN]) {
  if (!s_inited) return false;
  presence_forget(&s_tracker, hashed_id);
  return ble_scan::registry_remove(&s_registry, hashed_id);
}

size_t ble_scout_count() {
  if (!s_inited) return 0;
  return ble_scan::registry_count(&s_registry);
}

void ble_scout_tick(uint32_t now_ms) {
  if (!s_inited) return;

  /* Buffer sized to MAX_PAIRED_BEACONS so a tick that times-out every
   * paired beacon simultaneously (e.g. the home WiFi blackout case)
   * doesn't silently drop departed events past the first few. At
   * 16 beacons × 16 bytes = 256 bytes on the stack — well within
   * the tick handler's safety margin. */
  uint8_t departed_ids[ble_scan::MAX_PAIRED_BEACONS * ble_scan::HASHED_ID_LEN];
  size_t n = presence_on_tick(&s_tracker, now_ms,
                              departed_ids,
                              ble_scan::MAX_PAIRED_BEACONS);
  /* Defensive cap: presence_on_tick returns the count of transitions
   * but only writes up to MAX_PAIRED_BEACONS ids into the buffer.
   * Today the buffer matches, so n ≤ MAX_PAIRED_BEACONS, but the
   * explicit bound localizes the invariant and protects against a
   * future buffer-size / tracker-size mismatch silently overrunning
   * the read. */
  for (size_t i = 0; i < n && i < ble_scan::MAX_PAIRED_BEACONS; ++i) {
    const ble_scan::PairedBeacon* p =
      ble_scan::registry_find(&s_registry,
                              departed_ids + i * ble_scan::HASHED_ID_LEN);
    emit_departed(p ? p->label : "");
  }
}

void ble_scout_on_advert(const uint8_t mac[ble_scan::MAC_LEN],
                         int8_t        rssi_dbm,
                         uint32_t      now_ms) {
  if (!s_inited || mac == nullptr) return;

  uint8_t hashed[ble_scan::HASHED_ID_LEN];
  if (!ble_scout_key_hash_beacon(mac, hashed)) return;

  /* Drop adverts from non-paired beacons in O(N) registry lookup.
   * MAX_PAIRED_BEACONS=16 so this is ≤16 16-byte memcmps per advert —
   * sub-microsecond on ESP32-S3. */
  const ble_scan::PairedBeacon* paired =
    ble_scan::registry_find(&s_registry, hashed);
  if (paired == nullptr) return;

  PresenceEvent e = presence_on_advert(&s_tracker, hashed, rssi_dbm, now_ms);
  if (e == PresenceEvent::ARRIVED)  emit_arrived(paired->label);
  /* DEPARTED isn't emitted here — it's a timer event surfaced by
   * ble_scout_tick(). on_advert only ever produces ARRIVED. */
}

const ::csi_module* ble_scout_module() {
  return &MODULE;
}

void set_broadcast_callback(beacon_event_broadcast_fn fn) {
  /* Last writer wins; pass nullptr to unhook. The store/read is plain
   * because both happen from the same task (the integration layer
   * installs at boot from setup(), and emit_arrived/departed run from
   * ble_scout_tick / on_advert — both on the main loop). */
  s_broadcast_cb = fn;
}

}  /* namespace ble_scout */
