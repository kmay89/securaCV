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

typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#define ESP_FAIL -1
#endif
typedef void* httpd_handle_t;

struct httpd_req {
  std::string body;
  size_t body_off = 0;
  std::string status = "200 OK";
  std::string type;
  std::map<std::string, std::string> resp_hdrs;
  std::string resp;
  int sends = 0;
};
typedef struct httpd_req httpd_req_t;

inline int httpd_req_recv(httpd_req_t* r, char* buf, size_t len) {
  const size_t left = r->body.size() - r->body_off;
  const size_t n = left < len ? left : len;
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
inline esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t*) { return ESP_OK; }

#endif
