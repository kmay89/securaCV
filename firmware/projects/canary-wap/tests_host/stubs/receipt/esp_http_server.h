/* esp_http_server for the receipt-session suite (test_receipt_session.cpp,
 * sweep F289): a request is its headers and its query string, a response is
 * a status, a type, headers and a body, all inspectable by the test. The
 * query calls follow ESP-IDF 5.x's (esp_http_server/src/httpd_parse.c):
 * httpd_req_get_url_query_len() is the query's length (0 when there is
 * none), httpd_req_get_url_query_str() copies it and answers
 * ESP_ERR_HTTPD_RESULT_TRUNC when it is cut, and httpd_query_key_value()
 * finds `key=value` between '&'s, copies the value (cut, with
 * ESP_ERR_HTTPD_RESULT_TRUNC, when it does not fit) or answers
 * ESP_ERR_NOT_FOUND. */
#ifndef STUB_RECEIPT_ESP_HTTP_SERVER_H
#define STUB_RECEIPT_ESP_HTTP_SERVER_H

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
#define ESP_ERR_HTTPD_BASE 0xb000
#define ESP_ERR_HTTPD_RESULT_TRUNC (ESP_ERR_HTTPD_BASE + 3)
#define HTTPD_RESP_USE_STRLEN -1
typedef void* httpd_handle_t;

typedef enum {
  HTTPD_500_INTERNAL_SERVER_ERROR = 0,
} httpd_err_code_t;

struct httpd_req {
  std::map<std::string, std::string> req_hdrs;
  std::string query;          // without the '?'; empty = no query
  std::string status = "200 OK";
  std::string type;
  std::map<std::string, std::string> resp_hdrs;
  std::string resp;
  int sends = 0;
};
typedef struct httpd_req httpd_req_t;

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
inline esp_err_t httpd_resp_send_err(httpd_req_t* r, httpd_err_code_t, const char* msg) {
  r->status = "500 Internal Server Error";
  return httpd_resp_send(r, msg, -1);
}
inline size_t httpd_req_get_hdr_value_len(httpd_req_t* r, const char* f) {
  auto it = r->req_hdrs.find(f);
  return it == r->req_hdrs.end() ? 0 : it->second.size();
}
inline esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* r, const char* f, char* buf, size_t len) {
  auto it = r->req_hdrs.find(f);
  if (it == r->req_hdrs.end() || len == 0) return ESP_ERR_NOT_FOUND;
  strncpy(buf, it->second.c_str(), len - 1);
  buf[len - 1] = '\0';
  return it->second.size() >= len ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
}
inline size_t httpd_req_get_url_query_len(httpd_req_t* r) { return r->query.size(); }
inline esp_err_t httpd_req_get_url_query_str(httpd_req_t* r, char* buf, size_t len) {
  if (r->query.empty()) return ESP_ERR_NOT_FOUND;
  if (len == 0) return ESP_ERR_HTTPD_RESULT_TRUNC;
  strncpy(buf, r->query.c_str(), len - 1);
  buf[len - 1] = '\0';
  return r->query.size() >= len ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
}
inline esp_err_t httpd_query_key_value(const char* qry, const char* key, char* val, size_t val_size) {
  if (!qry || !key || !val || val_size == 0) return ESP_ERR_NOT_FOUND;
  const size_t klen = strlen(key);
  const char* p = qry;
  while (*p) {
    const char* amp = strchr(p, '&');
    const size_t plen = amp ? (size_t)(amp - p) : strlen(p);
    if (plen > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
      const char* v = p + klen + 1;
      const size_t vlen = plen - klen - 1;
      const size_t n = vlen < val_size - 1 ? vlen : val_size - 1;
      memcpy(val, v, n);
      val[n] = '\0';
      return vlen > val_size - 1 ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
    }
    if (!amp) break;
    p = amp + 1;
  }
  return ESP_ERR_NOT_FOUND;
}

#endif
