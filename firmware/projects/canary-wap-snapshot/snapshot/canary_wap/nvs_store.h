/*
 * SecuraCV Canary — NVS Storage Wrapper
 *
 * Simple wrapper functions for ESP32 NVS (Non-Volatile Storage)
 * using the Arduino Preferences library.
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
// NVS WRAPPER FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

// Get a uint8_t value from NVS
// Returns true if key exists and value was read successfully
inline bool nvs_get_u8(const char* key, uint8_t* out_val) {
  Preferences prefs;
  if (!prefs.begin(NVS_CHIRP_NS, true)) {
    return false;
  }

  if (!prefs.isKey(key)) {
    prefs.end();
    return false;
  }

  *out_val = prefs.getUChar(key, 0);
  prefs.end();
  return true;
}

// Set a uint8_t value in NVS
// Returns true if write was successful
inline bool nvs_set_u8(const char* key, uint8_t val) {
  Preferences prefs;
  if (!prefs.begin(NVS_CHIRP_NS, false)) {
    return false;
  }

  size_t written = prefs.putUChar(key, val);
  prefs.end();
  return (written == sizeof(uint8_t));
}

// Get a uint32_t value from NVS
// Returns true if key exists and value was read successfully
inline bool nvs_get_u32(const char* key, uint32_t* out_val) {
  Preferences prefs;
  if (!prefs.begin(NVS_CHIRP_NS, true)) {
    return false;
  }

  if (!prefs.isKey(key)) {
    prefs.end();
    return false;
  }

  *out_val = prefs.getUInt(key, 0);
  prefs.end();
  return true;
}

// Set a uint32_t value in NVS
// Returns true if write was successful
inline bool nvs_set_u32(const char* key, uint32_t val) {
  Preferences prefs;
  if (!prefs.begin(NVS_CHIRP_NS, false)) {
    return false;
  }

  size_t written = prefs.putUInt(key, val);
  prefs.end();
  return (written == sizeof(uint32_t));
}

// Check if a key exists in NVS
inline bool nvs_has_key(const char* key) {
  Preferences prefs;
  if (!prefs.begin(NVS_CHIRP_NS, true)) {
    return false;
  }

  bool exists = prefs.isKey(key);
  prefs.end();
  return exists;
}

// Remove a key from NVS
inline bool nvs_remove(const char* key) {
  Preferences prefs;
  if (!prefs.begin(NVS_CHIRP_NS, false)) {
    return false;
  }

  bool removed = prefs.remove(key);
  prefs.end();
  return removed;
}

#endif // SECURACV_NVS_STORE_H
