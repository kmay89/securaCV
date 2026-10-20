/* test_securacv_auth.cpp — the flagship's bearer check and its lockout, on
 * the REAL canary/lib/securacv_auth/src/securacv_auth.cpp (compiled whole,
 * over stubs/securacv_auth: a movable 32-bit millis(), a header-map httpd
 * request, a token derive that needs no mbedtls) and the REAL
 * firmware/common/network/auth_backoff.h it runs on.
 *
 * Sweep F340, part 1. AuthManager::check() counted a request with no
 * Authorization header as a strike. The flagship's dashboard withholds the
 * token on a home-network load (no BOOT tap, no recovery-kit token), so its
 * api() sends no header while it polls /api/status every 2 s through
 * auth_gate, and the lockout was one per device: the owner's own open tab
 * held every bearer caller, the iPhone app's right token included, at 429. A
 * request with no credential carries no guess; it is now a 401 and no
 * strike, the WAP's rule (canary-wap's auth_logic.h).
 *
 * Sweep F340, part 2. The lockout is auth_backoff.h's table, the one the WAP
 * runs: one backoff per source address (8 tracked plus a shared overflow),
 * 2 s doubling, never more than 60 s, wrap-safe, with a saturating count.
 * The source is passed in (securacv_network.cpp's request_peer_ipv4(), the
 * socket peer), so one host's wrong bearers hold only that host. The
 * provisioning receipt's bearer used to be compared outside the lockout
 * (auth_check_optional: no strike, no backoff read); it is now the accounted
 * grant, auth_bearer_grant(): NONE, OK, WRONG or HELD, and a held bearer is
 * never compared. The text pins at the end hold securacv_network.cpp
 * (CANARY_NETWORK_CPP, passed in by the Makefile) to that wiring.
 *
 * Each case drives auth_check() the way auth_gate() does, and
 * auth_bearer_grant() the way bearer_present_and_valid() does, with the
 * token auth_load_or_derive() made, and reads what the library sent.
 *
 * Build/run: make -C firmware/tests_host. */

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "securacv_auth.h"

#if !defined(CANARY_NETWORK_CPP)
#error "build with -DCANARY_NETWORK_CPP (the path of securacv_network.cpp, for the text pins)"
#endif

using BearerResult = canary::net::auth_backoff::BearerResult;

static int g_checks = 0;
static int g_fail = 0;
#define CHECK(cond, msg) do { \
  ++g_checks; \
  if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); g_fail++; } \
  else { std::printf("ok   %s\n", msg); } } while (0)

// Two hosts on the home network (192.168.1.20 and .21), host order, as
// request_peer_ipv4() hands them in.
static constexpr uint32_t kHostA = 0xC0A80114;
static constexpr uint32_t kHostB = 0xC0A80115;

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

// `source`'s backoff as a header-less call reads it: the 429's Retry-After
// in seconds, 0 when the call is the 401. Changes nothing either way (a
// header-less call is no strike, and nothing is recorded while one runs).
static int held_for(uint32_t source) {
  httpd_req_t r = no_header();
  auth_check(&r, token(), source);
  if (!is_429(r)) return 0;
  return std::atoi(r.resp_headers["Retry-After"].c_str());
}

/* ── The owner's tab and the owner's app ──────────────────────────────── */

/* Ten minutes of a token-less dashboard tab (a header-less /api/status every
 * 2 s: 300 calls) beside the app's right bearer every 5 s (120 calls), from
 * one address (one Mac running both) and from two. Every app call is
 * served, and every tab call is the 401 with its WWW-Authenticate challenge,
 * never a 429. Before F340 the tab's calls armed the one lockout and held
 * the app's: 119 of the 120 were refused. */
