/* Minimal esp_http_server stub for compiling the flagship's REAL
 * securacv_auth.cpp under test_securacv_auth.cpp.
 *
 * A request carries a header map the test fills; a response records what
 * the library set and sent. Set-status, set-type and set-hdr keep the
 * caller's pointer and the send reads it, as ESP-IDF does (it copies nothing
 * until the send), so a value that died before the send would show. Header
 * names match case-insensitively, as in ESP-IDF; a value longer than the
 * caller's buffer is copied cut short and answered
 * ESP_ERR_HTTPD_RESULT_TRUNC, as in ESP-IDF. Each read of a header's value
 * is counted (value_reads), so a test can tell a request whose bearer was
 * compared from one whose bearer was not (the lockout's HELD). The library
 * is handed its source explicitly and reads no peer; `peer` is the address
 * the test's request_peer_ipv4() stand-in hands the handlers cut from
 * securacv_network.cpp (the real one reads the socket with getpeername). */
#ifndef STUB_SECURACV_AUTH_ESP_HTTP_SERVER_H
#define STUB_SECURACV_AUTH_ESP_HTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include <map>
#include <string>

typedef int esp_err_t;
#define ESP_OK                     0
#define ESP_FAIL                   -1
#define ESP_ERR_NOT_FOUND          0x105
#define ESP_ERR_HTTPD_RESULT_TRUNC 0xb003

struct HostCaseLess {
  bool operator()(const std::string& a, const std::string& b) const {
    return strcasecmp(a.c_str(), b.c_str()) < 0;
  }
};

typedef struct httpd_req {
  // The request, as the test builds it.
  std::map<std::string, std::string, HostCaseLess> headers;
  int value_reads = 0;  // httpd_req_get_hdr_value_str() calls
  uint32_t peer = 0;    // the socket peer's IPv4, host order, for request_peer_ipv4()

  // The response, as the library sets it. The pointers are the caller's;
  // the send copies them into the strings below.
  const char* status_ptr = nullptr;
  const char* type_ptr = nullptr;
  std::map<std::string, const char*, HostCaseLess> resp_hdr_ptrs;

  int sends = 0;
  std::string status;
  std::string type;
  std::string body;
  std::map<std::string, std::string, HostCaseLess> resp_headers;
} httpd_req_t;

inline size_t httpd_req_get_hdr_value_len(httpd_req_t* r, const char* field) {
  auto it = r->headers.find(field);
  return it == r->headers.end() ? 0 : it->second.size();
}

inline esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* r, const char* field,
                                             char* val, size_t val_size) {
  r->value_reads++;
  auto it = r->headers.find(field);
  if (it == r->headers.end()) return ESP_ERR_NOT_FOUND;
  if (!val || val_size == 0) return ESP_FAIL;
  const std::string& v = it->second;
  const size_t n = v.size() < val_size - 1 ? v.size() : val_size - 1;
  memcpy(val, v.data(), n);
  val[n] = '\0';
  return v.size() > n ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
}

inline esp_err_t httpd_resp_set_status(httpd_req_t* r, const char* status) {
  r->status_ptr = status;
  return ESP_OK;
}

inline esp_err_t httpd_resp_set_type(httpd_req_t* r, const char* type) {
  r->type_ptr = type;
  return ESP_OK;
}

inline esp_err_t httpd_resp_set_hdr(httpd_req_t* r, const char* field, const char* value) {
  r->resp_hdr_ptrs[field] = value;
  return ESP_OK;
}

typedef enum { HTTPD_500_INTERNAL_SERVER_ERROR = 500 } httpd_err_code_t;

inline esp_err_t httpd_resp_sendstr(httpd_req_t* r, const char* str);

// ESP-IDF's canned error answer: the status and the message, sent at once.
inline esp_err_t httpd_resp_send_err(httpd_req_t* r, httpd_err_code_t, const char* msg) {
  r->status_ptr = "500 Internal Server Error";
  return httpd_resp_sendstr(r, msg);
}

inline esp_err_t httpd_resp_sendstr(httpd_req_t* r, const char* str) {
  r->sends++;
  r->status = r->status_ptr ? r->status_ptr : "200 OK";
  r->type = r->type_ptr ? r->type_ptr : "text/html";
  r->resp_headers.clear();
  for (const auto& kv : r->resp_hdr_ptrs) r->resp_headers[kv.first] = kv.second ? kv.second : "";
  r->body = str ? str : "";
  return ESP_OK;
}

#endif
