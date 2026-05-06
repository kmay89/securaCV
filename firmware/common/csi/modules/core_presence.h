/**
 * @file modules/core_presence.h
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

#include "../csi_module.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Returns the static singleton manifest. Pass to csi_module_register(). */
const csi_module_t* core_presence_module(void);

/* Optional dismiss handler — exposed for the unit test. */
void core_presence_handle_dismiss(uint32_t event_id);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_MODULE_CORE_PRESENCE_H */
