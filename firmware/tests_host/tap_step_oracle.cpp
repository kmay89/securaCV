// tap_step_oracle.cpp — the flagship's BOOT-tap step, run for a node test.
//
// The step every credential-less ask on the flagship goes through (sweep
// F354): provisioning_gate_take(), cut verbatim out of
// canary/lib/securacv_network/src/securacv_network.cpp by cut_functions.awk
// (TAP_STEP_INC) and compiled over the real provisioning_gate.h, the real
// canary_config.h TTL and stand-ins for the socket's peer, millis(), the
// serial console and the health log. The gate's State and the TapArbiter are
// the real ones, as main.cpp and the network lib own them.
//
// test_canary_provisioning_banner.test.js drives the dashboard's unlock
// banner against it, so the page's poll and a host's curl loop meet the
// firmware's own one-asker rule, not a JavaScript copy of it.
//
// Input, one command per line; output, one line per command:
//   tap <ms>            the owner's short press at <ms>       → open
//   ask <peer> <ms>     a credential-less ask from <peer>     → TAKE | CONTENDED | NO_TAP
//                       (<peer>: a.b.c.d, or 32 hex digits for an IPv6
//                       address, or "-" for a socket that cannot say)
//   body403             the receipt's 403 at the build's TTL  → its JSON
// The replay is deterministic: the same commands give the same answers.
//
// Built by the Makefile (TAP_STEP_ORACLE) or, run outside it, by the test.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include "canary_config.h"              // the real PROVISIONING_GATE_TTL_MS
#include "log_level.h"                  // the real LogLevel / LogCategory
#include "network/provisioning_gate.h"  // the real State, TapArbiter, tap_arbitrate()

#ifndef TAP_STEP_INC
#error "build with -DTAP_STEP_INC (provisioning_gate_take() cut from securacv_network.cpp)"
#endif

namespace pg = canary::net::provisioning_gate;

static uint32_t g_now = 0;
static uint32_t millis() { return g_now; }
struct OracleSerial {
  template <class... Args>
  void printf(const char*, Args...) {}
};
static OracleSerial Serial;
static void log_health(LogLevel, LogCategory, const char*, const char*) {}

struct httpd_req_t {
  bool known = false;
  uint8_t key[pg::PEER_KEY_LEN] = {};
};

typedef uint32_t (*network_gate_peek_fn_t)(void);
typedef bool (*network_gate_take_exact_fn_t)(uint32_t seen);
static pg::State g_gate = {0};
static uint32_t gate_peek_hook() { return pg::peek(g_gate); }
static bool gate_take_exact_hook(uint32_t seen) { return pg::take_exact(g_gate, seen); }
static network_gate_peek_fn_t s_gate_peek = gate_peek_hook;
static network_gate_take_exact_fn_t s_gate_take_exact = gate_take_exact_hook;
static pg::TapArbiter s_tap_arbiter = {};

static bool request_peer_key(httpd_req_t* req, uint8_t out[pg::PEER_KEY_LEN]) {
  if (!req->known) return false;
  std::memcpy(out, req->key, pg::PEER_KEY_LEN);
  return true;
}

#include TAP_STEP_INC

// <peer> as the real request_peer_key() would key it off the socket.
static bool parse_peer(const std::string& s, httpd_req_t* r) {
  if (s == "-") return true;
  unsigned a, b, c, d;
  char tail;
  if (std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 && b < 256 &&
      c < 256 && d < 256) {
    const uint8_t v4[4] = {(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d};
    r->known = pg::peer_key_from_addr(pg::AddrFamily::IPV4, v4, r->key);
    return true;
  }
  if (s.size() == 32) {
    uint8_t v6[16];
    for (size_t i = 0; i < 16; ++i) {
      unsigned byte;
      if (std::sscanf(s.c_str() + 2 * i, "%2x", &byte) != 1) return false;
      v6[i] = (uint8_t)byte;
    }
    r->known = pg::peer_key_from_addr(pg::AddrFamily::IPV6, v6, r->key);
    return true;
  }
  return false;
}

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "tap") {
      in >> g_now;
      pg::open(g_gate, g_now);
      std::puts("open");
    } else if (cmd == "ask") {
      std::string peer;
      in >> peer >> g_now;
      httpd_req_t r;
      if (!parse_peer(peer, &r)) {
        std::fprintf(stderr, "bad peer: %s\n", peer.c_str());
        return 2;
      }
      pg::TapVerdict v = pg::TapVerdict::NO_TAP;
      const bool took = provisioning_gate_take(&r, &v);
      std::puts(took ? "TAKE" : v == pg::TapVerdict::CONTENDED ? "CONTENDED" : "NO_TAP");
    } else if (cmd == "body403") {
      char body[pg::GATE_REFUSAL_JSON_CAP];
      if (!pg::build_gate_refusal_json(body, sizeof(body), PROVISIONING_GATE_TTL_MS)) return 3;
      std::puts(body);
    } else if (!cmd.empty()) {
      std::fprintf(stderr, "unknown command: %s\n", line.c_str());
      return 2;
    }
  }
  return 0;
}
