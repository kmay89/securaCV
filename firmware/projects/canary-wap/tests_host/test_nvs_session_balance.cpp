/* Source guard for canary-wap's NvsManager sessions (sweep F53): every block
 * in the sketch that opens a session on the shared settings handle closes it.
 *
 * Why it exists. NvsManager (arduino/canary_wap/nvs_store.h) holds a
 * recursive FreeRTOS mutex from begin() to the matching end(), so the five
 * tasks that use the handle cannot close it under each other. The price is
 * that a session which never ends keeps the lock on its task for good: every
 * other task's begin() then waits 2 s and fails, until a reboot. The vault
 * (vault_snapshot.cpp) opened five sessions and closed none — harmless under
 * the old bare open flag, a lockout under the lock — and nothing on the host
 * could have said so, because the sketch's glue compiles only in CI's
 * firmware legs. This reads the sketch instead.
 *
 * The rules, for every source file in the sketch directory:
 *
 *   1. NvsManager::instance() is bound to a reference
 *      (`NvsManager& nvs = NvsManager::instance();`), so the scan can follow
 *      the session. Only nvs_store.h's own legacy wrappers (nvs_open_rw() /
 *      nvs_open_ro() / nvs_close()) call it unbound, and their callers are
 *      held to rule 2 with nvs_close() as the end.
 *   2. The innermost block holding that binding, if it opens a session
 *      (nvs.begin / beginReadOnly / beginReadWrite), also calls nvs.end().
 *   3. Every `return` in that block after the statement that opens the
 *      session and before the block's last nvs.end() comes straight after an
 *      nvs.end(); (`{ nvs.end(); return false; }`). The return in the opening
 *      statement itself (`if (!nvs.beginReadOnly()) return false;`) is the
 *      path where begin() failed, which owes no end().
 *
 * What it is: a textual guard over comment- and literal-stripped source,
 * with brace matching. What it is not: a proof of every path. A session
 * ended in one branch and left open in another, or a `break`/`goto` out of a
 * session, gets past it; so does a session opened through a pointer or a
 * second name. Review and the bench rows in
 * docs/audit/hardware_verification_checklist.md cover those. The scanner's
 * own self-tests below include the vault's pre-fix shapes, so it is shown to
 * bite on the bug it was written for.
 *
 * Build & run: make -C firmware/projects/canary-wap/tests_host
 */
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

// ── the scanner ─────────────────────────────────────────────────────────────

static bool ident_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Blank comments and the insides of string, character and raw-string
// literals (a JSON body in a string holds braces), keeping every newline and
// every offset, so positions still map to source lines.
static std::string strip(const std::string& s) {
  std::string o = s;
  const size_t n = s.size();
  auto blank = [&o](size_t from, size_t to) {
    for (size_t k = from; k < to && k < o.size(); ++k) {
      if (o[k] != '\n') o[k] = ' ';
    }
  };
  size_t i = 0;
  while (i < n) {
    const char c = s[i];
    if (c == '/' && i + 1 < n && s[i + 1] == '/') {
      size_t j = s.find('\n', i);
      if (j == std::string::npos) j = n;
      blank(i, j);
      i = j;
    } else if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      size_t j = s.find("*/", i + 2);
      j = (j == std::string::npos) ? n : j + 2;
      blank(i, j);
      i = j;
    } else if (c == 'R' && i + 1 < n && s[i + 1] == '"' && (i == 0 || !ident_char(s[i - 1]) ||
                                                            s[i - 1] == '8' || s[i - 1] == 'u' ||
                                                            s[i - 1] == 'U' || s[i - 1] == 'L')) {
      const size_t paren = s.find('(', i + 2);
      if (paren == std::string::npos) { i++; continue; }
      const std::string close = ")" + s.substr(i + 2, paren - (i + 2)) + "\"";
      size_t j = s.find(close, paren + 1);
      j = (j == std::string::npos) ? n : j + close.size();
      blank(i, j);
      i = j;
    } else if (c == '"' || (c == '\'' && (i == 0 || !ident_char(s[i - 1])))) {
      size_t j = i + 1;
      while (j < n && s[j] != c) {
        if (s[j] == '\\') j++;
        if (j < n && s[j] == '\n') break;  // unterminated: stop at the line
        j++;
      }
      blank(i + 1, j);
      i = (j < n) ? j + 1 : n;
    } else {
      i++;
    }
  }
  return o;
}

static int line_of(const std::string& s, size_t pos) {
  return 1 + (int)std::count(s.begin(), s.begin() + (long)std::min(pos, s.size()), '\n');
}

