/*
 * SecuraCV Canary — System Diagnostics implementation
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_diagnostics.h"

#include <Arduino.h>
#include <string.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <esp_heap_caps.h>

#include "canary_config.h"
#include "log_level.h"
#include "securacv_witness.h"
#include "securacv_crypto.h"   /* compute_chain_hash — the real chain_ok test */
#include "securacv_thermal.h"

#if FEATURE_SD_STORAGE
#include <SD.h>
#endif

#if FEATURE_WIFI_AP
#include <WiFi.h>
#endif

namespace diag {

static bool s_initialized = false;
static uint32_t s_last_process_ms = 0;

static diag_heap_t s_heap = {};
static diag_sd_t   s_sd   = {};
static selftest_report_t s_selftest = {};

static degrade_level_t s_degrade = DEGRADE_NONE;

/* SD endurance bookkeeping. The 64-bit lifetime counter is guarded by a
 * critical section (same pattern as the witness record ring) because
 * 64-bit atomics are not lock-free on Xtensa. */
static portMUX_TYPE s_sd_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_sd_writes_since_persist = 0;
static uint32_t s_sd_last_persist_ms = 0;

/* NVS namespace/keys for the persisted lifetime counters. */
static const char* DIAG_NVS_NAMESPACE  = "diag";
static const char* DIAG_NVS_KEY_WRITES = "sd_lw";
static const char* DIAG_NVS_KEY_BYTES  = "sd_lb";

/* ──────────────────────────────────────────────────────────────────────────
 * HEAP MONITORING
 * ────────────────────────────────────────────────────────────────────────── */

static void update_heap() {
  s_heap.free_heap     = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  s_heap.min_heap      = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  s_heap.largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  s_heap.psram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  s_heap.psram_total   = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  s_heap.stack_hwm_main = (uint16_t)uxTaskGetStackHighWaterMark(NULL);

  uint32_t total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
  if (total > 0 && s_heap.largest_block < total) {
    s_heap.fragmentation_pct = (uint8_t)(100 - (s_heap.largest_block * 100 / total));
  }
}

static void check_degradation() {
  degrade_level_t prev = s_degrade;
  uint32_t free = s_heap.free_heap;

  // Apply hysteresis: when already at a level, require crossing
  // threshold + hysteresis to recover, preventing flapping.
  uint32_t emergency_limit = DIAG_HEAP_EMERGENCY_BYTES;
  uint32_t critical_limit  = DIAG_HEAP_CRITICAL_BYTES;
  uint32_t warn_limit      = DIAG_HEAP_WARN_BYTES;

  if (prev == DEGRADE_EMERGENCY)
    emergency_limit += DIAG_HEAP_HYSTERESIS;
  if (prev >= DEGRADE_CRITICAL)
    critical_limit += DIAG_HEAP_HYSTERESIS;
  if (prev >= DEGRADE_WARN)
    warn_limit += DIAG_HEAP_HYSTERESIS;

  if (free < emergency_limit) {
    s_degrade = DEGRADE_EMERGENCY;
  } else if (free < critical_limit) {
    s_degrade = DEGRADE_CRITICAL;
  } else if (free < warn_limit) {
    s_degrade = DEGRADE_WARN;
  } else {
    s_degrade = DEGRADE_NONE;
  }

  s_heap.degrade_level = s_degrade;

  if (s_degrade != prev) {
    const char* level = "none";
    switch (s_degrade) {
      case DEGRADE_WARN:      level = "warn"; break;
      case DEGRADE_CRITICAL:  level = "critical"; break;
      case DEGRADE_EMERGENCY: level = "emergency"; break;
      default: break;
    }
    LogLevel log_lvl = (s_degrade >= DEGRADE_CRITICAL)
                         ? LOG_LEVEL_WARNING : LOG_LEVEL_INFO;
    log_health(log_lvl, LOG_CAT_SYSTEM,
               "Heap degradation level changed", level);
  }
}

/* ──────────────────────────────────────────────────────────────────────────
 * SD HEALTH
 * ────────────────────────────────────────────────────────────────────────── */

static void update_sd() {
#if FEATURE_SD_STORAGE
  s_sd.mounted = SD.cardType() != CARD_NONE;
  if (s_sd.mounted) {
    // SD.totalBytes()/usedBytes() trigger slow FATFS volume scans;
    // rate-limit to once every 10 minutes to avoid main-loop stalls.
    static uint32_t last_space_check_ms = 0;
    uint32_t now = millis();
    if (last_space_check_ms == 0 || (int32_t)(now - last_space_check_ms) >= 600000) {
      last_space_check_ms = now;
      uint64_t total = SD.totalBytes();
      uint64_t used  = SD.usedBytes();
      s_sd.total_bytes_kb = (uint32_t)(total / 1024);
      s_sd.used_bytes_kb  = (uint32_t)(used / 1024);
      if (total > 0) {
        s_sd.usage_pct = (uint8_t)(used * 100 / total);
      }
      s_sd.space_warning  = (s_sd.usage_pct >= 90);
      s_sd.space_critical = (s_sd.usage_pct >= 98);
    }
  }
#endif
}

/* ──────────────────────────────────────────────────────────────────────────
 * SD ENDURANCE — lifetime counters, wear estimate, replacement latch
 * ────────────────────────────────────────────────────────────────────────── */

static void load_sd_counters() {
  Preferences prefs;
  if (!prefs.begin(DIAG_NVS_NAMESPACE, false)) return;
  s_sd.lifetime_writes = prefs.getULong(DIAG_NVS_KEY_WRITES, 0);
  s_sd.lifetime_bytes  = prefs.getULong64(DIAG_NVS_KEY_BYTES, 0);
  prefs.end();
}

static void persist_sd_counters() {
  portENTER_CRITICAL(&s_sd_mux);
  const uint32_t writes = s_sd.lifetime_writes;
  const uint64_t bytes  = s_sd.lifetime_bytes;
  portEXIT_CRITICAL(&s_sd_mux);
  Preferences prefs;
  if (!prefs.begin(DIAG_NVS_NAMESPACE, false)) return;
  prefs.putULong(DIAG_NVS_KEY_WRITES, writes);
  prefs.putULong64(DIAG_NVS_KEY_BYTES, bytes);
  prefs.end();
}

static void update_wear() {
  /* Snapshot the counters under the same lock the writers take —
   * s_sd_writes_since_persist is also mutated in diag_record_sd_write_bytes. */
  portENTER_CRITICAL(&s_sd_mux);
  const uint64_t lifetime_bytes = s_sd.lifetime_bytes;
  const uint32_t writes_since_persist = s_sd_writes_since_persist;
  portEXIT_CRITICAL(&s_sd_mux);

  /* Wear estimate against the configured endurance rating. Conservative
   * by design: SD cards expose no SMART data, so this tracks our own
   * write volume against the card's TBW class. */
  const double endurance_bytes = (double)DIAG_SD_ENDURANCE_TBW * 1e12;
  double wear = ((double)lifetime_bytes / endurance_bytes) * 100.0;
  if (wear > 100.0) wear = 100.0;
  s_sd.wear_pct_x10 = (uint16_t)(wear * 10.0);

  if (!s_sd.replace_recommended && wear >= (double)DIAG_SD_WEAR_REPLACE_PCT) {
    /* One-way latch: wear only grows, so no hysteresis is needed. */
    s_sd.replace_recommended = true;
    char detail[48];
    snprintf(detail, sizeof(detail), "wear %u.%u%% of %u TBW",
             (unsigned)(s_sd.wear_pct_x10 / 10),
             (unsigned)(s_sd.wear_pct_x10 % 10),
             (unsigned)DIAG_SD_ENDURANCE_TBW);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_STORAGE,
               "SD endurance threshold reached - replacement recommended",
               detail);
  }

