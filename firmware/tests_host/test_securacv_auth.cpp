/* test_securacv_auth.cpp — the flagship's bearer check and its lockout, on
 * the REAL canary/lib/securacv_auth/src/securacv_auth.cpp (compiled whole,
 * over stubs/securacv_auth: a movable 32-bit millis(), a header-map httpd
 * request, a token derive that needs no mbedtls).
 *
 * Sweep F340. AuthManager::check() counted a request with no Authorization
 * header as a strike. The flagship's dashboard withholds the token on a
 * home-network load (no BOOT tap, no recovery-kit token), so its api() sends
 * no header while it polls /api/status every 2 s through auth_gate, and the
 * lockout is one per device: the owner's own open tab held every bearer
 * caller, the iPhone app's right token included, at 429. A request with no
 * credential carries no guess; it is now a 401 and no strike, the WAP's rule
 * (canary-wap's auth_logic.h). A malformed or wrong credential is still a
 * strike, and the right one still clears the count.
 *
 * Each case drives auth_check() the way auth_gate() does, with the token
 * auth_load_or_derive() made, and reads what the library sent.
 *
 * Build/run: make -C firmware/tests_host. */

#include <cstdio>
#include <cstring>
#include <string>

#include "securacv_auth.h"

static int g_checks = 0;
static int g_fail = 0;
#define CHECK(cond, msg) do { \
  ++g_checks; \
  if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); g_fail++; } \
  else { std::printf("ok   %s\n", msg); } } while (0)

static const char* token() { return auth_get_token(); }

static httpd_req_t no_header() { return httpd_req_t{}; }

static httpd_req_t with_auth(const std::string& value) {
  httpd_req_t r{};
  r.headers["Authorization"] = value;
  return r;
}

static httpd_req_t right_bearer() { return with_auth(std::string("Bearer ") + token()); }

// Same length as the device token, one character off: the shape a guesser
// sends, compared in constant time and refused 403.
static httpd_req_t wrong_bearer() {
  std::string t = token();
  t.back() = t.back() == 'f' ? 'g' : 'f';
  return with_auth("Bearer " + t);
}

static void fresh(uint32_t now_ms) {
  auth_get_instance().reset();
  g_host_millis = now_ms;
}

static bool is_429(const httpd_req_t& r) { return r.status.rfind("429", 0) == 0; }

/* ── The owner's tab and the owner's app ──────────────────────────────── */

/* Ten minutes of a token-less dashboard tab (a header-less /api/status every
 * 2 s: 300 calls) beside the app's right bearer every 5 s (120 calls). Every
 * app call is served, and every tab call is the 401 with its
 * WWW-Authenticate challenge, never a 429. Before F340 the tab's calls armed
 * the one lockout and held the app's: 119 of the 120 were refused. */
static void header_less_requests_are_no_strike() {
  fresh(1000);
  int tab = 0, tab_401 = 0, tab_429 = 0, tab_sends_ok = 0;
  int app = 0, app_ok = 0, app_429 = 0, app_silent = 0;
  for (uint32_t s = 0; s < 600; s++) {
    g_host_millis = 1000 + s * 1000;
    if (s % 2 == 0) {
      httpd_req_t r = no_header();
      const bool ok = auth_check(&r, token());
      tab++;
      if (!ok && r.status == "401 Unauthorized" &&
          r.resp_headers["WWW-Authenticate"] == "Bearer realm=\"securacv\"") tab_401++;
      if (is_429(r)) tab_429++;
      if (r.sends == 1) tab_sends_ok++;
    }
    if (s % 5 == 1) {
      httpd_req_t r = right_bearer();
      const bool ok = auth_check(&r, token());
      app++;
      if (ok) app_ok++;
      if (is_429(r)) app_429++;
      if (r.sends == 0) app_silent++;
    }
  }
  std::printf("     tab %d calls (%d 401, %d 429); app %d calls (%d served, %d 429)\n",
              tab, tab_401, tab_429, app, app_ok, app_429);
  CHECK(tab == 300 && app == 120, "the scenario: 300 tab calls, 120 app calls");
  CHECK(app_ok == 120, "every one of the app's right-bearer calls is served beside the token-less tab");
  CHECK(app_silent == 120, "a served call sends nothing (the handler answers it)");
  CHECK(app_429 == 0, "the app is never held at 429 by the tab");
  CHECK(tab_401 == 300, "every token-less call is the 401 with the Bearer challenge");
  CHECK(tab_429 == 0, "no token-less call is ever a 429: it armed nothing");
  CHECK(tab_sends_ok == 300, "every token-less call is answered exactly once");
}

