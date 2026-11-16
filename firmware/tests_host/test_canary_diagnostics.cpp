/* test_canary_diagnostics.cpp — the canary's GET /api/diagnostics body
 * (sweep F179), through the REAL diagnostics_json.h the route builds it
 * with (canary/lib/securacv_diagnostics/src) and the REAL
 * csi_event_egress_stats_json() its egress object is spelled with
 * (canary/src/csi_event_egress.h).
 *
 * Before F179 the route carried no egress counters (they rode only the MQTT
 * health, so a bench run without a broker never saw them), and it appended
 * its body with `pos += snprintf(...)` into a 2048-byte buffer nothing built
 * off the device. This suite holds:
 *   - the counters' spelling: every field under its own name, the names
 *     main.cpp's MQTT health uses for its `csi_event_egress` object (read
 *     from main.cpp, its path passed in, so the read fails closed), the
 *     widest object inside kCsiEventEgressStatsJsonMax, a buffer one byte
 *     short refused whole;
 *   - the body: every key the route had, byte for byte, then
 *     `csi_event_egress` as an object (`null` when there is none), the
 *     self-test rows (all of them, a null name as "unknown"), the widest
 *     body inside kJsonMax with the longest self-test name read from
 *     securacv_diagnostics.cpp (path passed in), a buffer one byte short
 *     refused whole;
 *   - the handler (securacv_network.cpp, path passed in): it builds its body
 *     with diagnostics_json::build() into a kJsonMax buffer and its egress
 *     object with csi_event_egress_diagnostics_json() into a
 *     kCsiEventEgressDiagnosticsMax one, and spells nothing itself. Each
 *     clause is checked against an in-memory mutation of the handler that
 *     must fail it.
 *   - the self-test's watchdog row, which checks nothing (device-details
 *     §1.4): selftest_run() over stub probes, GET /api/selftest's body and
 *     its handler (it runs the suite, then builds with
 *     diagnostics_json::selftest()), the console's `d` and `t` counts and
 *     the wake's row states (main.cpp), and the "Self-test complete" health
 *     log (securacv_diagnostics.cpp), each source rule checked against
 *     in-memory mutations that must fail it.
 * What the route's copy holds (what the loop task published, null before
 * the first pump) is test_canary_event_egress.cpp's, on the real egress.
 *
 * Build/run: make -C firmware/tests_host. */

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "csi_event_egress.h"
#include "csi_event_egress_diagnostics.h"
#include "diagnostics_json.h"

#if !defined(CANARY_MAIN_CPP) || !defined(CANARY_DIAG_CPP) || !defined(CANARY_NETWORK_CPP)
#error "build with -DCANARY_MAIN_CPP, -DCANARY_DIAG_CPP and -DCANARY_NETWORK_CPP (source paths)"
#endif

static int g_checks = 0;
static int g_fail = 0;
#define CHECK(cond, msg) do { \
  ++g_checks; \
  if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); g_fail++; } \
  else { std::printf("ok   %s\n", msg); } } while (0)

