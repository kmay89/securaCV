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
//   * ipv4_in_subnet / request_on_softap answer false for every "not provably
//     over the Canary's own Wi-Fi" input (wrong interface, overlapping home
//     subnet, unusable mask, AP down).

#include "../common/network/provisioning_gate.h"

#include <cstdio>
#include <cstring>

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

int main() {
  closed_by_default();
  open_then_peek_does_not_consume();
  one_tap_admits_exactly_one_consumer();
  gate_self_closes_after_ttl();
  millis_zero_is_a_real_tap();
  uint32_wraparound_is_not_expiry();
  explicit_close();
  lan_with_no_grant_withholds();
  every_single_grant_injects();
  reason_pointer_is_optional();
  ap_subnet_match_is_conservative();
  request_on_softap_needs_the_ap_interface_and_no_overlap();
  subnet_overlap_math();
  refusal_body_carries_the_ttl_and_refuses_truncation();

  if (g_failures) {
    std::printf("test_provisioning_gate: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_provisioning_gate: all checks passed\n");
  return 0;
}