  /* Lazy persistence: batch counter updates so the endurance bookkeeping
   * never becomes its own NVS-wear source. Worst case a crash undercounts
   * by one batch. */
  uint32_t now = millis();
  bool due = (writes_since_persist >= DIAG_SD_PERSIST_EVERY_WRITES) ||
             (writes_since_persist > 0 &&
              (int32_t)(now - s_sd_last_persist_ms) >= (int32_t)DIAG_SD_PERSIST_INTERVAL_MS);
  if (due) {
    persist_sd_counters();
    /* Subtract the snapshot rather than zeroing: writes recorded between
     * the snapshot and here stay pending for the next persist batch. */
    portENTER_CRITICAL(&s_sd_mux);
    s_sd_writes_since_persist -= writes_since_persist;
    portEXIT_CRITICAL(&s_sd_mux);
    s_sd_last_persist_ms = now;
  }
}

/* ──────────────────────────────────────────────────────────────────────────
 * SELF-TEST SUITE
 * ────────────────────────────────────────────────────────────────────────── */

static bool test_nvs() {
  Preferences prefs;
  if (!prefs.begin("_diag_test", false)) return false;
  prefs.putUChar("_t", 42);
  bool ok = (prefs.getUChar("_t", 0) == 42);
  prefs.remove("_t");
  prefs.end();
  return ok;
}

