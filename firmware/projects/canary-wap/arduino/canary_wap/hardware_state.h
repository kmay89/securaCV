/*
 * SecuraCV Canary - Hardware State Manager
 *
 * Provides resilient hardware management for optional peripherals:
 * - GPS: Auto-detection, state machine, non-blocking reads
 * - SD Card: Hot-plug detection, safe operations, timeouts
 * - Safe Mode: Anti-reboot-loop protection
 *
 * SECURITY PRINCIPLE: A witness device that can be trivially disabled
 * by removing an SD card is not a witness device. The core witness
 * functions must continue operating even if every optional peripheral
 * is gone.
 *
 * Copyright (c) 2024-2026 SecuraCV Project Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SECURACV_HARDWARE_STATE_H
#define SECURACV_HARDWARE_STATE_H

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include <esp_attr.h>     // RTC_NOINIT_ATTR (crash-stage breadcrumb)
#include <esp_system.h>   // esp_reset_reason()
#include <esp_task_wdt.h> // fed while waiting on the SD mount worker

#include "sd_mount_logic.h"  // pure, host-tested mount decisions

// ============================================================================
// CONFIGURATION
// ============================================================================

namespace hw_config {
  // GPS detection timeouts
  static const uint32_t GPS_DETECT_TIMEOUT_MS   = 1000;   // Max time to wait for GPS on boot
  static const uint32_t GPS_DATA_TIMEOUT_MS     = 30000;  // Mark GPS absent if no data for 30s
  static const uint32_t GPS_FIX_TIMEOUT_MS      = 5000;   // Mark fix lost after 5s no update

  // SD card timeouts and intervals
  // SD_MOUNT_TIMEOUT_MS is the CALLER'S polling budget for the boot-time
  // mount wait, not a bound on the driver: the blocking SD.begin() runs on
  // the dedicated mount worker task and keeps going if the budget expires —
  // a late result is adopted by a later loop() pass instead of being lost.
  static const uint32_t SD_MOUNT_TIMEOUT_MS     = 4000;   // Boot wait budget for the mount worker
  static const uint32_t SD_RECHECK_INTERVAL_MS  = 30000;  // Recheck for SD every 30s when absent
  static const uint32_t SD_OP_TIMEOUT_MS        = 1000;   // Timeout for individual SD operations
  static const uint8_t  SD_MAX_RETRIES          = 2;      // Max retries before marking SD failed

  // Safe mode protection
  static const uint32_t SAFE_MODE_WINDOW_MS     = 60000;  // 60 second window
  static const uint8_t  SAFE_MODE_REBOOT_LIMIT  = 3;      // 3 reboots in window triggers safe mode
  static const uint32_t SAFE_MODE_RECOVERY_MS   = 60000;  // 60s stable in safe mode before a recovery reboot
  static const uint8_t  SAFE_MODE_MAX_RECOVERIES = 3;     // Bounded auto-reboot attempts before staying in safe mode

  // NVS keys for boot tracking
  static const char* NVS_BOOT_TIMES      = "boot_times";   // Array of recent boot timestamps
  static const char* NVS_SAFE_MODE       = "safe_mode";    // Safe mode flag
  static const char* NVS_SAFE_MODE_TIME  = "safe_time";    // When safe mode was entered
  static const char* NVS_RECOVERY_COUNT  = "recov_count";  // Auto-reboot recovery attempts used
}

// ============================================================================
// GPS STATE MACHINE
// ============================================================================

enum GpsState : uint8_t {
  GPS_ABSENT       = 0,   // No GPS module detected (or removed)
  GPS_DETECTED     = 1,   // GPS module present, no fix yet
  GPS_HAS_FIX      = 2,   // GPS has valid position fix
  GPS_LOST_FIX     = 3    // Had fix, now lost (still receiving data)
};

inline const char* gps_state_name(GpsState s) {
  switch (s) {
    case GPS_ABSENT:    return "ABSENT";
    case GPS_DETECTED:  return "DETECTED";
    case GPS_HAS_FIX:   return "HAS_FIX";
    case GPS_LOST_FIX:  return "LOST_FIX";
    default:            return "UNKNOWN";
  }
}

// ============================================================================
// SD CARD STATE MACHINE
// ============================================================================

enum SdState : uint8_t {
  SD_ABSENT        = 0,   // No SD card (or removed)
  SD_MOUNTED       = 1,   // SD card mounted and operational
  SD_ERROR         = 2    // SD card present but erroring (bad card, filesystem issue)
};

inline const char* sd_state_name(SdState s) {
  switch (s) {
    case SD_ABSENT:   return "ABSENT";
    case SD_MOUNTED:  return "MOUNTED";
    case SD_ERROR:    return "ERROR";
    default:          return "UNKNOWN";
  }
}

// ============================================================================
// HARDWARE STATE STRUCTURE
// ============================================================================

struct HardwareState {
  // === GPS State ===
  bool     gps_available;         // Is GPS currently providing data?
  GpsState gps_state;             // Current GPS state machine state
  uint32_t gps_last_data_ms;      // Last time we received any GPS data
  uint32_t gps_last_fix_ms;       // Last time we had a valid fix
  uint32_t gps_detect_time_ms;    // When GPS was first detected this session
  uint32_t gps_sentences_total;   // Total NMEA sentences received
  uint32_t gps_checksum_errors;   // Checksum failures (indicates noise/disconnect)
  bool     gps_ever_detected;     // Has GPS ever been detected this session?

  // === SD Card State ===
  bool     sd_available;          // Is SD card currently usable?
  SdState  sd_state;              // Current SD state machine state
  uint32_t sd_last_success_ms;    // Last successful SD operation
  uint32_t sd_last_check_ms;      // Last time we checked for SD presence
  uint32_t sd_mount_time_ms;      // When SD was mounted
  uint32_t sd_write_count;        // Successful writes this session
  uint32_t sd_error_count;        // Errors this session
  uint8_t  sd_consecutive_errors; // Consecutive errors (for backoff)
  uint64_t sd_total_bytes;        // Cached card size (to avoid blocking reads)
  uint64_t sd_free_bytes;         // Cached free space (updated periodically)

  // === Camera State ===
  bool     camera_available;      // Is camera initialized?
  bool     camera_ever_init;      // Did camera ever initialize successfully?
  bool     camera_standby;        // Parked by the power manager (healthy, wakes on use)

  // === Safe Mode ===
  bool     safe_mode;             // In safe mode (optional peripherals disabled)
  uint8_t  rapid_boot_count;      // Consecutive crash reboots (panic/watchdog/brownout)
  uint32_t safe_mode_entered_ms;  // When safe mode was entered
  uint32_t last_stable_ms;        // Last known stable operation time
  esp_reset_reason_t last_reset_reason;  // Why the chip last reset
  bool     last_reset_was_crash;  // Did the last reset look like a crash?
};

// Global hardware state instance
extern HardwareState g_hw;

// ============================================================================
// API FUNCTIONS - GPS
// ============================================================================

/**
 * Probe for GPS module presence (non-blocking with timeout).
 * Called once at boot. Sets gps_available and gps_state.
 * Returns true if GPS detected within timeout.
 */
