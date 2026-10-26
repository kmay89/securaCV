// The provisioning receipt's and the pairing QR's grants on canary-wap (sweep
// F289): a cv_session cookie alone buys neither; a real bearer buys both; the
// BOOT tap and the setup-door claim still buy the receipt.
//
// Before, GET /api/provisioning-receipt's first grant was
// api_auth_check_optional(), which takes a valid cv_session cookie, and GET
// /api/pairing-qr was gated by api_auth_check(), which takes it too. The
// landing page at / mints that cookie's session for whoever loads it on the
// home network (deliberate LAN posture for the dashboard, the 2026-07 audit's
// "Trust-on-LAN"), so any host on the LAN could turn a page load into the
// receipt — the bearer token, the SoftAP password, the TLS pin — or the QR
// that carries the token, with no tap. Both routes now take as their bearer
// grant ONLY an `Authorization: Bearer <token>` header (the receipt through
// api_auth_bearer_grant, the QR through api_auth_check_bearer, both in
// api_auth.h), constant-time compared as every admin route's is; the receipt's other grants, the claim and the tap, are
// unchanged, and the dashboard's own routes still take the cookie.
//
// This runs the REAL api_auth.h and the handlers cut verbatim out of
// canary_wap.ino (handle_provisioning_receipt, handle_pairing_qr, the gate
// they spend, provisioning_gate_take, the receipt's Host question,
// request_host_names_device, and the cookie writer the receipt sends,
// session_cookie_header; firmware/tests_host/
// cut_functions.awk) over stubs/receipt_grant (a request with headers and a
// query string, a response the test reads) and stand-ins for what else they
// call: the receipt writer (recording the grant that admitted the request
// and writing the two secrets, so a refusal is checked to carry neither), the
// QR writer (recording its payload), the session mint, the claim ticket (the
// sketch's own claim_ticket.h), the health log, the Wi-Fi facts. What it
// pins:
//   - the receipt on a valid cookie alone, the tap shut and no claim: 403
//     physical_confirmation_required, nothing served, no secret in the
//     answer, no cookie issued (before F289: the receipt, as a bearer);
//   - on a cookie beside a wrong bearer: the same 403 (before: the receipt);
//   - on a bearer, with or without a cookie: the receipt as a bearer, the
//     tap left open and the claim left pending;
//   - on a cookie beside an open tap: the receipt as the TAP (it is the hand
//     on the button, not the cookie, that serves), the tap spent, and the
//     cookie alone refused on the next request;
//   - on a cookie beside a claim: the receipt as the claim, the tap unspent;
//   - a bare request on an open tap: the receipt and a Set-Cookie, as before;
//     the cookie Secure and named __Host-cv_session when the answer leaves
//     over the HTTPS server (the tap and the claim alike; the name is sweep
//     F337's), never on a plain-HTTP boot, where the first-boot SoftAP
//     setup's recovery-kit save mints cv_session (sweep F289's residual; the
//     writer, session_cookie_header(), is cut with the handlers, and
//     test_session_cookie.cpp drives handle_ui's mint);
//   - a wrong claim is refused and spends nothing (sweep F342; before, it
//     burned the phone's claim): the real claim then serves; a host that
//     keeps sending wrong claims gets one health line per 10 s; a header
//     that is not "Bearer " is no bearer;
//   - the BOOT tap goes to one asker (sweep F342; before, the first request
//     from any host inside the tap's 30 s took it): with the owner's page
//     asking from before the tap, a second address asking in the same
//     window gets 409 tap_contended (no cookie, no secret, a body that
//     parses, a hint that sends the owner's client in first and never
//     advises a bare re-tap), the owner's page gets 409 too, the tap is
//     closed and one health line, naming no address in its message or its
//     detail, says so; the 409 ends lookback after the other address's last
//     ask, so the owner's retry 10 s after the other device stopped asks
//     first (403) and its next tap is served; a re-tap served to anyone
//     ends the 409; a sole asker is served at
//     once; an attacker that asks first after the tap loses to an owner who
//     asked before it; a wrong bearer or a wrong claim from the second
//     address counts as an ask, a right bearer or a right claim does not;
//     the BLE OTA break-glass take leaves nothing to take, and a take or a
//     re-tap that lands between the tap step's judgment and its take
//     refuses the request (403, nothing served; the re-tap left for the next
//     ask), and a re-tap stored while a request that found no fresh stamp
//     sits between its peek and its burn is kept and serves the next ask
//     (the burn is take_exact() of the peeked stamp, never a close());
//     every 403 says to start asking first, then short-tap BOOT, in its hint
//     and its button, and never "short tap, then poll" (sweep F342's second
//     review: that order hands the tap to a host that watches /api/logs);
//     contention does
//     not carry into a clean next tap; a socket that cannot say is refused
//     with the tap left for the next asker; a held bearer still gets its
//     429 under contention; the 409 clears no backoff; an expired tap is
//     burned by the request that finds it;
//   - the Host first (sweep F344): request_host_names_device(), cut with
//     the handlers over the sketch's staged host_guard.h, is the receipt's
//     first statement. A Host that cannot name this device (evil.example, a
//     name that only contains canary.local, a 108-byte rebinding name too
//     long for the 96-byte Host buffer whose first 95 bytes end in ".local",
//     a 96-byte Host, a 92-byte .local name and a port, that host_guard.h
//     passes read whole but the buffer cannot hold, an empty or a blank
//     Host, no Host at all) gets 403
//     {"error":"host"}, no cookie, no secret: the tap stays open, a claim
//     presented right or wrong stays pending, no bearer is compared and no
//     strike recorded, a held address hears the Host refusal and keeps its
//     backoff, and no asker is recorded, so the owner asking next from
//     another address is served the tap, not 409 (before: the tap, the claim
//     and the bearer served it, and a wrong bearer was a strike). The
//     device's names (.local, an IP with or without a port, the bare label,
//     .lan; a 39-character label, a full IPv6 literal, the longest Host the
//     buffer holds) are served. Every request here carries Host canary-7916.local
//     unless a row names another;
//   - the tap and the claim do not come back when millis() wraps (sweep
//     F347; before, a tap nobody took served the receipt again 49.7 days
//     after the press, and a claim read but never spent took it with its
//     old hex): loop()'s two sweeps, run here as the sketch's loop() spells
//     them (each statement only when the sketch's loop() holds it, so a
//     sketch without the sweep runs none), clear both past their TTLs, and
//     2^32 ms + 1 s after them a bare request and the old ?claim= are both
//     the 403; loop() holds both sweeps at its top level, after
//     handle_serial_commands(), with no return before them; the BOOT short
//     press opens the gate through the header's open(), and no line touches
//     the gate's word directly;
//   - the receipt's bearer is under every admin route's lockout (F289's
//     follow-up; before, its compare recorded nothing, so the receipt was
//     the one route where guessing the token cost nothing): a wrong or
//     malformed header is one strike and still gets the 403, a right one
//     clears the count; while the backoff runs no bearer is compared, a
//     bearer gets the 429 with Retry-After and a bare poll the 403 its tap
//     wait reads; the receipt's guesses lock the QR and the QR's lock the
//     receipt; and the lockout never reaches the BOOT tap or the claim,
//     which serve a stale bearer, inside the backoff or out of it; a backoff
//     that ends while the request is in the handler (after the held read,
//     before the 429) still answers Retry-After 1, never a wrapped count;
//   - the tap or the claim that serves ends the asking address's backoff
//     (sweep F341; before, the client just served stayed held for up to
//     60 s): A's right bearer straight after the tap, the claim, a stale
//     bearer the tap served or a header-less tap wait is the receipt as
//     BEARER, and the QR is served; a stale bearer's strike is still counted
//     first; another host's slot (armed by a direct write, never by a
//     receipt ask, which is the tap's own question, F342) keeps its count
//     and its 429; an address held only through the shared overflow stays
//     held; the 403, the 429, a wrong claim and a lapsed tap clear nothing;
//   - the pairing QR on a cookie alone: 401 with the WWW-Authenticate
//     challenge, no SVG, no token in the answer, no lockout strike (before:
//     the SVG with the token); on a cookie beside a wrong bearer: 403 and a
//     strike (before: the SVG); on a bearer: the SVG with the token, the pin
//     and the https .local base on a TLS unit on the home Wi-Fi, the AP
//     address while in setup; five wrong guesses lock the right token out
//     (429 with Retry-After) until the backoff passes;
//   - the lockout is per source address (sweep F343; before, one backoff
//     for the device, so one host's wrong bearers held every other host's
//     right token at 429): host A's wrong bearer holds A and never B, on the
//     receipt and the QR; the F289 follow-up review's replay (A strikes
//     whenever it is not held, B polls every 7 s for ten minutes) refuses B
//     0 of 86 times, where it refused 86 of 86; a socket that cannot say and
//     a real IPv6 peer share one bucket, which holds neither A nor B (the
//     REAL auth_request_source(), cut from the sketch, over the suites' one
//     stand-in for the socket read, stubs/receipt_grant/peer_source.h); the
//     table is ONE for every file that includes api_auth.h (a strike through
//     this file's gates holds api_auth_second_tu.cpp's, and back; before,
//     each file had its own `static` copy); no backoff outlives the 60 s
//     window (the sixth strike's Retry-After is 61, where it was 65, and its
//     hold ends at 60 s); a backoff armed just before millis() wraps holds
//     across the wrap; 256 strikes, each once the backoff before it ran out,
//     saturate the count instead of wrapping it to a clean 0; two strikes a
//     minute apart are two first strikes;
//   - every bearer compare is accounted (sweep F339; before, these compared
//     a bearer and recorded nothing): api_auth_check_optional() is the cookie
//     or api_auth_bearer_grant(), so a wrong bearer through it is one strike
//     for its address and, inside that address's backoff, the right one is
//     not compared and reports `held`; the CSI routes' gate
//     (CSI_AUTH_OR_RETURN, cut from csi_integration.cpp) strikes a wrong
//     bearer and answers a held one 429 with Retry-After, B served
//     throughout; the QR scan's status gate (qr_scan_request_allowed(), cut
//     from the sketch) strikes a wrong admin bearer, refuses a held one, and
//     still takes the wizard's pair token from a held address;
//     api_auth_check_or_query() (the peek and vault ?token=) runs one grant
//     over the header or the ?token=: inside a backoff a right ?token= is 429
//     and clears nothing, a wrong one records nothing, 250 of them leave A
//     held at its own count, B's right ?token= is served, a wrong header
//     beside a wrong ?token= is one strike, a wrong header beside the right
//     ?token= is refused and one strike (the query is compared only when no
//     header is sent: one candidate a strike), the right header alone is
//     served and frees its address, and a cookie is served, records nothing
//     and never frees a held address (before: the right ?token= was served
//     inside the backoff and cleared it, the wrong ones were recorded during
//     it);
//   - api_auth.h's contract: api_auth_check() and api_auth_check_optional()
//     still take the cookie (the dashboard's routes), api_auth_bearer_valid()
//     and api_auth_check_bearer() never do, and the cookie path never clears
//     the lockout; an empty device token (provision_device() can stop before
//     minting it) matches no bearer and no ?token=, on every gate, the
//     receipt and the QR included;
//   - the two writes that grant a credential take the bearer alone (sweep
//     F335, F346; before, both wrappers were api_auth_check(), which the
//     cookie passes): POST /api/vault/key's (who can open every frame sealed
//     from then on) and POST /api/mesh/pair/confirm's (an opera's secret),
//     the wrappers cut from the sketch over counting stand-ins for the
//     handlers. The landing page's session alone is 401 and no strike;
//     beside a wrong bearer, 403 and one strike for that address; another
//     host's right bearer is served at once; the striking host's right
//     bearer is 429 with Retry-After until its backoff runs out, then served;
//   - the sketch's text (CANARY_WAP_INO, comments stripped): the two handlers
//     name only the bearer-only checks and never the cookie bridge; every
//     line that names g_device.api_token_str or g_device.ap_password is a
//     comparison, a hand-over to a module's own gate, key management, the
//     serial console or the boot banner, or sits inside
//     send_provisioning_receipt() or handle_pairing_qr() — the only two
//     writers of either secret into an HTTP answer — and the receipt writer
//     is called from handle_provisioning_receipt() alone; POST /api/vault/key
//     and POST /api/mesh/pair/confirm are registered once each, on the two
//     wrappers the grant test above drives.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run-receipt-grant

#include "arduino_globals.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Arduino.h"           // stubs/receipt_grant
#include "esp_http_server.h"   // stubs/receipt_grant
#include "host_guard.h"        // the sketch's staged copy: the receipt's Host question calls it
#include "log_level.h"         // the sketch's LogLevel / LogCategory
#include "receipt_authority.h"
#include "claim_ticket.h"
#include "provisioning_gate.h" // the sketch's staged copy: the peer key and its IPv4
#include "api_auth.h"          // the REAL one
#include "peer_source.h"       // stubs/receipt_grant: request_peer_key(), the socket read
#include "json_strict.h"       // the 409's body parses as a browser's JSON.parse() reads it

#ifndef RECEIPT_GRANT_INC
#error "RECEIPT_GRANT_INC must name the cut of canary_wap.ino (the Makefile passes it)"
#endif
#ifndef CANARY_WAP_INO
#error "CANARY_WAP_INO must name canary_wap.ino (the Makefile passes it)"
#endif
#ifndef RECEIPT_GRANT_CSI_INC
#error "RECEIPT_GRANT_CSI_INC must name the cut of csi_integration.cpp's CSI_AUTH_OR_RETURN (the Makefile passes it)"
#endif

// ── the test's own bookkeeping ───────────────────────────────────────────────
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

// ── the cookie bridge csi_integration.cpp supplies ───────────────────────────
// One live session, as the landing page's ?cv_pair exchange would have minted
// it; a request carries it as `Cookie: cv_session=<hex>`.
static std::string g_live_session;
static int g_cookie_checks = 0;
bool cv_session_validate(httpd_req_t* req) {
  ++g_cookie_checks;
  if (!req || g_live_session.empty()) return false;
  auto it = req->req_hdrs.find("Cookie");
  if (it == req->req_hdrs.end()) return false;
  const std::string want = "cv_session=" + g_live_session;
  return it->second.find(want) != std::string::npos;
}

// ── the sketch's globals the cut functions read ──────────────────────────────
static const uint32_t PROVISIONING_GATE_TTL_MS = 30000;
// The BOOT-tap gate and its one-asker record, as canary_wap.ino declares them
// (provisioning_gate.h's State and TapArbiter; sweep F342).
static canary::net::provisioning_gate::State g_provisioning_gate = {0};
static canary::net::provisioning_gate::TapArbiter g_tap_arbiter = {};

struct { uint32_t http_requests = 0; } g_health;
struct {
  char device_id[32];
  char mdns_hostname[40];
  char api_token_str[36];
  char ap_password[16];
} g_device;
static bool g_tls_enabled = false;
static char g_tls_cert_fp_hex[65] = {0};
// The servers, as start_http_server() makes them (session_cookie_header()
// reads the HTTPS one): nullptr on a plain-HTTP boot. Any two distinct
// addresses stand in for the handles.
static int g_http_server_obj = 0;
static int g_https_server_obj = 0;
static httpd_handle_t g_https_server = nullptr;
static canary::net::claim_ticket::Ticket g_improv_claim = {};
static uint32_t g_claim_refusal_logged_ms = 0;
static uint32_t g_improv_claim_spent_at_ms = 0;

static void secure_zero(void* p, size_t n) {
  volatile uint8_t* vp = (volatile uint8_t*)p;
  while (n--) *vp++ = 0;
}

// Each health line as /api/logs would serve it: the message, then " | " and
// the detail when there is one (the device's ring entry keeps both, and any
// session can read them), so a check that a line names no address holds
// both halves.
static std::vector<std::string> g_health_lines;
// Milliseconds each health line moves the fake clock (0 but in the one test
// that needs time to pass inside the handler, between its reads).
static uint32_t g_health_line_ms = 0;
void log_health(LogLevel, LogCategory, const char* message, const char* detail = nullptr) {
  std::string line = message ? message : "";
  if (detail && *detail) {
    line += " | ";
    line += detail;
  }
  g_health_lines.push_back(line);
  stub_grant::now_ms += g_health_line_ms;
}

namespace csi_integration {
// The wizard's pair token, as qr_scan_request_allowed() asks for it: one live
// token, 64 hex characters, or none.
constexpr size_t PAIR_TOKEN_HEX_LEN = 64;
static std::string g_pair_token;
bool pair_token_valid(const char* hex) {
  return hex != nullptr && !g_pair_token.empty() && g_pair_token == hex;
}
constexpr size_t SESSION_COOKIE_HEX_LEN = 64;
static int g_sessions_issued = 0;
bool session_issue(char* hex_out, size_t out_cap) {
  if (!hex_out || out_cap < SESSION_COOKIE_HEX_LEN + 1) return false;
  ++g_sessions_issued;
  for (size_t i = 0; i < SESSION_COOKIE_HEX_LEN; ++i) hex_out[i] = "0123456789abcdef"[(i + g_sessions_issued) % 16];
  hex_out[SESSION_COOKIE_HEX_LEN] = '\0';
  return true;
}
}  // namespace csi_integration

// WiFi, as handle_pairing_qr reads it.
struct FakeIp {
  std::string s;
  const FakeIp& toString() const { return *this; }
  const char* c_str() const { return s.c_str(); }
};
struct FakeWiFi {
  bool connected = false;
  std::string ap_ip = "192.168.4.1";
  bool isConnected() const { return connected; }
  FakeIp softAPIP() const { return FakeIp{ap_ip}; }
};
static FakeWiFi WiFi;

// ── the two writers, standing in: what was served, on which grant ───────────
static std::vector<receipt_authority::Grant> g_served;
static esp_err_t send_provisioning_receipt(httpd_req_t* req, receipt_authority::Grant grant) {
  g_served.push_back(grant);
  std::string body = "{\"token\":\"";
  body += g_device.api_token_str;
  body += "\",\"ap_password\":\"";
  body += g_device.ap_password;
  body += "\"}";
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, body.c_str());
}

static std::vector<std::string> g_qr_payloads;
static esp_err_t send_qr_svg(httpd_req_t* req, const char* payload) {
  g_qr_payloads.push_back(payload ? payload : "");
  httpd_resp_set_type(req, "image/svg+xml");
  std::string body = "<svg>";
  body += payload ? payload : "";
  body += "</svg>";
  return httpd_resp_sendstr(req, body.c_str());
}

