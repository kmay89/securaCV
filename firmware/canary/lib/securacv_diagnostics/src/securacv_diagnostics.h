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