static void header_less_requests_are_no_strike(uint32_t tab_source, uint32_t app_source,
                                               const char* how) {
  std::printf("-- the token-less tab and the app, %s\n", how);
  fresh(1000);
  int tab = 0, tab_401 = 0, tab_429 = 0, tab_sends_ok = 0;
  int app = 0, app_ok = 0, app_429 = 0, app_silent = 0;
  for (uint32_t s = 0; s < 600; s++) {
    g_host_millis = 1000 + s * 1000;
    if (s % 2 == 0) {
      httpd_req_t r = no_header();
      const bool ok = auth_check(&r, token(), tab_source);
      tab++;
      if (!ok && r.status == "401 Unauthorized" &&
          r.resp_headers["WWW-Authenticate"] == "Bearer realm=\"securacv\"") tab_401++;
      if (is_429(r)) tab_429++;
      if (r.sends == 1) tab_sends_ok++;
    }
    if (s % 5 == 1) {
      httpd_req_t r = right_bearer();
      const bool ok = auth_check(&r, token(), app_source);
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
  std::printf("-- a header-less call never extends a backoff\n");
  fresh(50000);
  httpd_req_t w = wrong_bearer();
  CHECK(!auth_check(&w, token(), kHostA) && w.status == "403 Forbidden",
        "a wrong bearer is refused 403");
  g_host_millis = 50000 + 2000;
  httpd_req_t n = no_header();
  CHECK(!auth_check(&n, token(), kHostA) && n.status == "401 Unauthorized",
        "a header-less call after the 2 s step is the 401");
  g_host_millis = 50000 + 2001;
  httpd_req_t r = right_bearer();
  CHECK(auth_check(&r, token(), kHostA) && r.sends == 0,
        "the right bearer right after it is served: the header-less call struck nothing");
}

/* ── What is still a strike ───────────────────────────────────────────── */

/* Each credential a guesser can send is one strike against its address: the
 * next right bearer from that address, inside the 2 s first step, is the
 * lockout's 429 with Retry-After; at the step's end it is served and clears
 * the count. Another address's right bearer is served throughout. */
static void credential_shaped_failures_still_strike() {
  std::printf("-- every credential-shaped failure is one strike, against its own address\n");
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
    const bool ok = auth_check(&c.req, token(), kHostA);
    std::snprintf(msg, sizeof(msg), "%s is refused %s", c.name, c.status);
    CHECK(!ok && c.req.status == c.status, msg);

    g_host_millis = 100000 + 1999;
    httpd_req_t held = right_bearer();
    const bool held_ok = auth_check(&held, token(), kHostA);
    std::snprintf(msg, sizeof(msg), "%s is a strike: the right bearer 1999 ms later is 429", c.name);
    CHECK(!held_ok && is_429(held), msg);
    std::snprintf(msg, sizeof(msg), "%s: the 429 says Retry-After 1 and too_many_requests", c.name);
    CHECK(held.resp_headers["Retry-After"] == "1" &&
          held.body == "{\"error\":\"too_many_requests\",\"retry_after_s\":1}", msg);

    httpd_req_t other = right_bearer();
    std::snprintf(msg, sizeof(msg), "%s from A: B's right bearer is served inside A's backoff", c.name);
    CHECK(auth_check(&other, token(), kHostB), msg);

    g_host_millis = 100000 + 2000;
    httpd_req_t served = right_bearer();
    std::snprintf(msg, sizeof(msg), "%s: at the step's end the right bearer is served", c.name);
    CHECK(auth_check(&served, token(), kHostA), msg);

    httpd_req_t again = wrong_bearer();
    auth_check(&again, token(), kHostA);
    g_host_millis = 100000 + 2000 + 1999;
    httpd_req_t step = right_bearer();
    std::snprintf(msg, sizeof(msg), "%s: the success cleared the count (the next strike is 2 s again)", c.name);
    CHECK(!auth_check(&step, token(), kHostA) && step.resp_headers["Retry-After"] == "1", msg);
  }
}

/* While a backoff runs nothing from that address is compared and nothing
 * more is recorded: a header-less call from it is the 429 too (the lockout
 * is read first), and the backoff still ends where the strike put it. A
 * header-less call from another address is the plain 401. */
static void a_running_backoff_answers_429_first() {
  std::printf("-- a running backoff answers 429 first, to its own address only\n");
  fresh(200000);
  httpd_req_t w = wrong_bearer();
  auth_check(&w, token(), kHostA);
  g_host_millis = 200000 + 1000;
  httpd_req_t n = no_header();
  CHECK(!auth_check(&n, token(), kHostA) && is_429(n) && n.resp_headers["Retry-After"] == "2",
        "a header-less call inside the backoff is the 429, Retry-After 2");
  httpd_req_t nb = no_header();
  CHECK(!auth_check(&nb, token(), kHostB) && nb.status == "401 Unauthorized",
        "a header-less call from another address is the 401, never A's 429");
  httpd_req_t again = wrong_bearer();
  CHECK(!auth_check(&again, token(), kHostA) && is_429(again) && again.value_reads == 0,
        "a wrong bearer inside the backoff is the 429 and is never read");
  g_host_millis = 200000 + 2000;
  httpd_req_t r = right_bearer();
  CHECK(auth_check(&r, token(), kHostA), "the backoff ends where the strike put it");
}

/* ── Per address ──────────────────────────────────────────────────────── */

/* A strikes five times, each wrong bearer sent once its previous backoff
 * ran out (2, 4, 8, 16, 32 s). Right after the fifth, A is held for 32 s
 * and B's right bearer is served. Then the review's replay: for ten minutes
 * A sends a wrong bearer every second, and B polls with the right one every
 * 5 s. Every one of B's 120 polls is served, and no 429 A meets says more
 * than 61. Before F340 the one lockout held B whenever A had struck. */
static void one_sources_strikes_never_hold_another() {
  std::printf("-- one address's strikes never hold another\n");
  fresh(300000);
  uint32_t t = 300000;
  int strikes = 0;
  for (int i = 0; i < 5; i++) {
    httpd_req_t w = wrong_bearer();
    if (!auth_check(&w, token(), kHostA) && w.status == "403 Forbidden") strikes++;
    t += 2000u << i;  // the backoff that strike armed
    if (i < 4) g_host_millis = t;
  }
  CHECK(strikes == 5, "A's five wrong bearers, each after its backoff ended, are five 403s");
  httpd_req_t a = right_bearer();
  CHECK(!auth_check(&a, token(), kHostA) && is_429(a) && a.resp_headers["Retry-After"] == "33",
        "right after the fifth, A's right bearer is 429 with Retry-After 33 (a 32 s backoff)");
  httpd_req_t b = right_bearer();
  CHECK(auth_check(&b, token(), kHostB), "B's right bearer is served at once");
  CHECK(auth_bearer_grant(&b, token(), kHostB) == BearerResult::OK,
        "B's right bearer on the receipt's grant is OK at once");

  fresh(1000);
  int b_polls = 0, b_served = 0, a_wrong = 0, a_429 = 0, a_max_retry = 0;
  for (uint32_t s = 0; s < 600; s++) {
    g_host_millis = 1000 + s * 1000;
    httpd_req_t w = wrong_bearer();
    auth_check(&w, token(), kHostA);
    if (w.status == "403 Forbidden") a_wrong++;
    if (is_429(w)) {
      a_429++;
      const int ra = std::atoi(w.resp_headers["Retry-After"].c_str());
      if (ra > a_max_retry) a_max_retry = ra;
    }
    if (s % 5 == 2) {
      httpd_req_t r = right_bearer();
      b_polls++;
      if (auth_check(&r, token(), kHostB)) b_served++;
    }
  }
  std::printf("     replay: A sent 600 wrong bearers (%d of them strikes, %d held at 429, "
              "Retry-After at most %d); B polled %d times, %d served\n",
              a_wrong, a_429, a_max_retry, b_polls, b_served);
  CHECK(b_polls == 120 && b_served == 120, "every one of B's 120 right-bearer polls is served beside A");
  CHECK(a_wrong > 0 && a_429 > 0, "A struck and was held");
  CHECK(a_max_retry >= 1 && a_max_retry <= 61, "no 429 A met said more than 61");
}

/* The 9th address to strike while 8 slots are taken goes to the shared
 * overflow, and while it runs it holds every address without a slot of its
 * own, the right bearer included; a tracked address is held only by its own
 * slot. */
static void the_overflow_holds_every_untracked_address() {
  std::printf("-- nine striking addresses: the shared overflow\n");
  fresh(400000);
  for (uint32_t i = 0; i < 8; i++) {
    httpd_req_t w = wrong_bearer();
    auth_check(&w, token(), 0x0A000001u + i);  // 10.0.0.1 .. 10.0.0.8
  }
  httpd_req_t x = wrong_bearer();
  CHECK(!auth_check(&x, token(), 0x0A000009u) && x.status == "403 Forbidden",
        "a 9th address's wrong bearer is a strike (on the overflow)");
  httpd_req_t y = right_bearer();
  CHECK(!auth_check(&y, token(), kHostB) && is_429(y),
        "an untracked address's right bearer is held by the overflow");
  CHECK(auth_bearer_grant(&y, token(), kHostB) == BearerResult::HELD,
        "and the receipt's grant reads it HELD");
  g_host_millis = 400000 + 2000;
  httpd_req_t y2 = right_bearer();
  CHECK(auth_check(&y2, token(), kHostB), "once the overflow's backoff ends, it is served");
  httpd_req_t one = right_bearer();
  CHECK(auth_check(&one, token(), 0x0A000001u), "a tracked address is free once its own backoff ends");
}

/* ── The receipt's grant ──────────────────────────────────────────────── */

/* auth_bearer_grant() is what bearer_present_and_valid() (the provisioning
 * receipt, the page-token decision) asks. Before F340 that compare was
 * auth_check_optional(): no strike, no backoff read, so a host could guess
 * there for free and a right bearer was compared inside any backoff. */
static void the_receipt_grant_counts() {
  std::printf("-- the receipt's grant is accounted\n");
  fresh(500000);
  httpd_req_t none = no_header();
  CHECK(auth_bearer_grant(&none, token(), kHostA) == BearerResult::NONE && held_for(kHostA) == 0,
        "no header is NONE and records nothing");
  CHECK(none.sends == 0, "the grant sends nothing");

  httpd_req_t w = wrong_bearer();
  CHECK(auth_bearer_grant(&w, token(), kHostA) == BearerResult::WRONG && w.sends == 0,
        "a wrong bearer is WRONG, and the grant sends nothing");
  g_host_millis = 500000 + 1000;
  CHECK(held_for(kHostA) == 2, "it is one strike: A is held, Retry-After 2 (a 2 s backoff)");

  httpd_req_t held = right_bearer();
  CHECK(auth_bearer_grant(&held, token(), kHostA) == BearerResult::HELD,
        "inside A's backoff, A's right bearer is HELD");
  CHECK(held.value_reads == 0, "and it was not compared (its value never read)");
  httpd_req_t held_wrong = wrong_bearer();
  CHECK(auth_bearer_grant(&held_wrong, token(), kHostA) == BearerResult::HELD &&
        held_wrong.value_reads == 0, "a wrong bearer inside the backoff is HELD, unread");
  g_host_millis = 500000 + 1999;
  CHECK(held_for(kHostA) == 1, "nothing more was recorded: the backoff still ends at 2 s");

  httpd_req_t b = right_bearer();
  CHECK(auth_bearer_grant(&b, token(), kHostB) == BearerResult::OK,
        "B's right bearer is OK inside A's backoff");

  g_host_millis = 500000 + 2000;
  httpd_req_t ok = right_bearer();
  CHECK(auth_bearer_grant(&ok, token(), kHostA) == BearerResult::OK, "after it, A's right bearer is OK");
  httpd_req_t w2 = wrong_bearer();
  auth_bearer_grant(&w2, token(), kHostA);
  g_host_millis = 500000 + 3000;
  CHECK(held_for(kHostA) == 2, "the OK cleared A's count: the next strike is a 2 s backoff again");

  struct Malformed { const char* name; httpd_req_t req; };
  Malformed bad[] = {
    {"a non-Bearer scheme", with_auth(std::string("Basic ") + token())},
    {"an oversized header", with_auth("Bearer " + std::string(SECURACV_AUTH_HEADER_BUF, 'a'))},
    {"a wrong-length bearer", with_auth("Bearer cv_short")},
  };
  for (Malformed& m : bad) {
    fresh(600000);
    char msg[160];
    std::snprintf(msg, sizeof(msg), "%s is WRONG on the grant, one strike", m.name);
    const BearerResult r = auth_bearer_grant(&m.req, token(), kHostA);
    g_host_millis = 600000 + 1000;
    CHECK(r == BearerResult::WRONG && held_for(kHostA) == 2, msg);
  }

  fresh(700000);
  int wrong = 0, held_n = 0;
  for (uint32_t i = 0; i < 1000; i++) {
    g_host_millis = 700000 + i * 10;
    httpd_req_t g = wrong_bearer();
    const BearerResult r = auth_bearer_grant(&g, token(), kHostA);
    if (r == BearerResult::WRONG) wrong++;
    if (r == BearerResult::HELD) held_n++;
  }
  httpd_req_t after = right_bearer();
  std::printf("     1000 wrong grants over 10 s: %d strikes, %d held unread\n", wrong, held_n);
  CHECK(wrong == 3 && held_n == 997,
        "1000 wrong grants over 10 s are 3 strikes (at 0, 2 and 6 s) and 997 HELD");
  CHECK(!auth_check(&after, token(), kHostA) && is_429(after),
        "and A's right bearer after them is 429: the receipt's guesses counted");

  httpd_req_t* null_req = nullptr;
  CHECK(auth_bearer_grant(null_req, token(), kHostA) == BearerResult::NONE,
        "a null request is NONE");
}

/* The device's expected token is never empty on a provisioned unit, and
 * every caller refuses an empty one first (auth_gate's 503,
 * bearer_present_and_valid's guard). The library fails closed on its own:
 * an empty expected token matches no bearer, a bare "Bearer " included
 * (a zero-length constant-time compare is equal). */
static void an_empty_expected_token_matches_nothing() {
  std::printf("-- an empty expected token matches nothing\n");
  fresh(800000);
  httpd_req_t bare = with_auth("Bearer ");
  CHECK(!auth_check(&bare, "", kHostA) && bare.status == "403 Forbidden",
        "auth_check: a bare 'Bearer ' against an empty token is refused 403");
  httpd_req_t g = with_auth("Bearer ");
  CHECK(auth_bearer_grant(&g, "", kHostB) == BearerResult::WRONG,
        "auth_bearer_grant: a bare 'Bearer ' against an empty token is WRONG");
}

/* ── The clock ────────────────────────────────────────────────────────── */

/* millis() wraps every 49.7 days. A backoff armed 1024 ms before the wrap
 * ends 976 ms after it, and holds all the way there; the old `now < until`
 * read it as over at once, so the guess after the strike was compared. And
 * with strikes sent as each backoff ends, Retry-After walks 3, 5, 9, 17, 33
 * and then stays at 61, never more, however many follow (the count
 * saturates and never wraps back to a free slot). The old 5-minute cap
 * announced 65 on the sixth. */
static void the_lockout_survives_the_wrap_and_never_wraps() {
  std::printf("-- the wrap, and the 61 s ceiling\n");
  const uint32_t t0 = 0xFFFFFFFFu - 1023u;  // 1024 ms before the wrap
  fresh(t0);
  httpd_req_t w = wrong_bearer();
  auth_check(&w, token(), kHostA);
  g_host_millis = t0 + 500;
  CHECK(held_for(kHostA) == 2, "500 ms after the strike, still before the wrap, A is held");
  g_host_millis = t0 + 1524;
  CHECK(held_for(kHostA) == 1, "500 ms past the wrap, A is still held");
  g_host_millis = t0 + 2000;
  httpd_req_t r = right_bearer();
  CHECK(auth_check(&r, token(), kHostA), "at the backoff's end, across the wrap, A is served");

  fresh(900000);
  uint32_t t = 900000;
  int first[6] = {0};
  int min_ra = 1000, max_ra = 0;
  bool each_ends = true;
  for (int i = 0; i < 300; i++) {
    g_host_millis = t;
    httpd_req_t s = wrong_bearer();
    auth_check(&s, token(), kHostA);
    if (s.status != "403 Forbidden") { each_ends = false; break; }
    const int ra = held_for(kHostA);  // read at the strike: the whole backoff
    if (i < 6) first[i] = ra;
    if (ra < min_ra) min_ra = ra;
    if (ra > max_ra) max_ra = ra;
    if (ra < 1) { each_ends = false; break; }
    // Every backoff is whole seconds, so it is (ra - 1) s: its last
    // millisecond says Retry-After 1, and the next one is free. The next
    // strike lands there.
    const uint32_t backoff = (uint32_t)(ra - 1) * 1000u;
    g_host_millis = t + backoff - 1;
    if (held_for(kHostA) != 1) each_ends = false;
    g_host_millis = t + backoff;
    if (held_for(kHostA) != 0) each_ends = false;
    t += backoff;
  }
  std::printf("     Retry-After right after each strike: %d %d %d %d %d %d ... (min %d, max %d over 300)\n",
              first[0], first[1], first[2], first[3], first[4], first[5], min_ra, max_ra);
  CHECK(first[0] == 3 && first[1] == 5 && first[2] == 9 && first[3] == 17 && first[4] == 33,
        "the backoff doubles from 2 s: Retry-After 3, 5, 9, 17, 33");
  CHECK(first[5] == 61, "the sixth strike's Retry-After is 61 (a 60 s backoff), not the old 65");
  CHECK(min_ra >= 1 && max_ra == 61, "over 300 strikes every Retry-After is between 1 and 61");
  CHECK(each_ends, "every backoff's last millisecond says Retry-After 1, and the next is free");
}

/* ── The hand-over (the API sweep F341 calls) ─────────────────────────── */

/* auth_handed_token(source) is for a path that hands an address the token
 * without a bearer (the BOOT tap). It ends that address's backoff as a
 * right bearer would, and frees nothing else: not another address's slot,
 * and not the shared overflow, which an attacker's next strike would re-arm
 * at once. */
static void handed_token_frees_only_its_source() {
  std::printf("-- auth_handed_token frees its own address only\n");
  fresh(1000000);
  httpd_req_t wa = wrong_bearer();
  auth_check(&wa, token(), kHostA);
  httpd_req_t wb = wrong_bearer();
  auth_check(&wb, token(), kHostB);
  auth_handed_token(kHostA);
  httpd_req_t a = right_bearer();
  CHECK(auth_check(&a, token(), kHostA), "after auth_handed_token(A), A's right bearer is served at once");
  CHECK(held_for(kHostB) > 0, "B is still held: its slot was not touched");

  fresh(1100000);
  for (uint32_t i = 0; i < 8; i++) {
    httpd_req_t w = wrong_bearer();
    auth_check(&w, token(), 0x0A000001u + i);
  }
  httpd_req_t x = wrong_bearer();
  auth_check(&x, token(), 0x0A000009u);
  auth_handed_token(0x0A000009u);
  CHECK(held_for(0x0A000009u) > 0, "an address held only through the overflow stays held after it");
  CHECK(held_for(kHostB) > 0, "and so does every other untracked address");
}

/* ── securacv_network.cpp's wiring (text pins) ────────────────────────── */

static std::string read_file(const char* path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// `src` with every comment removed and string and character literals kept,
// so a brace or a "//" inside a literal is not taken for code.
static std::string strip_comments(const std::string& src) {
  std::string out;
  size_t i = 0;
  while (i < src.size()) {
    const char c = src[i];
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
      while (i < src.size() && src[i] != '\n') i++;
    } else if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
      const size_t end = src.find("*/", i + 2);
      i = end == std::string::npos ? src.size() : end + 2;
      out += ' ';
    } else if (c == '"' || c == '\'') {
      out += c;
      i++;
      while (i < src.size() && src[i] != c) {
        if (src[i] == '\\' && i + 1 < src.size()) out += src[i++];
        out += src[i++];
      }
      if (i < src.size()) out += src[i++];
    } else {
      out += src[i++];
    }
  }
  return out;
}