// The two credential grants behind a bearer-only wrapper (sweep F335, F346):
// the wrappers are cut from the sketch, the handlers they guard stand in.
static int g_vault_key_sets = 0;
static esp_err_t handle_vault_key_set(httpd_req_t* req) {
  ++g_vault_key_sets;
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}
static int g_mesh_confirms = 0;
static esp_err_t handle_mesh_pair_confirm(httpd_req_t* req) {
  ++g_mesh_confirms;
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// ── the handlers, cut verbatim out of canary_wap.ino ─────────────────────────
// With them, auth_request_source(): the lockout's source, the real mapping
// over request_peer_key() (peer_source.h).
#include RECEIPT_GRANT_INC

// api_auth_check_bearer(), as another file that includes api_auth.h calls it
// (api_auth_second_tu.cpp, linked into this binary).
bool second_tu_check_bearer(httpd_req_t* r, const char* t);

// ── the CSI routes' gate, cut verbatim out of csi_integration.cpp ───────────
// CSI_AUTH_OR_RETURN (sweep F339) over the module's copy of the token, as
// csi_integration::init() keeps it, and a route behind it the way every
// /api/csi/* handler starts.
static const char* g_api_token = nullptr;
#include RECEIPT_GRANT_CSI_INC
static esp_err_t csi_route(httpd_req_t* req) {
  CSI_AUTH_OR_RETURN(req);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

namespace {

const char* const kToken = "cv_7kX9mN2pQ4rS6tU8vW0xY1zA3bC5dE7f";   // 35 chars, "cv_" + 32
const char* const kWrong = "cv_7kX9mN2pQ4rS6tU8vW0xY1zA3bC5dE7g";   // one char off
const char* const kApPass = "cv-9f8e7d6c5b4a";
const char* const kClaimHex = "ffeeddccbbaa99887766554433221100";   // not the one mint_claim() mints

// ── the lockout's state, per source address (sweep F343) ─────────────────────
// api_auth.h's one table, read and written here only, by the source the cut
// auth_request_source() maps each request's peer to.
const uint32_t kHostA = 0xC0A80114;   // 192.168.1.20, the stub's default peer
const uint32_t kHostB = 0xC0A80115;   // 192.168.1.21: Req::from(192, 168, 1, 21)

void clear_lockout() { g_auth_table = {}; }
// The strikes `host`'s own slot holds now.
unsigned strikes(uint32_t host) {
  return canary::net::auth_backoff::failures_of(g_auth_table, host, millis());
}
// Whether `host` is held now.
bool held(uint32_t host) { return canary::net::auth_backoff::locked(g_auth_table, host, millis()); }
// `host`'s slot written directly: `failures` strikes, the last at `last_ms`,
// its backoff running until `until_ms`.
void set_strikes(uint32_t host, uint8_t failures, uint32_t last_ms, uint32_t until_ms) {
  canary::net::auth_backoff::Slot* s = canary::net::auth_backoff::tracked(g_auth_table, host);
  for (uint8_t i = 0; i < canary::net::auth_backoff::SLOTS && !s; ++i) {
    if (g_auth_table.slot[i].failures == 0) s = &g_auth_table.slot[i];
  }
  *s = canary::net::auth_backoff::Slot{host, last_ms, until_ms, failures};
}
// The clock moved to the end of `host`'s backoff, as a client that waits out
// its Retry-After.
void wait_out(uint32_t host) {
  stub_grant::now_ms += canary::net::auth_backoff::remaining_ms(g_auth_table, host, millis());
}
// What the 429 for `req` reads as left of its source's backoff.
uint32_t remaining_for(httpd_req_t* req) { return auth_lockout_remaining_ms(req); }

void reset_device() {
  std::snprintf(g_device.device_id, sizeof(g_device.device_id), "%s", "canary-wap-7916");
  std::snprintf(g_device.mdns_hostname, sizeof(g_device.mdns_hostname), "%s", "canary-7916");
  std::snprintf(g_device.api_token_str, sizeof(g_device.api_token_str), "%s", kToken);
  std::snprintf(g_device.ap_password, sizeof(g_device.ap_password), "%s", kApPass);
  g_tls_enabled = false;
  g_tls_cert_fp_hex[0] = '\0';
  g_https_server = nullptr;
  WiFi = FakeWiFi{};
  canary::net::provisioning_gate::close(g_provisioning_gate);
  g_tap_arbiter = {};
  canary::net::claim_ticket::wipe(g_improv_claim);
  g_claim_refusal_logged_ms = 0;
  g_improv_claim_spent_at_ms = 0;
  g_served.clear();
  g_qr_payloads.clear();
  g_health_lines.clear();
  g_health_line_ms = 0;
  stub_grant::millis_hook = nullptr;
  g_live_session = "";
  g_cookie_checks = 0;
  csi_integration::g_pair_token.clear();
  g_api_token = kToken;
  clear_lockout();
  stub_grant::now_ms = 100000;
}

// A session the landing page's exchange minted, for the browser that holds it.
void mint_session() {
  g_live_session.assign(64, 'a');
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
  Req& authorization(const std::string& raw) {
    r.req_hdrs["Authorization"] = raw;
    return *this;
  }
  Req& query(const std::string& q) {
    r.query = q;
    return *this;
  }
  // The server the request arrived on (default: the plain one).
  Req& on(httpd_handle_t server) {
    r.handle = server;
    return *this;
  }
  // The peer it came from (default ::ffff:192.168.1.20, kHostA): an IPv4
  // client of the dual-stack listener, ::ffff:a.b.c.d.
  Req& from(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, a, b, c, d};
    std::memcpy(r.peer, mapped, sizeof(mapped));
    return *this;
  }
  // A real IPv6 peer.
  Req& from6(const uint8_t (&v6)[16]) {
    std::memcpy(r.peer, v6, sizeof(v6));
    return *this;
  }
  // A socket that cannot say who it is.
  Req& unreadable() {
    r.peer_ok = false;
    return *this;
  }
  // The Host the request targeted (default: the device's .local name, as
  // the dashboard, the companion and the apps reach it); nullptr sends none.
  // Separate from the peer: the receipt asks the Host first (sweep F344),
  // the tap's arbiter and the lockout read the socket's peer.
  Req& host(const char* h) {
    if (h) {
      r.req_hdrs["Host"] = h;
    } else {
      r.req_hdrs.erase("Host");
    }
    return *this;
  }
  Req() {
    r.handle = &g_http_server_obj;
    r.req_hdrs["Host"] = "canary-7916.local";
  }
};

// The owner's short press, as the sketch's button handler makes it.
void open_tap() { canary::net::provisioning_gate::open(g_provisioning_gate, millis()); }
bool tap_open() {
  return canary::net::provisioning_gate::is_open(g_provisioning_gate, millis(), PROVISIONING_GATE_TTL_MS);
}

void mint_claim() {
  uint8_t raw[canary::net::claim_ticket::RAW_LEN];
  for (size_t i = 0; i < sizeof(raw); ++i) raw[i] = (uint8_t)(i * 0x11);
  canary::net::claim_ticket::mint(g_improv_claim, raw, millis());
}
bool claim_pending() { return canary::net::claim_ticket::pending(g_improv_claim, millis()); }

bool has_header(const httpd_req_t& r, const char* name) {
  for (const auto& h : r.resp_hdrs) if (h.first == name) return true;
  return false;
}
std::string header(const httpd_req_t& r, const char* name) {
  for (const auto& h : r.resp_hdrs) if (h.first == name) return h.second;
  return "";
}
bool carries_secret(const httpd_req_t& r) {
  return r.resp.find(kToken) != std::string::npos || r.resp.find(kApPass) != std::string::npos;
}

// The receipt on `req`: the number of receipts served by this call.
size_t receipt(Req& q) {
  const size_t before = g_served.size();
  CHECK(handle_provisioning_receipt(&q.r) == ESP_OK);
  CHECK(q.r.sends == 1);
  return g_served.size() - before;
}

// The answer when a second address asked inside the tap's window (sweep
// F342): 409 tap_contended, a JSON body a browser parses, no cookie, no
// secret, not cached.
void check_tap_contended(const httpd_req_t& r) {
  CHECK_MSG(r.status == "409 Conflict", "the contended tap answered " + r.status);
  CHECK(r.type == "application/json");
  CHECK(header(r, "Cache-Control") == "no-store");
  json_strict::Value v;
  CHECK_MSG(json_strict::parse(r.resp, &v), "the 409's body does not parse: " + r.resp);
  const json_strict::Value* err = v.get("error");
  CHECK(err && err->kind == json_strict::Value::String && err->text == "tap_contended");
  const json_strict::Value* hint = v.get("hint");
  // The hint sends the owner's client in first (sweep F342's review): the
  // device sends this answer only while another address is asking, and a
  // bare re-tap then goes to that address if it is the one still asking.
  CHECK(hint && hint->text.find("start your request first and keep it asking, then press BOOT") !=
                    std::string::npos);
  CHECK_MSG(hint && hint->text.find("tap BOOT again") == std::string::npos &&
                hint->text.find("wait 10 seconds") == std::string::npos,
            "the 409's hint advises a bare re-tap: " + (hint ? hint->text : std::string()));
  CHECK(!carries_secret(r));
  CHECK(!has_header(r, "Set-Cookie"));
}

// The answer when nothing admitted the request. Its words give the order the
// arbiter protects (sweep F342's second review): the client asks first and
// the press comes second, because a tap no client of the owner's is already
// asking for goes to the first asker, and a host that watches /api/logs for
// "Provisioning gate opened" is that asker. It used to say "short tap, then
// poll within 30 seconds", which leads a curl user or a client into the
// residual. The fields stay: error, hint, button, gate_ttl_seconds (the
// dashboard and the Vision SPA read gate_ttl_seconds).
void check_gate_refusal(const httpd_req_t& r) {
  CHECK(r.status == "403 Forbidden");
  CHECK(r.resp.find("\"error\":\"physical_confirmation_required\"") != std::string::npos);
  CHECK(r.resp.find("\"gate_ttl_seconds\":30") != std::string::npos);
  json_strict::Value v;
  CHECK_MSG(json_strict::parse(r.resp, &v), "the 403's body does not parse: " + r.resp);
  const json_strict::Value* hint = v.get("hint");
  const json_strict::Value* button = v.get("button");
  const json_strict::Value* ttl = v.get("gate_ttl_seconds");
  CHECK(ttl && ttl->kind == json_strict::Value::Number);
  CHECK_MSG(hint && hint->kind == json_strict::Value::String &&
                hint->text.find("Start asking for the receipt first, then short-tap BOOT") != std::string::npos,
            "the 403's hint does not put the ask before the tap: " + r.resp);
  CHECK_MSG(button && button->kind == json_strict::Value::String &&
                button->text.find("ask first, then a short tap") != std::string::npos,
            "the 403's button does not put the ask before the tap: " + r.resp);
  CHECK_MSG(r.resp.find("short tap, then poll") == std::string::npos,
            "the 403 says to tap first and poll after: " + r.resp);
  CHECK(!carries_secret(r));
  CHECK(!has_header(r, "Set-Cookie"));
}

// ── the receipt ──────────────────────────────────────────────────────────────

void test_receipt_cookie_alone_is_refused() {
  reset_device();
  mint_session();
  // The landing page's session, the tap shut, no claim: the F289 case.
  Req q;
  q.cookie();
  CHECK(cv_session_validate(&q.r));   // the bridge does take this cookie...
  const size_t served = receipt(q);
  CHECK_MSG(served == 0, "a cookie alone served the receipt");   // ...and the route does not
  check_gate_refusal(q.r);
  CHECK(csi_integration::g_sessions_issued == 0 || !has_header(q.r, "Set-Cookie"));
  CHECK(g_served.empty());
  // The refusal is the one every credential-less caller gets, so the
  // dashboard's secureFetch brings its sign-in back on it.
  CHECK(q.r.status == "403 Forbidden");
}

void test_receipt_cookie_beside_wrong_bearer_is_refused() {
  reset_device();
  mint_session();
  Req q;
  q.cookie().bearer(kWrong);
  CHECK_MSG(receipt(q) == 0, "a cookie rescued a wrong bearer");
  check_gate_refusal(q.r);
}

void test_receipt_bearer_serves_and_spends_nothing() {
  reset_device();
  mint_session();
  open_tap();
  mint_claim();
  for (int with_cookie = 0; with_cookie < 2; ++with_cookie) {
    g_served.clear();
    Req q;
    q.bearer(kToken);
    if (with_cookie) q.cookie();
    CHECK(receipt(q) == 1);
    CHECK(g_served.size() == 1 && g_served[0] == receipt_authority::Grant::BEARER);
    CHECK(q.r.status == "200 OK");
    CHECK(carries_secret(q.r));
    CHECK_MSG(tap_open(), "a bearer receipt spent the owner's tap");
    CHECK_MSG(claim_pending(), "a bearer receipt spent the setup door's claim");
    CHECK(!has_header(q.r, "Set-Cookie"));   // a bearer caller needs no session
  }
}

void test_receipt_cookie_beside_tap_is_the_tap() {
  reset_device();
  mint_session();
  open_tap();
  Req q;
  q.cookie();
  CHECK(receipt(q) == 1);
  CHECK(g_served.size() == 1 && g_served[0] == receipt_authority::Grant::BOOT_TAP);
  CHECK_MSG(!tap_open(), "the tap that served was not spent");
  CHECK(has_header(q.r, "Set-Cookie"));
  CHECK(header(q.r, "Set-Cookie").find("cv_session=") == 0);
  CHECK(header(q.r, "Set-Cookie").find("HttpOnly") != std::string::npos);
  // The same cookie again, the tap spent: refused.
  Req again;
  again.cookie();
  CHECK_MSG(receipt(again) == 0, "the cookie served a second receipt after the tap was spent");
  check_gate_refusal(again.r);
}

void test_receipt_cookie_beside_claim_is_the_claim() {
  reset_device();
  mint_session();
  mint_claim();
  open_tap();
  Req q;
  q.cookie().query(std::string("claim=") + g_improv_claim.hex);
  CHECK(receipt(q) == 1);
  CHECK(g_served.size() == 1 && g_served[0] == receipt_authority::Grant::SETUP_DOOR_CLAIM);
  CHECK_MSG(tap_open(), "a claim that served spent the tap");
  CHECK(!claim_pending());
  CHECK(g_improv_claim_spent_at_ms != 0);
  CHECK(has_header(q.r, "Set-Cookie"));
}

void test_receipt_bare_tap_unchanged() {
  reset_device();
  open_tap();
  Req q;
  CHECK(receipt(q) == 1);
  CHECK(!g_served.empty() && g_served[0] == receipt_authority::Grant::BOOT_TAP);
  CHECK(has_header(q.r, "Set-Cookie"));
  CHECK(!tap_open());
  // A tap older than its TTL is no tap.
  open_tap();
  stub_grant::now_ms += PROVISIONING_GATE_TTL_MS;
  Req late;
  CHECK(receipt(late) == 0);
  check_gate_refusal(late.r);
  // Nothing at all.
  Req none;
  CHECK(receipt(none) == 0);
  check_gate_refusal(none.r);
}

// The receipt's session cookie: Secure exactly when this answer leaves over
// the HTTPS server (sweep F289's residual). Before, it never was. And named
// by the same transport (sweep F337): __Host-cv_session over HTTPS,
// cv_session over plain HTTP. The name checks are anchored at the start:
// the cookie bridge above matches find("cv_session=" + live), which also
// matches inside the prefixed name, so only these rows tell the two apart.
void test_receipt_cookie_secure_over_https_only() {
  const std::string attrs = "; HttpOnly; SameSite=Strict; Path=/; Max-Age=86400";
  // The first-boot SoftAP setup runs HTTP-only (no HTTPS server): the
  // recovery-kit save's tap mints the cookie without Secure, which an http
  // page could not set.
  reset_device();
  open_tap();
  Req http;
  CHECK(receipt(http) == 1);
  const std::string plain = header(http.r, "Set-Cookie");
  CHECK(plain.find("cv_session=") == 0);
  CHECK_MSG(plain.size() > attrs.size() && plain.compare(plain.size() - attrs.size(), attrs.size(), attrs) == 0,
            "the plain-HTTP cookie was '" + plain + "'");
  CHECK(plain.find("Secure") == std::string::npos);
  // HTTPS up, the tap over it: Secure, after every attribute it had.
  reset_device();
  g_https_server = &g_https_server_obj;
  open_tap();
  Req tap;
  tap.on(g_https_server);
  CHECK(receipt(tap) == 1);
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
  const std::string tls = header(tap.r, "Set-Cookie");
  CHECK_MSG(tls.size() > 8 && tls.find(attrs + "; Secure") != std::string::npos &&
                tls.compare(tls.size() - 8, 8, "; Secure") == 0,
            "the HTTPS tap's cookie was '" + tls + "'");
  CHECK_MSG(tls.find("__Host-cv_session=") == 0, "the HTTPS tap's cookie was '" + tls + "', not __Host-cv_session");
  // The setup-door claim over it: the same cookie.
  reset_device();
  g_https_server = &g_https_server_obj;
  mint_claim();
  Req claim;
  claim.on(g_https_server).query(std::string("claim=") + g_improv_claim.hex);
  CHECK(receipt(claim) == 1);
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::SETUP_DOOR_CLAIM);
  CHECK(header(claim.r, "Set-Cookie").find(attrs + "; Secure") != std::string::npos);
  CHECK_MSG(header(claim.r, "Set-Cookie").find("__Host-cv_session=") == 0,
            "the HTTPS claim's cookie was '" + header(claim.r, "Set-Cookie") + "', not __Host-cv_session");
  // HTTPS up, but this answer on the plain server: no Secure.
  reset_device();
  g_https_server = &g_https_server_obj;
  open_tap();
  Req port80;
  CHECK(receipt(port80) == 1);
  CHECK(header(port80.r, "Set-Cookie").find("Secure") == std::string::npos);
  CHECK(header(port80.r, "Set-Cookie").find("cv_session=") == 0);
  // A refusal over HTTPS sets no cookie at all.
  reset_device();
  g_https_server = &g_https_server_obj;
  Req refused;
  refused.on(g_https_server);
  CHECK(receipt(refused) == 0);
  check_gate_refusal(refused.r);
}

// How many "setup-door claim did not match" health lines were written.
size_t claim_refusal_lines() {
  size_t n = 0;
  for (const auto& l : g_health_lines) if (l.find("claim did not match") != std::string::npos) ++n;
  return n;
}

// A wrong claim is refused and spends nothing (sweep F342). Before, it
// burned the phone's pending claim: one stray or hostile ?claim= from any
// host on the network, and the phone's own spend a moment later was 403,
// which the app treats as final ("on the Wi-Fi, not paired").
void test_receipt_wrong_claim_leaves_it_pending() {
  reset_device();
  mint_session();
  mint_claim();
  Req q;
  q.cookie().query(std::string("claim=") + kClaimHex);
  CHECK(receipt(q) == 0);
  check_gate_refusal(q.r);
  CHECK_MSG(claim_pending(), "a wrong claim spent the real one");
  CHECK(claim_refusal_lines() == 1);
  for (const auto& l : g_health_lines) {
    CHECK_MSG(l.find("claim spent") == std::string::npos, "the refusal still says the claim was spent: " + l);
  }
  // A truncated claim (longer than the handler's buffer) and a short one:
  // refused, the claim still pending.
  Req long_one;
  long_one.from(192, 168, 1, 21).query(std::string("claim=") + std::string(80, 'a'));
  CHECK(receipt(long_one) == 0);
  check_gate_refusal(long_one.r);
  Req short_one;
  short_one.from(192, 168, 1, 21).query("claim=abc");
  CHECK(receipt(short_one) == 0);
  CHECK_MSG(claim_pending(), "a truncated or short claim spent the real one");
  CHECK(g_improv_claim_spent_at_ms == 0);   // the reboot runway is not armed by a refusal
  // The phone's own spend then serves.
  Req phone;
  phone.query(std::string("claim=") + g_improv_claim.hex);
  CHECK_MSG(receipt(phone) == 1, "the phone's claim was refused after a wrong one");
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::SETUP_DOOR_CLAIM);
  CHECK(!claim_pending());
  CHECK(g_improv_claim_spent_at_ms != 0);
}

// The refusal's health line is written at most once per 10 s, so a host
// that keeps sending wrong claims cannot fill the health log.
void test_wrong_claims_log_once_per_ten_seconds() {
  reset_device();
  mint_claim();
  Req first;
  first.query(std::string("claim=") + kClaimHex);
  CHECK(receipt(first) == 0);
  stub_grant::now_ms += 9999;
  Req second;
  second.query(std::string("claim=") + kClaimHex);
  CHECK(receipt(second) == 0);
  CHECK_MSG(claim_refusal_lines() == 1, "two wrong claims inside 10 s wrote " +
                                            std::to_string(claim_refusal_lines()) + " lines");
  stub_grant::now_ms += 1;
  Req third;
  third.query(std::string("claim=") + kClaimHex);
  CHECK(receipt(third) == 0);
  CHECK_MSG(claim_refusal_lines() == 2, "a wrong claim 10 s after the logged one wrote no line");
  CHECK(claim_pending());
  // A clock of exactly 0 is still a logged time (stamp() stores it as 1),
  // so the next wrong claim inside 10 s of it writes no line.
  reset_device();
  mint_claim();
  stub_grant::now_ms = 0;
  Req at_zero;
  at_zero.query(std::string("claim=") + kClaimHex);
  CHECK(receipt(at_zero) == 0);
  stub_grant::now_ms = 5000;
  Req after_zero;
  after_zero.query(std::string("claim=") + kClaimHex);
  CHECK(receipt(after_zero) == 0);
  CHECK(claim_refusal_lines() == 1);
}

