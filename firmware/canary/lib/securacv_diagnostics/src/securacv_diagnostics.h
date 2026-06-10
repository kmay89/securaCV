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
} selftest_result_t;

typedef struct {
  selftest_result_t tests[SELFTEST_COUNT];
  uint8_t           passed_count;
  uint8_t           total_count;
  uint8_t           health_score;
  uint32_t          run_at_ms;
  bool              has_run;
} selftest_report_t;

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
 * until next call. Returns the health score (0-100). */
uint8_t diag_run_selftest(void);

/* Get the cached self-test report. */
bool diag_get_selftest(selftest_report_t* out);

/* Get full diagnostic snapshot (heap + SD + selftest + system). */
bool diag_get_snapshot(diag_snapshot_t* out);

#ifdef __cplusplus
}
#endif

#endif  /* SECURACV_DIAGNOSTICS_H */
