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
//     session_cookie_header(), and both Set-Cookie sites send its output;
//   - the two token stores' clock (sweep F347): the slot functions cut
//     verbatim out of csi_integration.cpp (pick_slot_for_issuance(),
//     find_slot(), pick_session_slot_for_issuance(), find_valid_session(),
//     ct_eq()) into a namespace this file opens for them, over one 64-bit
//     test clock whose low 32 bits are millis() and whose whole seconds are
//     uptime_s(). A session is valid at 24 h less a second and not at 24 h,
//     a pair token likewise at 10 minutes, and neither is valid again 2^32
//     ms + 5 s after issue (before, on millis(), both were: a slot not
//     reused since it expired took its old cookie or token again); eviction
//     picks an expired slot, so 49.7-day-old slots are all expired, else the
//     oldest across the 2^32 ms mark. The harness's slot structs carry both
//     the old issued_ms and the new issued_s, so the same rows run against
//     either cut; csi_integration.cpp's text (its path is passed in) says the
//     real slots carry issued_s only, stamped from uptime_s(), which reads
//     esp_timer_get_time(), and that the constants mirrored here are its own.
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
#ifndef SESSION_COOKIE_SLOTS_INC
#error "SESSION_COOKIE_SLOTS_INC must name the cut of csi_integration.cpp's slot functions (the Makefile passes it)"
#endif
#ifndef CSI_INTEGRATION_CPP
#error "CSI_INTEGRATION_CPP must name csi_integration.cpp (the Makefile passes it)"
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
// is on g_https_server and port 80 (g_http_server) keeps only the captive
// probes, /api/fleet and the redirect, none of which reads the cookie; on
// a plain-HTTP boot g_https_server stays nullptr. Any two distinct addresses
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

// ── the token stores' slot functions, cut verbatim out of csi_integration.cpp ─
// In a namespace of their own, beside the stand-in stores handle_ui uses
// above. The constants mirror csi_integration.cpp's (test_store_text() holds
// them to it). Each slot carries the old issued_ms AND the new issued_s, and
// stamp() below sets both from one clock, so the cut before sweep F347
// (millis()) and after it (uptime_s()) compile and run the same rows.
namespace slots {
constexpr size_t   PAIR_SLOTS        = 4;
constexpr uint32_t PAIR_TTL_MS       = 10UL * 60UL * 1000UL;
constexpr uint32_t PAIR_TTL_S        = PAIR_TTL_MS / 1000UL;
constexpr size_t   PAIR_TOK_BYTES    = 32;
constexpr size_t   SESSION_SLOTS     = 8;
constexpr uint32_t SESSION_TTL_MS    = 24UL * 60UL * 60UL * 1000UL;
constexpr uint32_t SESSION_TTL_S     = SESSION_TTL_MS / 1000UL;
constexpr size_t   SESSION_TOK_BYTES = 32;

struct PairSlot {
  bool     used;
  bool     active;
  uint32_t issued_ms;
  uint32_t issued_s;
  uint8_t  token[PAIR_TOK_BYTES];
};
PairSlot g_pair_slots[PAIR_SLOTS] = {};

struct SessionSlot {
  bool     active;
  uint32_t issued_ms;
  uint32_t issued_s;
  uint8_t  token[SESSION_TOK_BYTES];
};
SessionSlot g_session_slots[SESSION_SLOTS] = {};

// The test clock: milliseconds since boot, 64 bits wide, as
// esp_timer_get_time() counts (in microseconds). millis() is its low 32
// bits (stub_grant::now_ms, set by set_clock()); uptime_s() its whole
// seconds, as csi_integration.cpp's reads esp_timer_get_time() / 10^6.
uint64_t g_clock_ms = 0;
uint32_t uptime_s() { return (uint32_t)(g_clock_ms / 1000ULL); }

#include SESSION_COOKIE_SLOTS_INC
}  // namespace slots

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

// ── the token stores' clock (sweep F347) ─────────────────────────────────────

