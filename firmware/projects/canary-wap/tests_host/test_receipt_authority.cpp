// Which address the provisioning receipt's base_url names (sweep F255),
// through the REAL receipt_authority.h that send_provisioning_receipt()
// builds the authority with, and the sketch's glue read from canary_wap.ino.
//
// Before, the sketch wrote WiFi.softAPIP() for the bearer and BOOT-tap grants
// and the .local name only for a spent setup-door claim (via_claim), so a
// receipt fetched over the home LAN while the STA was up named 192.168.4.1:
// nothing on the LAN reaches it, and it is gone once the SoftAP drops. Every
// grant is checked here against a request on the SoftAP, a request on the
// STA with the STA up, and the STA down, beside the old rule (spelled below
// as the old sketch decided it), and each case where the two differ is named.

#include "arduino_globals.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "receipt_authority.h"
#include "network/provisioning_gate.h"

#ifndef CANARY_WAP_INO
#error "CANARY_WAP_INO must name canary_wap.ino (the Makefile passes it)"
#endif

namespace {

int g_checks = 0;
int g_failures = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      ++g_failures;                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);            \
    }                                                                     \
  } while (0)

using receipt_authority::Choice;
using receipt_authority::Grant;
using receipt_authority::Inputs;

constexpr uint32_t ip(unsigned a, unsigned b, unsigned c, unsigned d) {
  return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | (uint32_t)d;
}

const uint32_t kSoftAp = ip(192, 168, 4, 1);
const uint32_t kSta    = ip(192, 168, 1, 37);
const char*    kHost   = "canary-kitchen";

// The three network states the brief names, plus the socket that cannot say.
enum class Net { ON_SOFTAP_STA_UP, ON_STA_STA_UP, STA_DOWN_ON_SOFTAP, UNREAD_STA_UP, UNREAD_STA_DOWN };

Inputs make(Net n, Grant g, const char* host = kHost) {
  Inputs in = {};
  in.softap_ip = kSoftAp;
  in.mdns_host = host;
  in.grant = g;
  switch (n) {
    case Net::ON_SOFTAP_STA_UP:   in.request_local = kSoftAp; in.sta_ip = kSta; break;
    case Net::ON_STA_STA_UP:      in.request_local = kSta;    in.sta_ip = kSta; break;
    case Net::STA_DOWN_ON_SOFTAP: in.request_local = kSoftAp; in.sta_ip = 0;    break;
    case Net::UNREAD_STA_UP:      in.request_local = 0;       in.sta_ip = kSta; break;
    case Net::UNREAD_STA_DOWN:    in.request_local = 0;       in.sta_ip = 0;    break;
  }
  return in;
}

std::string authority(const Inputs& in) {
  char out[80];
  const size_t n = receipt_authority::write(out, sizeof(out), in);
  CHECK(n == std::strlen(out));
  return out;
}

// The old send_provisioning_receipt()'s rule, verbatim in effect:
//   String authority = WiFi.softAPIP().toString();
//   if (via_claim && WiFi.isConnected() && g_device.mdns_hostname[0] != '\0')
//     authority = String(g_device.mdns_hostname) + ".local";
std::string old_authority(const Inputs& in) {
  const bool via_claim = in.grant == Grant::SETUP_DOOR_CLAIM;
  const bool connected = in.sta_ip != 0;
  if (via_claim && connected && in.mdns_host && in.mdns_host[0] != '\0') {
    return std::string(in.mdns_host) + ".local";
  }
  char d[16];
  std::snprintf(d, sizeof(d), "%u.%u.%u.%u", (unsigned)(in.softap_ip >> 24),
                (unsigned)((in.softap_ip >> 16) & 0xFF), (unsigned)((in.softap_ip >> 8) & 0xFF),
                (unsigned)(in.softap_ip & 0xFF));
  return d;
}

const std::string kLocal = "canary-kitchen.local";
const std::string kApStr = "192.168.4.1";

// ── every grant x every network state ─────────────────────────────────────

