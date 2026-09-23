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
 *   3. Pairing (repo sweep F27) happens only through the proximity window:
 *      ble_scout_on_advert() hands the MAC of the first strong unpaired
 *      advert to ble_scout_pair() and nothing else. The HTTP surface arms
 *      the window and reads back hashed_id + label copies; no API, log or
 *      persisted byte carries a MAC.
 */

#include "ble_scout.h"
#include "ble_scout_state.h"
#include "ble_scout_key.h"
#include "ble_scout_registry_store.h"
#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
#include "fleet_roster_feed.h"   // expire the fleet roster on the same ~1 Hz tick
#endif

#include "csi_event.h"
#include "csi_module.h"

#include <string.h>
#include <atomic>

#ifndef CSI_TEST_HOST_BUILD
  #include <Arduino.h>      /* millis(), portMUX */
  #include <Preferences.h>  /* the paired-registry blob (loop task only) */
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
  namespace ble_scout { bool nimble_scan_init(); bool nimble_scan_start(); void nimble_scan_recover(); }
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
pairing::Window     s_window;
beacon_event_broadcast_fn s_broadcast_cb = nullptr;

/* A pair/unpair changed the registry; the loop task (ble_scout_tick) owes
 * the NVS blob. Set from the NimBLE host task (window pairing) and the HTTP
 * task (unpair), cleared by the loop — hence atomic. */
std::atomic<bool>   s_registry_dirty{false};

/* Loop-task-only scratch for the persisted blob (kept off the stack). */
uint8_t             s_blob[registry_store::BLOB_LEN];

/* One lock for the registry, the presence tracker and the pairing window —
 * three tasks touch them (see ble_scout.h "Threading"). Held only around
 * the table operations themselves: hashing, event emits and NVS I/O run
 * outside it. The host build is single-threaded. */
#ifdef CSI_TEST_HOST_BUILD
  #define SCOUT_LOCK()   do { } while (0)
  #define SCOUT_UNLOCK() do { } while (0)
#else
  portMUX_TYPE      s_scout_mux = portMUX_INITIALIZER_UNLOCKED;
  #define SCOUT_LOCK()   portENTER_CRITICAL(&s_scout_mux)
  #define SCOUT_UNLOCK() portEXIT_CRITICAL(&s_scout_mux)
#endif

/* Copy a registry label out while the lock is held. */
void copy_label(char out[ble_scan::MAX_LABEL_LEN + 1],
                const ble_scan::PairedBeacon* p) {
  if (p == nullptr) {
    out[0] = '\0';
    return;
  }
  memcpy(out, p->label, ble_scan::MAX_LABEL_LEN + 1);
  out[ble_scan::MAX_LABEL_LEN] = '\0';
}

/* Loop task only: write the registry blob. Returns false when the write did
 * not land (the caller keeps the dirty flag so the next tick retries). */
bool persist_registry() {
  SCOUT_LOCK();
  const size_t n = registry_store::serialize(&s_registry, s_blob, sizeof(s_blob));
  SCOUT_UNLOCK();
  if (n == 0) return false;
#ifdef CSI_TEST_HOST_BUILD
  return true;   /* no NVS on the host; serialize() above is the tested half */
#else
  Preferences prefs;
  if (!prefs.begin(registry_store::NVS_NAMESPACE, /*readOnly=*/false)) return false;
  const size_t put = prefs.putBytes(registry_store::NVS_KEY, s_blob, n);
  prefs.end();
  return put == n;
#endif
}

/* Init-time load of the persisted registry. A missing or malformed blob
 * leaves the registry empty (deserialize is all-or-nothing). */
void load_registry() {
#ifndef CSI_TEST_HOST_BUILD
  Preferences prefs;
  if (!prefs.begin(registry_store::NVS_NAMESPACE, /*readOnly=*/true)) return;
  const size_t got = prefs.getBytes(registry_store::NVS_KEY, s_blob, sizeof(s_blob));
  prefs.end();
  if (got != registry_store::BLOB_LEN) return;
  SCOUT_LOCK();
  (void)registry_store::deserialize(&s_registry, s_blob, got);
  SCOUT_UNLOCK();
#endif
}

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
  const uint32_t now = now_ms_impl();
  ble_scout_tick(now);
#if BLE_SCOUT_HAS_NIMBLE
  /* Scan-end recovery: onScanEnd restarts a dead scan inline; when the radio
   * refused right then, this re-arms it at the ~1 Hz tick. A deliberate
   * nimble_scan_stop() stays stopped (it clears the pending flag). */
  nimble_scan_recover();
#endif
#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
  /* Same ~1 Hz cadence ages stale peers out of the fleet roster the scan
   * callback feeds (fleet_roster_feed) — the roster's own 120 s window does
   * the real work; this just keeps the live count honest between sightings. */
  fleet_roster_feed::tick(now);
#endif
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
    SCOUT_LOCK();
    ble_scan::registry_init(&s_registry);
    presence_init(&s_tracker);
    pairing::init(&s_window);
    SCOUT_UNLOCK();
    load_registry();
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

  SCOUT_LOCK();
  const bool ok = ble_scan::registry_add(&s_registry, hashed, label);
  SCOUT_UNLOCK();
  if (ok) s_registry_dirty.store(true);
  return ok;
}

