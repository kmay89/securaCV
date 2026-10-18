/* esp_http_server for the MQTT bridge host build: a request is a body the
 * handler reads and a response it writes, both inspectable by the test. */
#ifndef STUB_MQTT_ESP_HTTP_SERVER_H
#define STUB_MQTT_ESP_HTTP_SERVER_H

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
#define ESP_ERR_NOT_FOUND 0x105
#define HTTPD_RESP_USE_STRLEN -1
/* esp_http_server.h's socket error codes, as httpd_req_recv() returns them. */
#define HTTPD_SOCK_ERR_FAIL    -1
#define HTTPD_SOCK_ERR_INVALID -2
#define HTTPD_SOCK_ERR_TIMEOUT -3
typedef void* httpd_handle_t;

struct httpd_req {
  /* The request line's Content-Length, as IDF fills it (0 when the client
   * sent none). The test sets it beside `body`; it may claim more than the
   * body holds (the peer then closes early) or less. */
  size_t content_len = 0;
  std::string body;
  size_t body_off = 0;
  /* How the socket hands the body over (sweep F266): each entry is the next
   * httpd_req_recv() call's answer. A positive entry caps what that call
   * returns, as one TCP segment would (a short read); a negative one is
   * returned as it is (HTTPD_SOCK_ERR_TIMEOUT, a socket error) and reads
   * nothing. Once the plan is spent each call returns all it may. */
  std::vector<int> recv_plan;
  size_t recv_calls = 0;
  size_t recv_asked_max = 0;   /* the largest `len` any call asked for */
  std::map<std::string, std::string> req_hdrs;
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
  if (len > r->recv_asked_max) r->recv_asked_max = len;
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
inline esp_err_t httpd_resp_set_status(httpd_req_t* r, const char* s) { r->status = s; return ESP_OK; }
inline esp_err_t httpd_resp_set_type(httpd_req_t* r, const char* t) { r->type = t; return ESP_OK; }
inline esp_err_t httpd_resp_set_hdr(httpd_req_t* r, const char* f, const char* v) {
  r->resp_hdrs[f] = v;
  return ESP_OK;
}
inline esp_err_t httpd_resp_send(httpd_req_t* r, const char* buf, ssize_t len) {
  r->resp.assign(buf ? buf : "", buf ? (len < 0 ? strlen(buf) : (size_t)len) : 0);
  ++r->sends;
  return ESP_OK;
}
inline esp_err_t httpd_resp_sendstr(httpd_req_t* r, const char* s) { return httpd_resp_send(r, s, -1); }
inline size_t httpd_req_get_hdr_value_len(httpd_req_t* r, const char* f) {
  auto it = r->req_hdrs.find(f);
  return it == r->req_hdrs.end() ? 0 : it->second.size();
}
inline esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* r, const char* f, char* buf, size_t len) {
  auto it = r->req_hdrs.find(f);
  if (it == r->req_hdrs.end() || len == 0) return ESP_ERR_NOT_FOUND;
  strncpy(buf, it->second.c_str(), len - 1);
  buf[len - 1] = '\0';
  return ESP_OK;
}
inline esp_err_t httpd_req_get_url_query_str(httpd_req_t*, char*, size_t) { return ESP_ERR_NOT_FOUND; }
inline esp_err_t httpd_query_key_value(const char*, const char*, char*, size_t) { return ESP_ERR_NOT_FOUND; }

#endif
