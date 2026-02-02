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

#endif // SECURACV_HEALTH_LOG_H
