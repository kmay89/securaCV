// Who may read the WAP's credentials (sweep F289): the provisioning receipt
// (the bearer token and the SoftAP password, the recovery kit) and the
// pairing QR (the token again), driven through the REAL code that decides.
//
// Compiled here, not copied: api_auth.h (every check, the lockout),
// cv_session_store.h (the session store and each session's origin),
// claim_ticket.h (the setup-door claim), and, cut verbatim out of the
// firmware by firmware/tests_host/cut_functions.awk, csi_integration.cpp's
// session glue (the 64-bit clock, the Cookie read, the lookup, the owner
// check, the mint and the two trampolines api_auth.h's weak hooks resolve to),
// its pair-token slots (the landing's one-shot tokens) and canary_wap.ino's
// handle_ui() (the landing page's /?cv_pair= exchange, which mints a
// session), handle_provisioning_receipt() (the grant order), handle_pairing_qr()
// and secure_zero(). Faked: the clock (one 64-bit count that millis() reads
// the low 32 bits of, as on the device, and esp_timer the whole of), the
// RNG, the station link (up, or down for the SoftAP-only setup phase), the
// landing page's HTML, the BOOT tap's gate, the receipt and QR writers (each records
// what it was asked to answer and puts the token and the AP password in the
// body, as the real ones do), the setup-door claim's spend stamp the loop
// task reads (a plain global here; #1804's g_improv_claim_spent_at_ms), and
// the rest of the sketch's world.
//
// Before F289 a session the landing page minted (for any caller that reached
// the device, on the home network too) was taken as the receipt's first grant
// and by the pairing QR: both answered the token, and the receipt the AP
// password, with no tap. Each check below that a landing session is refused
// fails on that code.

#include <cstdio>
#include <cstring>
#include <string>

#include "Arduino.h"          // stubs/receipt
#include "esp_http_server.h"  // stubs/receipt
#include "esp_timer.h"        // stubs/receipt

#include "receipt_authority.h"
#include "claim_ticket.h"
#include "cv_session_store.h"
#include "api_auth.h"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(cond)) {                                                        \
      ++g_failures;                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                     \
  } while (0)

}  // namespace

// ── the sketch's world, faked ─────────────────────────────────────────────

static const char kToken[]  = "cv_TESTtoken0123456789abcdefABCDEF";   // 35 chars, as the device's
static const char kApPass[] = "cv-apPASS1234";

static struct {
  uint32_t http_requests;
} g_health = {};

static struct {
  char api_token_str[36];
  char ap_password[16];
  char device_id[32];
  char mdns_hostname[40];
} g_device = {};

static bool g_tls_enabled = false;              // a certificate loaded
static httpd_handle_t g_https_server = nullptr;  // the HTTPS server started
static char g_tls_cert_fp_hex[65] = "";

// esp_fill_random: distinct bytes on every call, so every session differs.
static uint32_t g_rng = 0x2545F491u;
static void esp_fill_random(void* buf, size_t len) {
  uint8_t* p = (uint8_t*)buf;
  for (size_t i = 0; i < len; ++i) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    p[i] = (uint8_t)(g_rng >> 24);
  }
}

enum { SCV_LOG_INFO = 1, SCV_LOG_WARNING = 2 };
enum { SCV_CAT_AUTH = 7 };
static int g_health_logs = 0;
static void log_health(int, int, const char*, const char*) { ++g_health_logs; }

// The BOOT tap's gate: open after a tap, one consumer, closed after.
static bool g_tap_open = false;
static int  g_tap_takes = 0;
static bool provisioning_gate_take() {
  ++g_tap_takes;
  const bool was = g_tap_open;
  g_tap_open = false;
  return was;
}
static const uint32_t PROVISIONING_GATE_TTL_MS = 30000;

static canary::net::claim_ticket::Ticket g_improv_claim = {};
// When a served claim's receipt left (#1804): the loop task brings the reboot
// into steady state to a short runway from it. 0 is "no spend"; the receipt
// stamps it after a served claim and on no other grant.
static uint32_t g_improv_claim_spent_at_ms = 0;

// The receipt and the QR, as the sketch writes them (identity_json.h's
// receipt carries "token" and "ap_password"; the QR's payload "token").
static int g_receipts = 0;
static receipt_authority::Grant g_last_grant = receipt_authority::Grant::BEARER;
static esp_err_t send_provisioning_receipt(httpd_req_t* req, receipt_authority::Grant grant) {
  ++g_receipts;
  g_last_grant = grant;
  std::string body = std::string("{\"token\":\"") + g_device.api_token_str +
                     "\",\"ap_password\":\"" + g_device.ap_password + "\"}";
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, body.c_str());
}
static int g_qrs = 0;
static esp_err_t send_qr_svg(httpd_req_t* req, const char* payload) {
  ++g_qrs;
  httpd_resp_set_type(req, "image/svg+xml");
  return httpd_resp_sendstr(req, payload);
}

struct StubIP {
  std::string toString() const { return "192.168.4.1"; }
};
// The station link: up (joined to the home network) unless a case takes it
// down. Down is the setup phase, the SoftAP alone, where the receipt is most
// exposed: test_sta_down_setup_phase drives every grant there.
static bool g_sta_connected = true;
static struct {
  bool isConnected() const { return g_sta_connected; }
  StubIP softAPIP() const { return StubIP{}; }
} WiFi;

// handle_ui()'s world: no first-boot wizard, the dashboard page, the landing.
namespace setup_wizard {
static bool is_active() { return false; }
}  // namespace setup_wizard
static const char CAPTIVE_PORTAL_HTML[] = "captive";
static const uint8_t CSI_DASHBOARD_HTML_GZ[] = {0x1f, 0x8b};
static const size_t CSI_DASHBOARD_HTML_GZ_LEN = sizeof(CSI_DASHBOARD_HTML_GZ);
static int g_dashboards = 0;
static esp_err_t send_gzip_html(httpd_req_t* req, const uint8_t*, size_t) {
  ++g_dashboards;
  return httpd_resp_sendstr(req, "DASHBOARD");
}