void test_bearer() {
  // A bearer re-sync over the home LAN (the iPhone's poll, a desktop tool):
  // the .local name, where the old rule said 192.168.4.1. CHANGED.
  Inputs lan = make(Net::ON_STA_STA_UP, Grant::BEARER);
  CHECK(receipt_authority::choose(lan) == Choice::MDNS_NAME);
  CHECK(authority(lan) == kLocal);
  CHECK(old_authority(lan) == kApStr);   // the bug: unreachable from the LAN
  CHECK(authority(lan) != old_authority(lan));

  // Over the SoftAP inside its grace window: that is where this reader is.
  Inputs ap = make(Net::ON_SOFTAP_STA_UP, Grant::BEARER);
  CHECK(receipt_authority::choose(ap) == Choice::SOFTAP_ADDRESS);
  CHECK(authority(ap) == kApStr);
  CHECK(authority(ap) == old_authority(ap));

  // The STA down: there is no home network to name.
  Inputs down = make(Net::STA_DOWN_ON_SOFTAP, Grant::BEARER);
  CHECK(receipt_authority::choose(down) == Choice::SOFTAP_ADDRESS);
  CHECK(authority(down) == kApStr);
  CHECK(authority(down) == old_authority(down));
}

void test_boot_tap() {
  // The dashboard's BOOT-tap sign-in over the home LAN (and the recovery kit
  // it now offers, F257): the .local name. CHANGED.
  Inputs lan = make(Net::ON_STA_STA_UP, Grant::BOOT_TAP);
  CHECK(receipt_authority::choose(lan) == Choice::MDNS_NAME);
  CHECK(authority(lan) == kLocal);
  CHECK(old_authority(lan) == kApStr);
  CHECK(authority(lan) != old_authority(lan));

  // The /companion wizard's close-out on the SoftAP, STA just joined.
  Inputs ap = make(Net::ON_SOFTAP_STA_UP, Grant::BOOT_TAP);
  CHECK(receipt_authority::choose(ap) == Choice::SOFTAP_ADDRESS);
  CHECK(authority(ap) == kApStr);
  CHECK(authority(ap) == old_authority(ap));

  // A standalone (AP-only) unit, or one whose home Wi-Fi is down.
  Inputs down = make(Net::STA_DOWN_ON_SOFTAP, Grant::BOOT_TAP);
  CHECK(receipt_authority::choose(down) == Choice::SOFTAP_ADDRESS);
  CHECK(authority(down) == kApStr);
  CHECK(authority(down) == old_authority(down));
}

void test_setup_door_claim() {
  // The phone that provisioned over Bluetooth spends its claim on the LAN:
  // the .local name, as before.
  Inputs lan = make(Net::ON_STA_STA_UP, Grant::SETUP_DOOR_CLAIM);
  CHECK(receipt_authority::choose(lan) == Choice::MDNS_NAME);
  CHECK(authority(lan) == kLocal);
  CHECK(authority(lan) == old_authority(lan));

  // A claim spent over the SoftAP keeps the claim's contract (magic_pairing.md
  // §3.5: never the SoftAP address): the .local name, as before.
  Inputs ap = make(Net::ON_SOFTAP_STA_UP, Grant::SETUP_DOOR_CLAIM);
  CHECK(receipt_authority::choose(ap) == Choice::MDNS_NAME);
  CHECK(authority(ap) == kLocal);
  CHECK(authority(ap) == old_authority(ap));

  // The STA down (the join was lost): the SoftAP address, as before.
  Inputs down = make(Net::STA_DOWN_ON_SOFTAP, Grant::SETUP_DOOR_CLAIM);
  CHECK(receipt_authority::choose(down) == Choice::SOFTAP_ADDRESS);
  CHECK(authority(down) == kApStr);
  CHECK(authority(down) == old_authority(down));
}

// ── the socket that cannot say ────────────────────────────────────────────

void test_unread_socket_fallback() {
  // getsockname() failed (request_local == 0) with the STA up: the .local
  // name for every grant — what a LAN reader needs, and the durable answer
  // while the SoftAP is in its grace window or gone.
  for (Grant g : {Grant::BEARER, Grant::BOOT_TAP, Grant::SETUP_DOOR_CLAIM}) {
    Inputs in = make(Net::UNREAD_STA_UP, g);
    CHECK(receipt_authority::choose(in) == Choice::MDNS_NAME);
    CHECK(authority(in) == kLocal);
  }
  // ... and with the STA down, the SoftAP address.
  for (Grant g : {Grant::BEARER, Grant::BOOT_TAP, Grant::SETUP_DOOR_CLAIM}) {
    Inputs in = make(Net::UNREAD_STA_DOWN, g);
    CHECK(receipt_authority::choose(in) == Choice::SOFTAP_ADDRESS);
    CHECK(authority(in) == kApStr);
  }
}