static std::string read_file(const char* path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

/* ── The counters' spelling ────────────────────────────────────────────── */

/* Every counter a value of its own, so a name that spells another field's
 * counter shows. */
static CsiEventEgressStats distinct() {
  CsiEventEgressStats s;
  s.dropped = 101;
  s.held_dropped = 102;
  s.ambient_dropped = 103;
  s.unsent_dropped = 104;
  s.planner.live = 201;
  s.planner.held = 202;
  s.planner.queued = 203;
  s.planner.replayed = 204;
  s.planner.skipped = 205;
  s.planner.untrusted = 206;
  s.planner.unsendable = 207;
  s.planner.truncated_unsent = 208;
  s.planner.read_giveups = 209;
  return s;
}

/* The field main.cpp's health reads, by the name it reads it under there. */
static bool field_value(const CsiEventEgressStats& s, const std::string& field, bool planner,
                        uint32_t* out) {
  static const std::map<std::string, uint32_t CsiEventEgressStats::*> top = {
    {"dropped", &CsiEventEgressStats::dropped},
    {"held_dropped", &CsiEventEgressStats::held_dropped},
    {"ambient_dropped", &CsiEventEgressStats::ambient_dropped},
    {"unsent_dropped", &CsiEventEgressStats::unsent_dropped},
  };
  static const std::map<std::string, uint32_t csi_event_backfill::Stats::*> plan = {
    {"live", &csi_event_backfill::Stats::live},
    {"held", &csi_event_backfill::Stats::held},
    {"queued", &csi_event_backfill::Stats::queued},
    {"replayed", &csi_event_backfill::Stats::replayed},
    {"skipped", &csi_event_backfill::Stats::skipped},
    {"untrusted", &csi_event_backfill::Stats::untrusted},
    {"unsendable", &csi_event_backfill::Stats::unsendable},
    {"truncated_unsent", &csi_event_backfill::Stats::truncated_unsent},
    {"read_giveups", &csi_event_backfill::Stats::read_giveups},
  };
  if (planner) {
    auto it = plan.find(field);
    if (it == plan.end()) return false;
    *out = s.planner.*(it->second);
    return true;
  }
  auto it = top.find(field);
  if (it == top.end()) return false;
  *out = s.*(it->second);
  return true;
}

static std::string spell(const CsiEventEgressStats& s) {
  char buf[kCsiEventEgressStatsJsonMax];
  const size_t n = csi_event_egress_stats_json(s, buf, sizeof(buf));
  return std::string(buf, n);
}

static void test_the_counters_spell_the_healths_names() {
  std::printf("-- F179: the counters spell the names the MQTT health uses\n");
  const CsiEventEgressStats s = distinct();
  const std::string got = spell(s);
  CHECK(got ==
        "{\"dropped\":101,\"held_dropped\":102,\"ambient_dropped\":103,\"unsent_dropped\":104,"
        "\"planner\":{\"live\":201,\"held\":202,\"queued\":203,\"replayed\":204,\"skipped\":205,"
        "\"untrusted\":206,\"unsendable\":207,\"truncated_unsent\":208,\"read_giveups\":209}}",
        "every counter under its own name, the planner's nested");

  /* main.cpp mqtt_publish_health_update(): ego["<name>"] = st.<field>;
   * plo["<name>"] = st.planner.<field>; each name must carry that field's
   * counter here, and no name may be missing on either side. */
  const std::string main_cpp = read_file(CANARY_MAIN_CPP);
  CHECK(!main_cpp.empty(), "main.cpp read (the pins fail closed without it)");
  const std::regex ego_re("\\bego\\[\"([a-z0-9_]+)\"\\]\\s*=\\s*st\\.(\\w+);");
  const std::regex plo_re("\\bplo\\[\"([a-z0-9_]+)\"\\]\\s*=\\s*st\\.planner\\.(\\w+);");
  size_t names = 0;
  bool all_spelled = true;
  for (int pass = 0; pass < 2; ++pass) {
    const std::regex& re = pass == 0 ? ego_re : plo_re;
    for (std::sregex_iterator it(main_cpp.begin(), main_cpp.end(), re), end; it != end; ++it) {
      const std::string name = (*it)[1];
      const std::string field = (*it)[2];
      uint32_t v = 0;
      const bool known = field_value(s, field, pass == 1, &v);
      const std::string want = "\"" + name + "\":" + std::to_string(v);
      if (!known || got.find(want) == std::string::npos) {
        std::printf("  health spells %s as st.%s%s; the diagnostics object does not\n",
                    name.c_str(), pass == 1 ? "planner." : "", field.c_str());
        all_spelled = false;
      }
      ++names;
    }
  }
  CHECK(names == 13, "the health names thirteen counters");
  CHECK(all_spelled, "each one here, under the health's name, with the health's field");
  size_t keys = 0;
  for (size_t at = got.find("\":"); at != std::string::npos; at = got.find("\":", at + 1)) ++keys;
  CHECK(keys == names + 1, "and nothing else (the thirteen and the planner's object)");

  CsiEventEgressStats wide;
  std::memset(&wide, 0xFF, sizeof(wide));
  char buf[kCsiEventEgressStatsJsonMax];
  const size_t n = csi_event_egress_stats_json(wide, buf, sizeof(buf));
  CHECK(n == 319 && n < kCsiEventEgressStatsJsonMax,
        "every counter at 4294967295: 319 bytes, inside kCsiEventEgressStatsJsonMax");
  CHECK(kCsiEventEgressStatsJsonMax <= (size_t)kCsiEventEgressDiagnosticsMax,
        "and inside the route's egress buffer");
  std::vector<char> short_buf(n, 'x');
  CHECK(csi_event_egress_stats_json(wide, short_buf.data(), short_buf.size()) == 0 &&
        short_buf[0] == '\0', "a buffer one byte short gets nothing, never a partial object");
  CHECK(csi_event_egress_stats_json(wide, buf, n + 1) == n, "one that fits gets it all");
  CHECK(csi_event_egress_stats_json(wide, nullptr, 64) == 0, "no buffer: nothing");
}

/* ── The body ──────────────────────────────────────────────────────────── */

static diag_snapshot_t typical() {
  diag_snapshot_t s;
  std::memset(&s, 0, sizeof(s));
  s.heap.free_heap = 182344;
  s.heap.min_heap = 151020;
  s.heap.largest_block = 110592;
  s.heap.psram_free = 7340032;
  s.heap.psram_total = 8388608;
  s.heap.stack_hwm_main = 3120;
  s.heap.degrade_level = DEGRADE_NONE;
  s.heap.fragmentation_pct = 39;
  s.sd.mounted = true;
  s.sd.usage_pct = 37;
  s.sd.total_writes = 4100;
  s.sd.write_errors = 2;
  s.selftest.has_run = true;
  s.selftest.health_score = 90;
  s.selftest.passed_count = 1;
  s.selftest.total_count = 2;
  s.selftest.tests[0] = {"nvs_rw", true, 4, false};
  s.selftest.tests[1] = {"sd_card", false, 120, false};
  s.uptime_sec = 86400;
  s.boot_count = 12;
  s.reset_reason = 1;
  return s;
}

static std::string body(const diag_snapshot_t& snap, const char* egress) {
  char buf[diagnostics_json::kJsonMax];
  const size_t n = diagnostics_json::build(snap, "2.4.15", egress, buf, sizeof(buf));
  return std::string(buf, n);
}

/* The route's keys as they stood before F179, for typical(): the old
 * handler's snprintf chain run over it (checked once against that code). */
static const char* const kBeforeF179 =
    "{\"heap\":{\"free\":182344,\"min\":151020,\"largest_block\":110592,"
    "\"psram_free\":7340032,\"psram_total\":8388608,"
    "\"stack_hwm\":3120,\"fragmentation_pct\":39,\"degrade_level\":\"none\"},"
    "\"sd\":{\"mounted\":true,\"usage_pct\":37,\"total_writes\":4100,\"write_errors\":2,"
    "\"space_warning\":false,\"space_critical\":false},"
    "\"selftest\":{\"has_run\":true,\"health_score\":90,\"passed\":1,\"total\":2,\"tests\":["
    "{\"name\":\"nvs_rw\",\"passed\":true,\"ms\":4},"
    "{\"name\":\"sd_card\",\"passed\":false,\"ms\":120}]},"
    "\"system\":{\"uptime_sec\":86400,\"boot_count\":12,\"reset_reason\":1,"
    "\"firmware\":\"2.4.15\"}";

static void test_the_body_keeps_the_routes_keys_and_adds_the_counters() {
  std::printf("-- F179: the body is the route's, byte for byte, then the egress counters\n");
  const std::string egress = spell(distinct());
  const std::string got = body(typical(), egress.c_str());
  CHECK(got == std::string(kBeforeF179) + ",\"csi_event_egress\":" + egress + "}",
        "every key the route had, unchanged, then csi_event_egress as an object");
  CHECK(body(typical(), "null") == std::string(kBeforeF179) + ",\"csi_event_egress\":null}",
        "before the first pump: null");
  CHECK(body(typical(), nullptr) == body(typical(), "null") &&
        body(typical(), "") == body(typical(), "null"),
        "no object, or one that did not fit its buffer, is null too");
  CHECK(got.find("\"csi_event_egress\":\"") == std::string::npos,
        "the object is not quoted into a string");

  diag_snapshot_t s = typical();
  const char* names[4] = {"warn", "critical", "emergency", "none"};
  const uint8_t levels[4] = {DEGRADE_WARN, DEGRADE_CRITICAL, DEGRADE_EMERGENCY, 7};
  bool all = true;
  for (int i = 0; i < 4; ++i) {
    s.heap.degrade_level = levels[i];
    const std::string want = std::string("\"degrade_level\":\"") + names[i] + "\"";
    all = all && body(s, nullptr).find(want) != std::string::npos;
  }
  CHECK(all, "the degradation level spelled as the route always did (unknown: none)");

  s = typical();
  s.selftest.total_count = SELFTEST_COUNT;
  for (int i = 0; i < SELFTEST_COUNT; ++i) s.selftest.tests[i] = {"t", true, (uint16_t)i, false};
  s.selftest.tests[3].name = nullptr;
  const std::string all_rows = body(s, nullptr);
  size_t rows = 0;
  for (size_t at = all_rows.find("{\"name\":"); at != std::string::npos;
       at = all_rows.find("{\"name\":", at + 1)) ++rows;
  CHECK(rows == SELFTEST_COUNT, "every self-test row, none dropped to stay inside the buffer");
  CHECK(all_rows.find("{\"name\":\"unknown\",\"passed\":true,\"ms\":3}") != std::string::npos,
        "a row with no name is \"unknown\"");
  s.selftest.total_count = 200;
  const std::string past = body(s, nullptr);
  size_t past_rows = 0;
  for (size_t at = past.find("{\"name\":"); at != std::string::npos;
       at = past.find("{\"name\":", at + 1)) ++past_rows;
  CHECK(past_rows == SELFTEST_COUNT, "a count past SELFTEST_COUNT reads no row past the array");
}

static void test_the_widest_body_fits_the_routes_buffer() {
  std::printf("-- F179: the widest body fits kJsonMax; one byte short is refused whole\n");
  /* The longest self-test name the firmware registers ({"name", fn} rows
   * in securacv_diagnostics.cpp's table). */
  const std::string diag_cpp = read_file(CANARY_DIAG_CPP);
  /* A row is {"name", test_fn}, or {"name", nullptr} for one the suite lists
   * and does not probe (the watchdog's). */
  const std::regex row_re("\\{\\s*\"([a-z0-9_]+)\"\\s*,\\s*(?:test_\\w+|nullptr)\\s*\\}");
  size_t longest = 0;
  size_t registered = 0;
  for (std::sregex_iterator it(diag_cpp.begin(), diag_cpp.end(), row_re), end; it != end; ++it) {
    longest = std::max(longest, (size_t)(*it)[1].length());
    ++registered;
  }
  CHECK(registered == SELFTEST_COUNT && longest >= 6,
        "securacv_diagnostics.cpp read: SELFTEST_COUNT tests and their names");

  diag_snapshot_t s;
  std::memset(&s, 0xFF, sizeof(s));
  s.heap.degrade_level = DEGRADE_EMERGENCY;   /* the longest name */
  s.sd.mounted = s.sd.space_warning = s.sd.space_critical = false;   /* "false" is the wider */
  s.selftest.has_run = false;
  s.selftest.total_count = SELFTEST_COUNT;
  const std::string name(longest, 'n');
  for (int i = 0; i < SELFTEST_COUNT; ++i) s.selftest.tests[i] = {name.c_str(), false, 0xFFFF, false};
  const std::string firmware(32, '9');
  CsiEventEgressStats wide;
  std::memset(&wide, 0xFF, sizeof(wide));
  const std::string egress = spell(wide);

  char buf[diagnostics_json::kJsonMax];
  const size_t n = diagnostics_json::build(s, firmware.c_str(), egress.c_str(), buf, sizeof(buf));
  std::printf("  widest body: %zu bytes (longest self-test name %zu)\n", n, longest);
  CHECK(n == 1352, "the widest body is 1352 bytes (the number diagnostics_json.h states)");
  CHECK(n > 0 && n < diagnostics_json::kJsonMax, "inside kJsonMax");
  std::vector<char> short_buf(n, 'x');
  CHECK(diagnostics_json::build(s, firmware.c_str(), egress.c_str(), short_buf.data(),
                                short_buf.size()) == 0 && short_buf[0] == '\0',
        "a buffer one byte short gets nothing, never a partial body");
  CHECK(diagnostics_json::build(s, firmware.c_str(), egress.c_str(), buf, n + 1) == n,
        "one that fits gets it all");
  CHECK(diagnostics_json::build(s, firmware.c_str(), egress.c_str(), nullptr, 64) == 0,
        "no buffer: nothing");
}

/* ── A row that is not a probe (device-details §1.4) ─────────────────────── */

/* The watchdog row's old test returned true on both branches of its #if, so
 * every run counted a pass nothing checked: a tenth of the health score. The
 * table now lists it with no function; the runner marks it not_a_probe, the
 * body says so in place of passed/ms, and `total` counts the probes. */
static void test_a_row_that_is_not_a_probe_says_so() {
  std::printf("-- §1.4: the watchdog row is listed, not probed, and counted as neither\n");
  const std::string diag_cpp = read_file(CANARY_DIAG_CPP);
  const std::regex watchdog_re("\\{\\s*\"watchdog\"\\s*,\\s*nullptr\\s*\\}");
  CHECK(std::regex_search(diag_cpp, watchdog_re),
        "securacv_diagnostics.cpp lists the watchdog row with no probe function");
  CHECK(diag_cpp.find("test_watchdog") == std::string::npos,
        "no test_watchdog() that answers true without checking anything");

  diag_snapshot_t s = typical();
  s.selftest.total_count = 3;
  s.selftest.passed_count = 1;
  s.selftest.health_score = 50;
  s.selftest.tests[2] = {"watchdog", false, 0, true};
  CHECK(selftest_probe_count(&s.selftest) == 2, "two of the three rows are probes");
  const std::string got = body(s, nullptr);
  CHECK(got.find("\"passed\":1,\"total\":2,\"tests\":[") != std::string::npos,
        "total counts the probes, so passed of total is the score's own fraction");
  CHECK(got.find(",{\"name\":\"watchdog\",\"probe\":false}]") != std::string::npos,
        "the row says probe:false, with no passed and no ms");
  CHECK(got.find("{\"name\":\"watchdog\",\"passed\"") == std::string::npos,
        "the row never reads as a pass or a fail");
  CHECK(got.find("{\"name\":\"nvs_rw\",\"passed\":true,\"ms\":4},"
                 "{\"name\":\"sd_card\",\"passed\":false,\"ms\":120},") != std::string::npos,
        "the probe rows are spelled as they always were");

  s.selftest.tests[0].not_a_probe = true;
  s.selftest.tests[1].not_a_probe = true;
  CHECK(selftest_probe_count(&s.selftest) == 0, "a report with no probe counts none");
  CHECK(selftest_probe_count(nullptr) == 0, "no report: none");
  s.selftest.total_count = 200;
  CHECK(selftest_probe_count(&s.selftest) <= SELFTEST_COUNT, "a count past SELFTEST_COUNT reads no row past the array");
}

/* ── The runner (device-details §1.4) ───────────────────────────────────── */

/* The real runner, selftest_run() (securacv_diagnostics.h), which
 * diag_run_selftest() runs the device's table through, over stub probes. A
 * call of a row with no probe would be a call through a null pointer: the
 * suite would crash, which fails it. */
static int g_probe_calls = 0;
static bool probe_pass() { ++g_probe_calls; return true; }
static bool probe_fail() { ++g_probe_calls; return false; }
static uint32_t g_clock_ms = 0;
static uint32_t fake_clock_ms() { return g_clock_ms += 5; }

/* The device's rows, in its table's order (securacv_diagnostics.cpp). */
static const char* const kRowNames[SELFTEST_COUNT] = {
    "nvs_rw", "heap_ok", "psram_ok", "crypto_ok", "sd_card",
    "wifi_ok", "temp_range", "uptime_ok", "watchdog", "chain_ok"};
static const int kWatchdogRow = 8;

static void test_the_runner_scores_the_probes_and_calls_no_unprobed_row() {
  std::printf("-- §1.4: the runner scores the probes, and never calls a row with none\n");
  selftest_def_t defs[SELFTEST_COUNT];
  for (int i = 0; i < SELFTEST_COUNT; ++i)
    defs[i] = {kRowNames[i], i == kWatchdogRow ? nullptr : probe_pass};
  selftest_report_t r;
  std::memset(&r, 0xAB, sizeof(r));
  g_probe_calls = 0;
  const uint8_t probes = selftest_run(defs, SELFTEST_COUNT, fake_clock_ms, &r);
  CHECK(probes == 9 && g_probe_calls == 9,
        "nine probes run, each once; the row with none is never called");
  CHECK(r.has_run && r.total_count == SELFTEST_COUNT, "every row is listed");
  CHECK(r.passed_count == 9 && r.health_score == 100,
        "every probe passing scores 100: 9 of 9, never 9 of 10");
  CHECK(r.tests[kWatchdogRow].not_a_probe && !r.tests[kWatchdogRow].passed &&
            r.tests[kWatchdogRow].duration_ms == 0 &&
            std::strcmp(r.tests[kWatchdogRow].name, "watchdog") == 0,
        "the watchdog row: not a probe, and not a pass");
  bool rows_ok = true;
  for (int i = 0; i < SELFTEST_COUNT; ++i) {
    if (i == kWatchdogRow) continue;
    rows_ok = rows_ok && !r.tests[i].not_a_probe && r.tests[i].passed &&
              r.tests[i].duration_ms == 5 && std::strcmp(r.tests[i].name, kRowNames[i]) == 0;
  }
  CHECK(rows_ok, "each probe row: its name, a probe, its result and its time");
  CHECK(selftest_probe_count(&r) == probes, "the report counts the probes the runner ran");

  for (int i = 0; i < SELFTEST_COUNT; ++i)
    if (i != kWatchdogRow) defs[i].fn = i < 4 ? probe_pass : probe_fail;
  (void)selftest_run(defs, SELFTEST_COUNT, fake_clock_ms, &r);
  CHECK(r.passed_count == 4 && r.health_score == 44,
        "four of nine probes passing scores 44, under the OTA confirm gate's 50");

  for (int i = 0; i < SELFTEST_COUNT; ++i) defs[i].fn = probe_pass;
  defs[0].fn = probe_fail;
  (void)selftest_run(defs, SELFTEST_COUNT, fake_clock_ms, &r);
  CHECK(r.passed_count == 9 && r.health_score == 90 && selftest_probe_count(&r) == 10,
        "a suite with every row a probe scores over all of them: 9 of 10 is 90");

  for (int i = 0; i < SELFTEST_COUNT; ++i) defs[i].fn = nullptr;
  g_probe_calls = 0;
  CHECK(selftest_run(defs, SELFTEST_COUNT, fake_clock_ms, &r) == 0 && r.health_score == 0 &&
            r.passed_count == 0 && g_probe_calls == 0,
        "a suite with no probe scores 0 and calls nothing");
  CHECK(selftest_run(defs, 200, fake_clock_ms, &r) == 0 && r.total_count == SELFTEST_COUNT,
        "a count past SELFTEST_COUNT runs no row past the array");
  CHECK(selftest_run(nullptr, 3, fake_clock_ms, &r) == 0 && r.total_count == 0,
        "no table: no rows");
  CHECK(selftest_run(defs, 3, fake_clock_ms, nullptr) == 0, "no report: nothing");

  /* The device runs its table through it, and counts, scores and marks
   * nothing itself. */
  const std::string diag_cpp = read_file(CANARY_DIAG_CPP);
  const size_t at = diag_cpp.find("uint8_t diag_run_selftest(void) {");
  const size_t end = at == std::string::npos ? at : diag_cpp.find("\n}\n", at);
  const std::string fn = end == std::string::npos ? "" : diag_cpp.substr(at, end - at);
  CHECK(!fn.empty() && fn.find("selftest_run(TESTS, SELFTEST_COUNT, ") != std::string::npos,
        "diag_run_selftest() runs its table through selftest_run()");
  const std::regex writes("(passed_count|health_score|total_count|not_a_probe|passed|duration_ms)"
                          "\\s*(=[^=]|\\+\\+)");
  CHECK(!fn.empty() && !std::regex_search(fn, writes),
        "and writes no count, score, result or mark of its own");
  CHECK(diag_cpp.find("static const selftest_def_t TESTS[SELFTEST_COUNT] = {") != std::string::npos,
        "its table is the runner's row type");
  std::vector<std::string> names;
  const std::regex row_re("\\{\\s*\"([a-z0-9_]+)\"\\s*,\\s*(?:test_\\w+|nullptr)\\s*\\}");
  for (std::sregex_iterator it(diag_cpp.begin(), diag_cpp.end(), row_re), e; it != e; ++it)
    names.push_back((*it)[1]);
  bool same = names.size() == SELFTEST_COUNT;
  for (size_t i = 0; same && i < names.size(); ++i) same = names[i] == kRowNames[i];
  CHECK(same, "the stubs above are the device's rows, in its order");
}

/* ── GET /api/selftest (device-details §1.4) ─────────────────────────────── */

static std::string selftest_body(const selftest_report_t& st) {
  char buf[diagnostics_json::kSelftestJsonMax];
  const size_t n = diagnostics_json::selftest(st, buf, sizeof(buf));
  return std::string(buf, n);
}

static selftest_report_t three_rows() {
  selftest_report_t st;
  std::memset(&st, 0, sizeof(st));
  st.has_run = true;
  st.health_score = 50;
  st.passed_count = 1;
  st.total_count = 3;
  st.tests[0] = {"nvs_rw", true, 4, false};
  st.tests[1] = {"sd_card", false, 120, false};
  st.tests[2] = {"watchdog", false, 0, true};
  return st;
}

static void test_the_selftest_route_says_not_a_probe_too() {
  std::printf("-- §1.4: GET /api/selftest spells its rows as /api/diagnostics does\n");
  selftest_report_t st = three_rows();
  const std::string rows =
      "{\"name\":\"nvs_rw\",\"passed\":true,\"ms\":4},"
      "{\"name\":\"sd_card\",\"passed\":false,\"ms\":120},"
      "{\"name\":\"watchdog\",\"probe\":false}";
  CHECK(selftest_body(st) ==
            "{\"ok\":true,\"health_score\":50,\"passed\":1,\"total\":2,\"tests\":[" + rows + "]}",
        "the watchdog row says probe:false, and total counts the probes (1 of 2, the score's 50)");
  diag_snapshot_t snap = typical();
  snap.selftest = st;
  CHECK(body(snap, nullptr).find("\"passed\":1,\"total\":2,\"tests\":[" + rows + "]}") !=
            std::string::npos,
        "the same rows and totals, byte for byte, as GET /api/diagnostics");

  st.total_count = 2;   /* no unprobed row: the route's old body, unchanged */
  CHECK(selftest_body(st) ==
            "{\"ok\":true,\"health_score\":50,\"passed\":1,\"total\":2,\"tests\":["
            "{\"name\":\"nvs_rw\",\"passed\":true,\"ms\":4},"
            "{\"name\":\"sd_card\",\"passed\":false,\"ms\":120}]}",
        "a report of probes only reads as the route always wrote it");

  selftest_def_t defs[SELFTEST_COUNT];
  for (int i = 0; i < SELFTEST_COUNT; ++i)
    defs[i] = {kRowNames[i], i == kWatchdogRow ? nullptr : probe_pass};
  (void)selftest_run(defs, SELFTEST_COUNT, fake_clock_ms, &st);
  const std::string healthy = selftest_body(st);
  CHECK(healthy.find("{\"ok\":true,\"health_score\":100,\"passed\":9,\"total\":9,") == 0,
        "a unit with every probe passing answers 9 of 9 at 100, not 9 of 10");
  CHECK(healthy.find("{\"name\":\"watchdog\",\"probe\":false}") != std::string::npos &&
            healthy.find("{\"name\":\"watchdog\",\"passed\"") == std::string::npos,
        "and its watchdog row is never a failed probe");

  st = three_rows();
  st.tests[0].name = nullptr;
  st.total_count = 200;
  const std::string past = selftest_body(st);
  CHECK(past.find("{\"name\":\"unknown\",\"passed\":true,\"ms\":4}") != std::string::npos,
        "a row with no name is \"unknown\"");
  size_t n_rows = 0;
  for (size_t p = past.find("{\"name\":"); p != std::string::npos; p = past.find("{\"name\":", p + 1))
    ++n_rows;
  CHECK(n_rows == SELFTEST_COUNT, "a count past SELFTEST_COUNT reads no row past the array");

  /* The widest body fits its buffer; one byte short gets nothing. */
  const std::string diag_cpp = read_file(CANARY_DIAG_CPP);
  const std::regex row_re("\\{\\s*\"([a-z0-9_]+)\"\\s*,\\s*(?:test_\\w+|nullptr)\\s*\\}");
  size_t longest = 0;
  for (std::sregex_iterator it(diag_cpp.begin(), diag_cpp.end(), row_re), e; it != e; ++it)
    longest = std::max(longest, (size_t)(*it)[1].length());
  selftest_report_t wide;
  std::memset(&wide, 0xFF, sizeof(wide));
  wide.total_count = SELFTEST_COUNT;
  const std::string name(longest, 'n');
  for (int i = 0; i < SELFTEST_COUNT; ++i) wide.tests[i] = {name.c_str(), false, 0xFFFF, false};
  char buf[diagnostics_json::kSelftestJsonMax];
  const size_t n = diagnostics_json::selftest(wide, buf, sizeof(buf));
  std::printf("  widest /api/selftest body: %zu bytes (longest self-test name %zu)\n", n, longest);
  CHECK(n == 544, "the widest body is 544 bytes (the number diagnostics_json.h states)");
  CHECK(n > 0 && n < diagnostics_json::kSelftestJsonMax, "inside kSelftestJsonMax");
  std::vector<char> short_buf(n, 'x');
  CHECK(diagnostics_json::selftest(wide, short_buf.data(), short_buf.size()) == 0 &&
            short_buf[0] == '\0',
        "a buffer one byte short gets nothing, never a partial body");
}

/* "" when handle_selftest builds its body the way this suite holds it, else
 * what is wrong (whitespace squashed first). */
static std::string selftest_handler_rule(const std::string& raw) {
  std::string h;
  for (char c : raw) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
    h += c;
  }
  if (h.empty()) return "no handle_selftest";
  if (h.find("if(!auth_gate(req))returnESP_OK;") == std::string::npos)
    return "it is not behind the bearer";
  /* GET /api/selftest runs the suite: without the run it answers whatever
   * report the last run (the boot's, or the console's `t`) left behind. */
  const size_t run = h.find("(void)diag_run_selftest();");
  const size_t read = h.find("if(!diag_get_selftest(&report)){returnhttp_send_error(");
  if (run == std::string::npos || read == std::string::npos || run > read)
    return "it does not run the suite before it reads the report (it would answer a cached run)";
  if (h.find("charbuf[diagnostics_json::kSelftestJsonMax];") == std::string::npos)
    return "the body buffer is not diagnostics_json::kSelftestJsonMax";
  if (h.find("if(diagnostics_json::selftest(report,buf,sizeof(buf))==0){returnhttp_send_error(") ==
      std::string::npos)
    return "the body is not built by diagnostics_json::selftest() (or its refusal is not answered)";
  if (h.find("returnhttp_send_json(req,buf);") == std::string::npos)
    return "the built body is not what it sends";
  if (h.find("printf(") != std::string::npos || h.find("total_count") != std::string::npos ||
      h.find("passed_count") != std::string::npos || h.find(".passed") != std::string::npos)
    return "it spells a body, or reads the report's counts, itself";
  return "";
}

