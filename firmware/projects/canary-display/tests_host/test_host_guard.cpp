// Host test for the Host-header guard (include/canary/net/host_guard.h): the
// glass's web API refuses writes, the CSRF token and the per-witness reads
// unless the Host a request targeted can only mean this device on this
// network. Pins the rule that defeats DNS rebinding — a public domain
// re-pointed at the device's LAN IP arrives with Origin == Host and would
// otherwise pass the same-site check — while keeping every way a household
// reaches the glass (the .local name, the raw IP, a private-suffix alias).
// No Arduino, no board.
//
// Prints "ALL HOST GUARD TESTS PASSED" on success (a CI grep makes a silent
// pass impossible to fake). Build (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//     firmware/projects/canary-display/tests_host/test_host_guard.cpp -o t && ./t

#include "canary/net/host_guard.h"

#include <cstdio>
#include <cstring>

using namespace canary::net;

static int g_fail = 0;

#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                      \
    }                                                                \
  } while (0)

// ── How the household reaches the glass: all of these are this device ────
static void test_own_names_and_addresses_pass() {
  CHECK(host_names_this_device("canary-watch-001-a1b2c3.local"),
        "the mDNS name the glass registers");
  CHECK(host_names_this_device("canary-watch-001-a1b2c3.local:80"),
        "the mDNS name with an explicit port");
  CHECK(host_names_this_device("Canary-Watch-001-A1B2C3.LOCAL"),
        "case-insensitive, like DNS");
  CHECK(host_names_this_device("canary-watch-001-a1b2c3"),
        "the bare single-label name (LAN resolver only)");
  CHECK(host_names_this_device("192.168.4.1"), "the AP-mode address");
  CHECK(host_names_this_device("10.0.0.23:8080"), "an IPv4 literal with port");
  CHECK(host_names_this_device("[fe80::1]"), "a bracketed IPv6 literal");
  CHECK(host_names_this_device("[fe80::1]:80"),
        "a bracketed IPv6 literal with port");
  CHECK(host_names_this_device("fd00::a1"), "a bare IPv6 literal");
  CHECK(host_names_this_device("glass.lan"), "a router alias under .lan");
  CHECK(host_names_this_device("glass.home.arpa"),
        "a router alias under .home.arpa");
  CHECK(host_names_this_device("glass.internal"),
        "a router alias under .internal");
  CHECK(host_names_this_device("  canary.local  "),
        "surrounding whitespace is not the name");
}

// ── What a rebinding page arrives with: a public, registrable domain ─────
static void test_public_domains_are_foreign() {
  CHECK(!host_names_this_device("evil.example"), "a public domain");
  CHECK(!host_names_this_device("evil.example:80"),
        "a public domain with the glass's port");
  CHECK(!host_names_this_device("canary.local.evil.example"),
        "our name as a subdomain of theirs");
  CHECK(!host_names_this_device("192.168.1.5.evil.example"),
        "our address as a subdomain of theirs");
  CHECK(!host_names_this_device("evil.example."),
        "a trailing-dot FQDN of a public domain");
  CHECK(!host_names_this_device("glass.localhost.example"),
        "a private-looking label that is not the suffix");
  CHECK(!host_names_this_device("evil.lan.example"),
        ".lan in the middle is not the .lan suffix");
}

// ── Degenerate authorities are foreign, never a free pass ───────────────
static void test_degenerate_hosts_are_foreign() {
  CHECK(!host_names_this_device(nullptr), "no Host at all");
  CHECK(!host_names_this_device(""), "an empty Host");
  CHECK(!host_names_this_device("   "), "a blank Host");
  CHECK(!host_names_this_device(":80"), "a port with no name");
  CHECK(!host_names_this_device("[fe80::1"), "an unclosed bracket");
  CHECK(!host_names_this_device(".local"), "the bare suffix is not a name");
  CHECK(!host_names_this_device("256.1.1.1"), "not an IPv4 literal");
  char huge[200];
  memset(huge, 'a', sizeof(huge) - 1);
  huge[sizeof(huge) - 1] = '\0';
  CHECK(!host_names_this_device(huge),
        "an authority longer than the guard's buffer is foreign, not truncated");
}

// ── The literal classifiers themselves ────────────────────────────────────
static void test_literal_classifiers() {
  CHECK(host_is_ipv4_literal("0.0.0.0"), "ipv4 min");
  CHECK(host_is_ipv4_literal("255.255.255.255"), "ipv4 max");
  CHECK(!host_is_ipv4_literal("1.2.3"), "three octets");
  CHECK(!host_is_ipv4_literal("1.2.3.4.5"), "five octets");
  CHECK(!host_is_ipv4_literal("1..3.4"), "empty octet");
  CHECK(!host_is_ipv4_literal("1.2.3.4."), "trailing dot");
  CHECK(!host_is_ipv4_literal("a.b.c.d"), "letters");
  CHECK(host_is_ipv6_literal("::1"), "loopback v6");
  CHECK(host_is_ipv6_literal("::ffff:192.168.1.5"), "v4-mapped v6");
  CHECK(!host_is_ipv6_literal("fe80"), "no colons");
  CHECK(!host_is_ipv6_literal("host:80"), "one colon is host:port");
  CHECK(!host_is_ipv6_literal("fe80::g1"), "non-hex");

  char out[16];
  CHECK(host_authority_name("Example.LOCAL:80", out, sizeof(out)) == 13 &&
            strcmp(out, "example.local") == 0,
        "lowercased, port stripped");
  CHECK(host_authority_name("[fe80::1]:80", out, sizeof(out)) == 7 &&
            strcmp(out, "fe80::1") == 0,
        "brackets and port stripped");
  CHECK(host_authority_name("fe80::1", out, sizeof(out)) == 7 &&
            strcmp(out, "fe80::1") == 0,
        "a bare v6 literal keeps every colon");
  CHECK(host_authority_name("0123456789abcdef", out, sizeof(out)) == 0,
        "a name that does not fit is not truncated into a different name");
}

int main() {
  test_own_names_and_addresses_pass();
  test_public_domains_are_foreign();
  test_degenerate_hosts_are_foreign();
  test_literal_classifiers();
  if (g_fail) {
    std::printf("%d HOST GUARD CHECK(S) FAILED\n", g_fail);
    return 1;
  }
  std::printf("ALL HOST GUARD TESTS PASSED\n");
  return 0;
}
