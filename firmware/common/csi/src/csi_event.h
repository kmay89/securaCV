/**
 * @file csi_event.h
 * @brief Privacy chokepoint for module-emitted events (pillar A of the plan).
 *
 * Every event a module wants to publish flows through `csi_event::emit()`.
 * The chokepoint:
 *   1. Looks up the module's manifest entry for the given event type.
 *   2. Verifies the event is permitted (registered + allow-list intersected).
 *   3. Coarsens timestamps to the contract's bucket size (10 minutes).
 *   4. Strips fields not on the allow-list.
 *   5. Hands the cleaned event to the bundler (csi_bundler.h).
 *   6. When the bundler commits, routes through the optional witness chain
 *      and the optional outbound stream callback.
 *   7. Gives every committed row its event_id AT COMMIT, from one allocator
 *      (backlog F46): bundled and direct rows share one id space, and ids
 *      rise in commit order, on every task. See csi_event_id_floor.h for
 *      where the space starts (kIdSpaceBase) and why.
 *
 * The chokepoint is the only path from module to the rest of the system;
 * there is no back-channel. This is what makes the privacy contract
 * verifiable rather than aspirational.
 */

#ifndef SECURACV_CSI_EVENT_H
#define SECURACV_CSI_EVENT_H

#include "csi_module.h"
#include "csi_types.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * EVENT VALUE STRUCT
 *
 * Modules build one of these and call emit(). Only fields whose corresponding
 * bit is set in `present_fields` AND in the manifest's `allowed_fields` are
 * persisted; all others are silently dropped at the chokepoint.
 *
 * Strings are short, fixed-length, never user-typed. The chokepoint enforces
 * length limits and ASCII-only.
 * ────────────────────────────────────────────────────────────────────────── */

#define CSI_EVENT_NAME_MAX        24   /* state_name, dominant_signal, note */
#define CSI_EVENT_CONFIDENCE_MAX  16

typedef enum {
  CSI_CATEGORY_AMBIENT = 0,  /* never persisted, drives live UI only */
  CSI_CATEGORY_EVENT   = 1,  /* persisted, eligible for ribbon */
  CSI_CATEGORY_ANOMALY = 2,  /* persisted, eligible for prominent surfacing */
} csi_event_category_t;

typedef struct {
  /* Bitmask of csi_event_field_t values that the caller has populated.
   * Anything not in the manifest's allow-list will be zeroed before
   * persistence. */
  uint32_t              present_fields;

  /* Required regardless of allow-list — these are intrinsic to the event
   * row itself and cannot be stripped. */
  csi_event_category_t  category;

  /* Optional fields below; populated based on present_fields bits. */
  char                  state_name[CSI_EVENT_NAME_MAX];
  char                  confidence[CSI_EVENT_CONFIDENCE_MAX];
  char                  dominant_signal[CSI_EVENT_NAME_MAX];
  char                  note[CSI_EVENT_NAME_MAX];

  uint16_t              duration_sec;       /* coarse, capped at 65535 */
  uint8_t               time_bucket;        /* 0..143 ten-minute buckets */
  uint8_t               motion_score;       /* 0..100 */
  uint8_t               breathing_score;    /* 0..100 */
  uint8_t               breathing_rate_bpm; /* approximate; P1 only */
  uint16_t              bundled_count;      /* for bundler-output rows */
  uint8_t               dismissed;          /* 0 / 1 */
} csi_event_values_t;

/* Zero-initialize a values struct in a way the compiler can't optimize away. */
void csi_event_values_init(csi_event_values_t* out);

/* ──────────────────────────────────────────────────────────────────────────
 * EMIT
 * ────────────────────────────────────────────────────────────────────────── */

/**
 * Emit one event from a registered module.
 *
 * Parameters:
 *   module_id   Stable id of the emitting module. MUST match a registered
 *               module; unregistered ids are silently dropped.
 *   type_name   Event type. MUST match one of the module's manifest entries;
 *               otherwise dropped.
 *   values      Fields the module wants to publish. The chokepoint copies
 *               this struct, so the caller may discard its copy after emit.
 *
 * Returns non-zero on success, or 0 if the event was rejected (privacy /
 * allow-list / module not registered / values malformed). Rejection is
 * silent by design — leaking "this would have been blocked" is itself a
 * side channel.
 *
 * A direct commit (ambient, or a stateless emit) returns its event_id. A
 * state-bearing emit goes into an open bundle, which has no event id until
 * it commits; it returns the bundle's HANDLE instead, in
 * [csi_event_id_floor::kHandleBase, kIdSpaceBase): never an event id, so
 * nothing can mistake one for the other (backlog F46).
 *
 * Thread safety: callable from any task that produces CSI windows (the
 * loop task; ble.scout's NimBLE host task). Commits are serialized by the
 * chokepoint's commit lock. NOT callable from an ISR, and never from inside
 * a commit hook.
 */
uint32_t csi_event_emit(const char*               module_id,
                        const char*               type_name,
                        const csi_event_values_t* values);