static void test_the_selftest_handler_builds_with_the_builder() {
  std::printf("-- §1.4: GET /api/selftest builds its body with diagnostics_json::selftest()\n");
  const std::string src = read_file(CANARY_NETWORK_CPP);
  const std::string open = "static esp_err_t handle_selftest(httpd_req_t* req) {";
  const size_t at = src.find(open);
  const size_t end = at == std::string::npos ? at : src.find("\n}\n", at);
  const std::string h = end == std::string::npos ? "" : src.substr(at, end - at);
  CHECK(!h.empty(), "securacv_network.cpp read; handle_selftest found");
  const std::string verdict = selftest_handler_rule(h);
  if (!verdict.empty()) std::printf("  %s\n", verdict.c_str());
  CHECK(verdict.empty(), "the handler holds to the builder and its buffer");

  struct Mutation { const char* name; const char* from; const char* to; };
  const Mutation muts[] = {
    {"a smaller buffer", "char buf[diagnostics_json::kSelftestJsonMax];", "char buf[256];"},
    {"the refusal unanswered", ") == 0) {\n    return http_send_error(req, 500, \"selftest_too_large\");\n  }",
     ");"},
    {"the bearer dropped", "if (!auth_gate(req)) return ESP_OK;", ""},
    {"the old totals back", "return http_send_json(req, buf);",
     "snprintf(buf, sizeof(buf), \"%u\", report.total_count); return http_send_json(req, buf);"},
    {"another body sent", "return http_send_json(req, buf);", "return http_send_json(req, \"{}\");"},
    {"the run dropped (a cached report answered)", "(void)diag_run_selftest();", ""},
    {"the report read before the run",
     "(void)diag_run_selftest();\n\n  selftest_report_t report;\n  if (!diag_get_selftest(&report)) {\n"
     "    return http_send_error(req, 500, \"selftest_failed\");\n  }",
     "selftest_report_t report;\n  if (!diag_get_selftest(&report)) {\n"
     "    return http_send_error(req, 500, \"selftest_failed\");\n  }\n  (void)diag_run_selftest();"},
  };
  int caught = 0;
  int applied = 0;
  for (const Mutation& m : muts) {
    std::string mutated = h;
    const size_t p = mutated.find(m.from);
    if (p == std::string::npos) {
      std::printf("  mutation \"%s\" does not apply\n", m.name);
      continue;
    }
    ++applied;
    mutated.replace(p, std::strlen(m.from), m.to);
    if (!selftest_handler_rule(mutated).empty()) ++caught;
    else std::printf("  mutation \"%s\" passes the rule\n", m.name);
  }
  const int n_muts = (int)(sizeof(muts) / sizeof(muts[0]));
  CHECK(applied == n_muts && caught == n_muts, "and each of seven mutations of it fails the rule");
}

