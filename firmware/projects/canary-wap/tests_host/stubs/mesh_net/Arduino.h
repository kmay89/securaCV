/* Host stand-in for <Arduino.h> (canary-wap mesh_network host harness).
 * millis() reads a clock the test sets; esp_fill_random is a fixed-seed
 * xorshift, so every run draws the same keys and nonces. */
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

inline uint32_t millis() { return host_sim::now_ms; }
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