const uint64_t kWrapMs = 1ULL << 32;            // 2^32 ms: 49.7 days of millis()
const uint64_t kDayMs = 24ULL * 60 * 60 * 1000;
const uint64_t kTenMinMs = 10ULL * 60 * 1000;

void set_clock(uint64_t ms) {
  slots::g_clock_ms = ms;
  stub_grant::now_ms = (uint32_t)ms;
}
void clear_slots() {
  for (auto& p : slots::g_pair_slots) p = slots::PairSlot{};
  for (auto& q : slots::g_session_slots) q = slots::SessionSlot{};
}
// A session issued now, as session_issue() does: the slot the cut picks, a
// token of `tag` bytes, both clock fields stamped. Its slot index.
size_t issue_session(uint8_t tag) {
  slots::SessionSlot* s = slots::pick_session_slot_for_issuance();
  std::memset(s->token, tag, sizeof(s->token));
  s->issued_ms = (uint32_t)slots::g_clock_ms;
  s->issued_s = slots::uptime_s();
  s->active = true;
  return (size_t)(s - slots::g_session_slots);
}
bool session_ok(uint8_t tag) {
  uint8_t tok[slots::SESSION_TOK_BYTES];
  std::memset(tok, tag, sizeof(tok));
  return slots::find_valid_session(tok);
}
size_t issue_pair(uint8_t tag) {
  slots::PairSlot* p = slots::pick_slot_for_issuance();
  std::memset(p->token, tag, sizeof(p->token));
  p->issued_ms = (uint32_t)slots::g_clock_ms;
  p->issued_s = slots::uptime_s();
  p->used = false;
  p->active = true;
  return (size_t)(p - slots::g_pair_slots);
}
bool pair_ok(uint8_t tag) {
  uint8_t tok[slots::PAIR_TOK_BYTES];
  std::memset(tok, tag, sizeof(tok));
  return slots::find_slot(tok) != nullptr;
}

void test_session_lives_24_hours_at_any_uptime() {
  clear_slots();
  const uint64_t t = 3000000;   // 50 minutes after boot
  set_clock(t);
  issue_session(0x5a);
  CHECK(session_ok(0x5a));
  set_clock(t + kDayMs - 1000);
  CHECK_MSG(session_ok(0x5a), "a session was refused a second before its 24 h");
  set_clock(t + kDayMs);
  CHECK_MSG(!session_ok(0x5a), "a session was still valid at 24 h");
  // millis() comes back round to the issue: 2^32 ms + 5 s later. Before,
  // the slot read 5 s old and took its old cookie for another 24 h.
  set_clock(t + kWrapMs + 5000);
  CHECK(stub_grant::now_ms == (uint32_t)(t + 5000));   // millis() reads 5 s after the issue
  CHECK_MSG(!session_ok(0x5a), "a 49.7-day-old session took its old cookie again");
  // A session issued past the 2^32 ms mark lives its 24 h like any other.
  set_clock(kWrapMs + 7000);
  issue_session(0x6b);
  set_clock(kWrapMs + 7000 + kDayMs - 1000);
  CHECK(session_ok(0x6b));
  set_clock(kWrapMs + 7000 + kDayMs);
  CHECK(!session_ok(0x6b));
}

void test_pair_token_lives_10_minutes_at_any_uptime() {
  clear_slots();
  const uint64_t t = 4321000;
  set_clock(t);
  issue_pair(0x3c);
  CHECK(pair_ok(0x3c));
  set_clock(t + kTenMinMs - 1000);
  CHECK_MSG(pair_ok(0x3c), "a pair token was refused a second before its 10 minutes");
  set_clock(t + kTenMinMs);
  CHECK_MSG(!pair_ok(0x3c), "a pair token was still valid at 10 minutes");
  set_clock(t + kWrapMs + 5000);
  CHECK_MSG(!pair_ok(0x3c), "a 49.7-day-old pair token was valid again");
}

