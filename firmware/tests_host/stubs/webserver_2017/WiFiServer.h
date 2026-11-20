/* Host fake of WiFiServer for test_webserver_boundary.cpp: the tests hand
 * WebServer a client directly, so the listener never accepts one. */
#ifndef STUB_WEBSERVER_2017_WIFISERVER_H
#define STUB_WEBSERVER_2017_WIFISERVER_H
#include "IPAddress.h"
#include "WiFiClient.h"
class WiFiServer {
 public:
  explicit WiFiServer(int port = 80) { (void)port; }
  WiFiServer(IPAddress addr, int port) { (void)addr; (void)port; }
  void begin() {}
  void begin(uint16_t port) { (void)port; }
  void setNoDelay(bool) {}
  WiFiClient available() { return WiFiClient(); }
  void close() {}
};
#endif
