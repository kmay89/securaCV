/**
 * @file modules/meta_daily_summary.h
 * @brief meta.daily_summary — at the wall-clock day boundary, walks the
 *        in-memory event ring and emits one `daily_summary` event with
 *        coarsened counts (active periods, anomaly count, quiet hours).
 *
 * Pure consumer module: it does not touch CSI features directly. Instead it
 * runs on tick() and self-triggers when the host's reported wall-clock day
 * has rolled over. The host supplies wall-clock via
 * `meta_daily_summary_set_clock(uint16_t minutes_of_day)`.
 */

#ifndef SECURACV_CSI_MODULE_META_DAILY_SUMMARY_H
#define SECURACV_CSI_MODULE_META_DAILY_SUMMARY_H

#include "../csi_module.h"

#ifdef __cplusplus
extern "C" {
#endif

const csi_module_t* meta_daily_summary_module(void);

/** Host calls this whenever it has fresh wall-clock data. */
void meta_daily_summary_set_clock(uint16_t minutes_of_day);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_MODULE_META_DAILY_SUMMARY_H */
