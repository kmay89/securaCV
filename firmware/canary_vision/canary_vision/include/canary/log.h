#pragma once
#include <Arduino.h>

static inline uint32_t ms_now() { return millis(); }

static inline void log_header(const char* tag) {
  Serial.printf("[%08lu][%s] ", (unsigned long)ms_now(), tag);
}

static inline void log_line(const char* tag, const char* msg) {
  log_header(tag);
  Serial.println(msg);
}

static inline void log_kv(const char* tag, const char* k, const char* v) {
  log_header(tag);
  Serial.printf("%s=%s\n", k, v);
}
