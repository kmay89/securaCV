#pragma once
// Arduino-ESP32 stand-in for the Improv glue host tests (test_improv_glue.cpp
// over firmware/common/network/improv_ble.cpp, test_ble_improv_command_wap.cpp
// over the WAP's health_log.h): only the surface those reach, in the core's
// shape (cores/esp32 WString.h and Print.h, the same in 2.0.17 and 3.x).
// Serial keeps every line it is handed, so a test reads the glue's words.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

class String {
 public:
  String() {}
  String(const char* c) : s_(c ? c : "") {}
  const char* c_str() const { return s_.c_str(); }
  unsigned int length() const { return (unsigned int)s_.size(); }

 private:
  std::string s_;
};

// millis(): 32 bits, as the chip's; the test's clock (the glue itself reads
// the now_ms it is handed, WiFi.h's scan give-up reads this).
inline uint32_t g_millis = 0;
inline uint32_t millis() { return g_millis; }

// HardwareSerial / HWCDC through Print: println(const char[]) and a
// printf the compiler checks against its format, as the core declares it.
class SerialT {
 public:
  std::vector<std::string> lines;
  size_t println(const char t[]) {
    lines.push_back(t ? t : "");
    return lines.back().size() + 2;
  }
  __attribute__((format(printf, 2, 3))) size_t printf(const char* format, ...) {
    char b[512];
    va_list ap;
    va_start(ap, format);
    const int n = vsnprintf(b, sizeof b, format, ap);
    va_end(ap);
    std::string l(b);
    if (!l.empty() && l.back() == '\n') l.pop_back();
    lines.push_back(l);
    return n < 0 ? 0 : (size_t)n;
  }
};
extern SerialT Serial;
