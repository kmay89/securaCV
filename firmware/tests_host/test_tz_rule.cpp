// Host test for firmware/common/time/tz_rule.h (repo sweep F28) — the fleet's
// IANA -> POSIX table, the rule validator, the local minute-of-day the CSI
// chokepoint's clock offset now keys on, and the two small input helpers the
// settings handlers use.
//
// glibc honors the same POSIX TZ rules newlib does (setenv + tzset +
// localtime_r), so the DST vectors below are the real transitions the device
// will compute, not a model of them.
//
// Run: make -C firmware/tests_host   (PORTAL_SRC is passed by the Makefile)

#include "time/tz_rule.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef PORTAL_SRC
#define PORTAL_SRC "../projects/canary-display/src/net/provision.cpp"
#endif

static int g_fail = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      g_fail++;                                                              \
    }                                                                        \
  } while (0)

// PASS only when the test added no failure (a CHECK prints and counts).
static void report(const char* name, int fails_before) {
  std::printf("%s %s\n", g_fail == fails_before ? "PASS" : "FAIL", name);
}

static void use_tz(const char* rule) {
  if (rule) setenv("TZ", rule, 1);
  else unsetenv("TZ");
  tzset();
}

// 00:00:00 UTC on y-m-d, plus h:m.
static time_t utc(int y, int mo, int d, int h, int mi) {
  struct tm t;
  std::memset(&t, 0, sizeof(t));
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  return timegm(&t);
}

static int minute(int h, int m) { return h * 60 + m; }

static void test_table_shape() {
  const int f0 = g_fail;
  size_t n = 0;
  const tz_rule::ZoneRule* z = tz_rule::zones(&n);
  CHECK(n >= 43);                                   // the display's table, at least
  std::set<std::string> seen;
  for (size_t i = 0; i < n; ++i) {
    CHECK(z[i].iana != nullptr && z[i].posix != nullptr);
    CHECK(std::strlen(z[i].iana) <= tz_rule::MAX_IANA_LEN);
    CHECK(tz_rule::posix_plausible(z[i].posix));
    CHECK(seen.insert(z[i].iana).second);            // no duplicate IANA names
    CHECK(tz_rule::posix_for_iana(z[i].iana) == z[i].posix);
  }
  std::printf("  (%zu zones)\n", n);
  report("table_shape", f0);
}

static void test_lookup() {
  const int f0 = g_fail;
  CHECK(std::strcmp(tz_rule::posix_for_iana("America/New_York"), "EST5EDT,M3.2.0,M11.1.0") == 0);
  CHECK(std::strcmp(tz_rule::posix_for_iana("Asia/Kolkata"), "IST-5:30") == 0);
  CHECK(std::strcmp(tz_rule::posix_for_iana("UTC"), "UTC0") == 0);
  CHECK(tz_rule::posix_for_iana("Mars/Olympus_Mons") == nullptr);
  CHECK(tz_rule::posix_for_iana("america/new_york") == nullptr);   // exact names only
  CHECK(tz_rule::posix_for_iana("") == nullptr);
  CHECK(tz_rule::posix_for_iana(nullptr) == nullptr);
  report("lookup", f0);
}

static void test_plausible() {
  const int f0 = g_fail;
  CHECK(tz_rule::posix_plausible("UTC0"));
  CHECK(tz_rule::posix_plausible("EST5EDT,M3.2.0,M11.1.0"));
  CHECK(tz_rule::posix_plausible("<-03>3"));
  CHECK(tz_rule::posix_plausible("IST-5:30"));
  CHECK(!tz_rule::posix_plausible(nullptr));
  CHECK(!tz_rule::posix_plausible(""));
  CHECK(!tz_rule::posix_plausible("5EST"));                // must start with a name
  CHECK(!tz_rule::posix_plausible("EST 5"));               // no spaces
  CHECK(!tz_rule::posix_plausible("EST5\"EDT"));           // no quotes (JSON-breaking)
  CHECK(!tz_rule::posix_plausible("EST5\\EDT"));
  CHECK(!tz_rule::posix_plausible("EST5\nEDT"));           // no control bytes
  CHECK(!tz_rule::posix_plausible("EST5\x7f"));
  std::string at_max(tz_rule::MAX_POSIX_LEN, 'A');
  CHECK(tz_rule::posix_plausible(at_max.c_str()));
  std::string over(tz_rule::MAX_POSIX_LEN + 1, 'A');
  CHECK(!tz_rule::posix_plausible(over.c_str()));          // the display's 48-byte ceiling
  report("plausible", f0);
}

