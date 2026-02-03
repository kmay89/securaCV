/*
 * SecuraCV Canary — NVS Storage Manager
 *
 * Encapsulated NVS (Non-Volatile Storage) access using the Arduino Preferences
 * library. Provides both a singleton manager for the main namespace and an
 * RAII session class for module-specific namespaces.
 */

#ifndef SECURACV_NVS_STORE_H
#define SECURACV_NVS_STORE_H

#include <Arduino.h>
#include <Preferences.h>
#include <cstddef>  // For std::nullptr_t

// ════════════════════════════════════════════════════════════════════════════
// NVS NAMESPACES (centralized definitions)
// ════════════════════════════════════════════════════════════════════════════

// Main namespace for core device settings (keys, WiFi, Bluetooth, etc.)
static const char* NVS_MAIN_NS = "securacv";

// Chirp channel namespace
static const char* NVS_CHIRP_NS = "chirp";

// Mesh network namespace
static const char* NVS_MESH_NS = "mesh";

// ════════════════════════════════════════════════════════════════════════════
// NVS MANAGER SINGLETON
// ════════════════════════════════════════════════════════════════════════════

/*
 * NvsManager provides encapsulated access to the main NVS namespace.
 * Use this singleton for all operations on the "securacv" namespace instead
 * of directly accessing a global Preferences object.
 *
 * Example usage:
 *   NvsManager& nvs = NvsManager::instance();
 *   if (nvs.begin(false)) {  // Open for read-write
 *     nvs.putBool("key", true);
 *     nvs.end();
 *   }
 */
class NvsManager {
public:
  // Get the singleton instance
  static NvsManager& instance() {
    static NvsManager s_instance;
    return s_instance;
  }

  // Open NVS session. Returns true on success.
  // If already open in a compatible mode, returns true without reopening.
  // If write mode is requested but session is open in read-only mode, reopens.
  bool begin(bool readOnly = false) {
    if (m_open) {
      // If write is requested but we are in read-only mode, reopen
      if (m_readOnly && !readOnly) {
        m_prefs.end();
        m_open = false;  // Force reopen
      } else {
        return true;  // Already open in compatible mode
      }
    }
    m_open = m_prefs.begin(NVS_MAIN_NS, readOnly);
    m_readOnly = readOnly;
    return m_open;
  }

  // Open NVS in read-only mode (convenience wrapper)
  bool beginReadOnly() { return begin(true); }

  // Open NVS in read-write mode (convenience wrapper)
  bool beginReadWrite() { return begin(false); }

  // Close NVS session
  void end() {
    if (m_open) {
      m_prefs.end();
      m_open = false;
    }
  }

  // Check if NVS is currently open
  bool isOpen() const { return m_open; }

  // Check if opened in read-only mode
  bool isReadOnly() const { return m_readOnly; }

  // ──────────────────────────────────────────────────────────────────────────
  // Boolean operations
  // ──────────────────────────────────────────────────────────────────────────
  bool getBool(const char* key, bool defaultValue = false) {
    return m_prefs.getBool(key, defaultValue);
  }

  size_t putBool(const char* key, bool value) {
    return m_prefs.putBool(key, value);
  }

  // ──────────────────────────────────────────────────────────────────────────
  // Integer operations
  // ──────────────────────────────────────────────────────────────────────────
  uint8_t getUChar(const char* key, uint8_t defaultValue = 0) {
    return m_prefs.getUChar(key, defaultValue);
  }

  size_t putUChar(const char* key, uint8_t value) {
    return m_prefs.putUChar(key, value);
  }

  int8_t getChar(const char* key, int8_t defaultValue = 0) {
    return m_prefs.getChar(key, defaultValue);
  }

  size_t putChar(const char* key, int8_t value) {
    return m_prefs.putChar(key, value);
  }

  uint32_t getUInt(const char* key, uint32_t defaultValue = 0) {
    return m_prefs.getUInt(key, defaultValue);
  }

  size_t putUInt(const char* key, uint32_t value) {
    return m_prefs.putUInt(key, value);
  }

  unsigned long getULong(const char* key, unsigned long defaultValue = 0) {
    return m_prefs.getULong(key, defaultValue);
  }

  size_t putULong(const char* key, unsigned long value) {
    return m_prefs.putULong(key, value);
  }

  // ──────────────────────────────────────────────────────────────────────────
  // Byte array operations
  // ──────────────────────────────────────────────────────────────────────────
  size_t getBytesLength(const char* key) {
    return m_prefs.getBytesLength(key);
  }

  size_t getBytes(const char* key, void* buf, size_t maxLen) {
    return m_prefs.getBytes(key, buf, maxLen);
  }

  size_t putBytes(const char* key, const void* value, size_t len) {
    return m_prefs.putBytes(key, value, len);
  }

  // ──────────────────────────────────────────────────────────────────────────
  // Key management
  // ──────────────────────────────────────────────────────────────────────────
  bool isKey(const char* key) {
    return m_prefs.isKey(key);
  }

  bool remove(const char* key) {
    return m_prefs.remove(key);
  }

  bool clear() {
    return m_prefs.clear();
  }

  // Prevent copying
  NvsManager(const NvsManager&) = delete;
  NvsManager& operator=(const NvsManager&) = delete;

private:
  NvsManager() : m_open(false), m_readOnly(false) {}
  ~NvsManager() { end(); }

