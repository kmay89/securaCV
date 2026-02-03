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

enum LogLevel : uint8_t {
  LOG_LEVEL_DEBUG    = 0,   // Verbose debugging (not stored by default)
  LOG_LEVEL_INFO     = 1,   // Normal operational events
  LOG_LEVEL_NOTICE   = 2,   // Notable but expected events
  LOG_LEVEL_WARNING  = 3,   // Potential issues requiring attention
  LOG_LEVEL_ERROR    = 4,   // Errors requiring review
  LOG_LEVEL_CRITICAL = 5,   // Critical failures affecting operation
  LOG_LEVEL_ALERT    = 6,   // Immediate action required
  LOG_LEVEL_TAMPER   = 7    // Security/integrity events (highest priority)
};

// ════════════════════════════════════════════════════════════════════════════
// LOG CATEGORIES
// ════════════════════════════════════════════════════════════════════════════

enum LogCategory : uint8_t {
  LOG_CAT_SYSTEM     = 0,   // Boot, shutdown, watchdog
  LOG_CAT_CRYPTO     = 1,   // Key generation, signing, verification
  LOG_CAT_CHAIN      = 2,   // Hash chain operations
  LOG_CAT_GPS        = 3,   // GNSS fix, satellites, time sync
  LOG_CAT_STORAGE    = 4,   // SD card, NVS operations
  LOG_CAT_NETWORK    = 5,   // WiFi, HTTP server
  LOG_CAT_SENSOR     = 6,   // PIR, tamper, environmental
  LOG_CAT_USER       = 7,   // User actions (config changes, acknowledgments)
  LOG_CAT_WITNESS    = 8,   // Witness record creation
  LOG_CAT_MESH       = 9,   // Mesh network (flock) operations
  LOG_CAT_BLUETOOTH  = 10,  // Bluetooth Low Energy operations
  LOG_CAT_RF         = 11   // RF presence detection operations
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
    case LOG_LEVEL_DEBUG:    return "DEBUG";
    case LOG_LEVEL_INFO:     return "INFO";
    case LOG_LEVEL_NOTICE:   return "NOTICE";
    case LOG_LEVEL_WARNING:  return "WARN";
    case LOG_LEVEL_ERROR:    return "ERROR";
    case LOG_LEVEL_CRITICAL: return "CRIT";
    case LOG_LEVEL_ALERT:    return "ALERT";
    case LOG_LEVEL_TAMPER:   return "TAMPER";
    default:                 return "???";
  }
}

inline const char* log_level_name_short(LogLevel level) {
  switch (level) {
    case LOG_LEVEL_DEBUG:    return "DBG";
    case LOG_LEVEL_INFO:     return "INF";
    case LOG_LEVEL_NOTICE:   return "NTC";
    case LOG_LEVEL_WARNING:  return "WRN";
    case LOG_LEVEL_ERROR:    return "ERR";
    case LOG_LEVEL_CRITICAL: return "CRT";
    case LOG_LEVEL_ALERT:    return "ALT";
    case LOG_LEVEL_TAMPER:   return "TMP";
    default:                 return "???";
  }
}

inline const char* log_category_name(LogCategory cat) {
  switch (cat) {
    case LOG_CAT_SYSTEM:    return "SYSTEM";
    case LOG_CAT_CRYPTO:    return "CRYPTO";
    case LOG_CAT_CHAIN:     return "CHAIN";
    case LOG_CAT_GPS:       return "GPS";
    case LOG_CAT_STORAGE:   return "STORAGE";
    case LOG_CAT_NETWORK:   return "NETWORK";
    case LOG_CAT_SENSOR:    return "SENSOR";
    case LOG_CAT_USER:      return "USER";
    case LOG_CAT_WITNESS:   return "WITNESS";
    case LOG_CAT_MESH:      return "MESH";
    case LOG_CAT_BLUETOOTH: return "BLUETOOTH";
    case LOG_CAT_RF:        return "RF";
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
  return level >= LOG_LEVEL_WARNING;
}

// Determine if a log level is security-related
inline bool log_level_is_security(LogLevel level) {
  return level >= LOG_LEVEL_ALERT;
}

#endif // SECURACV_LOG_LEVEL_H