void test_eviction_across_the_mark() {
  // Eight sessions a minute apart, then 2^32 ms + 1 s after the first: every
  // slot is 49.7 days old, so the next issue takes an expired slot (the
  // first), and no old cookie is valid. Before, on millis(), slot 0 read
  // 1 s old: it kept its cookie and the issue went to slot 1.
  clear_slots();
  const uint64_t t = 9000000;
  for (uint8_t k = 0; k < slots::SESSION_SLOTS; ++k) {
    set_clock(t + 60000ULL * k);
    CHECK(issue_session((uint8_t)(0x10 + k)) == k);
  }
  set_clock(t + kWrapMs + 1000);
  for (uint8_t k = 0; k < slots::SESSION_SLOTS; ++k) {
    CHECK_MSG(!session_ok((uint8_t)(0x10 + k)), "a 49.7-day-old session slot " + std::to_string(k) + " was valid");
  }
  CHECK_MSG(slots::pick_session_slot_for_issuance() == &slots::g_session_slots[0],
            "the issue did not take the first expired slot");
  // None expired: the oldest is evicted, with the clock past the 32-bit mark.
  clear_slots();
  const uint64_t t0 = kWrapMs - 3ULL * 60 * 60 * 1000;   // three hours before the mark
  for (uint8_t k = 0; k < slots::SESSION_SLOTS; ++k) {
    set_clock(t0 + 30ULL * 60 * 1000 * k);                // every 30 minutes, across it
    issue_session((uint8_t)(0x20 + k));
  }
  set_clock(t0 + 4ULL * 60 * 60 * 1000);
  for (uint8_t k = 0; k < slots::SESSION_SLOTS; ++k) CHECK(session_ok((uint8_t)(0x20 + k)));
  CHECK_MSG(slots::pick_session_slot_for_issuance() == &slots::g_session_slots[0],
            "the oldest session was not the one evicted");
  // The pair store: a used slot first, then an expired one, else the oldest.
  clear_slots();
  for (uint8_t k = 0; k < slots::PAIR_SLOTS; ++k) {
    set_clock(t + 30000ULL * k);
    CHECK(issue_pair((uint8_t)(0x40 + k)) == k);
  }
  set_clock(t + kWrapMs + 1000);
  for (uint8_t k = 0; k < slots::PAIR_SLOTS; ++k) {
    CHECK_MSG(!pair_ok((uint8_t)(0x40 + k)), "a 49.7-day-old pair token " + std::to_string(k) + " was valid");
  }
  CHECK_MSG(slots::pick_slot_for_issuance() == &slots::g_pair_slots[0],
            "the pair issue did not take the first expired slot");
  clear_slots();
  for (uint8_t k = 0; k < slots::PAIR_SLOTS; ++k) {
    set_clock(kWrapMs - 120000 + 60000ULL * k);           // two minutes before the mark, a minute apart
    issue_pair((uint8_t)(0x50 + k));
  }
  set_clock(kWrapMs + 240000);
  for (uint8_t k = 0; k < slots::PAIR_SLOTS; ++k) CHECK(pair_ok((uint8_t)(0x50 + k)));
  CHECK_MSG(slots::pick_slot_for_issuance() == &slots::g_pair_slots[0],
            "the oldest pair token was not the one evicted");
  slots::g_pair_slots[2].used = true;
  CHECK(slots::pick_slot_for_issuance() == &slots::g_pair_slots[2]);
}

// csi_integration.cpp with its comments stripped and its whitespace dropped,
// for a text check that does not care how a line is aligned.
std::string squeeze(const std::string& t) {
  std::string out;
  for (char c : t) if (c != ' ' && c != '\t') out += c;
  return out;
}
std::string strip_comments(const std::string& t) {
  std::string out;
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
        if (c == '\\' && i + 1 < t.size()) out += t[++i];
        else if ((st == STR && c == '"') || (st == CHR && c == '\'')) st = CODE;
        break;
    }
  }
  return out;
}

