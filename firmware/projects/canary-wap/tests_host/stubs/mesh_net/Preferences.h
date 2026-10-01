/* Host stand-in for the Arduino-ESP32 Preferences (NVS) class: one
 * in-memory store per simulated device (host_sim::nvs points at the
 * current one). getBytes refuses a buffer smaller than the value, as the
 * real one does. A test can count the writes each key took
 * (host_sim::nvs_writes, "<namespace>/<key>") and make every write fail
 * (host_sim::nvs_writes_fail): on the Arduino-ESP32 cores canary-wap
 * builds with (PlatformIO's pinned 3.3.8, the Arduino CLI leg's latest
 * 3.x), putBytes returns 0 when nvs_set_blob or nvs_commit fails (a full
 * or worn partition), and the value is not stored. */
#ifndef STUB_MESH_NET_PREFERENCES_H
#define STUB_MESH_NET_PREFERENCES_H

#include <map>
#include <string>
#include <vector>

#include "Arduino.h"

namespace host_sim {
using NvsStore = std::map<std::string, std::vector<uint8_t>>;
inline NvsStore default_nvs;
inline NvsStore* nvs = &default_nvs;
inline std::map<std::string, unsigned> nvs_writes;
inline bool nvs_writes_fail = false;
}  // namespace host_sim

class Preferences {
 public:
  bool begin(const char* name, bool read_only = false) {
    ns_ = name ? name : "";
    ro_ = read_only;
    return true;
  }
  void end() {}
  bool isKey(const char* key) { return host_sim::nvs->count(k(key)) != 0; }
  bool remove(const char* key) { return !ro_ && host_sim::nvs->erase(k(key)) != 0; }
  size_t putBytes(const char* key, const void* v, size_t n) {
    if (ro_ || host_sim::nvs_writes_fail) return 0;
    const uint8_t* b = static_cast<const uint8_t*>(v);
    (*host_sim::nvs)[k(key)].assign(b, b + n);
    ++host_sim::nvs_writes[k(key)];
    return n;
  }
  size_t getBytes(const char* key, void* buf, size_t max_len) {
    auto it = host_sim::nvs->find(k(key));
    if (it == host_sim::nvs->end() || it->second.size() > max_len) return 0;
    if (!it->second.empty()) memcpy(buf, it->second.data(), it->second.size());
    return it->second.size();
  }
  size_t putBool(const char* key, bool v) { uint8_t b = v ? 1 : 0; return putBytes(key, &b, 1); }
  bool getBool(const char* key, bool def = false) {
    uint8_t b = 0;
    return getBytes(key, &b, 1) == 1 ? b != 0 : def;
  }
  size_t putUChar(const char* key, uint8_t v) { return putBytes(key, &v, 1); }
  uint8_t getUChar(const char* key, uint8_t def = 0) {
    uint8_t b = 0;
    return getBytes(key, &b, 1) == 1 ? b : def;
  }
  size_t putString(const char* key, const char* v) { return putBytes(key, v, strlen(v)); }
  String getString(const char* key, const char* def = "") {
    auto it = host_sim::nvs->find(k(key));
    if (it == host_sim::nvs->end()) return String(def);
    std::string s(it->second.begin(), it->second.end());
    return String(s.c_str());
  }
 private:
  std::string k(const char* key) const { return ns_ + "/" + key; }
  std::string ns_;
  bool ro_ = false;
};

#endif
