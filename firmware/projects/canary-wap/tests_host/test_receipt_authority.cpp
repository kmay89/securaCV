// Which address names the WAP to a caller: the provisioning receipt's
// base_url (sweep F255) and port 80's redirect to https, through the REAL
// receipt_authority.h that send_provisioning_receipt() and
// handle_https_redirect() build the authority with, and the sketch's glue
// read from canary_wap.ino (its comments left out).
//
// Before, the sketch wrote WiFi.softAPIP() for the bearer and BOOT-tap grants
// and the .local name only for a spent setup-door claim (via_claim), so a
// receipt fetched over the home LAN while the STA was up named 192.168.4.1:
// nothing on the LAN reaches it, and it is gone once the SoftAP drops. The
// redirect wrote WiFi.softAPIP() for every caller. Every grant is checked
// here against a request on the SoftAP, a request on the STA with the STA
// up, and the STA down, beside the old rule (spelled below as the old sketch
// decided it), and each case where the two differ is named; and again with
// the SoftAP dropped, an installed WAP's steady state, where core 3.x's
// WiFi.softAPIP() reads 0.0.0.0 (APClass::onDisable releases the AP netif and
// NetworkInterface::localIP() answers IPAddress() for a null one).
//
// Since 2026-10-19 (the post-#1802 verification): the redirect keeps the
// request's Host in front of that choice when it is an IP literal or one of
// the device's own names (receipt_authority::redirect_host(); #1804 kept any
// Host in the alphabet, #1802's merge kept none, so a browser that typed the
// device's IP was sent to a .local name it could not resolve), its 256-byte
// Location goes to the root rather than cut a URI (redirect_location()), and
// a setup-door claim answers the .local name whenever the device has one,
// the STA down included (#1804's rule, which #1802 had narrowed to the STA
// holding an address; old_authority() below models #1804's).

#include "arduino_globals.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
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

// The three network states the brief names, the socket that cannot say, and
// the SoftAP dropped (wifi_drop_ap(): softAPIP() reads 0.0.0.0 on core 3.x).
enum class Net {
  ON_SOFTAP_STA_UP, ON_STA_STA_UP, STA_DOWN_ON_SOFTAP, UNREAD_STA_UP, UNREAD_STA_DOWN,
  AP_DROPPED_ON_STA, AP_DROPPED_UNREAD, AP_DROPPED_STA_DOWN
};

Inputs make(Net n, Grant g, const char* host = kHost) {
  Inputs in = {};
  in.softap_ip = kSoftAp;
  in.mdns_host = host;
  in.grant = g;
  switch (n) {
    case Net::ON_SOFTAP_STA_UP:    in.request_local = kSoftAp; in.sta_ip = kSta; break;
    case Net::ON_STA_STA_UP:       in.request_local = kSta;    in.sta_ip = kSta; break;
    case Net::STA_DOWN_ON_SOFTAP:  in.request_local = kSoftAp; in.sta_ip = 0;    break;
    case Net::UNREAD_STA_UP:       in.request_local = 0;       in.sta_ip = kSta; break;
    case Net::UNREAD_STA_DOWN:     in.request_local = 0;       in.sta_ip = 0;    break;
    case Net::AP_DROPPED_ON_STA:   in.softap_ip = 0; in.request_local = kSta; in.sta_ip = kSta; break;
    case Net::AP_DROPPED_UNREAD:   in.softap_ip = 0; in.request_local = 0;    in.sta_ip = kSta; break;
    case Net::AP_DROPPED_STA_DOWN: in.softap_ip = 0; in.request_local = 0;    in.sta_ip = 0;    break;
  }
  return in;
}

const Grant kAllGrants[] = {Grant::BEARER, Grant::BOOT_TAP, Grant::SETUP_DOOR_CLAIM, Grant::HTTPS_REDIRECT};

std::string authority(const Inputs& in) {
  char out[80];
  const size_t n = receipt_authority::write(out, sizeof(out), in);
  CHECK(n == std::strlen(out));
  return out;
}