  Preferences m_prefs;
  bool m_open;
  bool m_readOnly;
};

// ════════════════════════════════════════════════════════════════════════════
// LEGACY COMPATIBILITY FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

/*
 * These inline functions provide backward compatibility for code that
 * previously used the global g_prefs object directly. They delegate to
 * the NvsManager singleton.
 */

// Open NVS in read-write mode (uses main namespace)
inline bool nvs_open_rw() {
  return NvsManager::instance().beginReadWrite();
}

// Open NVS in read-only mode (uses main namespace)
inline bool nvs_open_ro() {
  return NvsManager::instance().beginReadOnly();
}

// Close NVS
inline void nvs_close() {
  NvsManager::instance().end();
}

// ════════════════════════════════════════════════════════════════════════════
// RAII NVS SESSION CLASS (for module-specific namespaces)
// ════════════════════════════════════════════════════════════════════════════

/*
 * NvsSession provides RAII-based NVS access for module-specific namespaces.
 * Unlike NvsManager, this creates a separate Preferences instance for
 * namespace isolation.
 *
 * Example usage:
 *   {
 *     NvsSession nvs(NVS_CHIRP_NS, false);  // Open chirp namespace for read-write
 *     if (nvs.isOpen()) {
 *       nvs.setU8("key1", 42);
 *       nvs.setU8("key2", 100);
 *     }
 *   }  // Automatically closes on scope exit
 */
class NvsSession {
public:
  // Open NVS partition with specified namespace. readOnly=true for read-only access.
  explicit NvsSession(const char* ns = NVS_CHIRP_NS, bool readOnly = true) : m_open(false) {
    m_open = m_prefs.begin(ns, readOnly);
  }

  // Legacy constructor for backward compatibility (uses chirp namespace)
  explicit NvsSession(bool readOnly) : m_open(false) {
    m_open = m_prefs.begin(NVS_CHIRP_NS, readOnly);
  }

  // Prevent ambiguity with nullptr (nullptr could match both const char* and bool)
  explicit NvsSession(std::nullptr_t) = delete;

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
// CONVENIENCE FUNCTIONS (for single operations on chirp namespace)
// ════════════════════════════════════════════════════════════════════════════

// Get a uint8_t value from NVS (chirp namespace)
// Returns true if key exists and value was read successfully
inline bool nvs_get_u8(const char* key, uint8_t* out_val) {
  NvsSession nvs(NVS_CHIRP_NS, true);
  return nvs.getU8(key, out_val);
}

// Set a uint8_t value in NVS (chirp namespace)
// Returns true if write was successful
inline bool nvs_set_u8(const char* key, uint8_t val) {
  NvsSession nvs(NVS_CHIRP_NS, false);
  return nvs.setU8(key, val);
}

// Get a uint32_t value from NVS (chirp namespace)
// Returns true if key exists and value was read successfully
inline bool nvs_get_u32(const char* key, uint32_t* out_val) {
  NvsSession nvs(NVS_CHIRP_NS, true);
  return nvs.getU32(key, out_val);
}

// Set a uint32_t value in NVS (chirp namespace)
// Returns true if write was successful
inline bool nvs_set_u32(const char* key, uint32_t val) {
  NvsSession nvs(NVS_CHIRP_NS, false);
  return nvs.setU32(key, val);
}

// Check if a key exists in NVS (chirp namespace)
inline bool nvs_has_key(const char* key) {
  NvsSession nvs(NVS_CHIRP_NS, true);
  return nvs.hasKey(key);
}

// Remove a key from NVS (chirp namespace)
inline bool nvs_remove(const char* key) {
  NvsSession nvs(NVS_CHIRP_NS, false);
  return nvs.remove(key);
}

// ════════════════════════════════════════════════════════════════════════════
// NVS_STORE NAMESPACE (for RF presence and other modules)
// ════════════════════════════════════════════════════════════════════════════

/*
 * Namespace-based convenience functions for NVS operations.
 * These use the main "securacv" namespace via NvsManager singleton.
 */
namespace nvs_store {

// Get a uint32_t value from NVS
// Returns the value if key exists, otherwise returns default_val
inline uint32_t get_u32(const char* key, uint32_t default_val) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.begin(true)) return default_val;
  uint32_t val = nvs.getUInt(key, default_val);
  nvs.end();
  return val;
}

// Set a uint32_t value in NVS
// Returns true on success
inline bool set_u32(const char* key, uint32_t val) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.begin(false)) return false;
  size_t written = nvs.putUInt(key, val);
  nvs.end();
  return written == sizeof(uint32_t);
}

// Get a blob (byte array) from NVS
// Returns true if key exists and data was read successfully
inline bool get_blob(const char* key, void* buf, size_t len) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.begin(true)) return false;
  if (!nvs.isKey(key)) {
    nvs.end();
    return false;
  }
  size_t stored_len = nvs.getBytesLength(key);
  if (stored_len != len) {
    nvs.end();
    return false;
  }
  size_t read = nvs.getBytes(key, buf, len);
  nvs.end();
  return read == len;
}

// Set a blob (byte array) in NVS
// Returns true on success
inline bool set_blob(const char* key, const void* buf, size_t len) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.begin(false)) return false;
  size_t written = nvs.putBytes(key, buf, len);
  nvs.end();
  return written == len;
}

} // namespace nvs_store

#endif // SECURACV_NVS_STORE_H
