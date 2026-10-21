// Host tests for firmware/common/network/provisioning_gate.h — the BOOT-tap
// gate behind the provisioning receipt and the page-token policy that stops
// the dashboard from handing the bearer credential to any device on the
// home LAN.
//
// The rows that matter most:
//   * one tap admits exactly one consumer (take() twice → second refused);
//   * the gate self-closes after the TTL;
//   * millis()==0 is stored as 1, never as "closed";
//   * the 49-day uint32 wrap of millis() does not turn a fresh tap stale;
//   * LAN + nothing → WITHHOLD; every grant (setup / bearer / AP / gate) → INJECT;
//   * one tap is ONE consumer across the page and receipt paths: five LAN
//     page loads after a tap inject once, and whichever of page load /
//     receipt fetch comes first leaves nothing for the other;
//   * the four-grant overload (receipt_decide with a claim hook) keeps that
//     order with the Bluetooth setup door's claim between the bearer and the
//     tap: a claim that serves leaves the tap unspent, a spent one falls
//     through to it, and with no claim presented every verdict matches the
//     three-grant overload;
//   * the Host comes first on both paths: a request whose Host cannot name
//     the device gets no credential, is refused on the receipt route before
//     the bearer is read, and never spends the tap — foreign-Host requests
//     throughout the TTL leave it for the owner's own request;
//   * ipv4_in_subnet / request_on_softap answer false for every "not provably
//     over the Canary's own Wi-Fi" input (wrong interface, overlapping home
//     subnet, unusable mask, AP down);
//   * ipv4_host_order_from_addr unwraps the ::ffff:a.b.c.d address the
//     dual-stack httpd socket really reports, and reads every true IPv6
//     address as 0 (without it, every real request looked like "unknown");
//   * peer_key_from_addr names one peer by one 16-byte key in either family
//     (IPv4 as ::ffff:a.b.c.d, IPv6 as itself), refuses the unspecified
//     address, an unknown family and a null pointer, and the IPv4 of every
//     key it writes is the IPv4 ipv4_host_order_from_addr reads off the
//     socket address itself: the WAP's per-address lockout keys on it;
//   * the BOOT tap goes to ONE asker on the WAP (sweep F342): peek() reads
//     the stamp without consuming it and take_exact() consumes only the
//     stamp that was judged (a re-tap, a BLE OTA take or a close between the
//     two leaves it); tap_arbitrate(), driven the way the WAP's receipt
//     drives it on a 32-bit clock: a poller and the owner's dashboard across
//     one tap are both refused and the tap closed; a sole owner asking from
//     before the tap is served on its first ask after it, once; an attacker
//     that asks first after the tap loses to an owner who asked before it;
//     an address silent for longer than the lookback does not contend, at
//     the boundary to the millisecond; one address alone is served (the
//     residual); a clean re-tap after contention serves and a re-tap inside
//     the lookback does not; the 409 ends once the other address has been
//     quiet for the lookback (to the millisecond), so a retry 10 s after the
//     other device stopped is NO_TAP and its next tap serves, while two
//     addresses still asking hear it to the closed tap's TTL; a re-tap
//     served to anyone ends it; the same address never contends with
//     itself; the record fails closed across a 49.7-day silence; and a peer
//     key prints as a.b.c.d (only behind a zero prefix) or as eight hex
//     groups;
//   * the sweep (sweep F347): expire() clears a stamp at its TTL and never
//     one a millisecond younger, and its step on a stamp it read
//     (expire_seen()) never clears one re-opened since that read,
//     and a swept tap stays closed when millis() comes back round to it
//     2^32 ms later, where an unswept one reads open again (the
//     characterization row: why the sweep exists); and the canary tree's
//     handle_boot_button() (firmware/canary/src/main.cpp, its path passed in
//     as CANARY_MAIN_CPP) sweeps g_prov_gate as its first statement, from
//     loop() on every pass.

#include "../common/network/provisioning_gate.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>

#ifndef CANARY_MAIN_CPP
#error "CANARY_MAIN_CPP must name firmware/canary/src/main.cpp (the Makefile passes it)"
#endif

using namespace canary::net::provisioning_gate;

static int g_failures = 0;

#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d: ", __func__, __LINE__);         \
      std::printf(__VA_ARGS__);                                \
      std::printf("\n");                                       \
      ++g_failures;                                            \
    }                                                          \
  } while (0)

static const uint32_t kTtl = 30000;

static void closed_by_default() {
  State s{0};
  CHECK(!is_open(s, 1000, kTtl), "a zeroed state is closed");
  CHECK(!take(s, 1000, kTtl), "a zeroed state cannot be taken");
}

static void open_then_peek_does_not_consume() {
  State s{0};
  open(s, 5000);
  CHECK(is_open(s, 5000, kTtl), "open at the tap instant");
  CHECK(is_open(s, 5000 + kTtl - 1, kTtl), "open one ms before expiry");
  CHECK(is_open(s, 6000, kTtl), "a peek must not consume the tap");
  CHECK(take(s, 7000, kTtl), "take after peeks still admits");
}

static void one_tap_admits_exactly_one_consumer() {
  State s{0};
  open(s, 1000);
  CHECK(take(s, 1500, kTtl), "first consumer admitted");
  CHECK(!take(s, 1500, kTtl), "second consumer refused — the tap was spent");
  CHECK(!is_open(s, 1500, kTtl), "gate reads closed after take");
}

static void gate_self_closes_after_ttl() {
  State s{0};
  open(s, 1000);
  CHECK(!is_open(s, 1000 + kTtl, kTtl), "closed exactly at expiry");
  CHECK(!take(s, 1000 + kTtl, kTtl), "an expired tap cannot be taken");
  // And the expired stamp was consumed by that take — a later, earlier-looking
  // clock (wrap) cannot revive it.
  CHECK(!take(s, 1001, kTtl), "an expired stamp is spent by the refused take");
}

static void millis_zero_is_a_real_tap() {
  State s{0};
  open(s, 0);
  CHECK(s.opened_at_ms == 1, "millis()==0 stored as the sentinel 1, got %u", (unsigned)s.opened_at_ms);
  CHECK(is_open(s, 10, kTtl), "a tap at millis()==0 is open");
  CHECK(take(s, 10, kTtl), "and can be taken");
}

static void uint32_wraparound_is_not_expiry() {
  State s{0};
  const uint32_t near_wrap = 0xFFFFFFF0u;
  open(s, near_wrap);
  const uint32_t after_wrap = near_wrap + 100u;  // wrapped to a small number
  CHECK(after_wrap < near_wrap, "test premise: the clock wrapped");
  CHECK(is_open(s, after_wrap, kTtl), "a fresh tap across the wrap stays open");
  CHECK(!is_open(s, near_wrap + kTtl, kTtl), "and still expires after the TTL across the wrap");
}

static void explicit_close() {
  State s{0};
  open(s, 100);
  close(s);
  CHECK(!is_open(s, 100, kTtl), "close() shuts the gate");
  CHECK(!take(s, 100, kTtl), "close() leaves nothing to take");
}

// ── the sweep (sweep F347) ──────────────────────────────────────────────────

static const uint64_t kWrap = 1ull << 32;   // 2^32 ms: 49.7 days of millis()

static void expire_clears_at_the_ttl_and_never_a_fresh_stamp() {
  State s{0};
  CHECK(!expire(s, 1000, kTtl), "a closed gate: nothing to sweep");
  open(s, 1000);
  CHECK(!expire(s, 1000, kTtl), "a tap this millisecond is not swept");
  CHECK(!expire(s, 1000 + kTtl - 1, kTtl), "one ms before the TTL: not swept");
  CHECK(peek(s) == 1000, "and the stamp is kept, got %u", (unsigned)peek(s));
  CHECK(is_open(s, 1000 + kTtl - 1, kTtl), "still open");
  CHECK(expire(s, 1000 + kTtl, kTtl), "at the TTL: swept");
  CHECK(peek(s) == 0, "the gate is closed, got %u", (unsigned)peek(s));
  CHECK(!expire(s, 1000 + kTtl, kTtl), "a second sweep finds nothing");
  // Across the wrap: a tap stamped just before millis() wraps is swept on
  // its age, not on the raw numbers.
  State w{0};
  const uint32_t near_wrap = 0xFFFFF000u;
  open(w, near_wrap);
  CHECK(!expire(w, near_wrap + kTtl - 1, kTtl), "across the wrap, one ms before the TTL: kept");
  CHECK(expire(w, near_wrap + kTtl, kTtl), "and swept at the TTL");
}

static void a_swept_tap_does_not_revive_after_the_wrap() {
  // The owner's loop sweeps every pass: the tap nobody took is cleared at
  // its TTL, and millis() coming back round 2^32 ms after the tap finds
  // nothing to serve.
  State s{0};
  open(s, 1000);
  CHECK(expire(s, 1000 + kTtl, kTtl), "the loop's sweep at the TTL clears it");
  const uint32_t later = (uint32_t)(1000 + kWrap + 5000);
  CHECK(later == 6000, "test premise: 2^32 ms + 5 s after the tap reads 6000, got %u", (unsigned)later);
  CHECK(!is_open(s, later, kTtl), "a swept tap is closed when millis() comes back round");
  CHECK(!take(s, later, kTtl), "and gives nothing to take");
  uint8_t key[PEER_KEY_LEN] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 168, 1, 20};
  TapArbiter a{};
  CHECK(tap_arbitrate(a, peek(s), key, later, kTtl) == TapVerdict::NO_TAP,
        "the receipt's arbiter sees no tap either");
}

