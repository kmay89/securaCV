/**
 * @file wap_diagnostics.h
 * @brief The body of the canary-wap's GET /api/diagnostics (sweep F149).
 *
 * The route (canary_wap.ino handle_diagnostics, token-gated,
 * FEATURE_SYS_MONITOR builds) answers a heap snapshot, the degradation
 * level, the SD card's write health, the uptime and the committed-event
 * egress's counters:
 *
 *   {"ok":true,
 *    "heap":{"free":N,"min_free":N,"largest_block":N,"total":N},
 *    "degradation":{"level":N,"level_name":"NONE"},
 *    "sd_health":{"total_writes":N,"write_errors":N,"usage_pct":N,
 *                 "space_warning":false,"space_critical":false},
 *    "uptime_sec":N,
 *    "csi_event_egress":{...}}
 *
 * `csi_event_egress` is csi_event_egress::stats_json() of the copy the loop
 * task's last pump published (csi_event_egress::read_stats(); the handler
 * runs on the httpd task, which may not read the pump's own state), in the
 * names the canary's MQTT health and this device's `egress` topic use, or
 * `null` before the first pump.
 *
 * The handler gathers the inputs from sys_monitor and builds the body here,
 * into a buffer of kJsonMax bytes, so the body and its worst case are
 * host-tested (tests_host/test_wap_diagnostics.cpp) rather than counted by
 * hand. Pure hosted C++ (no Arduino/ESP-IDF includes): any task.
 */

#ifndef CANARY_WAP_DIAGNOSTICS_H
#define CANARY_WAP_DIAGNOSTICS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "csi_event_egress.h"

namespace wap_diagnostics {

/* What the handler reads from sys_monitor, in its types. */
struct Inputs {
  uint32_t    heap_free;           /* g_sys_metrics.heap_free */
  uint32_t    heap_min_free;       /* g_sys_metrics.heap_min_free */
  uint32_t    heap_largest_block;  /* g_sys_metrics.heap_largest_block */
  uint32_t    heap_total;          /* g_sys_metrics.heap_total */
  uint8_t     degrade_level;       /* sys_monitor::DegradeLevel */
  const char* degrade_name;        /* sys_monitor::degrade_level_name() */
  uint32_t    sd_total_writes;     /* SDHealthStats::total_writes */
  uint32_t    sd_write_errors;     /* SDHealthStats::write_errors */
  uint8_t     sd_usage_pct;        /* SDHealthStats::usage_pct */
  bool        sd_space_warning;
  bool        sd_space_critical;
  uint32_t    uptime_sec;          /* g_sys_metrics.uptime_sec */
};

/* The longest name sys_monitor::degrade_level_name() returns ("EMERGENCY";
 * the host test reads sys_monitor.h to hold it), and the route's buffer: the
 * widest body (every number at its type's widest, that name, every egress
 * counter at 4294967295) is 649 bytes and the NUL. */
constexpr size_t kDegradeNameMax = 9;
constexpr size_t kJsonMax = 768;

/* The body into `out`. `egress` is the read_stats() copy, or nullptr before
 * the first pump (`null`). Returns its length, or 0 (and `out` holds no
 * partial body) when it does not fit `cap`. */
inline size_t build_json(const Inputs& in, const csi_event_egress::Stats* egress,
                         char* out, size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  char object[csi_event_egress::kStatsJsonMax];
  if (egress == nullptr ||
      csi_event_egress::stats_json(*egress, object, sizeof(object)) == 0) {
    strcpy(object, "null");
  }
  const int n = snprintf(out, cap,
    "{"
    "\"ok\":true,"
    "\"heap\":{"
      "\"free\":%lu,"
      "\"min_free\":%lu,"
      "\"largest_block\":%lu,"
      "\"total\":%lu"
    "},"
    "\"degradation\":{"
      "\"level\":%u,"
      "\"level_name\":\"%s\""
    "},"
    "\"sd_health\":{"
      "\"total_writes\":%lu,"
      "\"write_errors\":%lu,"
      "\"usage_pct\":%u,"
      "\"space_warning\":%s,"
      "\"space_critical\":%s"
    "},"
    "\"uptime_sec\":%lu,"
    "\"csi_event_egress\":%s"
    "}",
    (unsigned long)in.heap_free,
    (unsigned long)in.heap_min_free,
    (unsigned long)in.heap_largest_block,
    (unsigned long)in.heap_total,
    (unsigned)in.degrade_level,
    in.degrade_name != nullptr ? in.degrade_name : "UNKNOWN",
    (unsigned long)in.sd_total_writes,
    (unsigned long)in.sd_write_errors,
    (unsigned)in.sd_usage_pct,
    in.sd_space_warning  ? "true" : "false",
    in.sd_space_critical ? "true" : "false",
    (unsigned long)in.uptime_sec,
    object);
  if (n <= 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n;
}

}  // namespace wap_diagnostics

#endif  // CANARY_WAP_DIAGNOSTICS_H