// The braced body of the definition that starts with `signature` (comments
// already stripped), braces included; "" when it is not there exactly once.
static std::string body_of(const std::string& code, const std::string& signature) {
  const size_t at = code.find(signature);
  if (at == std::string::npos || code.find(signature, at + 1) != std::string::npos) return "";
  const size_t open = code.find('{', at + signature.size() - 1);
  if (open == std::string::npos) return "";
  int depth = 0;
  for (size_t i = open; i < code.size(); i++) {
    const char c = code[i];
    if (c == '"' || c == '\'') {
      for (i++; i < code.size() && code[i] != c; i++) {
        if (code[i] == '\\') i++;
      }
      continue;
    }
    if (c == '{') depth++;
    if (c == '}' && --depth == 0) return code.substr(open, i - open + 1);
  }
  return "";
}

static std::string squash(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') out += c;
  }
  return out;
}

// Occurrences of `needle` in `hay`. With `whole_name`, only where it starts
// a name (`auth_check(` is not the tail of `xauth_check(`): read on the
// unsquashed code, where a space still separates `return` from the call.
static int count_of(const std::string& hay, const std::string& needle, bool whole_name) {
  int n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) {
    if (whole_name && at > 0 && (std::isalnum((unsigned char)hay[at - 1]) || hay[at - 1] == '_')) {
      continue;
    }
    n++;
  }
  return n;
}

