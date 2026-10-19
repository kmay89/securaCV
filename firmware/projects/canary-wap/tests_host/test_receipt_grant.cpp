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
// grant ONLY an `Authorization: Bearer <token>` header (api_auth_bearer_valid,
// api_auth_check_bearer in api_auth.h), constant-time compared as every admin
// route's is; the receipt's other grants, the claim and the tap, are
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
//     which serve a stale bearer, inside the backoff or out of it;
//   - the pairing QR on a cookie alone: 401 with the WWW-Authenticate
//     challenge, no SVG, no token in the answer, no lockout strike (before:
//     the SVG with the token); on a cookie beside a wrong bearer: 403 and a
//     strike (before: the SVG); on a bearer: the SVG with the token, the pin
//     and the https .local base on a TLS unit on the home Wi-Fi, the AP
//     address while in setup; five wrong guesses lock the right token out
//     (429 with Retry-After) until the backoff passes;
//   - api_auth.h's contract: api_auth_check() and api_auth_check_optional()
//     still take the cookie (the dashboard's routes), api_auth_bearer_valid()
//     and api_auth_check_bearer() never do, and the cookie path never clears
//     the lockout;
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
#include "api_auth.h"          // the REAL one

#ifndef RECEIPT_GRANT_INC
#error "RECEIPT_GRANT_INC must name the cut of canary_wap.ino (the Makefile passes it)"
#endif
#ifndef CANARY_WAP_INO
#error "CANARY_WAP_INO must name canary_wap.ino (the Makefile passes it)"
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
void log_health(LogLevel, LogCategory, const char* message, const char* detail = nullptr) {
  (void)detail;
  g_health_lines.push_back(message ? message : "");
}

namespace csi_integration {
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
#include RECEIPT_GRANT_INC

namespace {

const char* const kToken = "cv_7kX9mN2pQ4rS6tU8vW0xY1zA3bC5dE7f";   // 35 chars, "cv_" + 32
const char* const kWrong = "cv_7kX9mN2pQ4rS6tU8vW0xY1zA3bC5dE7g";   // one char off
const char* const kApPass = "cv-9f8e7d6c5b4a";
const char* const kClaimHex = "ffeeddccbbaa99887766554433221100";   // not the one mint_claim() mints

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
  g_live_session = "";
  g_cookie_checks = 0;
  g_auth = AuthState{0, 0, 0, 0, 0};
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
    CHECK_MSG(g_auth.consecutive_failures == 1, "not a strike: " + raw);
  }
  // Whitespace around the token is trimmed, as api_auth_check trims it.
  reset_device();
  Req padded;
  padded.authorization(std::string("Bearer   ") + kToken + "  ");
  CHECK(receipt(padded) == 1);
  CHECK(g_served.back() == receipt_authority::Grant::BEARER);
}

// A backoff running now: `failures` strikes, the last one just sent.
void arm_lockout(uint8_t failures) {
  g_auth.consecutive_failures = failures;
  g_auth.last_failure_ms = millis();
  g_auth.lockout_until_ms = millis() + 8000;
}

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
  CHECK_MSG(g_auth.consecutive_failures == 1, "a wrong bearer on the receipt was not a strike");
  CHECK(g_auth.total_failures == 1);
  // A request with no Authorization header carries no guess: no strike,
  // the cookie beside it or not (auth_logic.h).
  reset_device();
  mint_session();
  Req bare;
  CHECK(receipt(bare) == 0);
  Req cookie_only;
  cookie_only.cookie();
  CHECK(receipt(cookie_only) == 0);
  CHECK(g_auth.consecutive_failures == 0);
  CHECK(g_auth.total_failures == 0);
}

void test_receipt_right_bearer_clears_the_count() {
  reset_device();
  g_auth.consecutive_failures = 2;
  g_auth.last_failure_ms = millis();
  g_auth.lockout_until_ms = millis();   // the backoff just passed
  Req right;
  right.bearer(kToken);
  CHECK(receipt(right) == 1);
  CHECK(g_served.back() == receipt_authority::Grant::BEARER);
  CHECK_MSG(g_auth.consecutive_failures == 0, "a right bearer on the receipt left the count standing");
  CHECK(g_auth.total_successes == 1);
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
  CHECK(g_auth.consecutive_failures == 3);
  CHECK(g_auth.total_successes == 0);
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
  CHECK(g_auth.consecutive_failures == 3);
  // The backoff passes: the right token serves again, and clears the count.
  stub_grant::now_ms += 8001;
  Req later;
  later.bearer(kToken);
  CHECK(receipt(later) == 1);
  CHECK(g_served.back() == receipt_authority::Grant::BEARER);
  CHECK(g_auth.consecutive_failures == 0);
}

void test_receipt_arming_guess_then_backoff() {
  reset_device();
  Req first;
  first.bearer(kWrong);
  CHECK(receipt(first) == 0);
  check_gate_refusal(first.r);   // 403: this guess armed the backoff
  CHECK(g_auth.consecutive_failures == 1);
  Req second;
  second.bearer(kWrong);
  CHECK(receipt(second) == 0);
  check_lockout_refusal(second.r);   // inside it: 429, not compared, no strike
  CHECK(g_auth.consecutive_failures == 1);
  stub_grant::now_ms += AUTH_LOCKOUT_BASE_MS + 1;
  Req third;
  third.bearer(kWrong);
  CHECK(receipt(third) == 0);
  check_gate_refusal(third.r);   // after it: compared, a strike, the 403
  CHECK(g_auth.consecutive_failures == 2);
}

