// canary-local/emulator/test/native9/Preferences.h — the Arduino-ESP32 NVS
// wrapper as splash.cpp uses it (F246): one-byte flags ("scv-hello"/"met",
// the first meeting has happened) in a process-wide map the test clears
// before each splash (native9::prefs()). begin() always opens.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>

namespace native9 {
inline std::map<std::string, uint8_t>& prefs() {
  static std::map<std::string, uint8_t> m;
  return m;
}
}  // namespace native9

class Preferences {
 public:
  bool begin(const char* ns, bool /*readOnly*/ = false) {
    ns_ = ns;
    return true;
  }
  void end() {}
  uint8_t getUChar(const char* key, uint8_t def = 0) {
    const auto it = native9::prefs().find(ns_ + "/" + key);
    return it == native9::prefs().end() ? def : it->second;
  }
  size_t putUChar(const char* key, uint8_t v) {
    native9::prefs()[ns_ + "/" + key] = v;
    return 1;
  }

 private:
  std::string ns_;
};