/**
 * Force-flush any open bundles. Called by the runtime at shutdown,
 * at the daily-summary boundary, and when the user opens the Today sheet
 * (so the most recent activity shows up promptly even if the bundle
 * window is still open).
 */
void csi_event_flush_bundles(void);

/* ──────────────────────────────────────────────────────────────────────────
 * COMMIT HOOKS (optional — host application provides)
 *
 * The library is standalone-buildable; if the host doesn't override these,
 * commits become no-ops (events still drive the in-memory ring used by the
 * SSE stream and the activity ribbon, but never touch persistent storage).
 *
 * Implementations override the weak symbols in csi_event.cpp.
 * ────────────────────────────────────────────────────────────────────────── */

/**
 * Commit a fully-cleaned event to the witness chain (Ed25519-signed,
 * hash-chained). Return true on success. Called only for P0 events and
 * for P1 events whose category is EVENT or ANOMALY.
 */
bool csi_event_commit_witness(uint32_t                  event_id,
                              const char*               module_id,
                              const char*               type_name,
                              csi_event_category_t      category,
                              const csi_event_values_t* values);

/**
 * Optional outbound-stream hook. Called for every emitted event AFTER the
 * privacy filter has stripped disallowed fields. The host uses this to feed
 * the SSE stream at /api/csi/stream and the live ribbon. Never blocks.
 *
 * Privacy classes higher than the host's current consent level are NOT
 * delivered to this hook; the chokepoint enforces it.
 */
void csi_event_on_committed(uint32_t                  event_id,
                            const char*               module_id,
                            const char*               type_name,
                            csi_event_category_t      category,
                            csi_privacy_class_t       privacy,
                            const csi_event_values_t* values);

/* ──────────────────────────────────────────────────────────────────────────
 * RUNTIME CONFIGURATION
 * ────────────────────────────────────────────────────────────────────────── */

/**
 * Set the host's currently-consented privacy ceiling. P0 is the default.
 * Modules emitting events above this ceiling are rejected. The host must
 * call this when the user toggles "Enable detailed metrics" in settings.
 */
void csi_event_set_privacy_ceiling(csi_privacy_class_t ceiling);

csi_privacy_class_t csi_event_get_privacy_ceiling(void);

/**
 * Per-module per-hour ceiling override. The default ceiling is taken from
 * the module's manifest entry (`default_ceiling_per_hour`); pass override=0
 * to revert to manifest default.
 */
void csi_event_set_module_ceiling(const char* module_id, uint8_t override_per_hour);

/**
 * Align the chokepoint's `time_bucket` derivation to wall clock. Pass the
 * delta, in minutes, between the host's wall clock and our monotonic
 * uptime — i.e. `(wall_minutes_since_local_midnight) - (millis()/60000)`,
 * where local midnight is the household time zone's (repo sweep F28:
 * tz_rule::local_minute_of_day; UTC while no zone is set).
 *
 * Both hosts recompute it on every loop pass with a set clock, which keeps
 * it drift-corrected and carries DST transitions and zone changes. Without
 * this call, time_bucket is consistent within a session but unaligned
 * with wall clock (rolls over at boot+0, not midnight).
 */
void csi_event_set_clock_offset_minutes(int32_t offset_minutes);

/**
 * Current 10-minute wall-clock bucket (0..143) — the exact derivation
 * coarsen_time_fields() applies to emitted events, exposed so callers that
 * stamp a coarse bucket into their OWN artifacts (e.g. the sealed-snapshot
 * vault header) stay consistent with the chokepoint's coarsening. Call from
 * the main loop task only (reads the loop-owned clock offset).
 */
uint8_t csi_event_current_bucket(void);

/**
 * Configure the Quiet Hours window. While `enabled` and the current
 * minute-of-day (derived from monotonic time + clock offset) is inside
 * [start_min, end_min) (inclusive of midnight wrap), the chokepoint
 * suppresses non-anomaly emits and increments an internal hold counter
 * instead. At the first emit AFTER the window closes (or after the
 * setter is called with config that puts the clock outside the
 * window), the chokepoint synthesises a single `held_summary` event
 * through the registered `meta.quiet_hours` module so the dashboard
 * can render one row representing the suppressed window. Anomaly
 * events (CSI_CATEGORY_ANOMALY) always pass through — the night-time
 * category is precisely when unusual activity matters most.
 *
 * `start_min` and `end_min` are minutes-of-day in [0, 1440). A window
 * may cross midnight (start > end). `start_min == end_min` disables
 * the gate even if `enabled` is true.
 *
 * Thread safety: this setter performs only single-word state writes
 * (start_min, end_min, enabled). It DOES NOT flush the held summary
 * itself — that runs from the next emit on whichever task drives
 * emit() (the main loop on canary-wap). This keeps the chokepoint's
 * "emit() is single-task" invariant intact even when the host's
 * /api/settings POST handler calls this setter from a different task
 * than the one running module ticks. Held events accumulated under
 * the previous config are flushed by the transition detector inside
 * emit() the next time it runs.
 */