bool gps_probe(HardwareSerial& serial, uint32_t timeout_ms = hw_config::GPS_DETECT_TIMEOUT_MS);

/**
 * Update GPS state machine based on data reception.
 * Call this after processing GPS data in loop().
 *
 * @param received_data   true if any GPS data was received this cycle
 * @param has_valid_fix   true if GPS reports a valid position fix
 */
void gps_update_state(bool received_data, bool has_valid_fix);

/**
 * Check if GPS is currently providing data.
 * Returns false if GPS absent or timed out.
 */
inline bool gps_is_available() { return g_hw.gps_available; }

/**
 * Get current GPS state.
 */
inline GpsState gps_get_state() { return g_hw.gps_state; }

// ============================================================================
// API FUNCTIONS - SD CARD
// ============================================================================

/**
 * Attempt to mount the SD card, bounded and watchdog-safe.
 * The blocking SD.begin() runs on a dedicated worker task; this call waits
 * up to SD_MOUNT_TIMEOUT_MS, feeding the task watchdog while it polls. If
 * the worker is still probing when the budget expires, this returns false
 * (state SD_ABSENT) and the eventual result is adopted by a later
 * sd_periodic_check() pass — a slow or wedged card can delay mounting but
 * can never crash-loop the device.
 *
 * @param spi      SPI bus instance
 * @param cs_pin   Chip select pin
 * @param speed    SPI speed (Hz), will fall back to slower if needed
 * @return true if mounted within the wait budget
 */
bool sd_mount_safe(SPIClass& spi, int cs_pin, uint32_t speed);

/**
 * True while a background mount attempt is running on the mount worker task.
 * While in flight: no new mount requests, no SD teardown (SD.end()), and —
 * on boards where the user LED shares the SD chip-select pin (XIAO ESP32-S3:
 * LED_BUILTIN == GPIO21 == SD_CS) — no LED writes, which would glitch CS
 * mid-transaction and corrupt the mount.
 */
bool sd_mount_in_flight();

/**
 * Check if SD card is still present and operational.
 * Quick non-blocking check, updates state if card removed.
 */
bool sd_verify_present();

/**
 * Perform periodic SD card maintenance.
 * - Re-attempts mount if card absent
 * - Updates cached space info
 * - Clears error count after sustained success
 * Call from loop() periodically.
 */
void sd_periodic_check(SPIClass& spi, int cs_pin, uint32_t speed);

/**
 * Mark SD operation success/failure for state tracking.
 */
void sd_op_success();
void sd_op_failure();

/**
 * Safely unmount SD card (call before hot-unplug or sleep).
 */
void sd_unmount_safe();

/**
 * Check if SD is currently available and mounted.
 */
inline bool sd_is_available() { return g_hw.sd_available && g_hw.sd_state == SD_MOUNTED; }

/**
 * Get cached SD card info (non-blocking, may be stale).
 */
inline uint64_t sd_get_total_bytes() { return g_hw.sd_total_bytes; }
inline uint64_t sd_get_free_bytes() { return g_hw.sd_free_bytes; }

/**
 * Update cached SD space info (call periodically, not on every request).
 */
void sd_update_space_cache();

// ============================================================================
// API FUNCTIONS - SAFE MODE
// ============================================================================

/**
 * Check for rapid reboot condition and enter safe mode if needed.
 * Call early in setup() before peripheral init.
 * Returns true if in safe mode (skip optional peripheral init).
 */
