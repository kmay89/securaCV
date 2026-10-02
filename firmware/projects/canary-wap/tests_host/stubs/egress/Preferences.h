/* Preferences for the events-egress host build: one store that outlives
 * every handle (a reboot keeps it), the Arduino-ESP32 calls the egress and
 * the test's floor glue make (begin/end, getULong/putULong), and two
 * failure knobs: refuse the next begin(), or refuse every put.
 *
 * Namespaces are NVS's (sweep F150): one exists once a read-write begin()
 * created it or a row is stored in it, and a read-only begin() of one that
 * does not exist fails, as nvs_open() answers ESP_ERR_NVS_NOT_FOUND, and
 * counts one error log, as Arduino-ESP32's Preferences::begin() logs
 * "nvs_open failed: ..." at error level for each failed open. nvs.h beside
 * this file answers IDF's nvs_open() from the same store, and logs
 * nothing. */
#ifndef STUB_EGRESS_PREFERENCES_H
#define STUB_EGRESS_PREFERENCES_H

#include <stdint.h>

#include <map>
#include <set>
#include <string>

struct HostNvs {
  std::map<std::string, uint32_t> u32;  /* "<namespace>/<key>" */
  std::set<std::string> created;        /* namespaces a read-write begin() made */
  bool fail_next_begin = false;
  bool fail_puts = false;
  int  puts = 0;                        /* puts that wrote */
  int  error_logs = 0;                  /* failed begin()s: Preferences logs each */
  bool has_namespace(const std::string& ns) const {
    if (created.count(ns) != 0) return true;
    const std::string prefix = ns + "/";
    auto it = u32.lower_bound(prefix);
    return it != u32.end() && it->first.compare(0, prefix.size(), prefix) == 0;
  }
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
      host_nvs().error_logs++;
      return false;
    }
    if (readOnly && !host_nvs().has_namespace(name)) {
      host_nvs().error_logs++;          /* log_e("nvs_open failed: NOT_FOUND") */
      return false;
    }
    if (!readOnly) host_nvs().created.insert(name);
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