// The old send_provisioning_receipt()'s rule (#1804's, main's until F255),
// verbatim in effect:
//   String authority = WiFi.softAPIP().toString();
//   if (via_claim && g_device.mdns_hostname[0] != '\0')
//     authority = String(g_device.mdns_hostname) + ".local";
// (#1780's first cut asked WiFi.isConnected() too; #1804 dropped that, so the
// claim's receipt spells the steady-state address whatever the STA is doing,
// and F255's first cut modeled #1780's rule here and asserted the SoftAP
// address for a claim with the STA down "as before".) handle_https_redirect()
// before #1804 wrote WiFi.softAPIP().toString() for every caller: the same
// rule with no claim (#1804's Host rule, kept in front of this choice since
// 2026-10-19, is test_redirect_host()'s).
std::string old_authority(const Inputs& in) {
  const bool via_claim = in.grant == Grant::SETUP_DOOR_CLAIM;
  if (via_claim && in.mdns_host && in.mdns_host[0] != '\0') {
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
const std::string kStaStr = "192.168.1.37";

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

  // The STA down (the join lost in the same moment): still the .local name.
  // The claim receipt is the one the phone keeps for the life of the
  // pairing, so it spells the steady-state address, never the SoftAP's,
  // which is dead once the AP drops (#1804's rule, as before; F255's first
  // cut named 192.168.4.1 here, and the post-#1802 verification put the
  // contract back).
  Inputs down = make(Net::STA_DOWN_ON_SOFTAP, Grant::SETUP_DOOR_CLAIM);
  CHECK(receipt_authority::choose(down) == Choice::MDNS_NAME);
  CHECK(authority(down) == kLocal);
  CHECK(authority(down) == old_authority(down));

  // Every network state, then: the .local name, as #1804 answered it.
  for (Net n : {Net::ON_SOFTAP_STA_UP, Net::ON_STA_STA_UP, Net::STA_DOWN_ON_SOFTAP, Net::UNREAD_STA_UP,
                Net::UNREAD_STA_DOWN, Net::AP_DROPPED_ON_STA, Net::AP_DROPPED_UNREAD,
                Net::AP_DROPPED_STA_DOWN}) {
    Inputs in = make(n, Grant::SETUP_DOOR_CLAIM);
    CHECK(receipt_authority::choose(in) == Choice::MDNS_NAME);
    CHECK(authority(in) == kLocal);
    CHECK(authority(in) == old_authority(in));
  }
  // ... and with no name, like any other grant: the SoftAP address with the
  // STA down, the STA's own address on the STA (never the SoftAP's, even
  // when it arrived there), nothing with neither.
  for (const char* none : {(const char*)nullptr, ""}) {
    CHECK(receipt_authority::choose(make(Net::STA_DOWN_ON_SOFTAP, Grant::SETUP_DOOR_CLAIM, none)) ==
          Choice::SOFTAP_ADDRESS);
    CHECK(receipt_authority::choose(make(Net::ON_STA_STA_UP, Grant::SETUP_DOOR_CLAIM, none)) ==
          Choice::STA_ADDRESS);
    CHECK(receipt_authority::choose(make(Net::ON_SOFTAP_STA_UP, Grant::SETUP_DOOR_CLAIM, none)) ==
          Choice::STA_ADDRESS);
    CHECK(receipt_authority::choose(make(Net::AP_DROPPED_STA_DOWN, Grant::SETUP_DOOR_CLAIM, none)) ==
          Choice::NONE);
  }
}

void test_https_redirect() {
  // Port 80's redirect on a TLS unit, typed on the home network
  // (http://canary-kitchen.local/settings): https at the .local name, where
  // the old redirect said 192.168.4.1. CHANGED.
  Inputs lan = make(Net::ON_STA_STA_UP, Grant::HTTPS_REDIRECT);
  CHECK(receipt_authority::choose(lan) == Choice::MDNS_NAME);
  CHECK(authority(lan) == kLocal);
  CHECK(old_authority(lan) == kApStr);
  CHECK(authority(lan) != old_authority(lan));

  // The setup network's captive browser inside the grace window: it stays
  // on the SoftAP address it reached.
  Inputs ap = make(Net::ON_SOFTAP_STA_UP, Grant::HTTPS_REDIRECT);
  CHECK(receipt_authority::choose(ap) == Choice::SOFTAP_ADDRESS);
  CHECK(authority(ap) == kApStr);
  CHECK(authority(ap) == old_authority(ap));

  // The STA down: the SoftAP address, as before.
  Inputs down = make(Net::STA_DOWN_ON_SOFTAP, Grant::HTTPS_REDIRECT);
  CHECK(receipt_authority::choose(down) == Choice::SOFTAP_ADDRESS);
  CHECK(authority(down) == kApStr);
  CHECK(authority(down) == old_authority(down));
}

// ── the SoftAP dropped: an installed WAP's steady state ───────────────────

void test_ap_dropped() {
  // wifi_drop_ap() ran after the grace window; on core 3.x softAPIP() now
  // reads 0.0.0.0. A request on the STA, and one whose socket could not say,
  // gets the .local name for every grant, never "0.0.0.0" (what the old rule
  // wrote for a bearer, a BOOT tap and the redirect: CHANGED). A socket that
  // could not say (0) does not "arrive on" a SoftAP address that reads 0.
  for (Grant g : kAllGrants) {
    for (Net n : {Net::AP_DROPPED_ON_STA, Net::AP_DROPPED_UNREAD}) {
      Inputs in = make(n, g);
      CHECK(!receipt_authority::arrived_on_softap(in));
      CHECK(receipt_authority::choose(in) == Choice::MDNS_NAME);
      CHECK(authority(in) == kLocal);
      if (g == Grant::SETUP_DOOR_CLAIM) {
        CHECK(authority(in) == old_authority(in));   // the claim always named .local here
      } else {
        CHECK(old_authority(in) == "0.0.0.0");       // the bug: names nothing anywhere
      }
    }
  }
  // The AP dropped and the join lost at the same moment (a request accepted
  // on the STA, answered after it went; wifi_raise_ap() is about to bring
  // the AP back): the .local name, the one the device answers to on
  // whichever network returns. The old rule wrote "0.0.0.0" for every grant
  // but the claim, whose .local answer #1804 already had.
  for (Grant g : kAllGrants) {
    Inputs in = make(Net::AP_DROPPED_STA_DOWN, g);
    CHECK(receipt_authority::choose(in) == Choice::MDNS_NAME);
    CHECK(authority(in) == kLocal);
    if (g == Grant::SETUP_DOOR_CLAIM) {
      CHECK(old_authority(in) == kLocal);
    } else {
      CHECK(old_authority(in) == "0.0.0.0");
    }
  }
  // ... and with no name either there is nothing to give: nothing is
  // written, and the sketch refuses (503) rather than name 0.0.0.0.
  for (const char* none : {(const char*)nullptr, ""}) {
    for (Grant g : kAllGrants) {
      Inputs in = make(Net::AP_DROPPED_STA_DOWN, g, none);
      CHECK(receipt_authority::choose(in) == Choice::NONE);
      char out[80];
      std::memset(out, 'x', sizeof(out));
      CHECK(receipt_authority::write(out, sizeof(out), in) == 0);
      CHECK(out[0] == '\0');
    }
    // No name, the AP dropped, the STA up: the STA's own address.
    Inputs sta = make(Net::AP_DROPPED_ON_STA, Grant::BOOT_TAP, none);
    CHECK(receipt_authority::choose(sta) == Choice::STA_ADDRESS);
    CHECK(authority(sta) == kStaStr);
  }
}

// ── the socket that cannot say ────────────────────────────────────────────

void test_unread_socket_fallback() {
  // getsockname() failed (request_local == 0) with the STA up: the .local
  // name for every grant — what a LAN reader needs, and the durable answer
  // while the SoftAP is in its grace window or gone.
  for (Grant g : kAllGrants) {
    Inputs in = make(Net::UNREAD_STA_UP, g);
    CHECK(receipt_authority::choose(in) == Choice::MDNS_NAME);
    CHECK(authority(in) == kLocal);
  }
  // ... and with the STA down, the SoftAP address (the claim: its .local
  // name, rule 0, as in every state).
  for (Grant g : kAllGrants) {
    Inputs in = make(Net::UNREAD_STA_DOWN, g);
    if (g == Grant::SETUP_DOOR_CLAIM) {
      CHECK(receipt_authority::choose(in) == Choice::MDNS_NAME);
      CHECK(authority(in) == kLocal);
    } else {
      CHECK(receipt_authority::choose(in) == Choice::SOFTAP_ADDRESS);
      CHECK(authority(in) == kApStr);
    }
  }
}

// ── port 80's redirect: the Host the request carried ──────────────────────

std::string kept_host(const char* host, const Inputs& in, size_t cap = 80) {
  char out[80];
  std::memset(out, 'x', sizeof(out));
  const size_t n = receipt_authority::redirect_host(out, cap, host, in);
  CHECK(n == std::strlen(out));
  return out;
}

void test_redirect_host() {
  const Inputs lan = make(Net::ON_STA_STA_UP, Grant::HTTPS_REDIRECT);
  const Inputs ap  = make(Net::STA_DOWN_ON_SOFTAP, Grant::HTTPS_REDIRECT);

  // A typed IP is kept (a PC or phone with no mDNS reached the device at it:
  // Windows, Android without Bonjour), its port dropped: the STA's address,
  // the SoftAP's, any IPv4 literal the request reached the device at (a port
  // forward's), an IPv6 literal in brackets with its port after the bracket.
  CHECK(kept_host("192.168.1.37", lan) == "192.168.1.37");
  CHECK(kept_host("192.168.1.37:80", lan) == "192.168.1.37");
  CHECK(kept_host("192.168.4.1", ap) == "192.168.4.1");
  CHECK(kept_host("192.168.4.1:80", ap) == "192.168.4.1");
  CHECK(kept_host("10.0.0.7", lan) == "10.0.0.7");
  CHECK(kept_host("255.255.255.255", lan) == "255.255.255.255");
  CHECK(kept_host("[fe80::1]", lan) == "[fe80::1]");
  CHECK(kept_host("[fe80::1]:8080", lan) == "[fe80::1]");
  CHECK(kept_host("[::ffff:192.168.1.37]", lan) == "[::ffff:192.168.1.37]");
  CHECK(kept_host("[2001:db8:0:0:0:0:0:1]", lan) == "[2001:db8:0:0:0:0:0:1]");

  // The device's own names are kept as typed (DNS names compare without
  // case), the port dropped: its .local name and the canary.local catch-all.
  CHECK(kept_host("canary-kitchen.local", lan) == "canary-kitchen.local");
  CHECK(kept_host("canary-kitchen.local:80", lan) == "canary-kitchen.local");
  CHECK(kept_host("Canary-Kitchen.LOCAL", lan) == "Canary-Kitchen.LOCAL");
  CHECK(kept_host("canary.local", lan) == "canary.local");
  CHECK(kept_host("CANARY.local:80", ap) == "CANARY.local");

  // Anything else is refused, never forwarded: a foreign name, another
  // Canary's, the device's name with more around it, a byte outside the
  // alphabet (a slash, a space, a CR, a percent, an underscore), a bare IPv6
  // literal (its colons read as a port), an address that is not one, an
  // empty or missing Host, a port alone, two ports.
  for (const char* bad : {"evil.example", "other-canary.local", "canary-kitchen.local.evil.example",
                          "xcanary-kitchen.local", "canary-kitchen.localx", "canary-kitchen", "kitchen.local",
                          "canary-kitchen.local/x", "canary-kitchen.local x", "canary-kitchen.local\r\nX: y",
                          "canary-kitchen.local%00", "canary_kitchen.local", "fe80::1", "999.1.1.1", "256.1.1.1",
                          "1.2.3", "1.2.3.4.5", "1.2.3.", ".1.2.3", "1..2.3", "1234.1.1.1", "[fe80]", "[]", "[",
                          "]:80", "", ":80", "192.168.1.37:80:80", "[fe80::1]x", "[fe80::g]"}) {
    CHECK(kept_host(bad, lan) == "");
    CHECK(kept_host(bad, ap) == "");
  }
  CHECK(kept_host(nullptr, lan) == "");

  // A device with no name has no .local name to keep; the catch-all and an
  // address are still its.
  const Inputs unnamed = make(Net::ON_STA_STA_UP, Grant::HTTPS_REDIRECT, nullptr);
  CHECK(kept_host("canary-kitchen.local", unnamed) == "");
  CHECK(kept_host("canary.local", unnamed) == "canary.local");
  CHECK(kept_host("192.168.1.37", unnamed) == "192.168.1.37");

  // A Host that does not fit the buffer is refused, not cut.
  CHECK(kept_host("192.168.1.37", lan, 12) == "");
  CHECK(kept_host("192.168.1.37", lan, 13) == "192.168.1.37");
  char out[8];
  std::memset(out, 'x', sizeof(out));
  CHECK(receipt_authority::redirect_host(out, sizeof(out), "192.168.1.37", lan) == 0);
  CHECK(out[0] == '\0');
  CHECK(receipt_authority::redirect_host(nullptr, 80, "192.168.1.37", lan) == 0);
  CHECK(receipt_authority::redirect_host(out, 0, "192.168.1.37", lan) == 0);
}

std::string location(const char* host, const char* uri, const Inputs& in, size_t cap = 256) {
  char out[256];
  std::memset(out, 'x', sizeof(out));
  const size_t n = receipt_authority::redirect_location(out, cap, host, uri, in);
  CHECK(n == std::strlen(out));
  return out;
}

void test_redirect_location() {
  const Inputs lan  = make(Net::ON_STA_STA_UP, Grant::HTTPS_REDIRECT);
  const Inputs ap   = make(Net::ON_SOFTAP_STA_UP, Grant::HTTPS_REDIRECT);
  const Inputs down = make(Net::STA_DOWN_ON_SOFTAP, Grant::HTTPS_REDIRECT);

  // A typed IP stays the address the browser reached: a Windows PC or an
  // Android phone without mDNS lands on https at it, not on a .local name it
  // cannot resolve (what #1802's merge sent it to).
  CHECK(location("192.168.1.37", "/settings", lan) == "https://192.168.1.37/settings");
  CHECK(location("192.168.1.37:80", "/", lan) == "https://192.168.1.37/");
  // The device's own .local name typed on the home network stays.
  CHECK(location("canary-kitchen.local", "/settings?x=1", lan) == "https://canary-kitchen.local/settings?x=1");
  CHECK(location("canary-kitchen.local:80", "/", lan) == "https://canary-kitchen.local/");
  // The setup network's captive browser: 192.168.4.1 and canary.local, as typed.
  CHECK(location("192.168.4.1", "/", down) == "https://192.168.4.1/");
  CHECK(location("canary.local", "/", down) == "https://canary.local/");
  CHECK(location("192.168.4.1", "/", ap) == "https://192.168.4.1/");
  // A foreign Host is refused and the Location falls back to the device's
  // name for this reach (the receipt's choice): never the Host.
  CHECK(location("evil.example", "/settings", lan) == "https://canary-kitchen.local/settings");
  CHECK(location("evil.example", "/", ap) == "https://192.168.4.1/");
  CHECK(location("evil.example", "/", down) == "https://192.168.4.1/");
  CHECK(location("canary-kitchen.local\r\nX: y", "/", lan) == "https://canary-kitchen.local/");
  // No Host (HTTP/1.0, or one the sketch's buffer could not hold): the same.
  CHECK(location("", "/x", lan) == "https://canary-kitchen.local/x");
  CHECK(location(nullptr, "/x", lan) == "https://canary-kitchen.local/x");
  // The fallback with no name is the STA's own address, as the receipt's.
  CHECK(location("evil.example", "/", make(Net::ON_STA_STA_UP, Grant::HTTPS_REDIRECT, "")) ==
        "https://192.168.1.37/");
  // ... and with nothing to name (the AP dropped, the STA down, no name)
  // nothing is written: the sketch answers 503. A typed IP is still kept
  // there: the request reached the device at it.
  const Inputs none = make(Net::AP_DROPPED_STA_DOWN, Grant::HTTPS_REDIRECT, nullptr);
  char out[256];
  std::memset(out, 'x', sizeof(out));
  CHECK(receipt_authority::redirect_location(out, sizeof(out), "evil.example", "/", none) == 0);
  CHECK(out[0] == '\0');
  CHECK(location("", "/", none) == "");
  CHECK(location("192.168.1.37", "/", none) == "https://192.168.1.37/");

  // A URI the 256-byte Location cannot hold is not cut (a cut URI redirects
  // to the wrong page): the root instead. "https://canary-kitchen.local" is
  // 28 bytes, so a 227-byte URI fills the buffer exactly with its NUL and is
  // kept, and one byte more goes to the root.
  const std::string fits = "/" + std::string(226, 'a');
  CHECK(location("canary-kitchen.local", fits.c_str(), lan) == "https://canary-kitchen.local" + fits);
  CHECK(location("canary-kitchen.local", fits.c_str(), lan).size() == 255);
  const std::string over = "/" + std::string(227, 'a');
  CHECK(location("canary-kitchen.local", over.c_str(), lan) == "https://canary-kitchen.local/");
  const std::string huge = "/" + std::string(1000, 'a');
  CHECK(location("192.168.1.37", huge.c_str(), lan) == "https://192.168.1.37/");
  CHECK(location("evil.example", huge.c_str(), lan) == "https://canary-kitchen.local/");
  CHECK(location("evil.example", huge.c_str(), ap) == "https://192.168.4.1/");
  // An empty URI (nothing sends one: req->uri starts with "/") is appended
  // as it is.
  CHECK(location("192.168.1.37", "", lan) == "https://192.168.1.37");
  CHECK(location("192.168.1.37", nullptr, lan) == "https://192.168.1.37");
  // A buffer too small for even the root: nothing, not half a URL.
  std::memset(out, 'x', sizeof(out));
  CHECK(receipt_authority::redirect_location(out, 21, "192.168.1.37", "/", lan) == 0);   // 21 bytes + NUL
  CHECK(out[0] == '\0');
  CHECK(receipt_authority::redirect_location(out, 22, "192.168.1.37", "/", lan) == 21);
  CHECK(std::string(out) == "https://192.168.1.37/");
  CHECK(receipt_authority::redirect_location(out, 22, "192.168.1.37", "/x", lan) == 21);   // the root
  CHECK(std::string(out) == "https://192.168.1.37/");
  CHECK(receipt_authority::redirect_location(nullptr, 256, "192.168.1.37", "/", lan) == 0);
  CHECK(receipt_authority::redirect_location(out, 0, "192.168.1.37", "/", lan) == 0);
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

  // The SoftAP dropped on core 3.x reads 0.0.0.0: test_ap_dropped() holds
  // every grant there. On a core that kept the AP netif's configured
  // 192.168.4.1 after the drop, a LAN request still does not match it.
  Inputs kept = make(Net::ON_STA_STA_UP, Grant::BOOT_TAP);
  CHECK(authority(kept) == kLocal);

  // The longest label generate_mdns_hostname can make (canary- + 32 bytes,
  // inside the 40-byte mdns_hostname) fits the sketch's 80-byte buffer.
  std::string longest = "canary-" + std::string(32, 'a');
  Inputs big = make(Net::ON_STA_STA_UP, Grant::BEARER, longest.c_str());
  CHECK(authority(big) == longest + ".local");

  // A buffer too small for the name is not cut: the address this request
  // reaches the device at (the STA's for a request on the STA, the SoftAP's
  // for one that arrived there), and nothing at all when even that cannot fit.
  char small[16];
  CHECK(receipt_authority::write(small, sizeof(small), make(Net::ON_STA_STA_UP, Grant::BEARER)) ==
        kStaStr.size());
  CHECK(std::string(small) == kStaStr);
  CHECK(receipt_authority::write(small, sizeof(small), make(Net::ON_SOFTAP_STA_UP, Grant::SETUP_DOOR_CLAIM)) ==
        kApStr.size());
  CHECK(std::string(small) == kApStr);
  CHECK(receipt_authority::write(small, sizeof(small), make(Net::AP_DROPPED_STA_DOWN, Grant::BEARER)) == 0);
  CHECK(small[0] == '\0');
  // Exactly one byte short (the name fills the buffer, no room for its NUL)
  // is too small too.
  char exact[21];   // "canary-kitchen.local" is 20 bytes
  CHECK(receipt_authority::write(exact, kLocal.size(), make(Net::ON_STA_STA_UP, Grant::BEARER)) ==
        kStaStr.size());
  CHECK(std::string(exact) == kStaStr);
  CHECK(receipt_authority::write(exact, sizeof(exact), make(Net::ON_STA_STA_UP, Grant::BEARER)) ==
        kLocal.size());
  CHECK(std::string(exact) == kLocal);
  char tiny[8];
  std::memset(tiny, 'x', sizeof(tiny));
  CHECK(receipt_authority::write(tiny, sizeof(tiny), make(Net::ON_STA_STA_UP, Grant::BEARER)) == 0);
  CHECK(tiny[0] == '\0');
  CHECK(receipt_authority::write(nullptr, 80, make(Net::ON_STA_STA_UP, Grant::BEARER)) == 0);
  CHECK(receipt_authority::write(tiny, 0, make(Net::ON_STA_STA_UP, Grant::BEARER)) == 0);
}

// ── the octet packing the sketch's ipv4_host_order() uses ─────────────────

void test_ipv4_from_octets() {
  // ip[0] is the first octet: the packing every address in Inputs uses, so a
  // SoftAP address read from WiFi.softAPIP() equals the request's local
  // address read from the socket (ipv4_from_addr) for the same interface.
  CHECK(receipt_authority::ipv4_from_octets(192, 168, 4, 1) == kSoftAp);
  CHECK(receipt_authority::ipv4_from_octets(192, 168, 1, 37) == kSta);
  CHECK(receipt_authority::ipv4_from_octets(1, 2, 3, 4) == 0x01020304u);
  CHECK(receipt_authority::ipv4_from_octets(255, 0, 0, 0) == 0xFF000000u);
  CHECK(receipt_authority::ipv4_from_octets(0, 0, 0, 0) == 0u);
  const uint8_t wire[4] = {192, 168, 4, 1};
  CHECK(receipt_authority::ipv4_from_octets(wire[0], wire[1], wire[2], wire[3]) ==
        receipt_authority::ipv4_from_addr(receipt_authority::AddrFamily::IPV4, wire));
  char d[16];
  receipt_authority::write_dotted(d, sizeof(d), receipt_authority::ipv4_from_octets(10, 20, 30, 40));
  CHECK(std::string(d) == "10.20.30.40");
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
  // Not ::ffff:0:0/96: a nonzero byte 8 or 9 ahead of the ffff, and an ffff
  // that is only half there.
  const uint8_t zeros_short[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xFF, 0xFF, 192, 168, 4, 1};
  const uint8_t zeros_short8[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0xFF, 0xFF, 192, 168, 4, 1};
  const uint8_t half_ffff[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0x00, 192, 168, 4, 1};
  const uint8_t half_ffff2[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0xFF, 192, 168, 4, 1};
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV6, zeros_short) == 0);
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV6, zeros_short8) == 0);
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV6, half_ffff) == 0);
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV6, half_ffff2) == 0);
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV4, v4) == kSta);
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV6, mapped) == kSoftAp);
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV6, compat) == 0);
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV6, link_local) == 0);
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV6, loopback) == 0);
  CHECK(receipt_authority::ipv4_from_addr(RA::OTHER, v4) == 0);
  CHECK(receipt_authority::ipv4_from_addr(RA::IPV4, nullptr) == 0);
  // The same answers as the flagship's helper, vector for vector.
  const uint8_t* six[] = {mapped, compat, link_local, loopback, zeros_short, zeros_short8, half_ffff, half_ffff2};
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