bool safe_mode_check();

/**
 * Update safe mode state based on system stability.
 * Call from loop() to track uptime and clear safe mode after recovery.
 */
void safe_mode_update();

/**
 * Force entry into safe mode (e.g., after critical error).
 */
void safe_mode_enter(const char* reason);

/**
 * Clear safe mode (after user intervention or recovery period).
 */
void safe_mode_clear();

/**
 * User-initiated escape from a maxed-out safe mode: reset the recovery
 * budget and reboot into full operation. Reachable from the dashboard
 * since the WiFi AP + HTTP server keep running in safe mode.
 */
void safe_mode_force_retry();

/**
 * Check if in safe mode.
 */
inline bool is_safe_mode() { return g_hw.safe_mode; }

// ============================================================================
// API FUNCTIONS - GENERAL
// ============================================================================

/**
 * Initialize hardware state manager.
 * Call once at start of setup(), before any hardware init.
 */
void hw_state_init();

/**
 * Print hardware state summary to Serial.
 */
void hw_state_print();

/**
 * Get hardware state as JSON for API responses.
 * Non-blocking, uses cached values.
 */
size_t hw_state_json(char* buf, size_t buf_size);

// ============================================================================
// IMPLEMENTATION
// ============================================================================

// Global instance
HardwareState g_hw = {0};

// NVS for persistent state
static Preferences g_hw_nvs;

// ────────────────────────────────────────────────────────────────────────────

void hw_state_init() {
  memset(&g_hw, 0, sizeof(g_hw));

  g_hw.gps_state = GPS_ABSENT;
  g_hw.sd_state = SD_ABSENT;

  g_hw.last_stable_ms = millis();
}

// ────────────────────────────────────────────────────────────────────────────
// GPS FUNCTIONS
// ────────────────────────────────────────────────────────────────────────────

bool gps_probe(HardwareSerial& serial, uint32_t timeout_ms) {
  uint32_t start = millis();
  int bytes_received = 0;
  bool saw_nmea_start = false;

  // Clear any stale data
  while (serial.available()) {
    serial.read();
  }

  Serial.print("[GPS] Probing for GNSS module...");

  // Wait for any data with timeout
  while (millis() - start < timeout_ms) {
    if (serial.available()) {
      int c = serial.read();
      bytes_received++;

      // Look for NMEA sentence start
      if (c == '$') {
        saw_nmea_start = true;
      }

      // If we've seen a $ and received some reasonable data, GPS is present
      if (saw_nmea_start && bytes_received >= 10) {
        g_hw.gps_available = true;
        g_hw.gps_state = GPS_DETECTED;
        g_hw.gps_detect_time_ms = millis();
        g_hw.gps_last_data_ms = millis();
        g_hw.gps_ever_detected = true;
        Serial.printf(" detected (%d bytes in %lums)\n", bytes_received, millis() - start);
        return true;
      }
    }

    // Yield to prevent watchdog during probe
    yield();
    delay(1);
  }

  // Timeout - no GPS detected
  g_hw.gps_available = false;
  g_hw.gps_state = GPS_ABSENT;
  Serial.printf(" not detected (timeout after %lums)\n", timeout_ms);
  return false;
}

void gps_update_state(bool received_data, bool has_valid_fix) {
  uint32_t now = millis();
  GpsState old_state = g_hw.gps_state;

  if (received_data) {
    g_hw.gps_last_data_ms = now;

    // If we were absent and now receiving data, we're detected again
    if (g_hw.gps_state == GPS_ABSENT) {
      g_hw.gps_state = GPS_DETECTED;
      g_hw.gps_available = true;
      if (!g_hw.gps_ever_detected) {
        g_hw.gps_detect_time_ms = now;
        g_hw.gps_ever_detected = true;
      }
    }

    // Update fix state
    if (has_valid_fix) {
      g_hw.gps_last_fix_ms = now;
      if (g_hw.gps_state != GPS_HAS_FIX) {
        g_hw.gps_state = GPS_HAS_FIX;
      }
    } else if (g_hw.gps_state == GPS_HAS_FIX) {
      // Had fix, data still coming, but no fix
      if (now - g_hw.gps_last_fix_ms > hw_config::GPS_FIX_TIMEOUT_MS) {
        g_hw.gps_state = GPS_LOST_FIX;
      }
    }
  } else {
    // No data received this cycle - check for timeout
    if (g_hw.gps_last_data_ms > 0 &&
        now - g_hw.gps_last_data_ms > hw_config::GPS_DATA_TIMEOUT_MS) {
      g_hw.gps_state = GPS_ABSENT;
      g_hw.gps_available = false;
    }
  }

  // Log state transitions (don't log GPS coordinates for privacy)
  if (old_state != g_hw.gps_state) {
    Serial.printf("[GPS] State: %s -> %s\n",
                  gps_state_name(old_state),
                  gps_state_name(g_hw.gps_state));
  }
}

// ────────────────────────────────────────────────────────────────────────────
// SD CARD FUNCTIONS
// ────────────────────────────────────────────────────────────────────────────

static bool sd_try_mount(SPIClass& spi, int cs_pin, uint32_t speed) {
  // Try to mount with given speed
  if (SD.begin(cs_pin, spi, speed)) {
    return true;
  }
  return false;
}

