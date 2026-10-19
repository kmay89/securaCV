/* Host stand-in for esp_http_server.h (the receipt-grant harness,
 * test_receipt_grant.cpp, sweep F289): a request is its headers and its
 * query string, a response is what the handler set and sent, both
 * inspectable by the test. The query readers answer as IDF's do: no query is
 * ESP_ERR_NOT_FOUND, one that does not fit is ESP_ERR_HTTPD_RESULT_TRUNC with
 * the truncated text copied (httpd_query_key_value copies what fits and
 * says so), a missing key is ESP_ERR_NOT_FOUND. */
#ifndef STUB_RECEIPT_GRANT_ESP_HTTP_SERVER_H
#define STUB_RECEIPT_GRANT_ESP_HTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#define ESP_FAIL -1
#endif
#define ESP_ERR_NOT_FOUND 0x105
/* esp_http_server.h: ESP_ERR_HTTPD_BASE (0xb000) + 3. */
#define ESP_ERR_HTTPD_RESULT_TRUNC 0xb003
#define HTTPD_RESP_USE_STRLEN -1
typedef void* httpd_handle_t;

typedef enum {
  HTTPD_500_INTERNAL_SERVER_ERROR = 0,
  HTTPD_501_METHOD_NOT_IMPLEMENTED,
  HTTPD_505_VERSION_NOT_SUPPORTED,
  HTTPD_400_BAD_REQUEST,
  HTTPD_401_UNAUTHORIZED,
  HTTPD_403_FORBIDDEN,
  HTTPD_404_NOT_FOUND,
  HTTPD_405_METHOD_NOT_ALLOWED,
  HTTPD_408_REQ_TIMEOUT,
  HTTPD_411_LENGTH_REQUIRED,
  HTTPD_414_URI_TOO_LONG,
  HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE,
} httpd_err_code_t;

struct httpd_req {
  std::map<std::string, std::string> req_hdrs;   /* "Authorization", "Cookie" */
  std::string query;                             /* after the '?'; "" = none */
  std::string status = "200 OK";
  std::string type;
  std::vector<std::pair<std::string, std::string>> resp_hdrs;
  std::string resp;
  int sends = 0;
  int send_errs = 0;
};
typedef struct httpd_req httpd_req_t;

inline size_t httpd_req_get_hdr_value_len(httpd_req_t* r, const char* f) {
  auto it = r->req_hdrs.find(f);
  return it == r->req_hdrs.end() ? 0 : it->second.size();
}
inline esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* r, const char* f, char* buf, size_t len) {
  auto it = r->req_hdrs.find(f);
  if (it == r->req_hdrs.end() || len == 0) return ESP_ERR_NOT_FOUND;
  if (it->second.size() >= len) {
    memcpy(buf, it->second.data(), len - 1);
    buf[len - 1] = '\0';
    return ESP_ERR_HTTPD_RESULT_TRUNC;
  }
  memcpy(buf, it->second.c_str(), it->second.size() + 1);
  return ESP_OK;
}
inline size_t httpd_req_get_url_query_len(httpd_req_t* r) { return r->query.size(); }
inline esp_err_t httpd_req_get_url_query_str(httpd_req_t* r, char* buf, size_t len) {
  if (r->query.empty()) return ESP_ERR_NOT_FOUND;
  if (len == 0) return ESP_ERR_HTTPD_RESULT_TRUNC;
  if (r->query.size() >= len) {
    memcpy(buf, r->query.data(), len - 1);
    buf[len - 1] = '\0';
    return ESP_ERR_HTTPD_RESULT_TRUNC;
  }
  memcpy(buf, r->query.c_str(), r->query.size() + 1);
  return ESP_OK;
}
/* key=value pairs split on '&', the first match of the whole key wins, as
 * IDF's does (no percent-decoding there either). */
inline esp_err_t httpd_query_key_value(const char* qs, const char* key, char* val, size_t val_size) {
  if (!qs || !key || !val || val_size == 0) return ESP_ERR_NOT_FOUND;
  const std::string q(qs), k(key);
  size_t at = 0;
  while (at <= q.size()) {
    size_t end = q.find('&', at);
    if (end == std::string::npos) end = q.size();
    const std::string pair = q.substr(at, end - at);
    const size_t eq = pair.find('=');
    const std::string name = eq == std::string::npos ? pair : pair.substr(0, eq);
    if (name == k) {
      const std::string v = eq == std::string::npos ? "" : pair.substr(eq + 1);
      if (v.size() >= val_size) {
        memcpy(val, v.data(), val_size - 1);
        val[val_size - 1] = '\0';
        return ESP_ERR_HTTPD_RESULT_TRUNC;
      }
      memcpy(val, v.c_str(), v.size() + 1);
      return ESP_OK;
    }
    at = end + 1;
  }
  return ESP_ERR_NOT_FOUND;
}
inline esp_err_t httpd_resp_set_status(httpd_req_t* r, const char* s) { r->status = s; return ESP_OK; }
inline esp_err_t httpd_resp_set_type(httpd_req_t* r, const char* t) { r->type = t; return ESP_OK; }
inline esp_err_t httpd_resp_set_hdr(httpd_req_t* r, const char* f, const char* v) {
  r->resp_hdrs.emplace_back(f, v);
  return ESP_OK;
}
inline esp_err_t httpd_resp_send(httpd_req_t* r, const char* buf, ssize_t len) {
  r->resp.assign(buf ? buf : "", buf ? (len < 0 ? strlen(buf) : (size_t)len) : 0);
  ++r->sends;
  return ESP_OK;
}
inline esp_err_t httpd_resp_sendstr(httpd_req_t* r, const char* s) { return httpd_resp_send(r, s, -1); }
inline esp_err_t httpd_resp_send_err(httpd_req_t* r, httpd_err_code_t code, const char* msg) {
  r->status = code == HTTPD_500_INTERNAL_SERVER_ERROR ? "500 Internal Server Error" : "4xx";
  r->resp = msg ? msg : "";
  ++r->sends;
  ++r->send_errs;
  return ESP_OK;
}

#endif
