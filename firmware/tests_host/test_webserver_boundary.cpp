// Host test for firmware/third_party/WebServer-2.0.17-securacv: arduino-esp32
// 2.0.17's WebServer library with the GHSA-8cmm-3887-r32j multipart-boundary
// fix backported (that directory's README.md says exactly what changed). The
// library's own WebServer.cpp, Parsing.cpp and detail/mimetable.cpp are
// compiled here against host fakes (stubs/webserver_2017/), under
// AddressSanitizer, and each request goes through a fake socket into
// WebServer::_parseRequest, which handleClient() runs before any route
// handler (and so before any Host/Origin guard of ours) gets the request.
//
// What it holds:
//  1. A boundary longer than RFC 2046's 70 characters is refused in
//     _parseRequest before _parseForm reads one byte of the body: on a URI no
//     handler claims (2.0.17 parses a multipart body for any URI, so the
//     advisory's upload route is not needed) and on an upload route, from 71
//     characters up to the advisory's shape (8 KB and more, past Vision's
//     8 KB loop stack and the displays' 24 KB one).
//  2. _parseForm refuses one too when it is handed one directly (the guard
//     beside the buffer it would fill).
//  3. A boundary of exactly 70, bare or quoted, still carries a file upload
//     byte for byte: the fixed buffer that replaced the client-sized one is
//     exactly big enough, and AddressSanitizer turns a buffer one byte short
//     into a stack-buffer-overflow here instead of a silent pass.
//  4. An ordinary form field still parses.
// Against upstream 2.0.17's Parsing.cpp, cases 1 and 2 fail: it reads the body
// and returns true.
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cstdio>
#include <string>

#include <WebServer.h>

static int g_checks = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                            \
    }                                                                      \
    ++g_checks;                                                            \
  } while (0)

namespace {

// Exposes the two protected parsers; everything else is the real WebServer.
class Probe : public WebServer {
 public:
  Probe() : WebServer(80) {}
  bool parse(WiFiClient& c) { return _parseRequest(c); }
  bool parse_form(WiFiClient& c, const String& boundary, uint32_t len) { return _parseForm(c, boundary, len); }
  bool upload_started() const { return static_cast<bool>(_currentUpload); }
};

// An upload route on /upload that records what the parser hands it.
// WebServer deletes its handlers, so each test allocates one and reads it
// before its Probe goes out of scope.
class Recorder : public RequestHandler {
 public:
  bool canHandle(HTTPMethod m, String uri) override { return m == HTTP_POST && uri == "/upload"; }
  bool canUpload(String uri) override { return uri == "/upload"; }
  void upload(WebServer&, String, HTTPUpload& u) override {
    switch (u.status) {
      case UPLOAD_FILE_START: ++starts; filename = u.filename.c_str(); break;
      case UPLOAD_FILE_WRITE: bytes.append(reinterpret_cast<const char*>(u.buf), u.currentSize); break;
      case UPLOAD_FILE_END: ++ends; break;
      case UPLOAD_FILE_ABORTED: ++aborts; break;
    }
  }
  int starts = 0, ends = 0, aborts = 0;
  std::string filename, bytes;
};

std::string file_body(const std::string& boundary, const std::string& data) {
  return "--" + boundary + "\r\n"
         "Content-Disposition: form-data; name=\"f\"; filename=\"x.bin\"\r\n"
         "Content-Type: application/octet-stream\r\n"
         "\r\n" + data + "\r\n--" + boundary + "--\r\n";
}

std::string field_body(const std::string& boundary) {
  return "--" + boundary + "\r\n"
         "Content-Disposition: form-data; name=\"a\"\r\n"
         "\r\n"
         "hello\r\n"
         "--" + boundary + "--\r\n";
}

std::string request(const std::string& uri, const std::string& boundary_param, const std::string& body) {
  return "POST " + uri + " HTTP/1.1\r\n"
         "Host: canary.local\r\n"
         "Content-Type: multipart/form-data; boundary=" + boundary_param + "\r\n"
         "Content-Length: " + std::to_string(body.size()) + "\r\n"
         "\r\n" + body;
}

// File bytes that start the boundary pattern and break off, so the
// parser's partial-match replay runs, plus a NUL and a high byte.
const std::string kData = std::string("abc\r\n-x\r\n--") + '\0' + "\xff tail";

int refuses_oversize(size_t n, const std::string& uri) {
  const std::string b(n, 'a');
  const std::string body = file_body(b, kData);
  const std::string req = request(uri, b, body);
  Probe server;
  Recorder* rec = new Recorder;
  server.addHandler(rec);
  WiFiClient c(req);
  CHECK(!server.parse(c));
  // Refused while the headers were read: not one body byte consumed.
  CHECK(c.consumed() <= req.size() - body.size());
  CHECK(!server.upload_started());
  CHECK(rec->starts == 0 && rec->bytes.empty());
  return 0;
}

int carries_upload(const std::string& boundary, const std::string& boundary_param) {
  const std::string body = file_body(boundary, kData);
  Probe server;
  Recorder* rec = new Recorder;
  server.addHandler(rec);
  WiFiClient c(request("/upload", boundary_param, body));
  CHECK(server.parse(c));
  CHECK(rec->starts == 1 && rec->ends == 1 && rec->aborts == 0);
  CHECK(rec->filename == "x.bin");
  CHECK(rec->bytes == kData);
  return 0;
}

}  // namespace

int main() {
  // 1. Over the cap: refused in _parseRequest, on a URI nobody claims and on
  //    the upload route, from one past the cap to the advisory's shape.
  for (size_t n : {71u, 72u, 200u, 8200u, 30000u}) {
    if (refuses_oversize(n, "/no-such-route") || refuses_oversize(n, "/upload")) return 1;
  }
  // ...and the cap counts the boundary after its quotes come off, as upstream does.
  {
    const std::string b(71, 'q');
    const std::string body = file_body(b, kData);
    const std::string req = request("/upload", "\"" + b + "\"", body);
    Probe server;
    WiFiClient c(req);
    CHECK(!server.parse(c));
    CHECK(c.consumed() <= req.size() - body.size());
  }

  // 2. _parseForm's own guard, with the request headers already consumed.
  {
    const std::string b(71, 'g');
    Probe server;
    WiFiClient c(file_body(b, kData));
    CHECK(!server.parse_form(c, String(b.c_str()), 0));
    CHECK(c.consumed() == 0);
    CHECK(!server.upload_started());
  }

  // 3. Exactly 70, bare and quoted: the upload arrives byte for byte.
  if (carries_upload(std::string(70, 'z'), std::string(70, 'z'))) return 1;
  if (carries_upload(std::string(70, 'y'), "\"" + std::string(70, 'y') + "\"")) return 1;
  // A short boundary, as a browser sends, still works too.
  if (carries_upload("----WebKitFormBoundary7MA4YWxkTrZu0gW", "----WebKitFormBoundary7MA4YWxkTrZu0gW")) return 1;

  // 4. An ordinary field.
  {
    Probe server;
    WiFiClient c(request("/form", "XyZ", field_body("XyZ")));
    CHECK(server.parse(c));
    CHECK(server.hasArg("a"));
    CHECK(server.arg("a") == "hello");
  }

  std::printf("test_webserver_boundary: %d checks passed\n", g_checks);
  return 0;
}
