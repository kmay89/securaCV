/*
 * SecuraCV Canary — Log Level Definitions
 * 
 * Severity levels for system health and diagnostic logging.
 * Compatible with PWK event_contract.md severity classification.
 */

#ifndef SECURACV_LOG_LEVEL_H
#define SECURACV_LOG_LEVEL_H

#include <stdint.h>

// ════════════════════════════════════════════════════════════════════════════
// LOG SEVERITY LEVELS
// ════════════════════════════════════════════════════════════════════════════

// NimBLE-Arduino 2.x's log_common.h defines LOG_LEVEL_DEBUG/INFO/WARN/ERROR/
// CRITICAL as object-like macros (e.g. `#define LOG_LEVEL_DEBUG (0)`). When a
// translation unit includes <NimBLEDevice.h> before this header, those macros
// clobber the enumerators below — turning `LOG_LEVEL_DEBUG = 0` into `(0) = 0`
// (a parse error) and every later use into a bare int. Undef them here so this
// LogLevel enum is the single source of truth inside our own TUs; NimBLE's own
// .c files never include this header. (#undef of an undefined macro is a no-op.)
#undef LOG_LEVEL_DEBUG
#undef LOG_LEVEL_INFO
#undef LOG_LEVEL_WARN
#undef LOG_LEVEL_ERROR
#undef LOG_LEVEL_CRITICAL

enum LogLevel : uint8_t {
  SCV_LOG_DEBUG    = 0,   // Verbose debugging (not stored by default)
  SCV_LOG_INFO     = 1,   // Normal operational events
  SCV_LOG_NOTICE   = 2,   // Notable but expected events
  SCV_LOG_WARNING  = 3,   // Potential issues requiring attention
  SCV_LOG_ERROR    = 4,   // Errors requiring review
  SCV_LOG_CRITICAL = 5,   // Critical failures affecting operation
  SCV_LOG_ALERT    = 6,   // Immediate action required
  SCV_LOG_TAMPER   = 7    // Security/integrity events (highest priority)
};

// ════════════════════════════════════════════════════════════════════════════
// LOG CATEGORIES
// ════════════════════════════════════════════════════════════════════════════

enum LogCategory : uint8_t {
  SCV_CAT_SYSTEM     = 0,   // Boot, shutdown, watchdog
  SCV_CAT_CRYPTO     = 1,   // Key generation, signing, verification
  SCV_CAT_CHAIN      = 2,   // Hash chain operations
  SCV_CAT_GPS        = 3,   // GNSS fix, satellites, time sync
  SCV_CAT_STORAGE    = 4,   // SD card, NVS operations
  SCV_CAT_NETWORK    = 5,   // WiFi, HTTP server
  SCV_CAT_SENSOR     = 6,   // PIR, tamper, environmental
  SCV_CAT_USER       = 7,   // User actions (config changes, acknowledgments)
  SCV_CAT_WITNESS    = 8,   // Witness record creation
  SCV_CAT_MESH       = 9,   // Mesh network (flock) operations
  SCV_CAT_BLUETOOTH  = 10,  // Bluetooth Low Energy operations
  SCV_CAT_RF         = 11,  // RF presence detection operations
  SCV_CAT_AUTH       = 12   // API authentication & authorization
};

// ════════════════════════════════════════════════════════════════════════════
// ACKNOWLEDGMENT STATUS
// ════════════════════════════════════════════════════════════════════════════

enum AckStatus : uint8_t {
  ACK_STATUS_UNREAD     = 0,   // Not yet reviewed
  ACK_STATUS_REVIEWED   = 1,   // Reviewed but not resolved
  ACK_STATUS_ACKNOWLEDGED = 2, // Acknowledged by user (cleared from active view)
  ACK_STATUS_ARCHIVED   = 3    // Archived (retained for audit trail)
};

// ════════════════════════════════════════════════════════════════════════════
// HELPER FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

inline const char* log_level_name(LogLevel level) {
  switch (level) {
    case SCV_LOG_DEBUG:    return "DEBUG";
    case SCV_LOG_INFO:     return "INFO";
    case SCV_LOG_NOTICE:   return "NOTICE";
    case SCV_LOG_WARNING:  return "WARN";
    case SCV_LOG_ERROR:    return "ERROR";
    case SCV_LOG_CRITICAL: return "CRIT";
    case SCV_LOG_ALERT:    return "ALERT";
    case SCV_LOG_TAMPER:   return "TAMPER";
    default:               return "???";
  }
}

inline const char* log_level_name_short(LogLevel level) {
  switch (level) {
    case SCV_LOG_DEBUG:    return "DBG";
    case SCV_LOG_INFO:     return "INF";
    case SCV_LOG_NOTICE:   return "NTC";
    case SCV_LOG_WARNING:  return "WRN";
    case SCV_LOG_ERROR:    return "ERR";
    case SCV_LOG_CRITICAL: return "CRT";
    case SCV_LOG_ALERT:    return "ALT";
    case SCV_LOG_TAMPER:   return "TMP";
    default:               return "???";
  }
}

inline const char* log_category_name(LogCategory cat) {
  switch (cat) {
    case SCV_CAT_SYSTEM:    return "SYSTEM";
    case SCV_CAT_CRYPTO:    return "CRYPTO";
    case SCV_CAT_CHAIN:     return "CHAIN";
    case SCV_CAT_GPS:       return "GPS";
    case SCV_CAT_STORAGE:   return "STORAGE";
    case SCV_CAT_NETWORK:   return "NETWORK";
    case SCV_CAT_SENSOR:    return "SENSOR";
    case SCV_CAT_USER:      return "USER";
    case SCV_CAT_WITNESS:   return "WITNESS";
    case SCV_CAT_MESH:      return "MESH";
    case SCV_CAT_BLUETOOTH: return "BLUETOOTH";
    case SCV_CAT_RF:        return "RF";
    case SCV_CAT_AUTH:      return "AUTH";
    default:                return "???";
  }
}

inline const char* ack_status_name(AckStatus status) {
  switch (status) {
    case ACK_STATUS_UNREAD:       return "unread";
    case ACK_STATUS_REVIEWED:     return "reviewed";
    case ACK_STATUS_ACKNOWLEDGED: return "acknowledged";
    case ACK_STATUS_ARCHIVED:     return "archived";
    default:                      return "unknown";
  }
}

// Determine if a log level requires user attention
inline bool log_level_requires_attention(LogLevel level) {
  return level >= SCV_LOG_WARNING;
}

// Determine if a log level is security-related
inline bool log_level_is_security(LogLevel level) {
  return level >= SCV_LOG_ALERT;
}

#endif // SECURACV_LOG_LEVEL_H
