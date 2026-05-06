/**
 * @file modules/core_activity_ribbon.h
 * @brief core.activity_ribbon — writes the 96-bucket × 15-min activity ring
 *        that backs the dashboard's 24-hour aurora ribbon.
 *
 * The ring is in-memory by default. The host application can persist it via
 * the optional `core_activity_ribbon_save` / `core_activity_ribbon_load`
 * hooks (e.g. into NVS), called from the firmware's main loop. Persistence
 * is host-controlled so this library compiles standalone.
 */

#ifndef SECURACV_CSI_MODULE_CORE_ACTIVITY_RIBBON_H
#define SECURACV_CSI_MODULE_CORE_ACTIVITY_RIBBON_H

#include "../csi_module.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CSI_RIBBON_BUCKETS 96   /* 24 h × 4 (15-minute slots) */

typedef struct {
  uint8_t  intensity[CSI_RIBBON_BUCKETS]; /* 0..255 */
  uint16_t version;                       /* monotonic, increments on write */
} csi_ribbon_snapshot_t;

const csi_module_t* core_activity_ribbon_module(void);

/** Read a copy of the current ribbon. Always succeeds. */
void core_activity_ribbon_snapshot(csi_ribbon_snapshot_t* out);

/** Restore from a previous snapshot (used at boot if the host has NVS). */
void core_activity_ribbon_load(const csi_ribbon_snapshot_t* in);

/** Returns the current bucket index (0..95) for the wall clock (host-supplied). */
uint8_t core_activity_ribbon_current_bucket(uint32_t now_minutes_of_day);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_MODULE_CORE_ACTIVITY_RIBBON_H */