// Characterization: why the sweep exists. A stamp nobody took and nobody
// swept reads open again once millis() comes back round to it, and take()
// hands it out: the receipt for 30 s with no press.
static void an_unswept_tap_reads_open_after_the_wrap() {
  State s{0};
  open(s, 1000);
  CHECK(!is_open(s, 1000 + kTtl, kTtl), "closed at the TTL by the clock alone");
  const uint32_t later = (uint32_t)(1000 + kWrap + 5000);
  CHECK(is_open(s, later, kTtl), "unswept, it reads open 2^32 ms + 5 s after the tap");
  CHECK(take(s, later, kTtl), "and take() hands it out");
}

static void expire_leaves_a_stamp_reopened_since_it_was_read() {
  // The sweep's compare-and-swap, through its seam: a sweep that read an
  // expired stamp and finds a re-tap stored before its swap clears nothing.
  // (The loop task opens and sweeps, so on a device the re-tap cannot land
  // in between; the swap is the guard if a second task ever opens.)
  State s{0};
  open(s, 1000);
  const uint32_t seen = peek(s);
  open(s, 1000 + kTtl + 50);   // the owner taps again, after the sweep's read
  CHECK(!expire_seen(s, seen, 1000 + kTtl + 60, kTtl), "the sweep's swap of the old stamp fails");
  CHECK(peek(s) == 1000 + kTtl + 50, "the re-tap's stamp is kept");
  CHECK(is_open(s, 1000 + kTtl + 60, kTtl), "and the gate is open for it");
  CHECK(!expire(s, 1000 + kTtl + 60, kTtl), "a sweep now reads the fresh tap and keeps it");
  // The same seam on an unmoved stamp clears it, and on a fresh one keeps it.
  State u{0};
  open(u, 1000);
  CHECK(!expire_seen(u, peek(u), 1000 + kTtl - 1, kTtl), "one ms before the TTL: kept");
  CHECK(expire_seen(u, peek(u), 1000 + kTtl, kTtl), "at the TTL, unmoved: cleared");
  CHECK(peek(u) == 0, "closed");
  CHECK(!expire_seen(u, 0, 1000 + kTtl, kTtl), "a closed gate: nothing");
}

// ── the canary tree's sweep (firmware/canary/src/main.cpp) ──────────────────

