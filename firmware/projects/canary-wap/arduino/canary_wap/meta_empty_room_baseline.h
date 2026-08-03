/**
 * @file meta_empty_room_baseline.h
 * @brief meta.empty_room_baseline — captures a 10-minute reference
 *        feature-vector mean for an empty room and exposes it for
 *        runtime subtraction.
 *
 * The design doc's "moves the couch and now everything trips"
 * failure mode is solved by recording, when the room is reliably
 * empty (pairing-complete + every 24 h during a quiet-hours window),
 * a per-feature-index mean of the csi_features_t windows. Downstream
 * sensing modules can subtract this baseline so a heavy bookcase or
 * a static appliance doesn't keep being read as "weak motion".
 *
 * v1 (this PR) scope:
 *   • Calibrate the LOCAL features only. Per-link (peer) baselines
 *     come later when PR 2h's OperaPeer table lands.
 *   • In-RAM storage. NVS persistence is integration-layer work; this
 *     module exposes a getter and the host saves it.
 *   • Mean only. Std-dev (design doc mentions "mean+stddev") arrives
 *     in PR 4d alongside an outlier-rejection pass.
 *   • Calibration is triggered by an explicit API call. The
 *     meta.quiet_hours module fires it on its "window opened" hook
 *     in the integration layer; pairing fires it on PAIRED.
 *
 * Privacy class: P0. Events carry:
 *   • state_name = "calibrated" | "canceled" | "failed"
 *   • bundled_count = number of windows accumulated
 * No feature bytes leave the device through the event chokepoint —
 * callers read the int8[32] mean via the getter.
 */

#ifndef SECURACV_CSI_MODULE_META_EMPTY_ROOM_BASELINE_H
#define SECURACV_CSI_MODULE_META_EMPTY_ROOM_BASELINE_H

#include "csi_module.h"
#include "csi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default calibration duration. Matches the design doc's 10-min
 *  empty-room sample. Each window is CSI_WINDOW_MS (1 s), so the
 *  default = 600 windows. */
#define META_EMPTY_ROOM_DEFAULT_DURATION_MS (10u * 60u * 1000u)

/** Minimum windows required to accept a calibration. A device that
 *  produces fewer than this in the duration (e.g. CSI watchdog
 *  thrashing) emits "failed" instead of "calibrated". */
#define META_EMPTY_ROOM_MIN_WINDOWS         300u

/** Returns the static singleton manifest. Pass to csi_module_register(). */
const csi_module_t* meta_empty_room_baseline_module(void);

/**
 * Start a fresh calibration run.
 *
 * @param duration_ms  Total run length. Pass 0 to use the default
 *                     META_EMPTY_ROOM_DEFAULT_DURATION_MS.
 * @return true on success. Returns false if a calibration is already
 *         in progress (cancel the old one first).
 */
bool meta_empty_room_baseline_start(uint32_t duration_ms);

/**
 * Cancel an in-progress calibration. Wipes accumulator state and
 * emits a "canceled" event. Idempotent.
 */
void meta_empty_room_baseline_cancel(void);

/** True iff a calibration run is currently accumulating windows. */
bool meta_empty_room_baseline_is_calibrating(void);

/**
 * Retrieve the most recently completed baseline. Fills `out_mean` with
 * the per-feature-index int8 mean computed from the calibration sample
 * and `*out_window_count` with the number of windows that contributed.
 *
 * Returns true if a calibration has ever successfully completed since
 * the last context wipe; false otherwise (and `out_mean` is zeroed).
 *
 * NOT a snapshot — the mean lives in module state and is overwritten
 * by the next successful calibration. Callers wanting to persist the
 * baseline across reboots should copy it into NVS from the
 * "calibrated" event handler.
 */
bool meta_empty_room_baseline_get(int8_t out_mean[CSI_FEATURE_DIM],
                                  uint16_t* out_window_count);

/* ────────────────────────────────────────────────────────────────────────
 * TEST HOOKS (host build only)
 * ──────────────────────────────────────────────────────────────────────── */

#ifdef CSI_TEST_HOST_BUILD
/** Wipe ALL state (calibration accumulator + stored baseline). */
void meta_empty_room_baseline_test_reset(void);

/** Inject a virtual clock for duration tests. */
void meta_empty_room_baseline_test_set_now_ms(uint32_t now_ms);

/** Inspect the accumulator: how many windows have been added so far. */
uint32_t meta_empty_room_baseline_test_in_progress_count(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_MODULE_META_EMPTY_ROOM_BASELINE_H */