void test_receipt_bearer_shapes() {
  // A lowercase scheme, a Basic header, an empty bearer, a header too long
  // for the buffer: no bearer, and each one strike (the shapes a guesser
  // sends; api_auth_check_bearer counts them the same). Each from a clean
  // count, so none arrives inside the backoff the one before armed.
  for (const std::string& raw : {std::string("bearer ") + kToken, std::string("Basic ") + kToken, std::string("Bearer "),
                                std::string("Token ") + kToken, std::string("Bearer ") + std::string(200, 'x')}) {
    reset_device();
    Req q;
    q.authorization(raw);
    CHECK_MSG(receipt(q) == 0, "accepted as a bearer: " + raw);
    check_gate_refusal(q.r);
    CHECK_MSG(strikes(kHostA) == 1, "not a strike: " + raw);
  }
  // Whitespace around the token is trimmed, as api_auth_check trims it.
  reset_device();
  Req padded;
  padded.authorization(std::string("Bearer   ") + kToken + "  ");
  CHECK(receipt(padded) == 1);
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BEARER);
}

// A backoff running now for host A: `failures` strikes, the last one just
// sent, 8 s left.
void arm_lockout(uint8_t failures) { set_strikes(kHostA, failures, millis(), millis() + 8000); }

void check_lockout_refusal(const httpd_req_t& r) {
  CHECK(r.status == "429 Too Many Requests");
  CHECK(!header(r, "Retry-After").empty());
  CHECK(r.resp.find("\"error\":\"too_many_requests\"") != std::string::npos);
  CHECK(!carries_secret(r));
  CHECK(!has_header(r, "Set-Cookie"));
}

// F289's follow-up: before, the receipt's bearer compare recorded nothing,
// so a guesser could try tokens here at full speed while every other route
// that reads the bearer backed them off.
void test_receipt_wrong_bearer_is_a_strike() {
  reset_device();
  Req wrong;
  wrong.bearer(kWrong);
  CHECK(receipt(wrong) == 0);
  check_gate_refusal(wrong.r);   // the guess that arms the backoff: the 403 any wrong token gets
  CHECK_MSG(strikes(kHostA) == 1, "a wrong bearer on the receipt was not a strike");
  // A request with no Authorization header carries no guess: no strike,
  // the cookie beside it or not (auth_logic.h).
  reset_device();
  mint_session();
  Req bare;
  CHECK(receipt(bare) == 0);
  Req cookie_only;
  cookie_only.cookie();
  CHECK(receipt(cookie_only) == 0);
  CHECK(strikes(kHostA) == 0);
  CHECK(g_auth_table.overflow.failures == 0);
}

void test_receipt_right_bearer_clears_the_count() {
  reset_device();
  set_strikes(kHostA, 2, millis(), millis());   // the backoff just passed
  Req right;
  right.bearer(kToken);
  CHECK(receipt(right) == 1);
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BEARER);
  CHECK_MSG(strikes(kHostA) == 0, "a right bearer on the receipt left the count standing");
}

void test_receipt_lockout_holds_the_bearer() {
  // The backoff running: no bearer is compared. The right token gets the
  // lockout's 429, as on the QR and every admin route; so does a guess,
  // which records nothing more (api_auth_check_bearer counts none inside
  // the backoff either).
  reset_device();
  arm_lockout(3);
  Req right;
  right.bearer(kToken);
  CHECK_MSG(receipt(right) == 0, "the receipt compared a bearer inside the backoff");
  check_lockout_refusal(right.r);
  Req wrong;
  wrong.bearer(kWrong);
  CHECK(receipt(wrong) == 0);
  check_lockout_refusal(wrong.r);
  CHECK(strikes(kHostA) == 3);
  // A poll with no bearer is not held: the 403 with gate_ttl_seconds the
  // dashboard's BOOT-tap wait reads, the cookie beside it or not.
  mint_session();
  Req bare;
  CHECK(receipt(bare) == 0);
  check_gate_refusal(bare.r);
  Req cookie_only;
  cookie_only.cookie();
  CHECK(receipt(cookie_only) == 0);
  check_gate_refusal(cookie_only.r);
  CHECK(strikes(kHostA) == 3);
  // The backoff passes: the right token serves again, and clears the count.
  stub_grant::now_ms += 8001;
  Req later;
  later.bearer(kToken);
  CHECK(receipt(later) == 1);
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BEARER);
  CHECK(strikes(kHostA) == 0);
}

void test_receipt_arming_guess_then_backoff() {
  reset_device();
  Req first;
  first.bearer(kWrong);
  CHECK(receipt(first) == 0);
  check_gate_refusal(first.r);   // 403: this guess armed the backoff
  CHECK(strikes(kHostA) == 1);
  Req second;
  second.bearer(kWrong);
  CHECK(receipt(second) == 0);
  check_lockout_refusal(second.r);   // inside it: 429, not compared, no strike
  CHECK(strikes(kHostA) == 1);
  stub_grant::now_ms += canary::net::auth_backoff::BASE_MS + 1;
  Req third;
  third.bearer(kWrong);
  CHECK(receipt(third) == 0);
  check_gate_refusal(third.r);   // after it: compared, a strike, the 403
  CHECK(strikes(kHostA) == 2);
}

// A's right bearer on the receipt, sent at once: served as the bearer, the
// tap left shut. The check every hand-over case below ends with.
void check_right_bearer_served_at_once(const char* after) {
  Req right;
  right.bearer(kToken);
  CHECK_MSG(receipt(right) == 1,
            std::string("A's right bearer was held right after ") + after + " (no clear on the hand-over)");
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BEARER);
  CHECK(right.r.status != "429 Too Many Requests");
}

void test_receipt_lockout_never_holds_the_tap_or_the_claim() {
  // A page holding a stale token, the backoff running (a guesser on the
  // network, or the page's own earlier tries): the owner's tap serves it,
  // and the tap ends that address's backoff (sweep F341): the page just got
  // the token, so its next right-token call is served at once. Before F341
  // the count stayed at 5 and that call was 429 for the rest of the backoff.
  reset_device();
  arm_lockout(5);
  open_tap();
  Req stale;
  stale.bearer(kWrong);
  CHECK_MSG(receipt(stale) == 1, "the lockout held the owner's BOOT tap");
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
  CHECK(!tap_open());
  CHECK(has_header(stale.r, "Set-Cookie"));
  CHECK_MSG(strikes(kHostA) == 0, "the tap left the address it served its count");
  CHECK(!held(kHostA));
  check_right_bearer_served_at_once("the tap");
  // The right token inside the backoff, a tap open: served, as the tap (no
  // bearer is compared while it runs), and the address is cleared.
  reset_device();
  arm_lockout(5);
  open_tap();
  Req right;
  right.bearer(kToken);
  CHECK(receipt(right) == 1);
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
  CHECK(strikes(kHostA) == 0);
  check_right_bearer_served_at_once("a held right bearer the tap served");
  // The dashboard's own wait: no header at all (requestFromDevice), the tap
  // open, the address held by its earlier mistyped token. Served, cleared.
  reset_device();
  arm_lockout(4);
  open_tap();
  Req bare;
  CHECK(receipt(bare) == 1);
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
  CHECK(strikes(kHostA) == 0);
  check_right_bearer_served_at_once("a header-less tap wait");
  // The setup-door claim, a stale bearer beside it, the backoff running:
  // served, and the phone's address is cleared the same way.
  reset_device();
  arm_lockout(5);
  mint_claim();
  Req claim;
  claim.bearer(kWrong).query(std::string("claim=") + g_improv_claim.hex);
  CHECK_MSG(receipt(claim) == 1, "the lockout held the setup door's claim");
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::SETUP_DOOR_CLAIM);
  CHECK(!claim_pending());
  CHECK_MSG(strikes(kHostA) == 0, "the claim left the address it served its count");
  check_right_bearer_served_at_once("the claim");
  // Outside a backoff: the stale bearer is a strike (the grant counts it
  // before the tap is read: one rule for every route), the tap serves it,
  // and the hand-over then clears the strike it cost. Before F341 the count
  // stayed at 1 and A's right bearer straight after was 429.
  reset_device();
  open_tap();
  Req fresh;
  fresh.bearer(kWrong);
  CHECK_MSG(receipt(fresh) == 1, "a stale bearer kept the tap from serving");
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
  CHECK(strikes(kHostA) == 0);
  check_right_bearer_served_at_once("a stale bearer the tap served");
  // The QR shares the table: right after the tap, A's right token on it is
  // served too.
  reset_device();
  arm_lockout(5);
  open_tap();
  Req tapped;
  CHECK(receipt(tapped) == 1);
  Req qr_right;
  qr_right.bearer(kToken);
  CHECK(handle_pairing_qr(&qr_right.r) == ESP_OK);
  CHECK_MSG(qr_right.r.status != "429 Too Many Requests", "the QR held A right after the tap");
  CHECK(g_qr_payloads.size() == 1);
}

// The hand-over clears the address it served and nothing else (sweep F341):
// another host's own slot, the shared overflow, and every request this route
// refuses are left as they were.
void test_the_hand_over_clears_only_its_asker() {
  // B is armed by a direct write to its slot, never by a receipt request
  // (a second address asking for the receipt during the tap is the tap's
  // own question, F342). A's tap leaves B's count, its backoff and its 429
  // exactly where they were.
  reset_device();
  arm_lockout(3);
  set_strikes(kHostB, 4, millis(), millis() + 16000);
  open_tap();
  Req a;
  a.bearer(kWrong);
  CHECK(receipt(a) == 1);
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
  CHECK(strikes(kHostA) == 0);
  CHECK_MSG(strikes(kHostB) == 4, "A's tap changed B's count");
  CHECK(held(kHostB));
  Req b;
  b.bearer(kToken).from(192, 168, 1, 21);
  CHECK(handle_pairing_qr(&b.r) == ESP_OK);
  CHECK_MSG(b.r.status == "429 Too Many Requests", "A's tap freed B");
  CHECK(header(b.r, "Retry-After") == "17");   // 16000 ms / 1000 + 1
  // The claim likewise.
  reset_device();
  arm_lockout(3);
  set_strikes(kHostB, 2, millis(), millis() + 4000);
  mint_claim();
  Req c;
  c.query(std::string("claim=") + g_improv_claim.hex);
  CHECK(receipt(c) == 1);
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::SETUP_DOOR_CLAIM);
  CHECK(strikes(kHostA) == 0);
  CHECK(strikes(kHostB) == 2);
  CHECK(held(kHostB));

  // A held only through the shared overflow (eight other addresses hold
  // every slot, and A's own strike landed in the overflow): the tap serves
  // A, and A stays held. Clearing the overflow would free every untracked
  // address, an attacker's included, and its next strike re-arms it at once.
  reset_device();
  for (uint8_t i = 0; i < canary::net::auth_backoff::SLOTS; ++i) {
    set_strikes(0x0A000001u + i, 1, millis(), millis() + 2000);
  }
  Req into_overflow;
  into_overflow.bearer(kWrong);
  CHECK(receipt(into_overflow) == 0);   // A's strike: the overflow's
  CHECK(strikes(kHostA) == 0);          // A has no slot of its own
  CHECK(g_auth_table.overflow.failures == 1);
  CHECK(held(kHostA));
  open_tap();
  Req a_tap;
  CHECK(receipt(a_tap) == 1);
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
  CHECK_MSG(g_auth_table.overflow.failures == 1, "the tap cleared the shared overflow");
  CHECK_MSG(held(kHostA), "an address held only through the overflow was freed by the tap");
  Req a_right;
  a_right.bearer(kToken);
  CHECK(receipt(a_right) == 0);
  check_lockout_refusal(a_right.r);
  for (uint8_t i = 0; i < canary::net::auth_backoff::SLOTS; ++i) {
    CHECK(strikes(0x0A000001u + i) == 1);   // the tracked slots untouched too
  }

  // Every refusal clears nothing. No tap and no claim: a header-less poll
  // gets the 403 the BOOT-tap wait reads, a held bearer the 429.
  reset_device();
  arm_lockout(3);
  Req bare;
  CHECK(receipt(bare) == 0);
  check_gate_refusal(bare.r);
  Req held_right;
  held_right.bearer(kToken);
  CHECK(receipt(held_right) == 0);
  check_lockout_refusal(held_right.r);
  CHECK_MSG(strikes(kHostA) == 3, "a refused request cleared its address");
  // A wrong claim (no match: it spends nothing, sweep F342) and no tap:
  // refused, nothing cleared.
  mint_claim();
  Req wrong_claim;
  wrong_claim.query(std::string("claim=") + kClaimHex);
  CHECK(receipt(wrong_claim) == 0);
  check_gate_refusal(wrong_claim.r);
  CHECK(claim_pending());
  CHECK(strikes(kHostA) == 3);
  CHECK(held(kHostA));
  // A tap that has timed out serves nothing and clears nothing.
  reset_device();
  arm_lockout(3);
  open_tap();
  stub_grant::now_ms += PROVISIONING_GATE_TTL_MS;
  set_strikes(kHostA, 3, millis(), millis() + 8000);
  Req late;
  CHECK(receipt(late) == 0);
  check_gate_refusal(late.r);
  CHECK(strikes(kHostA) == 3);
}

// The receipt reads the lockout (api_auth_bearer_held) before the claim and
// the tap and answers it after them, so the backoff can end in between. The
// 429 still names a wait the client can use, at least 1 s, never the
// unsigned difference wrapped to about 4.29 million seconds.
void test_receipt_lockout_answer_never_wraps() {
  reset_device();
  arm_lockout(3);                 // 8 s left
  Req running;
  running.bearer(kToken);
  CHECK(receipt(running) == 0);
  check_lockout_refusal(running.r);
  CHECK(header(running.r, "Retry-After") == "9");   // 8000 ms / 1000 + 1
  CHECK(running.r.resp.find("\"retry_after_s\":9}") != std::string::npos);

  // A wrong claim beside the bearer: the claim's health line is where the
  // clock passes the backoff's end, after the held read, before the send.
  reset_device();
  arm_lockout(3);
  mint_claim();
  g_health_line_ms = 8001;
  Req raced;
  raced.bearer(kToken).query(std::string("claim=") + kClaimHex);
  CHECK(receipt(raced) == 0);
  CHECK(!held(kHostA));   // the backoff did end inside the handler
  check_lockout_refusal(raced.r);
  CHECK_MSG(header(raced.r, "Retry-After") == "1",
            "the lockout's Retry-After wrapped when the backoff ended mid-request");
  CHECK(raced.r.resp.find("\"retry_after_s\":1}") != std::string::npos);

  // The same answer sent straight after the backoff ended, and the remaining
  // time the health JSON reads: 0, not a wrap.
  reset_device();
  arm_lockout(1);
  stub_grant::now_ms += 8000;
  Req late;
  CHECK(remaining_for(&late.r) == 0);
  api_auth_send_lockout(&late.r);
  CHECK(header(late.r, "Retry-After") == "1");
  stub_grant::now_ms -= 3000;
  CHECK(remaining_for(&late.r) == 3000);
}

// ── the BOOT tap's one asker (sweep F342) ───────────────────────────────────

// How many "BOOT tap closed" health lines were written.
size_t tap_closed_lines() {
  size_t n = 0;
  for (const auto& l : g_health_lines) if (l.find("BOOT tap closed") != std::string::npos) ++n;
  return n;
}
uint32_t gate_stamp() { return canary::net::provisioning_gate::peek(g_provisioning_gate); }

// (a) The owner's dashboard asks from before the tap (Use BOOT Tap Once
// polls from the click); a host on the network asks just after the tap.
// Before: B was served the receipt as BOOT_TAP (the token, the SoftAP
// password) and the owner's next poll was 403.
void test_two_hosts_in_one_tap_are_both_refused() {
  reset_device();
  stub_grant::serial_lines.clear();
  Req a_before;                       // A at T-1 s: nothing to take yet
  CHECK(receipt(a_before) == 0);
  check_gate_refusal(a_before.r);
  stub_grant::now_ms += 1000;
  open_tap();                         // T
  stub_grant::now_ms += 100;
  Req b;                              // B at T+0.1 s
  b.from(192, 168, 1, 21);
  CHECK_MSG(receipt(b) == 0, "a second host was served the owner's tap");
  check_tap_contended(b.r);
  CHECK(g_served.empty());
  CHECK_MSG(gate_stamp() == 0, "the contended tap was left open");
  stub_grant::now_ms += 900;
  Req a_after;                        // A at T+1 s
  CHECK(receipt(a_after) == 0);
  check_tap_contended(a_after.r);
  CHECK_MSG(tap_closed_lines() == 1, "the closed tap wrote " + std::to_string(tap_closed_lines()) + " lines");
  // Message and detail alike (the stub keeps both, as /api/logs serves them).
  for (const auto& l : g_health_lines) {
    CHECK_MSG(l.find("192.168") == std::string::npos, "a health line names an address: " + l);
  }
  bool console_names_both = false;
  for (const auto& l : stub_grant::serial_lines) {
    if (l == "[AUTH] BOOT tap closed: two addresses asked for the receipt (192.168.1.21 and 192.168.1.20)\n") {
      console_names_both = true;
    }
  }
  CHECK_MSG(console_names_both, "the console does not name both askers");
  // The 409 lasts while B is still asking: at B's last ask plus the lookback
  // (T+10.1 s) the owner still hears it, and one millisecond later, inside
  // the closed tap's 30 s, the route answers its 403 again, so a page that
  // waited and asks again keeps asking for the next tap. (Before, every ask
  // was 409 until T+30 s.)
  stub_grant::now_ms += 9100;         // T+10.1 s
  Req a_edge;
  CHECK(receipt(a_edge) == 0);
  check_tap_contended(a_edge.r);
  stub_grant::now_ms += 1;
  Req a_done;
  CHECK(receipt(a_done) == 0);
  check_gate_refusal(a_done.r);
  CHECK(tap_closed_lines() == 1);
  CHECK(g_served.empty());
}

// The bench row (hardware_verification_checklist.md, "A second asker closes
// the tap"): B asks once and stops; the owner's page stops on its 409; the
// owner waits about 10 s and clicks Use BOOT Tap Once again. That click's
// first poll is 403, not 409, so the page keeps asking, and the owner's next
// tap serves it. Before, the first poll of a click 10 to 30 s after the tap
// was 409 and the page stopped again before the press.
void test_the_owner_retries_after_the_other_device_stops() {
  reset_device();
  Req a_click;
  CHECK(receipt(a_click) == 0);
  stub_grant::now_ms += 1000;
  const uint32_t T = stub_grant::now_ms;
  open_tap();
  stub_grant::now_ms += 100;
  Req b;
  b.from(192, 168, 1, 21);
  CHECK(receipt(b) == 0);
  check_tap_contended(b.r);
  stub_grant::now_ms += 900;
  Req a_stops;
  CHECK(receipt(a_stops) == 0);
  check_tap_contended(a_stops.r);     // the page stops here
  stub_grant::now_ms = T + 12000;     // waited 10 s, clicked again
  Req a_retry;
  CHECK(receipt(a_retry) == 0);
  CHECK_MSG(a_retry.r.status == "403 Forbidden",
            "the retry's first poll, B quiet 11.9 s, answered " + a_retry.r.status);
  check_gate_refusal(a_retry.r);
  stub_grant::now_ms += 2000;
  Req a_poll;
  CHECK(receipt(a_poll) == 0);
  check_gate_refusal(a_poll.r);
  stub_grant::now_ms += 1000;         // T+15 s: the tap
  open_tap();
  stub_grant::now_ms += 1000;
  Req a_served;
  CHECK_MSG(receipt(a_served) == 1, "the owner's retry was not served after the other device stopped");
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
  CHECK(has_header(a_served.r, "Set-Cookie"));
  CHECK(tap_closed_lines() == 1);
}