static std::string read_text(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// C and C++ comments out, string and character literals honored, newlines
// kept (check_route_security.py's strip_comments, in C++).
static std::string strip_comments(const std::string& t) {
  std::string out;
  enum { CODE, LINE, BLOCK, STR, CHR } st = CODE;
  for (size_t i = 0; i < t.size(); ++i) {
    const char c = t[i];
    const char n = i + 1 < t.size() ? t[i + 1] : '\0';
    switch (st) {
      case CODE:
        if (c == '/' && n == '/') { st = LINE; ++i; }
        else if (c == '/' && n == '*') { st = BLOCK; ++i; }
        else { out += c; if (c == '"') st = STR; else if (c == '\'') st = CHR; }
        break;
      case LINE:
        if (c == '\n') { out += c; st = CODE; }
        break;
      case BLOCK:
        if (c == '*' && n == '/') { st = CODE; ++i; }
        else out += (c == '\n') ? '\n' : ' ';
        break;
      case STR:
      case CHR:
        out += c;
        if (c == '\\' && i + 1 < t.size()) out += t[++i];
        else if ((st == STR && c == '"') || (st == CHR && c == '\'')) st = CODE;
        break;
    }
  }
  return out;
}

// The text of the function whose definition line starts with `sig`, to the
// first line that is exactly "}"; empty unless that line starts the file or
// follows a newline exactly once.
static std::string fn_body(const std::string& src, const std::string& sig) {
  const std::string at_line = "\n" + sig;
  const size_t at = src.find(at_line);
  if (at == std::string::npos || src.find(at_line, at + 1) != std::string::npos) return "";
  const size_t end = src.find("\n}\n", at);
  return end == std::string::npos ? "" : src.substr(at + 1, end - at);
}

static void the_flagship_sweeps_its_tap_every_pass() {
  const std::string src = strip_comments(read_text(CANARY_MAIN_CPP));
  CHECK(!src.empty(), "could not read %s", CANARY_MAIN_CPP);
  const std::string boot = fn_body(src, "static void handle_boot_button() {");
  CHECK(!boot.empty(), "handle_boot_button()'s definition not found once");
  const std::string sweep =
      "canary::net::provisioning_gate::expire(g_prov_gate, millis(), PROVISIONING_GATE_TTL_MS);";
  const size_t at = boot.find(sweep);
  CHECK(at != std::string::npos, "handle_boot_button() does not sweep g_prov_gate");
  // Its first statement after the two statics: nothing can return before it.
  const std::string head =
      "static void handle_boot_button() {\n"
      "  static uint32_t boot_btn_start = 0;\n"
      "  static bool boot_btn_was_pressed = false;\n";
  CHECK(boot.compare(0, head.size(), head) == 0, "handle_boot_button() no longer opens with its two statics");
  if (at != std::string::npos && boot.compare(0, head.size(), head) == 0) {
    std::string between = boot.substr(head.size(), at - head.size());
    bool blank = true;
    for (char c : between) blank = blank && (c == ' ' || c == '\n' || c == '\t');
    CHECK(blank, "the sweep is not handle_boot_button()'s first statement: '%s'", between.c_str());
  }
  CHECK(boot.find("provisioning_gate::open(g_prov_gate, millis())") != std::string::npos,
        "the sweep sits in the handler that opens the gate (the same task)");
  // And loop() calls the handler on every pass.
  const std::string loop = fn_body(src, "void loop() {");
  CHECK(!loop.empty(), "loop()'s definition not found once");
  CHECK(loop.find("\n  handle_boot_button();\n") != std::string::npos,
        "loop() does not call handle_boot_button() at its top level");
}

// ── page-token policy truth table ───────────────────────────────────────────

static void lan_with_no_grant_withholds() {
  const char* why = nullptr;
  CHECK(page_token_policy(false, false, false, false, &why) == PageToken::WITHHOLD,
        "LAN + no grant must WITHHOLD");
  CHECK(why && std::strstr(why, "withheld") != nullptr, "reason names the withhold");
}

static void every_single_grant_injects() {
  const char* why = nullptr;
  CHECK(page_token_policy(true, false, false, false, &why) == PageToken::INJECT, "setup active → INJECT");
  CHECK(why && std::strstr(why, "setup") != nullptr, "reason: setup");
  CHECK(page_token_policy(false, true, false, false, &why) == PageToken::INJECT, "bearer ok → INJECT");
  CHECK(why && std::strstr(why, "bearer") != nullptr, "reason: bearer");
  CHECK(page_token_policy(false, false, true, false, &why) == PageToken::INJECT, "AP subnet → INJECT");
  CHECK(why && std::strstr(why, "SoftAP") != nullptr, "reason: AP");
  CHECK(page_token_policy(false, false, false, true, &why) == PageToken::INJECT, "gate open → INJECT");
  CHECK(why && std::strstr(why, "gate") != nullptr, "reason: gate");
}

static void reason_pointer_is_optional() {
  CHECK(page_token_policy(false, false, false, false, nullptr) == PageToken::WITHHOLD,
        "nullptr reason is accepted");
  CHECK(page_token_policy(true, true, true, true, nullptr) == PageToken::INJECT,
        "all grants together still INJECT");
}

// ── one tap, one consumer, across the page and receipt paths ────────────────
//
// The firmware's two consumers call the header's decisions directly:
//   page load    → page_token_decide(foreign, setup, bearer, on_ap, take-hook)
//   receipt GET  → receipt_decide(foreign, bearer-hook, take-hook)
// (page_token_inject and handle_provisioning_receipt in securacv_network),
// so these helpers run the same functions the handlers run.

struct Taker {
  State* s;
  uint32_t now;
  int calls;
  bool operator()() {
    ++calls;
    return take(*s, now, kTtl);
  }
};

static bool lan_page_load(State& s, uint32_t now, int* take_calls = nullptr) {
  Taker t{&s, now, 0};
  const bool inject =
      page_token_decide(false, false, false, false, std::ref(t)) == PageToken::INJECT;
  if (take_calls) *take_calls = t.calls;
  return inject;
}

// Counts how often the receipt decision read the bearer.
struct Bearer {
  bool ok;
  int calls;
  bool operator()() {
    ++calls;
    return ok;
  }
};

static ReceiptVerdict receipt_verdict(State& s, uint32_t now, bool foreign_host,
                                      bool bearer_ok, int* bearer_calls = nullptr,
                                      int* take_calls = nullptr) {
  Bearer b{bearer_ok, 0};
  Taker t{&s, now, 0};
  const ReceiptVerdict v = receipt_decide(foreign_host, std::ref(b), std::ref(t));
  if (bearer_calls) *bearer_calls = b.calls;
  if (take_calls) *take_calls = t.calls;
  return v;
}

static bool receipt_served(ReceiptVerdict v) {
  return v == ReceiptVerdict::SERVE_BEARER || v == ReceiptVerdict::SERVE_TAP;
}

static bool receipt_fetch(State& s, uint32_t now, bool bearer_ok) {
  return receipt_served(receipt_verdict(s, now, false, bearer_ok));
}

static void one_tap_unlocks_exactly_one_lan_page_load() {
  State s{0};
  open(s, 1000);
  int injected = 0;
  for (int i = 0; i < 5; ++i) {
    if (lan_page_load(s, 1000 + (uint32_t)i * 100)) ++injected;
  }
  CHECK(injected == 1, "one tap, five LAN page loads → exactly one INJECT, got %d", injected);
  CHECK(!is_open(s, 1600, kTtl), "the page load spent the tap");
  CHECK(!receipt_fetch(s, 1700, false), "and the tapless receipt fetch is refused afterwards");
  // The page that won holds the bearer, so its Save-recovery-kit still works.
  CHECK(receipt_fetch(s, 1800, true), "a bearer-carrying receipt fetch needs no tap");
}

static void receipt_first_leaves_nothing_for_a_page_load() {
  State s{0};
  open(s, 1000);
  CHECK(receipt_fetch(s, 1200, false), "the app's receipt fetch spends the tap");
  CHECK(!lan_page_load(s, 1300), "a LAN page load after it is withheld");
}

static void standing_grants_never_spend_the_tap() {
  State s{0};
  open(s, 1000);
  Taker t{&s, 1100, 0};
  CHECK(page_token_decide(false, true, false, false, std::ref(t)) == PageToken::INJECT, "setup → INJECT");
  CHECK(page_token_decide(false, false, true, false, std::ref(t)) == PageToken::INJECT, "bearer → INJECT");
  CHECK(page_token_decide(false, false, false, true, std::ref(t)) == PageToken::INJECT, "SoftAP → INJECT");
  CHECK(t.calls == 0, "no standing grant touches the gate, %d take calls", t.calls);
  CHECK(is_open(s, 1100, kTtl), "the tap is still there for the one consumer that needs it");
  int calls = -1;
  CHECK(lan_page_load(s, 1200, &calls), "a LAN load then spends it");
  CHECK(calls == 1, "exactly one take per decision, got %d", calls);
}

static void no_tap_withholds_and_names_the_reason() {
  State s{0};
  int calls = -1;
  CHECK(!lan_page_load(s, 1000, &calls), "no tap → WITHHOLD");
  CHECK(calls == 1, "the gate was asked once, got %d", calls);
  open(s, 2000);
  Taker t{&s, 2100, 0};
  const char* why = nullptr;
  CHECK(page_token_decide(false, false, false, false, std::ref(t), &why) == PageToken::INJECT, "tap → INJECT");
  CHECK(why && std::strstr(why, "gate") != nullptr, "reason names the gate");
  CHECK(page_token_decide(false, false, false, false, std::ref(t), &why) == PageToken::WITHHOLD, "spent → WITHHOLD");
  CHECK(why && std::strstr(why, "withheld") != nullptr, "reason names the withhold");
  // An expired tap grants nothing either.
  open(s, 3000);
  CHECK(!lan_page_load(s, 3000 + kTtl), "an expired tap → WITHHOLD");
}

// ── the Host first, on both paths ───────────────────────────────────────────
//
// `foreign_host` is the firmware's host_is_foreign(): the Host cannot name
// this device (host_guard.h) and the request did not arrive over the SoftAP
// (over the AP it is always false, whatever the name). The rows below pin
// the order on both paths: the receipt refuses a foreign Host before the
// bearer or the tap is consulted, and a page load under one leaves the tap
// unspent for the owner's own request.

static void foreign_host_receipt_is_refused_before_bearer_and_tap() {
  State s{0};
  open(s, 1000);
  int bearer_calls = -1, take_calls = -1;
  CHECK(receipt_verdict(s, 1100, true, false, &bearer_calls, &take_calls) ==
            ReceiptVerdict::REFUSE_HOST,
        "foreign Host, no bearer, an open tap → REFUSE_HOST");
  CHECK(bearer_calls == 0, "the bearer is not read for a foreign Host, %d reads", bearer_calls);
  CHECK(take_calls == 0, "the gate is not touched for a foreign Host, %d takes", take_calls);
  CHECK(is_open(s, 1100, kTtl), "the tap is still there after the foreign poll");

  // Like auth_gate: a foreign-Host request that somehow holds the token still
  // cannot use it here.
  CHECK(receipt_verdict(s, 1200, true, true, &bearer_calls, &take_calls) ==
            ReceiptVerdict::REFUSE_HOST,
        "foreign Host with a valid bearer → REFUSE_HOST");
  CHECK(bearer_calls == 0 && take_calls == 0, "and neither grant was consulted (%d, %d)",
        bearer_calls, take_calls);

  // The owner's own app, under a name that means the device, gets the tap.
  CHECK(receipt_verdict(s, 1300, false, false, &bearer_calls, &take_calls) ==
            ReceiptVerdict::SERVE_TAP,
        "the owner's receipt fetch after the foreign polls → SERVE_TAP");
  CHECK(bearer_calls == 1 && take_calls == 1, "bearer read once, tap taken once (%d, %d)",
        bearer_calls, take_calls);
}

static void receipt_order_on_a_device_host() {
  State s{0};
  open(s, 1000);
  int bearer_calls = -1, take_calls = -1;
  CHECK(receipt_verdict(s, 1100, false, true, &bearer_calls, &take_calls) ==
            ReceiptVerdict::SERVE_BEARER,
        "valid bearer → SERVE_BEARER");
  CHECK(take_calls == 0, "a bearer fetch never spends the tap, %d takes", take_calls);
  CHECK(is_open(s, 1100, kTtl), "the tap survives a bearer fetch");
  CHECK(receipt_verdict(s, 1200, false, false) == ReceiptVerdict::SERVE_TAP,
        "no bearer, open tap → SERVE_TAP");
  CHECK(receipt_verdict(s, 1300, false, false, &bearer_calls, &take_calls) ==
            ReceiptVerdict::REFUSE_NO_TAP,
        "the second tapless fetch → REFUSE_NO_TAP");
  CHECK(take_calls == 1, "it asked the gate once, %d takes", take_calls);
  State closed{0};
  CHECK(receipt_verdict(closed, 1400, false, false) == ReceiptVerdict::REFUSE_NO_TAP,
        "never tapped → REFUSE_NO_TAP");
}

// Every (foreign, bearer, tap) combination: the verdict, and exactly which
// grants were consulted.
static void receipt_truth_table() {
  for (int foreign = 0; foreign <= 1; ++foreign) {
    for (int bearer = 0; bearer <= 1; ++bearer) {
      for (int tapped = 0; tapped <= 1; ++tapped) {
        State s{0};
        if (tapped) open(s, 1000);
        int bearer_calls = -1, take_calls = -1;
        const ReceiptVerdict v =
            receipt_verdict(s, 1100, foreign != 0, bearer != 0, &bearer_calls, &take_calls);
        ReceiptVerdict want = ReceiptVerdict::REFUSE_NO_TAP;
        if (foreign) {
          want = ReceiptVerdict::REFUSE_HOST;
        } else if (bearer) {
          want = ReceiptVerdict::SERVE_BEARER;
        } else if (tapped) {
          want = ReceiptVerdict::SERVE_TAP;
        }
        CHECK(v == want, "foreign=%d bearer=%d tapped=%d: verdict %d, want %d", foreign,
              bearer, tapped, (int)v, (int)want);
        CHECK(bearer_calls == (foreign ? 0 : 1), "foreign=%d bearer=%d tapped=%d: %d bearer reads",
              foreign, bearer, tapped, bearer_calls);
        CHECK(take_calls == ((foreign || bearer) ? 0 : 1),
              "foreign=%d bearer=%d tapped=%d: %d takes", foreign, bearer, tapped, take_calls);
        CHECK(is_open(s, 1100, kTtl) == (tapped && (foreign || bearer)),
              "foreign=%d bearer=%d tapped=%d: the tap is left exactly when it was not spent",
              foreign, bearer, tapped);
      }
    }
  }
}

static void foreign_host_page_load_leaves_the_tap() {
  State s{0};
  open(s, 1000);
  Taker t{&s, 1100, 0};
  const char* why = nullptr;
  CHECK(page_token_decide(true, false, false, false, std::ref(t), &why) ==
            PageToken::FOREIGN_HOST,
        "foreign Host, an open tap → FOREIGN_HOST");
  CHECK(t.calls == 0, "a foreign-Host page load does not take the tap, %d takes", t.calls);
  CHECK(is_open(s, 1100, kTtl), "the tap is still there after it");
  CHECK(why && std::strstr(why, "Host") != nullptr, "reason names the Host");
  CHECK(lan_page_load(s, 1200), "the owner's own page load then gets the token");
}

// No standing grant outweighs a foreign Host, and none of them lets it take.
static void foreign_host_page_truth_table() {
  for (int setup = 0; setup <= 1; ++setup) {
    for (int bearer = 0; bearer <= 1; ++bearer) {
      for (int on_ap = 0; on_ap <= 1; ++on_ap) {
        State s{0};
        open(s, 1000);
        Taker t{&s, 1100, 0};
        const PageToken v =
            page_token_decide(true, setup != 0, bearer != 0, on_ap != 0, std::ref(t));
        CHECK(v == PageToken::FOREIGN_HOST, "setup=%d bearer=%d on_ap=%d: verdict %d", setup,
              bearer, on_ap, (int)v);
        CHECK(t.calls == 0, "setup=%d bearer=%d on_ap=%d: %d takes", setup, bearer, on_ap,
              t.calls);
      }
    }
  }
  // And a device-naming Host is exactly the policy it was.
  State s{0};
  Taker t{&s, 1100, 0};
  CHECK(page_token_decide(false, false, false, false, std::ref(t)) == PageToken::WITHHOLD,
        "device Host, no grant, no tap → WITHHOLD (never FOREIGN_HOST)");
}

// Foreign-Host requests arriving throughout the window, receipt fetches and
// page loads alike, get nothing and never spend the tap, which then admits
// the owner's request at the last millisecond of the TTL.
static void foreign_host_requests_through_the_whole_ttl_leave_the_tap() {
  State s{0};
  open(s, 1000);
  int served = 0, injected = 0, takes = 0;
  for (uint32_t now = 1000; now < 1000 + kTtl - 1; now += 250) {
    int take_calls = 0;
    if (receipt_served(receipt_verdict(s, now, true, false, nullptr, &take_calls))) ++served;
    takes += take_calls;
    Taker t{&s, now, 0};
    if (page_token_decide(true, false, false, false, std::ref(t)) == PageToken::INJECT) ++injected;
    takes += t.calls;
  }
  CHECK(served == 0 && injected == 0, "foreign-Host requests got %d receipts, %d tokens", served,
        injected);
  CHECK(takes == 0, "and took the gate %d times", takes);
  CHECK(receipt_fetch(s, 1000 + kTtl - 1, false), "the owner's fetch still finds the tap");
}

// ── subnet check ────────────────────────────────────────────────────────────

static uint32_t ip4(unsigned a, unsigned b, unsigned c, unsigned d) {
  return (a << 24) | (b << 16) | (c << 8) | d;
}

static void ap_subnet_match_is_conservative() {
  const uint32_t ap = ip4(192, 168, 4, 1);
  const uint32_t mask = ip4(255, 255, 255, 0);
  CHECK(ipv4_in_subnet(ip4(192, 168, 4, 2), ap, mask), "AP client matches");
  CHECK(ipv4_in_subnet(ip4(192, 168, 4, 254), ap, mask), "last host matches");
  CHECK(!ipv4_in_subnet(ip4(192, 168, 1, 20), ap, mask), "home LAN peer does not match");
  CHECK(!ipv4_in_subnet(ip4(10, 0, 0, 5), ap, mask), "other private range does not match");
  CHECK(!ipv4_in_subnet(ip4(192, 168, 4, 2), ap, 0), "zero mask → false");
  CHECK(!ipv4_in_subnet(ip4(192, 168, 4, 2), 0, mask), "zero AP address → false (AP down)");
  CHECK(!ipv4_in_subnet(0, ap, mask), "zero peer → false");
  CHECK(!ipv4_in_subnet(ip4(192, 168, 4, 2), ap, ip4(255, 0, 255, 0)),
        "non-contiguous mask → false");
  CHECK(ipv4_in_subnet(ap, ap, 0xFFFFFFFFu), "/32 matches only the AP itself");
  CHECK(!ipv4_in_subnet(ip4(192, 168, 4, 2), ap, 0xFFFFFFFFu), "/32 rejects a client");
}

static void request_on_softap_needs_the_ap_interface_and_no_overlap() {
  const uint32_t ap = ip4(192, 168, 4, 1);
  const uint32_t m24 = ip4(255, 255, 255, 0);
  const uint32_t client = ip4(192, 168, 4, 2);
  const uint32_t sta = ip4(192, 168, 1, 50);

  CHECK(request_on_softap(client, ap, ap, m24, 0, 0), "AP-only: client on the AP address → true");
  CHECK(request_on_softap(client, ap, ap, m24, sta, m24),
        "dual mode, disjoint subnets, arrived on the AP address → true");

  // Arrived on the STA address: a LAN request, whatever the peer says.
  CHECK(!request_on_softap(client, sta, ap, m24, sta, m24), "local = STA address → false");
  CHECK(!request_on_softap(client, 0, ap, m24, 0, 0), "unknown local address → false");
  // Peer outside the AP subnet.
  CHECK(!request_on_softap(ip4(192, 168, 1, 20), ap, ap, m24, sta, m24), "LAN peer → false");
  // AP down.
  CHECK(!request_on_softap(client, 0, 0, m24, sta, m24), "AP address 0 (down) → false");

  // The home router uses the AP's own range: ambiguous → treated as LAN.
  const uint32_t sta_same = ip4(192, 168, 4, 77);
  CHECK(!request_on_softap(client, ap, ap, m24, sta_same, m24),
        "overlapping STA subnet (router on 192.168.4.0/24) → false");
  // A wide STA prefix that contains the AP range overlaps too.
  CHECK(!request_on_softap(client, ap, ap, m24, ip4(192, 168, 0, 9), ip4(255, 255, 0, 0)),
        "STA /16 containing the AP /24 → false");
  // A STA address whose mask cannot prove anything.
  CHECK(!request_on_softap(client, ap, ap, m24, sta, 0), "STA up with zero mask → false");
  CHECK(!request_on_softap(client, ap, ap, m24, sta, ip4(255, 0, 255, 0)),
        "STA up with a non-prefix mask → false");
}

// ── socket address → IPv4 (the dual-stack listener) ─────────────────────────

static void v4_and_v4_mapped_addresses_unwrap() {
  const uint8_t v4[4] = {192, 168, 4, 2};
  CHECK(ipv4_host_order_from_addr(AddrFamily::IPV4, v4) == ip4(192, 168, 4, 2),
        "AF_INET a.b.c.d reads as itself");

  // ::ffff:192.168.4.2 — what lwIP's getpeername reports for an IPv4 client
  // of esp_http_server's AF_INET6 dual-stack listener.
  const uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 168, 4, 2};
  CHECK(ipv4_host_order_from_addr(AddrFamily::IPV6, mapped) == ip4(192, 168, 4, 2),
        "AF_INET6 ::ffff:a.b.c.d unwraps to a.b.c.d");
  const uint8_t mapped_lan[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 10, 0, 0, 9};
  CHECK(ipv4_host_order_from_addr(AddrFamily::IPV6, mapped_lan) == ip4(10, 0, 0, 9),
        "a v4-mapped home-LAN address unwraps too (the receipt's base_url)");
}

