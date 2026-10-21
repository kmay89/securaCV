/* The suites' one stand-in for the socket read canary_wap.ino's
 * request_peer_key() makes (getpeername; sweep F343), for the harnesses that
 * compile the real api_auth.h over stubs/receipt_grant. The request's stub
 * peer (esp_http_server.h's `peer`, `peer_ok`) is handed to
 * provisioning_gate.h's peer_key_from_addr() as the dual-stack listener
 * reports it, an AF_INET6 address, exactly as the sketch's reader does; so a
 * peer that cannot say, or the unspecified address, is no key. The real
 * auth_request_source() is cut out of the sketch and maps what this answers;
 * test_receipt_authority.cpp pins the real reader's text. */
#ifndef STUB_RECEIPT_GRANT_PEER_SOURCE_H
#define STUB_RECEIPT_GRANT_PEER_SOURCE_H

#include <stdint.h>
#include <string.h>

#include "esp_http_server.h"
#include "provisioning_gate.h"   // the sketch's staged copy

static bool request_peer_key(httpd_req_t* r, uint8_t out[canary::net::provisioning_gate::PEER_KEY_LEN]) {
  if (!r->peer_ok) {
    memset(out, 0, canary::net::provisioning_gate::PEER_KEY_LEN);
    return false;
  }
  return canary::net::provisioning_gate::peer_key_from_addr(
      canary::net::provisioning_gate::AddrFamily::IPV6, r->peer, out);
}

#endif
