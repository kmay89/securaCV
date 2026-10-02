/**
 * @file csi_module.h
 * @brief Modular sensing-pipeline interface (pillar B of the CSI plan).
 *
 * A "module" is a small, allocation-free agent that consumes the 1 Hz CSI
 * feature stream and emits domain events (motion, breathing, etc.) through
 * the single `csi_event::emit()` privacy chokepoint defined in csi_event.h.
 *
 * Why an interface and not just a callback chain?
 *   - Each module declares, at compile time, the privacy class of its events
 *     and the exact field allow-list per event type. The runtime enforces
 *     both on every emit. This makes "does this feature respect the privacy
 *     contract?" a build-time check, not a documentation promise.
 *   - Modules can be enabled/disabled at compile time via build flags or at
 *     runtime via the registry, without scattering #ifdefs through call sites.
 *   - Third-party developers can ship their own module out-of-tree without
 *     touching SecuraCV core.
 *
 * Modules MUST NOT reach into each other's state. Cross-module coordination
 * is via published events.
 */

#ifndef SECURACV_CSI_MODULE_H
#define SECURACV_CSI_MODULE_H

#include "csi_types.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * PRIVACY CLASSES
 * ────────────────────────────────────────────────────────────────────────── */

/**
 * The privacy class of an emitted event. The runtime uses this to gate
 * whether the event can be persisted, exported, or shown on the developer
 * stream. See spec/event_contract.md and spec/invariants.md.
 *
 *   P0 — Always-on. Contract-conformant by construction: aggregate counts
 *        and bucketed scalars only. Eligible for the witness chain, the
 *        activity ribbon, and the public 1 Hz stream.
 *
 *   P1 — Opt-in. Carries fields that are still anonymous but provide more
 *        detail (e.g. a numeric breathing-rate estimate). Requires an
 *        explicit user toggle in settings before emit() will accept it.
 *
 *   P2 — Power-user / developer disclosure. Never leaves the device. The
 *        Tuning Lab and the raw 32-dim feature heatmap fall here. Stored
 *        only in volatile RAM unless the user explicitly opts in.
 */
typedef enum {
  CSI_PRIVACY_P0 = 0,
  CSI_PRIVACY_P1 = 1,
  CSI_PRIVACY_P2 = 2,
} csi_privacy_class_t;

/* ──────────────────────────────────────────────────────────────────────────
 * EVENT DECLARATIONS (the per-module allow-list)
 * ────────────────────────────────────────────────────────────────────────── */

/**
 * Bitmask describing which fields a given event type is allowed to carry
 * when it goes through `csi_event::emit()`. The runtime drops or zeros
 * any field set on the event but not in this allow-list. This is the
 * teeth behind the privacy contract.
 *
 * The bit layout is intentionally small and stable so module manifests
 * remain ABI-compatible across firmware versions. Add new bits at the end.
 */
typedef enum {
  CSI_FIELD_NONE             = 0,
  CSI_FIELD_STATE_NAME       = 1u << 0,  /* short symbolic state, e.g. "active" */
  CSI_FIELD_CONFIDENCE       = 1u << 1,  /* "tentative" / "observed" / "confirmed" */
  CSI_FIELD_DURATION_SEC     = 1u << 2,  /* whole-second duration, coarse */
  CSI_FIELD_TIME_BUCKET      = 1u << 3,  /* 10-min bucket (0..143) */
  CSI_FIELD_MOTION_SCORE     = 1u << 4,  /* 0..100, P0 */
  CSI_FIELD_BREATHING_SCORE  = 1u << 5,  /* 0..100, P0 */
  CSI_FIELD_BREATHING_RATE   = 1u << 6,  /* approximate BPM, P1 */
  CSI_FIELD_DOMINANT_SIGNAL  = 1u << 7,  /* "motion" | "breathing" | "none" */
  CSI_FIELD_BUNDLED_COUNT    = 1u << 8,  /* if this row is a bundle of N raw events */
  CSI_FIELD_DISMISSED        = 1u << 9,  /* user marked "that was nothing" */
  CSI_FIELD_NOTE             = 1u << 10, /* short fixed-length tag, never freeform text */
} csi_event_field_t;

/**
 * One row in a module's manifest: a permitted event type, the fields it
 * may carry, and the privacy class for events of that type.
 *
 * `type_name` is a short stable string ("presence_changed") used in the
 * witness chain and the developer stream. It MUST NOT contain user-typed
 * data; it is a compile-time constant.
 */
typedef struct {
  const char*         type_name;
  uint32_t            allowed_fields;   /* OR of csi_event_field_t bits */
  csi_privacy_class_t privacy;
  uint8_t             default_ceiling_per_hour; /* 0 = no cap; bundler still applies */
} csi_event_decl_t;

/* ──────────────────────────────────────────────────────────────────────────
 * MODULE SETTINGS (shared with the Tuning Lab)
 * ────────────────────────────────────────────────────────────────────────── */

