/*
 * SecuraCV Canary — the committed-event egress's counters for
 * GET /api/diagnostics (sweep F179)
 *
 * The egress lives in src/ (csi_event_egress.cpp) and the diagnostics route
 * in lib/securacv_network, and no library here includes a src/ header; this
 * directory is on every library's include path (platformio.ini). So the
 * route reaches the counters through this one declaration, defined in
 * src/csi_event_egress.cpp on every build.
 *
 * csi_event_egress_diagnostics_json() writes the counters as the loop
 * task's last csi_event_egress_pump() published them (a whole copy, at most
 * one pass old: csi_event_egress_read_stats()) as one JSON object, in the
 * names the MQTT health spells its `csi_event_egress` object with
 * (csi_event_egress_stats_json()), or `null` before the first pump and on a
 * build where no egress runs (FEATURE_CSI or FEATURE_HA_MQTT off). Returns
 * its length, or 0 (and `out` holds "") when it does not fit `cap`;
 * kCsiEventEgressDiagnosticsMax bytes always hold it. Any task: the route
 * runs on the HTTP server's.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_CSI_EVENT_EGRESS_DIAGNOSTICS_H
#define SECURACV_CSI_EVENT_EGRESS_DIAGNOSTICS_H

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A buffer this size holds every answer (src/csi_event_egress.cpp asserts
 * kCsiEventEgressStatsJsonMax fits it). */
enum { kCsiEventEgressDiagnosticsMax = 384 };

size_t csi_event_egress_diagnostics_json(char* out, size_t cap);

#ifdef __cplusplus
}
#endif

/* `null` into `out`: the answer before the first pump, or with no egress.
 * Returns 4, or 0 (and `out` holds "") when `cap` cannot hold it. */
static inline size_t csi_event_egress_diagnostics_null(char* out, size_t cap) {
  if (out == NULL || cap == 0) return 0;
  if (cap < 5) {
    out[0] = '\0';
    return 0;
  }
  memcpy(out, "null", 5);
  return 4;
}

#endif /* SECURACV_CSI_EVENT_EGRESS_DIAGNOSTICS_H */
