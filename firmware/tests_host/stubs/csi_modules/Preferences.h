/* Fake Preferences for the canary's CSI module bridge
 * (canary/src/csi_modules_integration.cpp), which reads its module settings
 * through read-only Preferences handles (csi_module_settings_nvs.h).
 *
 * One typed store that outlives every handle, as NVS outlives a reboot:
 * test_csi_module_boot.cpp writes rows into it before a modeled boot, and
 * test_csi_modules_integration.cpp leaves it empty, so every setting reads
 * as the module's own default, as on a device that was never configured.
 * Rows are typed as Arduino-ESP32's are: getInt finds only a row putInt
 * wrote, getBool only a putBool row. It counts begin() calls and the
 * handles they opened, and the rows each get asks for; it can refuse every
 * begin(), as NVS refuses a read-only open of a namespace never created. */
#ifndef STUB_CSI_MODULES_PREFERENCES_H
#define STUB_CSI_MODULES_PREFERENCES_H

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

struct HostPrefs {
  std::map<std::string, int32_t> i32;   /* "<namespace>/<key>" */
  std::map<std::string, bool>    flag;
  std::map<std::string, float>   f32;
  bool fail_begin = false;              /* every begin() refuses */
  int  begins = 0;                      /* begin() calls, opened or refused */
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
    host_prefs().begins++;
    if (open_ || !name || host_prefs().fail_begin) return false;
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
  size_t putFloat(const char* key, float v) { return put(host_prefs().f32, key, v); }

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
