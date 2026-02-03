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

void print_status_line() {
  char heap_str[16], psram_str[16], uptime_str[16];

  format_bytes(g_sys_metrics.heap_free, heap_str, sizeof(heap_str));
  format_uptime(g_sys_metrics.uptime_sec, uptime_str, sizeof(uptime_str));

  Serial.printf("[SYS] %s | Temp: %.1fC [%s] | Heap: %s | ",
                uptime_str,
                g_sys_metrics.temp_celsius,
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

void print_status() {
  char buf1[16], buf2[16], buf3[16];

  Serial.println();
  Serial.println("+---------------------------------------------------------+");
  Serial.println("|              SYSTEM MONITOR                             |");
  Serial.println("+---------------------------------------------------------+");

  // Chip Info
  Serial.println("| CHIP INFO                                               |");
  Serial.println("|  ----------------------------------------------------- |");
  Serial.printf("|  Model       : %-40s |\n", g_sys_metrics.chip_model);
  Serial.printf("|  Revision    : %u                                        |\n", g_sys_metrics.chip_revision);
  Serial.printf("|  Cores       : %u @ %u MHz                               |\n",
                g_sys_metrics.chip_cores, g_sys_metrics.cpu_freq_mhz);
  Serial.printf("|  Flash       : %-40s |\n", format_bytes(g_sys_metrics.flash_size, buf1, sizeof(buf1)));

  // Temperature
  Serial.println("|                                                         |");
  Serial.println("| TEMPERATURE (Chip Internal)                             |");
  Serial.println("|  ----------------------------------------------------- |");
  Serial.printf("|  Current     : %.1f C                                   |\n", g_sys_metrics.temp_celsius);
  Serial.printf("|  State       : %-40s |\n", temp_state_name(g_sys_metrics.temp_state));
  Serial.printf("|  Min/Max     : %.1f C / %.1f C                          |\n",
                g_sys_metrics.temp_min, g_sys_metrics.temp_max);
  Serial.printf("|  Average     : %.1f C (EMA)                             |\n", g_sys_metrics.temp_avg);
  Serial.printf("|  Readings    : %u                                       |\n", g_sys_metrics.temp_readings);

  // Alert thresholds
  Serial.println("|                                                         |");
  Serial.println("| ALERT THRESHOLDS                                        |");
  Serial.println("|  ----------------------------------------------------- |");
  Serial.printf("|  Cold Warn   : < %.1f C                                 |\n", TEMP_COLD_WARNING);
  Serial.printf("|  Cold Crit   : < %.1f C                                 |\n", TEMP_COLD_CRITICAL);
  Serial.printf("|  Hot Warn    : > %.1f C                                |\n", TEMP_HOT_WARNING);
  Serial.printf("|  Hot Crit    : > %.1f C                                |\n", TEMP_HOT_CRITICAL);
  Serial.printf("|  Cold Alerts : %u                                       |\n", g_sys_metrics.cold_alerts);
  Serial.printf("|  Hot Alerts  : %u                                       |\n", g_sys_metrics.hot_alerts);

  // Memory
  Serial.println("|                                                         |");
  Serial.println("| MEMORY                                                  |");
  Serial.println("|  ----------------------------------------------------- |");
  Serial.printf("|  Heap Total  : %-40s |\n", format_bytes(g_sys_metrics.heap_total, buf1, sizeof(buf1)));
  Serial.printf("|  Heap Free   : %-40s |\n", format_bytes(g_sys_metrics.heap_free, buf1, sizeof(buf1)));
  Serial.printf("|  Heap Min    : %-40s |\n", format_bytes(g_sys_metrics.heap_min_free, buf1, sizeof(buf1)));
  Serial.printf("|  Largest Blk : %-40s |\n", format_bytes(g_sys_metrics.heap_largest_block, buf1, sizeof(buf1)));

  float heap_pct = (float)(g_sys_metrics.heap_total - g_sys_metrics.heap_free) / g_sys_metrics.heap_total * 100.0f;
  Serial.printf("|  Heap Used   : %.1f%%                                   |\n", heap_pct);

  if (g_sys_metrics.psram_available) {
    Serial.println("|                                                         |");
    Serial.println("| PSRAM (External)                                        |");
    Serial.println("|  ----------------------------------------------------- |");
    Serial.printf("|  PSRAM Total : %-40s |\n", format_bytes(g_sys_metrics.psram_total, buf1, sizeof(buf1)));
    Serial.printf("|  PSRAM Free  : %-40s |\n", format_bytes(g_sys_metrics.psram_free, buf1, sizeof(buf1)));
    Serial.printf("|  PSRAM Min   : %-40s |\n", format_bytes(g_sys_metrics.psram_min_free, buf1, sizeof(buf1)));

    float psram_pct = (float)(g_sys_metrics.psram_total - g_sys_metrics.psram_free) / g_sys_metrics.psram_total * 100.0f;
    Serial.printf("|  PSRAM Used  : %.1f%%                                  |\n", psram_pct);
  } else {
    Serial.println("|                                                         |");
    Serial.println("| PSRAM: Not available                                    |");
  }

  // Uptime
  Serial.println("|                                                         |");
  Serial.printf("| Uptime: %-47s |\n", format_uptime(g_sys_metrics.uptime_sec, buf1, sizeof(buf1)));

  // Humidity note
  Serial.println("|                                                         |");
  Serial.println("| Note: ESP32-S3 has no built-in humidity sensor.         |");
  Serial.println("| External sensor (DHT22/BME280) required for humidity.   |");
  Serial.println("+---------------------------------------------------------+");
  Serial.println();
}

// ────────────────────────────────────────────────────────────────────────────

size_t get_json(char* buf, size_t buf_size) {
  char uptime_str[16];
  format_uptime(g_sys_metrics.uptime_sec, uptime_str, sizeof(uptime_str));

  float heap_pct = (float)(g_sys_metrics.heap_total - g_sys_metrics.heap_free) / g_sys_metrics.heap_total * 100.0f;

  int len = snprintf(buf, buf_size,
    "{"
    "\"temperature\":{"
      "\"current\":%.1f,"
      "\"min\":%.1f,"
      "\"max\":%.1f,"
      "\"avg\":%.1f,"
      "\"state\":\"%s\","
      "\"alert_active\":%s,"
      "\"thresholds\":{"
        "\"cold_warn\":%.1f,"
        "\"cold_crit\":%.1f,"
        "\"hot_warn\":%.1f,"
        "\"hot_crit\":%.1f"
      "},"
      "\"alerts\":{\"cold\":%u,\"hot\":%u}"
    "},"
    "\"memory\":{"
      "\"heap\":{\"total\":%u,\"free\":%u,\"min_free\":%u,\"largest_block\":%u,\"used_pct\":%.1f},"
      "\"psram\":{\"available\":%s,\"total\":%u,\"free\":%u,\"min_free\":%u}"
    "},"
    "\"chip\":{"
      "\"model\":\"%s\","
      "\"revision\":%u,"
      "\"cores\":%u,"
      "\"freq_mhz\":%u,"
      "\"flash\":%u"
    "},"
    "\"uptime\":{\"seconds\":%u,\"formatted\":\"%s\"},"
    "\"humidity_available\":false"
    "}",
    g_sys_metrics.temp_celsius,
    g_sys_metrics.temp_min,
    g_sys_metrics.temp_max,
    g_sys_metrics.temp_avg,
    temp_state_name(g_sys_metrics.temp_state),
    g_sys_metrics.alert_active ? "true" : "false",
    TEMP_COLD_WARNING,
    TEMP_COLD_CRITICAL,
    TEMP_HOT_WARNING,
    TEMP_HOT_CRITICAL,
    g_sys_metrics.cold_alerts,
    g_sys_metrics.hot_alerts,
    g_sys_metrics.heap_total,
    g_sys_metrics.heap_free,
    g_sys_metrics.heap_min_free,
    g_sys_metrics.heap_largest_block,
    heap_pct,
    g_sys_metrics.psram_available ? "true" : "false",
    g_sys_metrics.psram_total,
    g_sys_metrics.psram_free,
    g_sys_metrics.psram_min_free,
    g_sys_metrics.chip_model,
    g_sys_metrics.chip_revision,
    g_sys_metrics.chip_cores,
    g_sys_metrics.cpu_freq_mhz,
    g_sys_metrics.flash_size,
    g_sys_metrics.uptime_sec,
    uptime_str
  );

  return (len > 0 && len < (int)buf_size) ? len : 0;
}

} // namespace sys_monitor

#endif // FEATURE_SYS_MONITOR

#endif // SECURACV_SYS_MONITOR_H
