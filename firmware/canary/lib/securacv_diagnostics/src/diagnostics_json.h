/*
 * SecuraCV Canary — the body of GET /api/diagnostics (sweep F179)
 *
 * The route (securacv_network.cpp handle_diagnostics, token-gated,
 * FEATURE_DIAGNOSTICS builds) answers the diagnostics snapshot
 * (diag_get_snapshot()), the firmware version and the committed-event
 * egress's counters:
 *
 *   {"heap":{"free":N,"min":N,"largest_block":N,"psram_free":N,
 *            "psram_total":N,"stack_hwm":N,"fragmentation_pct":N,
 *            "degrade_level":"none"},
 *    "sd":{"mounted":true,"usage_pct":N,"total_writes":N,"write_errors":N,
 *          "space_warning":false,"space_critical":false},
 *    "selftest":{"has_run":true,"health_score":N,"passed":N,"total":N,
 *                "tests":[{"name":"nvs_rw","passed":true,"ms":N},...]},
 *    "system":{"uptime_sec":N,"boot_count":N,"reset_reason":N,
 *              "firmware":"2.4.15"},
 *    "csi_event_egress":{...}}
 *
 * Every key before `csi_event_egress` is the route's as it stood before
 * sweep F179, byte for byte. `csi_event_egress` is the object
 * csi_event_egress_diagnostics_json() (include/csi_event_egress_diagnostics.h)
 * writes: the counters the loop task's last egress pump published, in the
 * names the MQTT health uses, or `null` before the first pump and on a
 * build with no egress.
 *
 * The handler gathers the inputs and builds the body here, into a buffer of
 * kJsonMax bytes, so the body and its worst case are host-tested
 * (firmware/tests_host/test_canary_diagnostics.cpp) rather than counted by
 * hand: the route used to append with `pos += snprintf(...)`, which, past
 * the end of the buffer, would hand the next call a size that wrapped, and
 * dropped self-test rows to stay inside it. Pure hosted C++ (no Arduino or
 * ESP-IDF includes): any task.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_DIAGNOSTICS_JSON_H
#define SECURACV_DIAGNOSTICS_JSON_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "securacv_diagnostics.h"

namespace diagnostics_json {

/* The route's buffer. The widest body (every number at its type's widest,
 * all SELFTEST_COUNT rows under the longest self-test name, a 32-character
 * firmware version, every egress counter at 4294967295) is 1352 bytes and
 * the NUL; the host test holds it, with the self-test names read from
 * securacv_diagnostics.cpp. */
constexpr size_t kJsonMax = 2048;

/* The degradation level as the route has always spelled it. */
inline const char* degrade_name(uint8_t level) {
  switch (level) {
    case DEGRADE_WARN:      return "warn";
    case DEGRADE_CRITICAL:  return "critical";
    case DEGRADE_EMERGENCY: return "emergency";
    default:                return "none";
  }
}

/* Appends to a buffer and remembers whether everything fit. */
class Writer {
 public:
  Writer(char* out, size_t cap) : out_(out), cap_(cap), pos_(0), ok_(out != nullptr && cap > 0) {
    if (ok_) out_[0] = '\0';
  }

#if defined(__GNUC__)
  __attribute__((format(printf, 2, 3)))
#endif
  void add(const char* fmt, ...) {
    if (!ok_) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out_ + pos_, cap_ - pos_, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap_ - pos_) {
      ok_ = false;
      return;
    }
    pos_ += (size_t)n;
  }

  /* The length written, or 0 (and the buffer holds "") when anything did
   * not fit. */
  size_t finish() {
    if (ok_) return pos_;
    if (out_ != nullptr && cap_ > 0) out_[0] = '\0';
    return 0;
  }

 private:
  char*  out_;
  size_t cap_;
  size_t pos_;
  bool   ok_;
};

/* The body into `out`. `firmware` is FIRMWARE_VERSION; `egress` is the
 * object csi_event_egress_diagnostics_json() wrote, inserted as it is
 * (nullptr or "" spells `null`). Returns its length, or 0 (and `out` holds
 * no partial body) when it does not fit `cap`. */
inline size_t build(const diag_snapshot_t& snap, const char* firmware, const char* egress,
                    char* out, size_t cap) {
  Writer w(out, cap);
  const diag_heap_t& h = snap.heap;
  w.add("{\"heap\":{\"free\":%lu,\"min\":%lu,\"largest_block\":%lu,"
        "\"psram_free\":%lu,\"psram_total\":%lu,"
        "\"stack_hwm\":%u,\"fragmentation_pct\":%u,"
        "\"degrade_level\":\"%s\"},",
        (unsigned long)h.free_heap, (unsigned long)h.min_heap,
        (unsigned long)h.largest_block, (unsigned long)h.psram_free,
        (unsigned long)h.psram_total, (unsigned)h.stack_hwm_main,
        (unsigned)h.fragmentation_pct, degrade_name(h.degrade_level));

  const diag_sd_t& sd = snap.sd;
  w.add("\"sd\":{\"mounted\":%s,\"usage_pct\":%u,"
        "\"total_writes\":%lu,\"write_errors\":%lu,"
        "\"space_warning\":%s,\"space_critical\":%s},",
        sd.mounted ? "true" : "false", (unsigned)sd.usage_pct,
        (unsigned long)sd.total_writes, (unsigned long)sd.write_errors,
        sd.space_warning ? "true" : "false", sd.space_critical ? "true" : "false");

  const selftest_report_t& st = snap.selftest;
  w.add("\"selftest\":{\"has_run\":%s,\"health_score\":%u,"
        "\"passed\":%u,\"total\":%u,\"tests\":[",
        st.has_run ? "true" : "false", (unsigned)st.health_score,
        (unsigned)st.passed_count, (unsigned)st.total_count);
  for (uint8_t i = 0; i < st.total_count && i < SELFTEST_COUNT; i++) {
    w.add("%s{\"name\":\"%s\",\"passed\":%s,\"ms\":%u}", i > 0 ? "," : "",
          st.tests[i].name != nullptr ? st.tests[i].name : "unknown",
          st.tests[i].passed ? "true" : "false", (unsigned)st.tests[i].duration_ms);
  }
  w.add("]},");

  w.add("\"system\":{\"uptime_sec\":%lu,\"boot_count\":%lu,"
        "\"reset_reason\":%u,\"firmware\":\"%s\"},",
        (unsigned long)snap.uptime_sec, (unsigned long)snap.boot_count,
        (unsigned)snap.reset_reason, firmware != nullptr ? firmware : "");

  w.add("\"csi_event_egress\":%s}",
        (egress != nullptr && egress[0] != '\0') ? egress : "null");
  return w.finish();
}

}  // namespace diagnostics_json

#endif  // SECURACV_DIAGNOSTICS_JSON_H