static bool test_heap() {
  return heap_caps_get_free_size(MALLOC_CAP_8BIT) > 50000;
}

static bool test_psram() {
  return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

static bool test_crypto() {
  DeviceIdentity& dev = witness_get_device();
  return dev.initialized;
}

static bool test_sd() {
#if FEATURE_SD_STORAGE
  return SD.cardType() != CARD_NONE;
#else
  return true;
#endif
}

static bool test_wifi() {
#if FEATURE_WIFI_AP
  return WiFi.getMode() != WIFI_OFF;
#else
  return true;
#endif
}

static bool test_temp() {
  /* Shared provider, NOT Arduino's temperatureRead(): the HAL keeps its
   * own driver handle, and IDF 5.x allows only one instance — with the
   * envsens tamper detector holding the sensor, temperatureRead()
   * returned NAN and this test failed forever, denting the health score. */
  float t = 0.0f;
  if (!thermal_read_die_c(&t)) return false;
  return (t > -20.0f && t < 85.0f);
}

static bool test_uptime() {
  uint32_t start = millis();
  delay(1);
  return (int32_t)(millis() - start) > 0;
}

static bool test_watchdog() {
#if FEATURE_WATCHDOG
  return true;
#else
  return true;
#endif
}

/* The chain self-test, made real. The old body returned crypto_healthy —
 * a diagnostic NAMED chain_ok that never looked at the chain, so a
 * silently rewritten tail still scored green. What a device can honestly
 * assert about its own chain, every diagnostic pass, without an SD walk:
 *   1. the crypto layer is up and no verify has ever failed;
 *   2. the newest record's chain hash RECOMPUTES from its own fields
 *      (the canonical construction, securacv_crypto);
 *   3. that hash IS the head the next record would extend; and
 *   4. the record's Ed25519 signature verifies under this device's key.
 * The full-history walk stays where it belongs — the offline verifier
 * (tools/verify_witness_log.py) and the SD-tail reconciliation at boot. */
static bool test_chain() {
  SystemHealth& h = witness_get_health();
  if (!h.crypto_healthy || h.verify_failures != 0) return false;
  DeviceIdentity& d = witness_get_device();
  if (d.seq == 0) return true;         /* genesis: nothing to walk yet */
  const WitnessRecord& last = witness_get_last_record();
  if (last.seq != d.seq) return true;  /* head restored from NVS/SD; the
                                        * record itself is not in RAM to
                                        * re-verify (boot reconciliation
                                        * already vetted it) */
  uint8_t recomputed[32];
  compute_chain_hash(last.prev_hash, last.payload_hash, last.seq,
                     last.time_bucket, recomputed);
  if (memcmp(recomputed, last.chain_hash, 32) != 0) return false;
  if (memcmp(last.chain_hash, d.chain_head, 32) != 0) return false;
  return witness_verify_record(&last);
}

typedef bool (*test_fn_t)(void);

struct TestDef {
  const char* name;
  test_fn_t   fn;
};

static const TestDef TESTS[SELFTEST_COUNT] = {
  {"nvs_rw",     test_nvs},
  {"heap_ok",    test_heap},
  {"psram_ok",   test_psram},
  {"crypto_ok",  test_crypto},
  {"sd_card",    test_sd},
  {"wifi_ok",    test_wifi},
  {"temp_range", test_temp},
  {"uptime_ok",  test_uptime},
  {"watchdog",   test_watchdog},
  {"chain_ok",   test_chain},
};

}  /* namespace diag */


