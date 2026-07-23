/**
 * @file wifi_channel_activity.h
 * @brief Ambient, unattributed "the airwaves near me just got busy" signal.
 *
 * Watches the live CSI feature stream and emits a CSI_CATEGORY_AMBIENT event
 * when the channel's aggregate activity jumps above the room's own recent
 * rolling baseline — the moment a nearby device starts a burst of traffic
 * (e.g. a camera pushing a clip). It is a "heads up, something is happening"
 * glow, nothing more.
 *
 * DELIBERATELY UNATTRIBUTED AND SYMMETRIC (spec/canary_free_signals_v0.md,
 * Invariants A/E/F): it reads only identity-free per-window aggregates from
 * csi_features_t (RSSI spread + the frame-rate-health counters). It never sees,
 * stores, or infers a MAC, an OUI, a vendor, or "which device." All RF sources
 * are treated identically.
 *
 * NOT PERSISTENT (Invariant C): the event is CSI_CATEGORY_AMBIENT, which the
 * chokepoint never commits to the witness chain, SD, or NVS — it only drives
 * the live UI (the /api/csi/stream feed and the activity ribbon) and is then
 * gone. All module state is a handful of RAM scalars.
 *
 * Privacy class: P0. Events carry only:
 *   - state_name   ("channel_active")
 *   - time_bucket  (10-minute coarsening, set by the chokepoint)
 *   - motion_score (0..100 burst intensity — the generic P0 scalar slot)
 * No identifying fields; the chokepoint enforces this via the manifest.
 */

#ifndef SECURACV_CSI_MODULE_WIFI_CHANNEL_ACTIVITY_H
#define SECURACV_CSI_MODULE_WIFI_CHANNEL_ACTIVITY_H

#include "csi_module.h"

#ifdef __cplusplus
extern "C" {
#endif

const csi_module_t* wifi_channel_activity_module(void);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_MODULE_WIFI_CHANNEL_ACTIVITY_H */
