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
// device-unique AP password + max 1 client is that boundary), or an unspent
// BOOT tap — which the page load then SPENDS (page_token_decide). The tap is
// one consumer across both paths: one page load or one receipt fetch,
// whichever asks first. A page that got the token already holds the bearer,
// so its "Save recovery kit" fetch needs no second tap.
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

// Peek: true while the gate is open. Does NOT consume the tap, so it is for
// status and tests only — never a grant (a grant that peeks lets one tap
// admit many consumers; page_token_decide takes instead).
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
// `gate_open` means "a tap was spent on THIS request" — the firmware never
// calls this with a peeked gate; it goes through page_token_decide below.
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

// The page handlers' whole decision. `take_gate` (a callable returning bool:
// the firmware passes its take() hook) is called only when none of the three
// standing grants applies, and then exactly once — so a first-boot, bearer
// or SoftAP load never spends a tap, and a home-LAN load that is unlocked by
// a tap spends it. An earlier version PEEKED the gate here: one tap then
// unlocked every page load for the whole TTL, and each of those pages could
// fetch the receipt (AP password included) with the bearer it had been
// handed — one tap, any number of consumers, which is the opposite of the
// contract at the top of this file.
template <typename TakeGate>
inline PageToken page_token_decide(bool setup_active, bool bearer_ok,
                                   bool from_ap_subnet, TakeGate take_gate,
                                   const char** reason = nullptr) {
  if (page_token_policy(setup_active, bearer_ok, from_ap_subnet, false, reason) ==
      PageToken::INJECT) {
    return PageToken::INJECT;
  }
  const bool took = take_gate();
  return page_token_policy(false, false, false, took, reason);
}

// A netmask is a contiguous run of leading ones: ~mask + 1 is a power of
// two (or ~mask == 0 for /32). Equivalent: (~mask & (~mask + 1)) == 0.
inline bool mask_is_prefix(uint32_t mask) {
  const uint32_t inv = ~mask;
  return (inv & (inv + 1u)) == 0;
}

// ── Socket address → IPv4 ───────────────────────────────────────────────────

enum class AddrFamily : uint8_t { IPV4, IPV6, OTHER };

// Host-order IPv4 (a<<24 | b<<16 | c<<8 | d) carried by one socket address,
// or 0 when it carries none. `bytes` is the address exactly as it sits in the
// sockaddr, network order: 4 bytes for IPV4 (sin_addr), 16 for IPV6
// (sin6_addr).
//
// Why IPV6 matters for an IPv4-only feature: esp_http_server listens on ONE
// dual-stack AF_INET6 socket whenever the core sets CONFIG_LWIP_IPV6 (both
// Arduino-ESP32 cores this tree builds on do), and lwIP's getpeername /
// getsockname report an IPv4 client on it as the IPv4-mapped address
// ::ffff:a.b.c.d with family AF_INET6. Reading only AF_INET sees 0 for
// every real request — the AP-subnet unlock never fires and the receipt's
// base_url falls back to the SoftAP address even for a home-LAN caller.
//   IPV4                          → a.b.c.d
//   IPV6 ::ffff:a.b.c.d           → a.b.c.d  (the ::ffff:0:0/96 prefix, RFC 4291 §2.5.5.2)
//   every other IPV6 — global, link-local fe80::/10, loopback ::1, the
//   deprecated IPv4-compatible ::a.b.c.d          → 0
//   OTHER, or a null pointer      → 0
// 0 is what every consumer below already treats as "unknown", so a real
// IPv6 peer stays "not provably AP", i.e. the home LAN.
inline uint32_t ipv4_host_order_from_addr(AddrFamily family, const uint8_t* bytes) {
  if (!bytes) return 0;
  const uint8_t* v4 = nullptr;
  if (family == AddrFamily::IPV4) {
    v4 = bytes;
  } else if (family == AddrFamily::IPV6) {
    for (int i = 0; i < 10; ++i) {
      if (bytes[i] != 0) return 0;
    }
    if (bytes[10] != 0xFF || bytes[11] != 0xFF) return 0;
    v4 = bytes + 12;
  } else {
    return 0;
  }
  return ((uint32_t)v4[0] << 24) | ((uint32_t)v4[1] << 16) |
         ((uint32_t)v4[2] << 8)  |  (uint32_t)v4[3];
}

// True only when `peer` is an IPv4 address inside the SoftAP's IPv4 subnet.
// Deliberately conservative: a zero mask, a zero AP address, a zero peer, or
// a mask that is not a contiguous prefix all answer false — the caller
// treats "not provably AP" as LAN, because a wrong match re-opens the
// disclosure this policy exists to close. All three values are host-order
// (a.b.c.d packed as a<<24 | b<<16 | c<<8 | d); the caller converts the
// socket's addresses through ipv4_host_order_from_addr, so an IPv6 or
// link-local peer arrives here as 0 and answers false.
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
