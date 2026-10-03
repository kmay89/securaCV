/**
 * @file core_presence.h
 * @brief core.presence — derives `Empty / Subtle motion / Quiet / Active /
 *        Together` from the CSI feature stream and emits state-change events
 *        through the privacy chokepoint.
 *
 * This module wraps the existing rf_presence FSM hysteresis (when the host
 * provides it) and also runs a self-contained fallback that makes the
 * library usable as a stand-alone Arduino starter. It does not call
 * rf_presence directly; that integration lives in the canary-wap host.
 */

#ifndef SECURACV_CSI_MODULE_CORE_PRESENCE_H
#define SECURACV_CSI_MODULE_CORE_PRESENCE_H

#include <stdint.h>

#include "csi_module.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Returns the static singleton manifest. Pass to csi_module_register(). */
const csi_module_t* core_presence_module(void);

/* Optional dismiss handler — exposed for the unit test. */
void core_presence_handle_dismiss(uint32_t event_id);

/** core.presence's three thresholds, as 0..127 magnitudes. */
typedef struct {
  int32_t motion;
  int32_t active;
  int32_t breathing;
} core_presence_thresholds_t;

/** The thresholds core.presence runs for a threshold no row stores (sweep
 *  F166): the preset's baseline (0 sensitive 25 / 60 / 20, 2 quiet
 *  50 / 90 / 40, any other value balanced 35 / 75 / 30), moved by the
 *  sensitivity slider ((50 - sensitivity) * 40 / 100: +20 at 0, none at 50,
 *  -20 at 100), each clamped to 5..120. init() passes these as the
 *  defaults of the core.presence.*_threshold reads, so a stored row wins
 *  over them. A host that reports what the module runs (the canary-wap's
 *  calibration status) asks this rather than spelling the table again.
 *  Pure. */
core_presence_thresholds_t core_presence_baseline_thresholds(int32_t preset, int32_t sensitivity);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_MODULE_CORE_PRESENCE_H */
