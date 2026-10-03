/**
 * @file meta_daily_summary.h
 * @brief meta.daily_summary — once a household day, in its last five
 *        minutes (23:55..23:59), walks the newest rows of the in-memory
 *        event ring and commits one `daily_summary` row.
 *
 * Pure consumer module: it does not touch CSI features directly. It runs on
 * tick() (so only while CSI windows tick the modules) and needs the host's
 * household minute of day, from `meta_daily_summary_set_clock(minutes)`.
 * Both trees feed it from the loop pass that keeps the chokepoint's clock
 * offset, and only once the wall clock is synced (sweep F121): with no clock
 * fed, tick() commits nothing.
 *
 * The row: category event, P0, `bundled_count` = the rows walked (at most
 * 64), `time_bucket` coarsened by the chokepoint, and `note`
 * "a<A> q<Q> x<X>": A rows whose state is "active", Q rows whose state is
 * "empty", X anomaly rows, among the rows walked. Counts of committed ring
 * rows, not minutes or stretches. No published surface carries `note` today
 * (the MQTT events body, the card line and /api/events/today leave it out).
 *
 * One row per day: the latch that stops a second row lets go only at a
 * minute before 00:30, and a first clock that already reads 23:55..23:59 (a
 * boot or first sync inside the window) commits none that day, since that
 * boot cannot know whether the one before it committed today's.
 */

#ifndef SECURACV_CSI_MODULE_META_DAILY_SUMMARY_H
#define SECURACV_CSI_MODULE_META_DAILY_SUMMARY_H

#include "csi_module.h"

#ifdef __cplusplus
extern "C" {
#endif

const csi_module_t* meta_daily_summary_module(void);

/** Host calls this with the household minute of day (0..1439) on each loop
 *  pass with a synced wall clock; a value of 1440 or more is ignored. */
void meta_daily_summary_set_clock(uint16_t minutes_of_day);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_MODULE_META_DAILY_SUMMARY_H */