namespace csi_integration {

constexpr size_t PAIR_TOKEN_HEX_LEN     = 64;
constexpr size_t SESSION_COOKIE_HEX_LEN = cv_session::TOK_HEX_LEN;

// The landing's pair tokens are csi_integration.cpp's own slots, cut below:
// issued to any caller, spent once by /?cv_pair=.
bool pair_token_issue(char* hex_out, size_t out_cap);
bool pair_token_consume(const char* hex);
void hex_encode(const uint8_t* in, size_t len, char* out);
// The landing page itself (csi_integration.cpp's template, reduced to its link).
bool send_pair_landing(httpd_req_t* req) {
  char hex[PAIR_TOKEN_HEX_LEN + 1];
  if (!pair_token_issue(hex, sizeof(hex))) return false;
  std::string body = std::string("<a href=\"/?cv_pair=") + hex + "\">Open dashboard</a>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, body.c_str(), (ssize_t)body.size()) == ESP_OK;
}

}  // namespace csi_integration

// ── the firmware, cut verbatim ────────────────────────────────────────────

// csi_integration.cpp: the clock, the hex decoder, the store and its lookup,
// the pair-token table (its anonymous namespace), the checks (namespace
// csi_integration), the weak hooks' definitions (file scope), the mint, the
// encoder and the pair-token API.
namespace {
#include "test_receipt_session_store.inc"
#include "test_receipt_session_pair.inc"
}  // namespace
namespace csi_integration {
#include "test_receipt_session_hex.inc"
#include "test_receipt_session_check.inc"
}  // namespace csi_integration
#include "test_receipt_session_hooks.inc"
namespace csi_integration {
#include "test_receipt_session_issue.inc"
#include "test_receipt_session_pairapi.inc"
}  // namespace csi_integration

// canary_wap.ino: the landing's exchange, the receipt's grants, the QR.
#include "test_receipt_session_sketch.inc"

// ── helpers ───────────────────────────────────────────────────────────────

