/* Preferences for test_wap_first_boot_nvs.cpp: the "securacv" opens a
 * canary-wap boot makes before and around provisioning (setup_wizard::init(),
 * provision_device()'s key read and store through nvs_store.h's
 * NvsMainSession, power_monitor's load_nvs_state()), with the Arduino-ESP32
 * signatures they call.
 *
 * One typed store that outlives every handle, as NVS outlives a reboot.
 * Namespaces are NVS's: one exists once a read-write begin() created it or a
 * row is stored in it, and a read-only begin() of one that does not exist
 * fails, as nvs_open() answers ESP_ERR_NVS_NOT_FOUND. Every failed begin()
 * counts one error line, as Arduino-ESP32's Preferences::begin() logs
 * "nvs_open failed: ..." at error level for each (sweeps F125, F201).
 * fail_begin models NVS itself refusing every open (a fault, not a missing
 * namespace). begin() on a begun handle refuses, as Arduino's does. nvs.h
 * beside this file answers IDF's nvs_open() from the same store, and logs
 * nothing. The String Arduino.h comes from stubs/nvs_store. */
#ifndef STUB_FIRST_BOOT_PREFERENCES_H
#define STUB_FIRST_BOOT_PREFERENCES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "Arduino.h"

struct HostFirstBootNvs {
  struct Row {
    char type = 0;   // 'B' bool, 'U' u8, 'c' i8, 'S' u16, 'u' u32, 'L' ulong, 'b' blob, 's' string
    std::vector<uint8_t> bytes;
  };
  std::map<std::string, Row> rows;     // "<namespace>/<key>"
  std::set<std::string> created;       // namespaces a read-write begin() made
  bool fail_begin = false;             // NVS refuses every open (a fault)
  int begins = 0;                      // begin() calls, opened or refused
  int opens = 0;                       // begin() calls that opened
  int error_logs = 0;                  // failed begin()s: Preferences logs each
  int probes = 0;                      // nvs_open() calls (nvs.h), which log nothing

  void clear() { *this = HostFirstBootNvs(); }
  bool has_namespace(const std::string& ns) const {
    if (created.count(ns) != 0) return true;
    const std::string prefix = ns + "/";
    auto it = rows.lower_bound(prefix);
    return it != rows.end() && it->first.compare(0, prefix.size(), prefix) == 0;
  }
  void reset_counts() { begins = opens = error_logs = probes = 0; }
};
inline HostFirstBootNvs& host_nvs() {
  static HostFirstBootNvs nvs;
  return nvs;
}

class Preferences {
 public:
  bool begin(const char* name, bool readOnly = false, const char* = nullptr) {
    host_nvs().begins++;
    if (started_ || name == nullptr) return false;
    if (host_nvs().fail_begin || (readOnly && !host_nvs().has_namespace(name))) {
      host_nvs().error_logs++;          /* log_e("nvs_open failed: ...") */
      return false;
    }
    if (!readOnly) host_nvs().created.insert(name);
    ns_ = name;
    read_only_ = readOnly;
    started_ = true;
    host_nvs().opens++;
    return true;
  }
  void end() { started_ = false; }

  bool isKey(const char* key) { return find(key) != nullptr; }
  bool remove(const char* key) {
    if (!started_ || read_only_ || key == nullptr) return false;
    return host_nvs().rows.erase(path(key)) == 1;
  }
  bool clear() {
    if (!started_ || read_only_) return false;
    const std::string prefix = ns_ + "/";
    for (auto it = host_nvs().rows.begin(); it != host_nvs().rows.end();) {
      if (it->first.compare(0, prefix.size(), prefix) == 0) it = host_nvs().rows.erase(it);
      else ++it;
    }
    return true;
  }

  bool getBool(const char* key, bool d = false) { return scalar(key, 'B', d); }
  uint8_t getUChar(const char* key, uint8_t d = 0) { return scalar(key, 'U', d); }
  uint16_t getUShort(const char* key, uint16_t d = 0) { return scalar(key, 'S', d); }
  int8_t getChar(const char* key, int8_t d = 0) { return scalar(key, 'c', d); }
  uint32_t getUInt(const char* key, uint32_t d = 0) { return scalar(key, 'u', d); }
  uint32_t getULong(const char* key, uint32_t d = 0) { return scalar(key, 'L', d); }
  size_t getBytesLength(const char* key) {
    const HostFirstBootNvs::Row* r = find(key);
    return (r != nullptr && r->type == 'b') ? r->bytes.size() : 0;
  }
  size_t getBytes(const char* key, void* buf, size_t max_len) {
    const HostFirstBootNvs::Row* r = find(key);
    if (r == nullptr || r->type != 'b' || buf == nullptr || r->bytes.size() > max_len) return 0;
    memcpy(buf, r->bytes.data(), r->bytes.size());
    return r->bytes.size();
  }
  String getString(const char* key, String d = String()) {
    const HostFirstBootNvs::Row* r = find(key);
    if (r == nullptr || r->type != 's') return d;
    const std::string v(r->bytes.begin(), r->bytes.end());
    return String(v.c_str());
  }
  size_t getString(const char* key, char* out, size_t max_len) {
    const HostFirstBootNvs::Row* r = find(key);
    if (r == nullptr || r->type != 's' || out == nullptr || r->bytes.size() + 1 > max_len) return 0;
    memcpy(out, r->bytes.data(), r->bytes.size());
    out[r->bytes.size()] = '\0';
    return r->bytes.size() + 1;
  }

  size_t putBool(const char* key, bool v) { return put(key, 'B', &v, 1); }
  size_t putUChar(const char* key, uint8_t v) { return put(key, 'U', &v, 1); }
  size_t putUShort(const char* key, uint16_t v) { return put(key, 'S', &v, 2); }
  size_t putChar(const char* key, int8_t v) { return put(key, 'c', &v, 1); }
  size_t putUInt(const char* key, uint32_t v) { return put(key, 'u', &v, 4); }
  size_t putULong(const char* key, uint32_t v) { return put(key, 'L', &v, 4); }
  size_t putBytes(const char* key, const void* v, size_t len) { return put(key, 'b', v, len); }
  size_t putString(const char* key, const char* v) {
    return v != nullptr ? put(key, 's', v, strlen(v)) : 0;
  }

 private:
  std::string path(const char* key) const { return ns_ + "/" + (key != nullptr ? key : ""); }
  const HostFirstBootNvs::Row* find(const char* key) const {
    if (!started_ || key == nullptr) return nullptr;
    auto it = host_nvs().rows.find(path(key));
    return it == host_nvs().rows.end() ? nullptr : &it->second;
  }
  size_t put(const char* key, char type, const void* v, size_t len) {
    if (!started_ || read_only_ || key == nullptr) return 0;
    HostFirstBootNvs::Row& r = host_nvs().rows[path(key)];
    r.type = type;
    const uint8_t* p = static_cast<const uint8_t*>(v);
    r.bytes.assign(p, p + len);
    return len;
  }
  template <typename T>
  T scalar(const char* key, char type, T d) {
    const HostFirstBootNvs::Row* r = find(key);
    if (r == nullptr || r->type != type || r->bytes.size() != sizeof(T)) return d;
    T v;
    memcpy(&v, r->bytes.data(), sizeof(T));
    return v;
  }

  std::string ns_;
  bool read_only_ = false;
  bool started_ = false;
};

#endif