// ────────────────────────────────────────────────────────────────────────────
// SD MOUNT WORKER — bounded, watchdog-safe mounting
// ────────────────────────────────────────────────────────────────────────────
// SD.begin() is a chain of yield-free CPU spin loops in the SPI SD driver
// (500–1000 ms card waits, ×3 retries, across two mount speeds plus FAT
// sector reads) with NO overall deadline — a wedged-but-present or slow card
// can hold it well past the 8 s panic watchdog. It used to run directly on
// the WDT-subscribed loop task, where the SD_MOUNT_TIMEOUT_MS checks BETWEEN
// the two blocking attempts bounded nothing: a bad card panicked the
// watchdog and crash-looped the device, and the loop()'s periodic remount
// repeated the same call even in SAFE MODE (~38 s in, before the 60 s
// safe-mode recovery window) so safe mode itself crash-looped forever.
//
// All blocking mount work now runs on this dedicated worker task; the loop
// task only polls a state byte:
//   IDLE → REQUESTED (loop posts) → RUNNING (worker) → DONE (worker; result
//   written first) → IDLE (loop adopts the result into g_hw).
// Load-bearing details:
//  * The worker runs at tskIDLE_PRIORITY (0). BOTH cores' IDLE tasks are
//    subscribed to the panic watchdog and the SD driver's waits never yield
//    — at any higher priority a stuck mount would starve an IDLE task and
//    simply move the panic there. At priority 0 the tick time-slicer keeps
//    the IDLE task (and its watchdog feed) running.
//  * Only the loop task writes g_hw. The worker fills a private result and
//    release-stores the state byte, so httpd-task readers keep the single-
//    writer model they rely on and the 64-bit byte counters can't tear.
//  * Nothing ever cancels a running mount (no SD.end(), no bus teardown —
//    that would free driver state under the worker). Every wait in the
//    driver is count/deadline-bounded, so SD.begin always returns; a result
//    that lands after the caller's wait budget is adopted by a later loop
//    pass instead of being discarded.

struct SdMountResult {
  bool ok;
  uint64_t total_bytes;
  uint64_t used_bytes;
};

enum : uint8_t { SD_MW_IDLE = 0, SD_MW_REQUESTED = 1, SD_MW_RUNNING = 2, SD_MW_DONE = 3 };

static TaskHandle_t s_sd_worker = nullptr;
static volatile uint8_t s_sd_mount_state = SD_MW_IDLE;
static SdMountResult s_sd_mount_result = {};
static SPIClass* s_sd_worker_spi = nullptr;
static int s_sd_worker_cs = -1;
static uint32_t s_sd_worker_speed = 0;

static inline uint8_t sd_mount_state() {
  return __atomic_load_n(&s_sd_mount_state, __ATOMIC_ACQUIRE);
}

bool sd_mount_in_flight() {
  uint8_t st = sd_mount_state();
  return st == SD_MW_REQUESTED || st == SD_MW_RUNNING;
}

static void sd_mount_worker(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (sd_mount_state() != SD_MW_REQUESTED) continue;
    __atomic_store_n(&s_sd_mount_state, SD_MW_RUNNING, __ATOMIC_RELEASE);
    SdMountResult r = {};
    r.ok = sd_try_mount(*s_sd_worker_spi, s_sd_worker_cs, s_sd_worker_speed);
    if (!r.ok) {
      r.ok = sd_try_mount(*s_sd_worker_spi, s_sd_worker_cs, s_sd_worker_speed / 4);
    }
    if (r.ok) {
      // Cache card info here so the loop side never blocks on FAT scans.
      r.total_bytes = SD.totalBytes();
      r.used_bytes = SD.usedBytes();
    }
    s_sd_mount_result = r;  // written BEFORE the release-store below
    __atomic_store_n(&s_sd_mount_state, SD_MW_DONE, __ATOMIC_RELEASE);
  }
}

// Post a mount request (non-blocking). False if a previous attempt is still
// in flight or the worker can't be created; the caller treats either as
// "card not available yet" and a later periodic tick retries.
static bool sd_mount_request(SPIClass& spi, int cs_pin, uint32_t speed) {
  if (sd_mount_in_flight()) return false;
  if (!s_sd_worker) {
    // Priority 0 + 4 KB internal stack (PSRAM task stacks aren't supported
    // by the prebuilt core); pinned to core 0 so a busy loop task on core 1
    // can't starve it. See the worker comment block for why priority 0 is
    // load-bearing.
    if (xTaskCreatePinnedToCore(sd_mount_worker, "sd_mount", 4096, nullptr,
                                tskIDLE_PRIORITY, &s_sd_worker, 0) != pdPASS) {
      s_sd_worker = nullptr;
      Serial.println("[SD] mount worker create failed - treating card as absent");
      return false;
    }
  }
  s_sd_worker_spi = &spi;
  s_sd_worker_cs = cs_pin;
  s_sd_worker_speed = speed;
  __atomic_store_n(&s_sd_mount_state, SD_MW_REQUESTED, __ATOMIC_RELEASE);
  xTaskNotifyGive(s_sd_worker);
  return true;
}

