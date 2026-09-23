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

#pragma once

#include <stddef.h>

namespace http_body {

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

}  // namespace http_body
