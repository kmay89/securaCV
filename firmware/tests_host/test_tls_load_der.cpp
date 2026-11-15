/* Host tests for the flagship's TLS certificate load (repo sweep F421), run
 * on the firmware's own code: tls_load_der_from_nvs() and
 * tls_store_der_to_nvs(), cut out of
 * firmware/canary/lib/securacv_network/src/securacv_network.cpp, and the
 * NvsManager session and blob calls and secure_zero() under them, cut out of
 * firmware/canary/lib/securacv_crypto/src/securacv_crypto.cpp, all verbatim
 * by cut_functions.awk (the Makefile writes build/tls_load_der.inc), over
 * the fake NVS test_nvs_load_key.cpp uses (stubs/nvs_key: Preferences and
 * IDF's nvs_open() / nvs_get_blob() read one store) and stubs/nvs_manager's
 * FreeRTOS lock.
 *
 * THE FAILURE THIS PREVENTS. The load answered one bool. A session that did
 * not open, one entry stored without the other, a failed malloc and a
 * getBytes() that answered fewer bytes than its lookup all answered false,
 * and initTls() read every false as "no certificate": on a build that can
 * generate one (SECURACV_HAS_TLS_CERTGEN) it made a new pair and stored it
 * over the one whose SHA-256 the phones pin. The canary-wap had the same
 * shape until F404.
 *
 * Now the load answers Loaded, Absent (IDF's NVS says neither entry is
 * stored) or Refused (one line saying why, nothing handed back, both
 * buffers zeroed before they are freed), and initTls() generates only on
 * Absent (a source pin below: initTls() itself needs the whole network
 * stack). Every refusal leaves NVS byte for byte as it was.
 *
 * Every answer the fake can give, and where it goes:
 *   nothing stored (no namespace, or neither key in it)   Absent (generate)
 *   both stored, every read whole                         Loaded (serve)
 *   Read::Fails / Garbles / Short                          Refused
 *   the certificate alone, the key alone                   Refused
 *   an entry stored with 0 bytes                           Refused
 *   the Preferences session refuses to open                Refused
 *   IDF's open fails with a fault (not NOT_FOUND)          Refused
 *   IDF's length lookup fails (any answer but OK and
 *     NOT_FOUND, ESP_ERR_NVS_INVALID_LENGTH among them)    Refused
 *   no memory for either buffer                            Refused
 * So only true absence reaches the generate-and-store path.
 *
 * It also reads the canary's sources (argv): initTls() calls the load once
 * and tls_generate_self_signed() and tls_store_der_to_nvs() only in the
 * branch Absent reaches, after the Refused branch has returned; and no file
 * but securacv_network.cpp's load and store names the two NVS entries.
 *
 * What this does not prove: ESP-IDF's NVS or Arduino's Preferences
 * themselves (the fakes answer as their 2.0.17 sources read), mbedTLS's
 * certificate generation, or anything on a device. Certificate generation
 * is compiled out on every stock unit until F15. The canary envs compile the
 * real thing in CI; it has not run on a bench.
 *
 * Build & run: make -C firmware/tests_host (the cut needs the Makefile).
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Arduino.h"          // stubs/nvs_key: a Serial that keeps every line
#include "nvs.h"              // stubs/nvs_key: IDF's NVS over the Preferences fake's store
#include "securacv_crypto.h"  // the real KeyLoad, NvsManager and secure_zero()
#include "canary_config.h"    // NVS_MAIN_NS, NVS_KEY_TLS_CERT, NVS_KEY_TLS_KEY

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    g_checks++;                                                          \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

// NvsManager's constructor is private and instance() is its only way in; the
// firmware's is a one-line static local, and so is this one.
NvsManager& NvsManager::instance() {
  static NvsManager s_instance;
  return s_instance;
}

// ── the heap the cut load allocates from ───────────────────────────────────
//
// malloc() and free() inside the cut code reach these (the macros below are
// in force only over the .inc): every block is tracked, a test can make the
// Nth allocation fail, and each free records whether the block was all zero
// at that moment, which is how a refusal's zeroing is seen.
struct HostBlock {
  void* p;
  size_t n;
};
static std::vector<HostBlock> g_live;
static std::vector<HostBlock> g_freed_zeroed;   // freed blocks that were all zero
static std::vector<HostBlock> g_freed_dirty;    // freed blocks that were not
static int g_allocs = 0;
static int g_fail_alloc_at = 0;                 // 1-based; 0 never fails

static void* host_malloc(size_t n) {
  g_allocs++;
  if (g_fail_alloc_at != 0 && g_allocs == g_fail_alloc_at) return nullptr;
  void* p = std::malloc(n == 0 ? 1 : n);
  if (p) {
    std::memset(p, 0xCD, n);   // a fresh block is never zero by luck
    g_live.push_back({p, n});
  }
  return p;
}
static void host_free(void* p) {
  if (p == nullptr) return;
  for (size_t i = 0; i < g_live.size(); i++) {
    if (g_live[i].p != p) continue;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    bool zero = true;
    for (size_t j = 0; j < g_live[i].n; j++) zero = zero && b[j] == 0;
    (zero ? g_freed_zeroed : g_freed_dirty).push_back(g_live[i]);
    g_live.erase(g_live.begin() + (long)i);
    std::free(p);
    return;
  }
  std::printf("FAIL: free of a block the cut code did not allocate\n");
  g_failures++;
}

#define malloc host_malloc
#define free host_free
#include "tls_load_der.inc"
#undef malloc
#undef free

// ── the world each test starts from ─────────────────────────────────────────

static std::vector<uint8_t> bytes(size_t n, uint8_t seed) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) v[i] = (uint8_t)(seed + i * 13);
  return v;
}
static const std::vector<uint8_t> kCert = bytes(447, 0x30);   // a P-256 certificate's size
static const std::vector<uint8_t> kKey = bytes(121, 0x77);    // a P-256 key DER's size

// The namespace setup() reaches initTls() with: witness_provision_device()
// has stored the identity key in it by then (it halts the boot otherwise).
static void fresh_world() {
  host_key_nvs() = HostKeyNvs();
  host_key_nvs().ns = NVS_MAIN_NS;
  host_key_nvs().ns_exists = true;
  host_key_nvs().blobs["privkey"] = bytes(32, 0x11);
  Serial.lines.clear();
  g_live.clear();
  g_freed_zeroed.clear();
  g_freed_dirty.clear();
  g_allocs = 0;
  g_fail_alloc_at = 0;
}
static void store_pair(const std::vector<uint8_t>& cert, const std::vector<uint8_t>& key) {
  host_key_nvs().blobs[NVS_KEY_TLS_CERT] = cert;
  host_key_nvs().blobs[NVS_KEY_TLS_KEY] = key;
}

static bool printed(const char* needle) {
  for (const std::string& l : Serial.lines)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}

// After every call: no NVS session, the lock free, no handle left open,
// nothing written through IDF, and nothing the fakes count as a fault.
static void check_idle_and_clean() {
  const HostKeyNvs& n = host_key_nvs();
  CHECK(!NvsManager::instance().isOpen());
  CHECK(g_host_mutexes[0].count == 0 && g_host_mutexes[0].owner == -1);
  CHECK(g_host_mutexes[0].bad_gives == 0);
  CHECK(n.prefs_open == 0);
  CHECK(n.idf_open_handles == 0 && n.idf_opens >= n.idf_closes);
  CHECK(n.idf_rw_opens == 0);
  CHECK(n.double_begins == 0);
  CHECK(n.wrong_namespace == 0);
}

// One load, called as initTls() calls it but with the out-parameters dirty,
// so a load that leaves them as they were is caught.
struct LoadResult {
  KeyLoad answer;
  uint8_t* cert;
  size_t cert_len;
  uint8_t* key;
  size_t key_len;
};
static uint8_t g_dirty_cert[4] = {1, 2, 3, 4};
static uint8_t g_dirty_key[4] = {5, 6, 7, 8};
static LoadResult load() {
  LoadResult r{KeyLoad::Loaded, g_dirty_cert, 999, g_dirty_key, 999};
  r.answer = tls_load_der_from_nvs(&r.cert, &r.cert_len, &r.key, &r.key_len);
  return r;
}
static void release(LoadResult& r) {
  host_free(r.cert);
  host_free(r.key);
  r.cert = r.key = nullptr;
}

// A refusal: the answer, nothing handed back, nothing written, the store as
// it was, every block freed, and exactly one line, the one named.
static void check_refused(const LoadResult& r, const HostKeyNvs& before, const char* line) {
  CHECK(r.answer == KeyLoad::Refused);
  CHECK(r.cert == nullptr && r.cert_len == 0);
  CHECK(r.key == nullptr && r.key_len == 0);
  CHECK(host_key_nvs().rw_begins == 0 && host_key_nvs().puts == 0);
  CHECK(host_key_nvs().same_store(before));
  CHECK(g_live.empty());
  CHECK(Serial.lines.size() == 1);
  if (!printed(line)) std::printf("  expected a line with: %s\n", line);
  CHECK(printed(line));
  check_idle_and_clean();
}

// ════════════════════════════════════════════════════════════════════════════
// The guards: nothing stored is Absent, a stored pair loads.
// ════════════════════════════════════════════════════════════════════════════

// The one answer initTls() generates on. IDF says neither entry is stored,
// and the load opens no Preferences session and allocates nothing.
static void test_nothing_stored_is_absent() {
  fresh_world();
  const HostKeyNvs before = host_key_nvs();
  LoadResult r = load();
  CHECK(r.answer == KeyLoad::Absent);
  CHECK(r.cert == nullptr && r.cert_len == 0 && r.key == nullptr && r.key_len == 0);
  CHECK(host_key_nvs().ro_begins == 0 && host_key_nvs().reads == 0);
  CHECK(g_allocs == 0);
  CHECK(Serial.lines.empty());
  CHECK(host_key_nvs().same_store(before));
  check_idle_and_clean();

  // No namespace at all, which initTls() never meets (the identity key is
  // stored in it first): IDF's open says NOT_FOUND, and nothing is stored.
  fresh_world();
  host_key_nvs().ns_exists = false;
  host_key_nvs().blobs.clear();
  r = load();
  CHECK(r.answer == KeyLoad::Absent);
  CHECK(r.cert == nullptr && r.key == nullptr);
  CHECK(Serial.lines.empty());
  check_idle_and_clean();
}

// Both stored and read whole: Loaded, the bytes NVS holds, nothing written.
static void test_a_stored_pair_loads_unchanged() {
  fresh_world();
  store_pair(kCert, kKey);
  const HostKeyNvs before = host_key_nvs();
  LoadResult r = load();
  CHECK(r.answer == KeyLoad::Loaded);
  CHECK(r.cert != nullptr && r.cert_len == kCert.size() &&
        std::memcmp(r.cert, kCert.data(), kCert.size()) == 0);
  CHECK(r.key != nullptr && r.key_len == kKey.size() &&
        std::memcmp(r.key, kKey.data(), kKey.size()) == 0);
  CHECK(host_key_nvs().rw_begins == 0 && host_key_nvs().puts == 0);
  CHECK(host_key_nvs().same_store(before));
  CHECK(Serial.lines.empty());
  check_idle_and_clean();
  release(r);
  CHECK(g_live.empty());
}

// What the store writes, the load reads back as the same pair.
static void test_a_stored_pair_round_trips() {
  fresh_world();
  CHECK(tls_store_der_to_nvs(kCert.data(), kCert.size(), kKey.data(), kKey.size()));
  check_idle_and_clean();
  CHECK(host_key_nvs().blobs[NVS_KEY_TLS_CERT] == kCert);
  CHECK(host_key_nvs().blobs[NVS_KEY_TLS_KEY] == kKey);
  LoadResult r = load();
  CHECK(r.answer == KeyLoad::Loaded);
  CHECK(r.cert_len == kCert.size() && r.key_len == kKey.size());
  release(r);
  check_idle_and_clean();
}

// ════════════════════════════════════════════════════════════════════════════
// The refusals: a pair that is, or may be, stored and was not read whole.
// ════════════════════════════════════════════════════════════════════════════

// THE F421 CASES. Each read failing, filling the buffer and then failing, or
// answering one byte short. Before, every one answered false, and a
// certgen build stored a new pair over the stored one.
static void test_a_read_that_is_not_whole_is_refused() {
  struct Case {
    HostKeyNvs::Read read;
    const char* line;
  };
  char short_line[128];
  std::snprintf(short_line, sizeof(short_line),
                "[TLS] Certificate not loaded: the stored pair could not be read "
                "(%u of %u and %u of %u bytes)",
                (unsigned)kCert.size() - 1, (unsigned)kCert.size(),
                (unsigned)kKey.size() - 1, (unsigned)kKey.size());
  char zero_line[128];
  std::snprintf(zero_line, sizeof(zero_line),
                "[TLS] Certificate not loaded: the stored pair could not be read "
                "(0 of %u and 0 of %u bytes)",
                (unsigned)kCert.size(), (unsigned)kKey.size());
  const Case cases[] = {
      {HostKeyNvs::Read::Fails, zero_line},
      {HostKeyNvs::Read::Garbles, zero_line},
      {HostKeyNvs::Read::Short, short_line},
  };
  for (const Case& c : cases) {
    fresh_world();
    store_pair(kCert, kKey);
    host_key_nvs().read = c.read;
    const HostKeyNvs before = host_key_nvs();
    LoadResult r = load();
    check_refused(r, before, c.line);
    CHECK(host_key_nvs().reads == 2);
    // Both buffers were zeroed before they were freed: a garbled or short
    // read leaves bytes NVS refused (or part of the key) in them.
    CHECK(g_freed_zeroed.size() == 2 && g_freed_dirty.empty());

    // The entries are there for the next boot that can read them.
    host_key_nvs().clear_faults();
    Serial.lines.clear();
    LoadResult again = load();
    CHECK(again.answer == KeyLoad::Loaded);
    release(again);
    check_idle_and_clean();
  }

  // One entry's read refused while the other's is whole: refused all the
  // same, the whole buffer zeroed beside the refused one. The fault above
  // hits both reads at once, so a load that checked one count only, or
  // both together, passed it and handed initTls() a key whose read failed
  // (three such mutants, found by this suite's review).
  struct OneSided {
    const char* entry;
    HostKeyNvs::Read read;
    size_t cgot, kgot;
  };
  const size_t cl = kCert.size(), kl = kKey.size();
  const OneSided one_sided[] = {
      {NVS_KEY_TLS_CERT, HostKeyNvs::Read::Fails, 0, kl},
      {NVS_KEY_TLS_CERT, HostKeyNvs::Read::Garbles, 0, kl},
      {NVS_KEY_TLS_CERT, HostKeyNvs::Read::Short, cl - 1, kl},
      {NVS_KEY_TLS_KEY, HostKeyNvs::Read::Fails, cl, 0},
      {NVS_KEY_TLS_KEY, HostKeyNvs::Read::Garbles, cl, 0},
      {NVS_KEY_TLS_KEY, HostKeyNvs::Read::Short, cl, kl - 1},
  };
  for (const OneSided& c : one_sided) {
    fresh_world();
    store_pair(kCert, kKey);
    host_key_nvs().read = c.read;
    host_key_nvs().read_only_key = c.entry;
    char line[128];
    std::snprintf(line, sizeof(line),
                  "[TLS] Certificate not loaded: the stored pair could not be read "
                  "(%u of %u and %u of %u bytes)",
                  (unsigned)c.cgot, (unsigned)cl, (unsigned)c.kgot, (unsigned)kl);
    const HostKeyNvs before = host_key_nvs();
    LoadResult r = load();
    check_refused(r, before, line);
    CHECK(host_key_nvs().reads == 2);
    CHECK(g_freed_zeroed.size() == 2 && g_freed_dirty.empty());

    host_key_nvs().clear_faults();
    Serial.lines.clear();
    LoadResult again = load();
    CHECK(again.answer == KeyLoad::Loaded);
    release(again);
    check_idle_and_clean();
  }
}

// One entry stored without the other: refused, not replaced (F404's
// default; whether a lone entry should be replaced is F420's decision).
static void test_a_lone_entry_is_refused() {
  fresh_world();
  host_key_nvs().blobs[NVS_KEY_TLS_CERT] = kCert;
  HostKeyNvs before = host_key_nvs();
  LoadResult r = load();
  check_refused(r, before, "[TLS] Certificate not loaded: the certificate is stored without the key");
  CHECK(host_key_nvs().ro_begins == 0 && host_key_nvs().reads == 0 && g_allocs == 0);

  fresh_world();
  host_key_nvs().blobs[NVS_KEY_TLS_KEY] = kKey;
  before = host_key_nvs();
  r = load();
  check_refused(r, before, "[TLS] Certificate not loaded: the key is stored without the certificate");
  CHECK(host_key_nvs().ro_begins == 0 && host_key_nvs().reads == 0 && g_allocs == 0);
}

// An entry IDF says is stored with no bytes. Preferences' getBytesLength()
// answers 0 for it, as for an entry never stored; the load does not read
// that 0 as absence.
static void test_an_empty_entry_is_refused() {
  for (int c = 0; c < 3; c++) {
    fresh_world();
    store_pair(c == 1 ? kCert : std::vector<uint8_t>(),
               c == 0 ? kKey : std::vector<uint8_t>());
    const HostKeyNvs before = host_key_nvs();
    LoadResult r = load();
    char line[96];
    std::snprintf(line, sizeof(line),
                  "[TLS] Certificate not loaded: the stored pair is empty (%u and %u bytes)",
                  c == 1 ? (unsigned)kCert.size() : 0u, c == 0 ? (unsigned)kKey.size() : 0u);
    check_refused(r, before, line);
  }
}

// IDF says both are stored, and the Preferences session will not open.
static void test_a_session_that_will_not_open_is_refused() {
  fresh_world();
  store_pair(kCert, kKey);
  host_key_nvs().begin_fails = true;
  const HostKeyNvs before = host_key_nvs();
  LoadResult r = load();
  check_refused(r, before, "[TLS] Certificate not loaded: the NVS session did not open");
  CHECK(host_key_nvs().reads == 0 && g_allocs == 0);
}

// NVS that cannot say whether a pair is stored is not "no certificate":
// IDF's open failing with a fault, stored pair or none.
static void test_a_failed_open_is_refused() {
  for (esp_err_t fault : {ESP_ERR_NO_MEM, ESP_ERR_NVS_NOT_INITIALIZED, ESP_FAIL}) {
    for (bool stored : {true, false}) {
      fresh_world();
      if (stored) store_pair(kCert, kKey);
      if (!stored) {
        host_key_nvs().ns_exists = false;
        host_key_nvs().blobs.clear();
      }
      host_key_nvs().ro_open_fault = fault;
      const HostKeyNvs before = host_key_nvs();
      LoadResult r = load();
      char line[128];
      std::snprintf(line, sizeof(line),
                    "[TLS] Certificate not loaded: NVS could not say whether one is stored (%s)",
                    esp_err_to_name(fault));
      check_refused(r, before, line);
    }
  }
}

// The length lookup answering neither ESP_OK nor ESP_ERR_NVS_NOT_FOUND.
// Preferences' getBytesLength() answers 0 for every one of these, exactly as
// for an entry never stored, so a load that read its two zeros as absence
// would generate over a stored pair. Of these, IDF's size query gives only
// ESP_ERR_NVS_INVALID_HANDLE once the open succeeded (stubs/nvs_key/
// Preferences.h); the load fails closed on every answer it does not know.
static void test_a_failed_length_lookup_is_refused() {
  for (esp_err_t fault : {ESP_FAIL, ESP_ERR_NVS_TYPE_MISMATCH, ESP_ERR_NVS_INVALID_HANDLE,
                          ESP_ERR_NVS_INVALID_LENGTH}) {
    for (bool stored : {true, false}) {
      fresh_world();
      if (stored) store_pair(kCert, kKey);
      host_key_nvs().length_fault = fault;
      const HostKeyNvs before = host_key_nvs();
      LoadResult r = load();
      char line[128];
      std::snprintf(line, sizeof(line),
                    "[TLS] Certificate not loaded: NVS could not say whether one is stored (%s)",
                    esp_err_to_name(fault));
      check_refused(r, before, line);
    }
  }
}

// No memory for the certificate's buffer, or for the key's: refused, the
// one that was allocated freed, and the session closed.
static void test_no_memory_is_refused() {
  for (int at : {1, 2}) {
    fresh_world();
    store_pair(kCert, kKey);
    g_fail_alloc_at = at;
    const HostKeyNvs before = host_key_nvs();
    LoadResult r = load();
    char line[96];
    std::snprintf(line, sizeof(line),
                  "[TLS] Certificate not loaded: no memory for it (%u and %u bytes)",
                  (unsigned)kCert.size(), (unsigned)kKey.size());
    check_refused(r, before, line);
    CHECK(g_allocs == 2 && host_key_nvs().reads == 0);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// The sources around the cut: only Absent reaches the generator.
// ════════════════════════════════════════════════════════════════════════════

static std::string read_file(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
static bool ends_with(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}
static std::string body_of(const std::string& src, const std::string& head) {
  const size_t at = src.find(head);
  if (at == std::string::npos) return "";
  const size_t end = src.find("\n}\n", at);
  return end == std::string::npos ? "" : src.substr(at, end - at + 3);
}
static size_t count_of(const std::string& s, const std::string& needle) {
  size_t n = 0;
  for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) n++;
  return n;
}
// A line that is code, not a comment (the sources' comments name these
// functions in prose).
static bool is_code(const std::string& line) {
  const size_t first = line.find_first_not_of(" \t");
  if (first == std::string::npos) return false;
  return line.compare(first, 2, "//") != 0 && line.compare(first, 1, "*") != 0 &&
         line.compare(first, 2, "/*") != 0;
}
// A code line without its trailing // comment (a "//" inside a string or
// character literal is kept: the scan walks the literals).
static std::string strip_trailing_comment(const std::string& line) {
  char quote = 0;
  for (size_t i = 0; i + 1 < line.size(); i++) {
    if (quote) {
      if (line[i] == '\\') i++;
      else if (line[i] == quote) quote = 0;
    } else if (line[i] == '"' || line[i] == '\'') {
      quote = line[i];
    } else if (line[i] == '/' && line[i + 1] == '/') {
      return line.substr(0, i);
    }
  }
  return line;
}
static std::string code_only(const std::string& src) {
  std::istringstream lines(src);
  std::string line, out;
  while (std::getline(lines, line))
    if (is_code(line)) out += strip_trailing_comment(line) + "\n";
  return out;
}

static void test_init_tls_generates_only_on_absent(const std::string& network_path,
                                                   const std::vector<std::string>& tree) {
  const std::string net = read_file(network_path);
  CHECK(!net.empty());
  const std::string init = code_only(body_of(net, "\nbool ScvNetworkManager::initTls() {\n"));
  CHECK(!init.empty());

  // One call of the load, its answer kept, and three branches on it.
  const size_t load_at =
      init.find("  const KeyLoad stored = tls_load_der_from_nvs(&cert, &cert_len, &key, &key_len);\n");
  const size_t loaded_at = init.find("  if (stored == KeyLoad::Loaded) {\n");
  const size_t refused_at = init.find("  } else if (stored != KeyLoad::Absent) {\n");
  const size_t absent_at = init.find("  } else {\n", refused_at == std::string::npos ? 0 : refused_at);
  CHECK(load_at != std::string::npos && loaded_at != std::string::npos &&
        refused_at != std::string::npos && absent_at != std::string::npos);
  CHECK(load_at < loaded_at && loaded_at < refused_at && refused_at < absent_at);
  CHECK(count_of(init, "tls_load_der_from_nvs(") == 1);

  // The Refused branch says why and returns before anything is made or
  // stored.
  if (refused_at != std::string::npos && absent_at != std::string::npos && refused_at < absent_at) {
    const std::string refused = init.substr(refused_at, absent_at - refused_at);
    CHECK(refused.find("    m_tls_reason = \"stored certificate unreadable: HTTPS off this boot, "
                       "nothing stored over it\";\n") != std::string::npos);
    CHECK(count_of(refused, "log_health(") == 1);
    CHECK(refused.find("    return false;\n") != std::string::npos);
    CHECK(refused.find("tls_generate_self_signed(") == std::string::npos);
    CHECK(refused.find("tls_store_der_to_nvs(") == std::string::npos);
    const std::string loaded = init.substr(loaded_at, refused_at - loaded_at);
    CHECK(loaded.find("tls_generate_self_signed(") == std::string::npos);
    CHECK(loaded.find("tls_store_der_to_nvs(") == std::string::npos);
  }

  // The generator and the store are called once each in the file, both in
  // initTls(), both after the branch only Absent reaches.
  const size_t gen_at = init.find("tls_generate_self_signed(");
  const size_t store_at = init.find("tls_store_der_to_nvs(");
  CHECK(gen_at != std::string::npos && store_at != std::string::npos);
  CHECK(absent_at != std::string::npos && gen_at > absent_at && store_at > gen_at);
  const std::string code = code_only(net);
  CHECK(count_of(code, "tls_generate_self_signed(") == 2);  // its definition, initTls()'s call
  CHECK(count_of(code, "static bool tls_generate_self_signed(") == 1);
  CHECK(count_of(code, "tls_store_der_to_nvs(") == 2);       // its definition, initTls()'s call
  CHECK(count_of(code, "static bool tls_store_der_to_nvs(") == 1);
  CHECK(count_of(code, "tls_load_der_from_nvs(") == 2);      // its definition, initTls()'s call
  CHECK(count_of(code, "static KeyLoad tls_load_der_from_nvs(") == 1);

  // Nothing else in the canary tree names the two entries: the load and the
  // store above are their only reader and writer (canary_config.h defines
  // them; reset_scope_keys.h lists them as kept).
  int names = 0, unread = 0;
  for (const std::string& f : tree) {
    const std::string src = read_file(f);
    if (src.empty()) {
      std::printf("  could not read %s\n", f.c_str());
      unread++;
    }
    std::istringstream lines(src);
    std::string line;
    while (std::getline(lines, line)) {
      if (!is_code(line)) continue;
      const bool names_entry = line.find("NVS_KEY_TLS_CERT") != std::string::npos ||
                               line.find("NVS_KEY_TLS_KEY") != std::string::npos ||
                               line.find("\"tls_cert\"") != std::string::npos ||
                               line.find("\"tls_key\"") != std::string::npos;
      if (!names_entry) continue;
      if (ends_with(f, "lib/securacv_network/src/securacv_network.cpp")) {
        names++;
      } else {
        const bool allowed = ends_with(f, "canary/include/canary_config.h") ||
                             ends_with(f, "canary/include/reset_scope_keys.h");
        if (!allowed) std::printf("  a TLS entry named in %s: %s\n", f.c_str(), line.c_str());
        CHECK(allowed);
      }
    }
  }
  CHECK(unread == 0 && tree.size() > 50);
  CHECK(names == 6);  // the load's two probes and two reads, the store's two puts
}

int main(int argc, char** argv) {
  test_nothing_stored_is_absent();
  test_a_stored_pair_loads_unchanged();
  test_a_stored_pair_round_trips();
  test_a_read_that_is_not_whole_is_refused();
  test_a_lone_entry_is_refused();
  test_an_empty_entry_is_refused();
  test_a_session_that_will_not_open_is_refused();
  test_a_failed_open_is_refused();
  test_a_failed_length_lookup_is_refused();
  test_no_memory_is_refused();

  CHECK(argc >= 3);
  if (argc >= 3) {
    std::vector<std::string> tree(argv + 2, argv + argc);
    test_init_tls_generates_only_on_absent(argv[1], tree);
  }

  if (g_failures) {
    std::printf("test_tls_load_der: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("test_tls_load_der: OK, %d checks\n", g_checks);
  return 0;
}
