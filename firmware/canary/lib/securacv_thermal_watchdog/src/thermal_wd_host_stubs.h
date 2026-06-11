/*
 * SecuraCV Canary — host-build stubs for the thermal watchdog test
 *
 * Included by securacv_thermal_watchdog.cpp ONLY under
 * -DCSI_TEST_HOST_BUILD (the repo's host-test convention; see the
 * "Mesh + Scout Host Tests" CI job). The test binary owns the clock,
 * the sensor, the NVS map, and the health-log capture; this header
 * just declares the surface the module compiles against on a desktop
 * toolchain instead of Arduino/IDF.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef THERMAL_WD_HOST_STUBS_H
#define THERMAL_WD_HOST_STUBS_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include <map>
#include <string>
#include <vector>

/* ── Thermal thresholds — device builds single-source these from
 *    canary_config.h; the host harness pins the same values the tests
 *    assert against. ── */
#ifndef THERMAL_THROTTLE_TEMP_C
#define THERMAL_THROTTLE_TEMP_C   70
#define THERMAL_PAUSE_TEMP_C      80
#define THERMAL_RECOVER_MARGIN_C   5
#endif

/* ── Arduino time ── */
uint32_t millis();

/* ── Shared thermal provider (securacv_thermal) ── */
extern "C" bool thermal_read_die_c(float* out_c);

/* ── Health logging (securacv_witness / log_level.h subset) ── */
enum LogLevel : uint8_t {
  LOG_LEVEL_DEBUG    = 0,
  LOG_LEVEL_INFO     = 1,
  LOG_LEVEL_NOTICE   = 2,
  LOG_LEVEL_WARNING  = 3,
  LOG_LEVEL_ERROR    = 4,
  LOG_LEVEL_CRITICAL = 5,
  LOG_LEVEL_ALERT    = 6,
  LOG_LEVEL_TAMPER   = 7
};

enum LogCategory : uint8_t {
  LOG_CAT_SYSTEM = 0,
  LOG_CAT_SENSOR = 6
};

void log_health(LogLevel level, LogCategory category,
                const char* message, const char* detail = nullptr);

/* Captured log entries, inspectable by the test. */
struct HostLogEntry {
  LogLevel    level;
  std::string message;
  std::string detail;
};
extern std::vector<HostLogEntry> g_host_health_log;

/* ── Serial ── */
struct HostSerial {
  void printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
  }
  void println(const char* s) { fputs(s, stdout); fputc('\n', stdout); }
};
extern HostSerial Serial;

/* ── Preferences (NVS) — in-memory map, write-count instrumented ── */
extern std::map<std::string, int32_t> g_host_nvs;
extern uint32_t g_host_nvs_write_count;

class Preferences {
 public:
  bool begin(const char* /*ns*/, bool /*read_only*/ = false) { return true; }
  void end() {}

  int8_t getChar(const char* key, int8_t def = 0) {
    auto it = g_host_nvs.find(key);
    return it == g_host_nvs.end() ? def : (int8_t)it->second;
  }
  uint32_t getUInt(const char* key, uint32_t def = 0) {
    auto it = g_host_nvs.find(key);
    return it == g_host_nvs.end() ? def : (uint32_t)it->second;
  }
  size_t putChar(const char* key, int8_t v) {
    g_host_nvs[key] = v;
    g_host_nvs_write_count++;
    return 1;
  }
  size_t putUInt(const char* key, uint32_t v) {
    g_host_nvs[key] = (int32_t)v;
    g_host_nvs_write_count++;
    return 4;
  }
};

#endif /* THERMAL_WD_HOST_STUBS_H */