// A re-tap served inside the first tap's 30 s ends the contention: the asks
// after it hear 403, never "the tap was closed". Here B polls every 200 ms,
// the owner asks once after the closed tap (409) and then re-taps without
// asking first, the bare re-tap the 409's hint used to advise: the re-tap
// goes to B, the residual a tap no client of the owner's was asking for.
// Before, the owner's next ask was 409 although the token, the SoftAP
// password and the pin had just been handed out.
void test_a_served_retap_ends_the_contention() {
  reset_device();
  Req a_click;
  CHECK(receipt(a_click) == 0);
  stub_grant::now_ms += 1000;
  const uint32_t T = stub_grant::now_ms;
  open_tap();
  size_t b_served = 0;
  bool retapped = false;
  for (uint32_t d = 100; d <= 12700; d += 200) {
    stub_grant::now_ms = T + d;
    if (d == 2100) {
      Req a;
      CHECK(receipt(a) == 0);
      check_tap_contended(a.r);
    }
    if (!retapped && d >= 12500) {
      open_tap();
      retapped = true;
    }
    Req b;
    b.from(192, 168, 1, 21);
    b_served += receipt(b);
  }
  CHECK_MSG(b_served == 1, "the poller was served " + std::to_string(b_served) + " times");
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
  stub_grant::now_ms = T + 13000;
  Req a_next;
  CHECK(receipt(a_next) == 0);
  CHECK_MSG(a_next.r.status == "403 Forbidden",
            "after a served re-tap the owner heard " + a_next.r.status + " (\"the tap was closed\")");
  check_gate_refusal(a_next.r);
  CHECK(tap_closed_lines() == 1);
}

// The millis() hook (stubs/receipt_grant/Arduino.h), armed with its call
// count zeroed, and disarmed.
void arm_millis_hook(std::function<uint32_t(unsigned)> hook) {
  stub_grant::millis_calls = 0;
  stub_grant::millis_hook = std::move(hook);
}
void disarm_millis_hook() { stub_grant::millis_hook = nullptr; }

// Which millis() call of a credential-less receipt request is the tap
// step's clock read (tap_arbitrate()'s now_ms, read after the peek()): each
// call returns a clock one millisecond further on, and the arbiter records
// the ask at the time it read. The request is an ask with the tap closed
// (403), so it stands for the owner's click before the tap.
// The probe's message when the arbiter's record of a hooked ask is not the
// hook's time. The hooked ask comes from the address tap_step_clock_call()
// already put on record, so the record moves only if an ask by that address
// refreshes its time: a failure here is the harness missing the clock read,
// or tap_arbitrate() no longer refreshing a repeat asker, which is the
// refresh that keeps a polling owner on record (row (c) holds its effect).
const std::string kHookMissed =
    "the hook did not run at the tap step's clock read, or a repeat ask by the address on record "
    "no longer refreshes its time";

unsigned tap_step_clock_call() {
  const uint32_t base = stub_grant::now_ms;
  arm_millis_hook([base](unsigned i) { return base + 1 + i; });
  Req a;
  CHECK(receipt(a) == 0);
  disarm_millis_hook();
  check_gate_refusal(a.r);
  const uint32_t got = g_tap_arbiter.recent.last_ms;
  CHECK_MSG(got > base, "the tap step read no clock through millis()");
  return got - base - 1;
}

// The tap step peeks the stamp, judges it, then takes exactly that stamp:
// when another consumer takes the tap between the judgment and the take,
// the request is refused (403) and nothing is served. Two consumers race
// for the stamp on a device: the BLE OTA break-glass take() on the NimBLE
// task, and the loop task's BOOT handler storing a re-tap. The hook acts on
// the tap step's clock read, between peek() and take_exact(), and the
// arbiter's record of the ask at the hook's time proves it acted there. A
// tap step that ignored take_exact()'s false (`take_exact(...); return
// true;`) served both: the receipt on a tap another consumer held.
void test_a_take_lost_after_the_judgment_is_refused() {
  // The break-glass take between the judgment and the take.
  reset_device();
  const unsigned k = tap_step_clock_call();
  stub_grant::now_ms += 1000;
  open_tap();
  stub_grant::now_ms += 500;
  const uint32_t at = stub_grant::now_ms + 3;
  bool ran = false;
  arm_millis_hook([&](unsigned i) -> uint32_t {
    if (i != k) return stub_grant::now_ms;
    ran = true;
    CHECK(provisioning_gate_take());   // the NimBLE task's break-glass take
    return at;
  });
  Req a;
  CHECK_MSG(receipt(a) == 0, "served on a tap the break-glass take held");
  disarm_millis_hook();
  check_gate_refusal(a.r);
  CHECK(ran);
  CHECK_MSG(g_tap_arbiter.recent.last_ms == at, kHookMissed);
  CHECK(g_served.empty());
  CHECK(gate_stamp() == 0);
  // A re-tap stored between the judgment and the take: the request is
  // refused, the new stamp is left open, and the next ask is served once.
  reset_device();
  const unsigned k2 = tap_step_clock_call();
  stub_grant::now_ms += 1000;
  open_tap();
  const uint32_t first = gate_stamp();
  stub_grant::now_ms += 500;
  const uint32_t at2 = stub_grant::now_ms + 3;
  const uint32_t retap = stub_grant::now_ms + 2;
  arm_millis_hook([&](unsigned i) -> uint32_t {
    if (i != k2) return stub_grant::now_ms;
    canary::net::provisioning_gate::open(g_provisioning_gate, retap);   // the loop task's BOOT handler
    return at2;
  });
  Req a1;
  CHECK_MSG(receipt(a1) == 0, "served on a stamp nobody judged");
  disarm_millis_hook();
  check_gate_refusal(a1.r);
  CHECK_MSG(g_tap_arbiter.recent.last_ms == at2, kHookMissed);
  CHECK(g_served.empty());
  CHECK_MSG(gate_stamp() == retap && retap != first, "the re-tap was consumed unjudged");
  stub_grant::now_ms += 100;
  Req a2;
  CHECK(receipt(a2) == 1);
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
  Req a3;
  CHECK(receipt(a3) == 0);
  check_gate_refusal(a3.r);
  CHECK(g_served.size() == 1);
}

// The NO_TAP step's burn of an expired stamp is a compare-and-swap of the
// stamp it peeked (take_exact()), never a close() of whatever the gate holds
// by then: the loop task can sweep the old stamp and the BOOT handler store
// the owner's re-tap while this request sits between its peek() and its
// burn. With the gate closed there is nothing to burn, so the request
// touches nothing. Either way the re-tap stays open and serves the next ask
// once. A tap step that burned with close() (or closed unconditionally)
// cleared the owner's fresh press on a stranger's request, and the next ask
// heard 403.
void test_a_retap_during_a_no_tap_ask_is_kept() {
  for (int expired = 0; expired < 2; ++expired) {
    const std::string why = expired ? " (an expired stamp peeked)" : " (the gate closed at the peek)";
    reset_device();
    const unsigned k = tap_step_clock_call();
    if (expired) {
      open_tap();
      stub_grant::now_ms += PROVISIONING_GATE_TTL_MS + 5;
    }
    const uint32_t old_stamp = gate_stamp();
    CHECK(expired ? old_stamp != 0 : old_stamp == 0);
    const uint32_t retap = stub_grant::now_ms + 2;
    const uint32_t at = stub_grant::now_ms + 3;
    bool ran = false;
    arm_millis_hook([&](unsigned i) -> uint32_t {
      if (i != k) return stub_grant::now_ms;
      ran = true;
      canary::net::provisioning_gate::open(g_provisioning_gate, retap);   // the loop task's BOOT handler
      return at;
    });
    Req a;
    CHECK_MSG(receipt(a) == 0, "an ask that peeked no fresh stamp was served" + why);
    disarm_millis_hook();
    check_gate_refusal(a.r);
    CHECK(ran);
    CHECK_MSG(g_tap_arbiter.recent.last_ms == at, kHookMissed + why);
    CHECK_MSG(gate_stamp() == retap, "the re-tap was cleared by a NO_TAP ask" + why);
    CHECK(g_served.empty());
    stub_grant::now_ms += 100;
    Req a2;
    CHECK_MSG(receipt(a2) == 1, "the re-tap did not serve the next ask" + why);
    CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
    CHECK(gate_stamp() == 0);
  }
}

// The CONTENDED step closes the tap with a compare-and-swap of the stamp it
// peeked and judged (take_exact(seen)), never by reading the gate again: a
// re-tap the loop task's BOOT handler stores while this request sits between
// its peek() and its close is a new tap, judged on the next ask like any
// other. Here B asks 5 s before the tap and A first asks 8 s after it, so
// A's ask closes the tap (409); the re-tap lands between A's peek() and
// A's close. The re-tap is kept, and A's next ask is served: B last asked
// 13 s before the re-tap, outside its lookback. A close that re-read the
// gate (`take_exact(g, peek(g))`, an expire() of whatever it holds) cleared
// the owner's fresh press unjudged, and A's next ask heard 403.
void test_a_retap_during_a_contended_close_is_kept() {
  reset_device();
  const unsigned k = tap_step_clock_call();
  stub_grant::now_ms += 20000;
  Req b0;
  b0.from(192, 168, 1, 21);
  CHECK(receipt(b0) == 0);
  check_gate_refusal(b0.r);
  stub_grant::now_ms += 5000;
  open_tap();
  const uint32_t first = gate_stamp();
  stub_grant::now_ms += 8000;
  const uint32_t retap = stub_grant::now_ms + 2;
  const uint32_t at = stub_grant::now_ms + 3;
  bool ran = false;
  arm_millis_hook([&](unsigned i) -> uint32_t {
    if (i != k) return stub_grant::now_ms;
    ran = true;
    canary::net::provisioning_gate::open(g_provisioning_gate, retap);   // the loop task's BOOT handler
    return at;
  });
  Req a;
  CHECK_MSG(receipt(a) == 0, "an ask that found the tap contended was served");
  disarm_millis_hook();
  check_tap_contended(a.r);
  CHECK(ran);
  CHECK_MSG(g_tap_arbiter.recent.last_ms == at, kHookMissed);
  CHECK_MSG(g_tap_arbiter.contended_tap == first, "the ask did not close the tap it judged");
  CHECK_MSG(gate_stamp() == retap && retap != first, "the contended close cleared a re-tap nobody judged");
  CHECK(g_served.empty());
  CHECK_MSG(tap_closed_lines() == 0, "a close that lost its swap logged a closed tap");
  stub_grant::now_ms += 100;
  Req a2;
  CHECK_MSG(receipt(a2) == 1, "the re-tap kept through the contended close did not serve the next ask");
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
  CHECK(gate_stamp() == 0);
}

// (b) A sole asker is served at once: no settle delay, the cookie with it.
void test_sole_asker_is_served_at_once() {
  reset_device();
  Req a_before;
  CHECK(receipt(a_before) == 0);
  stub_grant::now_ms += 1000;
  open_tap();
  stub_grant::now_ms += 500;
  Req a;
  CHECK(receipt(a) == 1);
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
  CHECK(a.r.status == "200 OK");
  CHECK(has_header(a.r, "Set-Cookie"));
  CHECK(!tap_open() && gate_stamp() == 0);
  CHECK(tap_closed_lines() == 0);
}

// (c) A host that waits for the tap and asks first after it (it timed the
// tap from /api/logs, say) loses to an owner who asked before it. First one
// ask 2 s before the tap; then the owner's pages as they really ask, every
// 2 s from the click: Use BOOT Tap Once clicked 21 s before the tap, the
// companion's Save 29 s before it, both longer than the 10 s lookback. Each
// ask by the address already on record refreshes its time, so the owner's
// last poll (1 s before the tap) is what the host's ask is judged against;
// an arbiter that kept the owner's first ask time served the host.
void test_first_asker_after_the_tap_loses_to_a_waiting_owner() {
  reset_device();
  Req a_before;
  CHECK(receipt(a_before) == 0);
  stub_grant::now_ms += 2000;
  open_tap();
  stub_grant::now_ms += 3000;
  Req b;
  b.from(192, 168, 1, 21);
  CHECK_MSG(receipt(b) == 0, "the first asker after the tap took it from a waiting owner");
  check_tap_contended(b.r);
  stub_grant::now_ms += 1000;
  Req a;
  CHECK(receipt(a) == 0);
  check_tap_contended(a.r);
  CHECK(g_served.empty());
  const uint32_t leads[] = {21000, 29000};   // ms from the click to the tap
  for (const uint32_t lead : leads) {
    const std::string why = " (the click " + std::to_string(lead) + " ms before the tap)";
    reset_device();
    const uint32_t click = stub_grant::now_ms;
    uint32_t next = click;
    for (; next < click + lead; next += 2000) {
      stub_grant::now_ms = next;
      Req poll;
      CHECK(receipt(poll) == 0);
      check_gate_refusal(poll.r);
    }
    stub_grant::now_ms = click + lead;
    open_tap();
    stub_grant::now_ms += 100;
    Req host;
    host.from(192, 168, 1, 21);
    CHECK_MSG(receipt(host) == 0, "the host asking first after the tap took it from a polling owner" + why);
    check_tap_contended(host.r);
    CHECK_MSG(!carries_secret(host.r), "the host's answer carries the secret" + why);
    stub_grant::now_ms = next;
    Req owner;
    CHECK(receipt(owner) == 0);
    check_tap_contended(owner.r);
    CHECK_MSG(g_served.empty(), "something was served beside a polling owner" + why);
    CHECK(gate_stamp() == 0);
  }
}

// (c2) Row (c)'s polling owner and late host, both real IPv6 peers whose
// keys differ only in the prefix (2001:db8::1:c0a8:114 and
// 2001:db9::1:c0a8:114), and then two link-local peers. The per-address
// lockout puts every IPv6 peer in one bucket (auth_request_source() is 0 for
// each), but the tap's asker is the whole 16-byte key: an arbiter that
// compared the last four bytes, or the IPv4 a key maps to, merged the two
// and served the host the token. No WAP interface runs IPv6 today; this
// holds the handler for the day one does.
void test_ipv6_askers_are_two_askers() {
  static const uint8_t kOwner6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 1, 0xc0, 0xa8, 0x01, 0x14};
  static const uint8_t kHost6[16] = {0x20, 0x01, 0x0d, 0xb9, 0, 0, 0, 0, 0, 0, 0, 1, 0xc0, 0xa8, 0x01, 0x14};
  static const uint8_t kOwnerLink[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0x11, 0x22, 0xff, 0xfe, 0x33, 0x44, 0x55};
  static const uint8_t kHostLink[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x00, 0xaa, 0xbb, 0xff, 0xfe, 0xcc, 0xdd, 0xee};
  struct Pair {
    const uint8_t (*owner)[16];
    const uint8_t (*host)[16];
    const char* what;
  };
  const Pair pairs[] = {
      {&kOwner6, &kHost6, " (2001:db8::1:c0a8:114 and 2001:db9::1:c0a8:114)"},
      {&kOwnerLink, &kHostLink, " (fe80::211:22ff:fe33:4455 and fe80::aa:bbff:fecc:ddee)"},
  };
  for (const Pair& p : pairs) {
    reset_device();
    const uint32_t click = stub_grant::now_ms;
    uint32_t next = click;
    for (; next < click + 21000; next += 2000) {
      stub_grant::now_ms = next;
      Req poll;
      poll.from6(*p.owner);
      CHECK(receipt(poll) == 0);
      check_gate_refusal(poll.r);
    }
    stub_grant::now_ms = click + 21000;
    open_tap();
    stub_grant::now_ms += 100;
    Req host;
    host.from6(*p.host);
    CHECK_MSG(receipt(host) == 0, std::string("a second IPv6 peer was served as the owner") + p.what);
    check_tap_contended(host.r);
    CHECK_MSG(!carries_secret(host.r), std::string("the host's answer carries the secret") + p.what);
    stub_grant::now_ms = next;
    Req owner;
    owner.from6(*p.owner);
    CHECK(receipt(owner) == 0);
    check_tap_contended(owner.r);
    CHECK_MSG(g_served.empty(), std::string("something was served beside a polling IPv6 owner") + p.what);
    CHECK(gate_stamp() == 0);
  }
  // One IPv6 peer alone is one asker, and is served.
  reset_device();
  Req before;
  before.from6(kOwnerLink);
  CHECK(receipt(before) == 0);
  stub_grant::now_ms += 1000;
  open_tap();
  stub_grant::now_ms += 1000;
  Req alone;
  alone.from6(kOwnerLink);
  CHECK(receipt(alone) == 1);
  CHECK(g_served.size() == 1 && g_served.back() == receipt_authority::Grant::BOOT_TAP);
  CHECK(alone.r.status == "200 OK");
}

// (d) What counts as an ask: a request the bearer and the claim did not
// serve. B's wrong bearer (a strike against B) and B's wrong claim each
// contend the owner's tap; B's right bearer and B's right claim are served
// by their own grants and never reach the tap, so the owner's sign-in is
// still served.
void test_what_counts_as_an_ask() {
  for (int shape = 0; shape < 2; ++shape) {
    reset_device();
    mint_claim();
    Req a_before;
    CHECK(receipt(a_before) == 0);
    stub_grant::now_ms += 1000;
    open_tap();
    stub_grant::now_ms += 200;
    Req b;
    b.from(192, 168, 1, 21);
    if (shape == 0) b.bearer(kWrong);
    else b.query(std::string("claim=") + kClaimHex);
    CHECK(receipt(b) == 0);
    check_tap_contended(b.r);
    if (shape == 0) CHECK_MSG(strikes(kHostB) == 1, "B's wrong bearer was not B's strike");
    else CHECK(claim_pending());
    Req a;
    CHECK_MSG(receipt(a) == 0, std::string("the owner was served beside B's ") + (shape ? "wrong claim" : "wrong bearer"));
    check_tap_contended(a.r);
  }
  for (int shape = 0; shape < 2; ++shape) {
    reset_device();
    mint_claim();
    Req a_before;
    CHECK(receipt(a_before) == 0);
    stub_grant::now_ms += 1000;
    open_tap();
    stub_grant::now_ms += 200;
    Req b;
    b.from(192, 168, 1, 21);
    if (shape == 0) b.bearer(kToken);
    else b.query(std::string("claim=") + g_improv_claim.hex);
    CHECK(receipt(b) == 1);
    CHECK(!g_served.empty() && g_served.back() == (shape == 0 ? receipt_authority::Grant::BEARER
                                                              : receipt_authority::Grant::SETUP_DOOR_CLAIM));
    CHECK(tap_open());
    Req a;
    CHECK_MSG(receipt(a) == 1, std::string("B's right ") + (shape ? "claim" : "bearer") + " contended the owner's tap");
    CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
  }
}

// (e) The BLE OTA break-glass hook (NimBLE task) takes the tap with the
// header's take(), at once and unjudged: the receipt then finds nothing.
void test_break_glass_leaves_nothing_to_take() {
  reset_device();
  open_tap();
  CHECK(provisioning_gate_take());
  Req a;
  CHECK(receipt(a) == 0);
  check_gate_refusal(a.r);
  CHECK(g_served.empty());
}

