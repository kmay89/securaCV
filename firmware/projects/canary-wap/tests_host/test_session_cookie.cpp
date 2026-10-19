// The dashboard's session cookie on canary-wap (sweep F289's residual): it is
// Secure exactly when the answer that sets it goes out over the HTTPS server,
// and the device reads it by its whole name.
//
// Before, both places that mint a session (handle_ui's /?cv_pair exchange and
// the provisioning receipt's BOOT-tap or setup-door grant) spelled the
// cookie without Secure, so on a unit serving its dashboard over HTTPS a
// browser would still send it to http:// (port 80, the redirect server) in
// the clear. And csi_integration.cpp found it with strstr(buf,
// "cv_session="), which also matches inside a longer name ("xcv_session=")
// or a value ("foo=cv_session="): such a cookie was read as the session, and
// one in front of the real cookie hid it.
//
// This runs handle_ui() and session_cookie_header() cut verbatim out of
// canary_wap.ino, and read_session_cookie_hex() cut verbatim out of
// csi_integration.cpp (firmware/tests_host/cut_functions.awk), over
// stubs/receipt_grant (a request with the server it arrived on, its headers
// and its query string) and stand-ins for what else handle_ui calls: the
// setup wizard's flag, the pair-token and session stores (one live token
// each, the session validated through the REAL reader), the landing page and
// the gzip page writer. What it pins:
//   - the ?cv_pair exchange over the HTTPS server: 302 to /, and the cookie
//     with every attribute it had plus "; Secure" (before: no Secure);
//   - the same exchange on a plain-HTTP boot (no HTTPS server, the
//     first-boot SoftAP setup's case), and on a request that reached the
//     port-80 server while HTTPS is up: the same cookie, no Secure;
//   - the first-boot setup flow unchanged: canary.local or 192.168.4.1 is
//     sent to /companion with a pair token and no cookie; a foreign Host
//     gets the captive page;
//   - the reader: the cookie named cv_session (the header's first, or after
//     ';' with spaces or tabs between), exactly 64 hex characters ended by
//     ';', ' ' or the end; never one whose name only ends in cv_session, nor
//     a value that holds it, and such a cookie in front no longer hides the
//     real one (before: read as the session; the real one behind it lost);
//   - handle_ui's dashboard branch through that reader: "xcv_session=<the
//     live session>" gets the landing page, not the dashboard;
//   - the sketch's text: the cookie's attributes are spelled once, in
//     session_cookie_header(), and both Set-Cookie sites send its output.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run-session-cookie

#include "arduino_globals.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "Arduino.h"           // stubs/receipt_grant
#include "esp_http_server.h"   // stubs/receipt_grant

#ifndef SESSION_COOKIE_UI_INC
#error "SESSION_COOKIE_UI_INC must name the cut of canary_wap.ino (the Makefile passes it)"
#endif
#ifndef SESSION_COOKIE_CSI_INC
#error "SESSION_COOKIE_CSI_INC must name the cut of csi_integration.cpp (the Makefile passes it)"
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

// ── the reader, cut verbatim out of csi_integration.cpp ──────────────────────
// It reads csi_integration.cpp's own constant (32 bytes, 64 hex).
constexpr size_t SESSION_TOK_HEX_LEN = 64;
#include SESSION_COOKIE_CSI_INC

// ── the sketch's globals and the calls handle_ui makes ───────────────────────
struct { uint32_t http_requests = 0; } g_health;

// Two servers, as start_http_server() makes them: with HTTPS up, every route
// is on g_https_server and port 80 (g_http_server) only redirects; on a
// plain-HTTP boot g_https_server stays nullptr. Any two distinct addresses
// stand in for the handles.
static int g_http_server_obj = 0;
static int g_https_server_obj = 0;
static httpd_handle_t g_http_server = &g_http_server_obj;
static httpd_handle_t g_https_server = nullptr;

namespace setup_wizard {
static bool g_active = false;
inline bool is_active() { return g_active; }
}  // namespace setup_wizard

static const char CAPTIVE_PORTAL_HTML[] = "<p>Open canary.local</p>";
static const uint8_t CSI_DASHBOARD_HTML_GZ[] = {0x1f, 0x8b, 0x08};
static const size_t CSI_DASHBOARD_HTML_GZ_LEN = sizeof(CSI_DASHBOARD_HTML_GZ);

static int g_dashboards = 0;
static esp_err_t send_gzip_html(httpd_req_t* req, const uint8_t* gz, size_t gz_len) {
  (void)gz;
  (void)gz_len;
  ++g_dashboards;
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  return httpd_resp_sendstr(req, "<dashboard>");
}

