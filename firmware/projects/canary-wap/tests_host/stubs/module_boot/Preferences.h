/* Preferences for test_wap_module_boot.cpp: the canary-wap's CSI module
 * settings readers (csi_settings_nvs.cpp, by csi_module_settings_nvs.h's
 * rule) read their rows through read-only handles.
 *
 * One typed store that outlives every handle, as NVS outlives a reboot.
 * Rows are typed as Arduino-ESP32's are: getInt finds only a row putInt
 * wrote, getBool only a putBool row (the dashboard stores pet mode with
 * putBool, the preset and the thresholds with putInt). It counts the handles
 * that opened and the rows each get asked for. */
#ifndef STUB_MODULE_BOOT_PREFERENCES_H
#define STUB_MODULE_BOOT_PREFERENCES_H

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

struct HostPrefs {
  std::map<std::string, int32_t> i32;   /* "<namespace>/<key>" */
  std::map<std::string, bool>    flag;
  std::map<std::string, float>   f32;
  int  opens = 0;                       /* begin() calls that opened */
  std::vector<std::string> gets;        /* "<namespace>/<key>", one per get */

  void clear() { *this = HostPrefs(); }
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
    if (open_ || !name) return false;
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
