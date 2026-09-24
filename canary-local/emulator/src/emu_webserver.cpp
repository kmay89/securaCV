// canary-local/emulator/src/emu_webserver.cpp — WebServer, answered to the
// page.
//
// The first-boot portal (net/provision.cpp) runs verbatim; this is the socket
// under it. The page's phone calls emu_http_request(); the request waits in
// the server's queue until the firmware's own loop calls handleClient(); the
// firmware's handler answers; js_http_response() hands the status, headers and
// body to Module.onHttpResponse. One request per handleClient() call, the way
// the core serves one client per pass — so the JS side never re-enters the
// firmware, and a request landing while the firmware sleeps in delay() just
// waits its turn in the queue (Asyncify-safe: nothing here unwinds).
//
// Parsing follows the Arduino core's own rules (Parsing.cpp): the target
// splits at '?', arguments are '&'-separated key=value pairs, '+' decodes to a
// space and %XX to its byte, and an application/x-www-form-urlencoded body
// joins the query's arguments; any other body is the "plain" argument.
#include <WebServer.h>

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace {

EM_JS(void, js_http_response,
      (int id, int status, const char* ctype, const char* body,
       const char* headers),
      {
        if (Module.onHttpResponse)
          Module.onHttpResponse(id, status, UTF8ToString(ctype),
                                UTF8ToString(body), UTF8ToString(headers));
      });

// Every constructed server, so the page's request can find the one bound to
// the port it dials. The portal's WebServer lives in provision_run()'s scope
// and unregisters itself on the way out.
std::vector<WebServer*>& servers() {
  static std::vector<WebServer*> v;
  return v;
}

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string url_decode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    const char c = in[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && i + 2 < in.size() && hexval(in[i + 1]) >= 0 &&
               hexval(in[i + 2]) >= 0) {
      out.push_back((char)((hexval(in[i + 1]) << 4) | hexval(in[i + 2])));
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

void parse_args(const std::string& s,
                std::vector<std::pair<std::string, std::string>>& out) {
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t amp = s.find('&', pos);
    if (amp == std::string::npos) amp = s.size();
    const std::string pair = s.substr(pos, amp - pos);
    if (!pair.empty()) {
      const size_t eq = pair.find('=');
      if (eq == std::string::npos) {
        out.emplace_back(url_decode(pair), std::string());
      } else {
        out.emplace_back(url_decode(pair.substr(0, eq)),
                         url_decode(pair.substr(eq + 1)));
      }
    }
    pos = amp + 1;
  }
}

HTTPMethod method_of(const char* m) {
  if (!m) return HTTP_GET;
  if (!strcmp(m, "GET")) return HTTP_GET;
  if (!strcmp(m, "POST")) return HTTP_POST;
  if (!strcmp(m, "HEAD")) return HTTP_HEAD;
  if (!strcmp(m, "PUT")) return HTTP_PUT;
  if (!strcmp(m, "DELETE")) return HTTP_DELETE;
  if (!strcmp(m, "PATCH")) return HTTP_PATCH;
  if (!strcmp(m, "OPTIONS")) return HTTP_OPTIONS;
  return HTTP_ANY;  // an unknown verb matches no method-specific route
}

int g_next_id = 1;

}  // namespace

WebServer::WebServer(int port) : port_(port) { servers().push_back(this); }

WebServer::~WebServer() {
  stop();
  auto& v = servers();
  for (size_t i = 0; i < v.size(); i++) {
    if (v[i] == this) {
      v.erase(v.begin() + (long)i);
      break;
    }
  }
}

void WebServer::begin() { listening_ = true; }

void WebServer::stop() {
  listening_ = false;
  // A request the firmware will now never read gets the answer a closed
  // socket gives: none. The page must not be left waiting on it.
  while (!queue_.empty()) {
    js_http_response(queue_.front().id, 0, "", "", "");
    queue_.pop_front();
  }
}

void WebServer::on(const String& uri, HTTPMethod method, THandlerFunction fn) {
  routes_.push_back(Route{std::string(uri.c_str()), method, fn});
}

bool WebServer::enqueue(int id, HTTPMethod method, const char* target,
                        const char* content_type, const char* body) {
  if (!listening_) return false;
  queue_.push_back(Request{id, method, target ? target : "/",
                           content_type ? content_type : "",
                           body ? body : ""});
  return true;
}

void WebServer::parse(const Request& rq) {
  args_.clear();
  headers_.clear();
  method_ = rq.method;
  const size_t q = rq.target.find('?');
  uri_ = q == std::string::npos ? rq.target : rq.target.substr(0, q);
  if (q != std::string::npos) parse_args(rq.target.substr(q + 1), args_);
  if (!rq.body.empty()) {
    if (rq.content_type.find("application/x-www-form-urlencoded") !=
        std::string::npos) {
      parse_args(rq.body, args_);
    } else {
      args_.emplace_back("plain", rq.body);
    }
  }
}

void WebServer::handleClient() {
  if (!listening_ || queue_.empty()) return;
  const Request rq = queue_.front();
  queue_.pop_front();
  parse(rq);
  cur_id_ = rq.id;
  answered_ = false;

  bool handled = false;
  for (const Route& r : routes_) {
    if (r.path == uri_ && (r.method == HTTP_ANY || r.method == method_)) {
      if (r.fn) r.fn();
      handled = true;
      break;
    }
  }
  if (!handled) {
    if (not_found_) {
      not_found_();
    } else {
      // The core's own fallback when nothing matches and no onNotFound is set.
      const std::string msg = "Not found: " + uri_;
      send(404, "text/plain", String(msg.c_str()));
    }
  }
  if (!answered_) {
    // A handler that sends nothing leaves the client with a closed socket.
    js_http_response(cur_id_, 0, "", "", headers_.c_str());
    answered_ = true;
  }
}

String WebServer::arg(const String& name) const {
  for (const auto& kv : args_) {
    if (kv.first == name.c_str()) return String(kv.second.c_str());
  }
  return String("");
}

bool WebServer::hasArg(const String& name) const {
  for (const auto& kv : args_) {
    if (kv.first == name.c_str()) return true;
  }
  return false;
}

void WebServer::sendHeader(const String& name, const String& value, bool first) {
  const std::string line =
      std::string(name.c_str()) + ": " + std::string(value.c_str()) + "\n";
  headers_ = first ? line + headers_ : headers_ + line;
}

void WebServer::send(int code, const char* content_type, const String& content) {
  respond(code, content_type ? content_type : "text/html", content.c_str());
}

void WebServer::respond(int code, const char* content_type, const char* body) {
  if (answered_) return;  // one response per request, as on a socket
  answered_ = true;
  js_http_response(cur_id_, code, content_type, body, headers_.c_str());
  headers_.clear();
}

// ── The page's side ─────────────────────────────────────────────────────
extern "C" {
// Dial the server listening on `port` with one request. Returns the request
// id Module.onHttpResponse will answer, or 0 when nothing listens there
// (connection refused — e.g. the portal is not up, or has closed).
EMSCRIPTEN_KEEPALIVE int emu_http_request(int port, const char* method,
                                          const char* target,
                                          const char* content_type,
                                          const char* body) {
  for (WebServer* s : servers()) {
    if (s->port() != port || !s->listening()) continue;
    const int id = g_next_id++;
    if (g_next_id <= 0) g_next_id = 1;
    return s->enqueue(id, method_of(method), target, content_type, body) ? id
                                                                         : 0;
  }
  return 0;
}
}