/* ── The console's counts and the wake (main.cpp) ────────────────────────── */

/* "" when run_wake draws every row's state through scene::probe_state() the
 * way this suite holds it, else what is wrong. Its rows are set four times:
 * pending for the first frame, then revealed on the plain ASCII floor (one
 * printed block), at each ANSI step, and on a skip. A reveal left pending
 * prints every probe as [..] beside a finished score. */
static std::string wake_rule(const std::string& wake) {
  if (wake.empty()) return "no run_wake";
  if (wake.find("return scene::probe_state(st.tests[i].not_a_probe, revealed, st.tests[i].passed);") ==
      std::string::npos)
    return "its rows do not take their state from scene::probe_state() and the report";
  const std::regex assign("\\.state\\s*=\\s*([^;]+);");
  const std::regex through("state_of\\(\\s*\\w+\\s*,\\s*(true|false)\\s*\\)");
  std::vector<bool> revealed;
  for (std::sregex_iterator it(wake.begin(), wake.end(), assign), e; it != e; ++it) {
    const std::string rhs = (*it)[1];
    std::smatch m;
    if (!std::regex_match(rhs, m, through))
      return "a row's state is set as " + rhs + ", not through state_of()";
    revealed.push_back(m[1] == "true");
  }
  if (revealed.size() != 4)
    return "its rows are set " + std::to_string(revealed.size()) +
           " times, not four (pending, the plain floor, each ANSI step, a skip)";
  if (revealed[0]) return "the first frame reveals its rows";
  for (size_t k = 1; k < revealed.size(); ++k)
    if (!revealed[k]) return "a reveal (the plain floor's, an ANSI step's or a skip's) leaves its rows pending";
  const size_t floor_at = wake.find("if (!caps.ansi) {");
  const size_t ansi_at = floor_at == std::string::npos ? floor_at : wake.find("} else {", floor_at);
  const std::string floor = ansi_at == std::string::npos ? "" : wake.substr(floor_at, ansi_at - floor_at);
  const size_t reveal = floor.find("state_of(i, true)");
  if (reveal == std::string::npos || reveal > floor.find("scene::wake_frame("))
    return "the plain floor prints its block before it reveals the rows";
  return "";
}