// Adopt a completed mount result into g_hw (LOOP TASK ONLY — this is the
// single g_hw writer). Returns true if a result was applied this call;
// callers check g_hw.sd_state for the outcome.
static bool sd_mount_try_adopt() {
  if (sd_mount_state() != SD_MW_DONE) return false;
  SdMountResult r = s_sd_mount_result;  // worker is back in its idle wait
  __atomic_store_n(&s_sd_mount_state, SD_MW_IDLE, __ATOMIC_RELEASE);
  if (r.ok) {
    g_hw.sd_available = true;
    g_hw.sd_state = SD_MOUNTED;
    g_hw.sd_mount_time_ms = millis();
    g_hw.sd_last_success_ms = millis();
    g_hw.sd_consecutive_errors = 0;
    g_hw.sd_total_bytes = r.total_bytes;
    // Protect against wraparound if filesystem is corrupted
    g_hw.sd_free_bytes = (r.total_bytes > r.used_bytes)
                             ? (r.total_bytes - r.used_bytes) : 0;
    Serial.printf("[SD] mounted (%llu MB, %llu MB free)\n",
                  g_hw.sd_total_bytes / (1024 * 1024),
                  g_hw.sd_free_bytes / (1024 * 1024));
  } else {
    g_hw.sd_available = false;
    g_hw.sd_state = SD_ABSENT;
    g_hw.sd_consecutive_errors++;
    Serial.println("[SD] not present or failed");
  }
  return true;
}

bool sd_mount_safe(SPIClass& spi, int cs_pin, uint32_t speed) {
  Serial.println("[SD] Attempting mount (background worker)...");
  if (!sd_mount_request(spi, cs_pin, speed)) {
    g_hw.sd_available = false;
    g_hw.sd_state = SD_ABSENT;
    return false;
  }

  // Bounded wait: poll the worker in short slices, feeding the task watchdog
  // each slice — the mount can no longer starve the 8 s panic WDT no matter
  // how long the driver spins.
  const uint32_t start = millis();
  while (!sd_mount_logic::mount_wait_expired(millis(), start,
                                             hw_config::SD_MOUNT_TIMEOUT_MS)) {
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(50));
    if (sd_mount_try_adopt()) {
      return g_hw.sd_state == SD_MOUNTED;
    }
  }

  // Budget spent and the worker is still inside the blocking driver call.
  // Report "not available yet" and keep booting — the AP and dashboard come
  // up regardless, and the eventual result is adopted by a later loop pass
  // (sd_periodic_check). Deliberately NOT counted as a consecutive error:
  // the attempt hasn't concluded.
  Serial.println("[SD] still probing - continuing boot; result adopted when ready");
  g_hw.sd_available = false;
  g_hw.sd_state = SD_ABSENT;
  return false;
}

bool sd_verify_present() {
  if (!g_hw.sd_available || g_hw.sd_state != SD_MOUNTED) {
    return false;
  }
  // Never touch the SD object while the mount worker owns it (state can't
  // normally be MOUNTED with a mount in flight, but keep the teardown below
  // impossible by construction — SD.end() under the worker is use-after-free).
  if (sd_mount_in_flight()) {
    return true;
  }

  // Quick check - try to stat the root directory
  // This is fast and will fail if card removed
  File root = SD.open("/");
  if (!root) {
    // Card was removed or failed
    g_hw.sd_available = false;
    g_hw.sd_state = SD_ABSENT;
    g_hw.sd_consecutive_errors++;
    Serial.println("[SD] Card removed or failed");
    SD.end();  // Clean up
    return false;
  }
  root.close();
  return true;
}

void sd_periodic_check(SPIClass& spi, int cs_pin, uint32_t speed) {
  // Adopt any completed background mount first — one atomic read when
  // nothing is pending, and running it every pass means a result that
  // outlived a caller's wait budget (boot or a previous tick) lands
  // promptly instead of waiting out the 30 s interval.
  if (sd_mount_try_adopt() && g_hw.sd_state == SD_MOUNTED) {
    Serial.println("[SD] Card detected and mounted");
    // No deferred-write queue is maintained while the card is absent:
    // writes that arrived during SD_ABSENT are accounted for in
    // g_hw.sd_error_count (see sd_op_failure) and dropped by design.
    // If a RAM ring buffer for deferred writes is ever added, flush it
    // here right after remount.
  }

  uint32_t now = millis();
  const sd_mount_logic::PeriodicAction action = sd_mount_logic::periodic_action(
      g_hw.safe_mode, g_hw.sd_state == SD_MOUNTED, sd_mount_in_flight(),
      now, g_hw.sd_last_check_ms, hw_config::SD_RECHECK_INTERVAL_MS);
  // NONE covers: interval not elapsed, an attempt already in flight, and
  // SAFE MODE — boot skipped SD init on purpose, and this loop path used to
  // re-run the blocking mount anyway, crash-looping the one mode that
  // exists to be stable. The decision table is host-tested
  // (sd_mount_logic.h / test_sd_mount_logic.cpp).
  if (action == sd_mount_logic::PeriodicAction::NONE) {
    return;
  }
  g_hw.sd_last_check_ms = now;

  if (action == sd_mount_logic::PeriodicAction::VERIFY) {
    // Verify still mounted
    if (!sd_verify_present()) {
      Serial.println("[SD] Lost connection, will retry later");
    } else {
      // Periodically update space cache
      sd_update_space_cache();
    }
  } else {  // REMOUNT — card may have been (re-)inserted
    Serial.println("[SD] Periodic check - background remount attempt...");
    // Fire-and-forget: the worker does the blocking work; the result is
    // adopted by the try-adopt pass above on a later loop() iteration. The
    // loop task never blocks here, so a wedged card can't starve the
    // watchdog or stall the AP/portal.
    sd_mount_request(spi, cs_pin, speed);
  }
}