// Every position of `tok` in `code` that starts at an identifier boundary.
static std::vector<size_t> find_token(const std::string& code, const std::string& tok) {
  std::vector<size_t> out;
  for (size_t p = code.find(tok); p != std::string::npos; p = code.find(tok, p + 1)) {
    if (p > 0 && ident_char(code[p - 1])) continue;
    out.push_back(p);
  }
  return out;
}

// The innermost {...} around pos: [open, close], or false if unbalanced.
static bool enclosing_block(const std::string& code, size_t pos, size_t* open, size_t* close) {
  int depth = 0;
  size_t i = pos;
  bool found = false;
  while (i > 0) {
    --i;
    if (code[i] == '}') depth++;
    else if (code[i] == '{') {
      if (depth == 0) { found = true; break; }
      depth--;
    }
  }
  if (!found) return false;
  *open = i;
  depth = 0;
  for (size_t j = i; j < code.size(); ++j) {
    if (code[j] == '{') depth++;
    else if (code[j] == '}' && --depth == 0) { *close = j; return true; }
  }
  return false;
}

static std::string squeeze(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') o += c;
  }
  return o;
}

struct Scan {
  std::vector<std::string> findings;  // "file:line: what"
  int sessions = 0;                   // blocks that open a session, checked
};

// Rules 2 and 3 for one session: `opens` are where it opens (inside
// [open, close]), `end_call` is what closes it ("nvs.end()").
static void check_session(const std::string& file, const std::string& code, size_t open,
                          size_t close, const std::vector<size_t>& opens,
                          const std::string& end_call, Scan* out) {
  if (opens.empty()) return;
  out->sessions++;
  const size_t first_open = *std::min_element(opens.begin(), opens.end());
  const std::string what = end_call.substr(0, end_call.find('('));
  size_t last_end = std::string::npos;
  for (size_t p : find_token(code.substr(0, close), end_call)) {
    if (p > first_open) last_end = p;
  }
  if (last_end == std::string::npos) {
    out->findings.push_back(file + ":" + std::to_string(line_of(code, first_open)) +
                            ": opens a session and never calls " + end_call +
                            " in its block (the lock stays on this task)");
    return;
  }
  // The statement that opens it runs to its first ';' or '{'; a return in it
  // is the failed-begin path.
  const size_t stmt_end = code.find_first_of(";{", first_open);
  for (size_t r : find_token(code, "return")) {
    if (r <= stmt_end || r >= last_end || r < open || r > close) continue;
    if (r + 6 < code.size() && ident_char(code[r + 6])) continue;
    std::string before = code.substr(open, r - open);
    while (!before.empty() && (before.back() == ' ' || before.back() == '\t' ||
                               before.back() == '\n' || before.back() == '\r')) {
      before.pop_back();
    }
    if (!before.empty() && before.back() == '{') before.pop_back();
    const std::string tail = squeeze(before.size() > 96 ? before.substr(before.size() - 96) : before);
    const std::string want = squeeze(end_call) + ";";
    if (tail.size() < want.size() || tail.compare(tail.size() - want.size(), want.size(), want) != 0) {
      out->findings.push_back(file + ":" + std::to_string(line_of(code, r)) +
                              ": returns inside an open session without " + what + "()");
    }
  }
}

