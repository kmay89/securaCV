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
// they spend, provisioning_gate_take, and the cookie writer the receipt
// sends, session_cookie_header; firmware/tests_host/
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
//     the cookie Secure when the answer leaves over the HTTPS server (the tap
//     and the claim alike), never on a plain-HTTP boot, where the first-boot
//     SoftAP setup's recovery-kit save mints it (sweep F289's residual; the
//     writer, session_cookie_header(), is cut with the handlers, and
//     test_session_cookie.cpp drives handle_ui's mint);
//   - a wrong claim burns the claim (as before); a header that is not
//     "Bearer " is no bearer;
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
//     beside a wrong ?token= is one strike, and the right header alone is
//     served and frees its address (before: the right ?token= was served
//     inside the backoff and cleared it, the wrong ones were recorded during
//     it);
//   - api_auth.h's contract: api_auth_check() and api_auth_check_optional()
//     still take the cookie (the dashboard's routes), api_auth_bearer_valid()
//     and api_auth_check_bearer() never do, and the cookie path never clears
//     the lockout; an empty device token (provision_device() can stop before
//     minting it) matches no bearer and no ?token=, on every gate, the
//     receipt and the QR included;
//   - the sketch's text (CANARY_WAP_INO, comments stripped): the two handlers
//     name only the bearer-only checks and never the cookie bridge; every
//     line that names g_device.api_token_str or g_device.ap_password is a
//     comparison, a hand-over to a module's own gate, key management, the
//     serial console or the boot banner, or sits inside
//     send_provisioning_receipt() or handle_pairing_qr() — the only two
//     writers of either secret into an HTTP answer — and the receipt writer
//     is called from handle_provisioning_receipt() alone.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run-receipt-grant

#include "arduino_globals.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Arduino.h"           // stubs/receipt_grant
#include "esp_http_server.h"   // stubs/receipt_grant
#include "log_level.h"         // the sketch's LogLevel / LogCategory
#include "receipt_authority.h"
#include "claim_ticket.h"
#include "provisioning_gate.h" // the sketch's staged copy: the peer key and its IPv4
#include "api_auth.h"          // the REAL one
#include "peer_source.h"       // stubs/receipt_grant: request_peer_key(), the socket read

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
static uint32_t g_provisioning_gate_opened_at = 0;

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
static uint32_t g_improv_claim_spent_at_ms = 0;

static void secure_zero(void* p, size_t n) {
  volatile uint8_t* vp = (volatile uint8_t*)p;
  while (n--) *vp++ = 0;
}

static std::vector<std::string> g_health_lines;
// Milliseconds each health line moves the fake clock (0 but in the one test
// that needs time to pass inside the handler, between its reads).
static uint32_t g_health_line_ms = 0;
void log_health(LogLevel, LogCategory, const char* message, const char* detail = nullptr) {
  (void)detail;
  g_health_lines.push_back(message ? message : "");
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
  g_provisioning_gate_opened_at = 0;
  canary::net::claim_ticket::wipe(g_improv_claim);
  g_improv_claim_spent_at_ms = 0;
  g_served.clear();
  g_qr_payloads.clear();
  g_health_lines.clear();
  g_health_line_ms = 0;
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
  Req() { r.handle = &g_http_server_obj; }
};