// (f) Contention does not carry into a clean next tap: B went silent, and
// once it has been silent past the lookback the owner's re-tap serves.
void test_contention_does_not_carry_into_a_clean_next_tap() {
  reset_device();
  Req a_before;
  CHECK(receipt(a_before) == 0);
  stub_grant::now_ms += 1000;
  open_tap();
  stub_grant::now_ms += 100;
  Req b;
  b.from(192, 168, 1, 21);
  CHECK(receipt(b) == 0);
  check_tap_contended(b.r);
  // The owner keeps polling; B is silent. 11 s later the owner taps again.
  for (int i = 0; i < 5; ++i) {
    stub_grant::now_ms += 2000;
    Req a;
    CHECK(receipt(a) == 0);
    check_tap_contended(a.r);
  }
  stub_grant::now_ms += 1000;          // B silent 11 s
  open_tap();
  stub_grant::now_ms += 500;
  Req a_retap;
  CHECK_MSG(receipt(a_retap) == 1, "contention carried into a clean next tap");
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
  // A re-tap while B is still asking: closed again.
  reset_device();
  Req a0;
  CHECK(receipt(a0) == 0);
  stub_grant::now_ms += 1000;
  open_tap();
  Req b0;
  b0.from(192, 168, 1, 21);
  CHECK(receipt(b0) == 0);
  stub_grant::now_ms += 4000;
  Req b1;
  b1.from(192, 168, 1, 21);
  CHECK(receipt(b1) == 0);
  stub_grant::now_ms += 1000;
  open_tap();
  stub_grant::now_ms += 100;
  Req a1;
  CHECK(receipt(a1) == 0);
  check_tap_contended(a1.r);
  CHECK(gate_stamp() == 0);
  CHECK(tap_closed_lines() == 2);
}

// (g) A socket that cannot say who sent the request is refused, and the tap
// is left for the next asker: the arbiter is never skipped.
void test_unreadable_peer_is_refused_and_leaves_the_tap() {
  reset_device();
  open_tap();
  Req nobody;
  nobody.unreadable();
  CHECK_MSG(receipt(nobody) == 0, "a socket that cannot say was served the tap");
  check_gate_refusal(nobody.r);
  CHECK_MSG(gate_stamp() != 0, "a socket that cannot say spent the tap");
  Req a;
  CHECK(receipt(a) == 1);
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
}

// (h) A bearer the backoff holds still gets its 429 under contention, not
// the 409: a held page retries its token. And the 409 clears nothing.
void test_held_bearer_under_contention_is_429_and_the_409_clears_nothing() {
  reset_device();
  Req a_before;
  CHECK(receipt(a_before) == 0);
  stub_grant::now_ms += 1000;
  open_tap();
  stub_grant::now_ms += 100;
  Req b;
  b.from(192, 168, 1, 21);
  CHECK(receipt(b) == 0);
  check_tap_contended(b.r);
  arm_lockout(3);
  Req held_a;
  held_a.bearer(kToken);
  CHECK(receipt(held_a) == 0);
  check_lockout_refusal(held_a.r);
  Req bare_a;
  CHECK(receipt(bare_a) == 0);
  check_tap_contended(bare_a.r);
  CHECK_MSG(strikes(kHostA) == 3 && held(kHostA), "the 409 cleared the asker's backoff");
}

// A tap that ran out before anyone asked: the request that finds it is
// refused and burns the stale stamp (as the old take() did), so it cannot
// read as fresh again once millis() wraps.
void test_an_expired_tap_is_burned_by_the_request_that_finds_it() {
  reset_device();
  open_tap();
  stub_grant::now_ms += PROVISIONING_GATE_TTL_MS + 5;
  Req a;
  CHECK(receipt(a) == 0);
  check_gate_refusal(a.r);
  CHECK_MSG(gate_stamp() == 0, "an expired stamp survived the request that found it");
}

// ── the Host question (sweep F344) ───────────────────────────────────────────

// The receipt's answer to a Host that cannot name this device: 403 and
// exactly {"error":"host"} (the flagship's body), not cached, no cookie, no
// secret.
void check_host_refusal(const httpd_req_t& r, const std::string& why) {
  CHECK_MSG(r.status == "403 Forbidden", why + ": answered " + r.status);
  CHECK_MSG(r.resp == "{\"error\":\"host\"}", why + ": body " + r.resp);
  CHECK(r.type == "application/json");
  CHECK(header(r, "Cache-Control") == "no-store");
  CHECK_MSG(!has_header(r, "Set-Cookie"), why + ": a cookie was set");
  CHECK_MSG(!carries_secret(r), why + ": the answer carries a secret");
}

// The receipt asks the Host first, in every mode, before the bearer is read,
// the claim is taken or the tap is judged. A DNS-rebinding page (its own
// name re-pointed at the WAP), a name that only contains canary.local, and a
// request with no Host are refused, and leave everything as they found it:
// the tap open, the claim pending, no strike against their address (F343),
// no asker recorded for the tap's arbiter (F342). Before, each was the
// receipt's caller like any other: the tap and the claim served it (the
// token, the SoftAP password), a wrong bearer was a strike, and an ask
// before the tap contended the owner's.
// A rebinding name too long for the 96-byte Host buffer (108 bytes, labels
// of 60 and 28 under evil.example) whose first 95 bytes end in ".local": cut
// to the buffer it would read as a .local name, so only
// request_host_names_device()'s refusal of a truncated header keeps it
// foreign (review of F344: no row sent one before).
#define OVERSIZE_REBINDING_HOST                                    \
  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa." \
  "aaaaaaaaaaaaaaaaaaaaaaaaaaaa.local.evil.example"
static_assert(sizeof(OVERSIZE_REBINDING_HOST) - 1 == 108, "the oversize row is 108 bytes");
// The longest Host the buffer holds (95 bytes and the NUL), a .local name.
#define LONGEST_HELD_HOST                                                   \
  "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb."      \
  "ccccccccccccccccccccccccc.local"
static_assert(sizeof(LONGEST_HELD_HOST) - 1 == 95, "the longest held row is 95 bytes");
// The shortest Host the buffer cannot hold (96 bytes: a 92-byte .local name
// and a port), which host_guard.h passes read whole: refused, because the
// header is cut (review 6 of F344: the 108-byte row is foreign read whole
// too, so a 128-byte buffer passed every row before this one).
#define SHORTEST_CUT_HOST                                                   \
  "ddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd."      \
  "eeeeeeeeeeeeeeeeeeeeee.local:443"
static_assert(sizeof(SHORTEST_CUT_HOST) - 1 == 96, "the shortest cut row is 96 bytes");

void test_receipt_foreign_host_is_refused_first() {
  CHECK(canary::net::host_names_this_device(std::string(OVERSIZE_REBINDING_HOST, 95).c_str()));
  CHECK(canary::net::host_names_this_device(SHORTEST_CUT_HOST));
  // An empty and a blank Host too (review 4 of F344): host_guard.h calls an
  // empty authority foreign, and the receipt's question must not pass one.
  for (const char* host : {"evil.example", "canary.local.evil.example", OVERSIZE_REBINDING_HOST, SHORTEST_CUT_HOST,
                           "", "   ", (const char*)nullptr}) {
    const std::string why = host ? std::string("Host '") + host + "'" : std::string("no Host");
    // An open tap: refused, and the tap stays open. The owner then asks
    // from another address under the device's name and is served.
    reset_device();
    open_tap();
    Req foreign;
    foreign.host(host);
    CHECK_MSG(receipt(foreign) == 0, why + ": a foreign Host was served the owner's tap");
    check_host_refusal(foreign.r, why + " at an open tap");
    CHECK_MSG(tap_open(), why + ": the foreign request spent the tap");
    Req owner;
    owner.from(192, 168, 1, 21);
    CHECK_MSG(receipt(owner) == 1, why + ": the owner, asking after a foreign request, answered " + owner.r.status);
    CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
    CHECK(tap_closed_lines() == 0);
    // A foreign ask before the tap is no asker: the owner, asking after
    // the tap from another address, is served, not 409.
    reset_device();
    Req early;
    early.host(host).from(192, 168, 1, 21);
    CHECK(receipt(early) == 0);
    check_host_refusal(early.r, why + " before the tap");
    stub_grant::now_ms += 1000;
    open_tap();
    stub_grant::now_ms += 100;
    Req owner_after;
    CHECK_MSG(receipt(owner_after) == 1,
              why + ": a foreign ask before the tap contended the owner's (" + owner_after.r.status + ")");
    CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
    CHECK(tap_closed_lines() == 0);
    // A pending claim, presented right or wrong: refused, still pending,
    // nothing logged.
    for (int right = 0; right < 2; ++right) {
      reset_device();
      mint_claim();
      Req claim;
      claim.host(host).query(std::string("claim=") + (right ? g_improv_claim.hex : kClaimHex));
      CHECK(receipt(claim) == 0);
      check_host_refusal(claim.r, why + (right ? " with the right claim" : " with a wrong claim"));
      CHECK_MSG(claim_pending(), why + ": the foreign request spent the claim");
      CHECK(g_improv_claim_spent_at_ms == 0);
      CHECK(claim_refusal_lines() == 0);
    }
    // A right or a wrong bearer: refused, never compared, no strike.
    for (const char* tok : {kToken, kWrong}) {
      reset_device();
      Req bearer;
      bearer.host(host).bearer(tok);
      CHECK(receipt(bearer) == 0);
      check_host_refusal(bearer.r, why + (tok == kToken ? " with the right bearer" : " with a wrong bearer"));
      CHECK_MSG(strikes(kHostA) == 0, why + ": the foreign request's bearer was a strike");
      CHECK(!held(kHostA));
      CHECK(g_served.empty());
    }
    // An address already held: the Host refusal, not the lockout's 429, and
    // its backoff exactly as it was.
    reset_device();
    arm_lockout(3);
    Req held_bearer;
    held_bearer.host(host).bearer(kToken);
    CHECK(receipt(held_bearer) == 0);
    check_host_refusal(held_bearer.r, why + " from a held address");
    CHECK(strikes(kHostA) == 3 && held(kHostA));
  }
  // The device's names, as the dashboard, the companion and the apps reach
  // it: served by the tap. The long ones too (a 39-character mDNS label,
  // mdns_hostname's whole char[40], with a port; a full global IPv6 literal
  // with a port; the longest Host the buffer holds), so a shrunk buffer
  // refuses a name the household uses.
  for (const char* host : {"canary-7916.local", "192.168.1.50", "192.168.4.1:80", "canary-7916", "canary-7916.lan",
                           "canary-0123456789abcdef0123456789abcdef.local:443",
                           "[2001:db8:aaaa:bbbb:cccc:dddd:eeee:ffff]:443", LONGEST_HELD_HOST}) {
    reset_device();
    open_tap();
    Req q;
    q.host(host);
    CHECK_MSG(receipt(q) == 1, std::string("Host ") + host + " was not served by the tap: " + q.r.status);
    CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BOOT_TAP);
    CHECK(has_header(q.r, "Set-Cookie"));
  }
}

// ── the pairing QR ───────────────────────────────────────────────────────────

size_t qr(Req& q) {
  const size_t before = g_qr_payloads.size();
  CHECK(handle_pairing_qr(&q.r) == ESP_OK);
  CHECK(q.r.sends == 1);
  return g_qr_payloads.size() - before;
}

void test_qr_cookie_alone_is_refused() {
  reset_device();
  mint_session();
  Req q;
  q.cookie();
  CHECK(cv_session_validate(&q.r));
  CHECK_MSG(qr(q) == 0, "a cookie alone served the pairing QR");
  CHECK(q.r.status == "401 Unauthorized");
  CHECK(header(q.r, "WWW-Authenticate").find("Bearer") == 0);
  CHECK(q.r.resp.find("\"error\":\"unauthorized\"") != std::string::npos);
  CHECK(!carries_secret(q.r));
  CHECK(strikes(kHostA) == 0);   // no credential presented: no strike
}

void test_qr_cookie_beside_wrong_bearer_is_refused() {
  reset_device();
  mint_session();
  Req q;
  q.cookie().bearer(kWrong);
  CHECK_MSG(qr(q) == 0, "a cookie rescued a wrong bearer on the pairing QR");
  CHECK(q.r.status == "403 Forbidden");
  CHECK(!carries_secret(q.r));
  CHECK(strikes(kHostA) == 1);   // a guess: a strike
}

void test_qr_bearer_serves() {
  reset_device();
  mint_session();
  set_strikes(kHostA, 2, millis(), millis());   // two strikes, the backoff over
  for (int with_cookie = 0; with_cookie < 2; ++with_cookie) {
    g_qr_payloads.clear();
    Req q;
    q.bearer(kToken);
    if (with_cookie) q.cookie();
    CHECK(qr(q) == 1);
    CHECK(q.r.status == "200 OK");
    CHECK(q.r.type == "image/svg+xml");
    CHECK(g_qr_payloads[0].find(std::string("\"token\":\"") + kToken + "\"") != std::string::npos);
    CHECK(g_qr_payloads[0].find("\"device_id\":\"canary-wap-7916\"") != std::string::npos);
  }
  CHECK(strikes(kHostA) == 0);   // a right bearer clears the count
  // In setup (not on the home Wi-Fi), plain http at the SoftAP address.
  CHECK(g_qr_payloads[0].find("\"base_url\":\"http://192.168.4.1\"") != std::string::npos);
  CHECK(g_qr_payloads[0].find("tls_cert_fp") == std::string::npos);
  // On the home Wi-Fi with TLS on: https at the .local name, with the pin.
  WiFi.connected = true;
  g_tls_enabled = true;
  std::snprintf(g_tls_cert_fp_hex, sizeof(g_tls_cert_fp_hex), "%s", std::string(64, 'f').c_str());
  Req secure;
  secure.bearer(kToken);
  CHECK(qr(secure) == 1);
  CHECK(g_qr_payloads.back().find("\"base_url\":\"https://canary-7916.local\"") != std::string::npos);
  CHECK(g_qr_payloads.back().find(std::string("\"tls_cert_fp\":\"") + std::string(64, 'f') + "\"") != std::string::npos);
}

void test_qr_no_credential_is_401_without_strike() {
  reset_device();
  Req q;
  CHECK(qr(q) == 0);
  CHECK(q.r.status == "401 Unauthorized");
  CHECK(strikes(kHostA) == 0);
}

// The shapes api_auth_check_bearer() reads before any compare, through the
// gate every admin route shares (review of F343). A credential that is not a
// bearer, or a header too long for the buffer, is a 401 and one strike, as
// on the flagship (credential_shaped_failures_still_strike); the right token
// with spaces around it is the right token. Before these rows the receipt's
// grant was the only one that held them (test_receipt_bearer_shapes), and a
// gate that skipped either strike or the trim passed every suite.
void test_qr_bearer_shapes() {
  struct Shape {
    std::string raw;
    const char* hint;
  };
  // 128 bytes, the buffer's size: no room for the NUL.
  const std::string too_long = std::string("Bearer ") + std::string(121, 'x');
  CHECK(too_long.size() == 128);
  for (const Shape& s : {Shape{std::string("Basic ") + kToken, "Use 'Bearer <token>' format"},
                         Shape{std::string("bearer ") + kToken, "Use 'Bearer <token>' format"},
                         Shape{too_long, "Authorization header too long"}}) {
    reset_device();
    Req q;
    q.authorization(s.raw);
    CHECK_MSG(qr(q) == 0, "the pairing QR served: " + s.raw.substr(0, 16));
    CHECK(q.r.status == "401 Unauthorized");
    CHECK_MSG(q.r.resp.find(s.hint) != std::string::npos, "not the shape's own 401: " + q.r.resp);
    CHECK(!carries_secret(q.r));
    CHECK_MSG(strikes(kHostA) == 1, "a malformed credential was no strike: " + s.raw.substr(0, 16));
  }
  // One byte shorter fits the buffer, so it is compared: a wrong token, 403.
  reset_device();
  Req fits;
  fits.authorization(too_long.substr(0, 127));
  CHECK(qr(fits) == 0);
  CHECK(fits.r.status == "403 Forbidden");
  CHECK(strikes(kHostA) == 1);
  // Spaces after and before the right token: served, and the count it
  // carried cleared, as any right bearer clears it.
  for (const std::string& raw : {std::string("Bearer ") + kToken + "  ", std::string("Bearer   ") + kToken}) {
    reset_device();
    set_strikes(kHostA, 2, millis(), millis());   // two strikes, the backoff over
    g_qr_payloads.clear();
    Req q;
    q.authorization(raw);
    CHECK_MSG(qr(q) == 1, "the right token beside spaces was refused: " + q.r.status);
    CHECK(q.r.status == "200 OK");
    CHECK_MSG(strikes(kHostA) == 0, "the right token beside spaces left the count standing");
  }
}

void test_qr_lockout_is_every_admin_routes() {
  reset_device();
  mint_session();
  // Five wrong guesses, each once the backoff before it ended.
  for (int i = 0; i < 5; ++i) {
    wait_out(kHostA);
    Req q;
    q.cookie().bearer(kWrong);
    CHECK(qr(q) == 0);
    CHECK(q.r.status == "403 Forbidden");
  }
  CHECK(strikes(kHostA) == 5);
  // Locked out: the right token, the cookie beside it, is 429.
  Req right;
  right.cookie().bearer(kToken);
  CHECK(qr(right) == 0);
  CHECK(right.r.status == "429 Too Many Requests");
  CHECK(!header(right.r, "Retry-After").empty());
  CHECK(!carries_secret(right.r));
  // The backoff passes: served.
  wait_out(kHostA);
  Req later;
  later.bearer(kToken);
  CHECK(qr(later) == 1);
}

// One lockout for both routes: one host's guesses on the receipt hold its
// right token on the QR, and its guesses on the QR hold the receipt's.
void test_receipt_and_qr_share_the_lockout() {
  reset_device();
  for (int i = 0; i < 5; ++i) {
    wait_out(kHostA);
    Req q;
    q.bearer(kWrong);
    CHECK(receipt(q) == 0);
    check_gate_refusal(q.r);
  }
  CHECK(strikes(kHostA) == 5);
  Req on_qr;
  on_qr.bearer(kToken);
  CHECK_MSG(qr(on_qr) == 0, "five guesses on the receipt left the QR open");
  CHECK(on_qr.r.status == "429 Too Many Requests");

  reset_device();
  for (int i = 0; i < 5; ++i) {
    wait_out(kHostA);
    Req q;
    q.bearer(kWrong);
    CHECK(qr(q) == 0);
  }
  Req on_receipt;
  on_receipt.bearer(kToken);
  CHECK_MSG(receipt(on_receipt) == 0, "five guesses on the QR left the receipt's bearer open");
  check_lockout_refusal(on_receipt.r);
}

// ── per source address (sweep F343) ──────────────────────────────────────────

// One host's wrong bearers hold that host and no other. Before, the backoff
// was one for the device, so A's wrong token held B's right one at 429 on
// every route that reads the bearer.
void test_one_hosts_strikes_never_hold_another() {
  reset_device();
  Req wrong;   // A, the stub's default peer
  wrong.bearer(kWrong);
  CHECK(receipt(wrong) == 0);
  check_gate_refusal(wrong.r);
  CHECK(strikes(kHostA) == 1);
  Req b_receipt;
  b_receipt.from(192, 168, 1, 21).bearer(kToken);
  CHECK_MSG(receipt(b_receipt) == 1, "A's wrong bearer held B's right one on the receipt");
  CHECK(!g_served.empty() && g_served.back() == receipt_authority::Grant::BEARER);
  CHECK(b_receipt.r.status == "200 OK");
  Req b_qr;
  b_qr.from(192, 168, 1, 21).bearer(kToken);
  CHECK_MSG(qr(b_qr) == 1, "A's wrong bearer held B's right one on the pairing QR");
  Req b_admin;
  b_admin.from(192, 168, 1, 21).bearer(kToken);
  CHECK_MSG(api_auth_check(&b_admin.r, kToken), "A's wrong bearer held B's right one on an admin route");
  CHECK(strikes(kHostB) == 0);
  // A itself is held: its right token, inside its own backoff, is the 429.
  Req a_right;
  a_right.bearer(kToken);
  CHECK(receipt(a_right) == 0);
  check_lockout_refusal(a_right.r);
  CHECK(strikes(kHostA) == 1);
  // And B's own wrong bearer is B's strike alone.
  Req b_wrong;
  b_wrong.from(192, 168, 1, 21).bearer(kWrong);
  CHECK(qr(b_wrong) == 0);
  CHECK(b_wrong.r.status == "403 Forbidden");
  CHECK(strikes(kHostB) == 1 && strikes(kHostA) == 1);
}

