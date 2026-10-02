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

typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#define ESP_FAIL -1
#endif
#define ESP_ERR_NOT_FOUND 0x105
#define HTTPD_RESP_USE_STRLEN -1
typedef void* httpd_handle_t;

struct httpd_req {
  std::string body;
  size_t body_off = 0;
  std::map<std::string, std::string> req_hdrs;
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