void open_tap() { g_provisioning_gate_opened_at = millis(); }
bool tap_open() {
  return g_provisioning_gate_opened_at != 0 && (millis() - g_provisioning_gate_opened_at) < PROVISIONING_GATE_TTL_MS;
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

void check_gate_refusal(const httpd_req_t& r) {
  CHECK(r.status == "403 Forbidden");
  CHECK(r.resp.find("\"error\":\"physical_confirmation_required\"") != std::string::npos);
  CHECK(r.resp.find("\"gate_ttl_seconds\":30") != std::string::npos);
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
  CHECK(g_served[0] == receipt_authority::Grant::BOOT_TAP);
  CHECK(has_header(q.r, "Set-Cookie"));
  CHECK(!tap_open());
  // A tap older than its TTL is no tap.
  g_provisioning_gate_opened_at = millis();
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
// the HTTPS server (sweep F289's residual). Before, it never was.
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
  CHECK(g_served.back() == receipt_authority::Grant::BOOT_TAP);
  const std::string tls = header(tap.r, "Set-Cookie");
  CHECK_MSG(tls.size() > 8 && tls.find(attrs + "; Secure") != std::string::npos &&
                tls.compare(tls.size() - 8, 8, "; Secure") == 0,
            "the HTTPS tap's cookie was '" + tls + "'");
  // The setup-door claim over it: the same cookie.
  reset_device();
  g_https_server = &g_https_server_obj;
  mint_claim();
  Req claim;
  claim.on(g_https_server).query(std::string("claim=") + g_improv_claim.hex);
  CHECK(receipt(claim) == 1);
  CHECK(g_served.back() == receipt_authority::Grant::SETUP_DOOR_CLAIM);
  CHECK(header(claim.r, "Set-Cookie").find(attrs + "; Secure") != std::string::npos);
  // HTTPS up, but this answer on the plain server: no Secure.
  reset_device();
  g_https_server = &g_https_server_obj;
  open_tap();
  Req port80;
  CHECK(receipt(port80) == 1);
  CHECK(header(port80.r, "Set-Cookie").find("Secure") == std::string::npos);
  // A refusal over HTTPS sets no cookie at all.
  reset_device();
  g_https_server = &g_https_server_obj;
  Req refused;
  refused.on(g_https_server);
  CHECK(receipt(refused) == 0);
  check_gate_refusal(refused.r);
}

void test_receipt_wrong_claim_burns_it() {
  reset_device();
  mint_session();
  mint_claim();
  Req q;
  q.cookie().query(std::string("claim=") + kClaimHex);
  CHECK(receipt(q) == 0);
  check_gate_refusal(q.r);
  CHECK_MSG(!claim_pending(), "a wrong claim left the real one pending");
  bool logged = false;
  for (const auto& l : g_health_lines) if (l.find("claim did not match") != std::string::npos) logged = true;
  CHECK(logged);
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
  CHECK(g_served.back() == receipt_authority::Grant::BEARER);
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
  CHECK(g_served.back() == receipt_authority::Grant::BEARER);
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
  CHECK(g_served.back() == receipt_authority::Grant::BEARER);
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
  CHECK(g_served.back() == receipt_authority::Grant::BEARER);
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
  CHECK(g_served.back() == receipt_authority::Grant::BOOT_TAP);
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
  CHECK(g_served.back() == receipt_authority::Grant::BOOT_TAP);
  CHECK(strikes(kHostA) == 0);
  check_right_bearer_served_at_once("a held right bearer the tap served");
  // The dashboard's own wait: no header at all (requestFromDevice), the tap
  // open, the address held by its earlier mistyped token. Served, cleared.
  reset_device();
  arm_lockout(4);
  open_tap();
  Req bare;
  CHECK(receipt(bare) == 1);
  CHECK(g_served.back() == receipt_authority::Grant::BOOT_TAP);
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
  CHECK(g_served.back() == receipt_authority::Grant::SETUP_DOOR_CLAIM);
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
  CHECK(g_served.back() == receipt_authority::Grant::BOOT_TAP);
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
  CHECK(g_served.back() == receipt_authority::Grant::BOOT_TAP);
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
  CHECK(g_served.back() == receipt_authority::Grant::SETUP_DOOR_CLAIM);
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
  CHECK(g_served.back() == receipt_authority::Grant::BOOT_TAP);
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
  // A wrong claim (spent, no match) and no tap: refused, nothing cleared.
  mint_claim();
  Req wrong_claim;
  wrong_claim.query(std::string("claim=") + kClaimHex);
  CHECK(receipt(wrong_claim) == 0);
  check_gate_refusal(wrong_claim.r);
  CHECK(!claim_pending());
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

// One request is one grant: a wrong header beside a wrong ?token= is one
// strike (the 2 s first backoff); a wrong header beside the right ?token= is
// served, and clears the count; neither, or a query without the key, is the
// 401 and no strike.
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
  CHECK(api_auth_check_or_query(&mixed.r, kToken));
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

void test_sketch_text() {
  const std::string raw = read_file(CANARY_WAP_INO);
  CHECK_MSG(!raw.empty(), std::string("could not read ") + CANARY_WAP_INO);
  if (raw.empty()) return;
  const std::vector<std::string> lines = split_lines(strip_comments(raw));

  // The receipt's handler names only the bearer-only check and never the
  // cookie bridge; the pairing QR's names only the bearer-only gate.
  const std::string receipt_fn = function_text(lines, "static esp_err_t handle_provisioning_receipt(httpd_req_t* req) {");
  CHECK(!receipt_fn.empty());
  CHECK(has(receipt_fn, "api_auth_bearer_grant(req, g_device.api_token_str)"));
  CHECK(has(receipt_fn, "api_auth_bearer_held(req)"));
  CHECK_MSG(!has(receipt_fn, "api_auth_check_optional("), "the receipt takes the cookie again");
  CHECK_MSG(!has(receipt_fn, "api_auth_check("), "the receipt takes the cookie again");
  CHECK(!has(receipt_fn, "api_auth_check_or_query("));
  CHECK(!has(receipt_fn, "cv_session_validate"));
  CHECK(!has(receipt_fn, "session_validate_cookie"));
  CHECK(has(receipt_fn, "provisioning_gate_take()"));
  CHECK(has(receipt_fn, "claim_ticket::take("));

  const std::string qr_fn = function_text(lines, "static esp_err_t handle_pairing_qr(httpd_req_t* req) {");
  CHECK(!qr_fn.empty());
  CHECK(has(qr_fn, "api_auth_check_bearer(req, g_device.api_token_str)"));
  CHECK_MSG(!has(qr_fn, "api_auth_check(") && !has(qr_fn, "api_auth_check_optional(") && !has(qr_fn, "api_auth_check_or_query("),
            "the pairing QR takes the cookie again");
  CHECK(!has(qr_fn, "cv_session_validate"));
  CHECK(!has(qr_fn, "session_validate_cookie"));

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

int main() {
  test_receipt_cookie_alone_is_refused();
  test_receipt_cookie_beside_wrong_bearer_is_refused();
  test_receipt_bearer_serves_and_spends_nothing();
  test_receipt_cookie_beside_tap_is_the_tap();
  test_receipt_cookie_beside_claim_is_the_claim();
  test_receipt_bare_tap_unchanged();
  test_receipt_cookie_secure_over_https_only();
  test_receipt_wrong_claim_burns_it();
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
  test_csi_routes_account_every_bearer();
  test_qr_scan_fallback_is_accounted();
  test_api_auth_contract();
  test_empty_device_token_takes_no_bearer();
  test_sketch_text();

  if (g_failures) {
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d receipt-grant checks PASSED\n", g_checks);
  return 0;
}