static void scan_source(const std::string& file, const std::string& src, Scan* out) {
  const std::string code = strip(src);
  const bool is_store = file == "nvs_store.h";

  // Rules 1-3: NvsManager::instance(), bound or (in nvs_store.h) wrapped.
  const std::string inst = "NvsManager::instance()";
  for (size_t p : find_token(code, inst)) {
    size_t q = p + inst.size();
    while (q < code.size() && (code[q] == ' ' || code[q] == '\t' || code[q] == '\n')) q++;
    if (is_store && q < code.size() && code[q] == '.') continue;  // the legacy wrappers
    // Walk back over `NvsManager& <id> =`.
    size_t b = p;
    auto skip_ws_back = [&code](size_t x) {
      while (x > 0 && (code[x - 1] == ' ' || code[x - 1] == '\t' || code[x - 1] == '\n')) x--;
      return x;
    };
    b = skip_ws_back(b);
    std::string id;
    bool bound = q < code.size() && code[q] == ';' && b > 0 && code[b - 1] == '=';
    if (bound) {
      b = skip_ws_back(b - 1);
      size_t e = b;
      while (b > 0 && ident_char(code[b - 1])) b--;
      id = code.substr(b, e - b);
      b = skip_ws_back(b);
      bound = !id.empty() && b > 0 && code[b - 1] == '&';
      if (bound) {
        b = skip_ws_back(b - 1);
        const std::string ty = "NvsManager";
        bound = b >= ty.size() && code.compare(b - ty.size(), ty.size(), ty) == 0 &&
                (b == ty.size() || !ident_char(code[b - ty.size() - 1]));
      }
    }
    if (!bound) {
      out->findings.push_back(file + ":" + std::to_string(line_of(code, p)) +
                              ": NvsManager::instance() is not bound as "
                              "`NvsManager& nvs = NvsManager::instance();`, so its session "
                              "cannot be followed");
      continue;
    }
    size_t open = 0, close = 0;
    if (!enclosing_block(code, p, &open, &close)) {
      out->findings.push_back(file + ":" + std::to_string(line_of(code, p)) +
                              ": no enclosing block (unbalanced braces?)");
      continue;
    }
    std::vector<size_t> opens;
    for (const char* m : {".begin(", ".beginReadOnly(", ".beginReadWrite("}) {
      for (size_t o : find_token(code.substr(0, close), id + m)) {
        if (o > p) opens.push_back(o);
      }
    }
    check_session(file, code, open, close, opens, id + ".end()", out);
  }

  // The legacy wrappers' callers, with nvs_close() as the end.
  if (!is_store) {
    for (const char* call : {"nvs_open_rw(", "nvs_open_ro("}) {
      for (size_t p : find_token(code, call)) {
        size_t open = 0, close = 0;
        if (!enclosing_block(code, p, &open, &close)) {
          out->findings.push_back(file + ":" + std::to_string(line_of(code, p)) +
                                  ": no enclosing block (unbalanced braces?)");
          continue;
        }
        check_session(file, code, open, close, {p}, "nvs_close()", out);
      }
    }
  }
}

// ── the scanner bites: its own fixtures ─────────────────────────────────────

static Scan scan_one(const std::string& file, const std::string& src) {
  Scan s;
  scan_source(file, src, &s);
  return s;
}

// A fixture's findings are printed only when their count is not the one the
// test expects, so a passing run stays quiet.
static void dump_unless(const Scan& s, size_t want) {
  if (s.findings.size() == want) return;
  for (const std::string& f : s.findings) std::printf("  finding: %s\n", f.c_str());
}

static void test_balanced_shapes_pass() {
  // The shapes the sketch uses today, each clean.
  const Scan s = scan_one("ok.cpp", R"SRC(
static bool load(uint8_t* out) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly()) return false;
  size_t n = nvs.getBytesLength("k");
  if (n != 32) { nvs.end(); return false; }
  nvs.getBytes("k", out, 32);
  nvs.end();
  return true;
}
static esp_err_t handler(httpd_req_t* req) {
  {
    NvsManager& nvs = NvsManager::instance();
    if (nvs.beginReadWrite()) {
      nvs.putBool("wifi_en", true);
      nvs.end();
    }
  }
  return http_send_json(req, "{\"ok\":true}");
}
void init() {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly() && !nvs.beginReadWrite()) return;
  cfg = nvs.getBool("t3", false);
  nvs.end();
}
bool set_key(const uint8_t* pub) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;
  const bool stored = nvs.putBytes("vault_pub", pub, 32) == 32;
  nvs.end();
  if (!stored) return false;
  return true;
}
static bool tls_load() {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadOnly()) return false;
  if (a == 0) {
    nvs.end();
    return false;
  }
  nvs.end();
  return true;
}
void legacy() {
  if (!nvs_open_rw()) return;
  put();
  nvs_close();
}
)SRC");
  dump_unless(s, 0);
  CHECK(s.findings.empty());
  CHECK(s.sessions == 6);
}

static void test_the_vaults_prefix_shapes_fail() {
  // vault_snapshot.cpp before F53: persist_config() and request_capture()
  // never ended; set_pubkey_hex() returned inside its session on a failed
  // write; clear_pubkey() left its session open.
  const Scan s = scan_one("vault_snapshot.cpp", R"SRC(
static void persist_config() {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return;
  nvs.putBool("vault_t3", g_cfg.t3_enabled);
}
Decision request_capture(Trigger t) {
  NvsManager& nvs = NvsManager::instance();
  uint32_t seq = 1;
  if (nvs.beginReadWrite()) {
    seq = nvs.getUInt("vault_seq", 0) + 1;
    nvs.putUInt("vault_seq", seq);
  }
  return Decision::CAPTURE;
}
bool set_pubkey_hex(const char* hex64) {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return false;
  if (nvs.putBytes("vault_pub", pub, sizeof(pub)) != sizeof(pub)) return false;
  nvs.end();
  return true;
}
void clear_pubkey() {
  NvsManager& nvs = NvsManager::instance();
  if (nvs.beginReadWrite()) {
    nvs.remove("vault_pub");
  }
  persist_config();
}
)SRC");
  dump_unless(s, 4);
  CHECK(s.findings.size() == 4);
  CHECK(s.sessions == 4);
  auto has = [&s](const std::string& needle) {
    for (const std::string& f : s.findings) {
      if (f.find(needle) != std::string::npos) return true;
    }
    return false;
  };
  CHECK(has("vault_snapshot.cpp:4: opens a session and never calls nvs.end()"));
  CHECK(has("vault_snapshot.cpp:10: opens a session and never calls nvs.end()"));
  CHECK(has("vault_snapshot.cpp:19: returns inside an open session without nvs.end()"));
  CHECK(has("vault_snapshot.cpp:25: opens a session and never calls nvs.end()"));
}

