// http_body_reader.h — read an HTTP request body completely, or refuse it.
//
// esp_http_server's httpd_req_recv() returns whatever ONE socket read
// delivered: a client that sends its body in two TCP segments hands the
// handler the first fragment only, and a body longer than the handler's
// buffer is silently cut at the buffer's size. A handler that parses after
// one call therefore rejects valid requests depending on packetization, or
// acts on a truncated one. For the Beacon routes (an alert, an all-clear)
// neither is acceptable.
//
// read_all() loops until exactly `content_len` bytes are in, refuses a body
// that cannot fit (with its terminator) before reading any of it, tolerates
// a bounded number of socket timeouts, and never asks for more than the
// bytes still owed — so it cannot read into a following request.
//
// Pure and Arduino-free: the socket read is injected (`recv(buf, n)` returns
// the bytes read, 0 when the peer closed, or a negative error code), so the
// loop is host-tested (tests_host/test_http_body_reader.cpp).
//
// read_request() is the call a handler makes (sweep F300): read_all() over
// esp_http_server's own request, for exactly its Content-Length, with the
// timeout code and the three retries every handler uses. It is a template
// over the request so this header still needs no IDF include: it asks the
// request type for `content_len` and finds `httpd_req_recv(req, buf, n)`
// where the request type is declared (IDF's header, or a host suite's
// stand-in). Every handler in canary_wap.ino and the sketch's *_api.h files
// reads its body here or through read_all() itself; test_http_body_reader
// pins that no other httpd_req_recv() call is left in them.

#pragma once

#include <stddef.h>

namespace http_body {

// The code httpd_req_recv() returns for a socket read that timed out:
// esp_http_server.h's HTTPD_SOCK_ERR_TIMEOUT. Spelled here so this header
// needs no IDF include; a translation unit that includes the IDF header
// first (canary_wap.ino, beacon_api.h, csi_mqtt.cpp, a host stand-in) holds
// the two equal at the end of this file.
constexpr int REQUEST_TIMEOUT_CODE = -3;
// How many timed-out reads read_request() sits out before it refuses: a body
// that stops arriving is refused at the fourth wait. Each wait is the
// server's recv_wait_timeout (30 s in both of canary_wap.ino's configs), so
// a stalled sender holds the httpd task up to about two minutes, the budget
// /api/ota/config, /api/mqtt/config and the Beacon routes already had.
constexpr int REQUEST_MAX_TIMEOUTS = 3;

enum Result {
  OK = 0,
  TOO_LARGE,    // content_len does not fit the buffer with its terminator
  READ_FAILED,  // the peer closed early, a read errored, or timeouts ran out
};

template <typename Recv>
inline Result read_all(Recv recv, size_t content_len, char* buf, size_t cap,
                       int timeout_code, int max_timeouts, size_t* out_len) {
  *out_len = 0;
  if (cap == 0 || content_len >= cap) return TOO_LARGE;
  size_t got = 0;
  int timeouts = 0;
  while (got < content_len) {
    const int n = recv(buf + got, content_len - got);
    if (n == timeout_code) {
      if (++timeouts > max_timeouts) return READ_FAILED;
      continue;
    }
    if (n <= 0) return READ_FAILED;
    if (static_cast<size_t>(n) > content_len - got) return READ_FAILED;  // a recv never hands back more than asked
    got += static_cast<size_t>(n);
  }
  buf[got] = '\0';
  *out_len = got;
  return OK;
}

// A handler's body, read whole (NUL-terminated, *out_len bytes) or refused:
// read_all() over httpd_req_recv(), for exactly req->content_len bytes. A
// refusal leaves *out_len 0; on TOO_LARGE nothing was read and `buf` is
// untouched, so a caller never parses it unless this returned OK. An empty
// body is OK with *out_len 0: each route keeps its own answer for that.
template <typename Req>
inline Result read_request(Req* req, char* buf, size_t cap, size_t* out_len) {
  return read_all([req](char* p, size_t n) { return httpd_req_recv(req, p, n); },
                  req->content_len, buf, cap, REQUEST_TIMEOUT_CODE, REQUEST_MAX_TIMEOUTS, out_len);
}

}  // namespace http_body

#ifdef HTTPD_SOCK_ERR_TIMEOUT
static_assert(http_body::REQUEST_TIMEOUT_CODE == HTTPD_SOCK_ERR_TIMEOUT,
              "http_body::REQUEST_TIMEOUT_CODE is esp_http_server.h's HTTPD_SOCK_ERR_TIMEOUT");
#endif