static void test_the_console_counts_over_the_probes() {
  std::printf("-- §1.4: every console line counts passes over the probes; the wake draws each row by probe_state()\n");
  const std::string main_cpp = read_file(CANARY_MAIN_CPP);
  /* Every printf argument list that prints a passed_count puts the probes
   * beside it (`d`'s line and `t`'s), never the rows. */
  const std::regex pair("passed_count\\s*,\\s*([^,]+?)\\s*,");
  size_t lines = 0;
  size_t over_probes = 0;
  for (std::sregex_iterator it(main_cpp.begin(), main_cpp.end(), pair), e; it != e; ++it) {
    ++lines;
    if ((*it)[1].str().rfind("selftest_probe_count(", 0) == 0) ++over_probes;
    else std::printf("  passed_count over %s\n", (*it)[1].str().c_str());
  }
  CHECK(lines >= 2 && over_probes == lines,
        "`d` and `t` print the passes over selftest_probe_count(), never over the rows");

  const size_t at = main_cpp.find("static void run_wake() {");
  const size_t end = at == std::string::npos ? at : main_cpp.find("\n}\n", at);
  const std::string wake = end == std::string::npos ? "" : main_cpp.substr(at, end - at);
  const std::string verdict = wake_rule(wake);
  if (!verdict.empty()) std::printf("  %s\n", verdict.c_str());
  CHECK(verdict.empty(),
        "run_wake sets every row through probe_state(): pending first, revealed on the plain "
        "floor, at each ANSI step and on a skip");

  struct Mutation { const char* name; const char* from; const char* to; };
  const Mutation muts[] = {
    {"the plain floor never reveals", "for (int i = 0; i < n; ++i) probes[i].state = state_of(i, true);",
     "for (int i = 0; i < n; ++i) probes[i].state = state_of(i, false);"},
    {"an ANSI step never reveals", "!skipped; ++i) {\n      probes[i].state = state_of(i, true);",
     "!skipped; ++i) {\n      probes[i].state = state_of(i, false);"},
    {"a skip never reveals", "probes[j].state = state_of(j, true);", "probes[j].state = state_of(j, false);"},
    {"the first frame revealed", "probes[i].state = state_of(i, false);", "probes[i].state = state_of(i, true);"},
    {"a literal state", "probes[j].state = state_of(j, true);",
     "probes[j].state = scene::ProbeState::Pass;"},
    {"the report's flag dropped", "st.tests[i].not_a_probe, revealed", "false, revealed"},
    {"the plain floor's reveal gone",
     "    for (int i = 0; i < n; ++i) probes[i].state = state_of(i, true);\n", ""},
  };
  int caught = 0;
  int applied = 0;
  for (const Mutation& m : muts) {
    std::string mutated = wake;
    const size_t p = mutated.find(m.from);
    if (p == std::string::npos) {
      std::printf("  mutation \"%s\" does not apply\n", m.name);
      continue;
    }
    ++applied;
    mutated.replace(p, std::strlen(m.from), m.to);
    if (!wake_rule(mutated).empty()) ++caught;
    else std::printf("  mutation \"%s\" passes the rule\n", m.name);
  }
  const int n_muts = (int)(sizeof(muts) / sizeof(muts[0]));
  CHECK(applied == n_muts && caught == n_muts, "and each of seven mutations of it fails the rule");
}

