/* Host fake of IPAddress for test_webserver_boundary.cpp. */
#ifndef STUB_WEBSERVER_2017_IPADDRESS_H
#define STUB_WEBSERVER_2017_IPADDRESS_H
#include <stdint.h>
#include "Arduino.h"
class IPAddress {
 public:
  IPAddress() = default;
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : v_((uint32_t)a | (uint32_t)b << 8 | (uint32_t)c << 16 | (uint32_t)d << 24) {}
  operator uint32_t() const { return v_; }
  String toString() const {
    return String(std::to_string(v_ & 0xff) + "." + std::to_string((v_ >> 8) & 0xff) + "." +
                  std::to_string((v_ >> 16) & 0xff) + "." + std::to_string(v_ >> 24));
  }
 private:
  uint32_t v_ = 0;
};
#endif