/* A header-less call between a wrong bearer's backoff and the owner's next
 * try adds nothing to it: the right bearer at the end of the first 2 s step
 * is served. Before F340 the header-less call was a second strike, a 4 s
 * backoff, and the right bearer got 429. */
static void a_header_less_call_never_extends_a_backoff() {
  fresh(50000);
  httpd_req_t w = wrong_bearer();
  CHECK(!auth_check(&w, token()) && w.status == "403 Forbidden",
        "a wrong bearer is refused 403");
  g_host_millis = 50000 + 2000;
  httpd_req_t n = no_header();
  CHECK(!auth_check(&n, token()) && n.status == "401 Unauthorized",
        "a header-less call after the 2 s step is the 401");
  g_host_millis = 50000 + 2001;
  httpd_req_t r = right_bearer();
  CHECK(auth_check(&r, token()) && r.sends == 0,
        "the right bearer right after it is served: the header-less call struck nothing");
}

/* ── What is still a strike ───────────────────────────────────────────── */

/* Each credential a guesser can send is one strike: the next right bearer,
 * inside the 2 s first step, is the lockout's 429 with Retry-After; at the
 * step's end it is served and clears the count. */
static void credential_shaped_failures_still_strike() {
  struct Case { const char* name; httpd_req_t req; const char* status; };
  const std::string too_long = "Bearer " + std::string(SECURACV_AUTH_HEADER_BUF, 'a');
  Case cases[] = {
    {"a wrong bearer", wrong_bearer(), "403 Forbidden"},
    {"a wrong-length bearer", with_auth("Bearer cv_short"), "403 Forbidden"},
    {"a non-Bearer scheme", with_auth(std::string("Basic ") + token()), "401 Unauthorized"},
    {"an oversized header", with_auth(too_long), "401 Unauthorized"},
  };
  for (Case& c : cases) {
    fresh(100000);
    char msg[160];
    const bool ok = auth_check(&c.req, token());
    std::snprintf(msg, sizeof(msg), "%s is refused %s", c.name, c.status);
    CHECK(!ok && c.req.status == c.status, msg);

    g_host_millis = 100000 + 1999;
    httpd_req_t held = right_bearer();
    const bool held_ok = auth_check(&held, token());
    std::snprintf(msg, sizeof(msg), "%s is a strike: the right bearer 1999 ms later is 429", c.name);
    CHECK(!held_ok && is_429(held), msg);
    std::snprintf(msg, sizeof(msg), "%s: the 429 says Retry-After 1 and too_many_requests", c.name);
    CHECK(held.resp_headers["Retry-After"] == "1" &&
          held.body == "{\"error\":\"too_many_requests\",\"retry_after_s\":1}", msg);

    g_host_millis = 100000 + 2000;
    httpd_req_t served = right_bearer();
    std::snprintf(msg, sizeof(msg), "%s: at the step's end the right bearer is served", c.name);
    CHECK(auth_check(&served, token()), msg);

    httpd_req_t again = wrong_bearer();
    auth_check(&again, token());
    g_host_millis = 100000 + 2000 + 1999;
    httpd_req_t step = right_bearer();
    std::snprintf(msg, sizeof(msg), "%s: the success cleared the count (the next strike is 2 s again)", c.name);
    CHECK(!auth_check(&step, token()) && step.resp_headers["Retry-After"] == "1", msg);
  }
}

/* While a backoff runs nothing is compared and nothing more is recorded: a
 * header-less call is the 429 too (the lockout is read first), and the
 * backoff still ends where the strike put it. */
static void a_running_backoff_answers_429_first() {
  fresh(200000);
  httpd_req_t w = wrong_bearer();
  auth_check(&w, token());
  g_host_millis = 200000 + 1000;
  httpd_req_t n = no_header();
  CHECK(!auth_check(&n, token()) && is_429(n) && n.resp_headers["Retry-After"] == "2",
        "a header-less call inside the backoff is the 429, Retry-After 2");
  g_host_millis = 200000 + 2000;
  httpd_req_t r = right_bearer();
  CHECK(auth_check(&r, token()), "the backoff ends where the strike put it");
}

int main() {
  const uint8_t key[32] = {1};
  CHECK(auth_load_or_derive(key) && std::strlen(token()) == 35,
        "auth_load_or_derive() makes a token of the real shape");

  header_less_requests_are_no_strike();
  a_header_less_call_never_extends_a_backoff();
  credential_shaped_failures_still_strike();
  a_running_backoff_answers_429_first();

  if (g_fail) {
    std::printf("\n%d of %d checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  std::printf("\nALL %d securacv_auth CHECKS PASSED\n", g_checks);
  return 0;
}