namespace csi_integration {
constexpr size_t SESSION_COOKIE_HEX_LEN = 64;
constexpr size_t PAIR_TOKEN_HEX_LEN = 64;

// One live session and one live pair token, as the real stores would hold.
static std::string g_live_session;
static std::string g_live_pair;
static int g_sessions_issued = 0;
static int g_landings = 0;

// The real reader, then the one live session: the dashboard branch's
// question, asked through read_session_cookie_hex().
bool session_validate_cookie(httpd_req_t* req) {
  char hex[SESSION_TOK_HEX_LEN + 1];
  if (!read_session_cookie_hex(req, hex)) return false;
  return !g_live_session.empty() && g_live_session == hex;
}
bool session_issue(char* hex_out, size_t out_cap) {
  if (!hex_out || out_cap < SESSION_COOKIE_HEX_LEN + 1) return false;
  ++g_sessions_issued;
  g_live_session.assign(SESSION_COOKIE_HEX_LEN, "0123456789abcdef"[g_sessions_issued % 16]);
  std::memcpy(hex_out, g_live_session.c_str(), SESSION_COOKIE_HEX_LEN + 1);
  return true;
}
bool pair_token_issue(char* hex_out, size_t out_cap) {
  if (!hex_out || out_cap < PAIR_TOKEN_HEX_LEN + 1) return false;
  g_live_pair.assign(PAIR_TOKEN_HEX_LEN, 'c');
  std::memcpy(hex_out, g_live_pair.c_str(), PAIR_TOKEN_HEX_LEN + 1);
  return true;
}
bool pair_token_consume(const char* hex) {
  if (!hex || g_live_pair.empty() || g_live_pair != hex) return false;
  g_live_pair.clear();   // one-shot
  return true;
}
bool send_pair_landing(httpd_req_t* req) {
  ++g_landings;
  httpd_resp_set_type(req, "text/html");
  httpd_resp_sendstr(req, "<landing>");
  return true;
}
}  // namespace csi_integration

// ── the handler and the cookie writer, cut verbatim out of canary_wap.ino ───
#include SESSION_COOKIE_UI_INC