// ── edges ─────────────────────────────────────────────────────────────────

void test_edges() {
  // No mDNS name (generate_mdns_hostname always gives one; this is the
  // guard): a LAN reader gets the STA's own address, never the SoftAP's.
  for (const char* none : {(const char*)nullptr, ""}) {
    Inputs in = make(Net::ON_STA_STA_UP, Grant::BEARER, none);
    CHECK(receipt_authority::choose(in) == Choice::STA_ADDRESS);
    CHECK(authority(in) == "192.168.1.37");
    Inputs claim = make(Net::ON_SOFTAP_STA_UP, Grant::SETUP_DOOR_CLAIM, none);
    CHECK(authority(claim) == "192.168.1.37");
    Inputs ap = make(Net::ON_SOFTAP_STA_UP, Grant::BOOT_TAP, none);
    CHECK(authority(ap) == kApStr);
  }

  // A home router that handed the STA the SoftAP's own address: "arrived on
  // the SoftAP" cannot be told, and the name reaches it either way.
  Inputs same = make(Net::ON_SOFTAP_STA_UP, Grant::BOOT_TAP);
  same.sta_ip = kSoftAp;
  CHECK(!receipt_authority::arrived_on_softap(same));
  CHECK(authority(same) == kLocal);

  // The SoftAP already dropped (wifi_drop_ap()): softAPIP() still reads the
  // AP netif's configured 192.168.4.1 on the cores, and a LAN request does
  // not match it.
  Inputs dropped = make(Net::ON_STA_STA_UP, Grant::BOOT_TAP);
  CHECK(authority(dropped) == kLocal);

  // The longest label generate_mdns_hostname can make (canary- + 32 bytes,
  // inside the 40-byte mdns_hostname) fits the sketch's 80-byte buffer.
  std::string longest = "canary-" + std::string(32, 'a');
  Inputs big = make(Net::ON_STA_STA_UP, Grant::BEARER, longest.c_str());
  CHECK(authority(big) == longest + ".local");

  // A buffer too small for the name is not cut: the SoftAP address (what
  // every receipt named before), and nothing at all when even that cannot fit.
  char small[16];
  CHECK(receipt_authority::write(small, sizeof(small), make(Net::ON_STA_STA_UP, Grant::BEARER)) ==
        kApStr.size());
  CHECK(std::string(small) == kApStr);
  char tiny[8];
  std::memset(tiny, 'x', sizeof(tiny));
  CHECK(receipt_authority::write(tiny, sizeof(tiny), make(Net::ON_STA_STA_UP, Grant::BEARER)) == 0);
  CHECK(tiny[0] == '\0');
  CHECK(receipt_authority::write(nullptr, 80, make(Net::ON_STA_STA_UP, Grant::BEARER)) == 0);
}

// ── the socket address mapping, held to provisioning_gate.h's ─────────────

void test_ipv4_from_addr() {
  using RA = receipt_authority::AddrFamily;
  using PG = canary::net::provisioning_gate::AddrFamily;
  const uint8_t v4[4] = {192, 168, 1, 37};
  const uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 168, 4, 1};
  const uint8_t compat[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 192, 168, 4, 1};
  const uint8_t link_local[16] = {0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 168, 4, 1};
  const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV4, v4) == kSta);
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV6, mapped) == kSoftAp);
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV6, compat) == 0);
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV6, link_local) == 0);
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV6, loopback) == 0);
  CHECK(receipt_authority::ipv4_from_addr(RA::OTHER, v4) == 0);
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV4, nullptr) == 0);
  // The same answers as the flagship's helper, vector for vector.
  const uint8_t* six[] = {mapped, compat, link_local, loopback};
  for (const uint8_t* b : six) {
    CHECK(receipt_authority::ipv4_from_addr(RA::IPV6, b) ==
          canary::net::provisioning_gate::ipv4_host_order_from_addr(PG::IPV6, b));
  }
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV4, v4) ==
        canary::net::provisioning_gate::ipv4_host_order_from_addr(PG::IPV4, v4));
  // An IPv4-mapped SoftAP request is "on the SoftAP"; a mapped LAN one is not.
  Inputs in = make(Net::ON_STA_STA_UP, Grant::BOOT_TAP);
  in.request_local = receipt_authority::ipv4_from_addr(RA::IPV6, mapped);
  CHECK(receipt_authority::arrived_on_softap(in));
  CHECK(authority(in) == kApStr);
}