static void real_ipv6_and_unknown_families_read_as_zero() {
  const uint8_t global[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  CHECK(ipv4_host_order_from_addr(AddrFamily::IPV6, global) == 0, "2001:db8::1 → 0");
  const uint8_t link_local[16] = {0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0x11, 0x22, 0xFF, 0xFE, 0x33, 0x44, 0x55};
  CHECK(ipv4_host_order_from_addr(AddrFamily::IPV6, link_local) == 0, "fe80:: link-local → 0");
  const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  CHECK(ipv4_host_order_from_addr(AddrFamily::IPV6, loopback) == 0, "::1 → 0");
  // Deprecated IPv4-compatible ::a.b.c.d is NOT the mapped prefix.
  const uint8_t compat[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 192, 168, 4, 2};
  CHECK(ipv4_host_order_from_addr(AddrFamily::IPV6, compat) == 0, "::a.b.c.d (compat) → 0");
  // One stray bit in the prefix and it is an ordinary IPv6 address.
  const uint8_t near_mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xFF, 0xFF, 192, 168, 4, 2};
  CHECK(ipv4_host_order_from_addr(AddrFamily::IPV6, near_mapped) == 0, "::1:ffff:a.b.c.d → 0");
  const uint8_t half_ff[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0x00, 192, 168, 4, 2};
  CHECK(ipv4_host_order_from_addr(AddrFamily::IPV6, half_ff) == 0, "::ff00:a.b.c.d → 0");
  const uint8_t v4[4] = {192, 168, 4, 2};
  CHECK(ipv4_host_order_from_addr(AddrFamily::OTHER, v4) == 0, "unknown family → 0");
  CHECK(ipv4_host_order_from_addr(AddrFamily::IPV4, nullptr) == 0, "null bytes → 0");
  CHECK(ipv4_host_order_from_addr(AddrFamily::IPV6, nullptr) == 0, "null bytes (v6) → 0");
}

// ── socket address → one key per peer ──────────────────────────────────────

static bool key_is(const uint8_t key[PEER_KEY_LEN], const uint8_t want[PEER_KEY_LEN]) {
  return std::memcmp(key, want, PEER_KEY_LEN) == 0;
}

