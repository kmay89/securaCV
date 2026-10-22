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
//     with every attribute it had plus "; Secure" (before: no Secure), named
//     __Host-cv_session (sweep F337; before: cv_session, so a browser that
//     held it refused the same name from an http page at the same host), and
//     that name=value sent back reaches the dashboard, beside a stale plain
//     cookie too;
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
//     real one (before: read as the session; the real one behind it lost).
//     And the cookie named __Host-cv_session by the same rules, read FIRST
//     wherever it sits; when it is present but malformed nothing is read
//     and the plain one is not taken instead (sweep F337; before: the
//     prefixed name was never read, and a plain cookie beside it was);
//   - handle_ui's dashboard branch through that reader: "xcv_session=<the
//     live session>" gets the landing page, not the dashboard;
//   - the sketch's text: the cookie's attributes are spelled once, in
//     session_cookie_header(), whose name argument follows the transport,
//     both Set-Cookie sites send its output, and no Strict-Transport-Security
//     header is sent;
//   - the Host question (sweep F344): request_host_names_device(),
//     send_foreign_host_page() and request_host_is_setup_name() are cut with
//     handle_ui, and so are handle_legacy_ui() (/admin, /settings) and
//     handle_companion_html() (/companion), over the sketch's staged
//     host_guard.h. After setup, a Host that cannot name this device (a
//     rebinding page's evil.example, a name that only contains canary.local,
//     a router name under another suffix, a trailing dot, no Host, and a
//     108-byte rebinding name too long for the 96-byte Host buffer whose
//     first 95 bytes end in ".local") gets the
//     fixed text/plain 403 on '/', '/?cv_pair=', /admin, /settings and
//     /companion: no landing, no pair token, no session, the owner's token
//     left unspent, and a live cookie under that name refused too (before:
//     each was served, and the exchange minted a session). Every name a
//     household uses (the .local name, any IP with or without a port, the
//     bare router name, .lan, .internal, .home.arpa; a 39-character label, a
//     full IPv6 literal, the longest Host the buffer holds) still lands and
//     exchanges. With setup active, the wizard redirect and its token answer
//     exactly canary.local or 192.168.4.1, any port, any case (before: any
//     Host that contained one of them, and not CANARY.LOCAL); every other
//     name gets the captive page and no token, and /companion serves under
//     any Host as before. The wizard token's re-issue
//     (handle_wifi_pair_token(), cut with http_send_json()) answers its token
//     at those names with no Access-Control-Allow-Origin and
//     Cache-Control: no-store (review of F344; before: through
//     http_send_json(), `*` and no-cache, so a page on another site in the
//     owner's browser on the SoftAP read it), and 404 with no token after
//     setup or under any other name;
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
//   - every cookie gate's Host question (review 3 of F344): the REAL
//     validator, session_validate_cookie(), cut with hex_decode_to(),
//     session_issue() and hex_encode() into that namespace. A session the
//     real store minted validates under the device's names and is refused
//     under a rebinding page's, none, the oversize one and the router names
//     the rule leaves out (before: it validated under all of them, so a
//     value a page set for its own name with document.cookie passed every
//     cookie gate); and csi_integration.cpp's text: the cv_session_validate()
//     bridge forwards to it, and it is the cookie reader's one caller;
//   - the cookie writer's budget: the HTTPS cookie is 140 bytes and the
//     plain one 125, refused in a buffer of exactly that size and written in
//     one byte more.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run-session-cookie

#include "arduino_globals.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "Arduino.h"           // stubs/receipt_grant
#include "esp_http_server.h"   // stubs/receipt_grant
#include "host_guard.h"        // the sketch's staged copy: the cut Host helpers call it

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
static int g_touches = 0;
inline bool is_active() { return g_active; }
inline void touch() { ++g_touches; }
}  // namespace setup_wizard

// The wizard token's re-issue (handle_wifi_pair_token) writes its answer with
// ArduinoJson, which is not on the host. Not ArduinoJson: a flat object of
// bool and string members, written as ArduinoJson writes them (no spaces,
// the members in the order they were set), with no escaping (the one string
// is the token's hex).
class String {
 public:
  const char* c_str() const { return text_.c_str(); }
  std::string text_;
};
class JsonDocument {
 public:
  class Member {
   public:
    explicit Member(std::string* json) : json_(json) {}
    void operator=(bool v) { *json_ = v ? "true" : "false"; }
    void operator=(const char* v) { *json_ = std::string("\"") + v + "\""; }

   private:
    std::string* json_;
  };
  Member operator[](const char* key) {
    for (auto& m : members_) if (m.first == key) return Member(&m.second);
    members_.emplace_back(key, "null");
    return Member(&members_.back().second);
  }
  std::vector<std::pair<std::string, std::string>> members_;
};
inline size_t serializeJson(const JsonDocument& doc, String& out) {
  out.text_ = "{";
  for (size_t i = 0; i < doc.members_.size(); ++i) {
    if (i) out.text_ += ",";
    out.text_ += "\"" + doc.members_[i].first + "\":" + doc.members_[i].second;
  }
  out.text_ += "}";
  return out.text_.size();
}

static const char CAPTIVE_PORTAL_HTML[] = "<p>Open canary.local</p>";
static const uint8_t CSI_DASHBOARD_HTML_GZ[] = {0x1f, 0x8b, 0x08};
static const size_t CSI_DASHBOARD_HTML_GZ_LEN = sizeof(CSI_DASHBOARD_HTML_GZ);
// The two other gzip pages: /admin and /settings (handle_legacy_ui), and
// /companion (handle_companion_html).
static const uint8_t CANARY_UI_HTML_GZ[] = {0x1f, 0x8b, 0x08, 0x01};
static const size_t CANARY_UI_HTML_GZ_LEN = sizeof(CANARY_UI_HTML_GZ);
static const uint8_t COMPANION_HTML_GZ[] = {0x1f, 0x8b, 0x08, 0x02};
static const size_t COMPANION_HTML_GZ_LEN = sizeof(COMPANION_HTML_GZ);

