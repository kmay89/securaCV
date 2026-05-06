/**
 * @file core_breathing.h
 * @brief core.breathing — locks onto the 0.15–0.45 Hz Goertzel band, emits
 *        `breathing_confirmed` and `breathing_lost` with confidence and an
 *        approximate BPM (P1, only at "confirmed" state).
 */

#ifndef SECURACV_CSI_MODULE_CORE_BREATHING_H
#define SECURACV_CSI_MODULE_CORE_BREATHING_H

#include "csi_module.h"

#ifdef __cplusplus
extern "C" {
#endif

const csi_module_t* core_breathing_module(void);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_MODULE_CORE_BREATHING_H */
