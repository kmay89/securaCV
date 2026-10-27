// After first-boot setup, the bearer alone moves the network a canary-wap
// joins (sweep F334, F345, F333), and the bearer alone reaches its two
// reboot routes (F361, decided by the same rule; the firmware install, which
// restarts the unit only into a newer signed image, stays on the session).
//
// Before, POST /api/wifi/connect and /api/wifi/ap-only went through
// wifi_change_authorize(), which took the wizard's pair token first and then
// a cookie or a bearer through api_auth_check(), and POST /api/wifi/forget
// and /api/wifi/disconnect were api_auth_check() wrappers. The landing page
// hands any caller on the home network both a pair token (its ?cv_pair link
// mints into the wizard's slot pool) and a 24 h cv_session cookie, and
// api_auth_check() takes the cookie before it compares anything. So any host
// on the network could move the unit to a network of its choosing, knock it
// off its own, or Forget it, which wipes every saved network and re-opens the
// Bluetooth setup door, whose claim buys the receipt. Now, whenever
// setup_wizard::is_active() is false, the gate is api_auth_check_bearer():
// no cookie, the per-address lockout, its own 401/403/429. During setup the
// wizard's gate on Connect and AP-only is unchanged (handle_ui mints no
// session then, but the receipt route does on a BOOT tap or the setup door's
// claim, and that cookie takes the presented-credential branch); Forget and
// Disconnect ask nothing about setup and take the bearer then too. POST
// /api/reboot and POST /api/safe-mode/retry were api_auth_check() wrappers
// too, so any landing session rebooted the unit at will: a LAN denial, and
// each reboot emptied the RAM lockout table, resetting a guesser's budget
// (F361). Both wrappers are api_auth_check_bearer() now.
//
// This runs the REAL api_auth.h (the lockout table, sweep F343) and
// provisioning_logic.h (keyless_join_open(), the one rule for the setup door
// and the camera's boot scan, sweep F333), and the functions cut verbatim
// out of canary_wap.ino (firmware/tests_host/cut_functions.awk):
// wifi_change_authorize(), the Forget, Disconnect and Reconnect wrappers,
// the reboot and safe-mode-retry wrappers, ble_improv_door_should_open()
// and auth_request_source() (the lockout's source over stubs/receipt_grant's
// peer). Stand-ins: the wizard flag, the pair-token store, the session
// bridge, and the handlers the wrappers guard (Forget's clears the stored
// network, as wifi_clear_credentials() does).
// What it pins:
//   - after setup, the landing page's pair token alone, its session alone,
//     and both together: 401 with WWW-Authenticate and no strike (before:
//     the change went ahead);
//   - after setup, a wrong bearer beside the cookie: 403 and one strike for
//     that address (before: the cookie went ahead, with no strike); the
//     right bearer, with or without the cookie: served;
//   - a host's wrong bearers hold only its own address: after five, its
//     right bearer gets 429 with Retry-After and another host's is served;
//   - during setup: the pair token goes ahead, nothing at all is the
//     wizard's invalid_token, a presented credential takes api_auth_check;
//   - under a host striking from nine addresses, the owner's right bearer
//     from an untracked tenth: 429 on the Wi-Fi gate and on Forget while the
//     shared overflow runs, served once it ends;
//   - Forget and Disconnect, after setup and during it: a session alone is
//     401, the handler never runs, the network stays on file and the setup
//     door stays shut (before: Forget ran and the door opened); the bearer
//     runs each once;
//     Reconnect, which only rejoins the stored network, still takes the
//     session;
//   - Forget leaves a standalone unit standalone: a bearer Forget on a unit
//     standalone with a network on file leaves the setup door shut and the
//     boot scan's rule false;
//   - the reboot and the safe-mode retry: a session alone is 401 and the
//     unit does not restart (before: it did), a wrong bearer is 403 and a
//     strike, the bearer restarts it once;
//   - the door's composition of the keyless-join rule;
//   - the sketch's text (CANARY_WAP_INO, comments stripped): POST
//     /api/wifi/qr-scan answers 404 outside setup before it reads anything;
//     Connect and AP-only ask the gate before any write; the boot scan asks
//     the keyless-join rule first, and a running window asks after every
//     frame, before a code can act, whether it may still act (a boot window
//     that rule, a phone session's window whether setup still runs:
//     scan_window_open()); Forget clears the broker login
//     after the networks and reports it, and neither it nor
//     wifi_clear_credentials() writes the standalone choice, which the 10 s
//     hold clears; the six routes are registered on these handlers.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run-wifi-authority

