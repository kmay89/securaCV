/* Source guard for canary-wap's NvsManager sessions, second edition (sweep
 * F60; the first edition was F53's).
 *
 * Why it exists. NvsManager (arduino/canary_wap/nvs_store.h) holds a
 * recursive FreeRTOS mutex from begin() to the matching end(), so the five
 * tasks that use the handle cannot close it under each other. The price is
 * that a session which never ends keeps the lock on its task for good:
 * every other task's begin() then waits 2 s and fails, until a reboot.
 *
 * The first edition scanned the sketch for a block that opens a session and
 * does not close it — brace matching over stripped source. It was honest
 * about its gaps: a session ended in only one branch, or left by a `break`
 * or `goto`, got past it, and so did a session opened through a pointer or
 * a second name. F60 closes those gaps at the language level instead: every
 * main-namespace session in sketch code is an RAII NvsMainSession
 * (nvs_store.h), whose destructor is the end() on every path the compiler
 * can walk — branches, breaks and early returns included
 * (test_nvs_store_lock.cpp runs the guard's scenarios on the real header).
 *
 * What is left for a textual guard is the rule that makes the RAII sound,
 * and unlike balance it IS textually decidable: outside nvs_store.h, sketch
 * code never names NvsManager at all. A session you cannot name is a
 * session you cannot open unguarded, let alone leak. So, for every source
 * file in the sketch directory except nvs_store.h, comment- and
 * literal-stripped:
 *
 *   1. The token `NvsManager` does not appear.
 *   2. Neither do the retired wrappers nvs_open_rw / nvs_open_ro (they were
 *      unguarded doors to the same singleton; the sketch's nvs_close()-
 *      with-no-arguments wrapper is gone too, and securacv_ota.cpp's
 *      nvs_close(handle) calls are ESP-IDF's own C API, which takes an
 *      argument — the scan requires `nvs_close()` with none).
 *
 * And in nvs_store.h itself: the guard still exists, constructs its open
 * state from begin() and ends it in its destructor — so the header cannot
 * silently lose the guard while the rule above keeps passing.
 *
 * Build & run: make -C firmware/projects/canary-wap/tests_host
 */
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

// ── stripping and token search (unchanged from the first edition) ───────────

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

// ── the rule ─────────────────────────────────────────────────────────────────

// Findings for one sketch source (already stripped by the caller's use of
// scan(): every naming of the singleton or a retired wrapper, by line.
static std::vector<std::string> scan(const std::string& file, const std::string& src) {
  const std::string code = strip(src);
  std::vector<std::string> findings;
  for (size_t p : find_token(code, "NvsManager")) {
    findings.push_back(file + ":" + std::to_string(line_of(code, p)) +
                       ": names NvsManager — sketch code opens sessions through "
                       "NvsMainSession (nvs_store.h) only");
  }
  for (const char* tok : {"nvs_open_rw", "nvs_open_ro"}) {
    for (size_t p : find_token(code, tok)) {
      findings.push_back(file + ":" + std::to_string(line_of(code, p)) +
                         ": calls the retired wrapper " + tok + "()");
    }
  }
  // The no-argument nvs_close() wrapper is retired; nvs_close(handle) is
  // ESP-IDF's own API and stays.
  for (size_t p : find_token(code, "nvs_close")) {
    size_t q = p + std::string("nvs_close").size();
    while (q < code.size() && (code[q] == ' ' || code[q] == '\t')) q++;
    if (q < code.size() && code[q] == '(') {
      size_t r = q + 1;
      while (r < code.size() && (code[r] == ' ' || code[r] == '\t')) r++;
      if (r < code.size() && code[r] == ')') {
        findings.push_back(file + ":" + std::to_string(line_of(code, p)) +
                           ": calls the retired wrapper nvs_close()");
      }
    }
  }
  return findings;
}

// ── the scanner bites: fixtures ──────────────────────────────────────────────

static void test_naming_the_singleton_fails() {
  const auto f = scan("x.cpp", R"SRC(
void a() {
  NvsManager& nvs = NvsManager::instance();
  if (!nvs.beginReadWrite()) return;
  nvs.end();
}
void b() { nvs_open_rw(); nvs_close(); }
)SRC");
  // Two namings on line 3, one wrapper open and one no-arg close on line 7 —
  // even a block that would have passed the old balance scan fails now.
  CHECK(f.size() == 4);
  int named = 0, wrappers = 0;
  for (const std::string& s : f) {
    named += s.find("names NvsManager") != std::string::npos;
    wrappers += s.find("retired wrapper") != std::string::npos;
  }
  CHECK(named == 2 && wrappers == 2);
}

static void test_comments_literals_and_idf_close_do_not_count() {
  const auto f = scan("y.cpp", R"SRC(
// NvsManager::instance() in a comment is prose, not a session
/* nvs_open_rw() */
static const char* kDoc = "NvsManager belongs to nvs_store.h";
void c(nvs_handle_t handle) {
  nvs_close(handle);  // ESP-IDF's own close, takes the handle
}
void d() {
  NvsMainSession nvs(false);
  if (nvs.isOpen()) nvs->putBool("k", true);
}
)SRC");
  for (const std::string& s : f) std::printf("  unexpected: %s\n", s.c_str());
  CHECK(f.empty());
}

// ── the sketch ───────────────────────────────────────────────────────────────

static void test_no_sketch_source_names_the_singleton() {
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

  int scanned = 0;
  std::vector<std::string> findings;
  for (const fs::path& p : files) {
    const std::string name = p.filename().string();
    if (name == "nvs_store.h") continue;  // the one home of the singleton
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    for (std::string& f : scan(name, ss.str())) findings.push_back(std::move(f));
    scanned++;
  }
  for (const std::string& f : findings) std::printf("  %s\n", f.c_str());
  CHECK(findings.empty());
  std::printf("  %d sketch sources name no NvsManager outside nvs_store.h\n", scanned);

  // The guard the rule leans on is still there, still RAII: its open state
  // is begin()'s answer and its destructor is the end(). A header that lost
  // either would leave the rule above guarding nothing.
  std::ifstream in(std::string(CANARY_WAP_SKETCH_DIR) + "/nvs_store.h");
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string store = strip(ss.str());
  CHECK(store.find("class NvsMainSession") != std::string::npos);
  CHECK(store.find("m_open(m_nvs.begin(readOnly))") != std::string::npos);
  CHECK(store.find("if (m_open) m_nvs.end();") != std::string::npos);
  // ...and the retired wrappers stayed retired.
  CHECK(find_token(store, "nvs_open_rw").empty());
  CHECK(find_token(store, "nvs_open_ro").empty());
}

int main() {
  test_naming_the_singleton_fails();
  test_comments_literals_and_idf_close_do_not_count();
  test_no_sketch_source_names_the_singleton();

  if (g_failures == 0) { std::printf("ALL nvs-session-balance tests PASSED\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
