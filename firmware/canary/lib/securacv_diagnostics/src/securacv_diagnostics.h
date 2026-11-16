/*
 * SecuraCV Canary — System Diagnostics
 *
 * Three subsystems in one library:
 *
 *   1. HEAP MONITOR — tracks free heap, min heap, largest free block,
 *      PSRAM usage, and stack high-water marks. Emits degradation
 *      signals when memory pressure exceeds thresholds.
 *
 *   2. SD HEALTH — tracks write count, error rate, free space, and
 *      write latency. Warns at 10% free, critical at 2%.
 *
 *   3. SELF-TEST — runs on first boot and on-demand. Tests NVS, SD,
 *      WiFi, camera, ADC, crypto, touch, GPS, heap, and temp sensor.
 *      Produces a 0-100% health score.
 *
 * FEATURE DEGRADATION: when heap drops below configured thresholds,
 * the diagnostics module sets degradation flags that the power policy
 * engine (or main.cpp) reads to disable memory-hungry features like
 * camera, CSI, and MQTT. Features auto-restore when heap recovers
 * above threshold + hysteresis margin.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_DIAGNOSTICS_H
#define SECURACV_DIAGNOSTICS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ────────────────────────────────────────────────────────────────────────── */
/*  HEAP THRESHOLDS                                                          */
/* ────────────────────────────────────────────────────────────────────────── */

#define DIAG_HEAP_WARN_BYTES       30000
#define DIAG_HEAP_CRITICAL_BYTES   15000
#define DIAG_HEAP_EMERGENCY_BYTES  10000
#define DIAG_HEAP_HYSTERESIS       5000

/* ────────────────────────────────────────────────────────────────────────── */
/*  SD ENDURANCE                                                             */
/*                                                                           */
/*  SD cards are consumables with finite write endurance and no SMART        */
/*  reporting. Lifetime write counters are persisted in NVS (lazily, so the  */
/*  bookkeeping never wears NVS itself) and compared against a conservative  */
/*  endurance rating to produce a wear estimate and a one-way                */
/*  replace_recommended latch. The estimate is exactly that — an estimate.   */
/* ────────────────────────────────────────────────────────────────────────── */

/* Card endurance rating in TB written. Override in canary_config.h to match
 * the purchased card (32 TBW is a conservative default for the small
 * high-endurance cards typically used in Canary builds). */
#ifndef DIAG_SD_ENDURANCE_TBW
#define DIAG_SD_ENDURANCE_TBW      32
#endif

/* Wear percentage at which replacement is recommended. The latch is one-way:
 * wear only ever grows, so there is no flapping to damp. */
#define DIAG_SD_WEAR_REPLACE_PCT   80

/* Lazy NVS persistence of the lifetime counters: every N successful writes
 * or this many ms (whichever comes first), so a crash undercounts by at
 * most one batch — acceptable for an endurance estimate. */
#define DIAG_SD_PERSIST_EVERY_WRITES  64
#define DIAG_SD_PERSIST_INTERVAL_MS   600000

/* ────────────────────────────────────────────────────────────────────────── */
/*  DEGRADATION LEVEL                                                        */
/* ────────────────────────────────────────────────────────────────────────── */

typedef enum {
  DEGRADE_NONE      = 0,
  DEGRADE_WARN      = 1,
  DEGRADE_CRITICAL  = 2,
  DEGRADE_EMERGENCY = 3,
} degrade_level_t;

/* ────────────────────────────────────────────────────────────────────────── */
/*  HEAP SNAPSHOT                                                            */
/* ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  uint32_t free_heap;
  uint32_t min_heap;
  uint32_t largest_block;
  uint32_t psram_free;
  uint32_t psram_total;
  uint16_t stack_hwm_main;
  uint8_t  degrade_level;
  uint8_t  fragmentation_pct;
} diag_heap_t;

/* ────────────────────────────────────────────────────────────────────────── */
/*  SD HEALTH                                                                */
/* ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  uint32_t total_writes;
  uint32_t write_errors;
  uint32_t total_bytes_kb;
  uint32_t used_bytes_kb;
  uint8_t  usage_pct;
  bool     mounted;
  bool     space_warning;
  bool     space_critical;
  /* Endurance tracking (persisted in NVS, survives reboots). */
  uint32_t lifetime_writes;       /* total write ops across device lifetime */
  uint64_t lifetime_bytes;        /* total bytes written across lifetime    */
  uint16_t wear_pct_x10;          /* estimated wear in tenths of a percent  */
  bool     replace_recommended;   /* one-way latch at DIAG_SD_WEAR_REPLACE_PCT */
} diag_sd_t;

/* ────────────────────────────────────────────────────────────────────────── */
/*  SELF-TEST                                                                */
/* ────────────────────────────────────────────────────────────────────────── */

#define SELFTEST_COUNT  10

typedef struct {
  const char* name;
  bool        passed;
  uint16_t    duration_ms;
  /* A row the suite lists but does not check: it reads "not a probe" on
   * every surface, never passes or fails, and counts toward neither
   * passed_count nor health_score. The watchdog row is one until a real
   * probe exists (its old test returned true on both branches of its #if). */
  bool        not_a_probe;
} selftest_result_t;

