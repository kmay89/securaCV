/* Host stand-in for <Arduino.h> (canary-wap mesh_network host harness).
 * millis() reads a clock the test sets; esp_fill_random is a fixed-seed
 * xorshift, so every run draws the same keys and nonces.
 *
 * millis_step_ms: a test that sets it makes every millis() call move that
 * clock on by that much after it reads it, so two reads in one pass differ
 * as they can on a device (test_chirp_commands_wap's edge tests). 0, the
 * default, keeps the clock where the test put it.
 *
 * on_httpd_task: a test sets it while it plays the HTTP server's task
 * (test_mesh_commands_wap, sweep F96). Every NVS write and ESP-NOW peer or
 * send call made meanwhile is counted in httpd_side_effects: those belong
 * to the loop task. */
#ifndef STUB_MESH_NET_ARDUINO_H
#define STUB_MESH_NET_ARDUINO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace host_sim {
inline uint32_t now_ms = 1000;
inline uint64_t rng_state = 0x9E3779B97F4A7C15ull;
inline uint32_t millis_step_ms = 0;
inline bool on_httpd_task = false;
inline unsigned httpd_side_effects = 0;
inline void note_side_effect() {
  if (on_httpd_task) ++httpd_side_effects;
}
inline void fill_random(void* buf, size_t len) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  for (size_t i = 0; i < len; ++i) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    p[i] = static_cast<uint8_t>(rng_state >> 24);
  }
}
}  // namespace host_sim

inline uint32_t millis() {
  const uint32_t t = host_sim::now_ms;
  host_sim::now_ms += host_sim::millis_step_ms;
  return t;
}
inline void esp_fill_random(void* buf, size_t len) { host_sim::fill_random(buf, len); }

struct HostSerial {
  template <typename... A> int printf(const char*, A...) { return 0; }
  void println(const char* = "") {}
  void print(const char*) {}
};
inline HostSerial Serial;

class String {
 public:
  String(const char* c = "") : s_(c ? c : "") {}
  const char* c_str() const { return s_.c_str(); }
  size_t length() const { return s_.size(); }
 private:
  std::string s_;
};

#endif
