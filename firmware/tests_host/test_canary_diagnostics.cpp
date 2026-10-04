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
  s.selftest.tests[0] = {"nvs_rw", true, 4};
  s.selftest.tests[1] = {"sd_card", false, 120};
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
  for (int i = 0; i < SELFTEST_COUNT; ++i) s.selftest.tests[i] = {"t", true, (uint16_t)i};
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
  const std::regex row_re("\\{\\s*\"([a-z0-9_]+)\"\\s*,\\s*test_\\w+\\s*\\}");
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
  for (int i = 0; i < SELFTEST_COUNT; ++i) s.selftest.tests[i] = {name.c_str(), false, 0xFFFF};
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
  test_the_handler_builds_with_the_builder();
  if (g_fail) {
    std::printf("test_canary_diagnostics: %d of %d checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  std::printf("test_canary_diagnostics: %d checks passed\n", g_checks);
  return 0;
}
