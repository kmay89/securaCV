/* Preferences for the MQTT bridge host build: one in-memory NVS (every
 * value as bytes under "<namespace>/<key>") with the typed calls
 * csi_mqtt.cpp makes. */
#ifndef STUB_MQTT_PREFERENCES_H
#define STUB_MQTT_PREFERENCES_H

#include <stdint.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

inline std::map<std::string, std::vector<uint8_t>>& stub_nvs() {
  static std::map<std::string, std::vector<uint8_t>> nvs;
  return nvs;
}

class Preferences {
 public:
  bool begin(const char* name, bool read_only = false, const char* = nullptr) {
    ns_ = name ? name : "";
    ro_ = read_only;
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