static void peer_key_names_one_peer_in_either_family() {
  static_assert(PEER_KEY_LEN == 16, "a peer key is an IPv6 address's 16 bytes");
  uint8_t key[PEER_KEY_LEN];

  // AF_INET 192.168.1.37 and the dual-stack listener's ::ffff:192.168.1.37
  // are one peer, one key.
  const uint8_t v4[4] = {192, 168, 1, 37};
  const uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 168, 1, 37};
  CHECK(peer_key_from_addr(AddrFamily::IPV4, v4, key), "an IPv4 peer has a key");
  CHECK(key_is(key, mapped), "IPv4 a.b.c.d keys as ::ffff:a.b.c.d");
  CHECK(peer_key_from_addr(AddrFamily::IPV6, mapped, key), "a mapped peer has a key");
  CHECK(key_is(key, mapped), "::ffff:a.b.c.d keys as itself");

  // A real IPv6 peer keys as its own 16 bytes, global and link-local alike.
  const uint8_t global[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  const uint8_t link_local[16] = {0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0x11, 0x22, 0xFF, 0xFE, 0x33, 0x44, 0x55};
  CHECK(peer_key_from_addr(AddrFamily::IPV6, global, key) && key_is(key, global), "2001:db8::1 is copied");
  CHECK(peer_key_from_addr(AddrFamily::IPV6, link_local, key) && key_is(key, link_local), "fe80:: is copied");

  // No peer at all: refused, and the key left zero.
  const uint8_t zero16[16] = {0};
  const uint8_t mapped_zero[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0};
  const uint8_t zero4[4] = {0, 0, 0, 0};
  std::memset(key, 0xAA, sizeof(key));
  CHECK(!peer_key_from_addr(AddrFamily::IPV6, zero16, key), ":: is no peer");
  CHECK(key_is(key, zero16), "a refused key is zeroed");
  std::memset(key, 0xAA, sizeof(key));
  CHECK(!peer_key_from_addr(AddrFamily::IPV6, mapped_zero, key), "::ffff:0.0.0.0 is no peer");
  CHECK(key_is(key, zero16), "a refused key is zeroed");
  CHECK(!peer_key_from_addr(AddrFamily::IPV4, zero4, key), "0.0.0.0 is no peer");
  std::memset(key, 0xAA, sizeof(key));
  CHECK(!peer_key_from_addr(AddrFamily::OTHER, v4, key), "an unknown family is refused");
  CHECK(key_is(key, zero16), "a refused key is zeroed");
  CHECK(!peer_key_from_addr(AddrFamily::IPV4, nullptr, key), "null bytes are refused");
  CHECK(!peer_key_from_addr(AddrFamily::IPV4, v4, nullptr), "a null key is refused");

  // The lockout's bucket and the address the socket names agree: the IPv4 of
  // a key is the IPv4 of the address it was made from, and 0 (the shared
  // bucket) for a real IPv6 peer.
  const uint8_t v4b[4] = {10, 0, 0, 9};
  const uint8_t* const v4s[] = {v4, v4b};
  for (const uint8_t* a : v4s) {
    CHECK(peer_key_from_addr(AddrFamily::IPV4, a, key), "keyed");
    CHECK(ipv4_host_order_from_addr(AddrFamily::IPV6, key) == ipv4_host_order_from_addr(AddrFamily::IPV4, a),
          "an IPv4 key's IPv4 is the address's");
  }
  CHECK(peer_key_from_addr(AddrFamily::IPV6, mapped, key) &&
            ipv4_host_order_from_addr(AddrFamily::IPV6, key) == ipv4_host_order_from_addr(AddrFamily::IPV6, mapped),
        "a mapped key's IPv4 is the address's");
  CHECK(ipv4_host_order_from_addr(AddrFamily::IPV6, key) == ip4(192, 168, 1, 37), "and it is 192.168.1.37");
  const uint8_t* const v6s[] = {global, link_local};
  for (const uint8_t* a : v6s) {
    CHECK(peer_key_from_addr(AddrFamily::IPV6, a, key) && ipv4_host_order_from_addr(AddrFamily::IPV6, key) == 0,
          "a real IPv6 key's IPv4 is 0, the shared bucket");
  }
}

// The whole AP test fed what the dual-stack socket really reports: before
// the v4-mapped unwrap every request reached request_on_softap as 0/0 and
// the AP unlock could never fire.
static void dual_stack_request_reaches_the_ap_test_intact() {
  const uint8_t peer[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 168, 4, 2};
  const uint8_t local_ap[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 168, 4, 1};
  const uint8_t local_sta[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 168, 1, 50};
  const uint32_t ap = ip4(192, 168, 4, 1);
  const uint32_t m24 = ip4(255, 255, 255, 0);
  CHECK(request_on_softap(ipv4_host_order_from_addr(AddrFamily::IPV6, peer),
                          ipv4_host_order_from_addr(AddrFamily::IPV6, local_ap),
                          ap, m24, 0, 0),
        "v4-mapped AP client on the AP address → true");
  CHECK(!request_on_softap(ipv4_host_order_from_addr(AddrFamily::IPV6, peer),
                           ipv4_host_order_from_addr(AddrFamily::IPV6, local_sta),
                           ap, m24, ip4(192, 168, 1, 50), m24),
        "v4-mapped request that arrived on the STA address → false");
  const uint8_t v6_peer[16] = {0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7};
  CHECK(!request_on_softap(ipv4_host_order_from_addr(AddrFamily::IPV6, v6_peer),
                           ipv4_host_order_from_addr(AddrFamily::IPV6, local_ap),
                           ap, m24, 0, 0),
        "a real IPv6 (link-local) peer stays not-provably-AP → false");
}

static void subnet_overlap_math() {
  const uint32_t m24 = ip4(255, 255, 255, 0);
  CHECK(subnets_overlap(ip4(10, 0, 0, 1), m24, ip4(10, 0, 0, 200), m24), "same /24 overlaps");
  CHECK(!subnets_overlap(ip4(10, 0, 0, 1), m24, ip4(10, 0, 1, 1), m24), "adjacent /24s do not");
  CHECK(subnets_overlap(ip4(10, 0, 0, 1), m24, ip4(10, 9, 9, 9), ip4(255, 0, 0, 0)),
        "a /8 containing the /24 overlaps");
  CHECK(mask_is_prefix(0xFFFFFFFFu) && mask_is_prefix(0) && mask_is_prefix(m24), "prefix masks");
  CHECK(!mask_is_prefix(ip4(255, 0, 255, 0)), "non-prefix mask");
}

// ── refusal body ────────────────────────────────────────────────────────────

static void refusal_body_carries_the_ttl_and_refuses_truncation() {
  char body[256];
  CHECK(build_gate_refusal_json(body, sizeof(body), kTtl), "256 bytes holds the body");
  CHECK(std::strstr(body, "\"error\":\"physical_confirmation_required\"") != nullptr,
        "error code present");
  CHECK(std::strstr(body, "\"gate_ttl_seconds\":30}") != nullptr, "ttl derived from the constant: %s", body);
  CHECK(std::strstr(body, "within 30 seconds") != nullptr, "button hint carries the same ttl");

  char tiny[64];
  CHECK(!build_gate_refusal_json(tiny, sizeof(tiny), kTtl), "a too-small buffer is refused");
  CHECK(tiny[0] == '\0', "refused output is emptied, never half-JSON");
  CHECK(!build_gate_refusal_json(nullptr, 10, kTtl), "null out refused");
  CHECK(!build_gate_refusal_json(body, 0, kTtl), "zero cap refused");
}


// ── the four-grant receipt decision: the claim ticket between the bearer
//    and the tap ────────────────────────────────────────────────────────────
//
// GET /api/provisioning-receipt?claim=<hex> on a WAP (and the flagship, in
// a follow-up): the claim a phone read over the Bluetooth setup door is the
// third grant. Same shape as receipt_decide above: the Host first, then the
// bearer, then the claim, then the tap — each hook called only when the one
// before it did not decide.

struct Hook {
  bool answer;
  int calls = 0;
  bool operator()() { ++calls; return answer; }
};

static void four_grant_foreign_host_calls_nothing() {
  Hook bearer{true}, claim{true}, tap{true};
  CHECK(receipt_decide(true, std::ref(bearer), std::ref(claim), std::ref(tap)) == ReceiptVerdict::REFUSE_HOST,
        "foreign Host refuses");
  CHECK(bearer.calls == 0 && claim.calls == 0 && tap.calls == 0, "nothing else consulted");
}

static void four_grant_bearer_before_claim_before_tap() {
  Hook bearer{true}, claim{true}, tap{true};
  CHECK(receipt_decide(false, std::ref(bearer), std::ref(claim), std::ref(tap)) == ReceiptVerdict::SERVE_BEARER,
        "a bearer serves first");
  CHECK(claim.calls == 0 && tap.calls == 0, "the claim is not spent and the tap is left for the owner");

  Hook bearer2{false}, claim2{true}, tap2{true};
  CHECK(receipt_decide(false, std::ref(bearer2), std::ref(claim2), std::ref(tap2)) == ReceiptVerdict::SERVE_CLAIM,
        "no bearer: a good claim serves");
  CHECK(claim2.calls == 1, "the claim was spent exactly once");
  CHECK(tap2.calls == 0, "and the BOOT tap is left unspent");
}

static void four_grant_spent_claim_falls_through_to_the_tap() {
  Hook bearer{false}, claim{false}, tap{true};
  CHECK(receipt_decide(false, std::ref(bearer), std::ref(claim), std::ref(tap)) == ReceiptVerdict::SERVE_TAP,
        "a wrong or spent claim: the tap still admits");
  CHECK(claim.calls == 1 && tap.calls == 1, "each asked once");

  Hook bearer2{false}, claim2{false}, tap2{false};
  CHECK(receipt_decide(false, std::ref(bearer2), std::ref(claim2), std::ref(tap2)) == ReceiptVerdict::REFUSE_NO_TAP,
        "nothing: refused, physical confirmation required");
}

static void four_grant_agrees_with_three_grant_when_no_claim_is_presented() {
  // A request with no ?claim= is wired as a claim hook that answers false
  // without spending anything; every verdict then matches the old overload.
  const bool fh[] = {false, true};
  const bool br[] = {false, true};
  const bool tp[] = {false, true};
  for (bool f : fh) for (bool b : br) for (bool t : tp) {
    Hook bearer{b}, tap{t};
    Hook bearer4{b}, none{false}, tap4{t};
    const ReceiptVerdict three = receipt_decide(f, std::ref(bearer), std::ref(tap));
    const ReceiptVerdict four = receipt_decide(f, std::ref(bearer4), std::ref(none), std::ref(tap4));
    CHECK(three == four, "f=%d b=%d t=%d: the overloads agree", f, b, t);
    CHECK(bearer.calls == bearer4.calls && tap.calls == tap4.calls, "and consult the same hooks");
  }
}

// ── The BOOT tap's one asker (sweep F342) ─────────────────────────────────