/* ── The health log (securacv_diagnostics.cpp) ───────────────────────────── */

/* "" when diag_run_selftest() logs its passes over the probes selftest_run()
 * counted, else what is wrong (whitespace squashed first). Its "Self-test
 * complete" line read x/10 while the score was over nine. */
static std::string health_log_rule(const std::string& raw) {
  std::string b;
  for (char c : raw) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
    b += c;
  }
  if (b.empty()) return "no diag_run_selftest";
  if (b.find("constuint8_tprobes=selftest_run(TESTS,SELFTEST_COUNT,selftest_clock_ms,&s_selftest);") ==
      std::string::npos)
    return "it does not run the suite through selftest_run(), keeping its probe count";
  if (b.find("(unsigned)s_selftest.passed_count,(unsigned)probes,(unsigned)s_selftest.health_score);") ==
      std::string::npos)
    return "its log line does not print the passes over the probes";
  size_t rows = 0;
  for (size_t p = b.find("SELFTEST_COUNT"); p != std::string::npos; p = b.find("SELFTEST_COUNT", p + 1)) ++rows;
  if (rows != 1 || b.find("total_count") != std::string::npos)
    return "it counts the rows somewhere besides the runner's argument";
  if (b.find("log_health(LOG_LEVEL_INFO,LOG_CAT_SYSTEM,\"Self-testcomplete\",detail);") == std::string::npos)
    return "it does not log the line it built";
  return "";
}

