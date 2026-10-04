// tests_host/fake_lvgl/Preferences.h — the Arduino-ESP32 NVS wrapper, as
// the one-byte flags splash.cpp keeps ("scv-hello"/"met": the first
// meeting has happened). A process-wide map a test reads and clears
// (fake_prefs::uchars()); begin() always opens.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>

namespace fake_prefs {
inline std::map<std::string, uint8_t>& uchars() {
  static std::map<std::string, uint8_t> m;
  return m;
}
}  // namespace fake_prefs

class Preferences {
 public:
  bool begin(const char* ns, bool /*readOnly*/ = false) {
    ns_ = ns;
    return true;
  }
  void end() {}
  uint8_t getUChar(const char* key, uint8_t def = 0) {
    const std::map<std::string, uint8_t>::const_iterator it =
        fake_prefs::uchars().find(ns_ + "/" + key);
    return it == fake_prefs::uchars().end() ? def : it->second;
  }
  size_t putUChar(const char* key, uint8_t v) {
    fake_prefs::uchars()[ns_ + "/" + key] = v;
    return 1;
  }

 private:
  std::string ns_;
};
