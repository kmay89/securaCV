/* Host fake of WiFiClient for test_webserver_boundary.cpp: the request bytes
 * a test feeds are the socket. Copies share one connection, as the real
 * client's copies share one socket (WebServer copies its client). connected()
 * turns false once every fed byte is read: the peer sent its request and
 * closed, which also ends 2.0.17's _uploadReadByte wait instead of spinning
 * on a clock that never moves here. */
#ifndef STUB_WEBSERVER_2017_WIFICLIENT_H
#define STUB_WEBSERVER_2017_WIFICLIENT_H

#include <memory>
#include <string>

#include "Arduino.h"
#include "IPAddress.h"

class WiFiClient {
 public:
  WiFiClient() = default;
  explicit WiFiClient(const std::string& request) : conn_(std::make_shared<Conn>()) { conn_->in = request; }

  int available() { return conn_ ? static_cast<int>(conn_->in.size() - conn_->pos) : 0; }
  int read() { return available() > 0 ? static_cast<unsigned char>(conn_->in[conn_->pos++]) : -1; }
  size_t readBytes(char* buf, size_t n) {
    size_t got = 0;
    while (got < n && available() > 0) buf[got++] = static_cast<char>(read());
    return got;
  }
  size_t readBytes(uint8_t* buf, size_t n) { return readBytes(reinterpret_cast<char*>(buf), n); }
  String readStringUntil(char terminator) {
    std::string out;
    int c;
    while ((c = read()) >= 0 && c != terminator) out += static_cast<char>(c);
    return String(out);
  }
  uint8_t connected() { return available() > 0 ? 1 : 0; }
  void flush() {}
  void stop() {}
  void setTimeout(unsigned long) {}
  IPAddress localIP() const { return IPAddress(); }
  size_t write(const char* b, size_t l) {
    if (conn_) conn_->out.append(b, l);
    return l;
  }
  size_t write_P(PGM_P b, size_t l) { return write(b, l); }
  template <typename T>
  size_t write(T&) { return 0; }  // streamFile's write(File&): no test serves a file
  operator bool() const { return static_cast<bool>(conn_); }

  // Test-side views of the connection.
  size_t consumed() const { return conn_ ? conn_->pos : 0; }
  const std::string& sent() const { return conn_->out; }

 private:
  struct Conn {
    std::string in;
    size_t pos = 0;
    std::string out;
  };
  std::shared_ptr<Conn> conn_;
};

#endif