// A struct's text, from the line that starts with `sig` to its "};" line.
std::string struct_body(const std::string& src, const std::string& sig) {
  const size_t at = src.find("\n" + sig);
  if (at == std::string::npos) return "";
  const size_t end = src.find("\n};\n", at);
  return end == std::string::npos ? "" : src.substr(at, end - at);
}

void test_store_text() {
  const std::string csi = strip_comments(read_file(CSI_INTEGRATION_CPP));
  CHECK_MSG(!csi.empty(), std::string("could not read ") + CSI_INTEGRATION_CPP);
  const std::string sq = squeeze(csi);
  // The constants this file mirrors are csi_integration.cpp's.
  for (const char* decl : {"constexprsize_tPAIR_SLOTS=4;", "constexpruint32_tPAIR_TTL_MS=10UL*60UL*1000UL;",
                           "constexpruint32_tPAIR_TTL_S=PAIR_TTL_MS/1000UL;", "constexprsize_tPAIR_TOK_BYTES=32;",
                           "constexprsize_tSESSION_SLOTS=8;",
                           "constexpruint32_tSESSION_TTL_MS=24UL*60UL*60UL*1000UL;",
                           "constexpruint32_tSESSION_TTL_S=SESSION_TTL_MS/1000UL;",
                           "constexprsize_tSESSION_TOK_BYTES=32;"}) {
    CHECK_MSG(sq.find(decl) != std::string::npos, std::string("csi_integration.cpp no longer declares ") + decl);
  }
  // Both slots age on issued_s, and nothing names issued_ms.
  const std::string pair = struct_body(csi, "struct PairSlot {");
  const std::string session = struct_body(csi, "struct SessionSlot {");
  CHECK(!pair.empty() && !session.empty());
  CHECK(squeeze(pair).find("uint32_tissued_s;") != std::string::npos);
  CHECK(squeeze(session).find("uint32_tissued_s;") != std::string::npos);
  CHECK_MSG(count_of(csi, "issued_ms") == 0, "csi_integration.cpp still names issued_ms");
  // The clock: whole seconds of esp_timer_get_time()'s microseconds.
  CHECK(sq.find("uint32_tuptime_s(){return(uint32_t)(esp_timer_get_time()/1000000LL);}") != std::string::npos);
  // The four slot functions read uptime_s() and never millis().
  for (const char* sig : {"PairSlot* pick_slot_for_issuance() {", "PairSlot* find_slot(const uint8_t* token) {",
                          "SessionSlot* pick_session_slot_for_issuance() {",
                          "bool find_valid_session(const uint8_t* token) {"}) {
    const std::string body = fn_body(csi, sig);
    CHECK_MSG(!body.empty(), std::string("not found: ") + sig);
    CHECK_MSG(body.find("millis()") == std::string::npos, std::string(sig) + " reads millis()");
    CHECK_MSG(body.find("const uint32_t now = uptime_s();") != std::string::npos, std::string(sig) + " does not read uptime_s()");
  }
  // And the two issuers stamp from it.
  for (const char* sig : {"bool session_issue(char* hex_out, size_t out_cap) {",
                          "bool pair_token_issue(char* hex_out, size_t out_cap) {"}) {
    const std::string body = fn_body(csi, sig);
    CHECK_MSG(!body.empty(), std::string("not found: ") + sig);
    CHECK_MSG(squeeze(body).find("s->issued_s=uptime_s();") != std::string::npos,
              std::string(sig) + " does not stamp issued_s from uptime_s()");
    CHECK_MSG(body.find("millis()") == std::string::npos, std::string(sig) + " reads millis()");
  }
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
  test_session_lives_24_hours_at_any_uptime();
  test_pair_token_lives_10_minutes_at_any_uptime();
  test_eviction_across_the_mark();
  test_store_text();
  if (g_failures) {
    std::printf("%d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d SESSION COOKIE CHECKS PASSED\n", g_checks);
  return 0;
}
