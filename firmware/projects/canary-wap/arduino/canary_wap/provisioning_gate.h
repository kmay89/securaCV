// provisioning_gate.h — the physical-presence gate behind the provisioning
// receipt, and the rule that decides whether a served page may carry the
// bearer credential.
//
// Pure: no Arduino, no WiFi, no httpd. Both firmware trees own one State.
// The canary tree's main.cpp opens it from the BOOT-button handler, and the
// network lib takes or peeks it from the receipt handler and the page
// handlers. The canary-wap sketch stages this header byte-identical
// (firmware/scripts/check_improv_sync.sh); its g_provisioning_gate is this
// State (sweep F342; it used to keep an inline copy of open and take). Each
// tree sweeps its State every loop pass with expire() (sweep F347): the
// WAP's loop() and the canary tree's handle_boot_button(). The contract both
// honor, host-tested in tests_host/test_provisioning_gate.cpp:
//
//   * one tap admits exactly ONE consumer. take() is a single atomic exchange
//     and take_exact() a single compare-and-swap of the stamp its caller
//     judged, so two tasks racing for the same tap (the httpd receipt handler
//     and, on the WAP, the BLE OTA break-glass hook) cannot both be admitted;
//   * on the WAP, the receipt's tap goes to ONE asker: tap_arbitrate() serves
//     it only to the sole address that asked for the receipt without a
//     credential from TAP_LOOKBACK_MS before the tap onward, and a second
//     address in that window closes the tap (the WAP answers 409
//     tap_contended). The flagship's receipt and page loads still take the
//     tap for the first asker (the ledger's F354);
//   * the gate self-closes after ttl_ms even if nobody fetches the receipt;
//   * a millis() of exactly 0 is stored as 1, because 0 means "closed";
//   * unsigned subtraction (now - opened) ages a stamp correctly only while
//     it is younger than 2^32 ms (49.7 days): a tap nobody took would read
//     as fresh again once millis() came back round to it. So the owner of
//     the State calls expire() every loop pass, on the task that opens it,
//     and no stamp gets older than its TTL plus one pass (sweep F347).
//
// The sketch also reads every request's peer through this header's socket
// address mapping (peer_key_from_addr(), ipv4_host_order_from_addr(),
// below), so both trees map a peer's address through one function, and the
// tap's asker and the per-address bearer lockout's bucket are one read.
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
//
// And one question comes before all of that, on both paths: can the Host the
// request targeted name this device (network/host_guard.h), or did it arrive
// over the Canary's own SoftAP, where the name is this device by
// construction? A foreign Host is decided FIRST — before a grant is read,
// before the bearer is looked at, before the tap is taken
// (page_token_decide's and receipt_decide's `foreign_host`). So a page load
// or a receipt fetch under a name that cannot mean this device neither gets a
// credential nor spends the tap the owner meant for their own page or app,
// which the order "grant first, Host after" did not guarantee.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

// The raw stamp: the millis() the tap landed at, 0 = closed. No TTL is
// applied and nothing is consumed: the caller judges the stamp
// (tap_arbitrate() below) and then takes exactly that one (take_exact()).
// Never a grant by itself.
inline uint32_t peek(const State& s) {
  return __atomic_load_n(&s.opened_at_ms, __ATOMIC_ACQUIRE);
}

