/*
 * SecuraCV Canary - System Monitor
 *
 * Monitors ESP32-S3 system health including:
 * - Internal temperature sensor (chip temperature)
 * - Heap/RAM usage (internal + PSRAM)
 * - CPU frequency and chip info
 * - Temperature alerts (hot/cold thresholds)
 *
 * Note: The ESP32-S3 does NOT have a built-in humidity sensor.
 * Humidity monitoring would require external hardware (DHT22, BME280, etc.)
 *
 * Reference: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/temp_sensor.html
 */

#ifndef SECURACV_SYS_MONITOR_H
#define SECURACV_SYS_MONITOR_H

#include <Arduino.h>
#include <esp_mac.h>
#include <esp_system.h>
#include "log_level.h"

// ════════════════════════════════════════════════════════════════════════════
// FEATURE FLAG
// ════════════════════════════════════════════════════════════════════════════

#define FEATURE_SYS_MONITOR 1

// ════════════════════════════════════════════════════════════════════════════
// TEMPERATURE THRESHOLDS (Celsius)
// ESP32-S3 operating range: -40C to 85C (junction temperature)
// Internal sensor measures chip die temperature, typically 10-20C above ambient
// ════════════════════════════════════════════════════════════════════════════

namespace sys_monitor {

// Temperature alert thresholds (chip temperature)
static const float TEMP_COLD_WARNING   = 5.0f;    // Below this: cold warning
static const float TEMP_COLD_CRITICAL  = 0.0f;    // Below this: critical cold
static const float TEMP_HOT_WARNING    = 65.0f;   // Above this: hot warning
static const float TEMP_HOT_CRITICAL   = 80.0f;   // Above this: critical (throttle/protect)

// Hysteresis to prevent alert flapping (degrees)
static const float TEMP_HYSTERESIS     = 3.0f;

// How often to log system metrics (ms)
static const uint32_t METRICS_LOG_INTERVAL_MS = 30000;  // Every 30 seconds

// How often to check for alerts (ms)
static const uint32_t ALERT_CHECK_INTERVAL_MS = 5000;   // Every 5 seconds

// ════════════════════════════════════════════════════════════════════════════
// TEMPERATURE ALERT STATE
// ════════════════════════════════════════════════════════════════════════════

enum TempAlertState : uint8_t {
  TEMP_NORMAL         = 0,  // Temperature within safe operating range
  TEMP_COLD_WARN      = 1,  // Getting cold - may affect performance
  TEMP_COLD_CRIT      = 2,  // Critically cold - potential damage risk
  TEMP_HOT_WARN       = 3,  // Getting hot - reduce load if possible
  TEMP_HOT_CRIT       = 4   // Critically hot - immediate action needed
};

// ════════════════════════════════════════════════════════════════════════════
// SYSTEM METRICS STRUCTURE
// ════════════════════════════════════════════════════════════════════════════

struct SystemMetrics {
  // Temperature (chip internal)
  float    temp_celsius;         // Current temperature
  float    temp_min;             // Minimum recorded
  float    temp_max;             // Maximum recorded
  float    temp_avg;             // Running average (EMA)
  TempAlertState temp_state;     // Current alert state
  uint32_t temp_readings;        // Number of readings taken

  // Internal heap (SRAM)
  uint32_t heap_total;           // Total internal heap
  uint32_t heap_free;            // Current free heap
  uint32_t heap_min_free;        // Minimum free ever (high water mark)
  uint32_t heap_largest_block;   // Largest contiguous free block

  // PSRAM (external SPI RAM - 8MB on XIAO ESP32S3 Sense)
  bool     psram_available;      // Whether PSRAM is present
  uint32_t psram_total;          // Total PSRAM
  uint32_t psram_free;           // Current free PSRAM
  uint32_t psram_min_free;       // Minimum free PSRAM

  // CPU/System info (static, set once at init)
  uint32_t cpu_freq_mhz;         // CPU frequency
  uint32_t flash_size;           // Flash chip size
  uint8_t  chip_revision;        // Chip revision number
  uint8_t  chip_cores;           // Number of CPU cores
  char     chip_model[24];       // Chip model string

  // Timing
  uint32_t last_update_ms;       // Last metrics update
  uint32_t last_log_ms;          // Last periodic log
  uint32_t last_alert_check_ms;  // Last alert check
  uint32_t uptime_sec;           // System uptime in seconds