// Each page the gzip writer sent, by which one.
static int g_dashboards = 0;
static int g_admin_pages = 0;
static int g_companion_pages = 0;
static esp_err_t send_gzip_html(httpd_req_t* req, const uint8_t* gz, size_t gz_len) {
  (void)gz_len;
  const char* body = "<?>";
  if (gz == CSI_DASHBOARD_HTML_GZ) { ++g_dashboards; body = "<dashboard>"; }
  if (gz == CANARY_UI_HTML_GZ) { ++g_admin_pages; body = "<admin>"; }
  if (gz == COMPANION_HTML_GZ) { ++g_companion_pages; body = "<companion>"; }
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  return httpd_resp_sendstr(req, body);
}

namespace csi_integration {
constexpr size_t SESSION_COOKIE_HEX_LEN = 64;
constexpr size_t PAIR_TOKEN_HEX_LEN = 64;

// One live session and one live pair token, as the real stores would hold.
static std::string g_live_session;
static std::string g_live_pair;
static int g_sessions_issued = 0;
static int g_pair_tokens_issued = 0;
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
  ++g_pair_tokens_issued;
  g_live_pair.assign(PAIR_TOKEN_HEX_LEN, 'c');
  std::memcpy(hex_out, g_live_pair.c_str(), PAIR_TOKEN_HEX_LEN + 1);
  return true;
}
bool pair_token_consume(const char* hex) {
  if (!hex || g_live_pair.empty() || g_live_pair != hex) return false;
  g_live_pair.clear();   // one-shot
  return true;
}
// The landing mints a pair token for its one-tap link, as the real one does
// (csi_integration.cpp's send_pair_landing()).
bool send_pair_landing(httpd_req_t* req) {
  ++g_landings;
  char pair[PAIR_TOKEN_HEX_LEN + 1];
  pair_token_issue(pair, sizeof(pair));
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

// session_issue()'s entropy: a counter, so each session the real store
// mints here differs from the last.
uint8_t g_fill = 0;
void esp_fill_random(void* buf, size_t len) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  for (size_t i = 0; i < len; ++i) p[i] = (uint8_t)(++g_fill * 37u + i);
}
// Declared in csi_integration.h; its cut definition comes after its caller.
void hex_encode(const uint8_t* in, size_t len, char* out);

#include SESSION_COOKIE_SLOTS_INC
}  // namespace slots

