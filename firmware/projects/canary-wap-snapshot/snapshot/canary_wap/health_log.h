/*
 * SecuraCV Canary — Health Log Interface
 *
 * Logging interface for system health and diagnostic events.
 * This header provides the health_log() function declaration
 * for use by subsystems like chirp_channel.
 *
 * The actual implementation is provided by the main firmware
 * (securacv_canary_wap.ino) which maintains the ring buffer
 * and handles log persistence.
 */

#ifndef SECURACV_HEALTH_LOG_H
#define SECURACV_HEALTH_LOG_H

#include <Arduino.h>
#include <cstdarg>
#include <cstdio>
#include "log_level.h"

// ════════════════════════════════════════════════════════════════════════════
// HEALTH LOG FUNCTION
// ════════════════════════════════════════════════════════════════════════════

/*
 * Log a health/diagnostic event.
 *
 * @param level    Severity level (see log_level.h)
 * @param category Event category (see log_level.h)
 * @param message  Human-readable message describing the event
 *
 * Events at LOG_LEVEL_DEBUG are typically not stored.
 * Events at LOG_LEVEL_WARNING and above may require user acknowledgment.
 * Events at LOG_LEVEL_TAMPER are security-related and always stored.
 *
 * Example usage:
 *   health_log(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "chirp: new session");
 *   health_log(LOG_LEVEL_WARNING, LOG_CAT_CRYPTO, "key derivation slow");
 */
void health_log(LogLevel level, LogCategory category, const char* message);

/*
 * Log a health/diagnostic event with optional detail.
 *
 * @param level    Severity level (see log_level.h)
 * @param category Event category (see log_level.h)
 * @param message  Human-readable message describing the event
 * @param detail   Optional additional detail (can be nullptr)
 *
 * Example usage:
 *   log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "BLE connected", "AA:BB:CC:DD:EE:FF");
 *   log_health(LOG_LEVEL_WARNING, LOG_CAT_BLUETOOTH, "Pairing timeout", nullptr);
 */
void log_health(LogLevel level, LogCategory category, const char* message, const char* detail);

// ════════════════════════════════════════════════════════════════════════════
// HEALTH_LOGGING NAMESPACE (for RF presence and other modules)
// ════════════════════════════════════════════════════════════════════════════

/*
 * Namespace-based interface for health logging.
 * Provides a unified API consistent with other module conventions.
 * Note: Named health_logging (not health_log) to avoid conflict with the
 * global health_log() function.
 */
namespace health_logging {

// Re-export log level constants for namespace-qualified access
// Note: LOG_LEVEL_WARN alias removed to avoid conflict with NimBLE's macro
constexpr LogLevel LEVEL_DEBUG    = ::LOG_LEVEL_DEBUG;
constexpr LogLevel LEVEL_INFO     = ::LOG_LEVEL_INFO;
constexpr LogLevel LEVEL_NOTICE   = ::LOG_LEVEL_NOTICE;
constexpr LogLevel LEVEL_WARNING  = ::LOG_LEVEL_WARNING;
constexpr LogLevel LEVEL_ERROR    = ::LOG_LEVEL_ERROR;
constexpr LogLevel LEVEL_CRITICAL = ::LOG_LEVEL_CRITICAL;
constexpr LogLevel LEVEL_ALERT    = ::LOG_LEVEL_ALERT;
constexpr LogLevel LEVEL_TAMPER   = ::LOG_LEVEL_TAMPER;

// Re-export log category constants for namespace-qualified access
constexpr LogCategory CAT_SYSTEM    = ::LOG_CAT_SYSTEM;
constexpr LogCategory CAT_CRYPTO    = ::LOG_CAT_CRYPTO;
constexpr LogCategory CAT_CHAIN     = ::LOG_CAT_CHAIN;
constexpr LogCategory CAT_GPS       = ::LOG_CAT_GPS;
constexpr LogCategory CAT_STORAGE   = ::LOG_CAT_STORAGE;
constexpr LogCategory CAT_NETWORK   = ::LOG_CAT_NETWORK;
constexpr LogCategory CAT_SENSOR    = ::LOG_CAT_SENSOR;
constexpr LogCategory CAT_USER      = ::LOG_CAT_USER;
constexpr LogCategory CAT_WITNESS   = ::LOG_CAT_WITNESS;
constexpr LogCategory CAT_MESH      = ::LOG_CAT_MESH;
constexpr LogCategory CAT_BLUETOOTH = ::LOG_CAT_BLUETOOTH;
constexpr LogCategory CAT_RF        = ::LOG_CAT_RF;

// Log function wrapper
// Delegates to the global health_log() function
inline void log(LogLevel level, LogCategory category, const char* message) {
  ::health_log(level, category, message);
}

// Log function wrapper with detail
// Delegates to the global log_health() function
inline void log(LogLevel level, LogCategory category, const char* message, const char* detail) {
  ::log_health(level, category, message, detail);
}

// Variadic log function for printf-style formatted messages
// Uses a stack buffer to format the message before logging
// Buffer size of 128 matches health_log_entry_t.message in common/health/health_log.h
inline void logf(LogLevel level, LogCategory category, const char* fmt, ...) {
  char buffer[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  ::health_log(level, category, buffer);
}

} // namespace health_logging

#endif // SECURACV_HEALTH_LOG_H