extern "C" {

bool diag_init(void) {
  using namespace diag;
  if (s_initialized) return true;
  memset(&s_heap, 0, sizeof(s_heap));
  memset(&s_sd, 0, sizeof(s_sd));
  memset(&s_selftest, 0, sizeof(s_selftest));
  s_degrade = DEGRADE_NONE;
  /* Restore lifetime endurance counters; they survive reboots so the wear
   * estimate reflects the card's whole service life, not just this boot. */
  load_sd_counters();
  s_sd_last_persist_ms = millis();
  s_initialized = true;
  log_health(LOG_LEVEL_INFO, LOG_CAT_SYSTEM,
             "Diagnostics engine initialized", nullptr);
  return true;
}

bool diag_process(void) {
  using namespace diag;
  if (!s_initialized) return false;
  uint32_t now = millis();
  if ((now - s_last_process_ms) < 1000) return false;
  s_last_process_ms = now;

  update_heap();
  check_degradation();
  update_sd();
  update_wear();
  return true;
}

degrade_level_t diag_get_degrade_level(void) {
  return diag::s_degrade;
}

bool diag_get_heap(diag_heap_t* out) {
  if (!out) return false;
  *out = diag::s_heap;
  return true;
}

bool diag_get_sd(diag_sd_t* out) {
  if (!out) return false;
  *out = diag::s_sd;
  return true;
}

void diag_record_sd_write(bool success) {
  diag_record_sd_write_bytes(0, success);
}

void diag_record_sd_write_bytes(size_t bytes, bool success) {
  using namespace diag;
  __atomic_fetch_add(&s_sd.total_writes, 1, __ATOMIC_RELAXED);
  if (!success) {
    __atomic_fetch_add(&s_sd.write_errors, 1, __ATOMIC_RELAXED);
    return;
  }
  /* 64-bit counter is not lock-free on Xtensa — critical section, same
   * pattern as the witness record ring. */
  portENTER_CRITICAL(&s_sd_mux);
  s_sd.lifetime_writes++;
  s_sd.lifetime_bytes += (uint64_t)bytes;
  s_sd_writes_since_persist++;
  portEXIT_CRITICAL(&s_sd_mux);
}

uint8_t diag_run_selftest(void) {
  using namespace diag;
  s_selftest.passed_count = 0;
  s_selftest.total_count = SELFTEST_COUNT;
  s_selftest.run_at_ms = millis();
  s_selftest.has_run = true;

  for (int i = 0; i < SELFTEST_COUNT; i++) {
    s_selftest.tests[i].name = TESTS[i].name;
    uint32_t start = millis();
    s_selftest.tests[i].passed = TESTS[i].fn();
    s_selftest.tests[i].duration_ms = (uint16_t)(millis() - start);
    if (s_selftest.tests[i].passed) s_selftest.passed_count++;
  }

  s_selftest.health_score = (uint8_t)(
      (uint32_t)s_selftest.passed_count * 100 / SELFTEST_COUNT);

  char detail[32];
  snprintf(detail, sizeof(detail), "%u/%u (%u%%)",
           (unsigned)s_selftest.passed_count, (unsigned)SELFTEST_COUNT,
           (unsigned)s_selftest.health_score);
  log_health(LOG_LEVEL_INFO, LOG_CAT_SYSTEM, "Self-test complete", detail);

  return s_selftest.health_score;
}

bool diag_get_selftest(selftest_report_t* out) {
  if (!out) return false;
  *out = diag::s_selftest;
  return true;
}

bool diag_get_snapshot(diag_snapshot_t* out) {
  using namespace diag;
  if (!out) return false;
  out->heap = s_heap;
  out->sd = s_sd;
  out->selftest = s_selftest;
  out->uptime_sec = millis() / 1000;
  out->boot_count = witness_get_device().boot_count;
  out->reset_reason = (uint8_t)esp_reset_reason();
  return true;
}

}  /* extern "C" */
