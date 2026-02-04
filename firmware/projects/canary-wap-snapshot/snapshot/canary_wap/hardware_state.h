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

// ============================================================================
// CONFIGURATION
// ============================================================================

namespace hw_config {
  // GPS detection timeouts
  static const uint32_t GPS_DETECT_TIMEOUT_MS   = 1000;   // Max time to wait for GPS on boot
  static const uint32_t GPS_DATA_TIMEOUT_MS     = 30000;  // Mark GPS absent if no data for 30s
  static const uint32_t GPS_FIX_TIMEOUT_MS      = 5000;   // Mark fix lost after 5s no update

  // SD card timeouts and intervals
  static const uint32_t SD_MOUNT_TIMEOUT_MS     = 2000;   // Max time to attempt SD mount
  static const uint32_t SD_RECHECK_INTERVAL_MS  = 30000;  // Recheck for SD every 30s when absent
  static const uint32_t SD_OP_TIMEOUT_MS        = 1000;   // Timeout for individual SD operations
  static const uint8_t  SD_MAX_RETRIES          = 2;      // Max retries before marking SD failed

  // Safe mode protection
  static const uint32_t SAFE_MODE_WINDOW_MS     = 60000;  // 60 second window
  static const uint8_t  SAFE_MODE_REBOOT_LIMIT  = 3;      // 3 reboots in window triggers safe mode
  static const uint32_t SAFE_MODE_RECOVERY_MS   = 300000; // 5 minutes stable to clear safe mode

  // NVS keys for boot tracking
  static const char* NVS_BOOT_TIMES      = "boot_times";   // Array of recent boot timestamps
  static const char* NVS_SAFE_MODE       = "safe_mode";    // Safe mode flag
  static const char* NVS_SAFE_MODE_TIME  = "safe_time";    // When safe mode was entered
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