// Consume exactly the stamp `seen` a caller peeked and judged: one
// compare-and-swap of `seen` to 0. False for 0, and false when the stamp
// moved since the peek (a re-tap, the BLE OTA break-glass take, another
// request's take, the expiry sweep expire()), so a stamp nobody judged is
// never consumed in its place and two takers of one stamp cannot both win.
inline bool take_exact(State& s, uint32_t seen) {
  if (seen == 0) return false;
  uint32_t expect = seen;
  return __atomic_compare_exchange_n(&s.opened_at_ms, &expect, 0u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

// The sweep's step on a stamp `seen` its caller read (peek()): clear it when
// it is `ttl_ms` old or older, with one compare-and-swap of `seen`
// (take_exact()), so a fresh re-tap stored since the read is never cleared.
// True when it cleared one. The seam a test drives with a stamp it read
// before a re-tap; expire() is this with the stamp it reads now.
inline bool expire_seen(State& s, uint32_t seen, uint32_t now_ms, uint32_t ttl_ms) {
  if (seen == 0 || (uint32_t)(now_ms - seen) < ttl_ms) return false;
  return take_exact(s, seen);
}

// The sweep (sweep F347): clear a stamp that is `ttl_ms` old or older, with
// one compare-and-swap of the stamp it read (expire_seen()), so a fresh
// re-tap that lands between the read and the swap is never cleared. True
// when it cleared one. Its owner calls it every loop pass, ON THE TASK THAT
// OPENS the State (on both trees, the loop task's BOOT handler): a
// `now_ms` read before another task's open() would see that newer stamp as
// 2^32 ms old. With the sweep, no stamp outlives its TTL by more than one
// pass, so none can age into the wrap of millis() and read as fresh again.
inline bool expire(State& s, uint32_t now_ms, uint32_t ttl_ms) {
  return expire_seen(s, peek(s), now_ms, ttl_ms);
}

// ── Page-token policy ───────────────────────────────────────────────────────

enum class PageToken : uint8_t {
  INJECT,        // stream the bearer credential into the page's placeholder
  WITHHOLD,      // stream the page with an empty credential (the SPA shows how to unlock)
  FOREIGN_HOST,  // the Host cannot name this device: an empty credential, no
                 // "withheld" hint (no unlock helps under that name), and the
                 // BOOT tap left unspent
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

// The page handlers' whole decision. `foreign_host` is the Host verdict the
// API gate uses too (the firmware's host_is_foreign: the Host cannot name
// this device and the request did not arrive over the SoftAP); when it is
// set the answer is FOREIGN_HOST and nothing else is consulted — no grant,
// and never the gate, so a load under a foreign name cannot spend a tap.
// Otherwise `take_gate` (a callable returning bool: the firmware passes its
// take() hook) is called only when none of the three standing grants
// applies, and then exactly once — so a first-boot, bearer or SoftAP load
// never spends a tap, and a home-LAN load that is unlocked by a tap spends
// it. An earlier version PEEKED the gate here: one tap then unlocked every
// page load for the whole TTL, and each of those pages could fetch the
// receipt (AP password included) with the bearer it had been handed — one
// tap, any number of consumers, which is the opposite of the contract at the
// top of this file.
template <typename TakeGate>
inline PageToken page_token_decide(bool foreign_host, bool setup_active,
                                   bool bearer_ok, bool from_ap_subnet,
                                   TakeGate take_gate,
                                   const char** reason = nullptr) {
  if (foreign_host) {
    if (reason) *reason = "foreign Host: no token and the BOOT tap left unspent (use the IP, the .local name or a private-suffix alias)";
    return PageToken::FOREIGN_HOST;
  }
  if (page_token_policy(setup_active, bearer_ok, from_ap_subnet, false, reason) ==
      PageToken::INJECT) {
    return PageToken::INJECT;
  }
  const bool took = take_gate();
  return page_token_policy(false, false, false, took, reason);
}

// ── Provisioning-receipt decision ───────────────────────────────────────────

enum class ReceiptVerdict : uint8_t {
  REFUSE_HOST,    // 403 {"error":"host"}: the bearer unread, the gate untouched
  SERVE_BEARER,   // a valid bearer: serve the receipt; the gate is left alone
  SERVE_CLAIM,    // no bearer, but a claim ticket from the Bluetooth setup
                  // door was presented and spent: serve; the tap left alone
  SERVE_TAP,      // no bearer, and this request took an unspent BOOT tap: serve
  REFUSE_NO_TAP,  // no bearer and no tap to take: 403 physical_confirmation_required
};

// GET /api/provisioning-receipt's whole decision, in the order auth_gate
// asks its own questions: the Host first (`foreign_host`, the same verdict
// the gate and the pages use), then a valid bearer, then the tap. Both
// grants are callables so this function decides WHEN they run: `bearer_ok`
// is never called for a foreign Host, and `take_gate` (the firmware's take()
// hook, one atomic exchange) is called at most once, and only when the Host
// passed and no bearer did. A receipt fetch under a foreign name therefore
// gets neither the receipt nor the tap, and the owner's own app or page load
// still finds the tap waiting.
template <typename BearerOk, typename TakeGate>
inline ReceiptVerdict receipt_decide(bool foreign_host, BearerOk bearer_ok,
                                     TakeGate take_gate) {
  if (foreign_host) return ReceiptVerdict::REFUSE_HOST;
  if (bearer_ok()) return ReceiptVerdict::SERVE_BEARER;
  return take_gate() ? ReceiptVerdict::SERVE_TAP : ReceiptVerdict::REFUSE_NO_TAP;
}

// The same decision with a third grant between the bearer and the tap: a
// claim ticket (network/claim_ticket.h) the phone read over the Bluetooth
// setup door once the join it asked for succeeded, presented as
// ?claim=<hex>. `take_claim` is the firmware's claim_ticket::take hook —
// called at most once, only when the Host passed and no bearer did, and
// spending the claim only on a match (a wrong value spends nothing; an
// expired claim is burned; sweep F342). A claim
// that serves leaves the BOOT tap unspent, so the owner's own page load
// still finds it; a spent or absent claim falls through to the tap exactly
// as before, so every caller the three-grant overload admitted is admitted
// here too, in the same order.
template <typename BearerOk, typename TakeClaim, typename TakeGate>
inline ReceiptVerdict receipt_decide(bool foreign_host, BearerOk bearer_ok,
                                     TakeClaim take_claim, TakeGate take_gate) {
  if (foreign_host) return ReceiptVerdict::REFUSE_HOST;
  if (bearer_ok()) return ReceiptVerdict::SERVE_BEARER;
  if (take_claim()) return ReceiptVerdict::SERVE_CLAIM;
  return take_gate() ? ReceiptVerdict::SERVE_TAP : ReceiptVerdict::REFUSE_NO_TAP;
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

// ── Socket address → one key per peer ──────────────────────────────────────

// The 16 bytes that name one peer, whichever family its socket reports: an
// IPv4 address as its IPv4-mapped form ::ffff:a.b.c.d (RFC 4291 §2.5.5.2), an
// IPv6 address as its own 16 bytes. So a client keys the same on an AF_INET
// listener and on the dual-stack AF_INET6 one, which reports it as
// ::ffff:a.b.c.d already, and ipv4_host_order_from_addr(AddrFamily::IPV6,
// key) is the IPv4 of a mapped key and 0 for every other. The WAP's one
// reader of who sent a request (canary_wap.ino request_peer_key(), over
// getpeername) writes it; the per-address bearer lockout keys on that IPv4
// (auth_request_source(), network/auth_backoff.h).
//   IPV4 a.b.c.d          → ::ffff:a.b.c.d
//   IPV6 (any)            → its 16 bytes
//   OTHER, a null `bytes` or `out`, or no address at all (:: or
//   ::ffff:0.0.0.0, the unspecified address in either family) → false
// `out` is zeroed whenever the answer is false, so a refused key is never
// mistaken for a peer.
inline constexpr size_t PEER_KEY_LEN = 16;

inline bool peer_key_from_addr(AddrFamily family, const uint8_t* bytes,
                               uint8_t out[PEER_KEY_LEN]) {
  if (!out) return false;
  for (size_t i = 0; i < PEER_KEY_LEN; ++i) out[i] = 0;
  if (!bytes) return false;
  if (family == AddrFamily::IPV4) {
    out[10] = 0xFF;
    out[11] = 0xFF;
    for (size_t i = 0; i < 4; ++i) out[12 + i] = bytes[i];
  } else if (family == AddrFamily::IPV6) {
    for (size_t i = 0; i < PEER_KEY_LEN; ++i) out[i] = bytes[i];
  } else {
    return false;
  }
  bool prefix_zero = true;
  for (size_t i = 0; i < 10; ++i) prefix_zero = prefix_zero && out[i] == 0;
  const bool mapped_or_zero = prefix_zero && ((out[10] == 0xFF && out[11] == 0xFF) ||
                                              (out[10] == 0 && out[11] == 0));
  const bool tail_zero = out[12] == 0 && out[13] == 0 && out[14] == 0 && out[15] == 0;
  if (mapped_or_zero && tail_zero) {
    for (size_t i = 0; i < PEER_KEY_LEN; ++i) out[i] = 0;
    return false;
  }
  return true;
}

// A peer key as text for the serial console: a.b.c.d for an IPv4-mapped key
// (::ffff:a.b.c.d), else its eight 16-bit groups in hex joined by ':' (no
// :: shortening). Writes at most PEER_KEY_TEXT_CAP bytes, NUL included, and
// returns the characters written; 0, and an empty string when `cap` allows
// one, when `out` cannot hold the longest form or `key` is null.
inline constexpr size_t PEER_KEY_TEXT_CAP = 40;   // 39 characters and the NUL

inline size_t peer_key_text(const uint8_t key[PEER_KEY_LEN], char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!key || cap < PEER_KEY_TEXT_CAP) return 0;
  static const char hex[] = "0123456789abcdef";
  size_t n = 0;
  bool mapped = key[10] == 0xFF && key[11] == 0xFF;
  for (size_t i = 0; i < 10; ++i) mapped = mapped && key[i] == 0;
  if (mapped) {
    for (size_t i = 12; i < PEER_KEY_LEN; ++i) {
      const unsigned v = key[i];
      if (v >= 100) out[n++] = (char)('0' + v / 100);
      if (v >= 10) out[n++] = (char)('0' + (v / 10) % 10);
      out[n++] = (char)('0' + v % 10);
      if (i + 1 < PEER_KEY_LEN) out[n++] = '.';
    }
  } else {
    for (size_t i = 0; i < PEER_KEY_LEN; ++i) {
      out[n++] = hex[(key[i] >> 4) & 0x0F];
      out[n++] = hex[key[i] & 0x0F];
      if ((i & 1) && i + 1 < PEER_KEY_LEN) out[n++] = ':';
    }
  }
  out[n] = '\0';
  return n;
}

// ── The BOOT tap's one asker (the WAP's receipt; sweep F342) ───────────────
//
// A tap identifies nobody: before this, the WAP handed it to whichever
// request asked first, so any host on the network that polled
// GET /api/provisioning-receipt won the owner's next tap and was handed the
// token, the SoftAP password and the TLS pin (and /api/logs, readable by any
// session, says when the tap lands). The WAP has no screen, so the one
// signal it has is who else asks. tap_arbitrate() remembers the last two
// DISTINCT addresses that asked for the receipt without a credential (the
// caller asks it only for a request the bearer and the claim did not
// serve), before a tap as well as after it, and serves the tap only to the
// sole address seen from `lookback_ms` before the tap onward. A second
// address in that window closes the tap. After that, an ask is answered
// CONTENDED while the closed tap's TTL runs AND an address other than the
// asker asked within `lookback_ms` of it: the answer ends when its cause
// ends, so a page that waits `lookback_ms` after the other device stopped
// and asks again is answered NO_TAP and keeps asking for the owner's next
// tap (an earlier version answered CONTENDED for the whole TTL, and a page
// that followed its own "wait 10 seconds" copy stopped again before the
// press). Any ask that finds no other address asking ends it, for every
// asker: a page that comes back while a host still polls hears NO_TAP,
// and its next tap is judged on the lookback like any other, so it is
// closed again. A served tap ends it too. A sole asker is served at once:
// there is no settle delay.
//
// Why two entries answer the question exactly: `recent` is the address of
// the latest ask, and `prior.last_ms` is the time of the latest ask by any
// OTHER address (an ask by `recent` refreshes its time; an ask by anyone
// else pushes `recent` down to `prior`, and `recent`'s time was the latest
// ask overall, so it is the latest by an address other than the new one).
// So "did any other address ask since lookback_ms before the tap?" is one
// comparison against `prior`.
//
// The dashboard's sign-in and the companion's recovery-kit save poll from
// before the tap, so the owner's client is on record when the tap lands: a
// host that polls the route, that times the tap from the logs, or that
// delays the owner's next poll gets CONTENDED, never the receipt. What it
// cannot see: a tap no client of the owner's was asking for (an accidental
// tap, a page or curl started after it, a bare re-tap after a 409) still
// goes to the first asker, a poller included, which is why the WAP's 409
// says to start the request first and keep it asking, then press BOOT;
// hosts behind one address are one asker; a host that keeps the one web
// server busy for longer than the lookback before the tap can hide the
// owner's earlier ask; and a host that asks at least once every
// `lookback_ms` (polling included) makes every tap the owner's client is
// already asking for refuse, loudly.
//
// The record ages on 32-bit millis() and is never swept: after a silence of
// 2^32 ms (49.7 days) a stale entry's age wraps and can read as recent, which
// can only make a tap CONTENDED (it fails closed), never serve one. ONE task
// touches a TapArbiter (on the WAP, the httpd task's receipt handler), so it
// takes no atomics; the gate's stamp it judges is read with peek() and then
// taken with take_exact(), never re-read.
inline constexpr uint32_t TAP_LOOKBACK_MS = 10000;   // five of the dashboard's 2 s polls

struct TapAsker {
  bool has;
  uint8_t key[PEER_KEY_LEN];
  uint32_t last_ms;   // millis() of this address's latest ask
};

struct TapArbiter {
  TapAsker recent;          // the address of the latest ask
  TapAsker prior;           // the latest ask by any other address
  uint32_t contended_tap;   // the stamp of a tap closed by contention, 0 = none
};

enum class TapVerdict : uint8_t {
  NO_TAP,      // no fresh tap to take: the caller refuses (403)
  CONTENDED,   // a second address asked inside the window: 409, the tap closed
  TAKE,        // the sole asker: take_exact() the judged stamp and serve
};

// Judge one credential-less ask from `peer` against the tap stamp
// `stamp_seen` (peek(); 0 = closed) at `now_ms`. The ask is recorded first,
// whatever the answer, so asks made before a tap count.
inline TapVerdict tap_arbitrate(TapArbiter& a, uint32_t stamp_seen,
                                const uint8_t peer[PEER_KEY_LEN], uint32_t now_ms,
                                uint32_t ttl_ms, uint32_t lookback_ms = TAP_LOOKBACK_MS) {
  if (a.recent.has && memcmp(a.recent.key, peer, PEER_KEY_LEN) == 0) {
    a.recent.last_ms = now_ms;
  } else {
    a.prior = a.recent;
    a.recent.has = true;
    memcpy(a.recent.key, peer, PEER_KEY_LEN);
    a.recent.last_ms = now_ms;
  }
  const bool fresh = stamp_seen != 0 && (uint32_t)(now_ms - stamp_seen) < ttl_ms;
  if (a.contended_tap != 0) {
    // `prior` is the latest ask by an address other than this one.
    const bool other_asking =
        a.prior.has && (uint32_t)(now_ms - a.prior.last_ms) <= lookback_ms;
    if ((uint32_t)(now_ms - a.contended_tap) >= ttl_ms || !other_asking) {
      a.contended_tap = 0;   // the closed tap's window has run, or the other
                             // address has gone quiet: judge afresh
    } else if (!(fresh && stamp_seen != a.contended_tap)) {
      return TapVerdict::CONTENDED;   // inside it, and no new tap: still closed
    }
  }
  if (!fresh) return TapVerdict::NO_TAP;
  // Another address asked at or after (tap - lookback_ms): its ask is no
  // older than the tap's age plus the lookback (summed wide, so no TTL and
  // lookback can wrap the bound).
  if (a.prior.has &&
      (uint64_t)(uint32_t)(now_ms - a.prior.last_ms) <=
          (uint64_t)(uint32_t)(now_ms - stamp_seen) + lookback_ms) {
    a.contended_tap = stamp_seen;
    return TapVerdict::CONTENDED;
  }
  // Served: no earlier contention outlives a tap that went to one asker, so
  // the asks after it are NO_TAP, never "the tap was closed". (The rule
  // above already cleared any: a TAKE means no other address asked since
  // lookback_ms before this tap, so none within lookback_ms of now.)
  a.contended_tap = 0;
  return TapVerdict::TAKE;
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