void test_receipt_lockout_never_holds_the_tap_or_the_claim() {
  // A page holding a stale token, the backoff running (a guesser on the
  // network, or the page's own earlier tries): the owner's tap serves it.
  reset_device();
  arm_lockout(5);
  open_tap();
  Req stale;
  stale.bearer(kWrong);
  CHECK_MSG(receipt(stale) == 1, "the lockout held the owner's BOOT tap");
  CHECK(g_served.back() == receipt_authority::Grant::BOOT_TAP);
  CHECK(!tap_open());
  CHECK(has_header(stale.r, "Set-Cookie"));
  CHECK(g_auth.consecutive_failures == 5);   // the tap clears no count
  // The right token inside the backoff, a tap open: served, as the tap (no
  // bearer is compared while it runs).
  reset_device();
  arm_lockout(5);
  open_tap();
  Req right;
  right.bearer(kToken);
  CHECK(receipt(right) == 1);
  CHECK(g_served.back() == receipt_authority::Grant::BOOT_TAP);
  // The setup-door claim, a stale bearer beside it, the backoff running.
  reset_device();
  arm_lockout(5);
  mint_claim();
  Req claim;
  claim.bearer(kWrong).query(std::string("claim=") + g_improv_claim.hex);
  CHECK_MSG(receipt(claim) == 1, "the lockout held the setup door's claim");
  CHECK(g_served.back() == receipt_authority::Grant::SETUP_DOOR_CLAIM);
  CHECK(!claim_pending());
  // Outside a backoff: the stale bearer is a strike, and the tap serves it.
  reset_device();
  open_tap();
  Req fresh;
  fresh.bearer(kWrong);
  CHECK_MSG(receipt(fresh) == 1, "a stale bearer kept the tap from serving");
  CHECK(g_served.back() == receipt_authority::Grant::BOOT_TAP);
  CHECK(g_auth.consecutive_failures == 1);
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
  CHECK(g_auth.consecutive_failures == 0);   // no credential presented: no strike
}

void test_qr_cookie_beside_wrong_bearer_is_refused() {
  reset_device();
  mint_session();
  Req q;
  q.cookie().bearer(kWrong);
  CHECK_MSG(qr(q) == 0, "a cookie rescued a wrong bearer on the pairing QR");
  CHECK(q.r.status == "403 Forbidden");
  CHECK(!carries_secret(q.r));
  CHECK(g_auth.consecutive_failures == 1);   // a guess: a strike
}

void test_qr_bearer_serves() {
  reset_device();
  mint_session();
  g_auth.consecutive_failures = 2;
  g_auth.last_failure_ms = millis();
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
  CHECK(g_auth.consecutive_failures == 0);   // a right bearer clears the count
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
  CHECK(g_auth.consecutive_failures == 0);
}

void test_qr_lockout_is_every_admin_routes() {
  reset_device();
  mint_session();
  // Five wrong guesses, each outside the previous lockout.
  for (int i = 0; i < 5; ++i) {
    stub_grant::now_ms += 400000;   // past AUTH_LOCKOUT_MAX_MS, inside nothing
    g_auth.last_failure_ms = millis() - 1;   // keep the failure window from resetting the count
    Req q;
    q.cookie().bearer(kWrong);
    CHECK(qr(q) == 0);
    CHECK(q.r.status == "403 Forbidden");
  }
  CHECK(g_auth.consecutive_failures == 5);
  // Locked out: the right token, the cookie beside it, is 429.
  Req right;
  right.cookie().bearer(kToken);
  CHECK(qr(right) == 0);
  CHECK(right.r.status == "429 Too Many Requests");
  CHECK(!header(right.r, "Retry-After").empty());
  CHECK(!carries_secret(right.r));
  // The backoff passes: served.
  stub_grant::now_ms += AUTH_LOCKOUT_MAX_MS + 1000;
  Req later;
  later.bearer(kToken);
  CHECK(qr(later) == 1);
}

// One lockout for both routes: guesses on the receipt hold the QR's right
// token, and guesses on the QR hold the receipt's.
void test_receipt_and_qr_share_the_lockout() {
  reset_device();
  for (int i = 0; i < 5; ++i) {
    stub_grant::now_ms += 400000;   // past AUTH_LOCKOUT_MAX_MS, inside nothing
    g_auth.last_failure_ms = millis() - 1;
    Req q;
    q.bearer(kWrong);
    CHECK(receipt(q) == 0);
    check_gate_refusal(q.r);
  }
  CHECK(g_auth.consecutive_failures == 5);
  Req on_qr;
  on_qr.bearer(kToken);
  CHECK_MSG(qr(on_qr) == 0, "five guesses on the receipt left the QR open");
  CHECK(on_qr.r.status == "429 Too Many Requests");

  reset_device();
  for (int i = 0; i < 5; ++i) {
    stub_grant::now_ms += 400000;
    g_auth.last_failure_ms = millis() - 1;
    Req q;
    q.bearer(kWrong);
    CHECK(qr(q) == 0);
  }
  Req on_receipt;
  on_receipt.bearer(kToken);
  CHECK_MSG(receipt(on_receipt) == 0, "five guesses on the QR left the receipt's bearer open");
  check_lockout_refusal(on_receipt.r);
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
  g_auth.consecutive_failures = 4;
  g_auth.last_failure_ms = millis();
  Req again;
  again.cookie();
  CHECK(api_auth_check(&again.r, kToken));
  CHECK(g_auth.consecutive_failures == 4);
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
  test_qr_cookie_alone_is_refused();
  test_qr_cookie_beside_wrong_bearer_is_refused();
  test_qr_bearer_serves();
  test_qr_no_credential_is_401_without_strike();
  test_qr_lockout_is_every_admin_routes();
  test_receipt_and_qr_share_the_lockout();
  test_api_auth_contract();
  test_sketch_text();

  if (g_failures) {
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d receipt-grant checks PASSED\n", g_checks);
  return 0;
}