typedef struct {
  selftest_result_t tests[SELFTEST_COUNT];
  uint8_t           passed_count;   /* probes that passed */
  uint8_t           total_count;    /* rows in tests[], probes or not */
  uint8_t           health_score;   /* passed_count over the probes, 0-100 */
  uint32_t          run_at_ms;
  bool              has_run;
} selftest_report_t;

/* How many of a report's rows are probes: its rows less those marked
 * not_a_probe. health_score is passed_count over this, never over every
 * row, so a listed row that checks nothing cannot raise it. */
static inline uint8_t selftest_probe_count(const selftest_report_t* r) {
  if (r == NULL) return 0;
  uint8_t n = 0;
  for (uint8_t i = 0; i < r->total_count && i < SELFTEST_COUNT; i++) {
    if (!r->tests[i].not_a_probe) n++;
  }
  return n;
}

/* One row of the suite: its name, and the probe that checks it, or NULL for
 * a row the suite lists and does not check (it reads "not a probe"). */
typedef bool (*selftest_probe_fn_t)(void);
typedef struct {
  const char*         name;
  selftest_probe_fn_t fn;
} selftest_def_t;

/* Run a suite into `out`: its first `n` rows (at most SELFTEST_COUNT), in
 * order. A row with a probe calls it once, timed with `now_ms`; a row with
 * none is marked not_a_probe and never called. passed_count counts the
 * probes that passed and health_score is passed_count over the probes (0
 * when there are none), so a listed row that checks nothing cannot raise
 * it. Returns the number of probes. diag_run_selftest() runs the device's
 * table through it, and the host test (test_canary_diagnostics.cpp) runs it
 * over stubs, so the scoring is tested rather than read. */
static inline uint8_t selftest_run(const selftest_def_t* defs, uint8_t n,
                                   uint32_t (*now_ms)(void), selftest_report_t* out) {
  if (out == NULL) return 0;
  if (defs == NULL) n = 0;
  if (n > SELFTEST_COUNT) n = SELFTEST_COUNT;
  out->passed_count = 0;
  out->total_count = n;
  out->run_at_ms = now_ms != NULL ? now_ms() : 0;
  out->has_run = true;
  uint8_t probes = 0;
  for (uint8_t i = 0; i < n; i++) {
    selftest_result_t* t = &out->tests[i];
    t->name = defs[i].name;
    t->not_a_probe = (defs[i].fn == NULL);
    t->passed = false;
    t->duration_ms = 0;
    if (t->not_a_probe) continue;
    probes++;
    const uint32_t start = now_ms != NULL ? now_ms() : 0;
    t->passed = defs[i].fn();
    t->duration_ms = (uint16_t)((now_ms != NULL ? now_ms() : 0) - start);
    if (t->passed) out->passed_count++;
  }
  out->health_score = probes == 0 ? 0 : (uint8_t)((uint32_t)out->passed_count * 100u / probes);
  return probes;
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  FULL DIAGNOSTIC SNAPSHOT                                                 */
/* ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  diag_heap_t       heap;
  diag_sd_t         sd;
  selftest_report_t selftest;
  uint32_t          uptime_sec;
  uint32_t          boot_count;
  uint8_t           reset_reason;
} diag_snapshot_t;

/* ────────────────────────────────────────────────────────────────────────── */
/*  C API                                                                    */
/* ────────────────────────────────────────────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

bool diag_init(void);

/* Call from main loop at ~1 Hz. Updates heap stats, checks degradation
 * thresholds, updates SD health counters. */
bool diag_process(void);

/* Get current degradation level. main.cpp uses this to gate features. */
degrade_level_t diag_get_degrade_level(void);

/* Get heap snapshot. */
bool diag_get_heap(diag_heap_t* out);

/* Get SD health snapshot. */
bool diag_get_sd(diag_sd_t* out);

/* Notify the diagnostics module of an SD write (success or error).
 * Called by the storage layer after each write operation. */
void diag_record_sd_write(bool success);

/* Byte-aware variant for wear estimation: records the write AND adds the
 * byte count to the NVS-persisted lifetime total. Use this wherever the
 * write size is known; diag_record_sd_write() is the 0-byte fallback for
 * metadata operations (deletes, renames). */
void diag_record_sd_write_bytes(size_t bytes, bool success);

/* Run the self-test suite. Blocks for ~2-5 seconds. Results cached
 * until next call. Returns the health score (0-100): the probes that
 * passed, over the probes (a row marked not_a_probe is neither). */
uint8_t diag_run_selftest(void);

/* Get the cached self-test report. */
bool diag_get_selftest(selftest_report_t* out);

/* Get full diagnostic snapshot (heap + SD + selftest + system). */
bool diag_get_snapshot(diag_snapshot_t* out);

#ifdef __cplusplus
}
#endif

#endif  /* SECURACV_DIAGNOSTICS_H */