bool ble_scout_unpair(const uint8_t hashed_id[ble_scan::HASHED_ID_LEN]) {
  if (!s_inited) return false;
  SCOUT_LOCK();
  presence_forget(&s_tracker, hashed_id);
  const bool removed = ble_scan::registry_remove(&s_registry, hashed_id);
  SCOUT_UNLOCK();
  if (removed) s_registry_dirty.store(true);
  return removed;
}

size_t ble_scout_count() {
  if (!s_inited) return 0;
  SCOUT_LOCK();
  const size_t n = ble_scan::registry_count(&s_registry);
  SCOUT_UNLOCK();
  return n;
}

pairing::ArmResult ble_scout_pair_window_start(const char* label,
                                               uint32_t    window_ms,
                                               int         rssi_min,
                                               uint32_t    now_ms) {
  if (!s_inited) return pairing::ArmResult::NOT_READY;
  SCOUT_LOCK();
  pairing::ArmResult r;
  if (ble_scan::registry_count(&s_registry) >= ble_scan::MAX_PAIRED_BEACONS) {
    r = pairing::ArmResult::REGISTRY_FULL;
  } else {
    r = pairing::arm(&s_window, label, window_ms, rssi_min, now_ms);
  }
  SCOUT_UNLOCK();
  return r;
}

bool ble_scout_pair_window_cancel(uint32_t now_ms) {
  SCOUT_LOCK();
  const bool canceled = pairing::cancel(&s_window, now_ms);
  SCOUT_UNLOCK();
  return canceled;
}

pairing::Status ble_scout_pair_window_status(uint32_t now_ms) {
  SCOUT_LOCK();
  const pairing::Status st = pairing::status(&s_window, now_ms);
  SCOUT_UNLOCK();
  return st;
}

size_t ble_scout_registry_snapshot(ble_scan::PairedBeacon* out, size_t max) {
  if (out == nullptr || max == 0) return 0;
  size_t n = 0;
  SCOUT_LOCK();
  for (size_t i = 0; i < ble_scan::MAX_PAIRED_BEACONS && n < max; ++i) {
    if (s_registry.slots[i].in_use) out[n++] = s_registry.slots[i];
  }
  SCOUT_UNLOCK();
  return n;
}

bool ble_scout_registry_dirty() {
  return s_registry_dirty.load();
}

void ble_scout_tick(uint32_t now_ms) {
  if (!s_inited) return;

  /* Buffer sized to MAX_PAIRED_BEACONS so a tick that times-out every
   * paired beacon simultaneously (e.g. the home WiFi blackout case)
   * doesn't silently drop departed events past the first few. At
   * 16 beacons × 16 bytes = 256 bytes on the stack — well within
   * the tick handler's safety margin. */
  uint8_t departed_ids[ble_scan::MAX_PAIRED_BEACONS * ble_scan::HASHED_ID_LEN];
  SCOUT_LOCK();
  size_t n = presence_on_tick(&s_tracker, now_ms,
                              departed_ids,
                              ble_scan::MAX_PAIRED_BEACONS);
  (void)pairing::expire(&s_window, now_ms);
  SCOUT_UNLOCK();
  /* Defensive cap: presence_on_tick returns the count of transitions
   * but only writes up to MAX_PAIRED_BEACONS ids into the buffer.
   * Today the buffer matches, so n ≤ MAX_PAIRED_BEACONS, but the
   * explicit bound localizes the invariant and protects against a
   * future buffer-size / tracker-size mismatch silently overrunning
   * the read. */
  for (size_t i = 0; i < n && i < ble_scan::MAX_PAIRED_BEACONS; ++i) {
    char label[ble_scan::MAX_LABEL_LEN + 1];
    SCOUT_LOCK();
    copy_label(label,
               ble_scan::registry_find(&s_registry,
                                       departed_ids + i * ble_scan::HASHED_ID_LEN));
    SCOUT_UNLOCK();
    emit_departed(label);
  }

  /* NVS writes stay on this (loop) task. A failed write keeps the flag set
   * and retries on the next ~1 Hz tick. */
  if (s_registry_dirty.exchange(false) && !persist_registry()) {
    s_registry_dirty.store(true);
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
   * sub-microsecond on ESP32-S3. An unpaired advert is first offered to
   * the proximity pairing window (F27): the first one at/above the
   * window's threshold claims it, and its MAC goes to ble_scout_pair()
   * right here — the only place the raw bytes exist. Already-paired
   * beacons never consume the window. */
  char label[ble_scan::MAX_LABEL_LEN + 1] = {0};
  PresenceEvent e = PresenceEvent::NONE;
  bool pair_now = false;
  SCOUT_LOCK();
  const ble_scan::PairedBeacon* paired =
    ble_scan::registry_find(&s_registry, hashed);
  if (paired != nullptr) {
    copy_label(label, paired);
    e = presence_on_advert(&s_tracker, hashed, rssi_dbm, now_ms);
  } else {
    pair_now = pairing::offer(&s_window, rssi_dbm, now_ms);
    if (pair_now) memcpy(label, s_window.label, sizeof(label));
  }
  SCOUT_UNLOCK();

  if (pair_now) {
    label[ble_scan::MAX_LABEL_LEN] = '\0';
    const bool ok = ble_scout_pair(mac, label);
    SCOUT_LOCK();
    pairing::finish(&s_window, ok, hashed);
    SCOUT_UNLOCK();
    return;   /* presence starts with the beacon's next advert */
  }

  if (e == PresenceEvent::ARRIVED)  emit_arrived(label);
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