void sd_op_success() {
  g_hw.sd_last_success_ms = millis();
  g_hw.sd_write_count++;

  // Clear consecutive error count after success
  if (g_hw.sd_consecutive_errors > 0) {
    g_hw.sd_consecutive_errors = 0;
  }
}

void sd_op_failure() {
  g_hw.sd_error_count++;
  g_hw.sd_consecutive_errors++;

  // After multiple consecutive errors, mark card as failed
  if (g_hw.sd_consecutive_errors >= hw_config::SD_MAX_RETRIES) {
    if (g_hw.sd_state == SD_MOUNTED && !sd_mount_in_flight()) {
      Serial.println("[SD] Multiple consecutive errors - marking as error state");
      g_hw.sd_state = SD_ERROR;
      g_hw.sd_available = false;
      SD.end();
    }
  }
}

void sd_unmount_safe() {
  if (sd_mount_in_flight()) {
    // A mount attempt owns the SD object; tearing it down now would free
    // driver state under the worker. Callers of this path (pre-sleep /
    // hot-unplug) can retry after the attempt concludes.
    Serial.println("[SD] unmount deferred: mount attempt in flight");
    return;
  }
  if (g_hw.sd_state == SD_MOUNTED) {
    Serial.println("[SD] Flushing and unmounting...");

    // Force any pending directory/metadata writes to the card before we tear
    // down the FAT driver. SD.end() already calls esp_vfs_fat_sdmmc_unmount
    // which flushes, but opening+closing a zero-length sentinel guarantees
    // the FAT allocation table is synced even on driver variants that defer
    // the final metadata write until the next I/O.
    File sync_handle = SD.open("/.scv_sync", FILE_WRITE);
    if (sync_handle) {
      sync_handle.flush();
      sync_handle.close();
      SD.remove("/.scv_sync");
    }

    SD.end();
    g_hw.sd_available = false;
    g_hw.sd_state = SD_ABSENT;
  }
}

void sd_update_space_cache() {
  if (g_hw.sd_state != SD_MOUNTED || !g_hw.sd_available) {
    return;
  }

  // These calls can be slow, so we only do them periodically, not on every API request
  g_hw.sd_total_bytes = SD.totalBytes();
  uint64_t used = SD.usedBytes();
  g_hw.sd_free_bytes = (g_hw.sd_total_bytes > used) ? (g_hw.sd_total_bytes - used) : 0;
}

// ────────────────────────────────────────────────────────────────────────────
// SAFE MODE FUNCTIONS
// ────────────────────────────────────────────────────────────────────────────

// Human-readable name for the chip's last reset cause.
const char* reset_reason_name(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external-pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int-watchdog";
    case ESP_RST_TASK_WDT:  return "task-watchdog";
    case ESP_RST_WDT:       return "other-watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
  }
}

// Only resets that signal an actual fault count toward safe mode. A user
// toggling power, pressing reset, replugging USB, or our own ESP.restart()
// must NOT push the device toward safe mode — otherwise routine handling
// trips the boot-loop guard. Brownout counts because disabling the optional
// peripherals (camera inrush, BLE/WiFi radios) reduces current draw and can
// itself break a brownout loop on a marginal supply.
bool reset_is_crash(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC   ||
         r == ESP_RST_INT_WDT ||
         r == ESP_RST_TASK_WDT ||
         r == ESP_RST_WDT     ||
         r == ESP_RST_BROWNOUT;
}

// Optional hook invoked immediately before any safe-mode-initiated reboot
// (auto-recovery or manual retry). The application sets this to flush
// volatile state — chiefly the witness chain head/seq — to NVS. Without it
// a recovery reboot would roll the chain back to the last throttled persist
// and reuse sequence numbers, breaking the tamper-evidence guarantee.
// ── Crash-stage breadcrumb (bench triage) ────────────────────────────────
// The task-watchdog abort names the starved TASK but not what the firmware
// was DOING — and field crashes rarely come with the build's ELF for
// symbolizing backtraces. So the sketch drops a named breadcrumb into
// RTC-noinit memory before each risky stage; it survives every reset
// except power-on, and safe_mode_check() prints it on the crash reboot.
// One line of serial output replaces an addr2line session.
RTC_NOINIT_ATTR char     g_crash_stage[48];
RTC_NOINIT_ATTR uint32_t g_crash_stage_magic;
constexpr uint32_t CRASH_STAGE_MAGIC = 0x53544147;  // "STAG"

inline void boot_stage(const char* stage) {
  strlcpy(g_crash_stage, stage, sizeof(g_crash_stage));
  g_crash_stage_magic = CRASH_STAGE_MAGIC;
}

typedef void (*safe_mode_pre_reboot_fn)();
safe_mode_pre_reboot_fn g_safe_mode_pre_reboot = nullptr;

inline void safe_mode_flush_before_reboot() {
  if (g_safe_mode_pre_reboot) g_safe_mode_pre_reboot();
}