static void test_the_health_log_counts_over_the_probes() {
  std::printf("-- §1.4: the \"Self-test complete\" health log counts passes over the probes\n");
  const std::string diag_cpp = read_file(CANARY_DIAG_CPP);
  const size_t at = diag_cpp.find("uint8_t diag_run_selftest(void) {");
  const size_t end = at == std::string::npos ? at : diag_cpp.find("\n}\n", at);
  const std::string body = end == std::string::npos ? "" : diag_cpp.substr(at, end - at);
  CHECK(!body.empty(), "securacv_diagnostics.cpp read; diag_run_selftest found");
  const std::string verdict = health_log_rule(body);
  if (!verdict.empty()) std::printf("  %s\n", verdict.c_str());
  CHECK(verdict.empty(), "its log line reads x/<probes>, the count selftest_run() returned");

  struct Mutation { const char* name; const char* from; const char* to; };
  const Mutation muts[] = {
    {"over the rows again (x/10)", "(unsigned)s_selftest.passed_count, (unsigned)probes,",
     "(unsigned)s_selftest.passed_count, (unsigned)SELFTEST_COUNT,"},
    {"over the report's rows", "(unsigned)s_selftest.passed_count, (unsigned)probes,",
     "(unsigned)s_selftest.passed_count, (unsigned)s_selftest.total_count,"},
    {"the runner's count dropped", "const uint8_t probes = selftest_run(", "(void)selftest_run("},
  };
  int caught = 0;
  int applied = 0;
  for (const Mutation& m : muts) {
    std::string mutated = body;
    const size_t p = mutated.find(m.from);
    if (p == std::string::npos) {
      std::printf("  mutation \"%s\" does not apply\n", m.name);
      continue;
    }
    ++applied;
    mutated.replace(p, std::strlen(m.from), m.to);
    if (!health_log_rule(mutated).empty()) ++caught;
    else std::printf("  mutation \"%s\" passes the rule\n", m.name);
  }
  const int n_muts = (int)(sizeof(muts) / sizeof(muts[0]));
  CHECK(applied == n_muts && caught == n_muts, "and each of three mutations of it fails the rule");
}

