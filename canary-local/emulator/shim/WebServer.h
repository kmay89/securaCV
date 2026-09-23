// canary-local/emulator/shim/WebServer.h — the ESP32 WebServer, served to
// the page instead of a socket.
//
// The surface net/provision.cpp's captive portal uses: on()/onNotFound()
// route registration, begin()/stop(), handleClient() from the firmware's own
// loop, and the request/response calls its handlers make. A request exists
// only when the page's phone sends one (emu_http_request(), emu_webserver.cpp);
// handleClient() pops it, the FIRMWARE's handler runs and answers, and the
// answer — status, headers, every body byte — goes back to the page through
// Module.onHttpResponse. The shim routes and parses (path, query, a
// form-urlencoded body, the way the Arduino core does); it never writes a
// response of its own beyond what the core itself would (the 404 when no
// route and no onNotFound match; a status 0 "no response" when a handler
// sends nothing or the server stops with a request still queued).
#pragma once

#include <stdint.h>

#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "WString.h"

#ifndef PGM_P
#define PGM_P const char*
#endif

// http_parser's method numbering, as arduino-esp32 2.x's HTTP_Method.h
// re-exports it (HTTP_ANY is the core's own wildcard).
enum HTTPMethod {
  HTTP_DELETE = 0,
  HTTP_GET = 1,
  HTTP_HEAD = 2,
  HTTP_POST = 3,
  HTTP_PUT = 4,
  HTTP_OPTIONS = 6,
  HTTP_PATCH = 28,
  HTTP_ANY = 255,
};

class WebServer {
 public:
  typedef std::function<void(void)> THandlerFunction;

  explicit WebServer(int port = 80);
  ~WebServer();

  void begin();
  void stop();
  void handleClient();

  void on(const String& uri, THandlerFunction fn) { on(uri, HTTP_ANY, fn); }
  void on(const String& uri, HTTPMethod method, THandlerFunction fn);
  void onNotFound(THandlerFunction fn) { not_found_ = fn; }

  // The request being handled.
  String uri() const { return String(uri_); }
  HTTPMethod method() const { return method_; }
  String arg(const String& name) const;
  bool hasArg(const String& name) const;
  int args() const { return (int)args_.size(); }

  // The response.
  void sendHeader(const String& name, const String& value, bool first = false);
  void send(int code, const char* content_type = nullptr,
            const String& content = String(""));
  void send_P(int code, PGM_P content_type, PGM_P content) {
    send(code, content_type, String(content));
  }

  // Page side (emu_webserver.cpp's export): queue one request for this
  // server. Returns false when it is not listening (connection refused).
  bool enqueue(int id, HTTPMethod method, const char* target,
               const char* content_type, const char* body);
  int port() const { return port_; }
  bool listening() const { return listening_; }

 private:
  struct Route {
    std::string path;
    HTTPMethod method;
    THandlerFunction fn;
  };
  struct Request {
    int id;
    HTTPMethod method;
    std::string target;  // path + optional ?query, exactly as sent
    std::string content_type;
    std::string body;
  };

  void parse(const Request& rq);
  void respond(int code, const char* content_type, const char* body);

  int port_;
  bool listening_ = false;
  std::vector<Route> routes_;
  THandlerFunction not_found_;
  std::deque<Request> queue_;

  // Current request/response.
  int cur_id_ = 0;
  bool answered_ = true;
  std::string uri_;
  HTTPMethod method_ = HTTP_GET;
  std::vector<std::pair<std::string, std::string>> args_;
  std::string headers_;  // "Name: value\n" lines queued by sendHeader()
};