bool safe_mode_check() {
  g_hw_nvs.begin("hw_state", false);

  // Read previous safe mode flag
  g_hw.safe_mode = g_hw_nvs.getBool(hw_config::NVS_SAFE_MODE, false);

  // Classify why the chip reset. Without a real-time clock we count
  // consecutive *crash* reboots (panic / watchdog / brownout) in NVS; the
  // counter is cleared in `safe_mode_update()` once the device runs stably
  // for SAFE_MODE_WINDOW_MS. Benign resets (power-on, reset button, USB
  // replug, our own ESP.restart()) are logged but do not advance the count.
  g_hw.last_reset_reason = esp_reset_reason();
  g_hw.last_reset_was_crash = reset_is_crash(g_hw.last_reset_reason);

  uint8_t rapid_count = g_hw_nvs.getUChar("rapid_count", 0);

  if (g_hw.last_reset_was_crash) {
    rapid_count++;
    g_hw_nvs.putUChar("rapid_count", rapid_count);
    Serial.printf("[SAFE] Crash reset (%s) - consecutive crash count %u/%u\n",
                  reset_reason_name(g_hw.last_reset_reason),
                  rapid_count, hw_config::SAFE_MODE_REBOOT_LIMIT);
    if (g_crash_stage_magic == CRASH_STAGE_MAGIC && g_crash_stage[0]) {
      g_crash_stage[sizeof(g_crash_stage) - 1] = '\0';
      Serial.printf("[SAFE] Last breadcrumb before the crash: %s\n",
                    g_crash_stage);
    }
  } else {
    Serial.printf("[SAFE] Clean reset (%s) - not counted toward safe mode\n",
                  reset_reason_name(g_hw.last_reset_reason));
    // RTC-noinit is garbage after a true power-on: invalidate so a stale
    // or random breadcrumb can never masquerade as evidence.
    if (g_hw.last_reset_reason == ESP_RST_POWERON) g_crash_stage_magic = 0;
  }
  boot_stage("early-boot");
  g_hw.rapid_boot_count = rapid_count;

  g_hw_nvs.end();

  // Check if we should enter safe mode
  if (rapid_count >= hw_config::SAFE_MODE_REBOOT_LIMIT) {
    if (!g_hw.safe_mode) {
      safe_mode_enter("Repeated crash reboots detected");  // sets safe_mode_entered_ms
    } else {
      // Already in safe mode from a previous session — measure the recovery
      // window from this boot rather than the memset-zero default.
      g_hw.safe_mode_entered_ms = millis();
    }
    return true;
  }

  // If we were in safe mode and now booting normally, stay in safe mode
  // until the recovery period passes. Measure that period from this boot —
  // safe mode was entered in a previous session, so the in-RAM entry time
  // would otherwise be 0 and the recovery timer would be meaningless.
  if (g_hw.safe_mode) {
    g_hw.safe_mode_entered_ms = millis();
    Serial.println("[SAFE] Still in safe mode from previous session");
    return true;
  }

  return false;
}

void safe_mode_update() {
  uint32_t now = millis();

  // Track stability
  g_hw.last_stable_ms = now;

  // If in safe mode, attempt a bounded recovery once the device has been
  // stable for the recovery window. The subtraction is overflow-safe.
  if (g_hw.safe_mode) {
    if ((now - g_hw.safe_mode_entered_ms) > hw_config::SAFE_MODE_RECOVERY_MS) {
      // If NVS won't open we can't read/advance the bounded-recovery budget;
      // rebooting blindly would risk an unbounded loop, so stay in safe mode.
      if (!g_hw_nvs.begin("hw_state", false)) {
        static bool nvs_logged = false;
        if (!nvs_logged) {
          Serial.println("[SAFE] NVS unavailable - staying in safe mode to avoid an unbounded reboot loop");
          nvs_logged = true;
        }
        return;
      }
      uint8_t recoveries = g_hw_nvs.getUChar(hw_config::NVS_RECOVERY_COUNT, 0);

      // Bounded: if full operation keeps crashing back into safe mode, stop
      // rebooting and stay here (stable, dashboard reachable) for a human.
      if (recoveries >= hw_config::SAFE_MODE_MAX_RECOVERIES) {
        g_hw_nvs.end();
        static bool logged = false;
        if (!logged) {
          Serial.println("[SAFE] Max recovery attempts reached - staying in safe mode. "
                         "Fix the device, then use 'Retry full boot' on the dashboard.");
          logged = true;
        }
        return;
      }

      g_hw_nvs.putUChar(hw_config::NVS_RECOVERY_COUNT, recoveries + 1);
      g_hw_nvs.end();

      Serial.printf("[SAFE] Stable %us - recovery attempt %u/%u, rebooting to restore full operation\n",
                    hw_config::SAFE_MODE_RECOVERY_MS / 1000,
                    recoveries + 1, hw_config::SAFE_MODE_MAX_RECOVERIES);
      safe_mode_clear();             // clears safe_mode flag + rapid_count, NOT recov_count
      safe_mode_flush_before_reboot();  // persist witness chain so seq isn't reused
      delay(100);                    // let serial flush before the reset
      ESP.restart();
    }
  } else {
    // Not in safe mode - after a stable full-operation window, reset BOTH
    // counters so a transient fault self-heals and the recovery budget returns.
    if (now > hw_config::SAFE_MODE_WINDOW_MS && g_hw.rapid_boot_count > 0) {
      g_hw_nvs.begin("hw_state", false);
      g_hw_nvs.putUChar("rapid_count", 0);
      g_hw_nvs.putUChar(hw_config::NVS_RECOVERY_COUNT, 0);
      g_hw_nvs.end();
      g_hw.rapid_boot_count = 0;
    }
  }
}

