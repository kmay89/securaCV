/* Host fake of the FS library for test_webserver_boundary.cpp:
 * WebServer.cpp's serveStatic() compiles against it; no test serves a file. */
#ifndef STUB_WEBSERVER_2017_FS_H
#define STUB_WEBSERVER_2017_FS_H
#include "Arduino.h"
namespace fs {
class File {
 public:
  size_t size() const { return 0; }
  const char* name() const { return ""; }
  bool isDirectory() const { return false; }
  int available() const { return 0; }
  void close() {}
  operator bool() const { return false; }
};
class FS {
 public:
  bool exists(const String&) { return false; }
  File open(const String&, const char* mode = "r") { (void)mode; return File(); }
};
}  // namespace fs
using fs::File;
using fs::FS;
#endif