#include "arduino_globals.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "Arduino.h"                 // stubs/receipt_grant: the clock
#include "esp_http_server.h"         // stubs/receipt_grant
#include "provisioning_gate.h"       // the sketch's staged copy: the peer key
#include "api_auth.h"                // the REAL one
#include "peer_source.h"             // stubs/receipt_grant: request_peer_key()
#include "wifi_provisioning_auth.h"  // WifiChangeAuth
#include "provisioning_logic.h"      // the REAL keyless_join_open()

#ifndef WIFI_AUTHORITY_INC
#error "WIFI_AUTHORITY_INC must name the cut of canary_wap.ino (the Makefile passes it)"
#endif
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
#define CHECK_MSG(c, msg)                                                 \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      ++g_failures;                                                       \
      std::printf("FAIL %s:%d: %s -- %s\n", __FILE__, __LINE__, #c,       \
                  std::string(msg).c_str());                              \
    }                                                                     \
  } while (0)
}  // namespace

// One live session, as the landing page's ?cv_pair exchange mints it for any
// caller on the home network; a request carries it as a cv_session cookie.
// The bridge itself is defined after the cut, as the sketch sees it: through
// api_auth.h's weak declaration alone.
static std::string g_live_session;

// ── the sketch's state the cut functions read ────────────────────────────────
namespace setup_wizard {
// setup_wizard.h: set only by init() on a boot without setup_ok, cleared
// only at runtime.
static bool g_active = false;
bool is_active() { return g_active; }
}  // namespace setup_wizard

namespace csi_integration {
// The wizard's pair token: one slot pool, which the landing page's ?cv_pair
// link also mints into after setup (no token kind).
static std::string g_pair_token;
bool pair_token_valid(const char* hex) {
  return hex != nullptr && !g_pair_token.empty() && g_pair_token == hex;
}
}  // namespace csi_integration

struct { char api_token_str[36]; } g_device;
struct { bool configured = false; } g_wifi_creds;
static bool g_improv_join_pending = false;
static bool g_wifi_ap_only = false;

