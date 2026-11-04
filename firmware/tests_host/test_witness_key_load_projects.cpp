/* Host tests for the identity-key load of canary-sense, canary-vision and
 * canary-sentinel (repo sweep F362), run on each project's own code:
 * load_or_generate_keypair() and secure_zero(), cut verbatim out of
 * firmware/projects/<project>/src/witness.cpp by cut_functions.awk, with
 * the file's own NVS names and key buffers (the Makefile takes those four
 * declaration lines from the same file), each compiled in a namespace of its
 * own over the fake NVS test_nvs_load_key.cpp uses (stubs/nvs_key:
 * Preferences and IDF's nvs_open() / nvs_get_blob() read one store) and the
 * real canary/log.h, with a stand-in RNG and Ed25519.
 *
 * THE FAILURE THIS PREVENTS. The load opened the namespace read-write and
 * took the stored key only when getBytesLength() answered 32, getBytes()
 * answered all 32 bytes and the bytes were not all zero. Every other outcome
 * drew a new key and stored it over whatever "privkey" held: a failed length
 * lookup (getBytesLength() answers 0 for it, as for a key never stored), a
 * key of another length, a read that failed or came back short, and a stored
 * zero key. Each replaced the unit's identity for good, and Home Assistant
 * read the new key as "Fingerprint changed without rotation", which the
 * owner cannot tell from tampering.
 *
 * Now IDF is asked first, and only its ESP_ERR_NVS_NOT_FOUND (no namespace,
 * or no key in it) draws a key. Everything else disables signing for the
 * boot: the load answers false, the key buffer is zero, and nothing is
 * written, the path a failed open already took. Every answer the fake can
 * give, and where it goes:
 *   no namespace, or a namespace without the key        a key is drawn and stored
 *   a 32-byte key read whole, not all zero               loaded, nothing written
 *   IDF's open fails with a fault (not NOT_FOUND)        signing off, nothing written
 *   IDF's length lookup fails (ESP_FAIL, TYPE_MISMATCH,
 *     INVALID_HANDLE, INVALID_LENGTH)                    signing off, nothing written
 *   a stored key of 0, 1, 16, 31, 33 or 64 bytes         signing off, nothing written
 *   Read::Fails / Garbles / Short                        signing off, nothing written
 *   the Preferences session refuses to open              signing off, nothing written
 *   a stored key of 32 zero bytes                        signing off, nothing written
 * So only true absence reaches the draw-and-store path.
 *
 * It also reads the three sources (argv): the three functions are byte for
 * byte the same, each project's init() returns false on the load's false
 * before it hands the key to the signer, and the load is the only code in
 * each project that names the key's NVS entry.
 *
 * What this does not prove: ESP-IDF's NVS or Arduino's Preferences
 * themselves (the fakes answer as their 2.0.17 sources read), the Ed25519
 * library, or anything on a device. The chain-state load beside it is F363's.
 * The three projects' PlatformIO builds are CI's; none has run on a bench.
 *
 * Build & run: make -C firmware/tests_host (the cut needs the Makefile).
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Arduino.h"      // stubs/nvs_key: a Serial that keeps every line
#include "Preferences.h"  // stubs/nvs_key
#include "nvs.h"          // stubs/nvs_key: IDF's NVS over the Preferences fake's store
#include "canary/log.h"   // canary-sense's own log_line() / dbg_serial()

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

// ── the stand-ins the cut code calls ───────────────────────────────────────

static int g_draws = 0;
static int g_rng_gates = 0;     // bootloader_random_enable() calls not yet disabled
static int g_derives = 0;
static uint8_t drawn_byte(size_t i) { return (uint8_t)(0xC3 ^ (i * 11)); }
void bootloader_random_enable() { g_rng_gates++; }
void bootloader_random_disable() { g_rng_gates--; }
void esp_fill_random(void* buf, size_t len) {
  g_draws++;
  CHECK(g_rng_gates == 1);   // the identity draw runs inside the early-entropy gate
  uint8_t* b = static_cast<uint8_t*>(buf);
  for (size_t i = 0; i < len; i++) b[i] = drawn_byte(i);
}
struct Ed25519 {
  static void derivePublicKey(uint8_t pub[32], const uint8_t priv[32]) {
    g_derives++;
    for (size_t i = 0; i < 32; i++) pub[i] = (uint8_t)~priv[i];
  }
};

// ── each project's cut, in a namespace of its own ──────────────────────────

namespace canary {
namespace witness_sense {
#include "witness_key_load_canary-sense.inc"
}  // namespace witness_sense
namespace witness_vision {
#include "witness_key_load_canary-vision.inc"
}  // namespace witness_vision
namespace witness_sentinel {
#include "witness_key_load_canary-sentinel.inc"
}  // namespace witness_sentinel
}  // namespace canary

struct Project {
  const char* name;
  bool (*load)();
  uint8_t* priv;
  uint8_t* pub;
  const char* ns;
  const char* key;
};
static const Project kProjects[] = {
    {"canary-sense", canary::witness_sense::load_or_generate_keypair,
     canary::witness_sense::s_priv, canary::witness_sense::s_pub,
     canary::witness_sense::NVS_NAMESPACE, canary::witness_sense::KEY_PRIV},
    {"canary-vision", canary::witness_vision::load_or_generate_keypair,
     canary::witness_vision::s_priv, canary::witness_vision::s_pub,
     canary::witness_vision::NVS_NAMESPACE, canary::witness_vision::KEY_PRIV},
    {"canary-sentinel", canary::witness_sentinel::load_or_generate_keypair,
     canary::witness_sentinel::s_priv, canary::witness_sentinel::s_pub,
     canary::witness_sentinel::NVS_NAMESPACE, canary::witness_sentinel::KEY_PRIV},
};

// ── the world each test starts from ─────────────────────────────────────────

static std::vector<uint8_t> good_key() {
  std::vector<uint8_t> k(32);
  for (size_t i = 0; i < 32; i++) k[i] = (uint8_t)(i * 7 + 3);
  return k;
}
static std::vector<uint8_t> drawn_key() {
  std::vector<uint8_t> k(32);
  for (size_t i = 0; i < 32; i++) k[i] = drawn_byte(i);
  return k;
}

// The namespace is shared with runtime_config, so a real unit's holds other
// entries beside the key.
static void fresh_world(const Project& p) {
  host_key_nvs() = HostKeyNvs();
  host_key_nvs().ns = p.ns;
  Serial.lines.clear();
  std::memset(p.priv, 0, 32);
  std::memset(p.pub, 0, 32);
  g_draws = 0;
  g_rng_gates = 0;
  g_derives = 0;
}
static void store(const std::vector<uint8_t>& key) {
  host_key_nvs().ns_exists = true;
  host_key_nvs().blobs["device_id"] = {'s', 'c', 'v'};
  host_key_nvs().blobs["privkey"] = key;
}

static bool equals(const uint8_t* a, const std::vector<uint8_t>& b) {
  return b.size() == 32 && std::memcmp(a, b.data(), 32) == 0;
}
static bool all_zero(const uint8_t* a) {
  for (size_t i = 0; i < 32; i++)
    if (a[i] != 0) return false;
  return true;
}
static bool printed(const char* needle) {
  for (const std::string& l : Serial.lines)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}

// After every call: no handle left open, nothing written through IDF, and
// nothing the fakes count as a fault.
static void check_idle_and_clean() {
  const HostKeyNvs& n = host_key_nvs();
  CHECK(n.prefs_open == 0);
  CHECK(n.idf_open_handles == 0 && n.idf_opens >= n.idf_closes);
  CHECK(n.idf_rw_opens == 0);
  CHECK(n.double_begins == 0);
  CHECK(n.wrong_namespace == 0);
  CHECK(g_rng_gates == 0);
}

// Signing off for the boot: false, no key drawn, nothing opened for
// writing or put, the store as it was, the buffer zero, and one line naming
// why that says signing is off.
static void check_signing_off(const Project& p, const HostKeyNvs& before, const char* line) {
  CHECK(!p.load());
  CHECK(g_draws == 0 && g_derives == 0);
  CHECK(host_key_nvs().rw_begins == 0 && host_key_nvs().puts == 0);
  CHECK(host_key_nvs().same_store(before));
  CHECK(all_zero(p.priv));
  CHECK(Serial.lines.size() == 1);
  if (!printed(line)) std::printf("  %s: expected a line with: %s\n", p.name, line);
  CHECK(printed(line));
  CHECK(printed("signing disabled this boot."));
  CHECK(!printed("Generated new Ed25519 identity"));
  check_idle_and_clean();
}

// ════════════════════════════════════════════════════════════════════════════
// The guards: true absence draws and stores a key; a good key loads.
// ════════════════════════════════════════════════════════════════════════════

static void test_no_key_stored_draws_and_stores_one(const Project& p) {
  // No namespace yet: IDF's open says NOT_FOUND, and the read-write open
  // makes it.
  fresh_world(p);
  CHECK(p.load());
  CHECK(g_draws == 1 && g_derives == 1);
  CHECK(host_key_nvs().ns_exists);
  CHECK(host_key_nvs().blobs["privkey"] == drawn_key());
  CHECK(equals(p.priv, drawn_key()));
  CHECK(printed("[WITNESS] Generated new Ed25519 identity (first boot)."));
  CHECK(Serial.lines.size() == 1);
  check_idle_and_clean();

  // The next boot loads that key, and draws nothing.
  std::memset(p.priv, 0, 32);
  Serial.lines.clear();
  CHECK(p.load());
  CHECK(g_draws == 1 && equals(p.priv, drawn_key()));
  CHECK(Serial.lines.empty());
  check_idle_and_clean();

  // A namespace runtime_config made, with no key in it.
  fresh_world(p);
  host_key_nvs().ns_exists = true;
  host_key_nvs().blobs["device_id"] = {'s', 'c', 'v'};
  CHECK(p.load());
  CHECK(g_draws == 1);
  CHECK(host_key_nvs().blobs["privkey"] == drawn_key());
  CHECK(host_key_nvs().blobs["device_id"] == std::vector<uint8_t>({'s', 'c', 'v'}));
  check_idle_and_clean();
}

static void test_a_good_stored_key_loads_unchanged(const Project& p) {
  fresh_world(p);
  store(good_key());
  const HostKeyNvs before = host_key_nvs();
  CHECK(p.load());
  CHECK(equals(p.priv, good_key()));
  CHECK(g_derives == 1 && p.pub[0] == (uint8_t)~good_key()[0]);
  CHECK(g_draws == 0 && host_key_nvs().rw_begins == 0 && host_key_nvs().puts == 0);
  CHECK(host_key_nvs().same_store(before));
  CHECK(Serial.lines.empty());
  check_idle_and_clean();

  // One non-zero byte anywhere is a key.
  for (size_t at : {(size_t)0, (size_t)17, (size_t)31}) {
    fresh_world(p);
    std::vector<uint8_t> k(32, 0);
    k[at] = 0x01;
    store(k);
    CHECK(p.load());
    CHECK(equals(p.priv, k));
    CHECK(g_draws == 0);
    check_idle_and_clean();
  }
}

// A first boot whose read-write session will not open: no key is drawn or
// kept, the path the old load took too.
static void test_a_first_boot_with_no_session_keeps_no_key(const Project& p) {
  fresh_world(p);
  host_key_nvs().begin_fails = true;   // no namespace, and no session opens
  CHECK(!p.load());
  CHECK(g_draws == 0 && host_key_nvs().blobs.empty());
  CHECK(all_zero(p.priv));
  CHECK(printed("[WITNESS] NVS unavailable — signing disabled this boot."));
  check_idle_and_clean();
}

// ════════════════════════════════════════════════════════════════════════════
// THE F362 CASES: a key that is, or may be, stored and was not read whole.
// ════════════════════════════════════════════════════════════════════════════

static void test_a_failed_open_is_not_replaced(const Project& p) {
  for (esp_err_t fault : {ESP_ERR_NO_MEM, ESP_ERR_NVS_NOT_INITIALIZED, ESP_FAIL}) {
    for (bool stored : {true, false}) {
      fresh_world(p);
      if (stored) store(good_key());
      host_key_nvs().ro_open_fault = fault;
      const HostKeyNvs before = host_key_nvs();
      char line[128];
      std::snprintf(line, sizeof(line), "[WITNESS] NVS could not say whether a key is stored (%s)",
                    esp_err_to_name(fault));
      check_signing_off(p, before, line);
    }
  }
}

// getBytesLength() answers 0 for every one of these, exactly as for a key
// never stored, which is how the old load drew a new key over a stored one.
// IDF's size query gives only ESP_ERR_NVS_INVALID_HANDLE of them once the
// open succeeded (stubs/nvs_key/Preferences.h); the load fails closed on
// every answer it does not know.
static void test_a_failed_length_lookup_is_not_replaced(const Project& p) {
  for (esp_err_t fault : {ESP_FAIL, ESP_ERR_NVS_TYPE_MISMATCH, ESP_ERR_NVS_INVALID_HANDLE,
                          ESP_ERR_NVS_INVALID_LENGTH}) {
    for (bool stored : {true, false}) {
      fresh_world(p);
      if (stored) store(good_key());
      else host_key_nvs().ns_exists = true;
      host_key_nvs().length_fault = fault;
      const HostKeyNvs before = host_key_nvs();
      char line[128];
      std::snprintf(line, sizeof(line), "[WITNESS] NVS could not say whether a key is stored (%s)",
                    esp_err_to_name(fault));
      check_signing_off(p, before, line);
    }
  }
}

static void test_a_wrong_length_key_is_not_replaced(const Project& p) {
  for (size_t len : {(size_t)0, (size_t)1, (size_t)16, (size_t)31, (size_t)33, (size_t)64}) {
    fresh_world(p);
    store(std::vector<uint8_t>(len, 0x5A));
    const HostKeyNvs before = host_key_nvs();
    char line[96];
    std::snprintf(line, sizeof(line), "[WITNESS] Stored key is %u bytes, not 32", (unsigned)len);
    check_signing_off(p, before, line);
    CHECK(host_key_nvs().reads == 0 && host_key_nvs().ro_begins == 0);
  }
}

static void test_a_read_that_is_not_whole_is_not_replaced(const Project& p) {
  struct Case {
    HostKeyNvs::Read read;
    const char* line;
  };
  const Case cases[] = {
      {HostKeyNvs::Read::Fails, "[WITNESS] Stored key could not be read (0 of 32 bytes)"},
      {HostKeyNvs::Read::Garbles, "[WITNESS] Stored key could not be read (0 of 32 bytes)"},
      {HostKeyNvs::Read::Short, "[WITNESS] Stored key could not be read (31 of 32 bytes)"},
  };
  for (const Case& c : cases) {
    fresh_world(p);
    store(good_key());
    host_key_nvs().read = c.read;
    std::memset(p.priv, 0xEE, 32);   // a dirty buffer comes back zero
    const HostKeyNvs before = host_key_nvs();
    check_signing_off(p, before, c.line);
    CHECK(host_key_nvs().reads == 1 && host_key_nvs().ro_begins == 1);

    // The entry the fault left is there for the next boot that can read it.
    host_key_nvs().clear_faults();
    Serial.lines.clear();
    CHECK(p.load());
    CHECK(equals(p.priv, good_key()));
    CHECK(g_draws == 0 && host_key_nvs().puts == 0);
    check_idle_and_clean();
  }
}

// IDF says a 32-byte key is stored, and the read-only session will not open.
static void test_a_session_that_will_not_open_is_not_replaced(const Project& p) {
  fresh_world(p);
  store(good_key());
  host_key_nvs().begin_fails = true;
  const HostKeyNvs before = host_key_nvs();
  check_signing_off(p, before, "[WITNESS] NVS unavailable — signing disabled this boot.");
  CHECK(host_key_nvs().reads == 0);
}

// The flagship's rule (F360): 32 zero bytes are the one key whose every
// derivation is public, and a drawn key is that with odds of 2^-256.
static void test_an_all_zero_stored_key_is_not_replaced(const Project& p) {
  fresh_world(p);
  store(std::vector<uint8_t>(32, 0));
  const HostKeyNvs before = host_key_nvs();
  check_signing_off(p, before, "[WITNESS] Stored key is 32 zero bytes");
}

// ════════════════════════════════════════════════════════════════════════════
// The sources around the cut: three copies of one function, and init()'s
// refusal.
// ════════════════════════════════════════════════════════════════════════════

static std::string read_file(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
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

static void test_the_sources(const std::vector<std::string>& files) {
  CHECK(files.size() == 3);
  std::string first;
  for (const std::string& f : files) {
    const std::string src = read_file(f);
    CHECK(!src.empty());
    const std::string load = body_of(src, "\nbool load_or_generate_keypair() {\n");
    CHECK(!load.empty());
    if (first.empty()) first = load;
    if (load != first) std::printf("  %s: load_or_generate_keypair() differs from %s's\n",
                                   f.c_str(), files[0].c_str());
    CHECK(load == first);

    // init() refuses on the load's false before the key reaches the signer.
    const std::string init = body_of(src, "\nbool init() {\n");
    const size_t stop = init.find("  if (!load_or_generate_keypair()) {\n"
                                  "    s_ready = false;\n"
                                  "    return false;\n"
                                  "  }\n");
    CHECK(stop != std::string::npos);
    const size_t signer = init.find("device_signature::init(s_priv, s_pub");
    CHECK(signer != std::string::npos && signer > stop);
    CHECK(count_of(src, "\nbool load_or_generate_keypair() {\n") == 1);
    CHECK(count_of(src, "load_or_generate_keypair())") == 1);   // init()'s call, the only one

    // The key's entry is named by its declaration and the load alone: the
    // probe, the read and the put (no other code path writes a key).
    CHECK(count_of(src, "KEY_PRIV") == 4);
    CHECK(count_of(load, "KEY_PRIV") == 3);
    CHECK(count_of(src, "\"privkey\"") == 1);
  }
}

int main(int argc, char** argv) {
  for (const Project& p : kProjects) {
    const int before = g_failures;
    test_no_key_stored_draws_and_stores_one(p);
    test_a_good_stored_key_loads_unchanged(p);
    test_a_first_boot_with_no_session_keeps_no_key(p);
    test_a_failed_open_is_not_replaced(p);
    test_a_failed_length_lookup_is_not_replaced(p);
    test_a_wrong_length_key_is_not_replaced(p);
    test_a_read_that_is_not_whole_is_not_replaced(p);
    test_a_session_that_will_not_open_is_not_replaced(p);
    test_an_all_zero_stored_key_is_not_replaced(p);
    if (g_failures != before) std::printf("  ^ %s\n", p.name);
  }
  test_the_sources(std::vector<std::string>(argv + 1, argv + argc));

  if (g_failures) {
    std::printf("test_witness_key_load_projects: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("test_witness_key_load_projects: OK, %d checks (sense, vision, sentinel)\n", g_checks);
  return 0;
}
