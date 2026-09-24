/* Fake Preferences for test_nvs_store_lock.cpp, with the API the sketch's
 * nvs_store.h compiles against (the Arduino-ESP32 signatures it calls).
 *
 * Like the real one: begin() on a started handle refuses (false) rather than
 * reopening it, and the double begin a broken reopen would make is counted;
 * a put on a read-only handle writes nothing and returns 0; a get or put on a
 * closed handle returns the default or 0, and is counted, because nvs_store.h
 * may only touch the handle inside a session. Entries are typed, as NVS's
 * are: a blob read of a u32 key finds no blob. The store outlives every
 * handle (g_host_nvs), so a value written in one session reads back in the
 * next. The test can make the next begin() fail, and reads each instance
 * through g_host_prefs (NvsManager's handle is private), in construction
 * order. */
#ifndef STUB_NVS_STORE_PREFERENCES_H
#define STUB_NVS_STORE_PREFERENCES_H

#include <stdint.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "Arduino.h"

class Preferences;
inline Preferences* g_host_prefs[4] = {nullptr, nullptr, nullptr, nullptr};
inline int g_host_prefs_count = 0;
inline const char* g_host_prefs_namespace = nullptr;  // the test sets NVS_MAIN_NS

struct HostNvsEntry {
  char type = 0;  // 'b' blob, 'u' u32, 'U' u8, 'c' i8, 'B' bool, 'L' ulong, 's' string
  std::vector<uint8_t> bytes;
};
inline std::map<std::string, HostNvsEntry> g_host_nvs;  // "<namespace>/<key>"

class Preferences {
 public:
  Preferences() {
    if (g_host_prefs_count < 4) g_host_prefs[g_host_prefs_count++] = this;
  }

  bool begin(const char* name, bool readOnly = false, const char* = nullptr) {
    if (started) { double_begins++; return false; }
    if (fail_next) { fail_next = false; return false; }
    if (name == nullptr || g_host_prefs_namespace == nullptr ||
        strcmp(name, g_host_prefs_namespace) != 0) {
      wrong_namespace++;
    }
    ns = name != nullptr ? name : "";
    started = true;
    read_only = readOnly;
    begins++;
    return true;
  }

  void end() {
    if (!started) ends_while_closed++;
    started = false;
    read_only = false;
  }

  bool clear() { return writable() ? (erase_all(), true) : false; }
  bool remove(const char* key) {
    if (!writable()) return false;
    return g_host_nvs.erase(path(key)) == 1;
  }
  bool isKey(const char* key) { return readable() && find(key) != nullptr; }

  size_t putBool(const char* key, bool v) { return put(key, 'B', &v, 1); }
  size_t putUChar(const char* key, uint8_t v) { return put(key, 'U', &v, 1); }
  size_t putChar(const char* key, int8_t v) { return put(key, 'c', &v, 1); }
  size_t putUInt(const char* key, uint32_t v) { return put(key, 'u', &v, 4); }
  size_t putULong(const char* key, uint32_t v) { return put(key, 'L', &v, 4); }
  size_t putBytes(const char* key, const void* v, size_t len) {
    return put(key, 'b', v, len);
  }

  bool getBool(const char* key, bool def = false) { return get_scalar(key, 'B', def); }
  uint8_t getUChar(const char* key, uint8_t def = 0) { return get_scalar(key, 'U', def); }
  int8_t getChar(const char* key, int8_t def = 0) { return get_scalar(key, 'c', def); }
  uint32_t getUInt(const char* key, uint32_t def = 0) { return get_scalar(key, 'u', def); }
  uint32_t getULong(const char* key, uint32_t def = 0) { return get_scalar(key, 'L', def); }

  size_t getBytesLength(const char* key) {
    const HostNvsEntry* e = readable() ? find(key) : nullptr;
    return (e != nullptr && e->type == 'b') ? e->bytes.size() : 0;
  }
  size_t getBytes(const char* key, void* buf, size_t maxLen) {
    const HostNvsEntry* e = readable() ? find(key) : nullptr;
    if (e == nullptr || e->type != 'b' || buf == nullptr || e->bytes.size() > maxLen) return 0;
    memcpy(buf, e->bytes.data(), e->bytes.size());
    return e->bytes.size();
  }
  String getString(const char* key, String def = String()) {
    const HostNvsEntry* e = readable() ? find(key) : nullptr;
    if (e == nullptr || e->type != 's') return def;
    const std::string v(e->bytes.begin(), e->bytes.end());
    return String(v.c_str());
  }

  bool started = false;
  bool read_only = false;
  bool fail_next = false;     // the next begin() fails (a full or failed NVS)
  int begins = 0;             // begin() calls that opened the handle
  int double_begins = 0;      // begin() on a started handle
  int ends_while_closed = 0;  // end() on a handle that was not open
  int wrong_namespace = 0;    // begin() on any namespace but g_host_prefs_namespace
  int ops_while_closed = 0;   // a get or put with the handle closed
  int puts_while_read_only = 0;
  int puts = 0;               // puts that wrote

 private:
  std::string ns;

  std::string path(const char* key) const { return ns + "/" + (key != nullptr ? key : ""); }
  bool readable() {
    if (!started) { ops_while_closed++; return false; }
    return true;
  }
  bool writable() {
    if (!readable()) return false;
    if (read_only) { puts_while_read_only++; return false; }
    return true;
  }
  const HostNvsEntry* find(const char* key) const {
    auto it = g_host_nvs.find(path(key));
    return it == g_host_nvs.end() ? nullptr : &it->second;
  }
  size_t put(const char* key, char type, const void* v, size_t len) {
    if (!writable()) return 0;
    HostNvsEntry& e = g_host_nvs[path(key)];
    e.type = type;
    const uint8_t* p = static_cast<const uint8_t*>(v);
    e.bytes.assign(p, p + len);
    puts++;
    return len;
  }
  template <typename T>
  T get_scalar(const char* key, char type, T def) {
    const HostNvsEntry* e = readable() ? find(key) : nullptr;
    if (e == nullptr || e->type != type || e->bytes.size() != sizeof(T)) return def;
    T v;
    memcpy(&v, e->bytes.data(), sizeof(T));
    return v;
  }
  void erase_all() {
    const std::string prefix = ns + "/";
    for (auto it = g_host_nvs.begin(); it != g_host_nvs.end();) {
      if (it->first.compare(0, prefix.size(), prefix) == 0) it = g_host_nvs.erase(it);
      else ++it;
    }
  }
};

#endif
