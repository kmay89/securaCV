// Who may read the WAP's credentials (sweep F289): the provisioning receipt
// (the bearer token and the SoftAP password, the recovery kit) and the
// pairing QR (the token again), driven through the REAL code that decides.
//
// Compiled here, not copied: api_auth.h (every check, the lockout),
// cv_session_store.h (the session store and each session's origin),
// claim_ticket.h (the setup-door claim), and, cut verbatim out of the
// firmware by firmware/tests_host/cut_functions.awk, csi_integration.cpp's
// session glue (the Cookie read, the lookup, the owner check, the mint and
// the two trampolines api_auth.h's weak hooks resolve to) and canary_wap.ino's
// handle_ui() (the landing page's /?cv_pair= exchange, which mints a
// session), handle_provisioning_receipt() (the grant order), handle_pairing_qr()
// and secure_zero(). Faked: the clock, the RNG, the pair-token slots behind
// the landing, the BOOT tap's gate, the receipt and QR writers (each records
// what it was asked to answer and puts the token and the AP password in the
// body, as the real ones do), and the rest of the sketch's world.
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

static bool g_tls_enabled = false;
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
static struct {
  bool isConnected() const { return true; }
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

// The landing's pair tokens (csi_integration.cpp's slots, reduced to what
// handle_ui asks): issued to any caller, spent once by /?cv_pair=.
static std::string g_pair_issued;
static bool g_pair_spent = true;
static int  g_pair_serial = 0;
bool pair_token_issue(char* hex_out, size_t out_cap) {
  char buf[PAIR_TOKEN_HEX_LEN + 1];
  std::snprintf(buf, sizeof(buf), "%064x", ++g_pair_serial);
  if (out_cap < sizeof(buf)) return false;
  std::memcpy(hex_out, buf, sizeof(buf));
  g_pair_issued = buf;
  g_pair_spent = false;
  return true;
}
bool pair_token_consume(const char* hex) {
  if (g_pair_spent || g_pair_issued != hex) return false;
  g_pair_spent = true;
  return true;
}
bool send_pair_landing(httpd_req_t* req) {
  char hex[PAIR_TOKEN_HEX_LEN + 1];
  if (!pair_token_issue(hex, sizeof(hex))) return false;
  std::string body = std::string("<a href=\"/?cv_pair=") + hex + "\">Open dashboard</a>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, body.c_str(), (ssize_t)body.size()) == ESP_OK;
}

}  // namespace csi_integration

// ── the firmware, cut verbatim ────────────────────────────────────────────

// csi_integration.cpp: the hex decoder, the store and its lookup (its
// anonymous namespace), the checks (namespace csi_integration), the weak
// hooks' definitions (file scope), the mint and the encoder.
namespace {
#include "test_receipt_session_store.inc"
}  // namespace
namespace csi_integration {
#include "test_receipt_session_hex.inc"
#include "test_receipt_session_check.inc"
}  // namespace csi_integration
#include "test_receipt_session_hooks.inc"
namespace csi_integration {
#include "test_receipt_session_issue.inc"
}  // namespace csi_integration

// canary_wap.ino: the landing's exchange, the receipt's grants, the QR.
#include "test_receipt_session_sketch.inc"

// ── helpers ───────────────────────────────────────────────────────────────

namespace {

struct Reply {
  std::string status;
  std::string body;
  std::string set_cookie;   // the cv_session value a response set, or ""
};

Reply call(esp_err_t (*handler)(httpd_req_t*), const std::string& query,
           const std::string& cookie, const std::string& authorization) {
  httpd_req_t req;
  req.query = query;
  if (!cookie.empty()) req.req_hdrs["Cookie"] = "theme=dark; cv_session=" + cookie;
  if (!authorization.empty()) req.req_hdrs["Authorization"] = authorization;
  handler(&req);
  Reply r;
  r.status = req.status;
  r.body = req.resp;
  auto it = req.resp_hdrs.find("Set-Cookie");
  if (it != req.resp_hdrs.end()) {
    const std::string& v = it->second;
    const size_t at = v.find("cv_session=");
    if (at != std::string::npos) r.set_cookie = v.substr(at + 11, cv_session::TOK_HEX_LEN);
  }
  return r;
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
  std::memset(&g_auth, 0, sizeof(g_auth));
  canary::net::claim_ticket::wipe(g_improv_claim);
  g_tap_open = false;
  g_tap_takes = 0;
  g_receipts = 0;
  g_qrs = 0;
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

  // That session is the owner's: it serves the receipt and the QR alone.
  g_tap_open = false;
  const Reply again = call(handle_provisioning_receipt, "", receipt.set_cookie, "");
  CHECK(again.status == "200 OK" && carries_secret(again));
  CHECK(g_last_grant == receipt_authority::Grant::BEARER);
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
  const std::string claim2 = mint_claim();
  const Reply good = call(handle_provisioning_receipt, "claim=" + claim2, landing, "");
  CHECK(good.status == "200 OK" && carries_secret(good));
  CHECK(g_last_grant == receipt_authority::Grant::SETUP_DOOR_CLAIM);
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
  reset_world();
  std::string first;
  for (size_t i = 0; i < cv_session::SLOTS; ++i) {
    stub_receipt::now_ms += 10;
    const std::string s = landing_session();
    if (i == 0) first = s;
  }
  stub_receipt::now_ms += 10;
  g_tap_open = true;
  const std::string owner2 = call(handle_provisioning_receipt, "", "", "").set_cookie;
  size_t owners = 0;
  for (const auto& s : g_sessions.slots) owners += (s.active && s.origin == cv_session::Origin::OWNER);
  CHECK(owners == 1);
  CHECK(call(handle_provisioning_receipt, "", owner2, "").status == "200 OK");
  CHECK(call(handle_provisioning_receipt, "", first, "").status == "403 Forbidden");   // evicted
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
  if (g_failures) {
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d RECEIPT SESSION CHECKS PASSED\n", g_checks);
  return 0;
}