static void peek_reads_and_take_exact_takes_only_the_judged_stamp() {
  State s{0};
  CHECK(peek(s) == 0, "closed: peek reads 0");
  CHECK(!take_exact(s, 0), "0 is nothing to take");
  open(s, 5000);
  const uint32_t seen = peek(s);
  CHECK(seen == 5000, "peek reads the stamp, got %u", (unsigned)seen);
  CHECK(peek(s) == 5000, "and consumes nothing");
  CHECK(!take_exact(s, 4999), "a stamp that is not the one there is refused");
  CHECK(peek(s) == 5000, "and leaves the tap");
  // A re-tap between the peek and the take: the judged stamp is gone, the
  // new one is not consumed in its place.
  open(s, 6000);
  CHECK(!take_exact(s, seen), "a re-tap since the peek: refused");
  CHECK(peek(s) == 6000, "the new tap is untouched");
  // The BLE OTA break-glass take (or a close, or a sweep) between the two.
  const uint32_t seen2 = peek(s);
  CHECK(take(s, 6100, kTtl), "the other consumer takes it first");
  CHECK(!take_exact(s, seen2), "then the judged stamp is not there to take");
  open(s, 7000);
  const uint32_t seen3 = peek(s);
  close(s);
  CHECK(!take_exact(s, seen3), "closed since the peek: refused");
  open(s, 8000);
  CHECK(take_exact(s, peek(s)), "the judged stamp, still there: taken");
  CHECK(peek(s) == 0, "and the gate is closed");
  CHECK(!take_exact(s, 8000), "a second take of it is refused");
}

// A peer key as the WAP's request_peer_key() writes it for an IPv4 client of
// the dual-stack listener (::ffff:a.b.c.d).
struct Peer {
  uint8_t key[PEER_KEY_LEN];
};
static Peer v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  Peer p{};
  const uint8_t ip[4] = {a, b, c, d};
  peer_key_from_addr(AddrFamily::IPV4, ip, p.key);
  return p;
}

// The WAP's receipt step, as provisioning_gate_take_for() runs it: peek the
// stamp, judge the ask, take exactly the judged stamp on TAKE, close the tap
// on the ask that found the contention, and burn an expired stamp.
struct Bench {
  State gate{0};
  TapArbiter arb{};
  int served = 0;
  int closed_by = 0;   // how many asks closed a tap by contention
  TapVerdict ask(const Peer& p, uint32_t now) {
    const uint32_t seen = peek(gate);
    TapVerdict v = tap_arbitrate(arb, seen, p.key, now, kTtl);
    if (v == TapVerdict::TAKE) {
      if (take_exact(gate, seen)) ++served;
      else v = TapVerdict::NO_TAP;
    } else if (v == TapVerdict::CONTENDED) {
      if (seen != 0 && seen == arb.contended_tap && take_exact(gate, seen)) ++closed_by;
    } else if (seen != 0) {
      take_exact(gate, seen);   // NO_TAP with a stamp: an expired one, burned
    }
    return v;
  }
};

static const Peer kOwner = v4(192, 168, 1, 20);
static const Peer kOther = v4(192, 168, 1, 21);

// (1) A host polling every 100 ms and the owner's dashboard every 2 s, from
// before the tap to well after it: nothing is served, the tap is closed by
// the first ask after it, and every ask of the owner's inside the tap's
// window is CONTENDED (the page says another device asked).
static void a_poller_and_the_owner_across_one_tap_are_both_refused() {
  Bench b;
  const uint32_t T = 110050;
  bool tapped = false;
  int owner_contended = 0, owner_in_window = 0;
  for (uint32_t t = 100000; t < 100000 + 60000; t += 100) {
    if (!tapped && t >= T) { open(b.gate, T); tapped = true; }
    const TapVerdict vb = b.ask(kOther, t);
    CHECK(vb != TapVerdict::TAKE, "the poller was served at %u", (unsigned)t);
    if ((t - 100000) % 2000 == 500) {
      const TapVerdict va = b.ask(kOwner, t);
      CHECK(va != TapVerdict::TAKE, "the owner was served beside a poller at %u", (unsigned)t);
      if (t >= T && t < T + kTtl) {
        ++owner_in_window;
        if (va == TapVerdict::CONTENDED) ++owner_contended;
      }
    }
  }
  CHECK(b.served == 0, "nothing served, got %d", b.served);
  CHECK(b.closed_by == 1, "the tap was closed once, by the first ask after it (%d)", b.closed_by);
  CHECK(peek(b.gate) == 0, "and stays closed");
  CHECK(owner_in_window == 15 && owner_contended == owner_in_window,
        "every owner ask in the window CONTENDED (%d of %d)", owner_contended, owner_in_window);
}

// (2) The owner alone, asking every 2 s from before the tap: TAKE on its
// first ask after the tap, once, and NO_TAP after it.
static void a_sole_owner_is_served_on_its_first_ask_after_the_tap() {
  Bench b;
  const uint32_t T = 104100;
  int takes = 0;
  uint32_t first_take = 0;
  bool tapped = false;
  for (uint32_t t = 100000; t < 100000 + 40000; t += 2000) {
    if (!tapped && t >= T) { open(b.gate, T); tapped = true; }   // the tap, between two polls
    const TapVerdict v = b.ask(kOwner, t);
    if (v == TapVerdict::TAKE) {
      if (!takes) first_take = t;
      ++takes;
    } else {
      CHECK(v == TapVerdict::NO_TAP, "the sole owner was CONTENDED at %u", (unsigned)t);
    }
  }
  CHECK(takes == 1 && b.served == 1, "served once (%d)", takes);
  CHECK(first_take == 106000, "on its first ask after the tap, got %u", (unsigned)first_take);
}

// (3) The owner asks 1 s before the tap; an attacker asks first after it,
// at +5 s; the owner at +6 s. Both CONTENDED: the attacker cannot take the
// tap by asking first.
static void an_attacker_asking_first_loses_to_an_owner_who_asked_before() {
  Bench b;
  const uint32_t T = 200000;
  CHECK(b.ask(kOwner, T - 1000) == TapVerdict::NO_TAP, "before the tap: nothing to take");
  open(b.gate, T);
  CHECK(b.ask(kOther, T + 5000) == TapVerdict::CONTENDED, "the attacker, first after the tap: CONTENDED");
  CHECK(b.ask(kOwner, T + 6000) == TapVerdict::CONTENDED, "the owner after it: CONTENDED");
  CHECK(b.served == 0 && peek(b.gate) == 0, "nothing served, the tap closed");
  // The other order: the attacker asked before the tap (it polls), the owner
  // asks first after it. CONTENDED too.
  Bench c;
  c.ask(kOther, T - 3000);
  open(c.gate, T);
  CHECK(c.ask(kOwner, T + 500) == TapVerdict::CONTENDED, "an asker before the tap contends the owner");
  CHECK(c.served == 0, "nothing served");
}

// (3b) The owner's client as it really asks: Use BOOT Tap Once polls every
// 2 s for up to 60 s from the click, the companion's Save every 2 s for up
// to 30 s, so the press can land long after the first ask. Each ask by the
// address already on record refreshes its time, which is what keeps a
// polling owner inside the lookback: here the owner polls alone for 20 s
// (and 28 s), longer than TAP_LOOKBACK_MS, the tap lands 1 s after its last
// poll, and a host that times the tap from /api/logs asks first, 100 ms
// after it. The host is CONTENDED, the owner's next poll too, and nothing is
// served. An arbiter that kept the owner's FIRST ask time (no refresh) saw
// an owner silent for 21 s and served the host the token.
static void a_polling_owner_stays_on_record_past_the_lookback() {
  const uint32_t leads[] = {21000, 29000};   // the dashboard's click 21 s before the tap, the companion's 29 s
  for (const uint32_t lead : leads) {
    Bench b;
    const uint32_t click = 900000;
    const uint32_t T = click + lead;
    uint32_t t = click;
    for (; t < T; t += 2000) {
      CHECK(b.ask(kOwner, t) == TapVerdict::NO_TAP, "the owner's poll at click+%u, before the tap",
            (unsigned)(t - click));
    }
    CHECK(T - (t - 2000) == 1000, "the tap lands 1 s after the owner's last poll");
    CHECK(T - click > TAP_LOOKBACK_MS, "the first ask is older than the lookback");
    open(b.gate, T);
    CHECK(b.ask(kOther, T + 100) == TapVerdict::CONTENDED,
          "a host asking first, 100 ms after the tap, %u ms after the click: CONTENDED (an ask by the "
          "address on record no longer refreshes its time?)", (unsigned)lead);
    CHECK(b.ask(kOwner, t) == TapVerdict::CONTENDED, "the owner's next poll: CONTENDED");
    CHECK(b.served == 0, "nothing served, got %d", b.served);
    CHECK(b.closed_by == 1, "the tap closed once, by the host's ask (%d)", b.closed_by);
    CHECK(peek(b.gate) == 0, "and stays closed");
  }
}

// (4) An address that stopped asking more than the lookback before the tap
// does not contend; at the lookback exactly, it does.
static void an_address_silent_past_the_lookback_does_not_contend() {
  const uint32_t T = 300000;
  Bench b;
  b.ask(kOther, T - 15000);
  open(b.gate, T);
  CHECK(b.ask(kOwner, T + 500) == TapVerdict::TAKE, "silent 15 s before the tap: the owner is served");
  Bench edge;
  edge.ask(kOther, T - TAP_LOOKBACK_MS);
  open(edge.gate, T);
  CHECK(edge.ask(kOwner, T + 500) == TapVerdict::CONTENDED, "at the lookback exactly: CONTENDED");
  Bench past;
  past.ask(kOther, T - TAP_LOOKBACK_MS - 1);
  open(past.gate, T);
  CHECK(past.ask(kOwner, T + 500) == TapVerdict::TAKE, "one ms past it: served");
}

