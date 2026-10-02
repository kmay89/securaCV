// The body of the canary-wap's GET /api/diagnostics (sweep F149), through
// the REAL wap_diagnostics.h the handler builds it with, and its buffer.
//
// Before, the handler spelled the body in place with one snprintf into a
// buffer sized by a hand count, and nothing built it off the device: the
// egress counters' key could go, the object could be quoted into a string
// (invalid JSON), or the buffer could shrink back to 512 (a body of about
// 541 bytes once a few hundred events were counted, answered as "buffer
// overflow"), and every gate stayed green. check_wap_event_egress.py rule 12
// holds the handler to this builder and its kJsonMax buffer; this suite holds
// the builder: every key, the counters as an object (`null` before the first
// pump), and the widest body inside kJsonMax. It also reads sys_monitor.h
// (its path passed in, so the read fails closed) for the longest name
// degrade_level_name() returns.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "wap_diagnostics.h"

#ifndef SYS_MONITOR_H
#error "build with -DSYS_MONITOR_H=\"<path to sys_monitor.h>\""
#endif

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

wap_diagnostics::Inputs typical() {
  wap_diagnostics::Inputs in = {};
  in.heap_free = 180000;
  in.heap_min_free = 150000;
  in.heap_largest_block = 110000;
  in.heap_total = 320000;
  in.degrade_level = 0;
  in.degrade_name = "NONE";
  in.sd_total_writes = 4100;
  in.sd_write_errors = 2;
  in.sd_usage_pct = 37;
  in.sd_space_warning = false;
  in.sd_space_critical = false;
  in.uptime_sec = 86400;
  return in;
}

csi_event_egress::Stats counted() {
  csi_event_egress::Stats st = {};
  st.dropped = 1; st.held_dropped = 2; st.ambient_dropped = 3; st.unsent_dropped = 4;
  st.planner.live = 5; st.planner.held = 6; st.planner.queued = 7; st.planner.replayed = 8;
  st.planner.skipped = 9; st.planner.untrusted = 10; st.planner.unsendable = 11;
  st.planner.truncated_unsent = 12; st.planner.read_giveups = 13;
  return st;
}

std::string build(const wap_diagnostics::Inputs& in, const csi_event_egress::Stats* egress,
                  size_t cap = wap_diagnostics::kJsonMax) {
  std::string out(cap, '\x7f');
  const size_t n = wap_diagnostics::build_json(in, egress, &out[0], cap);
  if (n == 0) return out[0] == '\0' ? std::string() : std::string("<partial body left>");
  CHECK(out[n] == '\0' && std::strlen(out.c_str()) == n);
  return out.substr(0, n);
}

void test_every_key_and_the_counters_as_an_object() {
  const csi_event_egress::Stats st = counted();
  CHECK(build(typical(), &st) ==
        "{\"ok\":true,"
        "\"heap\":{\"free\":180000,\"min_free\":150000,\"largest_block\":110000,\"total\":320000},"
        "\"degradation\":{\"level\":0,\"level_name\":\"NONE\"},"
        "\"sd_health\":{\"total_writes\":4100,\"write_errors\":2,\"usage_pct\":37,"
        "\"space_warning\":false,\"space_critical\":false},"
        "\"uptime_sec\":86400,"
        "\"csi_event_egress\":{\"dropped\":1,\"held_dropped\":2,\"ambient_dropped\":3,"
        "\"unsent_dropped\":4,\"planner\":{\"live\":5,\"held\":6,\"queued\":7,\"replayed\":8,"
        "\"skipped\":9,\"untrusted\":10,\"unsendable\":11,\"truncated_unsent\":12,"
        "\"read_giveups\":13}}}");
  // The object is the one stats_json() spells, unquoted.
  char object[csi_event_egress::kStatsJsonMax];
  CHECK(csi_event_egress::stats_json(st, object, sizeof(object)) > 0);
  const std::string body = build(typical(), &st);
  CHECK(body.find(std::string("\"csi_event_egress\":") + object + "}") ==
        body.size() - std::strlen(object) - std::strlen("\"csi_event_egress\":}"));
  // The flags as JSON booleans, the level as a number.
  wap_diagnostics::Inputs warn = typical();
  warn.degrade_level = 2;
  warn.degrade_name = "CRITICAL";
  warn.sd_space_warning = true;
  warn.sd_space_critical = true;
  const std::string w = build(warn, &st);
  CHECK(w.find("\"degradation\":{\"level\":2,\"level_name\":\"CRITICAL\"}") != std::string::npos);
  CHECK(w.find("\"space_warning\":true,\"space_critical\":true}") != std::string::npos);
  std::printf("PASS every_key_and_the_counters_as_an_object\n");
}

