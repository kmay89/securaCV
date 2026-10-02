/* Preferences for test_wap_module_boot.cpp: the canary-wap's CSI module
 * settings readers (csi_settings_nvs.cpp, by csi_module_settings_nvs.h's
 * rule) read their rows through read-only handles.
 *
 * One typed store that outlives every handle, as NVS outlives a reboot.
 * Rows are typed as Arduino-ESP32's are: getInt finds only a row putInt
 * wrote, getBool only a putBool row (the dashboard stores pet mode with
 * putBool, the preset and the thresholds with putInt). It counts begin()
 * calls and the handles they opened, and the rows each get asks for.
 *
 * Namespaces are NVS's: one exists once a read-write begin() created it or a
 * row is stored in it, and a read-only begin() of one that does not exist
 * fails, as nvs_open() answers ESP_ERR_NVS_NOT_FOUND. Every failed begin()
 * counts one error log, as Arduino-ESP32's Preferences::begin() logs
 * "nvs_open failed: ..." at error level for each (sweep F125). fail_begin
 * models NVS itself refusing every open (a fault, not a missing namespace).
 * nvs.h beside this file answers IDF's nvs_open() from the same store, and
 * logs nothing. */
#ifndef STUB_MODULE_BOOT_PREFERENCES_H
#define STUB_MODULE_BOOT_PREFERENCES_H

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <set>
#include <string>
#include <vector>

struct HostPrefs {
  std::map<std::string, int32_t> i32;   /* "<namespace>/<key>" */
  std::map<std::string, bool>    flag;
  std::map<std::string, float>   f32;
  std::set<std::string> created;       /* namespaces a read-write begin() made */
  bool fail_begin = false;              /* NVS refuses every open (a fault) */
  int  begins = 0;                      /* begin() calls, opened or refused */
  int  opens = 0;                       /* begin() calls that opened */
  int  error_logs = 0;                  /* failed begin()s: Preferences logs each */
  int  probes = 0;                      /* nvs_open() calls (nvs.h), which log nothing */
  std::vector<std::string> gets;        /* "<namespace>/<key>", one per get */

  void clear() { *this = HostPrefs(); }
  /* Does NVS hold namespace `ns`? */
  bool has_namespace(const std::string& ns) const {
    if (created.count(ns) != 0) return true;
    const std::string prefix = ns + "/";
    return has_prefix(i32, prefix) || has_prefix(flag, prefix) || has_prefix(f32, prefix);
  }
  template <class Rows>
  static bool has_prefix(const Rows& rows, const std::string& prefix) {
    auto it = rows.lower_bound(prefix);
    return it != rows.end() && it->first.compare(0, prefix.size(), prefix) == 0;
  }
  size_t gets_of(const std::string& ns_key) const {
    size_t n = 0;
    for (const std::string& g : gets) n += (g == ns_key);
    return n;
  }
};
inline HostPrefs& host_prefs() {
  static HostPrefs prefs;
  return prefs;
}

class Preferences {
 public:
  bool begin(const char* name, bool readOnly = false, const char* = nullptr) {
    host_prefs().begins++;
    if (open_ || !name) return false;
    if (host_prefs().fail_begin || (readOnly && !host_prefs().has_namespace(name))) {
      host_prefs().error_logs++;        /* log_e("nvs_open failed: ...") */
      return false;
    }
    if (!readOnly) host_prefs().created.insert(name);
    ns_ = name;
    ro_ = readOnly;
    open_ = true;
    host_prefs().opens++;
    return true;
  }
  void end() { open_ = false; }

  int32_t getInt(const char* key, int32_t d = 0) { return get(host_prefs().i32, key, d); }
  bool getBool(const char* key, bool d = false) { return get(host_prefs().flag, key, d); }
  float getFloat(const char* key, float d = 0.0f) { return get(host_prefs().f32, key, d); }

  size_t putInt(const char* key, int32_t v) { return put(host_prefs().i32, key, v); }
  size_t putBool(const char* key, bool v) { return put(host_prefs().flag, key, v); }

 private:
  template <class T>
  T get(const std::map<std::string, T>& rows, const char* key, T d) {
    if (!open_ || !key) return d;
    const std::string k = ns_ + "/" + key;
    host_prefs().gets.push_back(k);
    auto it = rows.find(k);
    return it == rows.end() ? d : it->second;
  }
  template <class T>
  size_t put(std::map<std::string, T>& rows, const char* key, T v) {
    if (!open_ || ro_ || !key) return 0;
    rows[ns_ + "/" + key] = v;
    return sizeof(T);
  }

  std::string ns_;
  bool ro_ = false;
  bool open_ = false;
};

#endif
