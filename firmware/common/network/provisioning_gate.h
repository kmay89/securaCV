// provisioning_gate.h — the physical-presence gate behind the provisioning
// receipt, and the rule that decides whether a served page may carry the
// bearer credential.
//
// Pure: no Arduino, no WiFi, no httpd. The canary tree's main.cpp owns one
// State, opens it from the BOOT-button handler, and the network lib takes or
// peeks it from the receipt handler and the page handlers. The canary-wap
// sketch keeps its own inline copy of the same two operations
// (canary_wap.ino, g_provisioning_gate_opened_at); this header pins the
// contract both must honor, host-tested in tests_host/test_provisioning_gate.cpp:
//
//   * one tap admits exactly ONE consumer. take() is a single atomic exchange,
//     so two tasks racing for the same tap (the httpd receipt handler and, on
//     the WAP, the BLE OTA break-glass hook) cannot both be admitted;
//   * the gate self-closes after ttl_ms even if nobody fetches the receipt;
//   * a millis() of exactly 0 is stored as 1, because 0 means "closed";
//   * uint32 wraparound is handled by unsigned subtraction (now - opened).
//
// page_token_policy() is the second half: the dashboard (/) and the setup
// wizard (/setup) are served with the bearer token injected into the HTML so
// the page can call the API. That injection was unconditional on every
// interface, which meant any device on the home LAN could read the token
// out of view-source. The policy withholds it unless the request is one of:
// the first-boot setup wizard, a bearer-authenticated caller, a request that
// provably arrived over the Canary's own Wi-Fi (request_on_softap below: the
// device-unique AP password + max 1 client is that boundary), or the gate is
// open (peeked, never taken — loading the page must not spend the tap the
// receipt fetch needs).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace canary {
namespace net {
namespace provisioning_gate {

// millis() at which the BOOT tap landed; 0 means closed. Touched from the
// main-loop task (open) and the httpd task (take / is_open), so every access
// goes through the __atomic builtins — never a plain read or write.
struct State {
  uint32_t opened_at_ms;
};

// 0 is the "closed" sentinel, so a tap that lands at millis()==0 is stored
// as 1 (one millisecond of TTL lost, never a tap lost).
inline uint32_t stamp(uint32_t now_ms) { return now_ms == 0 ? 1u : now_ms; }

inline void open(State& s, uint32_t now_ms) {
  __atomic_store_n(&s.opened_at_ms, stamp(now_ms), __ATOMIC_RELEASE);
}

// Peek: true while the gate is open. Does NOT consume the tap.
inline bool is_open(const State& s, uint32_t now_ms, uint32_t ttl_ms) {
  const uint32_t opened = __atomic_load_n(&s.opened_at_ms, __ATOMIC_ACQUIRE);
  if (opened == 0) return false;
  return (uint32_t)(now_ms - opened) < ttl_ms;
}

// Consume: hands the open timestamp to exactly one caller. A second caller
// (any task) reads 0 and is refused. An expired stamp is consumed too, so a
// stale tap can never be revived by a later clock.
inline bool take(State& s, uint32_t now_ms, uint32_t ttl_ms) {
  const uint32_t opened = __atomic_exchange_n(&s.opened_at_ms, 0u, __ATOMIC_ACQ_REL);
  if (opened == 0) return false;
  return (uint32_t)(now_ms - opened) < ttl_ms;
}

// Explicit close (factory reset, receipt served through another path).
inline void close(State& s) {
  __atomic_store_n(&s.opened_at_ms, 0u, __ATOMIC_RELEASE);
}

// ── Page-token policy ───────────────────────────────────────────────────────

enum class PageToken : uint8_t {
  INJECT,    // stream the bearer credential into the page's placeholder
  WITHHOLD,  // stream the page with an empty credential (the SPA shows how to unlock)
};

// The order is the order of the reasons a reader sees on the wire: the most
// specific, physically-attested grant first. `reason` (optional) names the
// branch for the serial log; it is a string literal, never freed.
inline PageToken page_token_policy(bool setup_active, bool bearer_ok,
                                   bool from_ap_subnet, bool gate_open,
                                   const char** reason = nullptr) {
  const char* why = "lan: withheld (tap BOOT, use the Canary's own Wi-Fi, or paste the kit token)";
  PageToken out = PageToken::WITHHOLD;
  if (setup_active) {
    why = "setup wizard active";
    out = PageToken::INJECT;
  } else if (bearer_ok) {
    why = "bearer-authenticated request";
    out = PageToken::INJECT;
  } else if (from_ap_subnet) {
    why = "peer inside the SoftAP subnet";
    out = PageToken::INJECT;
  } else if (gate_open) {
    why = "provisioning gate open (BOOT tap)";
    out = PageToken::INJECT;
  }
  if (reason) *reason = why;
  return out;
}

// A netmask is a contiguous run of leading ones: ~mask + 1 is a power of
// two (or ~mask == 0 for /32). Equivalent: (~mask & (~mask + 1)) == 0.
inline bool mask_is_prefix(uint32_t mask) {
  const uint32_t inv = ~mask;
  return (inv & (inv + 1u)) == 0;
}

// True only when `peer` is an IPv4 address inside the SoftAP's IPv4 subnet.
// Deliberately conservative: a zero mask, a zero AP address, a zero peer, or
// a mask that is not a contiguous prefix all answer false — the caller
// treats "not provably AP" as LAN, because a wrong match re-opens the
// disclosure this policy exists to close. All three values are host-order
// (a.b.c.d packed as a<<24 | b<<16 | c<<8 | d); IPv6 and link-local peers
// never reach this function (the caller answers false for any non-AF_INET
// socket family).
inline bool ipv4_in_subnet(uint32_t peer, uint32_t ap_ip, uint32_t mask) {
  if (mask == 0 || ap_ip == 0 || peer == 0) return false;
  if (!mask_is_prefix(mask)) return false;
  return (peer & mask) == (ap_ip & mask);
}

// Two prefixes overlap iff they agree on the shorter of the two masks.
inline bool subnets_overlap(uint32_t a_ip, uint32_t a_mask,
                            uint32_t b_ip, uint32_t b_mask) {
  const uint32_t m = a_mask & b_mask;
  return (a_ip & m) == (b_ip & m);
}

// The whole "did this request come over the Canary's own Wi-Fi?" test, on
// numbers the caller reads off the socket (getpeername / getsockname) and
// the two netifs. Every input is host-order IPv4; 0 means "unknown/down".
// True only when ALL hold:
//   * the socket's local address IS the SoftAP address (the request arrived
//     on the AP interface, not the STA one);
//   * the peer is inside the AP subnet (ipv4_in_subnet, conservative);
//   * the STA is down, OR its subnet provably does not overlap the AP's. A
//     home router that happens to use the AP's range (192.168.4.0/24) makes
//     "which interface?" ambiguous, and ambiguous is the home LAN. A STA
//     address with an unusable mask cannot prove no overlap, so it refuses.
inline bool request_on_softap(uint32_t peer, uint32_t local,
                              uint32_t ap_ip, uint32_t ap_mask,
                              uint32_t sta_ip, uint32_t sta_mask) {
  if (ap_ip == 0 || local != ap_ip) return false;
  if (!ipv4_in_subnet(peer, ap_ip, ap_mask)) return false;
  if (sta_ip != 0) {
    if (sta_mask == 0 || !mask_is_prefix(sta_mask)) return false;
    if (subnets_overlap(ap_ip, ap_mask, sta_ip, sta_mask)) return false;
  }
  return true;
}

// The 403 body the receipt route answers when neither a bearer nor an open
// gate admits the caller. Built from the TTL constant so the advertised
// window and the gate's real window cannot drift apart. Returns false when
// `cap` cannot hold the whole body — the handler then answers 500 rather
// than shipping truncated JSON (a future field must never silently break
// the contract the iOS app parses).
inline bool build_gate_refusal_json(char* out, size_t cap, uint32_t ttl_ms) {
  if (!out || cap == 0) return false;
  const unsigned long ttl_s = (unsigned long)(ttl_ms / 1000u);
  const int n = snprintf(out, cap,
      "{\"error\":\"physical_confirmation_required\","
      "\"hint\":\"Press the BOOT button on the device to reveal the provisioning receipt.\","
      "\"button\":\"BOOT (short tap, then poll within %lu seconds)\","
      "\"gate_ttl_seconds\":%lu}",
      ttl_s, ttl_s);
  if (n < 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return false;
  }
  return true;
}

}  // namespace provisioning_gate
}  // namespace net
}  // namespace canary
