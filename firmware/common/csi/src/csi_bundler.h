/**
 * @file csi_bundler.h
 * @brief Same-state event bundling within a 10-minute sliding window.
 *
 * Rule (pillar C of the plan):
 *   Same `(module, type, state_name)` tuple within a 10-minute window, with
 *   at most a 2-minute gap between contributing observations, collapses into
 *   ONE event whose duration spans the union. The dashboard shows
 *   `Active 7:14–7:32 PM · 18 min`, not twelve flickering rows.
 *
 * Bundling is the only path emitted events take to persistence. The chokepoint
 * (csi_event::emit) calls `csi_bundler_admit` which returns:
 *
 *   CSI_BUNDLER_OPENED   — emit opened a NEW bundle: a row that will commit
 *                         later (window close, quiet gap, or an explicit
 *                         flush). Out-param is the open bundle's HANDLE, not
 *                         an event id.
 *
 *   CSI_BUNDLER_MERGED   — emit was rolled into a bundle that was open, and
 *                         still open, when the admit ran: no new row.
 *                         Out-param is that bundle's handle.
 *
 *   CSI_BUNDLER_COMMIT   — the emit cannot be keyed (ambient, or no state
 *                         name): the chokepoint commits it directly.
 *                         Out-param is 0.
 *
 *   CSI_BUNDLER_DROPPED  — bad input (null module, type or values).
 *
 * The hourly ceiling is spent by what admit DID (sweep F80): an opening and
 * a direct commit each spend a slot, a merge gives its slot back. Asking
 * first whether a key is open is not the same question: admit expires an
 * overdue bundle (its quiet gap or its 10-minute window) before it matches,
 * and then opens a new one, a row a refund decided beforehand would let
 * through uncounted.
 *
 * Ids (backlog F46): a bundle takes its event id when it COMMITS, from the
 * chokepoint's one allocator (csi_event_commit_bundle_ in csi_event.cpp,
 * under the chokepoint's commit lock), so bundled and direct rows share one
 * id space and ids rise in commit order. Until then it is known by a handle
 * from [kHandleBase, kIdSpaceBase) (csi_event_id_floor.h), below every
 * event id, so an open row is never mistaken for a committed one.
 *
 * The bundler only operates on category=EVENT or category=ANOMALY. Ambient
 * is always passed through untouched (returns COMMIT immediately).
 */

#ifndef SECURACV_CSI_BUNDLER_H
#define SECURACV_CSI_BUNDLER_H

#include "csi_event.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  CSI_BUNDLER_COMMIT   = 0,   /* not bundled: commit it now */
  CSI_BUNDLER_OPENED   = 1,   /* opened a new bundle (a future row) */
  CSI_BUNDLER_DROPPED  = 2,   /* bad input */
  CSI_BUNDLER_MERGED   = 3,   /* rolled into a bundle already open (no row) */
} csi_bundler_outcome_t;

/* Window definitions, in milliseconds. The plan's 10-minute window and
 * 2-minute gap are surfaced as macros so the Tuning Lab can override at
 * compile time. */
#ifndef CSI_BUNDLER_WINDOW_MS
#define CSI_BUNDLER_WINDOW_MS  (10u * 60u * 1000u)
#endif
#ifndef CSI_BUNDLER_MAX_GAP_MS
#define CSI_BUNDLER_MAX_GAP_MS (2u * 60u * 1000u)
#endif

/**
 * Try to admit an event into the bundler. The bundler MAY mutate `values`
 * (e.g. update bundled_count and duration_sec when buffering). The privacy
 * class is captured per-bundle so the eventual close-time commit honors it.
 *
 * Outcomes:
 *   COMMIT   — caller should persist this emit immediately. Reserved for
 *              ambient and stateless emits that the bundler cannot key.
 *   OPENED   — emit opened a new bundle (after closing any overdue one,
 *              its own key's included). The bundle commits later, through
 *              the chokepoint, and takes its event id then. `*handle_out`
 *              carries the open bundle's handle (stable while it is open;
 *              not an event id).
 *   MERGED   — emit was rolled into the open bundle of its key; no new row.
 *              `*handle_out` carries that bundle's handle.
 *   DROPPED  — emit rejected (currently only on bad input).
 *
 * Merge-or-open is decided under the slot lock, after overdue bundles are
 * expired, so the outcome is the one fact the caller may spend the hourly
 * ceiling by (sweep F80).
 */
csi_bundler_outcome_t csi_bundler_admit(const char*           module_id,
                                        const char*           type_name,
                                        csi_privacy_class_t   privacy,
                                        csi_event_values_t*   values,
                                        uint32_t*             handle_out);

/**
 * Close any bundle past its window or quiet gap NOW, without waiting for
 * the next admissible emit. Admission-time expiry alone lets a room that
 * goes quiet after a state-bearing event hold its last bundle "open"
 * indefinitely — snapshot_open would keep reporting it as current, and a
 * client's present-tense claim would be false. Call once per main loop
 * (cheap — a bounded slot scan that closes only when overdue); the commit
 * runs on the caller's task, outside the slot lock, like every close.
 */
void csi_bundler_tick(void);

/**
 * Force-close every open bundle. Each closed bundle is committed through
 * the chokepoint (it takes its event id then) so the host can update its
 * persistence and UI. Called by csi_event_flush_bundles(), which no
 * firmware calls today (see csi_event.h); a host closes bundles with
 * csi_bundler_tick().
 */
void csi_bundler_flush_all(void);

/**
 * Force-close ONE open bundle by its exact (module, type, state) key, if it
 * is open, committing it through the same hooks as every other close. For
 * one-shot records that must not wait out the gap window: a reset story has
 * to reach the witness chain NOW, because the very failure it records — the
 * next crash or power loss — would erase a buffered bundle. No-op when
 * nothing matches.
 */
void csi_bundler_flush_key(const char* module_id,
                           const char* type_name,
                           const char* state_name);

/**
 * Reset all bundler state. Tests only.
 */
void csi_bundler_reset(void);

/**
 * Diagnostics: number of currently open bundles.
 */
size_t csi_bundler_open_count(void);

/**
 * Copy up to `max` currently OPEN bundles into `out`, newest activity first.
 * Safe to call from the HTTP server task: the slot table is mutex-guarded
 * (see csi_bundler.cpp's threading note), and each record is a consistent
 * copy — never a live pointer into a slot the main loop may close. An open
 * record's `event_id` is the bundle's HANDLE, in [kHandleBase,
 * kIdSpaceBase): the bundle has no event id until it commits (backlog F46).
 * Its `values.duration_sec` carries the LIVE span so far; its
 * `values.dismissed` is always 0 (only committed ring rows are dismissable).
 * This is what lets /api/events/today show an alarm while it is still
 * happening instead of only after its bundle closes.
 */
size_t csi_bundler_snapshot_open(csi_event_record_t* out, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_BUNDLER_H */