namespace {

struct Reply {
  std::string status;
  std::string body;
  std::string set_cookie;       // the cv_session value a response set, or ""
  std::string set_cookie_hdr;   // the whole Set-Cookie header, or ""
};

// A request with its Cookie header exactly as given ("" for none).
Reply call_raw(esp_err_t (*handler)(httpd_req_t*), const std::string& query,
               const std::string& cookie_header, const std::string& authorization) {
  httpd_req_t req;
  req.query = query;
  if (!cookie_header.empty()) req.req_hdrs["Cookie"] = cookie_header;
  if (!authorization.empty()) req.req_hdrs["Authorization"] = authorization;
  handler(&req);
  Reply r;
  r.status = req.status;
  r.body = req.resp;
  auto it = req.resp_hdrs.find("Set-Cookie");
  if (it != req.resp_hdrs.end()) {
    const std::string& v = it->second;
    r.set_cookie_hdr = v;
    const size_t at = v.find("cv_session=");
    if (at != std::string::npos) r.set_cookie = v.substr(at + 11, cv_session::TOK_HEX_LEN);
  }
  return r;
}

// A browser's request: the session cookie after another cookie of its own.
Reply call(esp_err_t (*handler)(httpd_req_t*), const std::string& query,
           const std::string& cookie, const std::string& authorization) {
  return call_raw(handler, query, cookie.empty() ? "" : "theme=dark; cv_session=" + cookie,
                  authorization);
}

// What the session checks answer for a Cookie header, alone.
bool any_session(const std::string& cookie_header) {
  httpd_req_t req;
  req.req_hdrs["Cookie"] = cookie_header;
  return csi_integration::session_validate_cookie(&req);
}
bool owner_session(const std::string& cookie_header) {
  httpd_req_t req;
  req.req_hdrs["Cookie"] = cookie_header;
  return csi_integration::session_validate_owner_cookie(&req);
}

bool carries_secret(const Reply& r) {
  return r.body.find(kToken) != std::string::npos ||
         r.body.find(kApPass) != std::string::npos;
}

// The landing page's session, the way any caller on the network gets one:
// GET / with no session draws the landing, its link's pair token goes to
// GET /?cv_pair=<token>, and handle_ui sets the cookie.
std::string landing_session() {
  const Reply landing = call(handle_ui, "", "", "");
  const size_t at = landing.body.find("cv_pair=");
  if (at == std::string::npos) return "";
  const std::string pair = landing.body.substr(at + 8, csi_integration::PAIR_TOKEN_HEX_LEN);
  const Reply exchanged = call(handle_ui, "cv_pair=" + pair, "", "");
  return exchanged.status == "302 Found" ? exchanged.set_cookie : "";
}

std::string bearer() { return std::string("Bearer ") + kToken; }

void reset_world() {
  std::memset(&g_device, 0, sizeof(g_device));
  std::snprintf(g_device.api_token_str, sizeof(g_device.api_token_str), "%s", kToken);
  std::snprintf(g_device.ap_password, sizeof(g_device.ap_password), "%s", kApPass);
  std::snprintf(g_device.device_id, sizeof(g_device.device_id), "%s", "canary-ab12");
  std::snprintf(g_device.mdns_hostname, sizeof(g_device.mdns_hostname), "%s", "canary-ab12");
  std::memset(&g_sessions, 0, sizeof(g_sessions));
  std::memset(&g_pair_slots, 0, sizeof(g_pair_slots));
  std::memset(&g_auth, 0, sizeof(g_auth));
  canary::net::claim_ticket::wipe(g_improv_claim);
  g_improv_claim_spent_at_ms = 0;
  g_tap_open = false;
  g_tap_takes = 0;
  g_receipts = 0;
  g_qrs = 0;
  g_sta_connected = true;
  stub_receipt::now_ms += 3600000;   // past any lockout a previous case left
}

std::string mint_claim() {
  uint8_t raw[canary::net::claim_ticket::RAW_LEN];
  esp_fill_random(raw, sizeof(raw));
  canary::net::claim_ticket::mint(g_improv_claim, raw, millis());
  return g_improv_claim.hex;
}

// ── the cases ─────────────────────────────────────────────────────────────

// The hole: a landing session takes no credential. With no tap and no claim
// the receipt is the BOOT-tap refusal and holds neither secret; the QR is a
// plain 401 and holds no token. The tap is asked (and stays closed), as for
// a request with no session at all.
void test_landing_session_refused() {
  reset_world();
  const std::string landing = landing_session();
  CHECK(landing.size() == cv_session::TOK_HEX_LEN);

  const Reply receipt = call(handle_provisioning_receipt, "", landing, "");
  CHECK(receipt.status == "403 Forbidden");
  CHECK(receipt.body.find("physical_confirmation_required") != std::string::npos);
  CHECK(!carries_secret(receipt));
  CHECK(g_receipts == 0);
  CHECK(g_tap_takes == 1);
  CHECK(receipt.set_cookie.empty());
  CHECK(g_improv_claim_spent_at_ms == 0);   // no claim was spent

  const Reply qr = call(handle_pairing_qr, "", landing, "");
  CHECK(qr.status == "401 Unauthorized");
  CHECK(!carries_secret(qr));
  CHECK(g_qrs == 0);
  // No credential was presented: no lockout strike (auth_logic.h).
  CHECK(g_auth.consecutive_failures == 0);
}

// The posture the landing exists for is unchanged: its session runs the
// dashboard and every route that answers no credential.
void test_landing_session_keeps_the_dashboard() {
  reset_world();
  const std::string landing = landing_session();
  CHECK(call(handle_ui, "", landing, "").body == "DASHBOARD");
  httpd_req_t req;
  req.req_hdrs["Cookie"] = "cv_session=" + landing;
  CHECK(api_auth_check(&req, g_device.api_token_str));
  CHECK(api_auth_check_optional(&req, g_device.api_token_str));
  CHECK(api_auth_check_or_query(&req, g_device.api_token_str));
  CHECK(csi_integration::session_validate_cookie(&req));
  CHECK(!csi_integration::session_validate_owner_cookie(&req));
  CHECK(!api_auth_check_credential_optional(&req, g_device.api_token_str));
}

// A bearer in the Authorization header serves both, and leaves the tap and
// the claim alone.
void test_bearer_header_serves() {
  reset_world();
  g_tap_open = true;
  const std::string claim = mint_claim();
  const Reply receipt = call(handle_provisioning_receipt, "", "", bearer());
  CHECK(receipt.status == "200 OK");
  CHECK(carries_secret(receipt));
  CHECK(g_receipts == 1 && g_last_grant == receipt_authority::Grant::BEARER);
  CHECK(g_tap_open && g_tap_takes == 0);
  CHECK(canary::net::claim_ticket::pending(g_improv_claim, millis()));
  CHECK(receipt.set_cookie.empty());
  CHECK(g_improv_claim_spent_at_ms == 0);   // the claim is left alone, unspent

  const Reply qr = call(handle_pairing_qr, "", "", bearer());
  CHECK(qr.status == "200 OK");
  CHECK(qr.body.find(kToken) != std::string::npos);

  // Beside a landing session, the header is what serves.
  const std::string landing = landing_session();
  const Reply both = call(handle_provisioning_receipt, "", landing, bearer());
  CHECK(both.status == "200 OK" && carries_secret(both));
  CHECK(g_last_grant == receipt_authority::Grant::BEARER);
  (void)claim;
}

// A wrong bearer beside a landing session: the receipt falls to the tap
// (closed here), and the QR is a 403 that counts toward the lockout.
void test_wrong_bearer_beside_landing() {
  reset_world();
  const std::string landing = landing_session();
  const std::string wrong = std::string("Bearer ") + std::string(sizeof(kToken) - 1, 'x');
  const Reply receipt = call(handle_provisioning_receipt, "", landing, wrong);
  CHECK(receipt.status == "403 Forbidden" && !carries_secret(receipt));
  const Reply qr = call(handle_pairing_qr, "", landing, wrong);
  CHECK(qr.status == "403 Forbidden" && !carries_secret(qr));
  CHECK(g_auth.consecutive_failures == 1);
}

// A setup-door claim serves (grant 2), leaves the tap unspent, and mints an
// owner's session.
void test_claim_serves() {
  reset_world();
  g_tap_open = true;
  const std::string claim = mint_claim();
  const Reply receipt = call(handle_provisioning_receipt, "claim=" + claim, "", "");
  CHECK(receipt.status == "200 OK" && carries_secret(receipt));
  CHECK(g_last_grant == receipt_authority::Grant::SETUP_DOOR_CLAIM);
  CHECK(g_tap_open && g_tap_takes == 0);
  CHECK(receipt.set_cookie.size() == cv_session::TOK_HEX_LEN);
  // The spend is stamped for the loop task's reboot runway (#1804), never 0.
  CHECK(g_improv_claim_spent_at_ms == canary::net::claim_ticket::stamp(millis()));

  // That session is the owner's: it serves the receipt and the QR alone, as
  // grant 1, which spends no claim and stamps nothing.
  g_tap_open = false;
  g_improv_claim_spent_at_ms = 0;
  const Reply again = call(handle_provisioning_receipt, "", receipt.set_cookie, "");
  CHECK(again.status == "200 OK" && carries_secret(again));
  CHECK(g_last_grant == receipt_authority::Grant::BEARER);
  CHECK(g_improv_claim_spent_at_ms == 0);
  CHECK(call(handle_pairing_qr, "", receipt.set_cookie, "").status == "200 OK");
}

// A claim presented beside a landing session is still the claim (spent
// whatever it answers); a wrong one falls to the tap.
void test_claim_beside_landing() {
  reset_world();
  const std::string landing = landing_session();
  const std::string claim = mint_claim();
  std::string wrong = claim;
  wrong[0] = wrong[0] == 'a' ? 'b' : 'a';
  const Reply bad = call(handle_provisioning_receipt, "claim=" + wrong, landing, "");
  CHECK(bad.status == "403 Forbidden" && !carries_secret(bad));
  CHECK(!canary::net::claim_ticket::pending(g_improv_claim, millis()));   // burned
  CHECK(g_improv_claim_spent_at_ms == 0);   // burned, but no receipt left: no runway
  const std::string claim2 = mint_claim();
  const Reply good = call(handle_provisioning_receipt, "claim=" + claim2, landing, "");
  CHECK(good.status == "200 OK" && carries_secret(good));
  CHECK(g_last_grant == receipt_authority::Grant::SETUP_DOOR_CLAIM);
  CHECK(g_improv_claim_spent_at_ms != 0);
}

// The BOOT tap serves (grant 3), is spent, and mints an owner's session,
// which then serves the receipt and the QR with no tap: the dashboard's
// Settings save after a BOOT-tap sign-in (F257).
void test_tap_serves_and_mints_owner() {
  reset_world();
  g_tap_open = true;
  const Reply receipt = call(handle_provisioning_receipt, "", "", "");
  CHECK(receipt.status == "200 OK" && carries_secret(receipt));
  CHECK(g_last_grant == receipt_authority::Grant::BOOT_TAP);
  CHECK(!g_tap_open && g_tap_takes == 1);
  CHECK(receipt.set_cookie.size() == cv_session::TOK_HEX_LEN);
  CHECK(g_improv_claim_spent_at_ms == 0);   // a tap is not a claim's spend

  const std::string owner = receipt.set_cookie;
  const Reply again = call(handle_provisioning_receipt, "", owner, "");
  CHECK(again.status == "200 OK" && carries_secret(again));
  CHECK(g_last_grant == receipt_authority::Grant::BEARER);
  CHECK(g_tap_takes == 1);   // the owner's session took no second tap
  const Reply qr = call(handle_pairing_qr, "", owner, "");
  CHECK(qr.status == "200 OK" && qr.body.find(kToken) != std::string::npos);

  httpd_req_t req;
  req.req_hdrs["Cookie"] = "cv_session=" + owner;
  CHECK(csi_integration::session_validate_owner_cookie(&req));
  CHECK(api_auth_check(&req, g_device.api_token_str));   // and the dashboard
}

// The owner's path back to the kit from a landing session: the receipt
// refuses until the tap, the tap serves it to that same browser and mints a
// second session, the owner's; the landing session stays a landing one.
void test_landing_then_tap() {
  reset_world();
  const std::string landing = landing_session();
  CHECK(call(handle_provisioning_receipt, "", landing, "").status == "403 Forbidden");
  g_tap_open = true;
  const Reply tapped = call(handle_provisioning_receipt, "", landing, "");
  CHECK(tapped.status == "200 OK" && carries_secret(tapped));
  CHECK(g_last_grant == receipt_authority::Grant::BOOT_TAP);
  CHECK(!tapped.set_cookie.empty() && tapped.set_cookie != landing);
  CHECK(call(handle_provisioning_receipt, "", landing, "").status == "403 Forbidden");
  CHECK(call(handle_provisioning_receipt, "", tapped.set_cookie, "").status == "200 OK");
}

// The origin is the slot's, written at every mint: a landing session that
// takes the slot an owner's session expired out of is a landing session, and
// an owner's session past its 24 h is no session.
void test_origin_is_the_mints() {
  reset_world();
  g_tap_open = true;
  const std::string owner = call(handle_provisioning_receipt, "", "", "").set_cookie;
  CHECK(call(handle_provisioning_receipt, "", owner, "").status == "200 OK");
  stub_receipt::now_ms += cv_session::TTL_MS;
  CHECK(call(handle_provisioning_receipt, "", owner, "").status == "403 Forbidden");
  // Every slot now open or expired: the first mint takes slot 0, the
  // owner's old one.
  const std::string landing = landing_session();
  CHECK(!landing.empty());
  CHECK(g_sessions.slots[0].active &&
        g_sessions.slots[0].origin == cv_session::Origin::LANDING);
  CHECK(call(handle_provisioning_receipt, "", landing, "").status == "403 Forbidden");
  CHECK(call(handle_pairing_qr, "", landing, "").status == "401 Unauthorized");

  // And the other way: a full store, every slot a landing session, then a
  // tap: the oldest goes, the new one is the owner's, the rest stay landing.
  // Eviction is read from the session checks, not the receipt, which
  // refuses a landing session whether or not it was evicted.
  reset_world();
  std::string landings[cv_session::SLOTS];
  for (size_t i = 0; i < cv_session::SLOTS; ++i) {
    stub_receipt::now_ms += 10;
    landings[i] = landing_session();
    CHECK(any_session("cv_session=" + landings[i]));
  }
  stub_receipt::now_ms += 10;
  g_tap_open = true;
  const std::string owner2 = call(handle_provisioning_receipt, "", "", "").set_cookie;
  size_t owners = 0;
  for (const auto& s : g_sessions.slots) owners += (s.active && s.origin == cv_session::Origin::OWNER);
  CHECK(owners == 1);
  CHECK(call(handle_provisioning_receipt, "", owner2, "").status == "200 OK");
  CHECK(owner_session("cv_session=" + owner2));
  CHECK(!any_session("cv_session=" + landings[0]));             // the oldest: evicted
  for (size_t i = 1; i < cv_session::SLOTS; ++i) {
    CHECK(any_session("cv_session=" + landings[i]));             // the rest: still live
    CHECK(!owner_session("cv_session=" + landings[i]));          // and still landing
  }
}

// Nothing the client sends picks the origin: the cookie is the token alone,
// a malformed or unknown one is no session, and an owner's token presented
// another way (a query, a header) is not a session either.
void test_client_cannot_choose_origin() {
  reset_world();
  g_tap_open = true;
  const std::string owner = call(handle_provisioning_receipt, "", "", "").set_cookie;
  std::string unknown = owner;
  unknown[0] = unknown[0] == '0' ? '1' : '0';
  CHECK(call(handle_provisioning_receipt, "", unknown, "").status == "403 Forbidden");
  CHECK(call(handle_provisioning_receipt, "", owner.substr(0, 63), "").status == "403 Forbidden");
  CHECK(call(handle_provisioning_receipt, "", owner + "0", "").status == "403 Forbidden");
  CHECK(call(handle_provisioning_receipt, "cv_session=" + owner, "", "").status == "403 Forbidden");
  CHECK(call(handle_provisioning_receipt, "", "", "Bearer " + owner).status == "403 Forbidden");
  // Upper-case hex is the same token (the cookie scan and the decoder take it).
  std::string upper = owner;
  for (char& c : upper) c = (char)toupper((unsigned char)c);
  CHECK(call(handle_provisioning_receipt, "", upper, "").status == "200 OK");
}

// api_auth.h's two new checks, alone.
void test_credential_checks() {
  reset_world();
  const std::string landing = landing_session();
  g_tap_open = true;
  const std::string owner = call(handle_provisioning_receipt, "", "", "").set_cookie;
  {
    httpd_req_t req;   // nothing
    CHECK(!api_auth_check_credential_optional(&req, g_device.api_token_str));
    CHECK(!api_auth_check_credential(&req, g_device.api_token_str));
    CHECK(req.status == "401 Unauthorized");
  }
  {
    httpd_req_t req;
    req.req_hdrs["Cookie"] = "cv_session=" + owner;
    CHECK(api_auth_check_credential_optional(&req, g_device.api_token_str));
    CHECK(api_auth_check_credential(&req, g_device.api_token_str));
  }
  {
    httpd_req_t req;
    req.req_hdrs["Authorization"] = bearer();
    CHECK(api_auth_check_credential_optional(&req, g_device.api_token_str));
    CHECK(api_auth_check_credential(&req, g_device.api_token_str));
    CHECK(api_auth_bearer_header_ok(&req, g_device.api_token_str));
  }
  {
    httpd_req_t req;
    req.req_hdrs["Cookie"] = "cv_session=" + landing;
    CHECK(!api_auth_bearer_header_ok(&req, g_device.api_token_str));
    CHECK(!api_auth_check_credential(&req, g_device.api_token_str));
    CHECK(req.status == "401 Unauthorized");
  }
}

// A Cookie header nothing minted (sweep F289 review). The store is zero at
// boot, so a cookie of 64 zeros matches an unused slot's token byte for
// byte: only the slot's `active` flag stands between it and a session, and
// only the zero value of Origin (NONE) between it and an owner's. Checked
// inside the first 24 h of uptime, where a zeroed slot's issued_ms of 0 is
// not yet past the TTL.
void test_forged_zero_cookie() {
  reset_world();
  const uint64_t saved_now = stub_receipt::now_ms;
  stub_receipt::now_ms = 60000;   // a minute after boot
  const std::string zeros(cv_session::TOK_HEX_LEN, '0');
  const std::string hdr = "cv_session=" + zeros;
  CHECK(cv_session::Slot{}.origin == cv_session::Origin::NONE);
  CHECK(static_cast<uint8_t>(cv_session::Origin::NONE) == 0);
  CHECK(!any_session(hdr));
  CHECK(!owner_session(hdr));
  const Reply receipt = call_raw(handle_provisioning_receipt, "", hdr, "");
  CHECK(receipt.status == "403 Forbidden" && !carries_secret(receipt));
  const Reply qr = call_raw(handle_pairing_qr, "", hdr, "");
  CHECK(qr.status == "401 Unauthorized" && !carries_secret(qr));
  {
    httpd_req_t req;
    req.req_hdrs["Cookie"] = hdr;
    CHECK(!api_auth_check(&req, g_device.api_token_str));
    CHECK(req.status == "401 Unauthorized");
  }
  CHECK(call_raw(handle_ui, "", hdr, "").body != "DASHBOARD");   // the landing, not the dashboard
  // Beside one real session in slot 0, the other seven still zero.
  const std::string landing = landing_session();
  CHECK(any_session("cv_session=" + landing));
  CHECK(!any_session(hdr) && !owner_session(hdr));
  CHECK(call_raw(handle_provisioning_receipt, "", hdr, "").status == "403 Forbidden");
  stub_receipt::now_ms = saved_now + 60000;
}

// Every byte of the token is compared: an owner's cookie with any one of its
// 64 hex digits changed is no session at all, and a landing cookie likewise.
// (Changing digit i changes byte i / 2, so this reaches the first byte, the
// last, and every one between.)
void test_every_cookie_digit_counts() {
  reset_world();
  const std::string landing = landing_session();
  g_tap_open = true;
  const std::string owner = call(handle_provisioning_receipt, "", "", "").set_cookie;
  CHECK(owner.size() == cv_session::TOK_HEX_LEN);
  size_t taken = 0, receipts_served = 0, landing_taken = 0;
  for (size_t i = 0; i < cv_session::TOK_HEX_LEN; ++i) {
    std::string o = owner;
    o[i] = o[i] == '0' ? '1' : '0';
    if (any_session("cv_session=" + o) || owner_session("cv_session=" + o)) {
      std::printf("FAIL: an owner cookie with digit %zu changed is a session\n", i);
      ++taken;
    }
    const Reply r = call(handle_provisioning_receipt, "", o, "");
    if (r.status != "403 Forbidden" || carries_secret(r)) ++receipts_served;
    std::string l = landing;
    l[i] = l[i] == '0' ? '1' : '0';
    if (any_session("cv_session=" + l)) {
      std::printf("FAIL: a landing cookie with digit %zu changed is a session\n", i);
      ++landing_taken;
    }
  }
  CHECK(taken == 0);
  CHECK(receipts_served == 0);
  CHECK(landing_taken == 0);
  // And the cookies themselves still are what they were.
  CHECK(owner_session("cv_session=" + owner) && any_session("cv_session=" + landing));
}

// The cv_session pair need not be the last cookie in the header: a browser
// sends its cookies in the order it likes. Ended by ';', ' ' or the end.
void test_cookie_anywhere_in_the_header() {
  reset_world();
  g_tap_open = true;
  const std::string owner = call(handle_provisioning_receipt, "", "", "").set_cookie;
  const std::string shapes[] = {
      "cv_session=" + owner,
      "cv_session=" + owner + "; theme=dark",
      "cv_session=" + owner + ";theme=dark",
      "lang=en; cv_session=" + owner + "; theme=dark",
  };
  for (const std::string& h : shapes) {
    const Reply r = call_raw(handle_provisioning_receipt, "", h, "");
    if (r.status != "200 OK") std::printf("FAIL: Cookie \"%s\" was refused\n", h.c_str());
    CHECK(r.status == "200 OK" && carries_secret(r));
    CHECK(g_last_grant == receipt_authority::Grant::BEARER);
    CHECK(owner_session(h));
  }
  // A longer value is not the token followed by something.
  CHECK(!owner_session("cv_session=" + owner + "0; theme=dark"));
  CHECK(!owner_session("cv_session=" + owner + "x"));
}

// The cookie each mint sets: HttpOnly, SameSite=Strict, Path=/, 24 h, and
// Secure exactly when the HTTPS server started. The owner's stands in for
// the bearer for those 24 h; the landing's runs the dashboard. Three units:
// no certificate (TLS off); a certificate and no HTTPS server, which is a
// build without esp_https_server.h or a start that fell back to HTTP, where
// every route is served on port 80 and a browser would drop a Secure cookie
// (sweep F289's second review: Secure used to follow the certificate); and
// a certificate with the HTTPS server up.
void test_cookie_attributes() {
  static int s_https_handle = 0;
  const struct { bool cert; bool https; } units[] = {{false, false}, {true, false}, {true, true}};
  for (const auto& unit : units) {
    reset_world();
    g_tls_enabled = unit.cert;
    g_https_server = unit.https ? (httpd_handle_t)&s_https_handle : nullptr;
    const bool tls = unit.https;
    const std::string tail = std::string("; HttpOnly; SameSite=Strict; Path=/; Max-Age=86400") +
                             (tls ? "; Secure" : "");
    // The landing's mint (handle_ui's /?cv_pair= exchange).
    const Reply landing = call(handle_ui, "", "", "");
    const size_t at = landing.body.find("cv_pair=");
    CHECK(at != std::string::npos);
    const Reply exchanged =
        call(handle_ui, "cv_pair=" + landing.body.substr(at + 8, csi_integration::PAIR_TOKEN_HEX_LEN), "", "");
    CHECK(exchanged.status == "302 Found");
    CHECK(exchanged.set_cookie_hdr == "cv_session=" + exchanged.set_cookie + tail);
    // The tap's mint.
    g_tap_open = true;
    const Reply tapped = call(handle_provisioning_receipt, "", "", "");
    CHECK(tapped.status == "200 OK");
    CHECK(tapped.set_cookie_hdr == "cv_session=" + tapped.set_cookie + tail);
    // The claim's mint.
    const std::string claim = mint_claim();
    const Reply claimed = call(handle_provisioning_receipt, "claim=" + claim, "", "");
    CHECK(claimed.status == "200 OK");
    CHECK(claimed.set_cookie_hdr == "cv_session=" + claimed.set_cookie + tail);
    if (exchanged.set_cookie_hdr != "cv_session=" + exchanged.set_cookie + tail ||
        tapped.set_cookie_hdr != "cv_session=" + tapped.set_cookie + tail) {
      std::printf("FAIL: cert=%d https=%d landing \"%s\" tap \"%s\"\n", unit.cert, unit.https,
                  exchanged.set_cookie_hdr.c_str(), tapped.set_cookie_hdr.c_str());
    }
  }
  g_tls_enabled = false;
  g_https_server = nullptr;
}

// A session's 24 h hold across millis()'s wrap (sweep F289's second review).
// millis() is 32 bits and wraps every 2^32 ms (49.7 days of uptime); the
// store used to take a slot's age from it, so a slot nothing cleared read
// young again a whole wrap later, and an owner's cookie past its 24 h opened
// the receipt and the QR once more, for a day, with no tap. The session clock
// is 64-bit now (session_now_ms()). Arranged as the review's probe was: a
// landing session in slot 0, the tap's owner session in slot 1.
void test_session_ends_across_the_millis_wrap() {
  reset_world();
  const std::string landing = landing_session();
  g_tap_open = true;
  const Reply tapped = call(handle_provisioning_receipt, "", "", "");
  CHECK(tapped.status == "200 OK");
  const std::string owner = tapped.set_cookie;
  const uint64_t minted = stub_receipt::now_ms;
  CHECK(owner_session("cv_session=" + owner));
  CHECK(g_sessions.slots[1].active && g_sessions.slots[1].origin == cv_session::Origin::OWNER);

  stub_receipt::now_ms = minted + cv_session::TTL_MS;          // a day on: past its 24 h
  CHECK(!owner_session("cv_session=" + owner));
  CHECK(call(handle_provisioning_receipt, "", owner, "").status == "403 Forbidden");

  // A wrap and a minute after the mint. The 32-bit age reads one minute.
  stub_receipt::now_ms = minted + (1ULL << 32) + 60000;
  CHECK((uint32_t)(millis() - (uint32_t)minted) == 60000u);
  CHECK(!any_session("cv_session=" + owner));
  CHECK(!owner_session("cv_session=" + owner));
  CHECK(!any_session("cv_session=" + landing));
  const Reply receipt = call(handle_provisioning_receipt, "", owner, "");
  CHECK(receipt.status == "403 Forbidden" && !carries_secret(receipt));
  const Reply qr = call(handle_pairing_qr, "", owner, "");
  CHECK(qr.status == "401 Unauthorized" && !carries_secret(qr));
  CHECK(g_receipts == 1 && g_qrs == 0);

  // And a fresh mint after the wrap holds its 24 h and then ends.
  g_tap_open = true;
  const std::string after = call(handle_provisioning_receipt, "", "", "").set_cookie;
  CHECK(owner_session("cv_session=" + after));
  stub_receipt::now_ms += cv_session::TTL_MS - 1;
  CHECK(owner_session("cv_session=" + after));
  stub_receipt::now_ms += 1;
  CHECK(!owner_session("cv_session=" + after));
}

// The landing's pair token across the same wrap: issued, never spent, past
// its ten minutes, then a wrap and a minute after its issue. The exchange
// mints nothing at either time (it used to mint a landing session after the
// wrap, from a token the page showed 49.7 days before).
void test_pair_token_ends_across_the_millis_wrap() {
  reset_world();
  const Reply page = call(handle_ui, "", "", "");
  const size_t at = page.body.find("cv_pair=");
  CHECK(at != std::string::npos);
  const std::string pair = page.body.substr(at + 8, csi_integration::PAIR_TOKEN_HEX_LEN);
  const uint64_t issued = stub_receipt::now_ms;
  CHECK(csi_integration::pair_token_valid(pair.c_str()));

  stub_receipt::now_ms = issued + 10ULL * 60ULL * 1000ULL;      // its ten minutes are up
  CHECK(!csi_integration::pair_token_valid(pair.c_str()));
  stub_receipt::now_ms = issued + (1ULL << 32) + 60000;          // a wrap and a minute on
  CHECK(!csi_integration::pair_token_valid(pair.c_str()));
  const Reply exchanged = call(handle_ui, "cv_pair=" + pair, "", "");
  CHECK(exchanged.status != "302 Found" && exchanged.set_cookie.empty());
  size_t live = 0;
  for (const auto& s : g_sessions.slots) live += s.active;
  CHECK(live == 0);
}

// The cookie's name is the whole name (sweep F289's second review). The scan
// used to take the first "cv_session=" anywhere in the header, so another
// cookie whose name ends in it ("xcv_session=") or whose value holds it
// ("foo=cv_session=") was read as the session. Not an escalation (the value
// must still name a live slot), but the header's grammar says a name starts
// the header or follows ';', and that is what is read now. Between two
// cv_session pairs the first one wins, as before.
void test_cookie_name_is_the_whole_name() {
  reset_world();
  const std::string landing = landing_session();
  g_tap_open = true;
  const std::string owner = call(handle_provisioning_receipt, "", "", "").set_cookie;
  CHECK(owner.size() == cv_session::TOK_HEX_LEN && landing.size() == cv_session::TOK_HEX_LEN);
  const std::string not_the_name[] = {
      "xcv_session=" + owner,
      "theme=dark; xcv_session=" + owner,
      "foo=cv_session=" + owner,
      "theme=dark; foo=cv_session=" + owner + "; lang=en",
      "a_cv_session=" + owner + ";b=1",
  };
  for (const std::string& h : not_the_name) {
    if (any_session(h)) std::printf("FAIL: Cookie \"%s\" was read as a session\n", h.c_str());
    CHECK(!any_session(h));
    CHECK(!owner_session(h));
    const Reply r = call_raw(handle_provisioning_receipt, "", h, "");
    CHECK(r.status == "403 Forbidden" && !carries_secret(r));
  }
  // A look-alike before the real one does not hide it.
  CHECK(owner_session("xcv_session=" + landing + "; cv_session=" + owner));
  CHECK(owner_session("foo=cv_session=" + landing + ";cv_session=" + owner));
  CHECK(owner_session("  cv_session=" + owner));
  // The first cv_session pair wins: a landing pair first is a landing session.
  CHECK(any_session("cv_session=" + landing + "; cv_session=" + owner));
  CHECK(!owner_session("cv_session=" + landing + "; cv_session=" + owner));
}

// A unit whose token was never derived holds an empty api_token_str:
// provision_device() returns before it derives one when the device key cannot
// be made or stored, and setup() still serves HTTP so the owner can see why.
// An empty token is no token (sweep F289's second review; csi_mqtt's owner
// gate already said so): "Bearer " with nothing after it, or ?token= with
// nothing, matches no credential. Before, each check compared zero bytes and
// said yes, and the receipt and the QR answered.
void test_empty_device_token_is_no_token() {
  const std::string empties[] = {"Bearer ", "Bearer   "};
  for (const std::string& a : empties) {
    reset_world();
    g_device.api_token_str[0] = '\0';
    const Reply receipt = call(handle_provisioning_receipt, "", "", a);
    if (receipt.status != "403 Forbidden") {
      std::printf("FAIL: \"%s\" opened the receipt on an empty token\n", a.c_str());
    }
    CHECK(receipt.status == "403 Forbidden");
    CHECK(receipt.body.find("physical_confirmation_required") != std::string::npos);
    CHECK(g_receipts == 0);
    const Reply qr = call(handle_pairing_qr, "", "", a);
    CHECK(qr.status == "403 Forbidden");
    CHECK(g_qrs == 0);
    std::memset(&g_auth, 0, sizeof(g_auth));
    httpd_req_t req;
    req.req_hdrs["Authorization"] = a;
    CHECK(!api_auth_bearer_header_ok(&req, g_device.api_token_str));
    CHECK(!api_auth_check_optional(&req, g_device.api_token_str));
    CHECK(!api_auth_check_credential_optional(&req, g_device.api_token_str));
    CHECK(!api_auth_check_credential(&req, g_device.api_token_str));
    std::memset(&g_auth, 0, sizeof(g_auth));
    httpd_req_t req2;
    req2.req_hdrs["Authorization"] = a;
    CHECK(!api_auth_check(&req2, g_device.api_token_str));
    std::memset(&g_auth, 0, sizeof(g_auth));
    httpd_req_t req3;
    req3.req_hdrs["Authorization"] = a;
    CHECK(!api_auth_check_or_query(&req3, g_device.api_token_str));
  }
  {
    reset_world();
    g_device.api_token_str[0] = '\0';
    httpd_req_t req;
    req.query = "token=";
    CHECK(!api_auth_check_or_query(&req, g_device.api_token_str));
    CHECK(req.status == "401 Unauthorized");
  }
  // What a token-less unit still does: a landing session runs the dashboard,
  // and the tap serves the receipt (the owner's way back in).
  {
    reset_world();
    g_device.api_token_str[0] = '\0';
    const std::string landing = landing_session();
    CHECK(call(handle_ui, "", landing, "").body == "DASHBOARD");
    g_tap_open = true;
    const Reply tapped = call(handle_provisioning_receipt, "", "", "");
    CHECK(tapped.status == "200 OK");
    CHECK(g_receipts == 1 && g_last_grant == receipt_authority::Grant::BOOT_TAP);
  }
}

// The setup phase: the station is down and the SoftAP is the only link, the
// hour the receipt is most exposed (F289's landing-page caller sits on that
// SoftAP). The link is no grant: with no bearer, no claim and no tap, a
// request with no session and one with the landing's session each get the
// BOOT-tap refusal and neither secret, and the QR is a 401. Every other case
// runs with the station up, so an answer the handler gave on a down link
// alone would pass them all; this one fails it. Each grant still serves on
// the SoftAP alone, so the refusal is the grants' doing, not the link's.
void test_sta_down_setup_phase() {
  reset_world();
  g_sta_connected = false;
  const Reply anon = call(handle_provisioning_receipt, "", "", "");
  CHECK(anon.status == "403 Forbidden");
  CHECK(anon.body.find("physical_confirmation_required") != std::string::npos);
  CHECK(!carries_secret(anon));
  CHECK(g_receipts == 0);
  CHECK(g_tap_takes == 1);
  CHECK(anon.set_cookie.empty());

  const std::string landing = landing_session();
  CHECK(landing.size() == cv_session::TOK_HEX_LEN);
  const Reply landed = call(handle_provisioning_receipt, "", landing, "");
  CHECK(landed.status == "403 Forbidden");
  CHECK(!carries_secret(landed));
  CHECK(g_receipts == 0);

  const Reply qr = call(handle_pairing_qr, "", "", "");
  CHECK(qr.status == "401 Unauthorized" && !carries_secret(qr));
  const Reply qr_landed = call(handle_pairing_qr, "", landing, "");
  CHECK(qr_landed.status == "401 Unauthorized" && !carries_secret(qr_landed));
  CHECK(g_qrs == 0);

  // The grants, each on the SoftAP alone.
  reset_world();
  g_sta_connected = false;
  const Reply by_bearer = call(handle_provisioning_receipt, "", "", bearer());
  CHECK(by_bearer.status == "200 OK" && carries_secret(by_bearer));
  CHECK(g_last_grant == receipt_authority::Grant::BEARER);
  const Reply qr_bearer = call(handle_pairing_qr, "", "", bearer());
  CHECK(qr_bearer.status == "200 OK" && qr_bearer.body.find(kToken) != std::string::npos);
  CHECK(qr_bearer.body.find("192.168.4.1") != std::string::npos);   // the SoftAP's address, not .local

  reset_world();
  g_sta_connected = false;
  const std::string claim = mint_claim();
  const Reply by_claim = call(handle_provisioning_receipt, "claim=" + claim, "", "");
  CHECK(by_claim.status == "200 OK" && carries_secret(by_claim));
  CHECK(g_last_grant == receipt_authority::Grant::SETUP_DOOR_CLAIM);

  reset_world();
  g_sta_connected = false;
  g_tap_open = true;
  const Reply by_tap = call(handle_provisioning_receipt, "", "", "");
  CHECK(by_tap.status == "200 OK" && carries_secret(by_tap));
  CHECK(g_last_grant == receipt_authority::Grant::BOOT_TAP);
  CHECK(g_receipts == 1 && g_tap_takes == 1);
}

}  // namespace

int main() {
  // api_auth.h's statics this suite does not call otherwise.
  (void)&auth_stats_json;
  (void)&auth_redact_token;
  test_landing_session_refused();
  test_landing_session_keeps_the_dashboard();
  test_bearer_header_serves();
  test_wrong_bearer_beside_landing();
  test_claim_serves();
  test_claim_beside_landing();
  test_tap_serves_and_mints_owner();
  test_landing_then_tap();
  test_origin_is_the_mints();
  test_client_cannot_choose_origin();
  test_credential_checks();
  test_forged_zero_cookie();
  test_every_cookie_digit_counts();
  test_cookie_anywhere_in_the_header();
  test_cookie_attributes();
  test_session_ends_across_the_millis_wrap();
  test_pair_token_ends_across_the_millis_wrap();
  test_cookie_name_is_the_whole_name();
  test_empty_device_token_is_no_token();
  test_sta_down_setup_phase();
  if (g_failures) {
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d RECEIPT SESSION CHECKS PASSED\n", g_checks);
  return 0;
}