  // === Safe Mode ===
  bool     safe_mode;             // In safe mode (optional peripherals disabled)
  uint8_t  rapid_boot_count;      // Recent boot count
  uint32_t safe_mode_entered_ms;  // When safe mode was entered
  uint32_t last_stable_ms;        // Last known stable operation time
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
 * Attempt to mount SD card with timeout.
 * Non-blocking - uses polling internally.
 * Sets sd_available and sd_state.
 *
 * @param spi      SPI bus instance
 * @param cs_pin   Chip select pin
 * @param speed    SPI speed (Hz), will fall back to slower if needed
 * @return true if mounted successfully
 */
bool sd_mount_safe(SPIClass& spi, int cs_pin, uint32_t speed);

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

bool sd_mount_safe(SPIClass& spi, int cs_pin, uint32_t speed) {
  uint32_t start = millis();

  Serial.print("[SD] Attempting mount...");

  // First try at requested speed
  if (sd_try_mount(spi, cs_pin, speed)) {
    goto mount_success;
  }

  // Check timeout
  if (millis() - start > hw_config::SD_MOUNT_TIMEOUT_MS / 2) {
    Serial.println(" fast mount timeout");
    goto mount_failed;
  }

  // Fallback to slower speed
  Serial.print(" (trying slower speed)...");
  if (sd_try_mount(spi, cs_pin, speed / 4)) {
    goto mount_success;
  }

  // Check final timeout
  if (millis() - start > hw_config::SD_MOUNT_TIMEOUT_MS) {
    Serial.println(" timeout");
    goto mount_failed;
  }

mount_failed:
  g_hw.sd_available = false;
  g_hw.sd_state = SD_ABSENT;
  g_hw.sd_consecutive_errors++;
  Serial.println(" not present or failed");
  return false;

mount_success:
  g_hw.sd_available = true;
  g_hw.sd_state = SD_MOUNTED;
  g_hw.sd_mount_time_ms = millis();
  g_hw.sd_last_success_ms = millis();
  g_hw.sd_consecutive_errors = 0;

  // Cache card info (only do this once on mount, not on every request)
  g_hw.sd_total_bytes = SD.totalBytes();
  g_hw.sd_free_bytes = SD.totalBytes() - SD.usedBytes();

  Serial.printf(" mounted (%llu MB, %llu MB free)\n",
                g_hw.sd_total_bytes / (1024*1024),
                g_hw.sd_free_bytes / (1024*1024));
  return true;
}

bool sd_verify_present() {
  if (!g_hw.sd_available || g_hw.sd_state != SD_MOUNTED) {
    return false;
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
  uint32_t now = millis();

  // Don't check too frequently
  if (now - g_hw.sd_last_check_ms < hw_config::SD_RECHECK_INTERVAL_MS) {
    return;
  }
  g_hw.sd_last_check_ms = now;

  if (g_hw.sd_state == SD_MOUNTED) {
    // Verify still mounted
    if (!sd_verify_present()) {
      Serial.println("[SD] Lost connection, will retry later");
    } else {
      // Periodically update space cache
      sd_update_space_cache();
    }
  } else {
    // Not mounted - try to mount (card may have been re-inserted)
    Serial.println("[SD] Periodic check - attempting remount...");
    if (sd_mount_safe(spi, cs_pin, speed)) {
      Serial.println("[SD] Card re-detected and mounted");
      // TODO: Flush any buffered data to SD
    }
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
    if (g_hw.sd_state == SD_MOUNTED) {
      Serial.println("[SD] Multiple consecutive errors - marking as error state");
      g_hw.sd_state = SD_ERROR;
      g_hw.sd_available = false;
      SD.end();
    }
  }
}

void sd_unmount_safe() {
  if (g_hw.sd_state == SD_MOUNTED) {
    Serial.println("[SD] Unmounting...");
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

bool safe_mode_check() {
  g_hw_nvs.begin("hw_state", false);

  // Read previous safe mode flag
  g_hw.safe_mode = g_hw_nvs.getBool(hw_config::NVS_SAFE_MODE, false);

  // Get stored boot times (up to 8 recent boot timestamps)
  uint32_t boot_times[8] = {0};
  size_t stored_size = g_hw_nvs.getBytes(hw_config::NVS_BOOT_TIMES, boot_times, sizeof(boot_times));

  uint32_t now = millis();  // Will be small since we just booted
  uint32_t now_sec = esp_timer_get_time() / 1000000;  // Actual time since boot in seconds

  // Count recent boots in window (stored as relative to previous boot)
  // This is tricky because millis() resets on reboot
  // We use a simple approach: store just the boot count that increments with each boot
  // and timestamps relative to a base

  // Simpler approach: just track rapid reboot count
  uint8_t rapid_count = g_hw_nvs.getUChar("rapid_count", 0);
  uint32_t last_boot_ms = g_hw_nvs.getULong("last_boot", 0);

  // If last boot was very recent (within window), increment count
  // We can't easily track real time without RTC, so use boot count heuristic
  // If the device boots again within ~60s, the millis() will be small
  // Since we can't persist real time, we increment count and clear after sustained uptime

  rapid_count++;
  g_hw.rapid_boot_count = rapid_count;

  // Store updated count
  g_hw_nvs.putUChar("rapid_count", rapid_count);
  g_hw_nvs.putULong("last_boot", now);

  g_hw_nvs.end();

  // Check if we should enter safe mode
  if (rapid_count >= hw_config::SAFE_MODE_REBOOT_LIMIT) {
    if (!g_hw.safe_mode) {
      safe_mode_enter("Rapid reboot detected");
    }
    return true;
  }

  // If we were in safe mode and now booting normally, stay in safe mode
  // until recovery period passes
  if (g_hw.safe_mode) {
    Serial.println("[SAFE] Still in safe mode from previous session");
    return true;
  }

  return false;
}

void safe_mode_update() {
  uint32_t now = millis();

  // Track stability
  g_hw.last_stable_ms = now;

  // If in safe mode, check if we can clear it
  if (g_hw.safe_mode) {
    // After SAFE_MODE_RECOVERY_MS of stable operation, clear safe mode
    if (now > hw_config::SAFE_MODE_RECOVERY_MS) {
      Serial.println("[SAFE] Recovery period complete - clearing safe mode");
      safe_mode_clear();
    }
  } else {
    // Not in safe mode - clear rapid boot counter after stable operation
    if (now > hw_config::SAFE_MODE_WINDOW_MS && g_hw.rapid_boot_count > 0) {
      g_hw_nvs.begin("hw_state", false);
      g_hw_nvs.putUChar("rapid_count", 0);
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

// ────────────────────────────────────────────────────────────────────────────
// GENERAL FUNCTIONS
// ────────────────────────────────────────────────────────────────────────────

void hw_state_print() {
  Serial.println();
  Serial.println("=== HARDWARE STATE ===");
  Serial.printf("  Safe Mode: %s\n", g_hw.safe_mode ? "YES" : "no");
  Serial.printf("  Rapid Boot Count: %d/%d\n", g_hw.rapid_boot_count, hw_config::SAFE_MODE_REBOOT_LIMIT);
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