/**
 * Opaque settings handle passed to a module's init(). Modules read fields
 * by name via the helpers below; they don't need to know the underlying
 * storage format. Both firmware trees pass NULL: their strong overrides of
 * the helpers read NVS by key (csi_module_settings_nvs.h, one key map and
 * one read rule for both), so the handle carries nothing today.
 */
typedef struct csi_module_settings csi_module_settings_t;

/* Read a typed setting; returns the default if the key is absent. The
 * library's own definitions are weak and return the default; a host that
 * stores settings overrides all three. */
int32_t  csi_module_settings_int(const csi_module_settings_t*, const char* key, int32_t default_value);
float    csi_module_settings_float(const csi_module_settings_t*, const char* key, float default_value);
bool     csi_module_settings_bool(const csi_module_settings_t*, const char* key, bool default_value);

/* ──────────────────────────────────────────────────────────────────────────
 * THE MODULE INTERFACE
 * ────────────────────────────────────────────────────────────────────────── */

/**
 * Lifecycle and per-window callbacks. All function pointers may be NULL
 * except `tick`. The runtime guarantees:
 *   - init() is called once at boot, by csi_module_init_all(), before any
 *     tick(), and reads the module's persisted settings (or its defaults on
 *     a device that never stored one). csi_module_tick_all() does not tick
 *     a module whose boot init has not run. init() must not emit: it runs
 *     during the host's boot, before the SD event log refills the Today
 *     ring (which it does only while no row is live).
 *   - tick() is called from the main loop (NOT an ISR) at most once per
 *     CSI window (~1 Hz). Modules MUST NOT block.
 *   - on_event_dismissed() is called when the user swipes "That was nothing"
 *     on a previously-emitted event row in the dashboard. Local-only
 *     feedback; never crosses the network.
 *   - deinit() may be called at firmware shutdown or to disable the module.
 *     After deinit(), tick() will not be called again until init().
 */
typedef struct csi_module {
  /** Stable, dot-separated identifier. Convention: "<scope>.<name>",
   *  e.g. "core.presence", "core.breathing", "experimental.sleep_window".
   *  This string is used as the NVS prefix for the module's settings, the
   *  source field in emitted events, and the toggle key in build flags. */
  const char*               id;

  /** Default privacy class for events whose declaration omits a privacy
   *  field. Individual `events[i].privacy` overrides this. */
  csi_privacy_class_t       default_privacy;

  /** Allow-listed event types this module may emit. Pointed-to memory
   *  must outlive the module — a `static const csi_event_decl_t[]` is
   *  the standard pattern. */
  const csi_event_decl_t*   events;
  size_t                    event_count;

  /** Lifecycle. */
  void (*init)(const csi_module_settings_t* settings);
  void (*tick)(const csi_features_t* features);
  void (*on_event_dismissed)(uint32_t event_id);
  void (*deinit)(void);
} csi_module_t;

/* ──────────────────────────────────────────────────────────────────────────
 * REGISTRY
 * ────────────────────────────────────────────────────────────────────────── */

/**
 * Register a module with the runtime. Returns true on success, false if the
 * module's manifest is malformed (duplicate id, NULL tick, empty events
 * array, or an event whose `allowed_fields` contains a bit unknown to this
 * runtime build — i.e. the module was compiled against a newer header).
 *
 * Registration is one-shot; modules persist for the lifetime of the firmware.
 * A module with id `x.y` that tries to register a second time returns false.
 *
 * Maximum registered modules is bounded at compile time to avoid heap use;
 * see CSI_MODULE_MAX in csi_module.cpp.
 */
bool csi_module_register(const csi_module_t* module);

/** Number of registered modules (for diagnostics / Tuning Lab). */
size_t csi_module_count(void);

/** Lookup by id (returns NULL if absent). */
const csi_module_t* csi_module_find(const char* id);

/**
 * Run init() once for every registered module whose boot init has not run
 * yet, in registration order, passing `settings` through (both trees pass
 * NULL; see csi_module_settings_t). A module with no init() counts as
 * initialized. A second call runs nothing for a module already initialized,
 * so it initializes only modules registered since. Returns how many init()
 * calls it made.
 *
 * Registration alone does not initialize a module (sweep F93: neither tree
 * called init() at boot, so every stored setting was ignored until a
 * settings change in that boot on the canary-wap, and always on the
 * canary). Each host calls this once at boot, after its modules register
 * and after NVS is readable, the event-id floor is restored and the events
 * egress has begun, and before the first CSI window can tick a module.
 * A later settings change re-runs one module's init() directly (the
 * canary-wap's reinit_module()); that does not go through here.
 */
size_t csi_module_init_all(const csi_module_settings_t* settings);

/** Drive the tick() of every registered module whose boot init has run
 *  (csi_module_init_all) with the current features. The CSI HAL features
 *  callback should call this. */
void csi_module_tick_all(const csi_features_t* features);

/** Notify modules of a user dismissal. The runtime routes the call to the
 *  module that originally emitted `event_id`. */
void csi_module_dispatch_dismiss(uint32_t event_id);

/** Forget every registration, boot init and dismiss route, as a reboot
 *  does. Used by tests. Module-internal state is the modules' own. */
void csi_module_test_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_MODULE_H */
