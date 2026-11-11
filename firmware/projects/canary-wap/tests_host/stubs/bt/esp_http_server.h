/* Host stand-in for esp_http_server.h (the canary-wap Bluetooth channel host
 * harness, test_bluetooth_commands_wap.cpp, sweep F235): a request is a body
 * the handler reads and a response it writes, both inspectable by the test,
 * as the MQTT bridge's stand-in (stubs/mqtt) keeps them, plus the route
 * registration bluetooth_api.h's register_routes() makes. */
#ifndef STUB_BT_ESP_HTTP_SERVER_H
#define STUB_BT_ESP_HTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include <map>
#include <string>
#include <vector>

typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#define ESP_FAIL -1
#endif
/* esp_http_server.h's socket error codes, as httpd_req_recv() returns them. */
#define HTTPD_SOCK_ERR_FAIL    -1
#define HTTPD_SOCK_ERR_INVALID -2
#define HTTPD_SOCK_ERR_TIMEOUT -3
typedef void* httpd_handle_t;

struct httpd_req {
  /* The request line's Content-Length, as IDF fills it (0 when the client
   * sent none): the handlers read exactly this much (sweep F300,
   * http_body::read_request()). The test sets it beside `body`. */
  size_t content_len = 0;
  std::string body;
  size_t body_off = 0;
  /* How the socket hands the body over, as stubs/mqtt's request does: each
   * entry is the next httpd_req_recv() call's answer. A positive entry caps
   * what that call returns, as one TCP segment would (a short read); a
   * negative one is returned as it is (HTTPD_SOCK_ERR_TIMEOUT, a socket
   * error) and reads nothing. Once the plan is spent each call returns all
   * it may. */
  std::vector<int> recv_plan;
  size_t recv_calls = 0;
  std::string status = "200 OK";
  std::string type;
  std::map<std::string, std::string> resp_hdrs;
  std::string resp;
  int sends = 0;
};
typedef struct httpd_req httpd_req_t;

/* As IDF's httpd_req_recv(): at most `len` bytes of the body, never past
 * content_len, whatever one socket read delivered; 0 once the peer has
 * nothing more. */
inline int httpd_req_recv(httpd_req_t* r, char* buf, size_t len) {
  const size_t call = r->recv_calls++;
  size_t cap = len;
  if (call < r->recv_plan.size()) {
    const int step = r->recv_plan[call];
    if (step <= 0) return step;
    if ((size_t)step < cap) cap = (size_t)step;
  }
  const size_t owed = r->content_len > r->body_off ? r->content_len - r->body_off : 0;
  if (owed < cap) cap = owed;
  const size_t left = r->body.size() - r->body_off;
  const size_t n = left < cap ? left : cap;
  if (n == 0) return 0;
  memcpy(buf, r->body.data() + r->body_off, n);
  r->body_off += n;
  return (int)n;
}
inline esp_err_t httpd_resp_set_status(httpd_req_t* r, const char* s) {
  r->status = s;
  return ESP_OK;
}
inline esp_err_t httpd_resp_set_type(httpd_req_t* r, const char* t) {
  r->type = t;
  return ESP_OK;
}
inline esp_err_t httpd_resp_set_hdr(httpd_req_t* r, const char* f, const char* v) {
  r->resp_hdrs[f] = v;
  return ESP_OK;
}
// What a C string send reads: up to its first zero byte.
inline esp_err_t httpd_resp_sendstr(httpd_req_t* r, const char* s) {
  r->resp = s ? s : "";
  ++r->sends;
  return ESP_OK;
}

// ESP-IDF's http_method values (http_parser.h): DELETE 0, GET 1, POST 3.
typedef enum { HTTP_DELETE = 0, HTTP_GET = 1, HTTP_POST = 3 } httpd_method_t;
struct httpd_uri_t {
  const char* uri;
  httpd_method_t method;
  esp_err_t (*handler)(httpd_req_t* r);
  void* user_ctx;
};
// The routes register_routes() made, so a test drives the handler a route
// actually has (sweep F346: the bond confirm's gate is the registration's).
inline std::vector<httpd_uri_t>& registered_routes() {
  static std::vector<httpd_uri_t> routes;
  return routes;
}
inline esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t* u) {
  registered_routes().push_back(*u);
  return ESP_OK;
}

#endif