// The source with its // and /* */ comments blanked out (string and character
// literals kept as they are), so a comment that names a token cannot stand in
// for the code that should.
std::string strip_comments(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  size_t i = 0;
  while (i < src.size()) {
    const char c = src[i];
    if (c == '"' || c == '\'') {
      const char q = c;
      out += src[i++];
      while (i < src.size() && src[i] != q) {
        if (src[i] == '\\' && i + 1 < src.size()) out += src[i++];
        out += src[i++];
      }
      if (i < src.size()) out += src[i++];
    } else if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
      while (i < src.size() && src[i] != '\n') ++i;
    } else if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
      const size_t end = src.find("*/", i + 2);
      i = end == std::string::npos ? src.size() : end + 2;
      out += ' ';
    } else {
      out += src[i++];
    }
  }
  return out;
}

std::string body_of(const std::string& src, const std::string& head) {
  const size_t at = src.find(head);
  if (at == std::string::npos) return "";
  if (src.find(head, at + 1) != std::string::npos) return "";   // one definition only
  const size_t end = src.find("\n}\n", at);
  return src.substr(at, end == std::string::npos ? std::string::npos : end + 2 - at);
}

// Whitespace runs collapsed to one space, and none beside punctuation, so a
// function body compares by its tokens.
std::string squash(const std::string& s) {
  std::string out;
  bool space = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { space = true; continue; }
    if (space && !out.empty()) {
      const char p = out.back();
      const bool word_p = std::isalnum((unsigned char)p) || p == '_';
      const bool word_c = std::isalnum((unsigned char)c) || c == '_';
      if (word_p && word_c) out += ' ';
    }
    space = false;
    out += c;
  }
  return out;
}

