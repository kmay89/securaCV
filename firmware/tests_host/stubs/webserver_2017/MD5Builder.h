/* Host fake of MD5Builder for test_webserver_boundary.cpp: WebServer.cpp's
 * digest-auth path links it; no test reaches that path. */
#ifndef STUB_WEBSERVER_2017_MD5BUILDER_H
#define STUB_WEBSERVER_2017_MD5BUILDER_H
#include "Arduino.h"
class MD5Builder {
 public:
  void begin() {}
  void add(const String&) {}
  void calculate() {}
  String toString() { return String("00000000000000000000000000000000"); }
};
#endif
