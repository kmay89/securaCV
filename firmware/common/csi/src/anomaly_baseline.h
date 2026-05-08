/**
 * @file anomaly_baseline.h
 * @brief Watches the live CSI feature stream and emits CSI_CATEGORY_ANOMALY
 *        events when current intensity beats the room's recent rolling
 *        baseline by a configurable margin.
 *
 * The plan describes anomalies as "out-of-pattern by the room's own
 * historical baseline (no identity, no person model — just 'this room
 * is rarely active at 3 AM')." This is the simplest baseline-aware
 * implementation: a 60-window rolling average of motion / breathing
 * scores, with a spike-vs-average ratio gate plus a cooldown to keep
 * the user from being notified twice for the same event.
 *
 * Wall clock is NOT required — the module operates entirely in the
 * features stream's relative time. That makes it work even on devices
 * that haven't been time-synced (most fresh boots until NTP/GPS lock).
 *
 * Privacy class: P0. Events carry only:
 *   - state_name           (one of "unusual_motion" / "unusual_breathing")
 *   - confidence           (always "observed" for this detector)
 *   - time_bucket          (10-minute coarsening, set by the chokepoint)
 *   - motion_score / breathing_score (P0 scalar 0..100)
 * No identifying fields. The chokepoint enforces this via the manifest.
 */

#ifndef SECURACV_CSI_MODULE_ANOMALY_BASELINE_H
#define SECURACV_CSI_MODULE_ANOMALY_BASELINE_H

#include "csi_module.h"

#ifdef __cplusplus
extern "C" {
#endif

const csi_module_t* anomaly_baseline_module(void);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_MODULE_ANOMALY_BASELINE_H */
