/**
 * @file meta_quiet_hours.h
 * @brief meta.quiet_hours — emits one held_summary event at the moment
 *        the configured quiet window closes.
 *
 * The chokepoint (csi_event.cpp) gates non-anomaly events while the
 * configured quiet window is active. When the window closes, it
 * synthesises one summary row through this module. No on-tick logic
 * lives here; this module is purely a manifest registration so the
 * summary emit passes the chokepoint's allow-list check.
 *
 * Registered in firmware/projects/canary-wap/arduino/canary_wap/
 * csi_integration.cpp's register_v1_modules().
 */

#ifndef SECURACV_CSI_MODULE_META_QUIET_HOURS_H
#define SECURACV_CSI_MODULE_META_QUIET_HOURS_H

#include "csi_module.h"

#ifdef __cplusplus
extern "C" {
#endif

const csi_module_t* meta_quiet_hours_module(void);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_MODULE_META_QUIET_HOURS_H */
