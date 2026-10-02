/* Preferences for the MQTT bridge host build: one in-memory NVS (every
 * value as bytes under "<namespace>/<key>") with the typed calls
 * csi_mqtt.cpp makes.
 *
 * Namespaces are NVS's (sweep F150): one exists once a read-write begin()
 * created it or a value is stored in it, and a read-only begin() of one that
 * does not exist fails, as nvs_open() answers ESP_ERR_NVS_NOT_FOUND, and
 * counts one error log (stub_nvs_error_logs()), as Arduino-ESP32's
 * Preferences::begin() logs "nvs_open failed: ..." at error level. nvs.h
 * beside this file answers IDF's nvs_open() from the same store. */
#ifndef STUB_MQTT_PREFERENCES_H
#define STUB_MQTT_PREFERENCES_H

#include <stdint.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

inline std::map<std::string, std::vector<uint8_t>>& stub_nvs() {
  static std::map<std::string, std::vector<uint8_t>> nvs;
  return nvs;
}
inline std::set<std::string>& stub_nvs_created() {
  static std::set<std::string> created;
  return created;
}
inline int& stub_nvs_error_logs() {
  static int logs = 0;
  return logs;
}
inline bool stub_nvs_has_namespace(const std::string& ns) {
  if (stub_nvs_created().count(ns) != 0) return true;
  const std::string prefix = ns + "/";
  auto it = stub_nvs().lower_bound(prefix);
  return it != stub_nvs().end() && it->first.compare(0, prefix.size(), prefix) == 0;
}

class Preferences {
 public:
  bool begin(const char* name, bool read_only = false, const char* = nullptr) {
    ns_ = name ? name : "";
    ro_ = read_only;
    if (read_only && !stub_nvs_has_namespace(ns_)) {
      ++stub_nvs_error_logs();          /* log_e("nvs_open failed: NOT_FOUND") */
      return false;
    }
    if (!read_only) stub_nvs_created().insert(ns_);
    return true;
  }
  void end() {}
  bool isKey(const char* key) { return stub_nvs().count(k(key)) != 0; }
  bool remove(const char* key) { return !ro_ && stub_nvs().erase(k(key)) != 0; }

  size_t putBool(const char* key, bool v) { return put(key, v); }
  size_t putUChar(const char* key, uint8_t v) { return put(key, v); }
  size_t putUShort(const char* key, uint16_t v) { return put(key, v); }
  size_t putULong(const char* key, uint32_t v) { return put(key, v); }
  size_t putString(const char* key, const char* v) {
    if (ro_) return 0;
    const size_t n = strlen(v);
    stub_nvs()[k(key)].assign(v, v + n);
    return n == 0 ? 1 : n;
  }
  bool getBool(const char* key, bool def = false) { return get(key, def); }
  uint8_t getUChar(const char* key, uint8_t def = 0) { return get(key, def); }
  uint16_t getUShort(const char* key, uint16_t def = 0) { return get(key, def); }
  uint32_t getULong(const char* key, uint32_t def = 0) { return get(key, def); }
  size_t getString(const char* key, char* out, size_t cap) {
    if (cap == 0) return 0;
    auto it = stub_nvs().find(k(key));
    if (it == stub_nvs().end()) {
      out[0] = '\0';
      return 0;
    }
    const size_t n = it->second.size() < cap - 1 ? it->second.size() : cap - 1;
    if (n) memcpy(out, it->second.data(), n);
    out[n] = '\0';
    return n;
  }

 private:
  template <typename T>
  size_t put(const char* key, T v) {
    if (ro_) return 0;
    std::vector<uint8_t>& b = stub_nvs()[k(key)];
    b.resize(sizeof v);
    memcpy(b.data(), &v, sizeof v);
    return sizeof v;
  }
  template <typename T>
  T get(const char* key, T def) {
    auto it = stub_nvs().find(k(key));
    if (it == stub_nvs().end() || it->second.size() != sizeof(T)) return def;
    T v;
    memcpy(&v, it->second.data(), sizeof v);
    return v;
  }
  std::string k(const char* key) const { return ns_ + "/" + key; }
  std::string ns_;
  bool ro_ = false;
};

#endif
