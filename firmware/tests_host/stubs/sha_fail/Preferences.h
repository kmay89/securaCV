/* A fake Preferences for test_sha_fail_flagship.cpp and
 * test_key_draw_check.cpp (sweeps F455, F38): one store of blobs and u32s,
 * kept across handles as NVS keeps it across a reboot, with every put
 * counted, so a suite can say a refused hash or key draw wrote nothing.
 * Only the calls the cut code makes: begin/end, getBytesLength, getBytes,
 * putBytes, getULong, putULong. begin() refuses when the test says so.
 * securacv_crypto.h also names the type (NvsManager's member). */
#ifndef STUB_SHA_FAIL_PREFERENCES_H
#define STUB_SHA_FAIL_PREFERENCES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

struct HostPrefsStore {
  std::map<std::string, std::vector<uint8_t>> blobs;
  std::map<std::string, uint32_t> u32s;
  bool begin_fails = false;
  int begins = 0, puts = 0, open = 0;
  void reset() { *this = HostPrefsStore(); }
};
inline HostPrefsStore& host_prefs() {
  static HostPrefsStore s;
  return s;
}

class Preferences {
 public:
  bool begin(const char* name, bool readOnly = false) {
    (void)name;
    HostPrefsStore& s = host_prefs();
    s.begins++;
    if (s.begin_fails || started_) return false;
    started_ = true;
    read_only_ = readOnly;
    s.open++;
    return true;
  }
  void end() {
    if (started_) host_prefs().open--;
    started_ = false;
  }
  size_t getBytesLength(const char* key) {
    if (!started_) return 0;
    auto it = host_prefs().blobs.find(key);
    return it == host_prefs().blobs.end() ? 0 : it->second.size();
  }
  size_t getBytes(const char* key, void* buf, size_t len) {
    const size_t have = getBytesLength(key);
    if (have == 0 || have > len) return 0;
    memcpy(buf, host_prefs().blobs[key].data(), have);
    return have;
  }
  size_t putBytes(const char* key, const void* value, size_t len) {
    HostPrefsStore& s = host_prefs();
    s.puts++;
    if (!started_ || read_only_ || len == 0) return 0;
    const uint8_t* b = static_cast<const uint8_t*>(value);
    s.blobs[key] = std::vector<uint8_t>(b, b + len);
    return len;
  }
  uint32_t getULong(const char* key, uint32_t def = 0) {
    if (!started_) return def;
    auto it = host_prefs().u32s.find(key);
    return it == host_prefs().u32s.end() ? def : it->second;
  }
  size_t putULong(const char* key, uint32_t value) {
    HostPrefsStore& s = host_prefs();
    s.puts++;
    if (!started_ || read_only_) return 0;
    s.u32s[key] = value;
    return 4;
  }

 private:
  bool started_ = false;
  bool read_only_ = false;
};

#endif
