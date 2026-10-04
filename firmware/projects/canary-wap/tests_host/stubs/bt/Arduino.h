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

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

/* The threaded test (sweep F143: the NimBLE host task's callbacks on one
 * thread, the loop task on another, run under -fsanitize=thread by `make
 * tsan-bt-commands`) shares what is here: the clock is atomic, the task
 * name is each thread's own, and the call record takes a lock, so the only
 * races left to find are the channel's own. */
namespace host_sim {
inline std::atomic<uint32_t> now_ms{1000};
inline thread_local std::string task = "loop";
struct Call {
  std::string what;
  std::string task;
};
inline std::mutex calls_mu;
inline std::vector<Call> calls;   // radio, bond and NVS calls, in order
inline void note(const char* what) {
  std::lock_guard<std::mutex> g(calls_mu);
  calls.push_back({what, task});
}
inline unsigned count(const std::string& what, const std::string& on_task = "") {
  std::lock_guard<std::mutex> g(calls_mu);
  unsigned n = 0;
  for (const Call& c : calls) {
    if ((what.empty() || c.what == what) && (on_task.empty() || c.task == on_task)) ++n;
  }
  return n;
}
inline std::atomic<uint32_t> rng{0x12345678u};
}  // namespace host_sim

inline uint32_t millis() { return host_sim::now_ms.load(); }
inline uint32_t esp_random() {
  uint32_t x = host_sim::rng.load();
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  host_sim::rng.store(x);
  return x;
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
  void toCharArray(char* buf, unsigned int size) const {
    if (size == 0) return;
    const size_t n = s_.size() < size - 1 ? s_.size() : size - 1;
    memcpy(buf, s_.data(), n);
    buf[n] = '\0';
  }
  int indexOf(const char* needle) const {
    const size_t at = s_.find(needle);
    return at == std::string::npos ? -1 : (int)at;
  }
 private:
  std::string s_;
};

#endif