bool has(const std::string& s, const char* re) { return std::regex_search(s, std::regex(re)); }

size_t count_of(const std::string& s, const char* re) {
  const std::regex r(re);
  return (size_t)std::distance(std::sregex_iterator(s.begin(), s.end(), r), std::sregex_iterator());
}

void test_sketch_glue() {
  const std::string raw = read_file(CANARY_WAP_INO);
  CHECK(!raw.empty());
  const std::string ino = strip_comments(raw);
  CHECK(ino.find("#include \"receipt_authority.h\"") != std::string::npos);

  // The octets of an IPAddress, packed by the header's ipv4_from_octets in
  // index order (the order test_ipv4_from_octets holds), nothing else.
  const std::string host_order = body_of(ino, "static uint32_t ipv4_host_order(const IPAddress& ip) {");
  CHECK(squash(host_order) ==
        "static uint32_t ipv4_host_order(const IPAddress&ip){return "
        "receipt_authority::ipv4_from_octets(ip[0],ip[1],ip[2],ip[3]);}");

  // The socket's LOCAL address, read whole: both families, each from its own
  // address field, the IPv6 branch under the guard lwIP's opt.h defines, a
  // failed read as 0.
  const std::string sock = body_of(ino, "static uint32_t request_local_ipv4(httpd_req_t* req) {");
  CHECK(squash(sock) ==
        "static uint32_t request_local_ipv4(httpd_req_t*req){"
        "const int fd=httpd_req_to_sockfd(req);"
        "if(fd<0)return 0;"
        "struct sockaddr_storage ss;"
        "socklen_t len=sizeof(ss);"
        "memset(&ss,0,sizeof(ss));"
        "if(getsockname(fd,(struct sockaddr*)&ss,&len)!=0)return 0;"
        "if(ss.ss_family==AF_INET){"
        "const struct sockaddr_in*sin=(const struct sockaddr_in*)&ss;"
        "return receipt_authority::ipv4_from_addr(receipt_authority::AddrFamily::IPV4,"
        "(const uint8_t*)&sin->sin_addr.s_addr);}"
        "#if defined(LWIP_IPV6)&&LWIP_IPV6 "
        "if(ss.ss_family==AF_INET6){"
        "const struct sockaddr_in6*sin6=(const struct sockaddr_in6*)&ss;"
        "return receipt_authority::ipv4_from_addr(receipt_authority::AddrFamily::IPV6,"
        "(const uint8_t*)&sin6->sin6_addr);}"
        "#endif return 0;}");

  // Every input from where the device keeps it, once.
  const std::string reach = body_of(ino, "static receipt_authority::Inputs request_reach(httpd_req_t* req, "
                                         "receipt_authority::Grant grant) {");
  CHECK(squash(reach) ==
        "static receipt_authority::Inputs request_reach(httpd_req_t*req,receipt_authority::Grant grant){"
        "receipt_authority::Inputs reach={};"
        "reach.request_local=request_local_ipv4(req);"
        "reach.softap_ip=ipv4_host_order(WiFi.softAPIP());"
        "reach.sta_ip=WiFi.isConnected()?ipv4_host_order(WiFi.localIP()):0;"
        "reach.mdns_host=g_device.mdns_hostname;"
        "reach.grant=grant;"
        "return reach;}");

  // The receipt: the header writes the authority from the request's reach,
  // nothing written refuses, and that buffer is the base_url's authority,
  // assigned once.
  const std::string send = body_of(ino, "static esp_err_t send_provisioning_receipt(httpd_req_t* req, "
                                        "receipt_authority::Grant grant) {");
  CHECK(!send.empty());
  CHECK(has(squash(send), R"(char authority\[80\];if\(receipt_authority::write\(authority,sizeof\(authority\),request_reach\(req,grant\)\)==0\)\{return http_send_error\(req,503,"no_address"\);\})"));
  CHECK(count_of(send, R"(\bin\.ap_ip\s*=)") == 1);
  CHECK(has(send, R"(\bin\.ap_ip\s*=\s*authority\s*;)"));
  CHECK(count_of(send, R"(\bauthority\b)") == 4);   // declared, written, sized, read once
  // No second spelling of the address beside the header's.
  CHECK(!has(send, R"(softAPIP|localIP|\.local")"));

  // Each grant passes its own name: a bearer (the header alone, never the
  // cv_session cookie — api_auth_bearer_valid, sweep F289; test_receipt_grant
  // drives it), a served claim, else the tap.
  const std::string route = body_of(ino, "static esp_err_t handle_provisioning_receipt(httpd_req_t* req) {");
  CHECK(!route.empty());
  CHECK(has(route, R"(api_auth_bearer_valid\(req, g_device\.api_token_str\)\) \{\s*return send_provisioning_receipt\(req, receipt_authority::Grant::BEARER\);)"));
  CHECK(has(route, R"(send_provisioning_receipt\(\s*req, claim_served \? receipt_authority::Grant::SETUP_DOOR_CLAIM\s*: receipt_authority::Grant::BOOT_TAP\);)"));
  CHECK(count_of(ino, R"(\bsend_provisioning_receipt\()") == 3);   // the definition and the two calls

  // Port 80's redirect, whole: the Host read into a buffer (a header that
  // does not fit reads as none), and the Location — the Host kept by the
  // header's rule, else the same reach as the receipt's, as HTTPS_REDIRECT;
  // the root for a URI that does not fit — written by the header into 256
  // bytes; nothing written refuses; the SoftAP and STA addresses are not
  // read here.
  const std::string redirect = body_of(ino, "static esp_err_t handle_https_redirect(httpd_req_t* req) {");
  CHECK(!redirect.empty());
  CHECK(squash(redirect) ==
        "static esp_err_t handle_https_redirect(httpd_req_t*req){"
        "char host[96]={0};"
        "if(httpd_req_get_hdr_value_str(req,\"Host\",host,sizeof(host))!=ESP_OK)host[0]='\\0';"
        "char location[256];"
        "if(receipt_authority::redirect_location(location,sizeof(location),host,req->uri,"
        "request_reach(req,receipt_authority::Grant::HTTPS_REDIRECT))==0){"
        "httpd_resp_set_status(req,\"503 Service Unavailable\");"
        "return httpd_resp_sendstr(req,\"No address to redirect to.\");}"
        "httpd_resp_set_status(req,\"301 Moved Permanently\");"
        "httpd_resp_set_hdr(req,\"Location\",location);"
        "return httpd_resp_sendstr(req,\"Redirecting to HTTPS...\");}");
  CHECK(!has(redirect, R"(softAPIP|localIP)"));
  CHECK(count_of(ino, R"(\bhandle_https_redirect\b)") == 3);   // the definition and the two /* registrations

  // Only request_reach() reads where the device is for these answers.
  CHECK(count_of(ino, R"(\brequest_reach\()") == 3);   // the definition, the receipt, the redirect
  CHECK(count_of(ino, R"(\brequest_local_ipv4\()") == 2);   // the definition and request_reach
}

}  // namespace

int main() {
  test_bearer();
  test_boot_tap();
  test_setup_door_claim();
  test_https_redirect();
  test_redirect_host();
  test_redirect_location();
  test_ap_dropped();
  test_unread_socket_fallback();
  test_edges();
  test_ipv4_from_octets();
  test_ipv4_from_addr();
  test_sketch_glue();
  if (g_failures) {
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d RECEIPT AUTHORITY CHECKS PASSED\n", g_checks);
  return 0;
}