// Every table rule actually parses: a rule glibc could not read would fall
// back to UTC, so each non-zero-offset zone must differ from UTC in January
// or July.
static void test_every_rule_parses() {
  const int f0 = g_fail;
  size_t n = 0;
  const tz_rule::ZoneRule* z = tz_rule::zones(&n);
  const time_t jan = utc(2026, 1, 15, 12, 0), jul = utc(2026, 7, 15, 12, 0);
  for (size_t i = 0; i < n; ++i) {
    const bool utc_like = std::strncmp(z[i].posix, "UTC0", 4) == 0;
    const bool zero_winter = std::strncmp(z[i].posix, "GMT0", 4) == 0 ||
                             std::strncmp(z[i].posix, "WET0", 4) == 0;
    use_tz(z[i].posix);
    const int a = tz_rule::local_minute_of_day(jan);
    const int b = tz_rule::local_minute_of_day(jul);
    if (utc_like) {
      CHECK(a == minute(12, 0) && b == minute(12, 0));
    } else if (zero_winter) {
      CHECK(a == minute(12, 0));
      CHECK(b == minute(13, 0));                             // summer time parsed
    } else if (a == minute(12, 0) && b == minute(12, 0)) {
      std::fprintf(stderr, "FAIL: %s (%s) reads as UTC — unparsed rule?\n",
                   z[i].iana, z[i].posix);
      g_fail++;
    }
  }
  use_tz(nullptr);
  report("every_rule_parses", f0);
}

static void test_dst_vectors() {
  const int f0 = g_fail;
  // US Eastern: July is UTC-4, January UTC-5.
  use_tz(tz_rule::posix_for_iana("America/New_York"));
  CHECK(tz_rule::local_minute_of_day(utc(2026, 7, 1, 12, 0)) == minute(8, 0));
  CHECK(tz_rule::local_minute_of_day(utc(2026, 1, 15, 12, 0)) == minute(7, 0));
  // The spring-forward edge: 2026-03-08, 02:00 EST = 07:00 UTC.
  CHECK(tz_rule::local_minute_of_day(utc(2026, 3, 8, 6, 59)) == minute(1, 59));
  CHECK(tz_rule::local_minute_of_day(utc(2026, 3, 8, 7, 0)) == minute(3, 0));
  // Fall back: 2026-11-01, 02:00 EDT = 06:00 UTC -> 01:00 EST.
  CHECK(tz_rule::local_minute_of_day(utc(2026, 11, 1, 5, 59)) == minute(1, 59));
  CHECK(tz_rule::local_minute_of_day(utc(2026, 11, 1, 6, 0)) == minute(1, 0));

  // London: BST in July, GMT in January.
  use_tz(tz_rule::posix_for_iana("Europe/London"));
  CHECK(tz_rule::local_minute_of_day(utc(2026, 7, 1, 12, 0)) == minute(13, 0));
  CHECK(tz_rule::local_minute_of_day(utc(2026, 1, 15, 12, 0)) == minute(12, 0));

  // Sydney: southern-hemisphere DST — daylight time in January.
  use_tz(tz_rule::posix_for_iana("Australia/Sydney"));
  CHECK(tz_rule::local_minute_of_day(utc(2026, 1, 15, 12, 0)) == minute(23, 0));
  CHECK(tz_rule::local_minute_of_day(utc(2026, 7, 15, 12, 0)) == minute(22, 0));

  // Half-hour offset, and an angle-bracket name.
  use_tz(tz_rule::posix_for_iana("Asia/Kolkata"));
  CHECK(tz_rule::local_minute_of_day(utc(2026, 5, 1, 12, 0)) == minute(17, 30));
  use_tz(tz_rule::posix_for_iana("America/Sao_Paulo"));
  CHECK(tz_rule::local_minute_of_day(utc(2026, 5, 1, 2, 0)) == minute(23, 0));
  use_tz(nullptr);
  report("dst_vectors", f0);
}

// With the zone at UTC the new helper is exactly the pre-F28 arithmetic, so
// an unset zone (newlib: UTC) changes no bucket and no quiet window.
static void test_utc_matches_old_formula() {
  const int f0 = g_fail;
  use_tz("UTC0");
  for (int64_t t = 1700000000; t < 1700000000 + 3 * 86400; t += 571) {
    const int32_t old_min = (int32_t)(((time_t)t % 86400) / 60);
    CHECK(tz_rule::local_minute_of_day((time_t)t) == old_min);
  }
  use_tz(nullptr);
  report("utc_matches_old_formula", f0);
}