// The F289 follow-up review's replay: host A sends a wrong bearer every
// 100 ms, so it strikes again the moment each of its backoffs ends; host B,
// the owner's app, polls an admin route's gate every 7 s with the right
// token, for ten minutes. Before, every one of B's 86 polls was refused.
void test_the_review_replay_holds_no_other_host() {
  reset_device();
  const uint32_t start = millis();
  int a_sent = 0, a_served = 0, a_strikes = 0, b_polls = 0, b_refused = 0;
  for (uint32_t t = 0; t < 600000; t += 100) {
    stub_grant::now_ms = start + t;
    Req a;
    a.bearer(kWrong);
    if (api_auth_check_bearer(&a.r, kToken)) ++a_served;
    ++a_sent;
    if (a.r.status == "403 Forbidden") ++a_strikes;   // compared: not held, a strike
    if (t % 7000 == 0) {
      Req b;
      b.from(192, 168, 1, 21).bearer(kToken);
      ++b_polls;
      if (!api_auth_check(&b.r, kToken)) ++b_refused;
    }
  }
  std::printf("replay: A sent %d wrong bearers, %d of them strikes; B polled %d times, %d refused\n",
              a_sent, a_strikes, b_polls, b_refused);
  CHECK(a_served == 0);
  CHECK(b_polls == 86);
  CHECK_MSG(b_refused == 0, "B was refused " + std::to_string(b_refused) + " of " + std::to_string(b_polls));
  CHECK(a_strikes > 10);   // A kept striking, each time its own backoff ended
  CHECK(strikes(kHostB) == 0);
}

// A source the socket cannot name shares one bucket: a socket that cannot
// say and a real IPv6 peer both key as 0 (the REAL auth_request_source(), cut
// from the sketch, over peer_source.h). That bucket holds its own members and
// no IPv4 host.
void test_unknown_address_shares_one_bucket() {
  static const uint8_t kV6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  reset_device();
  Req a;
  CHECK(auth_request_source(&a.r) == kHostA);   // ::ffff:192.168.1.20 keys as its IPv4
  Req b;
  b.from(192, 168, 1, 21);
  CHECK(auth_request_source(&b.r) == kHostB);
  Req unreadable;
  unreadable.unreadable();
  CHECK(auth_request_source(&unreadable.r) == 0);
  Req v6;
  v6.from6(kV6);   // 2001:db8::1
  CHECK(auth_request_source(&v6.r) == 0);
  Req nobody;
  nobody.from(0, 0, 0, 0);   // ::ffff:0.0.0.0 names no peer
  CHECK(auth_request_source(&nobody.r) == 0);
  // One strike from the socket that cannot say holds the IPv6 peer...
  unreadable.bearer(kWrong);
  CHECK(qr(unreadable) == 0);
  CHECK(unreadable.r.status == "403 Forbidden");
  CHECK(strikes(0) == 1);
  v6.bearer(kToken);
  CHECK(qr(v6) == 0);
  CHECK_MSG(v6.r.status == "429 Too Many Requests", "the unknown sources were not one bucket: " + v6.r.status);
  // ...and not A.
  a.bearer(kToken);
  CHECK_MSG(qr(a) == 1, "the unknown sources' bucket held an IPv4 host");
  CHECK(strikes(kHostA) == 0);
}

// ONE table for every file that includes api_auth.h. A strike recorded by
// this file's gate (the sketch's cut handler) holds another file's gate
// (api_auth_second_tu.cpp), and the other way round. Before, the table was a
// `static` in the header, one per file: the second file's was clean, and
// answered 403 to a host the first file held.
void test_one_table_for_every_file() {
  reset_device();
  Req first;
  first.bearer(kWrong);
  CHECK(qr(first) == 0);
  CHECK(first.r.status == "403 Forbidden");
  Req second;
  second.bearer(kToken);
  CHECK(!second_tu_check_bearer(&second.r, kToken));
  CHECK_MSG(second.r.status == "429 Too Many Requests",
            "another file's gate did not see this file's strike: " + second.r.status);
  reset_device();
  Req there;
  there.bearer(kWrong);
  CHECK(!second_tu_check_bearer(&there.r, kToken));
  CHECK(there.r.status == "403 Forbidden");
  Req here;
  here.bearer(kToken);
  CHECK(qr(here) == 0);
  CHECK_MSG(here.r.status == "429 Too Many Requests",
            "this file's gate did not see another file's strike: " + here.r.status);
}

// No backoff outlives the 60 s window that clears its count. Six wrong
// bearers, each once the one before was waited out: the sixth holds 60 s and
// its 429 says Retry-After 61, and the right token is served 60 s after it.
// Before, the sixth armed 64 s and announced 65, for a hold the window ended
// at 60 s (and at exactly 60 s the right token was still refused).
void test_no_backoff_outlives_the_window() {
  reset_device();
  for (int i = 0; i < 6; ++i) {
    wait_out(kHostA);
    Req q;
    q.bearer(kWrong);
    CHECK(qr(q) == 0);
    CHECK(q.r.status == "403 Forbidden");
  }
  CHECK(strikes(kHostA) == 6);
  Req held_req;
  held_req.bearer(kToken);
  CHECK(qr(held_req) == 0);
  CHECK(held_req.r.status == "429 Too Many Requests");
  CHECK_MSG(header(held_req.r, "Retry-After") == "61",
            "the sixth strike announced Retry-After " + header(held_req.r, "Retry-After"));
  CHECK(held_req.r.resp.find("\"retry_after_s\":61}") != std::string::npos);
  stub_grant::now_ms += 60000;
  Req after;
  after.bearer(kToken);
  CHECK_MSG(qr(after) == 1, "the sixth strike held the right token past 60 s");
}

// A backoff armed 1024 ms before millis() wraps still holds 500 ms later.
// Before, `millis() < until` read the wrapped end as long past.
void test_a_backoff_armed_before_the_wrap_holds() {
  reset_device();
  stub_grant::now_ms = 0xFFFFFFFFu - 1023u;
  Req wrong;
  wrong.bearer(kWrong);
  CHECK(qr(wrong) == 0);
  CHECK(wrong.r.status == "403 Forbidden");
  stub_grant::now_ms += 500;
  Req right;
  right.bearer(kToken);
  CHECK(qr(right) == 0);
  CHECK_MSG(right.r.status == "429 Too Many Requests",
            "a backoff armed before the wrap did not hold: " + right.r.status);
  CHECK(header(right.r, "Retry-After") == "2");   // 1500 ms left
  stub_grant::now_ms += 2000;   // past the wrap and past the backoff
  Req later;
  later.bearer(kToken);
  CHECK(qr(later) == 1);
}

// The count saturates. 256 wrong ?token= from A, each sent once the backoff
// the one before it armed has run out (inside a backoff none is compared or
// recorded, since F339), are 256 strikes, and A is still held. Before F343
// the uint8_t count wrapped to a clean 0 and A was let through.
void test_wrong_query_tokens_never_wrap_the_count() {
  reset_device();
  for (int i = 0; i < 256; ++i) {
    wait_out(kHostA);
    Req q;
    q.query(std::string("token=") + kWrong);
    CHECK(!api_auth_check_or_query(&q.r, kToken));
    CHECK(q.r.status == "401 Unauthorized");
  }
  CHECK(strikes(kHostA) == canary::net::auth_backoff::MAX_COUNT);
  CHECK(canary::net::auth_backoff::remaining_ms(g_auth_table, kHostA, millis()) ==
        canary::net::auth_backoff::MAX_MS);
  Req right;
  right.bearer(kToken);
  CHECK(qr(right) == 0);
  CHECK_MSG(right.r.status == "429 Too Many Requests", "256 strikes wrapped the count: " + right.r.status);
}

// A count is cleared 60 s after its last strike: two wrong ?token= 61 s apart
// are two first strikes, so the right token is served 2 s after the second.
// Before, the second armed 4 s. (That the window clears a count no lockout
// read asked about is test_auth_backoff.cpp's.)
void test_the_window_clears_a_query_tokens_count() {
  reset_device();
  Req q1;
  q1.query(std::string("token=") + kWrong);
  CHECK(!api_auth_check_or_query(&q1.r, kToken));
  stub_grant::now_ms += 61000;
  Req q2;
  q2.query(std::string("token=") + kWrong);
  CHECK(!api_auth_check_or_query(&q2.r, kToken));
  CHECK(strikes(kHostA) == 1);
  stub_grant::now_ms += canary::net::auth_backoff::BASE_MS + 1;
  Req right;
  right.bearer(kToken);
  CHECK_MSG(qr(right) == 1, "two strikes a minute apart armed more than 2 s: " + right.r.status);
}

// ── every bearer compare accounted (sweep F339) ──────────────────────────────

// The camera peek's and the vault download's ?token= is under its address's
// backoff. A armed at 3 strikes (8 s): before, A's right ?token= was served
// inside the backoff and cleared the count, its wrong ones were recorded
// during it (each one more strike), and its right header on these routes was
// served too.
void test_query_token_is_held_by_the_backoff() {
  reset_device();
  arm_lockout(3);
  Req right_q;
  right_q.query(std::string("token=") + kToken);
  CHECK_MSG(!api_auth_check_or_query(&right_q.r, kToken), "a right ?token= was served inside its address's backoff");
  CHECK(right_q.r.status == "429 Too Many Requests");
  CHECK(header(right_q.r, "Retry-After") == "9");   // 8000 ms left
  CHECK(right_q.r.resp == "{\"error\":\"too_many_requests\",\"retry_after_s\":9}");
  CHECK(right_q.r.sends == 1);
  CHECK_MSG(strikes(kHostA) == 3, "a right ?token= inside the backoff changed the count");
  Req right_h;
  right_h.bearer(kToken);
  CHECK(!api_auth_check_or_query(&right_h.r, kToken));
  CHECK(right_h.r.status == "429 Too Many Requests");
  CHECK(strikes(kHostA) == 3);
  Req wrong_q;
  wrong_q.query(std::string("token=") + kWrong);
  CHECK(!api_auth_check_or_query(&wrong_q.r, kToken));
  CHECK_MSG(wrong_q.r.status == "429 Too Many Requests", "a wrong ?token= inside the backoff got " + wrong_q.r.status);
  CHECK_MSG(strikes(kHostA) == 3, "a wrong ?token= inside the backoff was recorded");
  // 250 more wrong ?token= inside the backoff: none recorded, A still held.
  size_t held_answers = 0;
  for (int i = 0; i < 250; ++i) {
    Req q;
    q.query(std::string("token=") + kWrong);
    CHECK(!api_auth_check_or_query(&q.r, kToken));
    if (q.r.status == "429 Too Many Requests") ++held_answers;
  }
  CHECK(held_answers == 250);
  CHECK(strikes(kHostA) == 3 && held(kHostA));
  // ...so A's next wrong admin bearer is the 429 too, not compared.
  Req admin;
  admin.bearer(kWrong);
  CHECK(!api_auth_check_bearer(&admin.r, kToken));
  CHECK(admin.r.status == "429 Too Many Requests");
  // B's right ?token= is served at once, nothing sent, nothing recorded.
  Req b;
  b.from(192, 168, 1, 21).query(std::string("token=") + kToken);
  CHECK_MSG(api_auth_check_or_query(&b.r, kToken), "another address's right ?token= was held");
  CHECK(b.r.sends == 0);
  CHECK(strikes(kHostB) == 0);
  // Once A's backoff has run out, A's right ?token= is served and clears A.
  wait_out(kHostA);
  Req after;
  after.query(std::string("token=") + kToken);
  CHECK(api_auth_check_or_query(&after.r, kToken));
  CHECK(strikes(kHostA) == 0);
}

// One request is one grant over one candidate: the header when there is
// one, the ?token= only when there is none. A wrong header beside a wrong
// ?token= is one strike (the 2 s first backoff). A wrong header beside the
// right ?token= is refused and is that strike too: the query is not
// compared beside a header, so one strike buys one guessed token, as on
// every other route (review of F339: a request that was right if either
// matched tried two tokens for one strike, about 63 guesses a minute through
// these links where the docs say 30). The right ?token= alone is served and
// clears the count; neither, or a query without the key, is the 401 and no
// strike.
void test_or_query_counts_one_per_request() {
  reset_device();
  Req both;
  both.bearer(kWrong).query(std::string("token=") + kWrong);
  CHECK(!api_auth_check_or_query(&both.r, kToken));
  CHECK(both.r.status == "401 Unauthorized");
  CHECK(has_header(both.r, "WWW-Authenticate"));
  CHECK_MSG(strikes(kHostA) == 1, "one request counted " + std::to_string(strikes(kHostA)) + " strikes");
  CHECK(remaining_for(&both.r) == canary::net::auth_backoff::BASE_MS);
  wait_out(kHostA);
  Req mixed;
  mixed.bearer(kWrong).query(std::string("token=") + kToken);
  CHECK_MSG(!api_auth_check_or_query(&mixed.r, kToken),
            "a wrong header beside the right ?token= was served: two candidates for one strike");
  CHECK(mixed.r.status == "401 Unauthorized");
  CHECK_MSG(strikes(kHostA) == 2, "a wrong header beside the right ?token= was not one strike");
  wait_out(kHostA);
  Req query_alone;
  query_alone.query(std::string("token=") + kToken);
  CHECK(api_auth_check_or_query(&query_alone.r, kToken));
  CHECK(query_alone.r.sends == 0);
  CHECK(strikes(kHostA) == 0);
  Req bare;
  CHECK(!api_auth_check_or_query(&bare.r, kToken));
  CHECK(bare.r.status == "401 Unauthorized");
  Req other_key;
  other_key.query("t=123");
  CHECK(!api_auth_check_or_query(&other_key.r, kToken));
  CHECK(other_key.r.status == "401 Unauthorized");
  CHECK(strikes(kHostA) == 0);
}

// The right Authorization header alone, with no ?token=, is served: the
// header half of the one grant. Every bearer client of the peek stream, the
// snapshot and the vault download sends it so. Lost, that client's right
// header would be compared as wrong: a 401, a strike against its address,
// and a 429 while that strike's backoff ran, again and again. Past a
// backoff it frees its address, as a right bearer does everywhere else.
void test_or_query_serves_the_right_header_alone() {
  reset_device();
  Req right_h;
  right_h.bearer(kToken);
  CHECK_MSG(api_auth_check_or_query(&right_h.r, kToken), "the right Authorization header alone was refused");
  CHECK_MSG(right_h.r.sends == 0, "the right header alone got an answer: " + right_h.r.status);
  CHECK_MSG(strikes(kHostA) == 0, "the right header alone was a strike");
  set_strikes(kHostA, 1, millis(), millis());   // one strike, its backoff just passed
  Req after;
  after.bearer(kToken);
  CHECK_MSG(api_auth_check_or_query(&after.r, kToken), "the right header alone was refused past a backoff");
  CHECK(after.r.sends == 0);
  CHECK_MSG(strikes(kHostA) == 0, "the right header alone left its address's count standing");
  CHECK(!held(kHostA));
}

// The cookie branch of the ?token= gate (review of F339): a valid cv_session
// cookie is served and records nothing, so it never frees a held address. A
// cookie that cleared the backoff would let a session holder free its own
// address with one GET of a peek or vault link between bearer guesses on any
// other route, and the per-address lockout would no longer limit it. The
// dashboard's cookie-only <img> and download links rely on the branch, so
// the cookie alone must be served (deleting the branch refuses them 401).
void test_or_query_cookie_never_clears_the_backoff() {
  reset_device();
  mint_session();
  Req c;
  c.cookie();
  CHECK_MSG(api_auth_check_or_query(&c.r, kToken), "the session cookie alone was refused on the ?token= gate");
  CHECK_MSG(c.r.sends == 0, "the session cookie alone got an answer: " + c.r.status);
  CHECK(strikes(kHostA) == 0);
  arm_lockout(3);
  Req held_c;
  held_c.cookie();
  CHECK_MSG(api_auth_check_or_query(&held_c.r, kToken), "the session cookie was refused inside its address's backoff");
  CHECK(held_c.r.sends == 0);
  CHECK_MSG(strikes(kHostA) == 3, "the session cookie changed its address's count");
  CHECK_MSG(held(kHostA), "the session cookie freed its address's backoff");
  // A cookie beside a wrong ?token=: the cookie decides, no strike, A still
  // held at its own count.
  Req beside;
  beside.cookie().query(std::string("token=") + kWrong);
  CHECK(api_auth_check_or_query(&beside.r, kToken));
  CHECK(beside.r.sends == 0);
  CHECK(strikes(kHostA) == 3 && held(kHostA));
  // ...and A's next right bearer on an admin route is still the 429.
  Req admin;
  admin.bearer(kToken);
  CHECK(!api_auth_check_bearer(&admin.r, kToken));
  CHECK(admin.r.status == "429 Too Many Requests");
}

// The right token with a tail is a wrong token, through every compare that
// reads one (review of F339, F343): each refuses a candidate whose length
// differs from the device token's, and only that check refuses this one (a
// shorter token already fails the byte compare at its NUL; a compare of the
// device token's own length takes the right token plus anything after it).
void test_a_token_with_a_tail_is_wrong() {
  const std::string tailed = std::string(kToken) + "X";
  // The receipt's grant (api_auth_bearer_grant() over api_auth_bearer_valid()).
  reset_device();
  Req receipt_q;
  receipt_q.bearer(tailed.c_str());
  CHECK_MSG(receipt(receipt_q) == 0, "the receipt took the right token with a tail");
  check_gate_refusal(receipt_q.r);
  CHECK(strikes(kHostA) == 1);
  // The raw compare itself.
  reset_device();
  Req raw;
  raw.bearer(tailed.c_str());
  CHECK_MSG(!api_auth_bearer_valid(&raw.r, kToken), "api_auth_bearer_valid() took the right token with a tail");
  // Every admin route's gate.
  reset_device();
  Req admin;
  admin.bearer(tailed.c_str());
  CHECK_MSG(!api_auth_check_bearer(&admin.r, kToken), "api_auth_check_bearer() took the right token with a tail");
  CHECK(admin.r.status == "403 Forbidden");
  CHECK(admin.r.resp == "{\"error\":\"forbidden\"}");
  CHECK(strikes(kHostA) == 1);
  // The ?token= link's query compare.
  reset_device();
  Req link;
  link.query("token=" + tailed);
  CHECK_MSG(!api_auth_check_or_query(&link.r, kToken), "the ?token= gate took the right token with a tail");
  CHECK(link.r.status == "401 Unauthorized");
  CHECK(strikes(kHostA) == 1);
}

