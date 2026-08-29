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
 *   CSI_BUNDLER_BUFFERED — emit was rolled into an open bundle. The bundle
 *                         will commit later (window close or explicit flush).
 *                         Out-param event_id is the bundle's id.
 *
 *   CSI_BUNDLER_COMMIT   — emit is the first observation of a new bundle (or
 *                         is a different state from the open bundle and the
 *                         old bundle was just committed). The chokepoint
 *                         should now persist normally. Out-param event_id
 *                         may be 0 (chokepoint allocates) or non-zero (the
 *                         bundler reused a freshly-closed bundle's id).
 *
 *   CSI_BUNDLER_DROPPED  — the emit is a redundant ambient sample within a
 *                         very tight window and should be silently dropped.
 *                         (Reserved; current implementation does not drop.)
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
  CSI_BUNDLER_COMMIT   = 0,   /* emit immediately */
  CSI_BUNDLER_BUFFERED = 1,   /* rolled into an open bundle */
  CSI_BUNDLER_DROPPED  = 2,   /* dropped as redundant */
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
 *   BUFFERED — emit was accepted into a new or existing bundle. The bundle
 *              will commit later via close_slot()'s commit hooks.
 *              `*event_id_out` carries the bundle's stable event id.
 *   DROPPED  — emit rejected (currently only on bad input).
 */
csi_bundler_outcome_t csi_bundler_admit(const char*           module_id,
                                        const char*           type_name,
                                        csi_privacy_class_t   privacy,
                                        csi_event_values_t*   values,
                                        uint32_t*             event_id_out);

/**
 * Force-close every open bundle. Each closed bundle is re-committed via the
 * chokepoint's commit hooks so the host can update its persistence and UI.
 * Called by csi_event_flush_bundles() and at firmware shutdown.
 */
void csi_bundler_flush_all(void);

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
 * record's `values.duration_sec` carries the LIVE span so far; its
 * `values.dismissed` is always 0 (only committed ring rows are dismissable).
 * This is what lets /api/events/today show an alarm while it is still
 * happening instead of only after its bundle closes.
 */
size_t csi_bundler_snapshot_open(csi_event_record_t* out, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_BUNDLER_H */