void test_null_before_the_first_pump() {
  const std::string body = build(typical(), nullptr);
  const std::string tail = ",\"uptime_sec\":86400,\"csi_event_egress\":null}";
  CHECK(body.size() > tail.size() &&
        body.compare(body.size() - tail.size(), tail.size(), tail) == 0);
  std::printf("PASS null_before_the_first_pump\n");
}

// The longest name sys_monitor::degrade_level_name() returns, read from its
// switch.
size_t longest_degrade_name() {
  std::ifstream f(SYS_MONITOR_H);
  CHECK(f.good());
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string src = ss.str();
  const size_t at = src.find("const char* degrade_level_name(DegradeLevel level) {");
  CHECK(at != std::string::npos);
  if (at == std::string::npos) return 0;
  const size_t end = src.find("\n}\n", at);
  const std::string body = src.substr(at, end - at);
  const std::regex ret("return\\s+\"([^\"]*)\"\\s*;");
  size_t longest = 0;
  int names = 0;
  for (auto it = std::sregex_iterator(body.begin(), body.end(), ret);
       it != std::sregex_iterator(); ++it) {
    longest = std::max(longest, (size_t)(*it)[1].length());
    ++names;
  }
  CHECK(names >= 5);   // NONE, WARN, CRITICAL, EMERGENCY and the default
  return longest;
}

void test_the_widest_body_fits_the_routes_buffer() {
  const size_t longest = longest_degrade_name();
  CHECK(longest == wap_diagnostics::kDegradeNameMax);
  const std::string name(wap_diagnostics::kDegradeNameMax, 'E');
  wap_diagnostics::Inputs in = {};
  in.heap_free = in.heap_min_free = in.heap_largest_block = in.heap_total = 4294967295u;
  in.degrade_level = 255;
  in.degrade_name = name.c_str();
  in.sd_total_writes = in.sd_write_errors = 4294967295u;
  in.sd_usage_pct = 255;
  in.sd_space_warning = in.sd_space_critical = false;   // "false" is the longer word
  in.uptime_sec = 4294967295u;
  csi_event_egress::Stats st;
  std::memset(&st, 0xFF, sizeof(st));
  const std::string widest = build(in, &st, 4096);
  CHECK(widest.size() == 649);
  // The route's buffer holds it and its NUL.
  CHECK(widest.size() < wap_diagnostics::kJsonMax);
  CHECK(build(in, &st) == widest);
  CHECK(widest.find("\"csi_event_egress\":{\"dropped\":4294967295,") != std::string::npos);
  // One byte short of the body and its NUL: refused whole, no partial body.
  CHECK(build(in, &st, widest.size()).empty());
  CHECK(build(in, &st, widest.size() + 1) == widest);
  // The 512 bytes the route had before the counters do not hold a typical
  // body once a few hundred events are counted.
  csi_event_egress::Stats busy = {};
  busy.planner.live = 12345; busy.planner.queued = 12345; busy.planner.replayed = 12345;
  busy.dropped = busy.held_dropped = busy.ambient_dropped = busy.unsent_dropped = 12345;
  busy.planner.held = busy.planner.skipped = busy.planner.untrusted = 12345;
  busy.planner.unsendable = busy.planner.truncated_unsent = busy.planner.read_giveups = 12345;
  CHECK(build(typical(), &busy, 512).empty());
  CHECK(!build(typical(), &busy).empty());
  std::printf("PASS the_widest_body_fits_the_routes_buffer (%zu of %zu bytes)\n",
              widest.size() + 1, wap_diagnostics::kJsonMax);
}

}  // namespace

int main() {
  test_every_key_and_the_counters_as_an_object();
  test_null_before_the_first_pump();
  test_the_widest_body_fits_the_routes_buffer();
  if (g_failures) {
    std::printf("%d of %d wap_diagnostics checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL wap_diagnostics TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