/* The request's source comes from the socket and reaches both gates. Each
 * pin is a fact of the file after F340 that the file before it lacks:
 * request_peer_ipv4() is getpeername through sockaddr_ip4_host_order (the
 * WAP's key mapping), 0 when the socket cannot say; auth_gate hands it to
 * auth_check; bearer_present_and_valid keeps its empty-token guard and is
 * the accounted grant over it; no call of either gate omits it; the old
 * silent compare is gone; and the receipt answers a held bearer as none
 * (its 403, the page's "tap BOOT"), never with a 429 of its own. */
static void the_network_layer_passes_the_source() {
  std::printf("-- securacv_network.cpp: the source reaches both gates\n");
  const std::string raw = read_file(CANARY_NETWORK_CPP);
  CHECK(raw.size() > 10000, "securacv_network.cpp read");
  const std::string code = strip_comments(raw);

  const std::string peer = squash(body_of(code, "static uint32_t request_peer_ipv4(httpd_req_t* req) {"));
  CHECK(peer ==
        "{constintfd=httpd_req_to_sockfd(req);if(fd<0)return0;"
        "structsockaddr_storagepeer_ss;socklen_tplen=sizeof(peer_ss);"
        "memset(&peer_ss,0,sizeof(peer_ss));"
        "if(getpeername(fd,(structsockaddr*)&peer_ss,&plen)!=0)return0;"
        "returnsockaddr_ip4_host_order(peer_ss);}",
        "request_peer_ipv4() is getpeername through sockaddr_ip4_host_order, 0 when the socket cannot say");

  const std::string bearer = squash(body_of(code, "static bool bearer_present_and_valid(httpd_req_t* req) {"));
  CHECK(bearer ==
        "{constchar*token=auth_get_token();if(!token||token[0]=='\\0')returnfalse;"
        "returnauth_bearer_grant(req,token,request_peer_ipv4(req))=="
        "AuthManager::BearerResult::OK;}",
        "bearer_present_and_valid() keeps its empty-token guard and is the accounted grant, OK only");

  const std::string gate = squash(body_of(code, "static bool auth_gate(httpd_req_t* req) {"));
  CHECK(gate.find("if(!auth_check(req,token,request_peer_ipv4(req))){") != std::string::npos,
        "auth_gate() hands auth_check() the request's source");

  const std::string flat = squash(code);
  CHECK(count_of(code, "auth_check(", true) == 1 &&
        count_of(flat, "auth_check(req,token,request_peer_ipv4(req))", false) == 1,
        "auth_check() is called once, with request_peer_ipv4(req)");
  CHECK(count_of(code, "auth_bearer_grant(", true) == 1 &&
        count_of(flat, "auth_bearer_grant(req,token,request_peer_ipv4(req))", false) == 1,
        "auth_bearer_grant() is called once, with request_peer_ipv4(req)");
  CHECK(raw.find("auth_check_optional") == std::string::npos &&
        raw.find("checkOptional") == std::string::npos,
        "the silent compare (auth_check_optional, checkOptional) is gone from the file");

  const std::string receipt =
      squash(body_of(code, "static esp_err_t handle_provisioning_receipt(httpd_req_t* req) {"));
  CHECK(receipt.find("[req](){returnbearer_present_and_valid(req);}") != std::string::npos,
        "the receipt's bearer grant is bearer_present_and_valid()");
  CHECK(!receipt.empty() && receipt.find("429") == std::string::npos &&
        receipt.find("HELD") == std::string::npos &&
        receipt.find("Retry-After") == std::string::npos &&
        receipt.find("auth_bearer_grant(") == std::string::npos,
        "the receipt has no 429 branch: a held bearer reads as none, so the tap or the 403");
}

int main() {
  const uint8_t key[32] = {1};
  CHECK(auth_load_or_derive(key) && std::strlen(token()) == 35,
        "auth_load_or_derive() makes a token of the real shape");

  header_less_requests_are_no_strike(kHostA, kHostA, "from one address");
  header_less_requests_are_no_strike(kHostA, kHostB, "from two addresses");
  a_header_less_call_never_extends_a_backoff();
  credential_shaped_failures_still_strike();
  a_running_backoff_answers_429_first();
  one_sources_strikes_never_hold_another();
  the_overflow_holds_every_untracked_address();
  the_receipt_grant_counts();
  an_empty_expected_token_matches_nothing();
  the_lockout_survives_the_wrap_and_never_wraps();
  handed_token_frees_only_its_source();
  the_network_layer_passes_the_source();

  if (g_fail) {
    std::printf("\n%d of %d checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  std::printf("\nALL %d securacv_auth CHECKS PASSED\n", g_checks);
  return 0;
}