  // Alert tracking
  uint32_t cold_alerts;          // Count of cold alerts
  uint32_t hot_alerts;           // Count of hot alerts
  bool     alert_active;         // Currently in an alert state
};

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL METRICS INSTANCE
// ════════════════════════════════════════════════════════════════════════════

extern SystemMetrics g_sys_metrics;

// ════════════════════════════════════════════════════════════════════════════
// API FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

/**
 * Initialize the system monitor.
 * Call once in setup() after Serial is ready.
 * Reads initial system info and takes first temperature reading.
 */
void init();

/**
 * Update system metrics.
 * Call regularly in loop() - internally rate-limited.
 * Updates temperature, heap, and checks for alerts.
 *
 * @param log_callback Optional callback for logging alerts
 */
void update(void (*log_callback)(LogLevel, LogCategory, const char*, const char*) = nullptr);

/**
 * Get the current temperature in Celsius.
 * Uses the ESP32-S3 internal temperature sensor.
 * Note: Measures chip die temperature, not ambient.
 *
 * @return Temperature in Celsius, or NAN if read fails
 */
float get_temperature();

/**
 * Get the current temperature alert state.
 */
TempAlertState get_temp_state();

/**
 * Get a human-readable name for a temperature state.
 */
const char* temp_state_name(TempAlertState state);

/**
 * Check if we're currently in an alert condition.
 */
bool is_alert_active();

/**
 * Print a comprehensive system status to Serial.
 * Beautiful formatted output for serial monitor.
 */
void print_status();

/**
 * Print a compact one-line status for periodic logging.
 */
void print_status_line();

/**
 * Get system metrics as JSON string.
 * Useful for HTTP API responses.
 *
 * @param buf Buffer to write JSON into
 * @param buf_size Size of buffer
 * @return Number of bytes written, or 0 on error
 */
size_t get_json(char* buf, size_t buf_size);

// ════════════════════════════════════════════════════════════════════════════
// UTILITY FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

/**
 * Format bytes as human-readable string (KB, MB).
 */
const char* format_bytes(uint32_t bytes, char* buf, size_t buf_size);

/**
 * Format uptime as human-readable string (HH:MM:SS or Xd HH:MM:SS).
 */
const char* format_uptime(uint32_t seconds, char* buf, size_t buf_size);

} // namespace sys_monitor

// ════════════════════════════════════════════════════════════════════════════
// IMPLEMENTATION
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_SYS_MONITOR