static void test_other_leaks_fail() {
  // An unbound instance() the scan cannot follow; a legacy open with no
  // close; a return between two ends that skips the first.
  const Scan s = scan_one("x.cpp", R"SRC(
void a() {
  NvsManager::instance().beginReadWrite();
  put();
}
void b() {
  if (!nvs_open_ro()) return;
  get();
}
bool c() {
  NvsManager& store = NvsManager::instance();
  if (!store.begin(true)) return false;
  if (bad) return false;
  store.end();
  return true;
}
)SRC");
  dump_unless(s, 3);
  CHECK(s.findings.size() == 3);
  bool unbound = false, legacy = false, early = false;
  for (const std::string& f : s.findings) {
    unbound |= f.find("x.cpp:3: NvsManager::instance() is not bound") == 0;
    legacy |= f.find("x.cpp:7: opens a session and never calls nvs_close()") == 0;
    early |= f.find("x.cpp:13: returns inside an open session without store.end()") == 0;
  }
  CHECK(unbound && legacy && early);
}

static void test_comments_and_literals_do_not_count() {
  // A session named in a comment, a string or a raw string is not a session;
  // braces inside them do not move the block; an end() in a comment does not
  // close one.
  const Scan s = scan_one("y.cpp", R"SRC(
// NvsManager& nvs = NvsManager::instance(); nvs.beginReadWrite();
/* NvsManager::instance().begin(false); */
static const char* kPage = R"HTML(<p>{ NvsManager::instance() }</p>)HTML";
static const char* kJson = "{\"a\":{}} NvsManager::instance()";
static const char kBrace = '{';
void d() {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return;
  put("}");
  // nvs.end();
}
)SRC");
  dump_unless(s, 1);
  CHECK(s.sessions == 1);
  CHECK(s.findings.size() == 1);
  CHECK(!s.findings.empty() && s.findings[0].find("y.cpp:9: opens a session and never calls") == 0);
}

// ── the sketch ──────────────────────────────────────────────────────────────

static void test_the_sketch_closes_every_session() {
  namespace fs = std::filesystem;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(CANARY_WAP_SKETCH_DIR)) {
    const std::string ext = e.path().extension().string();
    if (e.is_regular_file() && (ext == ".ino" || ext == ".cpp" || ext == ".h" || ext == ".c")) {
      files.push_back(e.path());
    }
  }
  std::sort(files.begin(), files.end());
  CHECK(files.size() > 100);  // the directory really is the sketch

  Scan all;
  std::map<std::string, int> per_file;
  for (const fs::path& p : files) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    const int before = all.sessions;
    scan_source(p.filename().string(), ss.str(), &all);
    if (all.sessions > before) per_file[p.filename().string()] = all.sessions - before;
  }
  for (const std::string& f : all.findings) std::printf("  %s\n", f.c_str());
  CHECK(all.findings.empty());

  // The scan reached the sessions the sweep item names, so a scanner that
  // silently matched nothing cannot pass.
  CHECK(per_file["canary_wap.ino"] >= 10);
  CHECK(per_file["vault_snapshot.cpp"] >= 5);
  CHECK(per_file["bluetooth_channel.cpp"] >= 4);
  CHECK(per_file["nvs_store.h"] >= 4);
  std::printf("  %d NvsManager sessions checked in %zu files:", all.sessions, per_file.size());
  for (const auto& kv : per_file) std::printf(" %s=%d", kv.first.c_str(), kv.second);
  std::printf("\n");
}

int main() {
  test_balanced_shapes_pass();
  test_the_vaults_prefix_shapes_fail();
  test_other_leaks_fail();
  test_comments_and_literals_do_not_count();
  test_the_sketch_closes_every_session();

  if (g_failures == 0) { std::printf("ALL nvs-session-balance tests PASSED\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