namespace {

const std::string kAttrs = "; HttpOnly; SameSite=Strict; Path=/; Max-Age=86400";

void reset() {
  g_https_server = nullptr;
  setup_wizard::g_active = false;
  setup_wizard::g_touches = 0;
  csi_integration::g_live_session.clear();
  csi_integration::g_live_pair.clear();
  csi_integration::g_sessions_issued = 0;
  csi_integration::g_pair_tokens_issued = 0;
  csi_integration::g_landings = 0;
  g_dashboards = 0;
  g_admin_pages = 0;
  g_companion_pages = 0;
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

// GET /?cv_pair=<the live pair token> on `server`, under `host` (the
// device's .local name unless a test names another); the request it
// answered.
httpd_req_t pair_exchange(httpd_handle_t server, const char* host = "canary-7916.local") {
  char pair[csi_integration::PAIR_TOKEN_HEX_LEN + 1];
  CHECK(csi_integration::pair_token_issue(pair, sizeof(pair)));
  httpd_req_t r;
  r.handle = server;
  r.req_hdrs["Host"] = host;
  r.query = std::string("cv_pair=") + pair;
  CHECK(handle_ui(&r) == ESP_OK);
  CHECK(r.sends == 1);
  return r;
}

// The cookie's name follows the answer's transport too (sweep F337):
// __Host-cv_session with Secure over HTTPS, cv_session without it otherwise.
void check_exchange_cookie(const httpd_req_t& r, bool secure) {
  CHECK(r.status == "302 Found");
  CHECK(header(r, "Location") == "/");
  CHECK(header_count(r, "Set-Cookie") == 1);
  CHECK(csi_integration::g_sessions_issued == 1);
  const std::string want = std::string(secure ? "__Host-cv_session=" : "cv_session=") +
                           csi_integration::g_live_session + kAttrs +
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

// The HTTPS exchange's cookie carries the __Host- name, and that cookie
// sent back (name=value, as a browser sends it) signs the browser into the
// dashboard; the plain-HTTP boot's does the same under cv_session (sweep
// F337). Before, the HTTPS cookie was named cv_session, and the reader did
// not read the prefixed name back.
void test_https_name_is_host_prefixed_and_round_trips() {
  for (const bool tls : {true, false}) {
    const std::string why = tls ? "over HTTPS" : "on a plain-HTTP boot";
    reset();
    if (tls) https_up();
    const httpd_handle_t server = tls ? g_https_server : g_http_server;
    const httpd_req_t r = pair_exchange(server);
    const std::string set = header(r, "Set-Cookie");
    const std::string name = tls ? "__Host-cv_session=" : "cv_session=";
    CHECK_MSG(set.compare(0, name.size(), name) == 0,
              why + ": Set-Cookie was '" + set + "', want it to start '" + name + "'");
    // What the browser sends back: the cookie's name=value, no attributes.
    const std::string sent = set.substr(0, set.find(';'));
    CHECK(sent == name + csi_integration::g_live_session);
    httpd_req_t back;
    back.handle = server;
    back.req_hdrs["Host"] = "canary-7916.local";
    back.req_hdrs["Cookie"] = "theme=dark; " + sent;
    CHECK(handle_ui(&back) == ESP_OK);
    CHECK_MSG(g_dashboards == 1 && back.resp == "<dashboard>",
              why + ": the cookie sent back did not reach the dashboard (" + back.status + ")");
    CHECK(csi_integration::g_landings == 0);
    // Beside a stale plain cookie from another unit at the same name, the
    // HTTPS cookie still signs in: it is read first.
    if (tls) {
      reset();
      https_up();
      const httpd_req_t again = pair_exchange(g_https_server);
      const std::string fresh = header(again, "Set-Cookie");
      httpd_req_t both;
      both.handle = g_https_server;
      both.req_hdrs["Host"] = "canary.local";
      both.req_hdrs["Cookie"] = "cv_session=" + std::string(64, 'e') + "; " + fresh.substr(0, fresh.find(';'));
      CHECK(handle_ui(&both) == ESP_OK);
      CHECK_MSG(g_dashboards == 1, "the HTTPS cookie behind a stale plain one did not reach the dashboard");
    }
  }
}

// The writer's budget, as its comment states it: the HTTPS cookie is 140
// bytes (18 for the name and "=", 64 hex, 50 of attributes, 8 for
// "; Secure") and the plain one 125, each written only into a buffer that
// also holds the NUL. A writer that took a buffer one byte short would send
// the HTTPS cookie cut to "...; Secur", which a browser refuses for a
// __Host- name, and the sign-in would fail without a word.
void test_cookie_writer_budget() {
  const std::string hex(64, 'a');
  for (const bool tls : {true, false}) {
    reset();
    if (tls) https_up();
    httpd_req_t r;
    r.handle = tls ? g_https_server : g_http_server;
    const size_t len = tls ? 140 : 125;
    char out[160];
    CHECK_MSG(!session_cookie_header(&r, hex.c_str(), out, len),
              std::string("a ") + std::to_string(len) + "-byte buffer was said to hold the " + std::to_string(len) +
                  "-byte cookie");
    CHECK(session_cookie_header(&r, hex.c_str(), out, len + 1));
    CHECK_MSG(std::strlen(out) == len, std::string("the cookie is ") + std::to_string(std::strlen(out)) +
                                           " bytes, want " + std::to_string(len));
    CHECK(session_cookie_header(&r, hex.c_str(), out, sizeof(out)));   // both callers pass 160
  }
}

void test_exchange_without_pair_token_mints_nothing() {
  reset();
  https_up();
  httpd_req_t r;
  r.handle = g_https_server;
  r.req_hdrs["Host"] = "canary-7916.local";
  r.query = std::string("cv_pair=") + std::string(64, 'd');   // never issued
  CHECK(handle_ui(&r) == ESP_OK);
  CHECK(header_count(r, "Set-Cookie") == 0);
  CHECK(csi_integration::g_landings == 1);
  CHECK(csi_integration::g_sessions_issued == 0);
}

void test_first_boot_setup_flow_unchanged() {
  // The SoftAP's first boot runs HTTP-only, and / sends the phone's own
  // browser to the wizard with a pair token: no cookie, as before. The two
  // setup names match exactly, with any port and in any case (sweep F344;
  // before, CANARY.LOCAL:80 got the captive page: the substring test was
  // case-sensitive).
  for (const char* host : {"canary.local", "CANARY.LOCAL:80", "192.168.4.1", "192.168.4.1:80"}) {
    reset();
    setup_wizard::g_active = true;
    httpd_req_t r;
    r.handle = g_http_server;
    r.req_hdrs["Host"] = host;
    CHECK(handle_ui(&r) == ESP_OK);
    CHECK_MSG(r.status == "302 Found", std::string("setup Host ") + host + " answered " + r.status);
    CHECK_MSG(header(r, "Location") == "/companion?token=" + std::string(64, 'c'),
              std::string("setup Host ") + host + " was not sent to the wizard");
    CHECK(header_count(r, "Set-Cookie") == 0);
  }
  // A captive-portal probe for a foreign Host, another address, the
  // device's own steady-state names, and a name that only CONTAINS one of the
  // two setup names (the captive DNS resolves every one of them to the
  // device) get the static page and no pair token (before: the last two were
  // redirected to the wizard with a token, which authorizes the Wi-Fi join).
  for (const char* host : {"captive.apple.com", "connectivitycheck.gstatic.com", "10.0.0.5",
                           "canary-7916.local", "canary-7916", "canary.local.evil.example",
                           "192.168.4.1.evil.example"}) {
    reset();
    setup_wizard::g_active = true;
    httpd_req_t probe;
    probe.handle = g_http_server;
    probe.req_hdrs["Host"] = host;
    CHECK(handle_ui(&probe) == ESP_OK);
    CHECK_MSG(probe.status == "200 OK", std::string("setup probe ") + host + " answered " + probe.status);
    CHECK_MSG(probe.resp == CAPTIVE_PORTAL_HTML, std::string("setup probe ") + host + " was not given the captive page");
    CHECK_MSG(csi_integration::g_pair_tokens_issued == 0,
              std::string("setup probe ") + host + " was minted a pair token");
    CHECK(header_count(probe, "Set-Cookie") == 0);
  }
  // No Host at all: the captive page.
  reset();
  setup_wizard::g_active = true;
  httpd_req_t bare;
  bare.handle = g_http_server;
  CHECK(handle_ui(&bare) == ESP_OK);
  CHECK(bare.resp == CAPTIVE_PORTAL_HTML);
  CHECK(csi_integration::g_pair_tokens_issued == 0);
}

// The setup branch's question on its own (request_host_is_setup_name()):
// the same two lists, and the shapes around them.
bool setup_name(const char* host) {
  httpd_req_t r;
  if (host) r.req_hdrs["Host"] = host;
  return request_host_is_setup_name(&r);
}
void test_setup_name_is_exact() {
  for (const char* host : {"canary.local", "CANARY.LOCAL:80", "Canary.Local", "192.168.4.1", "192.168.4.1:80",
                           " canary.local ", "canary.local:443"}) {
    CHECK_MSG(setup_name(host), std::string("not a setup name: ") + host);
  }
  for (const char* host : {"captive.apple.com", "connectivitycheck.gstatic.com", "10.0.0.5", "canary-7916.local",
                           "canary-7916", "canary.local.evil.example", "192.168.4.1.evil.example",
                           "xcanary.local", "canary.local.", "192.168.4.10", "192.168.4.1x", "", "[::1]"}) {
    CHECK_MSG(!setup_name(host), std::string("a setup name: '") + host + "'");
  }
  CHECK(!setup_name(nullptr));                                   // no Host
  CHECK(!setup_name(std::string(120, 'a').c_str()));             // too long for the buffer
  // Too long for the buffer, and cut to it, still canary.local and a port:
  // never read as the setup name (review of F344: a helper that took the cut
  // header passed every other row, since a cut all-'a' name names nothing).
  CHECK_MSG(!setup_name(("canary.local:" + std::string(100, '8')).c_str()),
            "a truncated canary.local:<port> Host was read as the setup name");
}

// The wizard token's re-issue, GET /api/wifi/pair-token (review of F344):
// its answer carries the token and NO Access-Control-Allow-Origin, and is
// not stored. The wizard reads it same-origin (companion_pwa.h's
// refreshToken()), so a page on another site, running in the owner's own
// browser while that browser is on the SoftAP for setup, can make the
// request at http://192.168.4.1/ (a Host the exact-name rule must pass) and
// is not handed the body. Before, the answer went out through
// http_send_json(), which sends Access-Control-Allow-Origin: *, so such a
// page read a live pair token and could spend it on /api/wifi/connect or
// /api/wifi/ap-only (a body those parse as JSON whatever its Content-Type,
// so the POST needs no preflight). The request below carries the Origin
// such a fetch sends; the handler reads none, and must not need to.
void test_wizard_token_reissue_is_same_origin_only() {
  const std::string token_body = "{\"ok\":true,\"token\":\"" + std::string(64, 'c') + "\"}";
  for (const char* host : {"192.168.4.1", "canary.local", "CANARY.LOCAL:80", "192.168.4.1:80"}) {
    reset();
    setup_wizard::g_active = true;
    httpd_req_t r;
    r.handle = g_http_server;
    r.req_hdrs["Host"] = host;
    r.req_hdrs["Origin"] = "http://evil.example";
    CHECK(handle_wifi_pair_token(&r) == ESP_OK);
    const std::string why = std::string("the re-issue under Host ") + host;
    CHECK_MSG(r.status == "200 OK", why + " answered " + r.status);
    CHECK_MSG(r.type == "application/json", why + ": type " + r.type);
    CHECK_MSG(r.resp == token_body, why + ": body " + r.resp);
    CHECK_MSG(header_count(r, "Access-Control-Allow-Origin") == 0,
              why + " sent Access-Control-Allow-Origin '" + header(r, "Access-Control-Allow-Origin") +
                  "': a page on another site reads the token");
    CHECK_MSG(header(r, "Cache-Control") == "no-store", why + ": Cache-Control '" + header(r, "Cache-Control") + "'");
    CHECK(r.sends == 1);
    CHECK(csi_integration::g_pair_tokens_issued == 1);
    CHECK(setup_wizard::g_touches == 1);
  }
  // Setup over, or a name that is not one of the two: 404, no token, and
  // still no wildcard.
  struct Refused { bool setup; const char* host; };
  for (const Refused& row : {Refused{false, "192.168.4.1"}, Refused{false, "canary.local"},
                             Refused{true, "canary.local.evil.example"}, Refused{true, "canary-7916.local"},
                             Refused{true, nullptr}}) {
    reset();
    setup_wizard::g_active = row.setup;
    httpd_req_t r;
    r.handle = g_http_server;
    if (row.host) r.req_hdrs["Host"] = row.host;
    CHECK(handle_wifi_pair_token(&r) == ESP_OK);
    const std::string why = std::string(row.setup ? "during setup, " : "after setup, ") +
                            (row.host ? std::string("Host ") + row.host : std::string("no Host"));
    CHECK_MSG(r.status == "404 Not Found", why + " answered " + r.status);
    CHECK_MSG(r.resp == "{\"ok\":false}", why + ": body " + r.resp);
    CHECK_MSG(csi_integration::g_pair_tokens_issued == 0, why + " was minted a pair token");
    CHECK_MSG(header_count(r, "Access-Control-Allow-Origin") == 0, why + " sent Access-Control-Allow-Origin");
    CHECK(setup_wizard::g_touches == 0);
  }
  // http_send_json() is cut beside it so that a re-issue answering through
  // it again compiles here and fails the rows above, not the build.
  (void)&http_send_json;
}

// ── the Host question after setup (sweep F344) ───────────────────────────────

const char* const kRefusedPage =
    "This Canary answers only its own names. Open it at its .local name, its IP address, or the bare name your router gives it.";

// The fixed refusal: 403, text/plain, not cached, the one sentence, no
// cookie, nothing else sent.
void check_foreign_host_page(const httpd_req_t& r, const std::string& why) {
  CHECK_MSG(r.status == "403 Forbidden", why + ": answered " + r.status);
  CHECK_MSG(r.type == "text/plain; charset=utf-8", why + ": type " + r.type);
  CHECK_MSG(header(r, "Cache-Control") == "no-store", why + ": Cache-Control " + header(r, "Cache-Control"));
  CHECK_MSG(r.resp == kRefusedPage, why + ": body " + r.resp);
  CHECK_MSG(header_count(r, "Set-Cookie") == 0, why + ": a cookie was set");
  CHECK_MSG(r.sends == 1, why + ": sent " + std::to_string(r.sends) + " times");
}

// A rebinding name too long for the sketch's 96-byte Host buffer: 108 bytes,
// labels of 60 and 28 under evil.example (a name public DNS can mint), whose
// first 95 bytes end in ".local". Cut to the buffer it would read as a .local
// name, so only request_host_names_device()'s refusal of a truncated header
// keeps it foreign (review of F344: no row sent one, and a helper that took
// the truncated buffer passed both suites).
#define OVERSIZE_REBINDING_HOST                                    \
  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa." \
  "aaaaaaaaaaaaaaaaaaaaaaaaaaaa.local.evil.example"
static_assert(sizeof(OVERSIZE_REBINDING_HOST) - 1 == 108, "the oversize row is 108 bytes");
// The longest Host the buffer holds (95 bytes and the NUL), a .local name:
// served, so a shrunk buffer refuses a name the household uses.
#define LONGEST_HELD_HOST                                                   \
  "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb."      \
  "ccccccccccccccccccccccccc.local"
static_assert(sizeof(LONGEST_HELD_HOST) - 1 == 95, "the longest held row is 95 bytes");
// The shortest Host the buffer cannot hold (96 bytes: a 92-byte .local name
// and a port), a name host_guard.h accepts read whole (its own 96-byte copy
// holds the name without the port): refused, because the header is cut. The
// 108-byte row is foreign read whole too, so before this row a 128-byte Host
// buffer, in the pages' question or in the cookie validator's, passed both
// suites (review 6 of F344).
#define SHORTEST_CUT_HOST                                                   \
  "ddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd."      \
  "eeeeeeeeeeeeeeeeeeeeee.local:443"
static_assert(sizeof(SHORTEST_CUT_HOST) - 1 == 96, "the shortest cut row is 96 bytes");

// A DNS-rebinding page's names (one too long for the buffer), the shortest
// of the device's names the buffer cannot hold, an empty and a blank Host,
// and a request with no Host at all. host_guard.h calls an empty authority
// foreign; no browser sends one, but the pages and the receipt must not read
// it as the device's name (review 4 of F344: a request_host_names_device()
// that passed an empty or blank Host passed every row before these two).
const char* const kForeignHosts[] = {"evil.example", "evil.example:80", "canary.local.evil.example",
                                     "canary-7916.local.evil.example", "192.168.1.50.nip.io",
                                     OVERSIZE_REBINDING_HOST, SHORTEST_CUT_HOST, "", "   ", nullptr};
std::string host_label(const char* host) {
  return host ? std::string("Host '") + host + "'" : std::string("no Host");
}

void test_foreign_host_gets_no_pair_token_and_no_session() {
  for (const char* host : kForeignHosts) {
    const std::string why = host_label(host);
    // '/': no landing, no pair token (before: the landing and its token).
    reset();
    httpd_req_t landing;
    landing.handle = g_http_server;
    if (host) landing.req_hdrs["Host"] = host;
    CHECK(handle_ui(&landing) == ESP_OK);
    check_foreign_host_page(landing, why + " on /");
    CHECK_MSG(csi_integration::g_landings == 0, why + ": the landing was served");
    CHECK_MSG(csi_integration::g_pair_tokens_issued == 0, why + ": a pair token was minted");
    // '/?cv_pair=<the owner's live token>': no cookie, no session, and the
    // token still good for the owner (before: a session, the token spent).
    reset();
    char pair[csi_integration::PAIR_TOKEN_HEX_LEN + 1];
    CHECK(csi_integration::pair_token_issue(pair, sizeof(pair)));
    httpd_req_t exchange;
    exchange.handle = g_http_server;
    if (host) exchange.req_hdrs["Host"] = host;
    exchange.query = std::string("cv_pair=") + pair;
    CHECK(handle_ui(&exchange) == ESP_OK);
    check_foreign_host_page(exchange, why + " on /?cv_pair=");
    CHECK_MSG(csi_integration::g_sessions_issued == 0, why + ": the exchange minted a session");
    CHECK_MSG(csi_integration::g_live_pair == pair, why + ": the owner's pair token was spent");
    // The owner's own exchange under the device's name still takes it.
    httpd_req_t owner;
    owner.handle = g_http_server;
    owner.req_hdrs["Host"] = "canary-7916.local";
    owner.query = std::string("cv_pair=") + pair;
    CHECK(handle_ui(&owner) == ESP_OK);
    CHECK_MSG(owner.status == "302 Found" && header_count(owner, "Set-Cookie") == 1,
              why + ": the owner's token no longer exchanged");
    // A live session sent under that name: refused, not the dashboard.
    reset();
    const std::string live(64, '7');
    csi_integration::g_live_session = live;
    httpd_req_t cookie;
    cookie.handle = g_http_server;
    if (host) cookie.req_hdrs["Host"] = host;
    cookie.req_hdrs["Cookie"] = "cv_session=" + live;
    CHECK(handle_ui(&cookie) == ESP_OK);
    check_foreign_host_page(cookie, why + " with a live cookie");
    CHECK_MSG(g_dashboards == 0, why + ": a live cookie under a foreign name was served the dashboard");
  }
}

// Every name a household reaches its device by still lands, and its
// exchange sets exactly one cookie: the long ones too (a 39-character mDNS
// label, mdns_hostname's whole char[40], with a port; a full global IPv6
// literal with a port; the longest Host the 96-byte buffer holds).
void test_every_name_the_household_uses_still_lands() {
  // The oversize row is foreign only through the buffer: its first 95 bytes
  // ARE a name host_guard.h accepts, so the row above tests the cut.
  CHECK(canary::net::host_names_this_device(std::string(OVERSIZE_REBINDING_HOST, 95).c_str()));
  CHECK(canary::net::host_names_this_device(LONGEST_HELD_HOST));
  // And the 96-byte row is refused only by the buffer: the rule passes it.
  CHECK(canary::net::host_names_this_device(SHORTEST_CUT_HOST));
  for (const char* host : {"canary-7916.local", "canary-7916.local:443", "canary.local", "CANARY-7916.LOCAL",
                           "192.168.1.50", "192.168.1.50:80", "192.168.4.1", "[fe80::1]", "[fe80::1]:443",
                           "canary-7916", "canary-7916.lan", "canary-7916.home.arpa", "canary-7916.internal",
                           "canary-0123456789abcdef0123456789abcdef.local:443",
                           "[2001:db8:aaaa:bbbb:cccc:dddd:eeee:ffff]:443", LONGEST_HELD_HOST}) {
    const std::string why = std::string("Host ") + host;
    reset();
    httpd_req_t landing;
    landing.handle = g_http_server;
    landing.req_hdrs["Host"] = host;
    CHECK(handle_ui(&landing) == ESP_OK);
    CHECK_MSG(csi_integration::g_landings == 1 && landing.resp == "<landing>", why + ": no landing (" + landing.status + ")");
    reset();
    const httpd_req_t r = pair_exchange(g_http_server, host);
    CHECK_MSG(r.status == "302 Found" && header_count(r, "Set-Cookie") == 1,
              why + ": the exchange answered " + r.status + " with " +
                  std::to_string(header_count(r, "Set-Cookie")) + " cookie(s)");
    CHECK(csi_integration::g_sessions_issued == 1);
  }
}

// Router names under a suffix the shared rule does not list, and a
// trailing-dot name: the documented trade (sweep F344; F356 weighs widening
// the rule). Before, each was served the landing.
void test_router_names_outside_the_rule_get_the_refusal() {
  for (const char* host : {"canary-7916.fritz.box", "canary-7916.localdomain", "canary-7916.home",
                           "canary-7916.local."}) {
    const std::string why = std::string("Host ") + host;
    reset();
    httpd_req_t landing;
    landing.handle = g_http_server;
    landing.req_hdrs["Host"] = host;
    CHECK(handle_ui(&landing) == ESP_OK);
    check_foreign_host_page(landing, why + " on /");
    CHECK_MSG(csi_integration::g_pair_tokens_issued == 0, why + ": a pair token was minted");
    reset();
    char pair[csi_integration::PAIR_TOKEN_HEX_LEN + 1];
    CHECK(csi_integration::pair_token_issue(pair, sizeof(pair)));
    httpd_req_t exchange;
    exchange.handle = g_http_server;
    exchange.req_hdrs["Host"] = host;
    exchange.query = std::string("cv_pair=") + pair;
    CHECK(handle_ui(&exchange) == ESP_OK);
    check_foreign_host_page(exchange, why + " on /?cv_pair=");
    CHECK(csi_integration::g_sessions_issued == 0);
  }
}

// GET <page> under `host` (nullptr: no Host), through `handler`.
httpd_req_t page(esp_err_t (*handler)(httpd_req_t*), const char* host) {
  httpd_req_t r;
  r.handle = g_http_server;
  if (host) r.req_hdrs["Host"] = host;
  CHECK(handler(&r) == ESP_OK);
  return r;
}

// /admin and /settings (one handler) and /companion after setup: a foreign
// Host gets the fixed page; the device's names get the page. While the
// first-boot wizard runs, both serve under any Host, as before.
void test_pages_refuse_a_foreign_host_after_setup() {
  for (const char* host : kForeignHosts) {
    const std::string why = host_label(host);
    reset();
    check_foreign_host_page(page(handle_legacy_ui, host), why + " on /admin and /settings");
    CHECK_MSG(g_admin_pages == 0, why + ": /settings was served");
    check_foreign_host_page(page(handle_companion_html, host), why + " on /companion");
    CHECK_MSG(g_companion_pages == 0, why + ": /companion was served");
  }
  for (const char* host : {"canary-7916.local", "192.168.1.50", "canary-7916", "canary-7916.lan"}) {
    reset();
    CHECK_MSG(page(handle_legacy_ui, host).resp == "<admin>", std::string("/settings under ") + host);
    CHECK_MSG(page(handle_companion_html, host).resp == "<companion>", std::string("/companion under ") + host);
    CHECK(g_admin_pages == 1 && g_companion_pages == 1);
  }
  for (const char* host : {"canary.local", "192.168.4.1", "captive.apple.com", "evil.example"}) {
    reset();
    setup_wizard::g_active = true;
    CHECK_MSG(page(handle_companion_html, host).resp == "<companion>",
              std::string("/companion during setup under ") + host);
    CHECK_MSG(page(handle_legacy_ui, host).resp == "<admin>", std::string("/settings during setup under ") + host);
  }
  reset();
  setup_wizard::g_active = true;
  CHECK(page(handle_companion_html, nullptr).resp == "<companion>");
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

// Both names (sweep F337): __Host-cv_session (set over HTTPS) is read
// first, wherever it sits in the header, and when it is present but
// malformed nothing is read; plain cv_session is read when it is the only
// one. Before, the reader knew one name: the prefixed cookie alone read
// nothing, and a plain cookie in front of it was read instead.
void test_reader_takes_both_names_the_host_prefixed_first() {
  CHECK_MSG(read_of("__Host-cv_session=" + kLive) == kLive,
            "the __Host- cookie alone read " + read_of("__Host-cv_session=" + kLive));
  CHECK_MSG(read_of("cv_session=" + kOther + "; __Host-cv_session=" + kLive) == kLive,
            "a plain cookie in front of the __Host- one: read " +
                read_of("cv_session=" + kOther + "; __Host-cv_session=" + kLive));
  CHECK_MSG(read_of("__Host-cv_session=" + kLive + "; cv_session=" + kOther) == kLive,
            "the __Host- cookie in front of a plain one: read " +
                read_of("__Host-cv_session=" + kLive + "; cv_session=" + kOther));
  CHECK(read_of("theme=dark; __Host-cv_session=" + kLive + "; lang=en") == kLive);
  CHECK(read_of("theme=dark;\t__Host-cv_session=" + kLive) == kLive);
  // A present but malformed __Host- cookie reads nothing; the plain one
  // behind or in front of it is not taken instead.
  CHECK_MSG(read_of("__Host-cv_session=" + kLive.substr(0, 10) + "; cv_session=" + kOther) == "<none>",
            "a malformed __Host- cookie fell back to the plain one");
  CHECK_MSG(read_of("cv_session=" + kOther + "; __Host-cv_session=" + kLive + "g") == "<none>",
            "a malformed __Host- cookie behind a plain one fell back to it");
  CHECK(read_of("__Host-cv_session=; cv_session=" + kOther) == "<none>");
  // The first cookie with the __Host- name is the one read.
  CHECK(read_of("__Host-cv_session=" + kOther + "; __Host-cv_session=" + kLive) == kOther);
  // Plain cv_session alone is still read, before or after other cookies.
  CHECK(read_of("cv_session=" + kOther) == kOther);
  CHECK(read_of("theme=dark; cv_session=" + kOther) == kOther);
  // Never a longer name, another case, or a value holding the name.
  CHECK_MSG(read_of("x__Host-cv_session=" + kLive) == "<none>", "x__Host-cv_session was read as the session");
  CHECK_MSG(read_of("__host-cv_session=" + kLive) == "<none>", "__host-cv_session (lower case) was read as the session");
  CHECK_MSG(read_of("foo=__Host-cv_session=" + kLive) == "<none>", "a value holding __Host-cv_session= was read");
  CHECK_MSG(read_of("x__Host-cv_session=" + kOther + "; cv_session=" + kLive) == kLive,
            "a lookalike of the __Host- name hid the plain cookie");
}

void test_dashboard_branch_reads_the_whole_name() {
  // A live session, presented under a longer name: the landing page, not the
  // dashboard (before: the dashboard).
  reset();
  csi_integration::g_live_session = kLive;
  httpd_req_t lookalike;
  lookalike.handle = g_http_server;
  lookalike.req_hdrs["Host"] = "canary-7916.local";
  lookalike.req_hdrs["Cookie"] = "xcv_session=" + kLive;
  CHECK(handle_ui(&lookalike) == ESP_OK);
  CHECK_MSG(g_dashboards == 0, "xcv_session=<live> was served the dashboard");
  CHECK(csi_integration::g_landings == 1);
  // The live session behind a lookalike: the dashboard (before: the landing).
  reset();
  csi_integration::g_live_session = kLive;
  httpd_req_t behind;
  behind.handle = g_http_server;
  behind.req_hdrs["Host"] = "canary-7916.local";
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
  // The attributes are spelled once, in the writer, and the name follows
  // the transport (sweep F337): __Host-cv_session over HTTPS, else
  // cv_session. No other format string in the sketch names the cookie.
  CHECK(count_of(ino, "\"%s=%s; HttpOnly;") == 1);
  CHECK_MSG(count_of(ino, "\"cv_session=%s;") == 0, "a format string spells the cookie's name again");
  const std::string writer = fn_body(ino, "static bool session_cookie_header(");
  CHECK(writer.find("\"%s=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=86400%s\"") != std::string::npos);
  CHECK_MSG(writer.find("tls ? \"__Host-cv_session\" : \"cv_session\", session_hex, tls ? \"; Secure\" : \"\"") != std::string::npos,
            "session_cookie_header() does not name the cookie by its transport");
  // No Strict-Transport-Security header is sent (THREAT_MODEL: declined).
  CHECK_MSG(ino.find("\"Strict-Transport-Security\"") == std::string::npos,
            "the sketch sends a Strict-Transport-Security header");
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

  // The Host question (sweep F344). The sketch stages host_guard.h.
  CHECK_MSG(ino.find("\n#include \"host_guard.h\"") != std::string::npos, "canary_wap.ino does not include host_guard.h");
  // handle_ui asks the setup names exactly, then the device's names before
  // the cookie branch, and nothing in it tests a Host by substring.
  const std::string gate = "  if (!request_host_names_device(req)) return send_foreign_host_page(req);";
  CHECK(ui.find("    if (request_host_is_setup_name(req)) {") != std::string::npos);
  CHECK_MSG(ui.find(gate) != std::string::npos, "handle_ui does not ask the Host question after setup");
  CHECK_MSG(ui.find(gate) < ui.find("csi_integration::session_validate_cookie(req)") &&
                ui.find("if (setup_wizard::is_active()) {") < ui.find(gate),
            "handle_ui's Host question is not between the setup branch and the cookie");
  CHECK_MSG(ui.find("strstr(") == std::string::npos, "handle_ui tests the Host by substring again");
  // The wizard token's re-issue asks the same exact setup names; the old
  // substring helper is gone from the sketch.
  const std::string reissue = fn_body(ino, "static esp_err_t handle_wifi_pair_token(httpd_req_t* req) {");
  CHECK(!reissue.empty());
  CHECK_MSG(reissue.find("if (!setup_wizard::is_active() || !request_host_is_setup_name(req)) {") != std::string::npos,
            "the wizard token's re-issue does not ask the exact setup names");
  CHECK_MSG(ino.find("request_host_is_direct") == std::string::npos, "request_host_is_direct() is back");
  // /admin, /settings and /companion ask it after setup, first thing after
  // the request count.
  for (const char* sig : {"static esp_err_t handle_legacy_ui(httpd_req_t* req) {",
                          "static esp_err_t handle_companion_html(httpd_req_t* req) {"}) {
    const std::string body = fn_body(ino, sig);
    CHECK_MSG(body.find("  g_health.http_requests++;\n  if (!setup_wizard::is_active() && !request_host_names_device(req)) return send_foreign_host_page(req);\n") != std::string::npos,
              std::string(sig) + " does not ask the Host question first after setup");
  }
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

// ── every cookie gate asks the Host (review 3 of F344) ──────────────────────
// The REAL validator, session_validate_cookie(), cut with hex_decode_to(),
// session_issue() and hex_encode() into the slots namespace: the one
// question behind every cookie gate (CSI_AUTH_OR_RETURN, api_auth_check(),
// api_auth_check_optional(), api_auth_check_or_query() through the
// cv_session_validate() bridge; /tune, /mqtt and handle_ui directly).
// Closing the mints keeps a rebinding page from being HANDED a session, but
// not from holding one: any host on the LAN can take a session at the
// landing, and a rebinding page can set that value for its own name with
// document.cookie. Before, the validator read the cookie and the slot and no
// Host, so the value reached the live camera, the vault download, every
// session admin route and GET /api/pair/token from evil.example for its
// 24 h. Now a Host that cannot name the device (host_guard.h's rule, as the
// pages ask it) is refused before the cookie is read.

// GET with `cookie` under `host` (nullptr: no Host), through the real
// validator.
bool cookie_validates(const char* host, const std::string& cookie) {
  httpd_req_t r;
  r.handle = g_http_server;
  if (host) r.req_hdrs["Host"] = host;
  r.req_hdrs["Cookie"] = cookie;
  return slots::session_validate_cookie(&r);
}

void test_every_cookie_gate_asks_the_host() {
  clear_slots();
  const uint64_t t = 7000000;
  set_clock(t);
  // A session the real store minted, as a LAN guest's ?cv_pair exchange at
  // the device's IP would: 64 hex, sent back as a browser sends it.
  char hex[65];
  CHECK(slots::session_issue(hex, sizeof(hex)));
  CHECK(std::strlen(hex) == 64);
  const std::string plain = std::string("cv_session=") + hex;
  const std::string prefixed = std::string("__Host-cv_session=") + hex;
  set_clock(t + 6ULL * 60 * 60 * 1000);   // six hours on: well inside its 24 h
  // The device's names: the session signs in, under either cookie name.
  for (const char* host : {"192.168.1.50", "192.168.1.50:80", "canary-7916.local", "canary-7916.local:443",
                           "CANARY-7916.LOCAL", "canary.local", "192.168.4.1", "[fe80::1]:443", "canary-7916",
                           "canary-7916.lan", "canary-7916.home.arpa", "canary-7916.internal", LONGEST_HELD_HOST}) {
    CHECK_MSG(cookie_validates(host, plain), std::string("Host ") + host + ": the live session was refused");
    CHECK_MSG(cookie_validates(host, "theme=dark; " + prefixed),
              std::string("Host ") + host + ": the live __Host- session was refused");
  }
  // A rebinding page's names, an empty or blank Host, none, the 108-byte
  // one, the 96-byte one the buffer cannot hold, and the router names the
  // rule leaves out: the same live value, set by the page itself for its own
  // name, is refused (before: it validated, so every cookie gate let it in).
  for (const char* host : kForeignHosts) {
    CHECK_MSG(!cookie_validates(host, plain), host_label(host) + ": a live session validated under it");
    CHECK_MSG(!cookie_validates(host, prefixed), host_label(host) + ": a live __Host- session validated under it");
  }
  for (const char* host : {"canary-7916.fritz.box", "canary-7916.localdomain", "canary-7916.home",
                           "canary-7916.local."}) {
    CHECK_MSG(!cookie_validates(host, plain), std::string("Host '") + host + "': a live session validated under it");
  }
  // Asking under a foreign name spends nothing: the owner's session still
  // signs in under the device's name.
  CHECK(cookie_validates("canary-7916.local", plain));
  // The rest of the validator is unchanged: a value no slot holds, a
  // malformed one, and the live one at 24 h are refused under the device's
  // own name.
  CHECK(!cookie_validates("canary-7916.local", "cv_session=" + std::string(64, 'f')));
  CHECK(!cookie_validates("canary-7916.local", std::string("cv_session=") + std::string(hex).substr(0, 63)));
  set_clock(t + kDayMs);
  CHECK_MSG(!cookie_validates("canary-7916.local", plain), "the session still validated at 24 h");
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
  // Every cookie gate reaches the validator test_every_cookie_gate_asks_the_host
  // drives (review 3 of F344): the bridge api_auth.h and CSI_AUTH_OR_RETURN
  // call forwards to it, and it is the cookie reader's one caller.
  const std::string bridge = fn_body(csi, "bool cv_session_validate(httpd_req_t* req) {");
  CHECK_MSG(squeeze(bridge).find("returncsi_integration::session_validate_cookie(req);") != std::string::npos,
            "cv_session_validate() no longer forwards to session_validate_cookie()");
  CHECK_MSG(count_of(csi, "read_session_cookie_hex(") == 2,
            "read_session_cookie_hex() has a caller other than session_validate_cookie()");
  CHECK(fn_body(csi, "bool session_validate_cookie(httpd_req_t* req) {").find("read_session_cookie_hex(req, hex)") !=
        std::string::npos);
}

}  // namespace

int main() {
  test_exchange_over_https_is_secure();
  test_exchange_on_plain_http_boot_is_not_secure();
  test_exchange_on_port_80_beside_https_is_not_secure();
  test_https_name_is_host_prefixed_and_round_trips();
  test_cookie_writer_budget();
  test_exchange_without_pair_token_mints_nothing();
  test_first_boot_setup_flow_unchanged();
  test_setup_name_is_exact();
  test_wizard_token_reissue_is_same_origin_only();
  test_foreign_host_gets_no_pair_token_and_no_session();
  test_every_name_the_household_uses_still_lands();
  test_router_names_outside_the_rule_get_the_refusal();
  test_pages_refuse_a_foreign_host_after_setup();
  test_reader_takes_the_cookie_named_cv_session();
  test_reader_never_takes_a_longer_name_or_a_value();
  test_reader_finds_the_real_cookie_behind_a_lookalike();
  test_reader_shape_rules_unchanged();
  test_reader_takes_both_names_the_host_prefixed_first();
  test_dashboard_branch_reads_the_whole_name();
  test_sketch_text();
  test_session_lives_24_hours_at_any_uptime();
  test_pair_token_lives_10_minutes_at_any_uptime();
  test_eviction_across_the_mark();
  test_every_cookie_gate_asks_the_host();
  test_store_text();
  if (g_failures) {
    std::printf("%d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d SESSION COOKIE CHECKS PASSED\n", g_checks);
  return 0;
}
