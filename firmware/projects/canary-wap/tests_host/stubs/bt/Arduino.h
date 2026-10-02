/* Host stand-in for <Arduino.h> (the canary-wap Bluetooth channel host
 * harness, test_bluetooth_commands_wap.cpp, sweep F111). millis() reads a
 * clock the test sets; String has the calls bluetooth_channel.cpp makes.
 *
 * host_sim::task names the task a call is made on, as the test plays it:
 * "loop", "httpd" (the HTTP server's) or "nimble" (the NimBLE host task's
 * callbacks). The fakes record every radio, bond and NVS call with it. */
#ifndef STUB_BT_ARDUINO_H
#define STUB_BT_ARDUINO_H

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace host_sim {
inline uint32_t now_ms = 1000;
inline std::string task = "loop";
struct Call {
  std::string what;
  std::string task;
};
inline std::vector<Call> calls;   // radio, bond and NVS calls, in order
inline void note(const char* what) { calls.push_back({what, task}); }
inline unsigned count(const std::string& what, const std::string& on_task = "") {
  unsigned n = 0;
  for (const Call& c : calls) {
    if ((what.empty() || c.what == what) && (on_task.empty() || c.task == on_task)) ++n;
  }
  return n;
}
inline uint32_t rng = 0x12345678u;
}  // namespace host_sim

inline uint32_t millis() { return host_sim::now_ms; }
inline uint32_t esp_random() {
  host_sim::rng ^= host_sim::rng << 13;
  host_sim::rng ^= host_sim::rng >> 17;
  host_sim::rng ^= host_sim::rng << 5;
  return host_sim::rng;
}

struct HostSerial {
  template <typename... A> int printf(const char*, A...) { return 0; }
  void println(const char* = "") {}
  void print(const char*) {}
};
inline HostSerial Serial;

class String {
 public:
  String(const char* c = "") : s_(c ? c : "") {}
  String(const std::string& s) : s_(s) {}
  explicit String(unsigned long v) : s_(std::to_string(v)) {}
  explicit String(unsigned int v) : s_(std::to_string(v)) {}
  explicit String(int v) : s_(std::to_string(v)) {}
  const char* c_str() const { return s_.c_str(); }
  size_t length() const { return s_.size(); }
  void toLowerCase() {
    for (char& ch : s_) ch = (char)tolower((unsigned char)ch);
  }
  int indexOf(const char* needle) const {
    const size_t at = s_.find(needle);
    return at == std::string::npos ? -1 : (int)at;
  }
 private:
  std::string s_;
};

#endif