// The strike's console line names no address (auth_backoff.h's contract:
// addresses stay in RAM, and its callers never log one). It prints the
// count and the backoff of the slot that took the strike, and " (shared
// overflow)" only when the strike landed there.
void test_the_strike_line_names_no_address() {
  reset_device();
  stub_grant::serial_lines.clear();
  Req wrong;
  wrong.bearer(kWrong);   // from kHostA, 192.168.1.20
  CHECK(!api_auth_check_bearer(&wrong.r, kToken));
  CHECK_MSG(stub_grant::serial_lines.size() == 1, "a strike printed " + std::to_string(stub_grant::serial_lines.size()) + " lines");
  const std::string line = stub_grant::serial_lines.empty() ? "" : stub_grant::serial_lines.back();
  CHECK_MSG(line == "[AUTH] Failed attempt #1. Lockout: 2000 ms\n", "the strike's line: " + line);
  CHECK(line.find("192.168") == std::string::npos && line.find("c0a8") == std::string::npos &&
        line.find("C0A8") == std::string::npos && line.find("3232235796") == std::string::npos);
  // A second strike from A, once its backoff ended: A's own count.
  wait_out(kHostA);
  Req second;
  second.bearer(kWrong);
  CHECK(!api_auth_bearer_grant(&second.r, kToken));
  CHECK_MSG(stub_grant::serial_lines.back() == "[AUTH] Failed attempt #2. Lockout: 4000 ms\n",
            "the second strike's line: " + stub_grant::serial_lines.back());
  // Eight more addresses fill the remaining slots and one strikes the
  // overflow: its line carries the tag and the overflow's count, and still
  // names no address.
  for (uint8_t i = 1; i <= 7; ++i) {
    Req q;
    q.from(10, 0, 0, i).bearer(kWrong);
    CHECK(!api_auth_check_bearer(&q.r, kToken));
  }
  CHECK_MSG(stub_grant::serial_lines.back() == "[AUTH] Failed attempt #1. Lockout: 2000 ms\n",
            "a tracked address's first strike: " + stub_grant::serial_lines.back());
  Req untracked;
  untracked.from(10, 0, 0, 9).bearer(kWrong);
  CHECK(!api_auth_check_bearer(&untracked.r, kToken));
  CHECK(g_auth_table.overflow.failures == 1);
  const std::string over = stub_grant::serial_lines.back();
  CHECK_MSG(over == "[AUTH] Failed attempt #1 (shared overflow). Lockout: 2000 ms\n", "the overflow's line: " + over);
  CHECK(over.find("10.0.0") == std::string::npos);
  for (const std::string& l : stub_grant::serial_lines) {
    CHECK_MSG(l.find("192.168") == std::string::npos && l.find("10.0.0") == std::string::npos,
              "a console line names an address: " + l);
  }
}

// The /api/csi/* routes' gate (CSI_AUTH_OR_RETURN, cut from
// csi_integration.cpp): the iPhone's /api/csi/stream poll and every other
// tooling call with a bearer. Before, a wrong bearer here recorded nothing
// and the right one was served inside a backoff.
void test_csi_routes_account_every_bearer() {
  reset_device();
  Req wrong;
  wrong.bearer(kWrong);
  CHECK(csi_route(&wrong.r) == ESP_OK);
  CHECK(wrong.r.status == "401 Unauthorized");
  CHECK(has_header(wrong.r, "WWW-Authenticate"));
  CHECK_MSG(strikes(kHostA) == 1, "a wrong bearer on a CSI route was no strike");
  // Inside A's backoff, A's right bearer is not compared: the lockout's 429.
  Req right;
  right.bearer(kToken);
  CHECK(csi_route(&right.r) == ESP_OK);
  CHECK_MSG(right.r.status == "429 Too Many Requests", "a held bearer on a CSI route got " + right.r.status);
  CHECK(header(right.r, "Retry-After") == "3");   // 2000 ms left
  CHECK(right.r.resp == "{\"error\":\"too_many_requests\",\"retry_after_s\":3}");
  CHECK(right.r.sends == 1);
  CHECK(strikes(kHostA) == 1);
  // B's right bearer is served at once.
  Req b;
  b.from(192, 168, 1, 21).bearer(kToken);
  CHECK(csi_route(&b.r) == ESP_OK);
  CHECK(b.r.status == "200 OK" && b.r.resp == "{\"ok\":true}");
  CHECK(strikes(kHostB) == 0);
  // The dashboard's cookie is served inside A's backoff and records nothing.
  mint_session();
  Req cookie;
  cookie.cookie();
  CHECK(csi_route(&cookie.r) == ESP_OK);
  CHECK(cookie.r.status == "200 OK");
  // A poll with no Authorization header is the 401, never the 429 and no strike.
  Req bare;
  CHECK(csi_route(&bare.r) == ESP_OK);
  CHECK(bare.r.status == "401 Unauthorized");
  CHECK(strikes(kHostA) == 1);
  // After the backoff, A's right bearer is served and clears A.
  wait_out(kHostA);
  Req later;
  later.bearer(kToken);
  CHECK(csi_route(&later.r) == ESP_OK);
  CHECK(later.r.status == "200 OK");
  CHECK(strikes(kHostA) == 0);
  // No module token: the 503 it always was.
  g_api_token = nullptr;
  Req unconfigured;
  unconfigured.bearer(kToken);
  CHECK(csi_route(&unconfigured.r) == ESP_OK);
  CHECK(unconfigured.r.status == "503 Service Unavailable");
}

// The QR scan's status and stop gate (qr_scan_request_allowed(), cut from the
// sketch): the wizard's pair token first, then the admin fallback, which is
// now accounted. A held address's admin bearer is refused (these routes'
// own 403) without a compare; the pair token is no bearer and is read first,
// so the wizard is never held. Before, a wrong admin bearer here recorded
// nothing and a right one was taken inside a backoff.
void test_qr_scan_fallback_is_accounted() {
  reset_device();
  csi_integration::g_pair_token.assign(csi_integration::PAIR_TOKEN_HEX_LEN, 'b');
  arm_lockout(3);
  Req wizard;
  wizard.query("token=" + csi_integration::g_pair_token);
  CHECK_MSG(qr_scan_request_allowed(&wizard.r), "the wizard's pair token was held");
  CHECK(strikes(kHostA) == 3);
  Req held_admin;
  held_admin.bearer(kToken);
  CHECK_MSG(!qr_scan_request_allowed(&held_admin.r), "a held address's admin bearer was taken");
  CHECK(held_admin.r.sends == 0);
  CHECK(strikes(kHostA) == 3);
  Req b_wrong;
  b_wrong.from(192, 168, 1, 21).bearer(kWrong);
  CHECK(!qr_scan_request_allowed(&b_wrong.r));
  CHECK_MSG(strikes(kHostB) == 1, "a wrong admin bearer on the QR scan gate was no strike");
  wait_out(kHostB);
  Req b_right;
  b_right.from(192, 168, 1, 21).bearer(kToken);
  CHECK(qr_scan_request_allowed(&b_right.r));
  CHECK(strikes(kHostB) == 0);
  mint_session();
  Req cookie;
  cookie.cookie();
  CHECK(qr_scan_request_allowed(&cookie.r));
  CHECK(strikes(kHostA) == 3);
}

// ── api_auth.h's contract ────────────────────────────────────────────────────

void test_api_auth_contract() {
  reset_device();
  mint_session();
  Req cookie_only;
  cookie_only.cookie();
  CHECK(api_auth_check(&cookie_only.r, kToken));            // the dashboard's routes: the cookie signs in
  CHECK(cookie_only.r.sends == 0);
  CHECK(api_auth_check_optional(&cookie_only.r, kToken));   // and the silent variant takes it too
  CHECK(!api_auth_bearer_valid(&cookie_only.r, kToken));    // the bearer-only checks never do
  CHECK(!api_auth_check_bearer(&cookie_only.r, kToken));
  CHECK(cookie_only.r.status == "401 Unauthorized");
  Req cookie_grant;
  cookie_grant.cookie();
  CHECK(!api_auth_bearer_grant(&cookie_grant.r, kToken));   // the receipt's grant never takes it either
  CHECK(cookie_grant.r.sends == 0);                          // and sends nothing
  CHECK(!api_auth_bearer_held(&cookie_grant.r));

  Req bearer;
  bearer.bearer(kToken);
  CHECK(api_auth_bearer_valid(&bearer.r, kToken));
  CHECK(api_auth_check_bearer(&bearer.r, kToken));
  CHECK(bearer.r.sends == 0);

  Req wrong;
  wrong.cookie().bearer(kWrong);
  CHECK(api_auth_check(&wrong.r, kToken));                  // the cookie carries an admin route past a wrong bearer...
  CHECK(!api_auth_bearer_valid(&wrong.r, kToken));          // ...and not a credential route
  CHECK(!api_auth_check_bearer(&wrong.r, kToken));
  CHECK(wrong.r.status == "403 Forbidden");

  // The cookie path clears no lockout (api_auth_check's own rule).
  set_strikes(kHostA, 4, millis(), millis());
  Req again;
  again.cookie();
  CHECK(api_auth_check(&again.r, kToken));
  CHECK(strikes(kHostA) == 4);

  // Every bearer compare is accounted (sweep F339). api_auth_check_optional()
  // is the cookie or api_auth_bearer_grant(): with no cookie, a wrong bearer
  // is one strike for its address (before: none, it compared silently).
  clear_lockout();
  Req opt_wrong;
  opt_wrong.bearer(kWrong);
  bool held_out = true;
  CHECK(!api_auth_check_optional(&opt_wrong.r, kToken, &held_out));
  CHECK(!held_out);                                          // compared, not held
  CHECK(opt_wrong.r.sends == 0);                             // and nothing sent
  CHECK_MSG(strikes(kHostA) == 1, "a wrong bearer through api_auth_check_optional was no strike");
  // Inside that address's backoff the right bearer is not compared (a compare
  // would have cleared the count): false, with held reported.
  Req opt_held;
  opt_held.bearer(kToken);
  held_out = false;
  CHECK_MSG(!api_auth_check_optional(&opt_held.r, kToken, &held_out),
            "api_auth_check_optional took the right bearer inside its address's backoff");
  CHECK(held_out);
  CHECK(opt_held.r.sends == 0);
  CHECK(strikes(kHostA) == 1 && held(kHostA));
  // The cookie is true with held false, beside a wrong bearer too, and
  // records nothing.
  Req opt_cookie;
  opt_cookie.cookie().bearer(kWrong);
  held_out = true;
  CHECK(api_auth_check_optional(&opt_cookie.r, kToken, &held_out));
  CHECK(!held_out);
  CHECK(strikes(kHostA) == 1);
  // No Authorization header: false, not held, nothing recorded.
  Req opt_bare;
  held_out = true;
  CHECK(!api_auth_check_optional(&opt_bare.r, kToken, &held_out));
  CHECK(!held_out);
  CHECK(strikes(kHostA) == 1);
  // Another address's right bearer is served while A is held.
  Req opt_b;
  opt_b.from(192, 168, 1, 21).bearer(kToken);
  held_out = true;
  CHECK(api_auth_check_optional(&opt_b.r, kToken, &held_out));
  CHECK(!held_out);
  CHECK(strikes(kHostB) == 0);
  // api_auth_bearer_grant()'s own held: the grant's reading, set only when
  // the backoff kept the header from being compared.
  Req grant_held;
  grant_held.bearer(kToken);
  held_out = false;
  CHECK(!api_auth_bearer_grant(&grant_held.r, kToken, &held_out));
  CHECK(held_out);
  Req grant_b_wrong;
  grant_b_wrong.from(192, 168, 1, 21).bearer(kWrong);
  held_out = true;
  CHECK(!api_auth_bearer_grant(&grant_b_wrong.r, kToken, &held_out));
  CHECK(!held_out);                                          // compared: a strike, not a hold
  CHECK(strikes(kHostB) == 1);
}

// An empty device token is no token. provision_device() can return before
// the token is minted (a key that will not generate or store) while setup()
// carries on to the servers, so g_device.api_token_str can be "" on a unit
// that answers HTTP. A zero-length compare would then take a bare "Bearer "
// as the right token: the receipt's secrets and the QR for anyone.
void test_empty_device_token_takes_no_bearer() {
  for (const char* raw : {"Bearer ", "Bearer    "}) {
    reset_device();
    g_device.api_token_str[0] = '\0';
    Req valid;
    valid.authorization(raw);
    CHECK_MSG(!api_auth_bearer_valid(&valid.r, g_device.api_token_str),
              "an empty device token took a bare Bearer header");
    CHECK(valid.r.sends == 0);
    CHECK(strikes(kHostA) == 0);   // the raw compare records nothing

    // api_auth_check_optional() is the accounted grant since F339: a
    // presented header against no token is one strike.
    Req optional;
    optional.authorization(raw);
    CHECK(!api_auth_check_optional(&optional.r, g_device.api_token_str));
    CHECK(optional.r.sends == 0);
    CHECK(strikes(kHostA) == 1);
    clear_lockout();

    Req grant;
    grant.authorization(raw);
    CHECK(!api_auth_bearer_grant(&grant.r, g_device.api_token_str));
    CHECK(strikes(kHostA) == 1);   // a presented header against no token: one strike
    clear_lockout();

    Req admin;
    admin.authorization(raw);
    CHECK_MSG(!api_auth_check_bearer(&admin.r, g_device.api_token_str),
              "an empty device token signed in an admin route");
    CHECK(admin.r.status == "403 Forbidden");
    CHECK(strikes(kHostA) == 1);
    clear_lockout();

    Req receipt_req;
    receipt_req.authorization(raw);
    CHECK_MSG(receipt(receipt_req) == 0, "an empty device token served the receipt to a bare Bearer");
    check_gate_refusal(receipt_req.r);
    CHECK(g_served.empty());
    clear_lockout();

    Req qr_req;
    qr_req.authorization(raw);
    CHECK_MSG(qr(qr_req) == 0, "an empty device token served the pairing QR to a bare Bearer");
    CHECK(qr_req.r.status == "403 Forbidden");
    clear_lockout();
  }
  // The ?token= gate, the same rule: an empty query token is not the token.
  reset_device();
  g_device.api_token_str[0] = '\0';
  Req query_req;
  query_req.query("token=");
  CHECK_MSG(!api_auth_check_or_query(&query_req.r, g_device.api_token_str),
            "an empty device token took an empty ?token=");
  CHECK(query_req.r.status == "401 Unauthorized");
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

std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> lines;
  size_t at = 0;
  while (at <= s.size()) {
    size_t end = s.find('\n', at);
    if (end == std::string::npos) end = s.size();
    lines.push_back(s.substr(at, end - at));
    at = end + 1;
  }
  return lines;
}

std::string rtrim(const std::string& s) {
  size_t e = s.size();
  while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
  return s.substr(0, e);
}

// A function as cut_functions.awk cuts it: from the line that starts with
// `sig` to the first line that is exactly "}". Empty when not found once.
std::string function_text(const std::vector<std::string>& lines, const std::string& sig) {
  std::string text;
  int found = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].compare(0, sig.size(), sig) != 0) continue;
    ++found;
    for (size_t j = i; j < lines.size(); ++j) {
      text += lines[j];
      text += '\n';
      if (lines[j] == "}") break;
    }
  }
  return found == 1 ? text : "";
}

// The definition a line sits in: the nearest line at or above it that starts
// at column 0 with a name (not a brace, a directive or a blank) and is not a
// prototype or a continued argument list. Its first word up to '(' / '{'.
std::string enclosing(const std::vector<std::string>& lines, size_t at) {
  for (size_t j = at + 1; j-- > 0;) {
    const std::string l = rtrim(lines[j]);
    if (l.empty()) continue;
    const char c = l[0];
    if (c == ' ' || c == '\t' || c == '}' || c == '#' || c == '/') continue;
    if (l.back() == ';' || l.back() == ',') continue;
    size_t cut = l.find_first_of("({=");
    std::string head = cut == std::string::npos ? l : l.substr(0, cut);
    head = rtrim(head);
    const size_t sp = head.find_last_of(" \t*&");
    return sp == std::string::npos ? head : head.substr(sp + 1);
  }
  return "";
}

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }
// How many times `needle` occurs in `s` (non-overlapping).
size_t count_of(const std::string& s, const char* needle) {
  const size_t n = std::strlen(needle);
  size_t c = 0;
  for (size_t at = s.find(needle); n && at != std::string::npos; at = s.find(needle, at + n)) ++c;
  return c;
}
// `word` in `s` as a whole identifier: no identifier character on either side
// ("returned" and "return_ms" are not `return`).
bool has_word(const std::string& s, const char* word) {
  const auto ident = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
  const size_t n = std::strlen(word);
  for (size_t at = s.find(word); at != std::string::npos; at = s.find(word, at + 1)) {
    if ((at == 0 || !ident(s[at - 1])) && (at + n >= s.size() || !ident(s[at + n]))) return true;
  }
  return false;
}

// ── loop()'s sweep (sweep F347) ──────────────────────────────────────────────
// The two statements loop() runs every pass, spelled once: the harness runs
// them and the text check below pins the sketch to the same spelling.
#define LOOP_SWEEP_GATE \
  canary::net::provisioning_gate::expire(g_provisioning_gate, millis(), PROVISIONING_GATE_TTL_MS)
#define LOOP_SWEEP_CLAIM canary::net::claim_ticket::expire(g_improv_claim, millis())
#define RG_STR(x) #x
#define RG_XSTR(x) RG_STR(x)
const std::string kLoopSweepGate = std::string("  ") + RG_XSTR(LOOP_SWEEP_GATE) + ";";
const std::string kLoopSweepClaim = std::string("  ") + RG_XSTR(LOOP_SWEEP_CLAIM) + ";";

// The sketch's loop(), comments stripped, as cut_functions.awk would cut it.
const std::string& sketch_loop() {
  static const std::string text = function_text(split_lines(strip_comments(read_file(CANARY_WAP_INO))),
                                                "void loop() {");
  return text;
}
// A line of `fn` that is exactly `stmt` (its two-space indent included: a
// statement at loop()'s top level, not inside a branch).
bool has_line(const std::string& fn, const std::string& stmt) {
  for (const auto& l : split_lines(fn)) if (rtrim(l) == stmt) return true;
  return false;
}

// One pass of loop()'s sweep, as the sketch runs it: each statement runs
// only when the sketch's loop() holds it, so a sketch without the sweep (the
// code before F347) runs none, and the row below sees what that device did.
void loop_sweep_pass() {
  if (has_line(sketch_loop(), kLoopSweepGate)) LOOP_SWEEP_GATE;
  if (has_line(sketch_loop(), kLoopSweepClaim)) LOOP_SWEEP_CLAIM;
}

// A tap nobody took and a claim read but never spent, 2^32 ms later (sweep
// F347). Before: millis() came back round to both stamps, the bare request
// was served the receipt as BOOT_TAP with no press, and the old hex took it
// as SETUP_DOOR_CLAIM.
void test_receipt_stale_tap_and_claim_do_not_revive_after_the_wrap() {
  reset_device();
  CHECK(!sketch_loop().empty());
  const uint32_t t0 = millis();
  open_tap();
  mint_claim();
  const std::string old_hex = g_improv_claim.hex;
  CHECK(tap_open() && claim_pending());
  // loop() passes inside the TTLs leave both alone.
  stub_grant::now_ms = t0 + PROVISIONING_GATE_TTL_MS - 1;
  loop_sweep_pass();
  CHECK_MSG(gate_stamp() != 0 && claim_pending(), "a sweep inside the TTLs cleared a fresh stamp");
  // Past both TTLs, the next pass sweeps both. Nothing asked meanwhile.
  stub_grant::now_ms = t0 + canary::net::claim_ticket::TTL_MS + 1;
  loop_sweep_pass();
  CHECK_MSG(gate_stamp() == 0, "loop() left a tap past its TTL");
  CHECK_MSG(__atomic_load_n(&g_improv_claim.minted_at_ms, __ATOMIC_ACQUIRE) == 0,
            "loop() left a claim past its TTL");
  // millis() comes back round: 2^32 ms + 1 s after the tap and the mint.
  stub_grant::now_ms = (uint32_t)((uint64_t)t0 + (1ull << 32) + 1000);
  CHECK(stub_grant::now_ms == t0 + 1000);
  Req bare;
  CHECK_MSG(receipt(bare) == 0, "a tap nobody took served the receipt 2^32 ms after the press");
  check_gate_refusal(bare.r);
  Req claim;
  claim.query("claim=" + old_hex);
  CHECK_MSG(receipt(claim) == 0, "a claim read but never spent took the receipt 2^32 ms after the mint");
  check_gate_refusal(claim.r);
  CHECK(g_served.empty());
  CHECK(g_improv_claim_spent_at_ms == 0);
}