// The display portal's phone-guess table (provision.cpp TZS) and this table
// must put the same IANA zone on the same wall clock — a phone seeding the
// WAP or the canary must land where it would on the display.
static void test_portal_parity() {
  const int f0 = g_fail;
  std::ifstream f(PORTAL_SRC);
  if (!f) {
    std::fprintf(stderr, "FAIL: cannot open %s\n", PORTAL_SRC);
    g_fail++;
    return;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string src = ss.str();
  const size_t a = src.find("var TZS=[");
  const size_t b = a == std::string::npos ? a : src.find("];", a);
  CHECK(a != std::string::npos && b != std::string::npos);
  if (a == std::string::npos || b == std::string::npos) return;
  const std::string block = src.substr(a, b - a);
  const std::regex row("\\[\"([^\"]*)\",\"([^\"]*)\",\"([^\"]*)\"\\]");
  size_t rows = 0, names = 0;
  for (std::sregex_iterator it(block.begin(), block.end(), row), end; it != end; ++it) {
    rows++;
    const std::string portal_rule = (*it)[2];
    std::stringstream list((*it)[3]);
    std::string iana;
    while (std::getline(list, iana, ',')) {
      names++;
      const char* ours = tz_rule::posix_for_iana(iana.c_str());
      if (ours == nullptr) {
        std::fprintf(stderr, "FAIL: portal zone %s is not in tz_rule.h\n", iana.c_str());
        g_fail++;
        continue;
      }
      // Same wall clock, sampled weekly (and off the hour) across two years.
      for (time_t t = utc(2026, 1, 1, 3, 17); t < utc(2028, 1, 1, 0, 0); t += 7 * 86400 + 3 * 3600) {
        use_tz(ours);
        struct tm x;
        localtime_r(&t, &x);
        use_tz(portal_rule.c_str());
        struct tm y;
        localtime_r(&t, &y);
        if (x.tm_hour != y.tm_hour || x.tm_min != y.tm_min || x.tm_mday != y.tm_mday) {
          std::fprintf(stderr, "FAIL: %s: %s vs portal %s disagree at %lld\n",
                       iana.c_str(), ours, portal_rule.c_str(), (long long)t);
          g_fail++;
          break;
        }
      }
    }
  }
  use_tz(nullptr);
  CHECK(rows >= 20 && names >= 40);
  std::printf("  (%zu portal rows, %zu zone names)\n", rows, names);
  report("portal_parity", f0);
}

static void test_resolve() {
  const int f0 = g_fail;
  char out[tz_rule::MAX_POSIX_LEN + 1] = "untouched";
  CHECK(tz_rule::resolve(nullptr, nullptr, out) == tz_rule::Resolve::NONE);
  CHECK(tz_rule::resolve("", "", out) == tz_rule::Resolve::NONE);
  CHECK(std::strcmp(out, "untouched") == 0);
  CHECK(tz_rule::resolve(nullptr, "Europe/Berlin", out) == tz_rule::Resolve::OK);
  CHECK(std::strcmp(out, "CET-1CEST,M3.5.0,M10.5.0/3") == 0);
  CHECK(tz_rule::resolve("JST-9", "Europe/Berlin", out) == tz_rule::Resolve::OK);
  CHECK(std::strcmp(out, "JST-9") == 0);                     // a typed rule wins
  std::strcpy(out, "kept");
  CHECK(tz_rule::resolve(nullptr, "Mars/Base", out) == tz_rule::Resolve::UNKNOWN_ZONE);
  CHECK(tz_rule::resolve("bad rule", nullptr, out) == tz_rule::Resolve::BAD_RULE);
  CHECK(std::strcmp(out, "kept") == 0);                      // written only on OK
  report("resolve", f0);
}

static void test_json_string_field() {
  const int f0 = g_fail;
  char v[48];
  CHECK(tz_rule::json_string_field("{\"tz\":\"EST5EDT,M3.2.0,M11.1.0\"}", "\"tz\"", v, sizeof(v)));
  CHECK(std::strcmp(v, "EST5EDT,M3.2.0,M11.1.0") == 0);
  CHECK(tz_rule::json_string_field("{ \"tz_iana\" : \"Asia/Tokyo\" , \"x\":1}", "\"tz_iana\"", v, sizeof(v)));
  CHECK(std::strcmp(v, "Asia/Tokyo") == 0);
  // "tz" must not match inside "tz_iana".
  CHECK(!tz_rule::json_string_field("{\"tz_iana\":\"Asia/Tokyo\"}", "\"tz\"", v, sizeof(v)));
  CHECK(tz_rule::json_string_field("{\"tz\":\"\"}", "\"tz\"", v, sizeof(v)));
  CHECK(v[0] == '\0');
  CHECK(!tz_rule::json_string_field("{\"tz\":5}", "\"tz\"", v, sizeof(v)));          // not a string
  CHECK(!tz_rule::json_string_field("{\"tz\":\"a\\\"b\"}", "\"tz\"", v, sizeof(v)));  // an escape
  CHECK(!tz_rule::json_string_field("{\"tz\":\"unterminated", "\"tz\"", v, sizeof(v)));
  CHECK(!tz_rule::json_string_field("{\"tz\":\"abcdef\"}", "\"tz\"", v, 6));          // no room
  CHECK(tz_rule::json_string_field("{\"tz\":\"abcde\"}", "\"tz\"", v, 6));
  CHECK(!tz_rule::json_string_field("{}", "\"tz\"", v, sizeof(v)));
  CHECK(!tz_rule::json_string_field(nullptr, "\"tz\"", v, sizeof(v)));
  report("json_string_field", f0);
}

int main() {
  test_table_shape();
  test_lookup();
  test_plausible();
  test_every_rule_parses();
  test_dst_vectors();
  test_utc_matches_old_formula();
  test_portal_parity();
  test_resolve();
  test_json_string_field();
  if (g_fail) {
    std::fprintf(stderr, "\n%d TZ_RULE CHECK(S) FAILED\n", g_fail);
    return 1;
  }
  std::printf("\nALL TZ_RULE TESTS PASSED\n");
  return 0;
}
