/* Preferences for the events-egress host build: one store that outlives
 * every handle (a reboot keeps it), the Arduino-ESP32 calls the egress and
 * the test's floor glue make (begin/end, getULong/putULong), and two
 * failure knobs: refuse the next begin(), or refuse every put. */
#ifndef STUB_EGRESS_PREFERENCES_H
#define STUB_EGRESS_PREFERENCES_H

#include <stdint.h>

#include <map>
#include <string>

struct HostNvs {
  std::map<std::string, uint32_t> u32;  /* "<namespace>/<key>" */
  bool fail_next_begin = false;
  bool fail_puts = false;
  int  puts = 0;                        /* puts that wrote */
};
inline HostNvs& host_nvs() {
  static HostNvs nvs;
  return nvs;
}

class Preferences {
 public:
  bool begin(const char* name, bool readOnly = false, const char* = nullptr) {
    if (open_ || !name) return false;
    if (host_nvs().fail_next_begin) {
      host_nvs().fail_next_begin = false;
      return false;
    }
    ns_ = name;
    ro_ = readOnly;
    open_ = true;
    return true;
  }
  void end() { open_ = false; }
  uint32_t getULong(const char* key, uint32_t def = 0) {
    if (!open_) return def;
    auto it = host_nvs().u32.find(ns_ + "/" + key);
    return it == host_nvs().u32.end() ? def : it->second;
  }
  size_t putULong(const char* key, uint32_t v) {
    if (!open_ || ro_ || host_nvs().fail_puts) return 0;
    host_nvs().u32[ns_ + "/" + key] = v;
    host_nvs().puts++;
    return 4;
  }

 private:
  std::string ns_;
  bool ro_ = false;
  bool open_ = false;
};

#endif