void safe_mode_enter(const char* reason) {
  Serial.printf("[SAFE] Entering safe mode: %s\n", reason);

  g_hw.safe_mode = true;
  g_hw.safe_mode_entered_ms = millis();

  g_hw_nvs.begin("hw_state", false);
  g_hw_nvs.putBool(hw_config::NVS_SAFE_MODE, true);
  g_hw_nvs.putULong(hw_config::NVS_SAFE_MODE_TIME, g_hw.safe_mode_entered_ms);
  g_hw_nvs.end();

  Serial.println("[SAFE] Optional peripherals disabled - core functions only");
}

void safe_mode_clear() {
  Serial.println("[SAFE] Clearing safe mode");

  g_hw.safe_mode = false;
  g_hw.rapid_boot_count = 0;

  g_hw_nvs.begin("hw_state", false);
  g_hw_nvs.putBool(hw_config::NVS_SAFE_MODE, false);
  g_hw_nvs.putUChar("rapid_count", 0);
  g_hw_nvs.end();
}

void safe_mode_force_retry() {
  Serial.println("[SAFE] Manual retry requested - resetting recovery budget and rebooting");

  g_hw_nvs.begin("hw_state", false);
  g_hw_nvs.putUChar(hw_config::NVS_RECOVERY_COUNT, 0);
  g_hw_nvs.end();

  safe_mode_clear();
  safe_mode_flush_before_reboot();  // persist witness chain so seq isn't reused
  delay(500);  // let serial + HTTP response flush over Wi-Fi before the reset
  ESP.restart();
}

// ────────────────────────────────────────────────────────────────────────────
// GENERAL FUNCTIONS
// ────────────────────────────────────────────────────────────────────────────

void hw_state_print() {
  Serial.println();
  Serial.println("=== HARDWARE STATE ===");
  Serial.printf("  Safe Mode: %s\n", g_hw.safe_mode ? "YES" : "no");
  Serial.printf("  Last Reset: %s (%s)\n",
                reset_reason_name(g_hw.last_reset_reason),
                g_hw.last_reset_was_crash ? "crash" : "clean");
  Serial.printf("  Crash Reboot Count: %d/%d\n", g_hw.rapid_boot_count, hw_config::SAFE_MODE_REBOOT_LIMIT);
  Serial.println();
  Serial.printf("  GPS: %s (%s)\n",
                g_hw.gps_available ? "available" : "absent",
                gps_state_name(g_hw.gps_state));
  if (g_hw.gps_ever_detected) {
    Serial.printf("       Last data: %lu ms ago\n", millis() - g_hw.gps_last_data_ms);
  }
  Serial.println();
  Serial.printf("  SD Card: %s (%s)\n",
                g_hw.sd_available ? "available" : "absent",
                sd_state_name(g_hw.sd_state));
  if (g_hw.sd_state == SD_MOUNTED) {
    Serial.printf("           %llu MB total, %llu MB free\n",
                  g_hw.sd_total_bytes / (1024*1024),
                  g_hw.sd_free_bytes / (1024*1024));
    Serial.printf("           Writes: %lu, Errors: %lu\n",
                  g_hw.sd_write_count, g_hw.sd_error_count);
  }
  Serial.println();
  Serial.printf("  Camera: %s\n", g_hw.camera_available ? "available" : "not initialized");
  Serial.println("========================");
}

size_t hw_state_json(char* buf, size_t buf_size) {
  int len = snprintf(buf, buf_size,
    "{"
    "\"safe_mode\":%s,"
    "\"rapid_boot_count\":%d,"
    "\"last_reset\":\"%s\","
    "\"last_reset_crash\":%s,"
    "\"gps\":{"
      "\"available\":%s,"
      "\"state\":\"%s\","
      "\"ever_detected\":%s"
    "},"
    "\"sd\":{"
      "\"available\":%s,"
      "\"state\":\"%s\","
      "\"total_bytes\":%llu,"
      "\"free_bytes\":%llu,"
      "\"writes\":%lu,"
      "\"errors\":%lu"
    "},"
    "\"camera\":{\"available\":%s}"
    "}",
    g_hw.safe_mode ? "true" : "false",
    g_hw.rapid_boot_count,
    reset_reason_name(g_hw.last_reset_reason),
    g_hw.last_reset_was_crash ? "true" : "false",
    g_hw.gps_available ? "true" : "false",
    gps_state_name(g_hw.gps_state),
    g_hw.gps_ever_detected ? "true" : "false",
    g_hw.sd_available ? "true" : "false",
    sd_state_name(g_hw.sd_state),
    g_hw.sd_total_bytes,
    g_hw.sd_free_bytes,
    g_hw.sd_write_count,
    g_hw.sd_error_count,
    g_hw.camera_available ? "true" : "false"
  );

  return (len > 0 && len < (int)buf_size) ? len : 0;
}

#endif // SECURACV_HARDWARE_STATE_H
