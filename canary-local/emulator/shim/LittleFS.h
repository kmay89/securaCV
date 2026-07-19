// canary-local/emulator/shim/LittleFS.h — journal persistence stub.
// FEATURE_TIME_MACHINE_PERSIST is 0 on both flavors today (bench-gated),
// so the journal rides its RAM ring; this shim exists to satisfy the
// include and honestly reports "not mounted" if anything asks.
#pragma once

#include <stddef.h>
#include <stdint.h>

class File {
 public:
  operator bool() const { return false; }
  size_t write(const uint8_t*, size_t) { return 0; }
  size_t read(uint8_t*, size_t) { return 0; }
  int available() { return 0; }
  void close() {}
  size_t size() { return 0; }
};

class LittleFSClass {
 public:
  bool begin(bool = false) { return false; }
  File open(const char*, const char* = "r") { return File(); }
  bool exists(const char*) { return false; }
  bool remove(const char*) { return false; }
  bool rename(const char*, const char*) { return false; }
};

extern LittleFSClass LittleFS;
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