// ── the sketch's glue (canary_wap.ino is not compiled here) ───────────────

std::string read_file(const char* path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string body_of(const std::string& src, const std::string& head) {
  const size_t at = src.find(head);
  if (at == std::string::npos) return "";
  const size_t end = src.find("\n}\n", at);
  return src.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

bool has(const std::string& s, const char* re) { return std::regex_search(s, std::regex(re)); }

void test_sketch_glue() {
  const std::string ino = read_file(CANARY_WAP_INO);
  CHECK(!ino.empty());
  CHECK(ino.find("#include \"receipt_authority.h\"") != std::string::npos);

  const std::string send = body_of(ino, "static esp_err_t send_provisioning_receipt(httpd_req_t* req, "
                                        "receipt_authority::Grant grant) {");
  CHECK(!send.empty());
  // Every input from where the device keeps it, and the authority from the header.
  CHECK(has(send, R"(reach\.request_local\s*=\s*request_local_ipv4\(req\);)"));
  CHECK(has(send, R"(reach\.softap_ip\s*=\s*ipv4_host_order\(WiFi\.softAPIP\(\)\);)"));
  CHECK(has(send, R"(reach\.sta_ip\s*=\s*WiFi\.isConnected\(\)\s*\?\s*ipv4_host_order\(WiFi\.localIP\(\)\)\s*:\s*0;)"));
  CHECK(has(send, R"(reach\.mdns_host\s*=\s*g_device\.mdns_hostname;)"));
  CHECK(has(send, R"(reach\.grant\s*=\s*grant;)"));
  CHECK(has(send, R"(receipt_authority::write\(authority,\s*sizeof\(authority\),\s*reach\);)"));
  CHECK(has(send, R"(in\.ap_ip\s*=\s*authority;)"));
  // No second spelling of the address beside the header's.
  CHECK(!has(send, R"(softAPIP\(\)\.toString\(\))"));
  CHECK(!has(send, R"(\.local")"));

  // The socket's LOCAL address, both families, a failed read as 0.
  const std::string sock = body_of(ino, "static uint32_t request_local_ipv4(httpd_req_t* req) {");
  CHECK(!sock.empty());
  CHECK(has(sock, R"(httpd_req_to_sockfd\(req\))"));
  CHECK(has(sock, R"(if \(fd < 0\) return 0;)"));
  CHECK(has(sock, R"(getsockname\(fd,)"));
  CHECK(!has(sock, R"(getpeername)"));
  CHECK(has(sock, R"(!= 0\) return 0;)"));
  CHECK(has(sock, R"(AddrFamily::IPV4)"));
  CHECK(has(sock, R"(AddrFamily::IPV6)"));

  // Each grant passes its own name: a bearer, a served claim, else the tap.
  const std::string route = body_of(ino, "static esp_err_t handle_provisioning_receipt(httpd_req_t* req) {");
  CHECK(!route.empty());
  CHECK(has(route, R"(api_auth_check_optional\(req, g_device\.api_token_str\)\) \{\s*return send_provisioning_receipt\(req, receipt_authority::Grant::BEARER\);)"));
  CHECK(has(route, R"(send_provisioning_receipt\(\s*req, claim_served \? receipt_authority::Grant::SETUP_DOOR_CLAIM\s*: receipt_authority::Grant::BOOT_TAP\);)"));
  // Exactly the two calls.
  size_t calls = 0;
  for (size_t at = ino.find("send_provisioning_receipt("); at != std::string::npos;
       at = ino.find("send_provisioning_receipt(", at + 1)) {
    ++calls;
  }
  CHECK(calls == 3);   // the definition and the two calls
}

}  // namespace

int main() {
  test_bearer();
  test_boot_tap();
  test_setup_door_claim();
  test_unread_socket_fallback();
  test_edges();
  test_ipv4_from_addr();
  test_sketch_glue();
  if (g_failures) {
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d RECEIPT AUTHORITY CHECKS PASSED\n", g_checks);
  return 0;
}
