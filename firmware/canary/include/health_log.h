/*
 * SecuraCV Canary — health_logging:: bridge for the shared CSI HAL
 *
 * firmware/common/csi/src/csi_hal.cpp routes its diagnostics through
 * `#if __has_include("health_log.h")` -> health_logging::log() / logf(),
 * and falls back to Serial.printf when no such header is on the include
 * path. The canary-wap sketch has a full health_log.h; this file is the
 * canary product's answer to the same probe — the smallest surface that
 * turns those calls into log_health() entries in this product's health
 * log, so "CSI start deferred" and "CSI silent for N ms" land where every
 * other canary diagnostic does instead of only on the serial console. That
 * is where the former lib/securacv_csi HAL copy logged them (roadmap 22).
 *
 * Only the names csi_hal.cpp uses are provided. CAT_RF maps to
 * LOG_CAT_SENSOR — this product's category for CSI. Nothing else in the
 * canary tree includes this header; delete it and the HAL degrades to
 * Serial.printf, nothing breaks.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_CANARY_HEALTH_LOG_BRIDGE_H
#define SECURACV_CANARY_HEALTH_LOG_BRIDGE_H

#include <stdarg.h>
#include <stdio.h>

#include "log_level.h"
#include "securacv_witness.h"  /* log_health() */

namespace health_logging {

constexpr LogLevel    LEVEL_INFO    = LOG_LEVEL_INFO;
constexpr LogLevel    LEVEL_WARNING = LOG_LEVEL_WARNING;
constexpr LogCategory CAT_RF        = LOG_CAT_SENSOR;

inline void log(LogLevel level, LogCategory category, const char* message) {
  log_health(level, category, message, nullptr);
}

inline void logf(LogLevel level, LogCategory category, const char* fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_health(level, category, buf, nullptr);
}

}  /* namespace health_logging */

#endif  /* SECURACV_CANARY_HEALTH_LOG_BRIDGE_H */