// (5) The residual, characterized: one address polling with no other asker
// is served. A tap no client of the owner's was asking for goes to the first
// asker, whoever it is.
static void a_lone_poller_is_served_the_residual() {
  Bench b;
  const uint32_t T = 400000;
  for (uint32_t t = T - 5000; t < T; t += 100) b.ask(kOther, t);
  open(b.gate, T);
  CHECK(b.ask(kOther, T + 100) == TapVerdict::TAKE, "a lone poller takes the tap");
  CHECK(b.served == 1, "served");
}

// (6) After a contended tap, the second address goes silent: a re-tap once
// it has been silent past the lookback serves the owner (contention does not
// carry into a clean next tap); a re-tap inside the lookback is CONTENDED.
static void a_clean_next_tap_serves_and_a_retap_inside_the_lookback_does_not() {
  Bench b;
  const uint32_t T = 500000;
  b.ask(kOwner, T - 1000);
  open(b.gate, T);
  CHECK(b.ask(kOther, T + 200) == TapVerdict::CONTENDED, "the contended tap");
  CHECK(b.ask(kOwner, T + 2000) == TapVerdict::CONTENDED, "inside its window");
  // A re-tap 4 s after B's ask (inside the lookback): CONTENDED, and closed.
  open(b.gate, T + 4200);
  CHECK(b.ask(kOwner, T + 4300) == TapVerdict::CONTENDED, "a re-tap inside the lookback");
  CHECK(peek(b.gate) == 0, "and that tap is closed too");
  // B silent since T+200. A re-tap at T+11 s (B silent 10.8 s before it):
  // served, inside the first tap's 30 s window as well.
  open(b.gate, T + 11000);
  CHECK(b.ask(kOwner, T + 11500) == TapVerdict::TAKE, "a clean re-tap serves the owner");
  CHECK(b.served == 1, "once");
  // Both addresses keep asking (each within the lookback of the other's
  // last ask): the 409 lasts to the closed tap's TTL, and the window running
  // out with no tap is NO_TAP, not CONTENDED.
  Bench w;
  w.ask(kOwner, T - 1000);
  open(w.gate, T);
  w.ask(kOther, T + 200);
  CHECK(w.ask(kOwner, T + 9000) == TapVerdict::CONTENDED, "the owner, B asked 8.8 s ago");
  CHECK(w.ask(kOther, T + 18000) == TapVerdict::CONTENDED, "B, the owner asked 9 s ago");
  CHECK(w.ask(kOwner, T + 27000) == TapVerdict::CONTENDED, "the owner, B asked 9 s ago");
  CHECK(w.ask(kOther, T + kTtl - 1) == TapVerdict::CONTENDED, "both still asking: CONTENDED to the TTL");
  CHECK(w.ask(kOwner, T + kTtl) == TapVerdict::NO_TAP, "and it ends with it");
  CHECK(w.arb.contended_tap == 0, "the record is cleared");
}

// (6b) The 409 ends when its cause ends: once the other address has been
// quiet for the lookback, an ask inside the closed tap's 30 s is NO_TAP (the
// page keeps asking) and the owner's next tap serves it. This is the flow the
// pages' "wait 10 seconds, then click Use BOOT Tap Once again, and only then
// short-tap BOOT" prescribes. Before, every ask was
// CONTENDED until the closed tap's TTL had run, so a retry 10 to 30 s after
// the tap stopped on its first answer, before the press.
static void the_409_ends_when_the_other_address_goes_quiet() {
  Bench b;
  const uint32_t T = 700000;
  CHECK(b.ask(kOwner, T - 1000) == TapVerdict::NO_TAP, "the owner's click, before the tap");
  open(b.gate, T);
  CHECK(b.ask(kOther, T + 100) == TapVerdict::CONTENDED, "B closes the tap");
  CHECK(b.ask(kOwner, T + 1000) == TapVerdict::CONTENDED, "the owner hears it (the page stops)");
  CHECK(b.ask(kOwner, T + 100 + TAP_LOOKBACK_MS) == TapVerdict::CONTENDED,
        "B's last ask exactly the lookback ago: still CONTENDED");
  CHECK(b.ask(kOwner, T + 100 + TAP_LOOKBACK_MS + 1) == TapVerdict::NO_TAP,
        "one ms later, B quiet past the lookback: NO_TAP, inside the closed tap's 30 s");
  CHECK(b.arb.contended_tap == 0, "the contention is cleared");
  // The bench row: B stopped, the owner waits, clicks (its first poll at
  // T+12 s is NO_TAP, not CONTENDED), keeps asking, and taps.
  Bench r;
  r.ask(kOwner, T - 1000);
  open(r.gate, T);
  r.ask(kOther, T + 100);
  r.ask(kOwner, T + 1000);
  CHECK(r.ask(kOwner, T + 12000) == TapVerdict::NO_TAP, "the retry's first poll is not a 409");
  CHECK(r.ask(kOwner, T + 14000) == TapVerdict::NO_TAP, "nor its second");
  open(r.gate, T + 15000);
  CHECK(r.ask(kOwner, T + 16000) == TapVerdict::TAKE, "and the next tap serves the owner");
  // The other side of the same rule: the 409 ends once an ask finds no
  // address other than the asker asking in the lookback, whoever asks. With
  // B still polling every 200 ms, B's own asks end it 10 s after the owner's
  // page stopped, so the owner's retry hears NO_TAP (403), not CONTENDED,
  // although B never stopped; the page keeps asking, and the next tap is
  // closed again (the poller residual). Nothing is served either way.
  Bench p;
  p.ask(kOwner, T - 1000);
  open(p.gate, T);
  TapVerdict owner_retry = TapVerdict::TAKE, owner_poll = TapVerdict::TAKE, owner_after_tap = TapVerdict::TAKE;
  for (uint32_t t = T + 100; t <= T + 20100; t += 100) {
    if (t == T + 15000) open(p.gate, t);
    if (t == T + 1000) CHECK(p.ask(kOwner, t) == TapVerdict::CONTENDED, "the owner hears the 409 (the page stops)");
    if (t == T + 12000) owner_retry = p.ask(kOwner, t);
    if (t == T + 14000) owner_poll = p.ask(kOwner, t);
    if (t == T + 16000) owner_after_tap = p.ask(kOwner, t);
    if ((t - T) % 200 == 100) {
      CHECK(p.ask(kOther, t) != TapVerdict::TAKE, "the poller was served at %u", (unsigned)t);
    }
  }
  CHECK(owner_retry == TapVerdict::NO_TAP, "the owner's retry, B still polling: NO_TAP, not CONTENDED");
  CHECK(owner_poll == TapVerdict::NO_TAP, "and its next poll");
  CHECK(owner_after_tap == TapVerdict::CONTENDED, "the next tap, B still polling, is closed again");
  CHECK(p.served == 0, "nothing served under the poller, got %d", p.served);
  CHECK(p.closed_by == 2, "both taps closed (%d)", p.closed_by);
  CHECK(r.served == 1, "once");
  // B's own asks hear the 409 while the owner keeps asking, and NO_TAP once
  // the owner has been quiet for the lookback.
  Bench o;
  o.ask(kOwner, T - 1000);
  open(o.gate, T);
  o.ask(kOther, T + 100);
  o.ask(kOwner, T + 2000);
  CHECK(o.ask(kOther, T + 12000) == TapVerdict::CONTENDED, "B, the owner asked 10 s ago");
  CHECK(o.ask(kOther, T + 12001) == TapVerdict::NO_TAP, "B, the owner quiet past the lookback");
}

// (6c) A tap served to one asker ends the earlier contention: the asks after
// it are NO_TAP, never "the tap was closed". Here B polls every 200 ms, the
// owner asked once after the closed tap and then re-tapped without asking
// first (the bare re-tap the 409's hint no longer advises): the re-tap goes
// to B, the residual (a tap no client of the owner's was asking for). The
// owner's next ask is NO_TAP. Before, it was CONTENDED for the rest of the
// first tap's 30 s, though the token had just been handed out.
static void a_served_retap_ends_the_contention() {
  Bench b;
  const uint32_t T = 800000;
  b.ask(kOwner, T - 1000);
  open(b.gate, T);
  bool retapped = false;
  TapVerdict b_at_retap = TapVerdict::NO_TAP;
  for (uint32_t t = T + 100; t <= T + 12700; t += 200) {
    if (t == T + 2100) CHECK(b.ask(kOwner, t) == TapVerdict::CONTENDED, "the owner's one ask: CONTENDED");
    if (!retapped && t >= T + 12500) { open(b.gate, T + 12500); retapped = true; }
    const TapVerdict v = b.ask(kOther, t);
    if (retapped && b_at_retap == TapVerdict::NO_TAP) b_at_retap = v;
  }
  CHECK(b_at_retap == TapVerdict::TAKE, "the bare re-tap went to the poller (the residual)");
  CHECK(b.served == 1, "served once");
  CHECK(b.arb.contended_tap == 0, "and no contention is left on record");
  CHECK(b.ask(kOwner, T + 13000) == TapVerdict::NO_TAP, "the owner's next ask: NO_TAP, not CONTENDED");
  // The 409's way instead: the owner's client asks first and keeps asking,
  // then the re-tap. With B still polling, every ask is CONTENDED and
  // nothing is served.
  // (d counts 100 ms steps from T-5 s: B asks on every odd one, 200 ms
  // apart; the owner every 2 s from T-1 s; the taps at T and T+12.5 s.)
  Bench h;
  const uint32_t start = T - 5000;
  for (uint32_t d = 0; d <= 25000; d += 100) {
    const uint32_t t = start + d;
    if (d == 5000 || d == 17500) open(h.gate, t);
    if (d >= 4000 && (d - 4000) % 2000 == 0) {
      CHECK(h.ask(kOwner, t) != TapVerdict::TAKE, "the owner was served beside a poller at %u", (unsigned)t);
    }
    if (d % 200 == 100) {
      CHECK(h.ask(kOther, t) != TapVerdict::TAKE, "the poller was served at %u", (unsigned)t);
    }
  }
  CHECK(h.served == 0, "following the hint under a poller serves nothing, got %d", h.served);
  CHECK(h.closed_by == 2, "both taps closed (%d)", h.closed_by);
}

