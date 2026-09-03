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
 * Events at SCV_LOG_DEBUG are typically not stored.
 * Events at SCV_LOG_WARNING and above may require user acknowledgment.
 * Events at SCV_LOG_TAMPER are security-related and always stored.
 *
 * Example usage:
 *   health_log(SCV_LOG_INFO, SCV_CAT_NETWORK, "chirp: new session");
 *   health_log(SCV_LOG_WARNING, SCV_CAT_CRYPTO, "key derivation slow");
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
 *   log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE connected", "AA:BB:CC:DD:EE:FF");
 *   log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH, "Pairing timeout", nullptr);
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
constexpr LogLevel LEVEL_DEBUG    = ::SCV_LOG_DEBUG;
constexpr LogLevel LEVEL_INFO     = ::SCV_LOG_INFO;
constexpr LogLevel LEVEL_NOTICE   = ::SCV_LOG_NOTICE;
constexpr LogLevel LEVEL_WARNING  = ::SCV_LOG_WARNING;
constexpr LogLevel LEVEL_ERROR    = ::SCV_LOG_ERROR;
constexpr LogLevel LEVEL_CRITICAL = ::SCV_LOG_CRITICAL;
constexpr LogLevel LEVEL_ALERT    = ::SCV_LOG_ALERT;
constexpr LogLevel LEVEL_TAMPER   = ::SCV_LOG_TAMPER;

// Re-export log category constants for namespace-qualified access
constexpr LogCategory CAT_SYSTEM    = ::SCV_CAT_SYSTEM;
constexpr LogCategory CAT_CRYPTO    = ::SCV_CAT_CRYPTO;
constexpr LogCategory CAT_CHAIN     = ::SCV_CAT_CHAIN;
constexpr LogCategory CAT_GPS       = ::SCV_CAT_GPS;
constexpr LogCategory CAT_STORAGE   = ::SCV_CAT_STORAGE;
constexpr LogCategory CAT_NETWORK   = ::SCV_CAT_NETWORK;
constexpr LogCategory CAT_SENSOR    = ::SCV_CAT_SENSOR;
constexpr LogCategory CAT_USER      = ::SCV_CAT_USER;
constexpr LogCategory CAT_WITNESS   = ::SCV_CAT_WITNESS;
constexpr LogCategory CAT_MESH      = ::SCV_CAT_MESH;
constexpr LogCategory CAT_BLUETOOTH = ::SCV_CAT_BLUETOOTH;
constexpr LogCategory CAT_RF        = ::SCV_CAT_RF;

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
// Buffer size of 128 matches the health log entry's message field
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