/* ── The handler ───────────────────────────────────────────────────────── */

static std::string handler_body(const std::string& src) {
  const std::string open = "static esp_err_t handle_diagnostics(httpd_req_t* req) {";
  const size_t at = src.find(open);
  if (at == std::string::npos) return "";
  const size_t end = src.find("\n}\n", at);
  return end == std::string::npos ? "" : src.substr(at, end - at);
}

/* "" when the handler builds its body the way this suite holds it, else
 * what is wrong. Whitespace is squashed first, so the rule reads the code,
 * not its layout. */
static std::string handler_rule(const std::string& raw) {
  std::string h;
  for (char c : raw) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
    h += c;
  }
  if (h.empty()) return "no handle_diagnostics";
  if (h.find("charegress[kCsiEventEgressDiagnosticsMax];") == std::string::npos)
    return "the egress buffer is not kCsiEventEgressDiagnosticsMax";
  if (h.find("csi_event_egress_diagnostics_json(egress,sizeof(egress))") == std::string::npos)
    return "the egress object is not read through csi_event_egress_diagnostics_json()";
  if (h.find("charbuf[diagnostics_json::kJsonMax];") == std::string::npos)
    return "the body buffer is not diagnostics_json::kJsonMax";
  if (h.find("diagnostics_json::build(snap,FIRMWARE_VERSION,egress,buf,sizeof(buf))==0") ==
      std::string::npos)
    return "the body is not built by diagnostics_json::build() (or its refusal is not answered)";
  if (h.find("returnhttp_send_json(req,buf);") == std::string::npos)
    return "the built body is not what it sends";
  if (h.find("printf(") != std::string::npos || h.find("csi_event_egress_stats") != std::string::npos)
    return "it spells a body, or reads the egress, itself";
  return "";
}

static void test_the_handler_builds_with_the_builder() {
  std::printf("-- F179: GET /api/diagnostics builds its body here and reads the published copy\n");
  const std::string src = read_file(CANARY_NETWORK_CPP);
  const std::string h = handler_body(src);
  CHECK(!h.empty(), "securacv_network.cpp read; handle_diagnostics found");
  const std::string verdict = handler_rule(h);
  if (!verdict.empty()) std::printf("  %s\n", verdict.c_str());
  CHECK(verdict.empty(), "the handler holds to the builder, its buffers and the published copy");

  struct Mutation { const char* name; const char* from; const char* to; };
  const Mutation muts[] = {
    {"a smaller body buffer", "char buf[diagnostics_json::kJsonMax];", "char buf[512];"},
    {"a smaller egress buffer", "char egress[kCsiEventEgressDiagnosticsMax];", "char egress[64];"},
    {"no egress read", "(void)csi_event_egress_diagnostics_json(egress, sizeof(egress));",
     "egress[0] = '\\0';"},
    {"the firmware version dropped", "FIRMWARE_VERSION, egress", "\"\", egress"},
    {"the egress object dropped", "FIRMWARE_VERSION, egress, buf", "FIRMWARE_VERSION, nullptr, buf"},
    {"the refusal unanswered", ") == 0) {\n    return http_send_error(req, 500, \"diagnostics_too_large\");\n  }",
     ");"},
    {"the old snprintf chain back", "return http_send_json(req, buf);",
     "pos += snprintf(buf + pos, sizeof(buf) - pos, \"}\"); return http_send_json(req, buf);"},
  };
  int caught = 0;
  int applied = 0;
  for (const Mutation& m : muts) {
    std::string mutated = h;
    const size_t at = mutated.find(m.from);
    if (at == std::string::npos) {
      std::printf("  mutation \"%s\" does not apply\n", m.name);
      continue;
    }
    ++applied;
    mutated.replace(at, std::strlen(m.from), m.to);
    if (!handler_rule(mutated).empty()) ++caught;
    else std::printf("  mutation \"%s\" passes the rule\n", m.name);
  }
  const int n_muts = (int)(sizeof(muts) / sizeof(muts[0]));
  CHECK(applied == n_muts && caught == n_muts, "and each of seven mutations of it fails the rule");
}

int main() {
  test_the_counters_spell_the_healths_names();
  test_the_body_keeps_the_routes_keys_and_adds_the_counters();
  test_the_widest_body_fits_the_routes_buffer();
  test_a_row_that_is_not_a_probe_says_so();
  test_the_runner_scores_the_probes_and_calls_no_unprobed_row();
  test_the_selftest_route_says_not_a_probe_too();
  test_the_selftest_handler_builds_with_the_builder();
  test_the_console_counts_over_the_probes();
  test_the_health_log_counts_over_the_probes();
  test_the_handler_builds_with_the_builder();
  if (g_fail) {
    std::printf("test_canary_diagnostics: %d of %d checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  std::printf("test_canary_diagnostics: %d checks passed\n", g_checks);
  return 0;
}
