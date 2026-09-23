// canary-local/emulator/shim/WiFiUdp.h — a UDP socket the page can reach.
//
// Exactly the WiFiUDP surface net/provision.cpp's captive DNS uses (bind
// :53, drain datagrams, answer each to its sender). Nothing here reaches a
// network — the emulator has none and must not grow one (Invariant IV). A
// datagram arrives only when the page's phone sends one through
// emu_dns_query() (emu_radio.cpp), and the firmware's answer — built by the
// shared, host-tested dns_build_response() — goes back to the page through
// Module.onUdpSend. The shim carries bytes; it never looks inside them.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "WiFi.h"

class WiFiUDP {
 public:
  WiFiUDP() {}
  ~WiFiUDP() { stop(); }

  uint8_t begin(uint16_t port);
  void stop();

  // Receive: pop the next queued datagram for this port; 0 when none.
  int parsePacket();
  int read(uint8_t* buf, size_t len);
  int read(char* buf, size_t len) { return read((uint8_t*)buf, len); }
  IPAddress remoteIP() { return rx_ip_; }
  uint16_t remotePort() { return rx_port_; }

  // Send: collect one datagram, hand it to the page on endPacket().
  int beginPacket(IPAddress ip, uint16_t port);
  size_t write(const uint8_t* buf, size_t size);
  size_t write(uint8_t b) { return write(&b, 1); }
  int endPacket();

 private:
  uint16_t port_ = 0;
  std::vector<uint8_t> rx_;
  size_t rx_off_ = 0;
  IPAddress rx_ip_;
  uint16_t rx_port_ = 0;
  std::vector<uint8_t> tx_;
  IPAddress tx_ip_;
  uint16_t tx_port_ = 0;
  bool tx_open_ = false;
};