namespace sys_monitor {

// Global metrics instance
SystemMetrics g_sys_metrics;

// EMA alpha for temperature averaging
static const float TEMP_EMA_ALPHA = 0.1f;

// ────────────────────────────────────────────────────────────────────────────

void init() {
  memset(&g_sys_metrics, 0, sizeof(g_sys_metrics));

  // Initialize temperature tracking with extremes
  g_sys_metrics.temp_min = 999.0f;
  g_sys_metrics.temp_max = -999.0f;
  g_sys_metrics.temp_state = TEMP_NORMAL;

  // Get static system info
  g_sys_metrics.cpu_freq_mhz = ESP.getCpuFreqMHz();
  g_sys_metrics.flash_size = ESP.getFlashChipSize();
  g_sys_metrics.chip_revision = ESP.getChipRevision();
  g_sys_metrics.chip_cores = ESP.getChipCores();

  // Get chip model
  const char* model = ESP.getChipModel();
  strncpy(g_sys_metrics.chip_model, model ? model : "ESP32-S3", sizeof(g_sys_metrics.chip_model) - 1);

  // Get heap info
  g_sys_metrics.heap_total = ESP.getHeapSize();
  g_sys_metrics.heap_free = ESP.getFreeHeap();
  g_sys_metrics.heap_min_free = g_sys_metrics.heap_free;
  g_sys_metrics.heap_largest_block = ESP.getMaxAllocHeap();

  // Check for PSRAM
  g_sys_metrics.psram_available = ESP.getPsramSize() > 0;
  if (g_sys_metrics.psram_available) {
    g_sys_metrics.psram_total = ESP.getPsramSize();
    g_sys_metrics.psram_free = ESP.getFreePsram();
    g_sys_metrics.psram_min_free = g_sys_metrics.psram_free;
  }

  // Take initial temperature reading
  float temp = get_temperature();
  if (!isnan(temp)) {
    g_sys_metrics.temp_celsius = temp;
    g_sys_metrics.temp_min = temp;
    g_sys_metrics.temp_max = temp;
    g_sys_metrics.temp_avg = temp;
    g_sys_metrics.temp_readings = 1;
  }

  g_sys_metrics.last_update_ms = millis();
  g_sys_metrics.last_log_ms = millis();
  g_sys_metrics.last_alert_check_ms = millis();
}

// ────────────────────────────────────────────────────────────────────────────

float get_temperature() {
  // ESP32-S3 provides temperatureRead() in Arduino core
  // Returns chip die temperature in Celsius
  return temperatureRead();
}

// ────────────────────────────────────────────────────────────────────────────

TempAlertState get_temp_state() {
  return g_sys_metrics.temp_state;
}

// ────────────────────────────────────────────────────────────────────────────

const char* temp_state_name(TempAlertState state) {
  switch (state) {
    case TEMP_NORMAL:    return "NORMAL";
    case TEMP_COLD_WARN: return "COLD-WARN";
    case TEMP_COLD_CRIT: return "COLD-CRIT";
    case TEMP_HOT_WARN:  return "HOT-WARN";
    case TEMP_HOT_CRIT:  return "HOT-CRIT";
    default:             return "UNKNOWN";
  }
}

// ────────────────────────────────────────────────────────────────────────────

bool is_alert_active() {
  return g_sys_metrics.alert_active;
}

// ────────────────────────────────────────────────────────────────────────────

static TempAlertState evaluate_temp_state(float temp, TempAlertState current) {
  // Apply hysteresis based on current state
  // Only return to normal if clearly past threshold + hysteresis

  if (temp <= TEMP_COLD_CRITICAL) {
    return TEMP_COLD_CRIT;
  }

  if (temp >= TEMP_HOT_CRITICAL) {
    return TEMP_HOT_CRIT;
  }

  // For warning states, apply hysteresis
  if (current == TEMP_COLD_WARN) {
    // Stay in cold warning until temp rises above threshold + hysteresis
    if (temp > TEMP_COLD_WARNING + TEMP_HYSTERESIS) {
      return (temp >= TEMP_HOT_WARNING) ? TEMP_HOT_WARN : TEMP_NORMAL;
    }
    return TEMP_COLD_WARN;
  }

  if (current == TEMP_HOT_WARN) {
    // Stay in hot warning until temp drops below threshold - hysteresis
    if (temp < TEMP_HOT_WARNING - TEMP_HYSTERESIS) {
      return (temp <= TEMP_COLD_WARNING) ? TEMP_COLD_WARN : TEMP_NORMAL;
    }
    return TEMP_HOT_WARN;
  }

  // From normal state, check thresholds
  if (temp <= TEMP_COLD_WARNING) {
    return TEMP_COLD_WARN;
  }

  if (temp >= TEMP_HOT_WARNING) {
    return TEMP_HOT_WARN;
  }

  return TEMP_NORMAL;
}

// ────────────────────────────────────────────────────────────────────────────

void update(void (*log_callback)(LogLevel, LogCategory, const char*, const char*)) {
  uint32_t now = millis();

  // Update uptime
  g_sys_metrics.uptime_sec = now / 1000;

  // Update heap metrics
  g_sys_metrics.heap_free = ESP.getFreeHeap();
  g_sys_metrics.heap_largest_block = ESP.getMaxAllocHeap();
  if (g_sys_metrics.heap_free < g_sys_metrics.heap_min_free) {
    g_sys_metrics.heap_min_free = g_sys_metrics.heap_free;
  }

  // Update PSRAM metrics
  if (g_sys_metrics.psram_available) {
    g_sys_metrics.psram_free = ESP.getFreePsram();
    if (g_sys_metrics.psram_free < g_sys_metrics.psram_min_free) {
      g_sys_metrics.psram_min_free = g_sys_metrics.psram_free;
    }
  }

  // Read temperature
  float temp = get_temperature();
  if (!isnan(temp)) {
    g_sys_metrics.temp_celsius = temp;
    g_sys_metrics.temp_readings++;

    // Update min/max
    if (temp < g_sys_metrics.temp_min) g_sys_metrics.temp_min = temp;
    if (temp > g_sys_metrics.temp_max) g_sys_metrics.temp_max = temp;

    // Update EMA average
    if (g_sys_metrics.temp_readings == 1) {
      g_sys_metrics.temp_avg = temp;
    } else {
      g_sys_metrics.temp_avg = TEMP_EMA_ALPHA * temp + (1.0f - TEMP_EMA_ALPHA) * g_sys_metrics.temp_avg;
    }
  }

  g_sys_metrics.last_update_ms = now;

  // Check for temperature alerts (rate limited)
  if (now - g_sys_metrics.last_alert_check_ms >= ALERT_CHECK_INTERVAL_MS) {
    g_sys_metrics.last_alert_check_ms = now;

    TempAlertState new_state = evaluate_temp_state(g_sys_metrics.temp_celsius, g_sys_metrics.temp_state);

    // State transition - log if changed
    if (new_state != g_sys_metrics.temp_state) {
      TempAlertState old_state = g_sys_metrics.temp_state;
      g_sys_metrics.temp_state = new_state;

      // Track alerts
      if (new_state == TEMP_COLD_WARN || new_state == TEMP_COLD_CRIT) {
        g_sys_metrics.cold_alerts++;
      } else if (new_state == TEMP_HOT_WARN || new_state == TEMP_HOT_CRIT) {
        g_sys_metrics.hot_alerts++;
      }

      // Update alert flag
      g_sys_metrics.alert_active = (new_state != TEMP_NORMAL);

      // Log the state change
      if (log_callback) {
        char detail[64];
        snprintf(detail, sizeof(detail), "%.1fC %s->%s",
                 g_sys_metrics.temp_celsius,
                 temp_state_name(old_state),
                 temp_state_name(new_state));

        LogLevel level;
        if (new_state == TEMP_COLD_CRIT || new_state == TEMP_HOT_CRIT) {
          level = LOG_LEVEL_ALERT;
        } else if (new_state == TEMP_COLD_WARN || new_state == TEMP_HOT_WARN) {
          level = LOG_LEVEL_WARNING;
        } else {
          level = LOG_LEVEL_INFO;
        }

        log_callback(level, LOG_CAT_SENSOR, "Temp alert state change", detail);
      }
    }
  }

  // Periodic status logging
  if (now - g_sys_metrics.last_log_ms >= METRICS_LOG_INTERVAL_MS) {
    g_sys_metrics.last_log_ms = now;
    print_status_line();
  }
}

// ────────────────────────────────────────────────────────────────────────────

const char* format_bytes(uint32_t bytes, char* buf, size_t buf_size) {
  if (bytes >= 1048576) {
    snprintf(buf, buf_size, "%.2f MB", bytes / 1048576.0f);
  } else if (bytes >= 1024) {
    snprintf(buf, buf_size, "%.1f KB", bytes / 1024.0f);
  } else {
    snprintf(buf, buf_size, "%u B", bytes);
  }
  return buf;
}

// ────────────────────────────────────────────────────────────────────────────

const char* format_uptime(uint32_t seconds, char* buf, size_t buf_size) {
  uint32_t days = seconds / 86400;
  uint32_t hours = (seconds % 86400) / 3600;
  uint32_t mins = (seconds % 3600) / 60;
  uint32_t secs = seconds % 60;

  if (days > 0) {
    snprintf(buf, buf_size, "%ud %02u:%02u:%02u", days, hours, mins, secs);
  } else {
    snprintf(buf, buf_size, "%02u:%02u:%02u", hours, mins, secs);
  }
  return buf;
}

// ────────────────────────────────────────────────────────────────────────────

// Convert Celsius to Fahrenheit
inline float celsius_to_fahrenheit(float c) {
  return (c * 9.0f / 5.0f) + 32.0f;
}

// ────────────────────────────────────────────────────────────────────────────

void print_status_line() {
  char heap_str[16], psram_str[16], uptime_str[16];

  format_bytes(g_sys_metrics.heap_free, heap_str, sizeof(heap_str));
  format_uptime(g_sys_metrics.uptime_sec, uptime_str, sizeof(uptime_str));

  float temp_f = celsius_to_fahrenheit(g_sys_metrics.temp_celsius);

  Serial.printf("[SYS] %s | Temp: %.1fC/%.1fF [%s] | Heap: %s | ",
                uptime_str,
                g_sys_metrics.temp_celsius,
                temp_f,
                temp_state_name(g_sys_metrics.temp_state),
                heap_str);

  if (g_sys_metrics.psram_available) {
    format_bytes(g_sys_metrics.psram_free, psram_str, sizeof(psram_str));
    Serial.printf("PSRAM: %s\n", psram_str);
  } else {
    Serial.println("No PSRAM");
  }
}

// ────────────────────────────────────────────────────────────────────────────

// Build a visual temperature meter bar
// Range: -10C to 100C mapped to 40 characters
static void print_temp_meter(float temp_c) {
  const int meter_width = 50;
  const float min_temp = -10.0f;
  const float max_temp = 100.0f;

  // Calculate position (0 to meter_width)
  int pos = (int)((temp_c - min_temp) / (max_temp - min_temp) * meter_width);
  if (pos < 0) pos = 0;
  if (pos > meter_width) pos = meter_width;

  // Zone boundaries (in character positions)
  int cold_crit_pos = (int)((TEMP_COLD_CRITICAL - min_temp) / (max_temp - min_temp) * meter_width);
  int cold_warn_pos = (int)((TEMP_COLD_WARNING - min_temp) / (max_temp - min_temp) * meter_width);
  int hot_warn_pos = (int)((TEMP_HOT_WARNING - min_temp) / (max_temp - min_temp) * meter_width);
  int hot_crit_pos = (int)((TEMP_HOT_CRITICAL - min_temp) / (max_temp - min_temp) * meter_width);

  Serial.print("  -10C                                                      100C\n");
  Serial.print("   |                                                          |\n");
  Serial.print("   [");

  for (int i = 0; i < meter_width; i++) {
    char c;
    if (i < cold_crit_pos) {
      c = '!';  // Critical cold zone
    } else if (i < cold_warn_pos) {
      c = '*';  // Warning cold zone
    } else if (i < hot_warn_pos) {
      c = '-';  // Normal zone
    } else if (i < hot_crit_pos) {
      c = '*';  // Warning hot zone
    } else {
      c = '!';  // Critical hot zone
    }

    if (i == pos) {
      Serial.print('#');  // Current temperature marker
    } else {
      Serial.print(c);
    }
  }

  Serial.println("]");
  Serial.println("   |!!**|----------- NORMAL -----------|****!!!!!|");
  Serial.println("   CRIT WARN                            WARN CRIT");
  Serial.println("   COLD COLD                            HOT  HOT");
}

// ────────────────────────────────────────────────────────────────────────────

// Print a memory bar
static void print_memory_bar(const char* label, uint32_t used, uint32_t total, int bar_width = 30) {
  float pct = (float)used / total * 100.0f;
  int filled = (int)(pct / 100.0f * bar_width);
  if (filled > bar_width) filled = bar_width;

  char used_str[16], total_str[16];
  format_bytes(used, used_str, sizeof(used_str));
  format_bytes(total, total_str, sizeof(total_str));

  Serial.printf("  %-8s [", label);
  for (int i = 0; i < bar_width; i++) {
    Serial.print(i < filled ? '#' : '-');
  }
  Serial.printf("] %5.1f%% (%s / %s)\n", pct, used_str, total_str);
}

// ────────────────────────────────────────────────────────────────────────────

void print_status() {
  char buf1[32], buf2[32];

  float temp_c = g_sys_metrics.temp_celsius;
  float temp_f = celsius_to_fahrenheit(temp_c);
  float temp_min_f = celsius_to_fahrenheit(g_sys_metrics.temp_min);
  float temp_max_f = celsius_to_fahrenheit(g_sys_metrics.temp_max);
  float temp_avg_f = celsius_to_fahrenheit(g_sys_metrics.temp_avg);

  Serial.println();
  Serial.println("================================================================================");
  Serial.println("                        SYSTEM MONITOR - ESP32-S3                              ");
  Serial.println("================================================================================");

  // ─────────────────────────────────────────────────────────────────────────
  // DEVICE INFORMATION
  // ─────────────────────────────────────────────────────────────────────────
  Serial.println();
  Serial.println("--- DEVICE INFORMATION --------------------------------------------------------");

  Serial.printf("  Chip Model     : %s (rev %u)\n", g_sys_metrics.chip_model, g_sys_metrics.chip_revision);
  Serial.printf("  CPU            : %u cores @ %u MHz\n", g_sys_metrics.chip_cores, g_sys_metrics.cpu_freq_mhz);
  Serial.printf("  Flash Size     : %s\n", format_bytes(g_sys_metrics.flash_size, buf1, sizeof(buf1)));
  Serial.printf("  SDK Version    : %s\n", ESP.getSdkVersion());
  Serial.printf("  Sketch Size    : %s\n", format_bytes(ESP.getSketchSize(), buf1, sizeof(buf1)));
  Serial.printf("  Sketch MD5     : %s\n", ESP.getSketchMD5().c_str());

  // MAC Address
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  Serial.printf("  MAC Address    : %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  Serial.printf("  Chip ID        : %llX\n", ESP.getEfuseMac());
  Serial.printf("  Flash Mode     : %s\n",
    ESP.getFlashChipMode() == FM_QIO ? "QIO" :
    ESP.getFlashChipMode() == FM_QOUT ? "QOUT" :
    ESP.getFlashChipMode() == FM_DIO ? "DIO" :
    ESP.getFlashChipMode() == FM_DOUT ? "DOUT" : "Unknown");
  Serial.printf("  Flash Speed    : %u MHz\n", ESP.getFlashChipSpeed() / 1000000);

  // ─────────────────────────────────────────────────────────────────────────
  // TEMPERATURE
  // ─────────────────────────────────────────────────────────────────────────
  Serial.println();
  Serial.println("--- TEMPERATURE (Internal Chip Sensor) ----------------------------------------");
  Serial.println();

  // Current temperature with big display
  Serial.printf("  CURRENT:  %.1f C  /  %.1f F    [%s]\n",
                temp_c, temp_f, temp_state_name(g_sys_metrics.temp_state));
  Serial.println();

  // Visual meter
  print_temp_meter(temp_c);

  Serial.println();
  Serial.printf("  Session Min  : %6.1f C  / %6.1f F\n", g_sys_metrics.temp_min, temp_min_f);
  Serial.printf("  Session Max  : %6.1f C  / %6.1f F\n", g_sys_metrics.temp_max, temp_max_f);
  Serial.printf("  Average (EMA): %6.1f C  / %6.1f F\n", g_sys_metrics.temp_avg, temp_avg_f);
  Serial.printf("  Readings     : %u\n", g_sys_metrics.temp_readings);

  // ─────────────────────────────────────────────────────────────────────────
  // TEMPERATURE ZONES REFERENCE
  // ─────────────────────────────────────────────────────────────────────────
  Serial.println();
  Serial.println("  TEMPERATURE ZONES:");
  Serial.println("  +-------------+------------------+------------------+------------------------+");
  Serial.println("  | Zone        | Celsius          | Fahrenheit       | Status                 |");
  Serial.println("  +-------------+------------------+------------------+------------------------+");
  Serial.printf("  | CRIT COLD   | < %5.1f C        | < %5.1f F        | DANGER - May damage!   |\n",
                TEMP_COLD_CRITICAL, celsius_to_fahrenheit(TEMP_COLD_CRITICAL));
  Serial.printf("  | WARN COLD   | %5.1f - %4.1f C  | %5.1f - %5.1f F | Caution - Too cold     |\n",
                TEMP_COLD_CRITICAL, TEMP_COLD_WARNING,
                celsius_to_fahrenheit(TEMP_COLD_CRITICAL), celsius_to_fahrenheit(TEMP_COLD_WARNING));
  Serial.printf("  | NORMAL      | %5.1f - %4.1f C  | %5.1f - %5.1f F | OK - Optimal range     |\n",
                TEMP_COLD_WARNING, TEMP_HOT_WARNING,
                celsius_to_fahrenheit(TEMP_COLD_WARNING), celsius_to_fahrenheit(TEMP_HOT_WARNING));
  Serial.printf("  | WARN HOT    | %5.1f - %4.1f C  | %5.1f - %5.1f F | Caution - Getting hot  |\n",
                TEMP_HOT_WARNING, TEMP_HOT_CRITICAL,
                celsius_to_fahrenheit(TEMP_HOT_WARNING), celsius_to_fahrenheit(TEMP_HOT_CRITICAL));
  Serial.printf("  | CRIT HOT    | > %5.1f C        | > %5.1f F        | DANGER - Throttle/halt |\n",
                TEMP_HOT_CRITICAL, celsius_to_fahrenheit(TEMP_HOT_CRITICAL));
  Serial.println("  +-------------+------------------+------------------+------------------------+");

  Serial.println();
  Serial.printf("  Alert History: %u cold alerts, %u hot alerts\n",
                g_sys_metrics.cold_alerts, g_sys_metrics.hot_alerts);

  // ─────────────────────────────────────────────────────────────────────────
  // MEMORY
  // ─────────────────────────────────────────────────────────────────────────
  Serial.println();
  Serial.println("--- MEMORY --------------------------------------------------------------------");
  Serial.println();

  // Internal Heap
  Serial.println("  INTERNAL HEAP (SRAM):");
  uint32_t heap_used = g_sys_metrics.heap_total - g_sys_metrics.heap_free;
  print_memory_bar("Used", heap_used, g_sys_metrics.heap_total);
  Serial.printf("  Free Now     : %s\n", format_bytes(g_sys_metrics.heap_free, buf1, sizeof(buf1)));
  Serial.printf("  Min Free Ever: %s (high water mark)\n", format_bytes(g_sys_metrics.heap_min_free, buf1, sizeof(buf1)));
  Serial.printf("  Largest Block: %s (max single allocation)\n", format_bytes(g_sys_metrics.heap_largest_block, buf1, sizeof(buf1)));

  // PSRAM
  Serial.println();
  if (g_sys_metrics.psram_available) {
    Serial.println("  PSRAM (External SPI RAM):");
    uint32_t psram_used = g_sys_metrics.psram_total - g_sys_metrics.psram_free;
    print_memory_bar("Used", psram_used, g_sys_metrics.psram_total);
    Serial.printf("  Free Now     : %s\n", format_bytes(g_sys_metrics.psram_free, buf1, sizeof(buf1)));
    Serial.printf("  Min Free Ever: %s\n", format_bytes(g_sys_metrics.psram_min_free, buf1, sizeof(buf1)));
  } else {
    Serial.println("  PSRAM: Not detected or not enabled");
  }

  // Sketch memory
  Serial.println();
  Serial.println("  FLASH PROGRAM MEMORY:");
  uint32_t sketch_used = ESP.getSketchSize();
  uint32_t sketch_total = ESP.getFreeSketchSpace() + sketch_used;
  print_memory_bar("Used", sketch_used, sketch_total);

  // ─────────────────────────────────────────────────────────────────────────
  // RUNTIME
  // ─────────────────────────────────────────────────────────────────────────
  Serial.println();
  Serial.println("--- RUNTIME -------------------------------------------------------------------");
  Serial.printf("  Uptime       : %s (%u seconds)\n",
                format_uptime(g_sys_metrics.uptime_sec, buf1, sizeof(buf1)),
                g_sys_metrics.uptime_sec);
  Serial.printf("  CPU Cycles   : %llu\n", (unsigned long long)ESP.getCycleCount());
  Serial.printf("  Reset Reason : %s\n",
    esp_reset_reason() == ESP_RST_POWERON ? "Power-on" :
    esp_reset_reason() == ESP_RST_EXT ? "External" :
    esp_reset_reason() == ESP_RST_SW ? "Software" :
    esp_reset_reason() == ESP_RST_PANIC ? "Panic" :
    esp_reset_reason() == ESP_RST_INT_WDT ? "Interrupt WDT" :
    esp_reset_reason() == ESP_RST_TASK_WDT ? "Task WDT" :
    esp_reset_reason() == ESP_RST_WDT ? "Other WDT" :
    esp_reset_reason() == ESP_RST_DEEPSLEEP ? "Deep Sleep" :
    esp_reset_reason() == ESP_RST_BROWNOUT ? "Brownout" :
    esp_reset_reason() == ESP_RST_SDIO ? "SDIO" : "Unknown");

  // ─────────────────────────────────────────────────────────────────────────
  // NOTES
  // ─────────────────────────────────────────────────────────────────────────
  Serial.println();
  Serial.println("--- NOTES ---------------------------------------------------------------------");
  Serial.println("  * Temperature is chip internal (die temp), typically 10-20C above ambient");
  Serial.println("  * ESP32-S3 has NO built-in humidity sensor");
  Serial.println("  * For humidity, add external sensor: DHT22, BME280, SHT31, etc.");
  Serial.println("  * PSRAM extends available RAM via external SPI chip (8MB on XIAO Sense)");
  Serial.println();
  Serial.println("================================================================================");
  Serial.println();
}

// ────────────────────────────────────────────────────────────────────────────

size_t get_json(char* buf, size_t buf_size) {
  char uptime_str[16];
  format_uptime(g_sys_metrics.uptime_sec, uptime_str, sizeof(uptime_str));

  float heap_pct = (float)(g_sys_metrics.heap_total - g_sys_metrics.heap_free) / g_sys_metrics.heap_total * 100.0f;

  // Get MAC address
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // Calculate Fahrenheit values
  float temp_f = celsius_to_fahrenheit(g_sys_metrics.temp_celsius);
  float temp_min_f = celsius_to_fahrenheit(g_sys_metrics.temp_min);
  float temp_max_f = celsius_to_fahrenheit(g_sys_metrics.temp_max);
  float temp_avg_f = celsius_to_fahrenheit(g_sys_metrics.temp_avg);

  int len = snprintf(buf, buf_size,
    "{"
    "\"temperature\":{"
      "\"celsius\":{\"current\":%.1f,\"min\":%.1f,\"max\":%.1f,\"avg\":%.1f},"
      "\"fahrenheit\":{\"current\":%.1f,\"min\":%.1f,\"max\":%.1f,\"avg\":%.1f},"
      "\"state\":\"%s\","
      "\"alert_active\":%s,"
      "\"thresholds\":{"
        "\"cold_warn_c\":%.1f,\"cold_warn_f\":%.1f,"
        "\"cold_crit_c\":%.1f,\"cold_crit_f\":%.1f,"
        "\"hot_warn_c\":%.1f,\"hot_warn_f\":%.1f,"
        "\"hot_crit_c\":%.1f,\"hot_crit_f\":%.1f"
      "},"
      "\"alerts\":{\"cold\":%u,\"hot\":%u},"
      "\"readings\":%u"
    "},"
    "\"memory\":{"
      "\"heap\":{\"total\":%u,\"free\":%u,\"min_free\":%u,\"largest_block\":%u,\"used_pct\":%.1f},"
      "\"psram\":{\"available\":%s,\"total\":%u,\"free\":%u,\"min_free\":%u},"
      "\"sketch\":{\"size\":%u,\"free\":%u}"
    "},"
    "\"device\":{"
      "\"model\":\"%s\","
      "\"revision\":%u,"
      "\"cores\":%u,"
      "\"freq_mhz\":%u,"
      "\"flash_size\":%u,"
      "\"flash_speed_mhz\":%u,"
      "\"mac\":\"%s\","
      "\"chip_id\":\"%llX\","
      "\"sdk_version\":\"%s\","
      "\"reset_reason\":\"%s\""
    "},"
    "\"uptime\":{\"seconds\":%u,\"formatted\":\"%s\"},"
    "\"humidity_available\":false"
    "}",
    // Celsius
    g_sys_metrics.temp_celsius,
    g_sys_metrics.temp_min,
    g_sys_metrics.temp_max,
    g_sys_metrics.temp_avg,
    // Fahrenheit
    temp_f, temp_min_f, temp_max_f, temp_avg_f,
    temp_state_name(g_sys_metrics.temp_state),
    g_sys_metrics.alert_active ? "true" : "false",
    // Thresholds C and F
    TEMP_COLD_WARNING, celsius_to_fahrenheit(TEMP_COLD_WARNING),
    TEMP_COLD_CRITICAL, celsius_to_fahrenheit(TEMP_COLD_CRITICAL),
    TEMP_HOT_WARNING, celsius_to_fahrenheit(TEMP_HOT_WARNING),
    TEMP_HOT_CRITICAL, celsius_to_fahrenheit(TEMP_HOT_CRITICAL),
    g_sys_metrics.cold_alerts,
    g_sys_metrics.hot_alerts,
    g_sys_metrics.temp_readings,
    // Memory
    g_sys_metrics.heap_total,
    g_sys_metrics.heap_free,
    g_sys_metrics.heap_min_free,
    g_sys_metrics.heap_largest_block,
    heap_pct,
    g_sys_metrics.psram_available ? "true" : "false",
    g_sys_metrics.psram_total,
    g_sys_metrics.psram_free,
    g_sys_metrics.psram_min_free,
    ESP.getSketchSize(),
    ESP.getFreeSketchSpace(),
    // Device
    g_sys_metrics.chip_model,
    g_sys_metrics.chip_revision,
    g_sys_metrics.chip_cores,
    g_sys_metrics.cpu_freq_mhz,
    g_sys_metrics.flash_size,
    ESP.getFlashChipSpeed() / 1000000,
    mac_str,
    (unsigned long long)ESP.getEfuseMac(),
    ESP.getSdkVersion(),
    esp_reset_reason() == ESP_RST_POWERON ? "power_on" :
    esp_reset_reason() == ESP_RST_SW ? "software" :
    esp_reset_reason() == ESP_RST_PANIC ? "panic" :
    esp_reset_reason() == ESP_RST_WDT ? "watchdog" :
    esp_reset_reason() == ESP_RST_BROWNOUT ? "brownout" : "other",
    // Uptime
    g_sys_metrics.uptime_sec,
    uptime_str
  );

  return (len > 0 && len < (int)buf_size) ? len : 0;
}

} // namespace sys_monitor

#endif // FEATURE_SYS_MONITOR

#endif // SECURACV_SYS_MONITOR_H
