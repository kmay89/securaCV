/*
 * SecuraCV Canary — NVS Storage Wrapper
 *
 * Simple wrapper functions for ESP32 NVS (Non-Volatile Storage)
 * using the Arduino Preferences library.
 *
 * Uses RAII pattern to efficiently batch multiple NVS operations
 * under a single open/close cycle.
 */

#ifndef SECURACV_NVS_STORE_H
#define SECURACV_NVS_STORE_H

#include <Arduino.h>
#include <Preferences.h>

// ════════════════════════════════════════════════════════════════════════════
// NVS NAMESPACE
// ════════════════════════════════════════════════════════════════════════════

static const char* NVS_CHIRP_NS = "chirp";

// ════════════════════════════════════════════════════════════════════════════
// RAII NVS SESSION CLASS
// ════════════════════════════════════════════════════════════════════════════

/*
 * NvsSession provides RAII-based NVS access for efficient batched operations.
 *
 * Example usage:
 *   {
 *     NvsSession nvs(false);  // Open for read-write
 *     if (nvs.isOpen()) {
 *       nvs.setU8("key1", 42);
 *       nvs.setU8("key2", 100);
 *     }
 *   }  // Automatically closes on scope exit
 */
class NvsSession {
public:
  // Open NVS partition. readOnly=true for read-only access.
  explicit NvsSession(bool readOnly = true) : m_open(false) {
    m_open = m_prefs.begin(NVS_CHIRP_NS, readOnly);
  }

  // Automatically close on destruction
  ~NvsSession() {
    if (m_open) {
      m_prefs.end();
    }
  }

  // Check if session opened successfully
  bool isOpen() const { return m_open; }

  // Get a uint8_t value. Returns true if key exists.
  bool getU8(const char* key, uint8_t* out_val) {
    if (!m_open || !m_prefs.isKey(key)) return false;
    *out_val = m_prefs.getUChar(key, 0);
    return true;
  }

  // Set a uint8_t value. Returns true on success.
  bool setU8(const char* key, uint8_t val) {
    if (!m_open) return false;
    return m_prefs.putUChar(key, val) == sizeof(uint8_t);
  }

  // Get a uint32_t value. Returns true if key exists.
  bool getU32(const char* key, uint32_t* out_val) {
    if (!m_open || !m_prefs.isKey(key)) return false;
    *out_val = m_prefs.getUInt(key, 0);
    return true;
  }

  // Set a uint32_t value. Returns true on success.
  bool setU32(const char* key, uint32_t val) {
    if (!m_open) return false;
    return m_prefs.putUInt(key, val) == sizeof(uint32_t);
  }

  // Check if a key exists
  bool hasKey(const char* key) {
    return m_open && m_prefs.isKey(key);
  }

  // Remove a key. Returns true on success.
  bool remove(const char* key) {
    if (!m_open) return false;
    return m_prefs.remove(key);
  }

  // Prevent copying
  NvsSession(const NvsSession&) = delete;
  NvsSession& operator=(const NvsSession&) = delete;

private:
  Preferences m_prefs;
  bool m_open;
};

// ════════════════════════════════════════════════════════════════════════════
// CONVENIENCE FUNCTIONS (for single operations)
// ════════════════════════════════════════════════════════════════════════════

// Get a uint8_t value from NVS
// Returns true if key exists and value was read successfully
inline bool nvs_get_u8(const char* key, uint8_t* out_val) {
  NvsSession nvs(true);
  return nvs.getU8(key, out_val);
}

// Set a uint8_t value in NVS
// Returns true if write was successful
inline bool nvs_set_u8(const char* key, uint8_t val) {
  NvsSession nvs(false);
  return nvs.setU8(key, val);
}

// Get a uint32_t value from NVS
// Returns true if key exists and value was read successfully
inline bool nvs_get_u32(const char* key, uint32_t* out_val) {
  NvsSession nvs(true);
  return nvs.getU32(key, out_val);
}

// Set a uint32_t value in NVS
// Returns true if write was successful
inline bool nvs_set_u32(const char* key, uint32_t val) {
  NvsSession nvs(false);
  return nvs.setU32(key, val);
}

// Check if a key exists in NVS
inline bool nvs_has_key(const char* key) {
  NvsSession nvs(true);
  return nvs.hasKey(key);
}

// Remove a key from NVS
inline bool nvs_remove(const char* key) {
  NvsSession nvs(false);
  return nvs.remove(key);
}

#endif // SECURACV_NVS_STORE_H
