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
#include <cstring>  // For memcpy
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// NvsManager's per-task session count (pure, host-tested): a staged copy of
// firmware/common/storage/nvs_session_depth.h, held byte-identical by
// firmware/scripts/check_csi_sync.sh (setup.sh arduino re-stages it).
#include "nvs_session_depth.h"

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
 * It is ONE Preferences handle, shared by every task that calls it, and five
 * tasks open sessions on it: the loop task (setup() and loop(): the witness
 * chain persist, the birth stamp, the vault's capture sequence, the modules'
 * nvs_store:: settings), the httpd task serving the API (the Wi-Fi, config,
 * reboot, vault, Bluetooth, household and RF-presence routes), the NimBLE
 * host task (a newly bonded peer's pairing record; a phone's BLE Wi-Fi
 * provisioning), the Bluetooth bring-up task (bluetooth_channel::init()) and
 * the QR-scan task (a scanned Wi-Fi credential). Sessions are serialized
 * across tasks, as the canary's are (securacv_crypto.cpp, sweep F52): begin()
 * takes a recursive mutex and keeps it until the matching end(), so another
 * task's begin() waits for it (at most nvs_session::kSessionWaitMs, then
 * returns false — fail soft, the way a failed Preferences::begin always
 * could) and another task's end() cannot close it. Before the lock, a
 * session ending on one task closed the handle under another, so a write
 * racing that end() landed nothing. Nesting on one task is counted
 * (nvs_session_depth.h): only the outermost end() closes the handle, and a
 * read-write session inside a read-only one reopens it read-write. An end()
 * from a task with no session is a no-op. The accessors below are valid only
 * between the calling task's begin() and end(); isOpen()/isReadOnly()
 * describe the session of the task that holds the lock.
 *
 * Every begin() that returns true owes exactly one end() on every path,
 * because the lock is held until then: a session that never ends keeps the
 * lock on its task, and every other task's begin() waits kSessionWaitMs and
 * fails until a reboot. tests_host/test_nvs_session_balance.cpp scans the
 * sketch for a block that opens a session and does not close it. A begin()
 * that returns false owes no end().
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

  // Open an NVS session. Returns true on success, and the session then
  // belongs to this task until its matching end(). Takes the lock and KEEPS
  // it on success; every false return has given back the take it made, so a
  // caller that got false owes no end(). Inside this task's own session it
  // nests: a compatible mode keeps the handle, and read-write asked inside a
  // read-only session reopens it read-write.
  bool begin(bool readOnly = false) {
    if (m_lock != nullptr &&
        xSemaphoreTakeRecursive(m_lock, pdMS_TO_TICKS(nvs_session::kSessionWaitMs)) != pdTRUE) {
      // Another task held a session for the whole wait. A session is NVS
      // reads and writes only (no send, no delay, no wait on another task),
      // so this is a leaked session — a begin() without its end() on some
      // path — or a stalled flash: say so once per boot, then fail soft.
      // (Two tasks timing out together may both print; nothing else rides
      // on the flag.)
      static volatile bool s_wait_reported = false;
      if (!s_wait_reported) {
        s_wait_reported = true;
        Serial.printf("[NVS] session wait timed out: another task held the settings store for %lu ms\n",
                      (unsigned long)nvs_session::kSessionWaitMs);
      }
      return false;
    }
    const nvs_session::Begin action = nvs_session::on_begin(m_session, readOnly);
    bool ok = true;
    switch (action) {
      case nvs_session::Begin::Open:
        ok = m_prefs.begin(NVS_MAIN_NS, readOnly);
        break;
      case nvs_session::Begin::ReopenRw:
        m_prefs.end();
        ok = m_prefs.begin(NVS_MAIN_NS, false);
        break;
      case nvs_session::Begin::Keep:
      case nvs_session::Begin::Refuse:
        break;
    }
    if (!nvs_session::commit_begin(m_session, action, readOnly, ok)) {
      if (m_lock != nullptr) xSemaphoreGiveRecursive(m_lock);
      return false;
    }
    return true;
  }

  // Open NVS in read-only mode (convenience wrapper)
  bool beginReadOnly() { return begin(true); }

  // Open NVS in read-write mode (convenience wrapper)
  bool beginReadWrite() { return begin(false); }

  // Close this task's session. The zero-wait take tells the cases apart: it
  // succeeds at once when this task holds the lock (its own session, or a
  // nested one) or when nobody does (the depth is then 0 and this end() is a
  // no-op), and fails when another task holds a session — which is not this
  // task's to close. Only the outermost end() closes the handle.
  void end() {
    if (m_lock != nullptr && xSemaphoreTakeRecursive(m_lock, 0) != pdTRUE) return;
    const nvs_session::End e = nvs_session::on_end(m_session);
    if (e == nvs_session::End::Close) m_prefs.end();
    if (m_lock != nullptr) {
      for (uint8_t i = 0; i < nvs_session::end_gives(e); i++) xSemaphoreGiveRecursive(m_lock);
    }
  }

  // The handle is open (for the task holding the lock)
  bool isOpen() const { return m_session.open; }

  // ...and read-only
  bool isReadOnly() const { return m_session.read_only; }

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
  // String operations
  // ──────────────────────────────────────────────────────────────────────────
  // Copies the value into buf NUL-terminated. Returns chars copied; 0 when
  // the key is absent, not string-typed, or the value plus NUL doesn't fit.
  size_t getString(const char* key, char* buf, size_t maxLen) {
    if (buf == nullptr || maxLen == 0) return 0;
    String v = m_prefs.getString(key, "");
    const size_t n = v.length();
    if (n + 1 > maxLen) return 0;
    memcpy(buf, v.c_str(), n);
    buf[n] = '\0';
    return n;
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
  NvsManager() : m_lock(xSemaphoreCreateRecursiveMutex()), m_session() {}
  ~NvsManager() { end(); }

  Preferences m_prefs;
  // Created in the constructor (the first instance() call, in setup()). Null
  // only if creation failed (heap exhaustion); NvsManager then proceeds
  // unlocked, the pre-lock behavior, as the canary's does.
  SemaphoreHandle_t m_lock;
  // Changed only by the task holding m_lock (or, with no lock, by whoever
  // calls): the depth of that task's sessions and the handle's mode.
  nvs_session::State m_session;
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
  bool success = false;
  if (nvs.isKey(key)) {
    size_t stored_len = nvs.getBytesLength(key);
    if (stored_len == len) {
      size_t read = nvs.getBytes(key, buf, len);
      success = (read == len);
    }
  }
  nvs.end();
  return success;
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