// The handlers the wrappers guard, each counting its runs. Forget clears the
// stored network, as wifi_clear_credentials() does (and re-arms the door).
static int g_disconnects = 0;
static int g_forgets = 0;
static int g_reconnects = 0;
static esp_err_t handle_wifi_disconnect(httpd_req_t* req) {
  ++g_disconnects;
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}
static esp_err_t handle_wifi_forget(httpd_req_t* req) {
  ++g_forgets;
  g_wifi_creds.configured = false;
  return httpd_resp_sendstr(req, "{\"ok\":true,\"broker_login_cleared\":true}");
}
static esp_err_t handle_wifi_reconnect(httpd_req_t* req) {
  ++g_reconnects;
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}
// The two restarts (F361), each counting the restart it would make.
static int g_reboots = 0;
static int g_safe_mode_retries = 0;
static esp_err_t handle_reboot(httpd_req_t* req) {
  ++g_reboots;
  return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Rebooting...\"}");
}
static esp_err_t handle_safe_mode_retry(httpd_req_t* req) {
  ++g_safe_mode_retries;
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// ── the functions, cut verbatim out of canary_wap.ino ────────────────────────
#include WIFI_AUTHORITY_INC

// ── the cookie bridge csi_integration.cpp supplies ───────────────────────────
bool cv_session_validate(httpd_req_t* req) {
  if (!req || g_live_session.empty()) return false;
  auto it = req->req_hdrs.find("Cookie");
  if (it == req->req_hdrs.end()) return false;
  return it->second.find("cv_session=" + g_live_session) != std::string::npos;
}

namespace {

const char* const kToken = "cv_7kX9mN2pQ4rS6tU8vW0xY1zA3bC5dE7f";   // 35 chars, "cv_" + 32
const char* const kWrong = "cv_7kX9mN2pQ4rS6tU8vW0xY1zA3bC5dE7g";   // one char off
const char* const kPair =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";  // the landing's pair token
const uint32_t kHostA = 0xC0A80114;   // 192.168.1.20, the stub's default peer
const uint32_t kHostB = 0xC0A80115;   // 192.168.1.21: Req::from(192, 168, 1, 21)

unsigned strikes(uint32_t host) {
  return canary::net::auth_backoff::failures_of(g_auth_table, host, millis());
}
bool held(uint32_t host) { return canary::net::auth_backoff::locked(g_auth_table, host, millis()); }
// The clock moved to the end of `host`'s backoff, as a client that waits out
// its Retry-After.
void wait_out(uint32_t host) {
  stub_grant::now_ms += canary::net::auth_backoff::remaining_ms(g_auth_table, host, millis());
}

// A set-up unit on its home network: the landing page has minted a session
// and a pair token for whoever loaded it, a network is on file, the door is
// shut.
void reset_device() {
  setup_wizard::g_active = false;
  g_live_session.assign(64, 'a');
  csi_integration::g_pair_token = kPair;
  std::snprintf(g_device.api_token_str, sizeof(g_device.api_token_str), "%s", kToken);
  g_wifi_creds.configured = true;
  g_improv_join_pending = false;
  g_wifi_ap_only = false;
  g_disconnects = g_forgets = g_reconnects = 0;
  g_reboots = g_safe_mode_retries = 0;
  g_auth_table = {};
  stub_grant::now_ms = 100000;
}

struct Req {
  httpd_req_t r;
  Req& cookie() {
    r.req_hdrs["Cookie"] = "cv_session=" + g_live_session + "; other=1";
    return *this;
  }
  Req& bearer(const char* tok) {
    r.req_hdrs["Authorization"] = std::string("Bearer ") + tok;
    return *this;
  }
  Req& from(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, a, b, c, d};
    std::memcpy(r.peer, mapped, sizeof(mapped));
    return *this;
  }
  Req() { r.req_hdrs["Host"] = "canary-7916.local"; }
};

bool has_header(const httpd_req_t& r, const char* name) {
  for (const auto& h : r.resp_hdrs) if (h.first == name) return true;
  return false;
}

const char* name_of(WifiChangeAuth v) {
  switch (v) {
    case WifiChangeAuth::PROCEED: return "PROCEED";
    case WifiChangeAuth::INVALID_TOKEN: return "INVALID_TOKEN";
    case WifiChangeAuth::RESPONDED: return "RESPONDED";
  }
  return "?";
}

// ── wifi_change_authorize(): POST /api/wifi/connect and /ap-only ─────────────

// After setup the landing page's grants move no network: its pair token
// (handed to the gate as the body's "token"), its session, and both. Each is
// 401 with WWW-Authenticate, and no strike (a request that sent no bearer
// guessed nothing). Before F334 each of the three went ahead.
void test_the_landing_grants_move_no_network() {
  struct Case { const char* name; bool cookie; const char* token; };
  const Case cases[] = {
      {"the pair token alone", false, kPair},
      {"the session alone", true, ""},
      {"the session and the pair token", true, kPair},
  };
  for (const Case& c : cases) {
    reset_device();
    Req q;
    if (c.cookie) q.cookie();
    CHECK(cv_session_validate(&q.r) == c.cookie);
    CHECK(csi_integration::pair_token_valid(kPair));
    const WifiChangeAuth v = wifi_change_authorize(&q.r, c.token);
    CHECK_MSG(v == WifiChangeAuth::RESPONDED, std::string(c.name) + ": " + name_of(v));
    CHECK_MSG(q.r.status == "401 Unauthorized" && q.r.sends == 1, c.name);
    CHECK_MSG(has_header(q.r, "WWW-Authenticate"), c.name);
    CHECK_MSG(strikes(kHostA) == 0 && !held(kHostA), c.name);
  }
  std::printf("PASS the_landing_grants_move_no_network\n");
}

// After setup the bearer moves it, with or without the cookie beside it; a
// wrong bearer beside the cookie is 403 and one strike for its address
// (before: the cookie went ahead and the wrong bearer was never compared).
void test_the_bearer_moves_the_network() {
  reset_device();
  Req both;
  both.cookie().bearer(kToken);
  CHECK(wifi_change_authorize(&both.r, "") == WifiChangeAuth::PROCEED);
  CHECK(both.r.sends == 0 && strikes(kHostA) == 0);
  Req alone;
  alone.bearer(kToken);
  CHECK(wifi_change_authorize(&alone.r, "") == WifiChangeAuth::PROCEED);
  CHECK(alone.r.sends == 0);
  Req pair_too;   // the pair token beside the bearer changes nothing
  pair_too.bearer(kToken);
  CHECK(wifi_change_authorize(&pair_too.r, kPair) == WifiChangeAuth::PROCEED);
  Req wrong;
  wrong.cookie().bearer(kWrong);
  const WifiChangeAuth v = wifi_change_authorize(&wrong.r, kPair);
  CHECK_MSG(v == WifiChangeAuth::RESPONDED, name_of(v));
  CHECK(wrong.r.status == "403 Forbidden" && wrong.r.sends == 1);
  CHECK(strikes(kHostA) == 1 && held(kHostA));
  std::printf("PASS the_bearer_moves_the_network\n");
}

// A host's wrong bearers hold only that host's address (F343): five, each
// after the last one's backoff ran out, then its right bearer is 429 with
// Retry-After while another host's right bearer is served at once.
void test_a_hosts_wrong_bearers_hold_only_its_address() {
  reset_device();
  for (unsigned i = 1; i <= 5; ++i) {
    Req w;
    w.bearer(kWrong);
    CHECK(wifi_change_authorize(&w.r, "") == WifiChangeAuth::RESPONDED);
    CHECK_MSG(w.r.status == "403 Forbidden", std::to_string(i));
    CHECK_MSG(strikes(kHostA) == i, std::to_string(i));
    if (i < 5) wait_out(kHostA);
  }
  Req a;
  a.cookie().bearer(kToken);
  CHECK(wifi_change_authorize(&a.r, "") == WifiChangeAuth::RESPONDED);
  CHECK(a.r.status == "429 Too Many Requests" && has_header(a.r, "Retry-After"));
  Req b;
  b.from(192, 168, 1, 21).bearer(kToken);
  CHECK(wifi_change_authorize(&b.r, "") == WifiChangeAuth::PROCEED);
  CHECK(b.r.sends == 0 && strikes(kHostB) == 0 && strikes(kHostA) == 5);
  std::printf("PASS a_hosts_wrong_bearers_hold_only_its_address\n");
}

// A host that strikes from nine addresses fills the eight tracked slots and
// starts the shared overflow, which holds every untracked address (F343), the
// owner's among them: the owner's right bearer, from a tenth address, is 429
// with Retry-After on the Wi-Fi gate and on Forget, compared with nothing and
// counted nowhere, and nothing moves; once the overflow's backoff ends, the
// same bearer moves the network. The way out the docs give under such a host
// (wait until it stops, then the token) rests on both halves.
void test_nine_striking_addresses_hold_the_owners_bearer() {
  reset_device();
  for (uint8_t i = 1; i <= 9; ++i) {
    Req w;
    w.from(10, 0, 0, i).bearer(kWrong);
    CHECK(wifi_change_authorize(&w.r, "") == WifiChangeAuth::RESPONDED);
    CHECK_MSG(w.r.status == "403 Forbidden", std::to_string(i));
  }
  CHECK(g_auth_table.overflow.failures == 1);
  const uint32_t kOwner = 0xC0A8011E;   // 192.168.1.30, tracked by no slot
  CHECK(held(kOwner) && strikes(kOwner) == 0);
  Req a;
  a.from(192, 168, 1, 30).cookie().bearer(kToken);
  CHECK(wifi_change_authorize(&a.r, "") == WifiChangeAuth::RESPONDED);
  CHECK(a.r.status == "429 Too Many Requests" && has_header(a.r, "Retry-After"));
  Req f;
  f.from(192, 168, 1, 30).bearer(kToken);
  CHECK(handle_wifi_forget_auth(&f.r) == ESP_OK);
  CHECK(f.r.status == "429 Too Many Requests" && has_header(f.r, "Retry-After"));
  CHECK(g_forgets == 0 && g_wifi_creds.configured);
  CHECK(g_auth_table.overflow.failures == 1 && strikes(kOwner) == 0);
  wait_out(kOwner);
  CHECK(!held(kOwner));
  Req later;
  later.from(192, 168, 1, 30).bearer(kToken);
  CHECK(wifi_change_authorize(&later.r, "") == WifiChangeAuth::PROCEED);
  CHECK(later.r.sends == 0);
  std::printf("PASS nine_striking_addresses_hold_the_owners_bearer\n");
}

// During first-boot setup the wizard's gate is as it was: its pair token
// goes ahead, a request with nothing is the friendly invalid_token (the
// wizard re-issues and retries; nothing sent here), and a presented
// credential takes api_auth_check (a cookie goes ahead, a wrong bearer is
// 403 and a strike).
void test_during_setup_the_wizard_gate_is_unchanged() {
  reset_device();
  setup_wizard::g_active = true;
  Req pair;
  CHECK(wifi_change_authorize(&pair.r, kPair) == WifiChangeAuth::PROCEED);
  CHECK(pair.r.sends == 0);
  Req none;
  CHECK(wifi_change_authorize(&none.r, "") == WifiChangeAuth::INVALID_TOKEN);
  CHECK(none.r.sends == 0);
  Req stale;
  CHECK(wifi_change_authorize(&stale.r, "00") == WifiChangeAuth::INVALID_TOKEN);
  Req cookie;
  cookie.cookie();
  CHECK(wifi_change_authorize(&cookie.r, "") == WifiChangeAuth::PROCEED);
  Req wrong;
  wrong.bearer(kWrong);
  CHECK(wifi_change_authorize(&wrong.r, "") == WifiChangeAuth::RESPONDED);
  CHECK(wrong.r.status == "403 Forbidden" && strikes(kHostA) == 1);
  std::printf("PASS during_setup_the_wizard_gate_is_unchanged\n");
}

// ── the Forget, Disconnect and Reconnect wrappers ────────────────────────────

// Forget and Disconnect take the bearer alone (F334, F345). A session alone
// is 401 with no strike, the handler never runs, the network stays on file
// and the Bluetooth setup door stays shut; a wrong bearer beside it is 403
// and a strike; the bearer runs the handler once, and only then does Forget
// open the door. Before, the session ran both, and Forget opened the door
// for the first phone in radio range to claim. The same during setup: the
// wrappers ask nothing about it, unlike wifi_change_authorize(), so a session
// the receipt route minted then (on a BOOT tap or the setup door's claim) is
// refused too, where before F334 it ran both.
void test_forget_and_disconnect_take_the_bearer() {
  struct Route { const char* name; esp_err_t (*wrapper)(httpd_req_t*); int* ran; bool opens_door; };
  const Route routes[] = {
      {"forget", handle_wifi_forget_auth, &g_forgets, true},
      {"disconnect", handle_wifi_disconnect_auth, &g_disconnects, false},
  };
  for (const Route& rt : routes) {
    for (bool during_setup : {false, true}) {
      const std::string name = std::string(rt.name) + (during_setup ? " (setup)" : "");
      reset_device();
      setup_wizard::g_active = during_setup;
      CHECK_MSG(!ble_improv_door_should_open(), name);
      Req s;
      s.cookie();
      CHECK(rt.wrapper(&s.r) == ESP_OK);
      CHECK_MSG(s.r.status == "401 Unauthorized" && has_header(s.r, "WWW-Authenticate"), name);
      CHECK_MSG(*rt.ran == 0 && g_wifi_creds.configured, name);
      CHECK_MSG(!ble_improv_door_should_open(), name);
      CHECK_MSG(strikes(kHostA) == 0, name);
      Req w;
      w.cookie().bearer(kWrong);
      CHECK(rt.wrapper(&w.r) == ESP_OK);
      CHECK_MSG(w.r.status == "403 Forbidden" && *rt.ran == 0, name);
      CHECK_MSG(strikes(kHostA) == 1, name);
      wait_out(kHostA);
      Req b;
      b.bearer(kToken);
      CHECK(rt.wrapper(&b.r) == ESP_OK);
      CHECK_MSG(b.r.status == "200 OK" && *rt.ran == 1, name);
      CHECK_MSG(ble_improv_door_should_open() == rt.opens_door, name);
    }
  }
  std::printf("PASS forget_and_disconnect_take_the_bearer\n");
}

// Forget leaves a standalone unit standalone (F333). A unit can be standalone
// with a network still on file (the wizard's Connect saved one, then "Use
// without home WiFi" set the choice and left it), and the dashboard shows
// Remove credentials whenever a network is on file. The bearer's Forget
// clears the network, not the standalone choice, so neither keyless join
// opens: the setup door stays shut, and the camera's boot scan, which asks
// keyless_join_open() over the same two facts, stays off. The dashboard's
// confirm and the FAQ say so; test_sketch_text holds the real Forget and the
// wipe it runs to never touching the choice.
void test_forget_leaves_a_standalone_unit_standalone() {
  reset_device();
  g_wifi_ap_only = true;
  CHECK(!ble_improv_door_should_open());
  Req b;
  b.bearer(kToken);
  CHECK(handle_wifi_forget_auth(&b.r) == ESP_OK);
  CHECK(b.r.status == "200 OK" && g_forgets == 1 && !g_wifi_creds.configured);
  CHECK(g_wifi_ap_only);
  CHECK(!ble_improv_door_should_open());
  CHECK(!provisioning_logic::keyless_join_open(g_wifi_creds.configured, g_wifi_ap_only));
  std::printf("PASS forget_leaves_a_standalone_unit_standalone\n");
}

// Reconnect only rejoins the network already on file: it stays on
// api_auth_check, so the session still reaches it.
void test_reconnect_still_takes_the_session() {
  reset_device();
  Req s;
  s.cookie();
  CHECK(handle_wifi_reconnect_auth(&s.r) == ESP_OK);
  CHECK(s.r.status == "200 OK" && g_reconnects == 1);
  std::printf("PASS reconnect_still_takes_the_session\n");
}

// ── the reboot and the safe-mode retry (F361) ────────────────────────────────

// Both take the bearer alone: a session alone is 401 with no strike and the
// unit does not restart, a wrong bearer beside it is 403 and one strike, and
// the bearer restarts it once. The same during setup (the wrappers ask
// nothing about it, and a session can exist then: the receipt route mints
// one on a BOOT tap or the setup door's claim). Before F361 the session
// restarted the unit at will, and each restart emptied the lockout table.
void test_the_reboot_routes_take_the_bearer() {
  struct Route { const char* name; esp_err_t (*wrapper)(httpd_req_t*); int* ran; };
  const Route routes[] = {
      {"reboot", handle_reboot_auth, &g_reboots},
      {"safe-mode retry", handle_safe_mode_retry_auth, &g_safe_mode_retries},
  };
  for (const Route& rt : routes) {
    for (bool during_setup : {false, true}) {
      const std::string name = std::string(rt.name) + (during_setup ? " (setup)" : "");
      reset_device();
      setup_wizard::g_active = during_setup;
      Req s;
      s.cookie();
      CHECK(rt.wrapper(&s.r) == ESP_OK);
      CHECK_MSG(s.r.status == "401 Unauthorized" && has_header(s.r, "WWW-Authenticate"), name);
      CHECK_MSG(*rt.ran == 0 && strikes(kHostA) == 0, name);
      Req w;
      w.cookie().bearer(kWrong);
      CHECK(rt.wrapper(&w.r) == ESP_OK);
      CHECK_MSG(w.r.status == "403 Forbidden" && *rt.ran == 0, name);
      CHECK_MSG(strikes(kHostA) == 1, name);
      Req b;
      b.from(192, 168, 1, 21).bearer(kToken);
      CHECK(rt.wrapper(&b.r) == ESP_OK);
      CHECK_MSG(b.r.status == "200 OK" && *rt.ran == 1, name);
    }
  }
  std::printf("PASS the_reboot_routes_take_the_bearer\n");
}

// The setup door's composition of keyless_join_open() (the boot scan's rule
// too): open on a unit with no proven network that is not standalone.
void test_the_door_asks_the_keyless_join_rule() {
  struct Case { bool configured, pending, ap_only, open; };
  const Case cases[] = {
      {true, false, false, false},   // set up: shut
      {false, false, false, true},   // nothing on file: open
      {true, true, false, true},     // the door's own join, unproven: still open
      {false, false, true, false},   // standalone: shut
      {true, true, true, false},
  };
  for (const Case& c : cases) {
    reset_device();
    g_wifi_creds.configured = c.configured;
    g_improv_join_pending = c.pending;
    g_wifi_ap_only = c.ap_only;
    CHECK(ble_improv_door_should_open() == c.open);
    CHECK(ble_improv_door_should_open() ==
          provisioning_logic::keyless_join_open(c.configured && !c.pending, c.ap_only));
  }
  std::printf("PASS the_door_asks_the_keyless_join_rule\n");
}

// ── the sketch's text ────────────────────────────────────────────────────────

std::string read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// C and C++ comments out, string and character literals honored, newlines
// kept (check_route_security.py's strip_comments, in C++).
std::string strip_comments(const std::string& t) {
  std::string out;
  out.reserve(t.size());
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
        if (c == '\\' && i + 1 < t.size()) { out += t[++i]; }
        else if ((st == STR && c == '"') || (st == CHR && c == '\'')) st = CODE;
        break;
    }
  }
  return out;
}