// (7) The same address asking again and again never contends with itself.
static void the_same_address_never_contends() {
  Bench b;
  const uint32_t T = 600000;
  for (int i = 0; i < 100; ++i) b.ask(kOwner, T - 9000 + (uint32_t)i * 50);
  open(b.gate, T);
  CHECK(b.ask(kOwner, T + 10) == TapVerdict::TAKE, "a hundred asks from one address: served");
  CHECK(!b.arb.prior.has, "and no second address was ever recorded");
}

// (8) take_exact() inside the step: a stamp that moved between the peek and
// the take is never consumed unjudged.
static void a_stamp_that_moved_since_the_peek_is_not_taken() {
  State s{0};
  TapArbiter a{};
  open(s, 1000);
  const uint32_t seen = peek(s);
  CHECK(tap_arbitrate(a, seen, kOwner.key, 1100, kTtl) == TapVerdict::TAKE, "judged: TAKE");
  open(s, 1050);   // a re-tap lands before the take
  CHECK(!take_exact(s, seen), "the judged stamp is gone: not taken");
  CHECK(peek(s) == 1050, "and the re-tap is left for the next ask");
  CHECK(!take_exact(s, 0), "0 is never taken");
}

// The record ages on 32-bit millis() and is never swept: a silence of about
// 2^32 ms makes a stale ask read as recent, which can only refuse a tap.
static void a_stale_record_fails_closed_across_the_wrap() {
  Bench b;
  b.ask(kOther, 1000);
  const uint32_t T = (uint32_t)(1000ull + 0x100000000ull + 2000ull);   // 2^32 ms + 2 s after B's ask
  open(b.gate, T);
  CHECK(b.ask(kOwner, T + 100) == TapVerdict::CONTENDED,
        "a 49.7-day-old ask reads as recent: refused, never served");
  CHECK(b.served == 0, "nothing served");
  // A tap stamped across the wrap of millis() is judged on its age.
  Bench w;
  w.ask(kOwner, 0xFFFFF000u);
  open(w.gate, 0xFFFFFF00u);
  CHECK(w.ask(kOwner, 0x00000400u) == TapVerdict::TAKE, "a tap across the wrap serves the sole asker");
}

static void a_peer_key_prints_as_ipv4_or_eight_groups() {
  char out[PEER_KEY_TEXT_CAP];
  CHECK(peer_key_text(kOwner.key, out, sizeof(out)) == 12 && std::strcmp(out, "192.168.1.20") == 0,
        "mapped: %s", out);
  const Peer zero_tail = v4(10, 0, 0, 1);
  peer_key_text(zero_tail.key, out, sizeof(out));
  CHECK(std::strcmp(out, "10.0.0.1") == 0, "a zero octet: %s", out);
  const Peer wide = v4(255, 255, 255, 255);
  peer_key_text(wide.key, out, sizeof(out));
  CHECK(std::strcmp(out, "255.255.255.255") == 0, "the widest IPv4: %s", out);
  // An octet of exactly 100 keeps its hundreds digit (192.168.100.x is a
  // common home subnet).
  const Peer hundred = v4(192, 168, 100, 5);
  peer_key_text(hundred.key, out, sizeof(out));
  CHECK(std::strcmp(out, "192.168.100.5") == 0, "an octet of 100: %s", out);
  const uint8_t ll[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0x11, 0x22, 0xff, 0xfe, 0x33, 0x44, 0x55};
  CHECK(peer_key_text(ll, out, sizeof(out)) == 39, "IPv6: 39 characters");
  CHECK(std::strcmp(out, "fe80:0000:0000:0000:0211:22ff:fe33:4455") == 0, "IPv6: %s", out);
  // ff ff at bytes 10-11 behind a nonzero prefix (2001:db8::ffff:c0a8:114)
  // is a real IPv6 address, not ::ffff:192.168.1.20.
  const uint8_t ff_tail[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xc0, 0xa8, 0x01, 0x14};
  CHECK(peer_key_text(ff_tail, out, sizeof(out)) == 39, "a nonzero prefix: 39 characters");
  CHECK(std::strcmp(out, "2001:0db8:0000:0000:0000:ffff:c0a8:0114") == 0, "a nonzero prefix: %s", out);
  // One nonzero byte anywhere in the prefix is enough.
  uint8_t one_byte[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 1, 20};
  one_byte[9] = 1;
  peer_key_text(one_byte, out, sizeof(out));
  CHECK(std::strcmp(out, "0000:0000:0000:0000:0001:ffff:c0a8:0114") == 0, "byte 9 set: %s", out);
  // The mapped prefix is ff ff at bytes 10 AND 11: a zero prefix with ff 00
  // there (::ff00:c0a8:114) is a real IPv6 address.
  const uint8_t ff00[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0x00, 192, 168, 1, 20};
  CHECK(peer_key_text(ff00, out, sizeof(out)) == 39, "ff 00 at bytes 10-11: 39 characters");
  CHECK(std::strcmp(out, "0000:0000:0000:0000:0000:ff00:c0a8:0114") == 0, "ff 00 at bytes 10-11: %s", out);
  // And 00 ff there (::ff:c0a8:114): byte 10 is held as well as byte 11.
  const uint8_t ff_low[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0xff, 192, 168, 1, 20};
  CHECK(peer_key_text(ff_low, out, sizeof(out)) == 39, "00 ff at bytes 10-11: 39 characters");
  CHECK(std::strcmp(out, "0000:0000:0000:0000:0000:00ff:c0a8:0114") == 0, "00 ff at bytes 10-11: %s", out);
  char small[PEER_KEY_TEXT_CAP - 1];
  small[0] = 'x';
  CHECK(peer_key_text(kOwner.key, small, sizeof(small)) == 0 && small[0] == '\0',
        "a buffer short of the longest form: nothing written");
  CHECK(peer_key_text(nullptr, out, sizeof(out)) == 0 && out[0] == '\0', "a null key: empty");
  CHECK(peer_key_text(kOwner.key, nullptr, 40) == 0, "a null buffer: 0");
}

int main() {
  closed_by_default();
  open_then_peek_does_not_consume();
  one_tap_admits_exactly_one_consumer();
  gate_self_closes_after_ttl();
  millis_zero_is_a_real_tap();
  uint32_wraparound_is_not_expiry();
  explicit_close();
  expire_clears_at_the_ttl_and_never_a_fresh_stamp();
  a_swept_tap_does_not_revive_after_the_wrap();
  an_unswept_tap_reads_open_after_the_wrap();
  expire_leaves_a_stamp_reopened_since_it_was_read();
  the_flagship_sweeps_its_tap_every_pass();
  lan_with_no_grant_withholds();
  every_single_grant_injects();
  reason_pointer_is_optional();
  one_tap_unlocks_exactly_one_lan_page_load();
  receipt_first_leaves_nothing_for_a_page_load();
  standing_grants_never_spend_the_tap();
  no_tap_withholds_and_names_the_reason();
  foreign_host_receipt_is_refused_before_bearer_and_tap();
  receipt_order_on_a_device_host();
  receipt_truth_table();
  foreign_host_page_load_leaves_the_tap();
  foreign_host_page_truth_table();
  foreign_host_requests_through_the_whole_ttl_leave_the_tap();
  ap_subnet_match_is_conservative();
  request_on_softap_needs_the_ap_interface_and_no_overlap();
  v4_and_v4_mapped_addresses_unwrap();
  real_ipv6_and_unknown_families_read_as_zero();
  peer_key_names_one_peer_in_either_family();
  dual_stack_request_reaches_the_ap_test_intact();
  subnet_overlap_math();
  refusal_body_carries_the_ttl_and_refuses_truncation();
  four_grant_foreign_host_calls_nothing();
  four_grant_bearer_before_claim_before_tap();
  four_grant_spent_claim_falls_through_to_the_tap();
  four_grant_agrees_with_three_grant_when_no_claim_is_presented();
  peek_reads_and_take_exact_takes_only_the_judged_stamp();
  a_poller_and_the_owner_across_one_tap_are_both_refused();
  a_sole_owner_is_served_on_its_first_ask_after_the_tap();
  an_attacker_asking_first_loses_to_an_owner_who_asked_before();
  a_polling_owner_stays_on_record_past_the_lookback();
  an_address_silent_past_the_lookback_does_not_contend();
  a_lone_poller_is_served_the_residual();
  a_clean_next_tap_serves_and_a_retap_inside_the_lookback_does_not();
  the_409_ends_when_the_other_address_goes_quiet();
  a_served_retap_ends_the_contention();
  the_same_address_never_contends();
  a_stamp_that_moved_since_the_peek_is_not_taken();
  a_stale_record_fails_closed_across_the_wrap();
  a_peer_key_prints_as_ipv4_or_eight_groups();

  if (g_failures) {
    std::printf("test_provisioning_gate: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_provisioning_gate: all checks passed\n");
  return 0;
}