void test_sketch_text() {
  const std::string raw = read_file(CANARY_WAP_INO);
  CHECK_MSG(!raw.empty(), std::string("could not read ") + CANARY_WAP_INO);
  if (raw.empty()) return;
  const std::vector<std::string> lines = split_lines(strip_comments(raw));

  // The receipt's handler names only the bearer-only check and never the
  // cookie bridge; the pairing QR's names only the bearer-only gate.
  const std::string receipt_fn = function_text(lines, "static esp_err_t handle_provisioning_receipt(httpd_req_t* req) {");
  CHECK(!receipt_fn.empty());
  // The Host question is the handler's first statement after the request
  // count (sweep F344), so it runs before the bearer is read, the claim is
  // taken and the tap is judged.
  {
    std::vector<std::string> code;
    for (const auto& l : split_lines(receipt_fn)) if (!rtrim(l).empty()) code.push_back(rtrim(l));
    CHECK(code.size() > 2 && code[1] == "  g_health.http_requests++;");
    CHECK_MSG(code.size() > 2 && code[2] == "  if (!request_host_names_device(req)) {",
              "the receipt's first statement is not the Host question: " + (code.size() > 2 ? code[2] : std::string()));
    const size_t host_at = receipt_fn.find("request_host_names_device(req)");
    CHECK(host_at != std::string::npos && host_at < receipt_fn.find("api_auth_bearer_held(req)") &&
          host_at < receipt_fn.find("claim_ticket::take(") && host_at < receipt_fn.find("provisioning_gate_take_for("));
    CHECK(has(receipt_fn, "return httpd_resp_sendstr(req, \"{\\\"error\\\":\\\"host\\\"}\");"));
  }
  CHECK(has(receipt_fn, "api_auth_bearer_grant(req, g_device.api_token_str)"));
  CHECK(has(receipt_fn, "api_auth_bearer_held(req)"));
  CHECK_MSG(!has(receipt_fn, "api_auth_check_optional("), "the receipt takes the cookie again");
  CHECK_MSG(!has(receipt_fn, "api_auth_check("), "the receipt takes the cookie again");
  CHECK(!has(receipt_fn, "api_auth_check_or_query("));
  CHECK(!has(receipt_fn, "cv_session_validate"));
  CHECK(!has(receipt_fn, "session_validate_cookie"));
  CHECK(has(receipt_fn, "claim_ticket::take("));
  // The tap goes to one asker (sweep F342): the handler takes it only
  // through provisioning_gate_take_for(), which judges the asker
  // (tap_arbitrate) and takes exactly the judged stamp (take_exact), never
  // the unjudged take(). BLE OTA break-glass keeps the immediate take.
  CHECK(has(receipt_fn, "!claim_served && !provisioning_gate_take_for(req, &tap)"));
  CHECK_MSG(!has(receipt_fn, "provisioning_gate_take()"), "the receipt takes the tap unjudged again");
  CHECK(has(receipt_fn, "\"409 Conflict\""));
  const std::string take_for = function_text(lines, "static bool provisioning_gate_take_for(");
  CHECK(!take_for.empty());
  CHECK(has(take_for, "request_peer_key(req, key)"));
  CHECK(has(take_for, "pg::peek(g_provisioning_gate)"));
  CHECK(has(take_for, "pg::tap_arbitrate(g_tap_arbiter, seen, key, millis(), PROVISIONING_GATE_TTL_MS)"));
  CHECK(has(take_for, "pg::take_exact(g_provisioning_gate, seen)"));
  CHECK_MSG(!has(take_for, "::take(") && !has(take_for, "provisioning_gate_take()"),
            "provisioning_gate_take_for() takes a stamp it did not judge");
  // The NO_TAP step burns the expired stamp it peeked with a compare-and-
  // swap, never a close() that would clear a re-tap stored since the peek
  // (test_a_retap_during_a_no_tap_ask_is_kept drives the race).
  CHECK_MSG(has_line(take_for, "  if (seen != 0) pg::take_exact(g_provisioning_gate, seen);"),
            "the NO_TAP step does not burn exactly the stamp it peeked");
  CHECK_MSG(!has(take_for, "close("), "provisioning_gate_take_for() closes the gate outright");
  // The stamp is read once: every swap is of the stamp the arbiter judged,
  // never of one read again after it (a re-peek, or an expire() of whatever
  // the gate holds, clears a re-tap nobody judged;
  // test_a_retap_during_a_contended_close_is_kept drives the CONTENDED race).
  CHECK_MSG(count_of(take_for, "peek(") == 1, "provisioning_gate_take_for() reads the stamp more than once");
  CHECK_MSG(!has(take_for, "expire("), "provisioning_gate_take_for() sweeps a stamp it did not judge");
  CHECK_MSG(count_of(take_for, "take_exact(") == 3 &&
                count_of(take_for, "pg::take_exact(g_provisioning_gate, seen)") == 3,
            "a swap in provisioning_gate_take_for() is not of the judged stamp");
  CHECK_MSG(has_line(take_for,
                     "    if (seen != 0 && seen == g_tap_arbiter.contended_tap && pg::take_exact(g_provisioning_gate, seen)) {"),
            "the CONTENDED step does not close exactly the stamp it judged");
  const std::string ble = function_text(lines, "static bool ble_ota_break_glass_take() {");
  CHECK(!ble.empty());
  CHECK(has(ble, "provisioning_gate_take()"));
  // The sketch's gate is the header's State: no inline copy remains.
  size_t gate_decls = 0;
  for (const auto& l : lines) {
    if (has(l, "g_provisioning_gate_opened_at")) CHECK_MSG(false, "the inline gate copy is back: " + rtrim(l));
    if (has(l, "static canary::net::provisioning_gate::State g_provisioning_gate")) ++gate_decls;
  }
  CHECK(gate_decls == 1);
  // Every touch of the gate goes through the header (open() stores 0 as 1
  // with a release store; peek(), take(), take_exact() and expire() are its
  // atomics): no line reaches into the State's word.
  std::string raw_gate_lines;
  for (const auto& l : lines) {
    if (has(l, "g_provisioning_gate.") || has(l, "&g_provisioning_gate")) raw_gate_lines += "\n" + rtrim(l);
  }
  CHECK_MSG(raw_gate_lines.empty(), "the sketch touches the gate's word directly:" + raw_gate_lines);

  // loop() sweeps the tap and the claim every pass (sweep F347): both
  // statements at its top level, after handle_serial_commands();.
  const std::string loop_fn = sketch_loop();
  CHECK(!loop_fn.empty());
  const size_t serial_at = loop_fn.find("\n  handle_serial_commands();\n");
  const size_t gate_at = loop_fn.find("\n" + kLoopSweepGate + "\n");
  const size_t claim_at = loop_fn.find("\n" + kLoopSweepClaim + "\n");
  CHECK(serial_at != std::string::npos);
  CHECK_MSG(gate_at != std::string::npos, "loop() does not sweep the BOOT tap: " + kLoopSweepGate);
  CHECK_MSG(claim_at != std::string::npos, "loop() does not sweep the setup-door claim: " + kLoopSweepClaim);
  CHECK(serial_at != std::string::npos && gate_at != std::string::npos && serial_at < gate_at);
  CHECK(serial_at != std::string::npos && claim_at != std::string::npos && serial_at < claim_at);
  // Nothing returns out of loop() before the sweeps run: every pass reaches
  // them (the flagship's row holds its sweep as handle_boot_button()'s first
  // statement; the WAP's sweeps sit mid-loop(), so the text before them is
  // what must not return).
  if (gate_at != std::string::npos && claim_at != std::string::npos) {
    const std::string before = loop_fn.substr(0, std::min(gate_at, claim_at));
    CHECK_MSG(!has_word(before, "return"), "loop() can return before it sweeps the tap and the claim");
  }
  // The BOOT short press opens the gate through the header's open(), the
  // branch's first statement.
  {
    const std::string branch = "} else if (duration >= BOOT_SHORT_PRESS_MS) {\n";
    const size_t at = loop_fn.find(branch);
    CHECK_MSG(at != std::string::npos, "loop()'s BOOT short-press branch was not found");
    if (at != std::string::npos) {
      const std::vector<std::string> rest = split_lines(loop_fn.substr(at + branch.size()));
      std::string first;
      for (const auto& l : rest) {
        if (!rtrim(l).empty()) { first = rtrim(l); break; }
      }
      CHECK_MSG(first == "      canary::net::provisioning_gate::open(g_provisioning_gate, millis());",
                "the BOOT short press does not open the gate through the header: " + first);
    }
  }
  size_t gate_sweeps = 0, claim_sweeps = 0;
  for (const auto& l : lines) {
    if (has(l, "provisioning_gate::expire(")) ++gate_sweeps;
    if (has(l, "claim_ticket::expire(")) ++claim_sweeps;
  }
  CHECK_MSG(gate_sweeps == 1 && claim_sweeps == 1, "the sweeps are loop()'s, once each");

  const std::string qr_fn = function_text(lines, "static esp_err_t handle_pairing_qr(httpd_req_t* req) {");
  CHECK(!qr_fn.empty());
  CHECK(has(qr_fn, "api_auth_check_bearer(req, g_device.api_token_str)"));
  CHECK_MSG(!has(qr_fn, "api_auth_check(") && !has(qr_fn, "api_auth_check_optional(") && !has(qr_fn, "api_auth_check_or_query("),
            "the pairing QR takes the cookie again");
  CHECK(!has(qr_fn, "cv_session_validate"));
  CHECK(!has(qr_fn, "session_validate_cookie"));

  // POST /api/vault/key and POST /api/mesh/pair/confirm are registered once
  // each, on the two wrappers test_credential_grants_take_the_bearer drives
  // (F335, F346). Without this, a session wrapper registered in their place
  // passed every check here (the owner-authority PR's second review).
  {
    struct Reg { const char* uri_method; const char* line; };
    const Reg regs[] = {
        {"\"/api/vault/key\", .method = HTTP_POST,",
         "httpd_uri_t vault_key_set = { .uri = \"/api/vault/key\", .method = HTTP_POST, .handler = handle_vault_key_set_auth };"},
        {"\"/api/mesh/pair/confirm\", .method = HTTP_POST,",
         "httpd_uri_t mesh_pair_confirm = { .uri = \"/api/mesh/pair/confirm\", .method = HTTP_POST, .handler = handle_mesh_pair_confirm_auth };"},
    };
    for (const Reg& g : regs) {
      size_t registered = 0, on_wrapper = 0;
      for (const auto& l : lines) {
        if (l.find(std::string(".uri = ") + g.uri_method) != std::string::npos) ++registered;
        if (rtrim(l).find(g.line) != std::string::npos) ++on_wrapper;
      }
      CHECK_MSG(registered == 1 && on_wrapper == 1, std::string("not registered once, on its bearer wrapper: ") + g.line);
    }
  }

  // The receipt writer is called from the receipt handler and nowhere else.
  size_t writer_calls = 0, in_handler = 0;
  for (const auto& l : lines) if (has(l, "send_provisioning_receipt(") && !has(l, "static esp_err_t send_provisioning_receipt(")) ++writer_calls;
  for (const auto& l : split_lines(receipt_fn)) if (has(l, "send_provisioning_receipt(")) ++in_handler;
  CHECK(in_handler >= 1);
  CHECK_MSG(writer_calls == in_handler, "send_provisioning_receipt() is called outside handle_provisioning_receipt()");

  // Every line that names either secret: a comparison, a hand-over to a
  // module's own gate, key management, the serial console or the boot
  // banner, or one of the two HTTP writers.
  static const char* const kShapes[] = {
      "api_auth_check(", "api_auth_check_or_query(", "api_auth_check_optional(", "api_auth_check_bearer(",
      "api_auth_bearer_valid(", "api_auth_bearer_grant(",         // compared, never written
      "::register_routes(", "csi_integration::init(",            // handed to a module's gate
      "nvs_load_token(", "nvs_store_token(", "derive_api_token(", "derive_ap_password(",
      "strlen(g_device.ap_password)", "snprintf(out_password",    // key management
      "auth_redact_token(",                                       // the console's redacted echo
      "Serial.printf(", "boot_kv(", "onboard::qr_line(",          // the serial console and the boot banner
  };
  static const std::set<std::string> kWriters = {"send_provisioning_receipt", "handle_pairing_qr"};
  std::set<std::string> writers_seen;
  size_t secret_lines = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& l = lines[i];
    if (!has(l, "g_device.api_token_str") && !has(l, "g_device.ap_password")) continue;
    ++secret_lines;
    bool shaped = false;
    for (const char* s : kShapes) if (has(l, s)) shaped = true;
    if (shaped) continue;
    const std::string fn = enclosing(lines, i);
    CHECK_MSG(kWriters.count(fn) == 1,
              "line " + std::to_string(i + 1) + " hands a secret to `" + fn + "`: " + rtrim(l));
    if (kWriters.count(fn)) writers_seen.insert(fn);
  }
  CHECK(secret_lines > 20);                   // the scan read the sketch
  CHECK(writers_seen == kWriters);            // both writers were seen, and only they
  // The struct's own members are declared, not written.
  size_t members = 0;
  for (const auto& l : lines) if (has(l, "api_token_str[36];") || has(l, "ap_password[16];")) ++members;
  CHECK(members == 2);
}

}  // namespace

// POST /api/vault/key (F335) and POST /api/mesh/pair/confirm (F346): the
// landing page's session alone is 401, no strike, and the handler never
// runs; a wrong bearer beside it is 403 and one strike for its address
// alone; another host's right bearer is served at once; the striking host's
// right bearer is 429 until its backoff runs out, then served. Before the fix
// both wrappers were api_auth_check(), which the cookie passes: a host on the
// network re-keyed the vault, or handed the opera's secret to its own radio.
void test_credential_grants_take_the_bearer() {
  struct Route { const char* name; esp_err_t (*wrapper)(httpd_req_t*); int* ran; };
  const Route routes[] = {
      {"vault key", handle_vault_key_set_auth, &g_vault_key_sets},
      {"mesh confirm", handle_mesh_pair_confirm_auth, &g_mesh_confirms},
  };
  for (const Route& rt : routes) {
    reset_device();
    mint_session();
    *rt.ran = 0;
    Req s;
    s.cookie();
    CHECK(cv_session_validate(&s.r));               // the bridge takes this cookie...
    CHECK(rt.wrapper(&s.r) == ESP_OK);
    CHECK_MSG(s.r.status == "401 Unauthorized" && *rt.ran == 0, rt.name);   // ...the grant does not
    CHECK_MSG(has_header(s.r, "WWW-Authenticate"), rt.name);
    CHECK_MSG(strikes(kHostA) == 0, rt.name);       // no credential, no strike
    Req w;
    w.cookie().bearer(kWrong);
    CHECK(rt.wrapper(&w.r) == ESP_OK);
    CHECK_MSG(w.r.status == "403 Forbidden" && *rt.ran == 0, rt.name);
    CHECK_MSG(strikes(kHostA) == 1 && held(kHostA), rt.name);
    Req b;                                          // another host, at once
    b.from(192, 168, 1, 21).bearer(kToken);
    CHECK(rt.wrapper(&b.r) == ESP_OK);
    CHECK_MSG(b.r.status == "200 OK" && *rt.ran == 1, rt.name);
    CHECK_MSG(strikes(kHostB) == 0 && strikes(kHostA) == 1, rt.name);
    Req a;                                          // the striking host, at once
    a.cookie().bearer(kToken);
    CHECK(rt.wrapper(&a.r) == ESP_OK);
    CHECK_MSG(a.r.status == "429 Too Many Requests" && has_header(a.r, "Retry-After") && *rt.ran == 1, rt.name);
    wait_out(kHostA);
    Req c;
    c.bearer(kToken);
    CHECK(rt.wrapper(&c.r) == ESP_OK);
    CHECK_MSG(c.r.status == "200 OK" && *rt.ran == 2, rt.name);
    CHECK_MSG(strikes(kHostA) == 0, rt.name);       // the right bearer frees its address
  }
  std::printf("PASS credential_grants_take_the_bearer\n");
}

int main() {
  test_receipt_cookie_alone_is_refused();
  test_receipt_cookie_beside_wrong_bearer_is_refused();
  test_receipt_bearer_serves_and_spends_nothing();
  test_receipt_cookie_beside_tap_is_the_tap();
  test_receipt_cookie_beside_claim_is_the_claim();
  test_receipt_bare_tap_unchanged();
  test_receipt_cookie_secure_over_https_only();
  test_receipt_wrong_claim_leaves_it_pending();
  test_wrong_claims_log_once_per_ten_seconds();
  test_two_hosts_in_one_tap_are_both_refused();
  test_sole_asker_is_served_at_once();
  test_first_asker_after_the_tap_loses_to_a_waiting_owner();
  test_ipv6_askers_are_two_askers();
  test_what_counts_as_an_ask();
  test_break_glass_leaves_nothing_to_take();
  test_contention_does_not_carry_into_a_clean_next_tap();
  test_the_owner_retries_after_the_other_device_stops();
  test_a_served_retap_ends_the_contention();
  test_a_take_lost_after_the_judgment_is_refused();
  test_a_retap_during_a_no_tap_ask_is_kept();
  test_a_retap_during_a_contended_close_is_kept();
  test_unreadable_peer_is_refused_and_leaves_the_tap();
  test_held_bearer_under_contention_is_429_and_the_409_clears_nothing();
  test_an_expired_tap_is_burned_by_the_request_that_finds_it();
  test_receipt_foreign_host_is_refused_first();
  test_receipt_stale_tap_and_claim_do_not_revive_after_the_wrap();
  test_receipt_bearer_shapes();
  test_receipt_wrong_bearer_is_a_strike();
  test_receipt_right_bearer_clears_the_count();
  test_receipt_lockout_holds_the_bearer();
  test_receipt_arming_guess_then_backoff();
  test_receipt_lockout_never_holds_the_tap_or_the_claim();
  test_the_hand_over_clears_only_its_asker();
  test_receipt_lockout_answer_never_wraps();
  test_qr_cookie_alone_is_refused();
  test_qr_cookie_beside_wrong_bearer_is_refused();
  test_qr_bearer_serves();
  test_qr_no_credential_is_401_without_strike();
  test_qr_bearer_shapes();
  test_qr_lockout_is_every_admin_routes();
  test_receipt_and_qr_share_the_lockout();
  test_one_hosts_strikes_never_hold_another();
  test_the_review_replay_holds_no_other_host();
  test_unknown_address_shares_one_bucket();
  test_one_table_for_every_file();
  test_no_backoff_outlives_the_window();
  test_a_backoff_armed_before_the_wrap_holds();
  test_wrong_query_tokens_never_wrap_the_count();
  test_the_window_clears_a_query_tokens_count();
  test_query_token_is_held_by_the_backoff();
  test_or_query_counts_one_per_request();
  test_or_query_serves_the_right_header_alone();
  test_or_query_cookie_never_clears_the_backoff();
  test_a_token_with_a_tail_is_wrong();
  test_the_strike_line_names_no_address();
  test_csi_routes_account_every_bearer();
  test_qr_scan_fallback_is_accounted();
  test_api_auth_contract();
  test_empty_device_token_takes_no_bearer();
  test_sketch_text();
  test_credential_grants_take_the_bearer();

  if (g_failures) {
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d receipt-grant checks PASSED\n", g_checks);
  return 0;
}