// A function as cut_functions.awk cuts it: from the line that starts with
// `sig` to the first line that is exactly "}", its blank lines dropped and
// each line trimmed. Empty unless found exactly once.
std::vector<std::string> statements_of(const std::string& code, const std::string& sig) {
  std::vector<std::string> out;
  int found = 0;
  size_t at = 0;
  bool inside = false;
  while (at <= code.size()) {
    size_t end = code.find('\n', at);
    if (end == std::string::npos) end = code.size();
    const std::string line = code.substr(at, end - at);
    at = end + 1;
    if (line.compare(0, sig.size(), sig) == 0) {
      ++found;
      inside = true;
      continue;
    }
    if (!inside) continue;
    if (line == "}") { inside = false; continue; }
    size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos) continue;
    size_t e = line.find_last_not_of(" \t\r");
    out.push_back(line.substr(b, e - b + 1));
  }
  if (found != 1) out.clear();
  return out;
}

// The index of the first statement that holds `needle`, or -1.
int first_with(const std::vector<std::string>& st, const std::string& needle) {
  for (size_t i = 0; i < st.size(); ++i) if (st[i].find(needle) != std::string::npos) return (int)i;
  return -1;
}

void test_sketch_text() {
  const std::string code = strip_comments(read_file(CANARY_WAP_INO));
  CHECK(!code.empty());

  // POST /api/wifi/qr-scan answers only during setup (F334): its first
  // statement after the request count is the wizard check, whose block is
  // the 404, ahead of the boot scan's yield and the pair token's read.
  const auto scan = statements_of(code, "static esp_err_t handle_qr_scan_start(httpd_req_t* req) {");
  CHECK(scan.size() > 6);
  if (scan.size() > 6) {
    CHECK(scan[0] == "g_health.http_requests++;");
    CHECK(scan[1] == "if (!setup_wizard::is_active()) {");
    CHECK(scan[2] == "httpd_resp_set_status(req, \"404 Not Found\");");
    CHECK(scan[3] == "return http_send_json(req, \"{\\\"ok\\\":false}\");");
    CHECK(scan[4] == "}");
    const int yield = first_with(scan, "g_qr_auto_next_ms = millis() + 15000;");
    const int token = first_with(scan, "csi_integration::pair_token_valid(token)");
    CHECK(yield > 4 && token > yield);
  }

  // Connect and AP-only ask the gate before they write anything.
  for (const char* sig : {"static esp_err_t handle_wifi_connect(httpd_req_t* req) {",
                          "static esp_err_t handle_wifi_ap_only(httpd_req_t* req) {"}) {
    const auto st = statements_of(code, sig);
    const int gate = first_with(st, "switch (wifi_change_authorize(req, token)) {");
    CHECK_MSG(gate > 0, sig);
    for (const char* write : {"NvsMainSession", "WiFi.", "g_wifi_creds", "g_wifi_ap_only =",
                              "wifi_save_credentials(", "setup_wizard::mark_complete("}) {
      const int w = first_with(st, write);
      CHECK_MSG(w == -1 || w > gate, std::string(sig) + " writes before its gate: " + write);
    }
  }

  // The camera's boot scan asks the keyless-join rule first (F333; before,
  // it asked only about the stored network, so a standalone unit scanned).
  const auto tick = statements_of(code, "static void qr_auto_scan_tick(uint32_t now) {");
  CHECK(!tick.empty() &&
        tick[0] == "if (!provisioning_logic::keyless_join_open(g_wifi_creds.configured, g_wifi_ap_only)) return;");

  // And a window already running asks after every frame, before a code read
  // in that frame can act, whether it may still act: a boot window the
  // keyless-join rule (F333's review: the wizard's standalone choice, or any
  // stored network, used to leave the running window up to 60 s in which a
  // WIFI: code held to the lens took the unit off standalone or replaced the
  // network), a phone session's window whether setup still runs (the
  // owner-authority PR's second review: setup ending by another path, the
  // Bluetooth door's join or a second tab's standalone choice, left the
  // phone's window up to 60 s in which a code joined the unit to the network
  // it named, no bearer asked). provisioning_logic::scan_window_open() is
  // the one predicate; test_provisioning_logic holds its table.
  const auto task = statements_of(code, "static void qr_scan_task_fn(void* param) {");
  const int loop = first_with(task, "while (g_qr_scan_active) {");
  const int frame = first_with(task, "esp_camera_fb_return(fb);");
  CHECK(loop >= 0 && frame > loop && (size_t)frame + 4 < task.size());
  if (loop >= 0 && frame > loop && (size_t)frame + 4 < task.size()) {
    CHECK(task[frame + 1] == "if (!provisioning_logic::scan_window_open(g_qr_auto_scan, g_wifi_creds.configured,");
    CHECK(task[frame + 2] == "g_wifi_ap_only, setup_wizard::is_active())) {");
    CHECK(task[frame + 3] == "memset(payload, 0, sizeof(payload));");
    CHECK(task[frame + 4] == "break;");
    const int code_read = first_with(task, "if (result == 1) {");
    const int hub = first_with(task, "csi_mqtt::config_save(mc)");
    const int save = first_with(task, "wifi_save_credentials();");
    CHECK(code_read > frame + 4 && hub > code_read && save > hub);
  }

  // Forget clears the broker login after the networks, asks for the re-init
  // when the clear held, and says whether it did (F333).
  const auto forget = statements_of(code, "static esp_err_t handle_wifi_forget(httpd_req_t* req) {");
  const int wipe = first_with(forget, "wifi_clear_credentials();");
  const int clear = first_with(forget, "const bool login_cleared = csi_mqtt::login_clear();");
  const int when = first_with(forget, "if (login_cleared) {");
  const int reinit = first_with(forget, "(void)csi_mqtt::request_reinit();");
  const int says = first_with(forget, "doc[\"broker_login_cleared\"] = login_cleared;");
  CHECK(wipe >= 0 && clear > wipe && when == clear + 1 && reinit == when + 1 && says > reinit);

  // And Forget leaves the standalone choice where it was: neither Forget nor
  // the wipe it runs writes it, and the 10 s hold is what clears it
  // (factory_reset_by_hold()), so a standalone unit's keyless joins stay shut
  // after a Forget, as the dashboard's confirm and the FAQ say.
  const auto wipe_fn = statements_of(code, "static bool wifi_clear_credentials() {");
  CHECK(!forget.empty() && !wipe_fn.empty());
  for (const auto* body : {&forget, &wipe_fn}) {
    CHECK(first_with(*body, "g_wifi_ap_only") == -1);
    CHECK(first_with(*body, "NVS_KEY_WIFI_AP_ONLY") == -1);
  }
  const auto hold = statements_of(code, "static void factory_reset_by_hold() {");
  const int hold_wipe = first_with(hold, "wifi_clear_credentials();");
  CHECK(hold_wipe >= 0 &&
        first_with(hold, "if (nvs.isOpen()) nvs->remove(NVS_KEY_WIFI_AP_ONLY);") > hold_wipe &&
        first_with(hold, "g_wifi_ap_only = false;") > hold_wipe);

  // The six routes are registered on the handlers this suite holds.
  for (const char* reg : {
           "httpd_uri_t reboot = { .uri = \"/api/reboot\", .method = HTTP_POST, .handler = handle_reboot_auth };",
           "httpd_uri_t safe_retry = { .uri = \"/api/safe-mode/retry\", .method = HTTP_POST, .handler = handle_safe_mode_retry_auth };",
           "httpd_uri_t wifi_connect = { .uri = \"/api/wifi/connect\", .method = HTTP_POST, .handler = handle_wifi_connect };",
           "httpd_uri_t wifi_ap_only = { .uri = \"/api/wifi/ap-only\", .method = HTTP_POST, .handler = handle_wifi_ap_only };",
           "httpd_uri_t wifi_disconnect = { .uri = \"/api/wifi/disconnect\", .method = HTTP_POST, .handler = handle_wifi_disconnect_auth };",
           "httpd_uri_t wifi_forget = { .uri = \"/api/wifi/forget\", .method = HTTP_POST, .handler = handle_wifi_forget_auth };",
       }) {
    CHECK_MSG(code.find(reg) != std::string::npos, reg);
  }
  std::printf("PASS sketch_text\n");
}

}  // namespace

int main() {
  test_the_landing_grants_move_no_network();
  test_the_bearer_moves_the_network();
  test_a_hosts_wrong_bearers_hold_only_its_address();
  test_nine_striking_addresses_hold_the_owners_bearer();
  test_during_setup_the_wizard_gate_is_unchanged();
  test_forget_and_disconnect_take_the_bearer();
  test_forget_leaves_a_standalone_unit_standalone();
  test_reconnect_still_takes_the_session();
  test_the_reboot_routes_take_the_bearer();
  test_the_door_asks_the_keyless_join_rule();
  test_sketch_text();
  if (g_failures) {
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d post-setup authority checks PASSED\n", g_checks);
  return 0;
}