void csi_event_set_quiet_window(uint16_t start_min,
                                uint16_t end_min,
                                bool     enabled);

/* ──────────────────────────────────────────────────────────────────────────
 * INTROSPECTION (for /api/events/today and the dashboard ribbon)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  uint32_t              event_id;
  uint32_t              first_seen_ms;   /* monotonic; coarsened by 10 min on display */
  uint32_t              last_seen_ms;
  csi_event_category_t  category;
  csi_privacy_class_t   privacy;
  uint16_t              bundled_count;
  csi_event_values_t    values;
  char                  module_id[CSI_EVENT_NAME_MAX];
  char                  type_name[CSI_EVENT_NAME_MAX];
} csi_event_record_t;

/**
 * Copy up to `max` recent committed events into `out`. Returns the number
 * actually copied, ordered most recent first. Backed by an in-memory ring
 * sized for one day at the per-module ceiling.
 */
size_t csi_event_recent(csi_event_record_t* out, size_t max);

/**
 * Lookup one event by id (returns false if absent or evicted).
 */
bool csi_event_find(uint32_t event_id, csi_event_record_t* out);

/**
 * Mark an event dismissed by the user. Increments the values->dismissed flag,
 * routes a notification to the originating module, and updates the in-memory
 * record so the dashboard can gray it out. Local-only.
 */
bool csi_event_dismiss(uint32_t event_id);

/**
 * Put one record that an earlier boot committed back into the in-memory
 * ring, so /api/events/today shows the tail of the SD event log after a
 * reboot (canary-wap's csi_event_log::load_into_ring is the caller).
 *
 * Ring only. The event was committed and witnessed when it happened, so
 * this fires no commit hook, no witness write, no MQTT publish, no SD
 * append, allocates no id and spends no per-module ceiling.
 *
 * The record comes off a removable card, so this fails closed. It returns
 * false and leaves the ring untouched when:
 *   - `rec` is null or its event_id is 0;
 *   - its event_id is at or above the allocator's next id. The host
 *     restores the id floor from NVS before loading, so every id an earlier
 *     boot handed out is below it; an id at or above it is one this boot
 *     can still hand out (a lost floor, another device's card);
 *   - an event committed live this boot is already in the ring. Injecting
 *     after that would put older rows ahead of newer ones;
 *   - a row with the same event_id is already in the ring;
 *   - its module or event type is not registered on this build;
 *   - the privacy class the MANIFEST declares for that type (not the class
 *     the record claims) is above the current privacy ceiling.
 * What it keeps is cleaned as emit cleans it: fields the type's manifest
 * does not allow are zeroed, strings are sanitized, a time_bucket outside
 * 0..143 is zeroed, and the earlier boot's monotonic first/last-seen stamps
 * are cleared, since they mean nothing on this boot's clock. The dismissed
 * flag is kept: it is the owner's own mark, set whatever the manifest says.
 *
 * Call it only while the ring has no live rows (at boot); injecting
 * oldest-first leaves the newest record at the head, as a live commit would.
 */
bool csi_event_inject(const csi_event_record_t* rec);

/**
 * Reset all in-memory state. Used by tests. Does NOT touch the witness chain.
 */
void csi_event_test_reset(void);

/**
 * Move the event-id allocator's next-id floor up to at least `floor`.
 * No-op when floor <= the current next-id. Called by both hosts at boot
 * (canary-wap's csi_integration::init(), the canary's
 * csi_event_egress_begin()) to restore the persisted high-water-mark from
 * NVS so allocations stay globally monotone across reboots. The allocator
 * starts at csi_event_id_floor::kIdSpaceBase, so a floor an older firmware
 * persisted below it changes nothing (backlog F46); the hosts compute what
 * to pass with csi_event_id_floor::boot_floor().
 *
 * Without this, csi_mqtt's reconnect-backfill watermark would be
 * ambiguous (previous-boot id=1 collides with current-boot id=1) and
 * the cross-reboot history would either re-publish duplicates or skip
 * unsent events. PR #395 worked around it by clearing the SD log on
 * cold boot; this hook lets PR #397 keep the log instead.
 */
void csi_event_set_event_id_floor(uint32_t floor);

/**
 * Read the allocator's next-id (i.e. the value the next allocate_event_id
 * call would return + 1). The host throttle-persists this to NVS so
 * subsequent boots can restore via csi_event_set_event_id_floor.
 */
uint32_t csi_event_get_next_event_id(void);

/**
 * Weak hook fired on every successful event-id allocation. Standalone
 * library default is a no-op. canary-wap's csi_integration.cpp
 * provides a strong override that NVS-persists the floor every N
 * advances so a future reboot can resume from the last persist plus a
 * safety margin (we lose at most N ids but never reuse one).
 *
 * Don't call from inside emit() — by the time the hook fires, the
 * allocator already advanced and the caller is mid-flight.
 */
void csi_event_on_id_advance(uint32_t new_id);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_EVENT_H */