namespace {

const std::string kAttrs = "; HttpOnly; SameSite=Strict; Path=/; Max-Age=86400";

void reset() {
  g_https_server = nullptr;
  setup_wizard::g_active = false;
  csi_integration::g_live_session.clear();
  csi_integration::g_live_pair.clear();
  csi_integration::g_sessions_issued = 0;
  csi_integration::g_landings = 0;
  g_dashboards = 0;
}

// The HTTPS server up, as a steady-state boot with a certificate starts it.
void https_up() { g_https_server = &g_https_server_obj; }

std::string header(const httpd_req_t& r, const char* name) {
  for (const auto& h : r.resp_hdrs) if (h.first == name) return h.second;
  return "";
}
int header_count(const httpd_req_t& r, const char* name) {
  int n = 0;
  for (const auto& h : r.resp_hdrs) if (h.first == name) ++n;
  return n;
}

// GET /?cv_pair=<the live pair token> on `server`; the request it answered.
httpd_req_t pair_exchange(httpd_handle_t server) {
  char pair[csi_integration::PAIR_TOKEN_HEX_LEN + 1];
  CHECK(csi_integration::pair_token_issue(pair, sizeof(pair)));
  httpd_req_t r;
  r.handle = server;
  r.req_hdrs["Host"] = "canary-7916.local";
  r.query = std::string("cv_pair=") + pair;
  CHECK(handle_ui(&r) == ESP_OK);
  CHECK(r.sends == 1);
  return r;
}

void check_exchange_cookie(const httpd_req_t& r, bool secure) {
  CHECK(r.status == "302 Found");
  CHECK(header(r, "Location") == "/");
  CHECK(header_count(r, "Set-Cookie") == 1);
  CHECK(csi_integration::g_sessions_issued == 1);
  const std::string want = "cv_session=" + csi_integration::g_live_session + kAttrs +
                           (secure ? "; Secure" : "");
  CHECK_MSG(header(r, "Set-Cookie") == want,
            "Set-Cookie was '" + header(r, "Set-Cookie") + "', want '" + want + "'");
}

// ── the Secure attribute ─────────────────────────────────────────────────────

void test_exchange_over_https_is_secure() {
  reset();
  https_up();
  const httpd_req_t r = pair_exchange(g_https_server);
  check_exchange_cookie(r, /*secure=*/true);
}

void test_exchange_on_plain_http_boot_is_not_secure() {
  // No HTTPS server (the first-boot SoftAP setup, a build without
  // esp_https_server.h, or a failed HTTPS start): every route is on port 80,
  // and a Secure cookie set by an http page is one the browser drops.
  reset();
  const httpd_req_t r = pair_exchange(g_http_server);
  check_exchange_cookie(r, /*secure=*/false);
}

void test_exchange_on_port_80_beside_https_is_not_secure() {
  // HTTPS up, but this answer leaves over the plain server (no route of
  // handle_ui's is registered there today; the cookie follows the answer's
  // own transport, not the unit's).
  reset();
  https_up();
  const httpd_req_t r = pair_exchange(g_http_server);
  check_exchange_cookie(r, /*secure=*/false);
}

void test_exchange_without_pair_token_mints_nothing() {
  reset();
  https_up();
  httpd_req_t r;
  r.handle = g_https_server;
  r.query = std::string("cv_pair=") + std::string(64, 'd');   // never issued
  CHECK(handle_ui(&r) == ESP_OK);
  CHECK(header_count(r, "Set-Cookie") == 0);
  CHECK(csi_integration::g_landings == 1);
  CHECK(csi_integration::g_sessions_issued == 0);
}

void test_first_boot_setup_flow_unchanged() {
  // The SoftAP's first boot runs HTTP-only, and / sends the phone's own
  // browser to the wizard with a pair token: no cookie, as before.
  for (const char* host : {"canary.local", "192.168.4.1"}) {
    reset();
    setup_wizard::g_active = true;
    httpd_req_t r;
    r.handle = g_http_server;
    r.req_hdrs["Host"] = host;
    CHECK(handle_ui(&r) == ESP_OK);
    CHECK(r.status == "302 Found");
    CHECK(header(r, "Location") == "/companion?token=" + std::string(64, 'c'));
    CHECK(header_count(r, "Set-Cookie") == 0);
  }
  // A captive-portal probe for a foreign Host gets the static page.
  reset();
  setup_wizard::g_active = true;
  httpd_req_t probe;
  probe.handle = g_http_server;
  probe.req_hdrs["Host"] = "captive.apple.com";
  CHECK(handle_ui(&probe) == ESP_OK);
  CHECK(probe.status == "200 OK");
  CHECK(probe.resp == CAPTIVE_PORTAL_HTML);
  CHECK(header_count(probe, "Set-Cookie") == 0);
}

// ── the whole-name read ──────────────────────────────────────────────────────

const std::string kLive = std::string(32, 'a') + std::string(32, 'B');   // 64 hex
const std::string kOther = std::string(64, 'e');

// The value read_session_cookie_hex() takes out of `cookie`, or "<none>".
std::string read_of(const std::string& cookie) {
  httpd_req_t r;
  r.req_hdrs["Cookie"] = cookie;
  char hex[SESSION_TOK_HEX_LEN + 1];
  std::memset(hex, 0, sizeof(hex));
  if (!read_session_cookie_hex(&r, hex)) return "<none>";
  return std::string(hex);
}

void test_reader_takes_the_cookie_named_cv_session() {
  CHECK(read_of("cv_session=" + kLive) == kLive);
  CHECK(read_of("theme=dark; cv_session=" + kLive) == kLive);
  CHECK(read_of("theme=dark;cv_session=" + kLive) == kLive);
  CHECK(read_of("theme=dark; \t cv_session=" + kLive + "; lang=en") == kLive);
  CHECK(read_of("  cv_session=" + kLive) == kLive);
  CHECK(read_of("cv_session=" + kLive + " ; lang=en") == kLive);
}

void test_reader_never_takes_a_longer_name_or_a_value() {
  // Before: each of these was read as the session.
  CHECK_MSG(read_of("xcv_session=" + kLive) == "<none>", "a cookie named xcv_session was read as the session");
  CHECK_MSG(read_of("theme=dark; xcv_session=" + kLive) == "<none>", "xcv_session after another cookie");
  CHECK_MSG(read_of("my_cv_session=" + kLive) == "<none>", "a longer name ending in cv_session");
  CHECK_MSG(read_of("foo=cv_session=" + kLive) == "<none>", "a value holding cv_session=");
  CHECK_MSG(read_of("foo=bar cv_session=" + kLive) == "<none>", "a value with a space before cv_session=");
}

void test_reader_finds_the_real_cookie_behind_a_lookalike() {
  // Before: the lookalike in front was read (the wrong value), and the real
  // session behind it lost.
  CHECK_MSG(read_of("xcv_session=" + kOther + "; cv_session=" + kLive) == kLive,
            "a lookalike in front hid the real session: read " +
                read_of("xcv_session=" + kOther + "; cv_session=" + kLive));
  CHECK(read_of("foo=cv_session=" + kOther + "; cv_session=" + kLive) == kLive);
  CHECK(read_of("xcv_session=" + kOther + ";cv_session=" + kLive) == kLive);
}

void test_reader_shape_rules_unchanged() {
  CHECK(read_of("cv_session=" + kLive.substr(0, 63)) == "<none>");          // short
  CHECK(read_of("cv_session=" + kLive + "f") == "<none>");                   // 65 hex
  CHECK(read_of("cv_session=" + kLive.substr(0, 63) + "g") == "<none>");    // not hex
  CHECK(read_of("cv_session=" + kLive + "=x") == "<none>");                  // bad terminator
  CHECK(read_of("cv_session=") == "<none>");
  CHECK(read_of("cv_session") == "<none>");
  CHECK(read_of("") == "<none>");
  // The FIRST cookie named cv_session is the one read, whole or malformed.
  CHECK(read_of("cv_session=" + kLive.substr(0, 10) + "; cv_session=" + kLive) == "<none>");
  CHECK(read_of("cv_session=" + kOther + "; cv_session=" + kLive) == kOther);
  // No Cookie header; one at the 512-byte ceiling.
  httpd_req_t none;
  char hex[SESSION_TOK_HEX_LEN + 1];
  CHECK(!read_session_cookie_hex(&none, hex));
  CHECK(read_of(std::string(500, 'z') + "; cv_session=" + kLive) == "<none>");
  CHECK(read_of(std::string(400, 'z') + "; cv_session=" + kLive) == kLive);
}

void test_dashboard_branch_reads_the_whole_name() {
  // A live session, presented under a longer name: the landing page, not the
  // dashboard (before: the dashboard).
  reset();
  csi_integration::g_live_session = kLive;
  httpd_req_t lookalike;
  lookalike.handle = g_http_server;
  lookalike.req_hdrs["Cookie"] = "xcv_session=" + kLive;
  CHECK(handle_ui(&lookalike) == ESP_OK);
  CHECK_MSG(g_dashboards == 0, "xcv_session=<live> was served the dashboard");
  CHECK(csi_integration::g_landings == 1);
  // The live session behind a lookalike: the dashboard (before: the landing).
  reset();
  csi_integration::g_live_session = kLive;
  httpd_req_t behind;
  behind.handle = g_http_server;
  behind.req_hdrs["Cookie"] = "xcv_session=" + kOther + "; cv_session=" + kLive;
  CHECK(handle_ui(&behind) == ESP_OK);
  CHECK_MSG(g_dashboards == 1, "the real session behind a lookalike was not served the dashboard");
  CHECK(csi_integration::g_landings == 0);
}

// ── the sketch's text ────────────────────────────────────────────────────────

std::string read_file(const char* path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
int count_of(const std::string& hay, const std::string& needle) {
  int n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
  return n;
}
// The body of the function whose signature line starts with `sig`, to its
// first line that is exactly "}".
std::string fn_body(const std::string& src, const std::string& sig) {
  const size_t at = src.find("\n" + sig);
  if (at == std::string::npos) return "";
  const size_t end = src.find("\n}\n", at);
  return end == std::string::npos ? "" : src.substr(at, end - at);
}

void test_sketch_text() {
  const std::string ino = read_file(CANARY_WAP_INO);
  CHECK(!ino.empty());
  // The attributes are spelled once, in the writer.
  CHECK(count_of(ino, "\"cv_session=%s;") == 1);
  const std::string writer = fn_body(ino, "static bool session_cookie_header(");
  CHECK(writer.find("\"cv_session=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=86400%s\"") != std::string::npos);
  CHECK(writer.find("req->handle == g_https_server") != std::string::npos);
  // Both Set-Cookie sites send the writer's output, and there are only two.
  CHECK(count_of(ino, "\"Set-Cookie\"") == 2);
  const std::string call = "session_cookie_header(req, session_hex, cookie_hdr, sizeof(cookie_hdr))";
  const std::string ui = fn_body(ino, "static esp_err_t handle_ui(httpd_req_t* req) {");
  const std::string receipt = fn_body(ino, "static esp_err_t handle_provisioning_receipt(httpd_req_t* req) {");
  CHECK(ui.find(call) != std::string::npos);
  CHECK(ui.find("httpd_resp_set_hdr(req, \"Set-Cookie\", cookie_hdr)") != std::string::npos);
  CHECK(receipt.find(call) != std::string::npos);
  CHECK(receipt.find("httpd_resp_set_hdr(req, \"Set-Cookie\", cookie_hdr)") != std::string::npos);
}

}  // namespace

int main() {
  test_exchange_over_https_is_secure();
  test_exchange_on_plain_http_boot_is_not_secure();
  test_exchange_on_port_80_beside_https_is_not_secure();
  test_exchange_without_pair_token_mints_nothing();
  test_first_boot_setup_flow_unchanged();
  test_reader_takes_the_cookie_named_cv_session();
  test_reader_never_takes_a_longer_name_or_a_value();
  test_reader_finds_the_real_cookie_behind_a_lookalike();
  test_reader_shape_rules_unchanged();
  test_dashboard_branch_reads_the_whole_name();
  test_sketch_text();
  if (g_failures) {
    std::printf("%d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d SESSION COOKIE CHECKS PASSED\n", g_checks);
  return 0;
}
