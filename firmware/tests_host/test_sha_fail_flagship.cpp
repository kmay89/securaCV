/* The flagship's and the common witness core's SHA-256 fail closed (repo
 * sweep F455, the canary-wap's F364 rule on the other trees).
 *
 * Before, securacv_crypto.cpp's sha256_raw() and sha256_domain() returned
 * void and checked none of mbedtls_sha256_starts(), _update() and
 * _finish(), and common/witness/witness_chain.h cast every step's answer
 * away. On the S3 and the C3 the SHA engine is hardware and a step can
 * answer an error; after one, the output is not, or not wholly, written, so
 * it keeps whatever the caller's stack held. Every caller used those bytes:
 * the canary (PIO) tree's record payload hash and chain link
 * (witness_create_record_gps()), its SD tail recovery and its chain_ok
 * self-test (compute_chain_hash()), its public key fingerprint and genesis
 * head (witness_provision_device()) and its TLS certificate pin
 * (initTls()); canary-sense's, canary-vision's and canary-sentinel's
 * fingerprint, genesis head and chain_advance(). Now every helper answers
 * whether each step ran (the _ret names on mbedTLS 2.x, the plain names on
 * 3.x, by MBEDTLS_VERSION_NUMBER), zeroes its output when one did not and
 * frees the context on every path, and every caller refuses what the hash
 * was for, by a path the tree already had.
 *
 * This runs the REAL code, cut verbatim out of the sources
 * (cut_functions.awk; the Makefile writes the .inc files): securacv_crypto.cpp's
 * three step wrappers, sha256_raw(), sha256_domain(), crypto_fingerprint() and
 * compute_chain_hash(); securacv_witness.cpp's persist_chain_blob(),
 * witness_provision_device(), update_chain(), witness_persist_chain_state(),
 * persist_chain_if_due(), witness_recover_from_sd(),
 * witness_create_record_gps() and witness_verify_record();
 * securacv_diagnostics.cpp's test_chain(); securacv_network.cpp's
 * tls_cert_fingerprint(); witness_chain.h itself; and, from each of the
 * three projects' src/witness.cpp, its sha256_domain() wrapper,
 * secure_zero(), load_or_start_chain(), persist_chain(), init() and
 * chain_advance() with the file's own names and buffers, each project in a
 * namespace of its own. Under them is stubs/sha_fail's mbedTLS stand-in
 * (a real SHA-256 over OpenSSL that fails the step the test names, once) in
 * the spelling the Makefile builds: this suite is built twice, once as
 * mbedTLS 2.28 (core 2.0.17) and once as 3.6 (core 3.x). What it pins, for
 * a failing start, each update and the finish, in turn:
 *   - every helper: false, 32 zero bytes, the context freed, no step run
 *     after the one that failed; a known answer when every step runs;
 *   - a record (payload hash or link): false, no seq or head advance, *out as
 *     it was, nothing signed, ringed, appended to the card or persisted; the
 *     next record takes the same seq from the same head;
 *   - the SD tail recovery: the tail is not adopted (seq and head as they
 *     were, nothing persisted), logged as a mismatch with "SHA-256 failed";
 *   - test_chain(): false;
 *   - the fingerprint: provisioning answers false before the device is named
 *     or anything after the key is written (no device id or SoftAP name, no
 *     boot count, no chain, no bearer derivation), and says why. The key
 *     loaders here are stand-ins that store nothing: on a first boot the
 *     real one has stored the new key by then, and the next boot loads it
 *     and computes the fingerprint again;
 *   - the genesis head: the same, nothing persisted;
 *   - the certificate pin: false and the hex emptied, so initTls() (pinned
 *     by text below) frees both buffers and serves no HTTPS with a reason;
 *   - each project: init() refuses a fingerprint or genesis head that did
 *     not compute (signing off, the key scrubbed before the signer sees it,
 *     no chain head or length written; the key load is the same kind of
 *     stand-in), and chain_advance() a payload hash or link that did
 *     not (head and length as they were, nothing persisted);
 *   - the sources (argv): every call of these helpers in the canary tree and
 *     the three projects uses its answer, and initTls()'s refusal comes
 *     before the certificate is kept.
 *
 * What this does not prove: the hardware engine, IDF's port, or anything on a
 * device. The ESP32 compiles of both spellings (the core 2.0.17 legs and the
 * core 3 legs) are firmware.yml's; nothing here has run on a bench.
 *
 * Build & run: make -C firmware/tests_host run-sha-fail-flagship
 */
#include <openssl/evp.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "Arduino.h"      // stubs/sha_fail: a Serial that keeps every line
#include "Preferences.h"  // stubs/sha_fail: one store, every put counted
#include "mbedtls/sha256.h"
#include "mbedtls/version.h"

#include "securacv_crypto.h"   // the real declarations (over stubs/sha_fail + nvs_manager's freertos)
#include "securacv_witness.h"  // the real DeviceIdentity / WitnessRecord / SystemHealth
#include "canary_config.h"
#include "network/tls_policy.h"
#include "witness/chain_persist.h"
#include "witness/chain_state.h"
#include "witness/witness_chain.h"  // built with WC_HAS_MBEDTLS=1: the helpers themselves
#include "witness/witness_store.h"
#include "identity/device_signature.h"  // the signer's declarations; faked below
#include "canary/log.h"                 // canary-sense's log_line() / dbg_serial()
#include "../projects/canary-sentinel/include/canary/types.h"  // SentinelClaim

static int g_checks = 0;
static int g_failures = 0;
static std::string g_case;  // the scenario, printed with a failure
#define CHECK(cond)                                                                   \
  do {                                                                                \
    g_checks++;                                                                       \
    if (!(cond)) {                                                                    \
      std::printf("FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__, #cond, g_case.c_str()); \
      g_failures++;                                                                   \
    }                                                                                 \
  } while (0)

// A helper's answer. The helpers answer bool; on a tree where one returned
// void (c2741d9f's, the fails-on-old run) a call counts as answered true, so
// the suite reports what that tree did with a failed hash as FAIL lines
// rather than as a build error.
template <typename F>
static bool answered(F&& call) {
  if constexpr (std::is_void_v<decltype(call())>) {
    call();
    return true;
  } else {
    return call();
  }
}

static bool all_zero(const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++)
    if (b[i] != 0) return false;
  return true;
}

// ── expected values, through none of the code under test ────────────────────

static void openssl_sha(const char* domain, const uint8_t* data, size_t n, uint8_t out[32]) {
  EVP_MD_CTX* c = EVP_MD_CTX_new();
  unsigned int olen = 0;
  const uint8_t sep = 0;
  EVP_DigestInit_ex(c, EVP_sha256(), nullptr);
  if (domain) {
    EVP_DigestUpdate(c, domain, std::strlen(domain));
    EVP_DigestUpdate(c, &sep, 1);
  }
  if (n > 0) EVP_DigestUpdate(c, data, n);
  EVP_DigestFinal_ex(c, out, &olen);
  EVP_MD_CTX_free(c);
}
static void expected_link(const uint8_t prev[32], const uint8_t ph[32], uint32_t seq, uint32_t tb,
                          uint8_t out[32]) {
  uint8_t buf[72];
  std::memcpy(buf, prev, 32);
  std::memcpy(buf + 32, ph, 32);
  for (int i = 0; i < 4; i++) {
    buf[64 + i] = (uint8_t)(seq >> (24 - 8 * i));
    buf[68 + i] = (uint8_t)(tb >> (24 - 8 * i));
  }
  openssl_sha("securacv:fw:chain:v1", buf, sizeof(buf), out);
}

// ── the failures a hash can meet ────────────────────────────────────────────

struct Fault {
  sha_stub::Step step;
  int update;
  std::string name() const {
    switch (step) {
      case sha_stub::Step::None: return "no failure";
      case sha_stub::Step::Starts: return "starts fails";
      case sha_stub::Step::Update: return "update #" + std::to_string(update + 1) + " fails";
      case sha_stub::Step::Finish: return "finish fails";
    }
    return "?";
  }
};
// Every step a hash with `updates` updates makes, failing in turn.
static std::vector<Fault> faults_for(int updates) {
  std::vector<Fault> f{{sha_stub::Step::Starts, 0}};
  for (int u = 0; u < updates; u++) f.push_back({sha_stub::Step::Update, u});
  f.push_back({sha_stub::Step::Finish, 0});
  return f;
}
static void arm(const Fault& f, int hash) {
  sha_stub::arm(f.step, hash, f.update);
}
// After a call that met `f`: the fault fired, no step ran after it, and
// every context was freed.
static void check_the_hash_stopped() {
  CHECK(sha_stub::g_fired);
  CHECK(sha_stub::g_steps_after_failure == 0);
  CHECK(sha_stub::g_inits == sha_stub::g_frees);
  CHECK(sha_stub::g_live == 0);
}

// ════════════════════════════════════════════════════════════════════════════
// The canary (PIO) tree: what the cut functions call
// ════════════════════════════════════════════════════════════════════════════

static DeviceIdentity g_device;
static SystemHealth g_health;
static WitnessRecord g_last_record;
static uint32_t g_state_entered_ms = 0;
static FixState g_pending_state = STATE_NO_FIX;
static const size_t WITNESS_RECORD_RING_SIZE = 32;
static WitnessRecord g_record_ring[WITNESS_RECORD_RING_SIZE];
static size_t g_record_ring_head = 0;
static size_t g_record_ring_count = 0;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
static portMUX_TYPE g_record_ring_mux = portMUX_INITIALIZER_UNLOCKED;

struct HostEsp {
  uint32_t getFreeHeap() { return 123456; }
};
static HostEsp ESP;

struct Ed25519 {
  static void derivePublicKey(uint8_t pub[32], const uint8_t priv[32]) {
    for (size_t i = 0; i < 32; i++) pub[i] = (uint8_t)(priv[i] ^ 0x3C);
  }
};

// The signer: a stand-in, deterministic in the message and the key.
static int g_signs = 0;
void crypto_sign(const uint8_t priv[32], const uint8_t pub[32], const uint8_t* msg, size_t len,
                 uint8_t sig[64]) {
  (void)priv;
  g_signs++;
  for (size_t i = 0; i < 64; i++) sig[i] = (uint8_t)(msg[i % len] ^ pub[i % 32] ^ 0x5A);
}
bool crypto_verify(const uint8_t pub[32], const uint8_t* msg, size_t len, const uint8_t sig[64]) {
  for (size_t i = 0; i < 64; i++)
    if (sig[i] != (uint8_t)(msg[i % len] ^ pub[i % 32] ^ 0x5A)) return false;
  return true;
}

// NVS, as securacv_crypto.cpp's helpers answer it.
static std::map<std::string, uint32_t> g_nvs_u32;
static std::map<std::string, std::vector<uint8_t>> g_nvs_bytes;
static int g_nvs_store_u32 = 0, g_nvs_store_bytes = 0;
void crypto_print_key_at_rest() {}
uint32_t nvs_load_u32(const char* key, uint32_t def) {
  auto it = g_nvs_u32.find(key);
  return it == g_nvs_u32.end() ? def : it->second;
}
bool nvs_store_u32(const char* key, uint32_t val) {
  g_nvs_store_u32++;
  g_nvs_u32[key] = val;
  return true;
}
bool nvs_load_bytes(const char* key, uint8_t* out, size_t len) {
  auto it = g_nvs_bytes.find(key);
  if (it == g_nvs_bytes.end() || it->second.size() != len) return false;
  std::memcpy(out, it->second.data(), len);
  return true;
}
bool nvs_store_bytes(const char* key, const uint8_t* data, size_t len) {
  g_nvs_store_bytes++;
  g_nvs_bytes[key] = std::vector<uint8_t>(data, data + len);
  return true;
}

// The names the fingerprint makes; a stand-in that counts.
static int g_named = 0;
void generate_device_id(char* out, size_t cap, const char* prefix, const uint8_t fp[2]) {
  g_named++;
  std::snprintf(out, cap, "%s%02x%02x", prefix, fp[0], fp[1]);
}
void generate_ap_ssid(char* out, size_t cap, const uint8_t fp[2]) {
  g_named++;
  std::snprintf(out, cap, "SecuraCV-canary-%02x%02x", fp[0], fp[1]);
}

static int g_auth_derives = 0;
static bool auth_load_or_derive(const uint8_t privkey[32]) {
  (void)privkey;
  g_auth_derives++;
  return true;
}

static uint8_t key_byte(size_t i) { return (uint8_t)(0x41 + 3 * i); }
static bool establish_identity_key() {
  for (size_t i = 0; i < 32; i++) g_device.privkey[i] = key_byte(i);
  return true;
}

struct LogLine {
  std::string message, detail;
};
static std::vector<LogLine> g_log;
void log_health(LogLevel level, LogCategory category, const char* message, const char* detail) {
  (void)level;
  (void)category;
  g_log.push_back({message ? message : "", detail ? detail : ""});
}
static bool logged(const char* message, const char* detail) {
  for (const LogLine& l : g_log)
    if (l.message == message && l.detail == detail) return true;
  return false;
}

static uint32_t g_tb = 42;
uint32_t time_bucket() { return g_tb; }

DeviceIdentity& witness_get_device() { return g_device; }
SystemHealth& witness_get_health() { return g_health; }
WitnessRecord& witness_get_last_record() { return g_last_record; }

// The SD card: one /WITNESS/records.jsonl, read whole.
#define FILE_READ "r"
struct File {
  const std::string* data = nullptr;
  size_t pos = 0;
  explicit operator bool() const { return data != nullptr; }
  size_t size() const { return data ? data->size() : 0; }
  bool seek(size_t p) {
    if (!data || p > data->size()) return false;
    pos = p;
    return true;
  }
  size_t read(uint8_t* buf, size_t n) {
    if (!data) return 0;
    const size_t k = (n < data->size() - pos) ? n : data->size() - pos;
    std::memcpy(buf, data->data() + pos, k);
    pos += k;
    return k;
  }
  void close() {}
};
struct HostSd {
  std::string records;
  bool has = false;
  File open(const char* path, const char* mode) {
    (void)mode;
    File f;
    if (has && std::strcmp(path, "/WITNESS/records.jsonl") == 0) f.data = &records;
    return f;
  }
};
static HostSd SD;
static bool storage_is_mounted() { return true; }
static int g_sd_appends = 0;
static bool sd_append_record(const WitnessRecord* rec) {
  (void)rec;
  g_sd_appends++;
  return true;
}

// ── the cut code ────────────────────────────────────────────────────────────

#include "sha_fail_crypto.inc"
#include "sha_fail_witness.inc"
namespace diag {
#include "sha_fail_diag.inc"
}  // namespace diag
#include "sha_fail_network.inc"

// ════════════════════════════════════════════════════════════════════════════
// The three projects: what their cut functions call
// ════════════════════════════════════════════════════════════════════════════

namespace canary {
namespace cfg {
struct HostRuntimeCfg {
  char device_id[32] = "canary-test-7f";
};
inline HostRuntimeCfg& get() {
  static HostRuntimeCfg c;
  return c;
}
}  // namespace cfg
}  // namespace canary

static int g_signer_inits = 0;
namespace device_signature {
void init(const uint8_t priv[32], const uint8_t pub[32], const char* device_id,
          const char* fingerprint_hex) {
  (void)priv;
  (void)pub;
  (void)device_id;
  (void)fingerprint_hex;
  g_signer_inits++;
}
const char* pubkey_hex() { return "00"; }
const char* device_id() { return canary::cfg::get().device_id; }
size_t build_sense_canonical(uint32_t seq, const char* event_name, const char* presence,
                             const char* occupants, const char* range, uint32_t bucket_uptime_s,
                             const char* device_id, char* out, size_t cap) {
  const int n = std::snprintf(out, cap, "sense|%u|%s|%s|%s|%s|%u|%s", (unsigned)seq, event_name,
                              presence, occupants, range, (unsigned)bucket_uptime_s, device_id);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}
size_t build_sentinel_canonical(uint32_t seq, const char* event_name, const char* level,
                                uint8_t confidence, uint8_t anomaly, const char* occupancy,
                                const char* range, uint8_t modality_bits,
                                uint32_t bucket_uptime_s, const char* device_id, char* out,
                                size_t cap) {
  const int n = std::snprintf(out, cap, "sentinel|%u|%s|%s|%u|%u|%s|%s|%u|%u|%s", (unsigned)seq,
                              event_name, level, (unsigned)confidence, (unsigned)anomaly,
                              occupancy, range, (unsigned)modality_bits,
                              (unsigned)bucket_uptime_s, device_id);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}
}  // namespace device_signature

static bool g_project_key_ok = true;
// Each project's cut, in a namespace of its own: the file's names and
// buffers, a stand-in for its key load (test_witness_key_load_projects.cpp
// runs the real one), then its functions.
#define PROJECT_KEY_LOAD                                                   \
  bool load_or_generate_keypair() {                                        \
    if (!g_project_key_ok) return false;                                   \
    for (size_t i = 0; i < 32; i++) s_priv[i] = key_byte(i);               \
    Ed25519::derivePublicKey(s_pub, s_priv);                               \
    return true;                                                           \
  }
namespace canary {
namespace witness_sense {
#include "sha_fail_decls_canary-sense.inc"
PROJECT_KEY_LOAD
#include "sha_fail_funcs_canary-sense.inc"
}  // namespace witness_sense
namespace witness_vision {
#include "sha_fail_decls_canary-vision.inc"
PROJECT_KEY_LOAD
#include "sha_fail_funcs_canary-vision.inc"
}  // namespace witness_vision
namespace witness_sentinel {
#include "sha_fail_decls_canary-sentinel.inc"
PROJECT_KEY_LOAD
#include "sha_fail_funcs_canary-sentinel.inc"
}  // namespace witness_sentinel
}  // namespace canary

// ════════════════════════════════════════════════════════════════════════════
// PART 1 — the helpers
// ════════════════════════════════════════════════════════════════════════════

struct Helper {
  const char* name;
  int updates;
  // Runs the helper over fixed input into out; answers its answer.
  bool (*run)(uint8_t out[32]);
  // The same digest through OpenSSL.
  void (*expect)(uint8_t out[32]);
};
static const uint8_t kData[5] = {'w', 'i', 't', 'n', 's'};
static const Helper kHelpers[] = {
    {"sha256_raw", 1,
     [](uint8_t out[32]) { return answered([&] { return sha256_raw(kData, sizeof(kData), out); }); },
     [](uint8_t out[32]) { openssl_sha(nullptr, kData, sizeof(kData), out); }},
    {"sha256_domain", 3,
     [](uint8_t out[32]) {
       return answered([&] { return sha256_domain("securacv:test:v1", kData, sizeof(kData), out); });
     },
     [](uint8_t out[32]) { openssl_sha("securacv:test:v1", kData, sizeof(kData), out); }},
    {"sha256_domain, no data", 2,
     [](uint8_t out[32]) {
       return answered([&] { return sha256_domain("securacv:test:v1", nullptr, 0, out); });
     },
     [](uint8_t out[32]) { openssl_sha("securacv:test:v1", nullptr, 0, out); }},
    {"compute_chain_hash", 3,
     [](uint8_t out[32]) {
       uint8_t prev[32], ph[32];
       std::memset(prev, 0x11, 32);
       std::memset(ph, 0x22, 32);
       return answered([&] { return compute_chain_hash(prev, ph, 7, 9, out); });
     },
     [](uint8_t out[32]) {
       uint8_t prev[32], ph[32];
       std::memset(prev, 0x11, 32);
       std::memset(ph, 0x22, 32);
       expected_link(prev, ph, 7, 9, out);
     }},
    {"wc_sha256_domain", 3,
     [](uint8_t out[32]) {
       return answered([&] { return wc_sha256_domain("securacv:test:v1", kData, sizeof(kData), out); });
     },
     [](uint8_t out[32]) { openssl_sha("securacv:test:v1", kData, sizeof(kData), out); }},
    {"wc_chain_advance", 3,
     [](uint8_t out[32]) {
       uint8_t prev[32], ph[32];
       std::memset(prev, 0x11, 32);
       std::memset(ph, 0x22, 32);
       return answered([&] { return wc_chain_advance(prev, ph, 7, 9, out); });
     },
     [](uint8_t out[32]) {
       uint8_t prev[32], ph[32];
       std::memset(prev, 0x11, 32);
       std::memset(ph, 0x22, 32);
       expected_link(prev, ph, 7, 9, out);
     }},
    {"wc_genesis", 3,
     [](uint8_t out[32]) { return answered([&] { return wc_genesis("canary-test-7f", out); }); },
     [](uint8_t out[32]) {
       openssl_sha("securacv:genesis:v1", (const uint8_t*)"canary-test-7f", 14, out);
     }},
};

static void test_the_helpers_answer_and_zero() {
  for (const Helper& h : kHelpers) {
    g_case = std::string(h.name) + ", no failure";
    sha_stub::disarm();
    uint8_t out[32], want[32];
    std::memset(out, 0xCC, sizeof(out));
    CHECK(h.run(out));
    h.expect(want);
    CHECK(std::memcmp(out, want, 32) == 0);
    CHECK(sha_stub::g_inits == 1 && sha_stub::g_frees == 1 && sha_stub::g_live == 0);
    for (const Fault& f : faults_for(h.updates)) {
      g_case = std::string(h.name) + ", " + f.name();
      arm(f, 0);
      std::memset(out, 0xCC, sizeof(out));
      CHECK(!h.run(out));
      CHECK(all_zero(out, sizeof(out)));
      check_the_hash_stopped();
    }
  }
  // The fingerprint: 8 zero bytes and false.
  for (const Fault& f : faults_for(3)) {
    g_case = "crypto_fingerprint, " + f.name();
    arm(f, 0);
    uint8_t pub[32], fp[8];
    std::memset(pub, 0x5E, sizeof(pub));
    std::memset(fp, 0xCC, sizeof(fp));
    CHECK(!answered([&] { return crypto_fingerprint(pub, fp); }));
    CHECK(all_zero(fp, sizeof(fp)));
    check_the_hash_stopped();
  }
  g_case = "crypto_fingerprint, no failure";
  sha_stub::disarm();
  uint8_t pub[32], fp[8], want[32];
  std::memset(pub, 0x5E, sizeof(pub));
  CHECK(answered([&] { return crypto_fingerprint(pub, fp); }));
  openssl_sha("securacv:pubkey:fingerprint", pub, 32, want);
  CHECK(std::memcmp(fp, want, 8) == 0);
}

// ════════════════════════════════════════════════════════════════════════════
// PART 2 — the canary (PIO) tree's callers
// ════════════════════════════════════════════════════════════════════════════

static void reset_flagship() {
  g_device = DeviceIdentity();
  g_health = SystemHealth();
  g_last_record = WitnessRecord();
  g_record_ring_head = g_record_ring_count = 0;
  g_nvs_u32.clear();
  g_nvs_bytes.clear();
  g_nvs_store_u32 = g_nvs_store_bytes = 0;
  g_named = g_auth_derives = g_signs = g_sd_appends = 0;
  g_log.clear();
  Serial.lines.clear();
  SD = HostSd();
  sha_stub::disarm();
}

// A device mid-chain: seq 19, the last persist at 10, so the next record's
// persist is due (and a refused record's would be).
static void mid_chain() {
  reset_flagship();
  establish_identity_key();
  Ed25519::derivePublicKey(g_device.pubkey, g_device.privkey);
  g_device.seq = 19;
  g_device.seq_persisted = 10;
  std::memset(g_device.chain_head, 0x6B, 32);
}

static const uint8_t kPayload[24] = {0xA2, 'k', 'e', 'y', 0x01, 'v', 'a', 'l', 'u', 'e', 0x02, 0x03,
                                     0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
                                     0x0E, 0x0F};

static void test_a_record_whose_hash_fails_is_not_made() {
  // Hash 0 is the payload's, hash 1 the link's.
  for (int hash = 0; hash < 2; hash++) {
    for (const Fault& f : faults_for(3)) {
      g_case = std::string("record, ") + (hash == 0 ? "payload hash " : "chain link ") + f.name();
      mid_chain();
      const DeviceIdentity before = g_device;
      WitnessRecord out;
      std::memset(&out, 0x3C, sizeof(out));
      WitnessRecord out_before;
      std::memcpy(&out_before, &out, sizeof(out));
      arm(f, hash);
      CHECK(!witness_create_record_gps(kPayload, sizeof(kPayload), RECORD_WITNESS_EVENT, nullptr, &out));
      check_the_hash_stopped();
      CHECK(g_device.seq == before.seq);
      CHECK(std::memcmp(g_device.chain_head, before.chain_head, 32) == 0);
      CHECK(g_device.seq_persisted == before.seq_persisted);
      CHECK(std::memcmp(&out, &out_before, sizeof(out)) == 0);  // *out as it was
      CHECK(g_signs == 0);
      CHECK(g_record_ring_count == 0 && g_health.records_created == 0);
      CHECK(g_last_record.seq == 0);
      CHECK(g_sd_appends == 0);
      CHECK(g_nvs_store_bytes == 0 && g_nvs_store_u32 == 0);

      // The next record takes the same seq from the same head.
      sha_stub::disarm();
      CHECK(witness_create_record_gps(kPayload, sizeof(kPayload), RECORD_WITNESS_EVENT, nullptr, &out));
      uint8_t ph[32], link[32];
      openssl_sha("securacv:fw:payload:v1", kPayload, sizeof(kPayload), ph);
      expected_link(before.chain_head, ph, before.seq + 1, g_tb, link);
      CHECK(out.seq == before.seq + 1 && g_device.seq == before.seq + 1);
      CHECK(std::memcmp(out.prev_hash, before.chain_head, 32) == 0);
      CHECK(std::memcmp(out.payload_hash, ph, 32) == 0);
      CHECK(std::memcmp(out.chain_hash, link, 32) == 0);
      CHECK(std::memcmp(g_device.chain_head, link, 32) == 0);
      CHECK(out.verified && g_signs == 1);
      CHECK(g_record_ring_count == 1 && g_sd_appends == 1);
      CHECK(g_nvs_store_bytes == 1 && g_device.seq_persisted == before.seq + 1);
    }
  }
}

// A signed tail line at `seq`. With zero_ch its chain hash field is 32 zero
// bytes (signed as such), which only a zeroed recompute matches: the line a
// check that compared bytes, and did not read the helper's answer, would
// adopt on a failed hash.
static std::string tail_line(uint32_t seq, uint32_t tb, uint8_t ch_out[32], bool zero_ch = false) {
  uint8_t prev[32], ph[32], sig[64];
  std::memset(prev, 0x71, 32);
  std::memset(ph, 0x72, 32);
  expected_link(prev, ph, seq, tb, ch_out);
  if (zero_ch) std::memset(ch_out, 0, 32);
  crypto_sign(g_device.privkey, g_device.pubkey, ch_out, 32, sig);
  g_signs = 0;
  char line[witness_store::RECORD_LINE_MAX + 1];
  const size_t n = witness_store::line_build(line, sizeof(line), seq, tb, 1, ph, prev, ch_out, sig);
  return std::string(line, n);
}

static void test_the_sd_tail_is_not_adopted_on_a_failed_recompute() {
  for (const Fault& f : faults_for(3)) {
    g_case = "SD tail recovery, " + f.name();
    mid_chain();
    g_device.seq = 5;
    uint8_t ch[32];
    SD.has = true;
    SD.records = tail_line(9, 77, ch);
    const DeviceIdentity before = g_device;
    arm(f, 0);
    CHECK(!witness_recover_from_sd());
    check_the_hash_stopped();
    CHECK(g_device.seq == 5);
    CHECK(std::memcmp(g_device.chain_head, before.chain_head, 32) == 0);
    CHECK(g_nvs_store_bytes == 0);
    CHECK(logged("Witness SD tail ignored: chain hash mismatch", "SHA-256 failed"));

    // A tail whose chain hash field is zero matches the zeroed output byte
    // for byte: only the answer refuses it.
    g_case = "SD tail recovery, zero ch, " + f.name();
    mid_chain();
    g_device.seq = 5;
    SD.has = true;
    SD.records = tail_line(9, 77, ch, /*zero_ch=*/true);
    arm(f, 0);
    CHECK(!witness_recover_from_sd());
    CHECK(g_device.seq == 5 && g_nvs_store_bytes == 0);
  }
  g_case = "SD tail recovery, no failure";
  mid_chain();
  g_device.seq = 5;
  uint8_t ch[32];
  SD.has = true;
  SD.records = tail_line(9, 77, ch);
  CHECK(witness_recover_from_sd());
  CHECK(g_device.seq == 9 && std::memcmp(g_device.chain_head, ch, 32) == 0);
  CHECK(g_nvs_store_bytes == 1);
  CHECK(logged("Witness chain head recovered from SD", "seq 9"));
}

static void test_chain_ok_is_red_on_a_failed_recompute() {
  mid_chain();
  WitnessRecord rec = WitnessRecord();
  rec.seq = 3;
  rec.time_bucket = 11;
  std::memset(rec.prev_hash, 0x21, 32);
  std::memset(rec.payload_hash, 0x22, 32);
  expected_link(rec.prev_hash, rec.payload_hash, rec.seq, rec.time_bucket, rec.chain_hash);
  crypto_sign(g_device.privkey, g_device.pubkey, rec.chain_hash, 32, rec.signature);
  g_device.seq = 3;
  std::memcpy(g_device.chain_head, rec.chain_hash, 32);
  g_health.crypto_healthy = true;
  g_last_record = rec;
  g_case = "test_chain, no failure";
  sha_stub::disarm();
  CHECK(diag::test_chain());
  for (const Fault& f : faults_for(3)) {
    g_case = "test_chain, " + f.name();
    arm(f, 0);
    CHECK(!diag::test_chain());
    check_the_hash_stopped();
  }
  // A record whose chain hash is zero (and so the head, signed as such)
  // matches the zeroed output byte for byte: only the answer turns it red.
  std::memset(rec.chain_hash, 0, 32);
  crypto_sign(g_device.privkey, g_device.pubkey, rec.chain_hash, 32, rec.signature);
  std::memset(g_device.chain_head, 0, 32);
  g_last_record = rec;
  for (const Fault& f : faults_for(3)) {
    g_case = "test_chain, zero chain hash, " + f.name();
    arm(f, 0);
    CHECK(!diag::test_chain());
  }
}

static void test_provisioning_refuses_a_fingerprint_that_did_not_compute() {
  for (const Fault& f : faults_for(3)) {
    g_case = "provisioning, fingerprint " + f.name();
    reset_flagship();
    arm(f, 0);  // the first hash provisioning makes
    CHECK(!witness_provision_device());
    check_the_hash_stopped();
    CHECK(g_device.device_id[0] == '\0' && g_device.ap_ssid[0] == '\0');
    CHECK(g_named == 0);
    CHECK(all_zero(g_device.pubkey_fp, sizeof(g_device.pubkey_fp)));
    CHECK(g_nvs_store_u32 == 0 && g_nvs_store_bytes == 0);  // no boot count, no chain
    CHECK(g_auth_derives == 0);
    CHECK(!g_device.initialized);
    CHECK(Serial.said("[!!] Public key fingerprint not computed (SHA-256 failed)"));
  }
}

static void test_provisioning_refuses_a_genesis_head_that_did_not_compute() {
  for (const Fault& f : faults_for(3)) {
    g_case = "provisioning, genesis " + f.name();
    reset_flagship();  // no chain stored: chain_state::choose() answers Genesis
    arm(f, 1);         // the fingerprint is hash 0, the genesis head hash 1
    CHECK(!witness_provision_device());
    check_the_hash_stopped();
    CHECK(all_zero(g_device.chain_head, 32));
    CHECK(g_nvs_store_u32 == 0 && g_nvs_store_bytes == 0);  // nothing persisted
    CHECK(g_auth_derives == 0);
    CHECK(!g_device.initialized);
    CHECK(Serial.said("[!!] Genesis chain head not computed (SHA-256 failed)"));
  }
  // Every step runs: named, the genesis head written once, the boot counted.
  g_case = "provisioning, no failure";
  reset_flagship();
  CHECK(witness_provision_device());
  uint8_t genesis[32];
  openssl_sha("securacv:genesis:v1", (const uint8_t*)g_device.device_id,
              std::strlen(g_device.device_id), genesis);
  CHECK(std::memcmp(g_device.chain_head, genesis, 32) == 0);
  CHECK(g_named == 2 && g_device.device_id[0] != '\0');
  CHECK(g_nvs_store_bytes == 1 && g_nvs_store_u32 == 1);
  CHECK(g_auth_derives == 1 && g_device.initialized);
  // A stored chain makes no genesis hash: one hash, the fingerprint.
  g_case = "provisioning, stored chain";
  reset_flagship();
  uint8_t head[32], blob[chain_state::BLOB_LEN];
  std::memset(head, 0x44, 32);
  CHECK(chain_state::encode(30, head, blob));
  g_nvs_bytes[NVS_KEY_CHAINST] = std::vector<uint8_t>(blob, blob + sizeof(blob));
  CHECK(witness_provision_device());
  CHECK(sha_stub::g_inits == 1);
  CHECK(g_device.seq == 30 && std::memcmp(g_device.chain_head, head, 32) == 0);
}

static void test_the_certificate_pin_is_refused() {
  uint8_t cert[600];
  for (size_t i = 0; i < sizeof(cert); i++) cert[i] = (uint8_t)(i * 7 + 1);
  char hex[65];
  g_case = "certificate pin, no failure";
  sha_stub::disarm();
  CHECK(answered([&] { return tls_cert_fingerprint(cert, sizeof(cert), hex); }));
  uint8_t want[32];
  openssl_sha(nullptr, cert, sizeof(cert), want);
  char want_hex[65];
  canary::net::tls_policy::fingerprint_hex(want, want_hex);
  CHECK(std::strcmp(hex, want_hex) == 0);
  for (const Fault& f : faults_for(1)) {
    g_case = "certificate pin, " + f.name();
    std::memset(hex, 'z', 64);
    hex[64] = '\0';
    arm(f, 0);
    CHECK(!answered([&] { return tls_cert_fingerprint(cert, sizeof(cert), hex); }));
    CHECK(hex[0] == '\0');
    check_the_hash_stopped();
  }
}

// ════════════════════════════════════════════════════════════════════════════
// PART 3 — canary-sense, canary-vision, canary-sentinel
// ════════════════════════════════════════════════════════════════════════════

struct Project {
  const char* name;
  bool (*init)();
  void (*advance)(uint32_t seq);  // one witnessed transition at seq
  void (*canonical)(uint32_t seq, char* out, size_t cap);
  uint8_t* priv;
  uint8_t* head;
  uint32_t* length;
  bool* ready;
};
static const char* kEvent = "presence_changed";
static const uint32_t kBucket = 1200;
static SentinelClaim sentinel_claim() {
  SentinelClaim c;
  c.level = "present";
  c.confidence = 61;
  c.anomaly = 4;
  c.occupancy = "1";
  c.range = "near";
  c.modality_bits = 5;
  return c;
}
static void sense_canonical(uint32_t seq, char* out, size_t cap) {
  device_signature::build_sense_canonical(seq, kEvent, "present", "1", "near", kBucket,
                                          canary::cfg::get().device_id, out, cap);
}
static void sentinel_canonical(uint32_t seq, char* out, size_t cap) {
  const SentinelClaim c = sentinel_claim();
  device_signature::build_sentinel_canonical(seq, c.event, c.level, c.confidence, c.anomaly,
                                             c.occupancy, c.range, c.modality_bits, kBucket,
                                             canary::cfg::get().device_id, out, cap);
}
static const Project kProjects[] = {
    {"canary-sense", canary::witness_sense::init,
     [](uint32_t seq) {
       canary::witness_sense::chain_advance(seq, kEvent, "present", "1", "near", kBucket);
     },
     sense_canonical, canary::witness_sense::s_priv, canary::witness_sense::s_head,
     &canary::witness_sense::s_length, &canary::witness_sense::s_ready},
    {"canary-vision", canary::witness_vision::init,
     [](uint32_t seq) {
       canary::witness_vision::chain_advance(seq, kEvent, "present", "1", "near", kBucket);
     },
     sense_canonical, canary::witness_vision::s_priv, canary::witness_vision::s_head,
     &canary::witness_vision::s_length, &canary::witness_vision::s_ready},
    {"canary-sentinel", canary::witness_sentinel::init,
     [](uint32_t seq) { canary::witness_sentinel::chain_advance(seq, sentinel_claim(), kBucket); },
     sentinel_canonical, canary::witness_sentinel::s_priv, canary::witness_sentinel::s_head,
     &canary::witness_sentinel::s_length, &canary::witness_sentinel::s_ready},
};

static void reset_project(const Project& p) {
  host_prefs().reset();
  *p.ready = false;
  *p.length = 0;
  std::memset(p.head, 0, 32);
  std::memset(p.priv, 0, 32);
  g_signer_inits = 0;
  g_project_key_ok = true;
  Serial.lines.clear();
  sha_stub::disarm();
}

static void test_project_init_refuses(const Project& p) {
  // Hash 0 is the fingerprint, hash 1 the genesis head (no chain stored).
  for (int hash = 0; hash < 2; hash++) {
    for (const Fault& f : faults_for(3)) {
      g_case = std::string(p.name) + " init, " + (hash == 0 ? "fingerprint " : "genesis ") + f.name();
      reset_project(p);
      arm(f, hash);
      CHECK(!p.init());
      check_the_hash_stopped();
      CHECK(!*p.ready);
      CHECK(all_zero(p.priv, 32));    // the key scrubbed
      CHECK(g_signer_inits == 0);     // before the signer saw it
      CHECK(host_prefs().puts == 0);  // nothing written
      CHECK(Serial.said(hash == 0 ? "Fingerprint not computed (SHA-256 failed)"
                                  : "Genesis head not computed (SHA-256 failed)"));
      CHECK(Serial.said("signing disabled this boot."));
      // A refused boot chains nothing.
      sha_stub::disarm();
      p.advance(1);
      CHECK(*p.length == 0 && host_prefs().puts == 0);
    }
  }
  g_case = std::string(p.name) + " init, no failure";
  reset_project(p);
  CHECK(p.init());
  CHECK(*p.ready && g_signer_inits == 1 && all_zero(p.priv, 32));
  uint8_t genesis[32];
  openssl_sha("securacv:genesis:v1", (const uint8_t*)canary::cfg::get().device_id,
              std::strlen(canary::cfg::get().device_id), genesis);
  CHECK(std::memcmp(p.head, genesis, 32) == 0 && *p.length == 0);
}

static void test_project_chain_advance_refuses(const Project& p) {
  // Hash 0 is the payload's, hash 1 the link's.
  for (int hash = 0; hash < 2; hash++) {
    for (const Fault& f : faults_for(3)) {
      g_case = std::string(p.name) + " chain_advance, " + (hash == 0 ? "payload hash " : "link ") +
               f.name();
      reset_project(p);
      CHECK(p.init());
      p.advance(1);
      p.advance(2);
      uint8_t head[32];
      std::memcpy(head, p.head, 32);
      const uint32_t length = *p.length;
      const int puts = host_prefs().puts;
      const std::map<std::string, std::vector<uint8_t>> stored = host_prefs().blobs;
      arm(f, hash);
      p.advance(3);
      check_the_hash_stopped();
      CHECK(std::memcmp(p.head, head, 32) == 0);
      CHECK(*p.length == length);
      CHECK(host_prefs().puts == puts);  // nothing persisted
      CHECK(host_prefs().blobs == stored);

      // The next record links from the same head.
      sha_stub::disarm();
      p.advance(3);
      char canon[256];
      p.canonical(3, canon, sizeof(canon));
      uint8_t ph[32], link[32];
      openssl_sha("securacv:fw:payload:v1", (const uint8_t*)canon, std::strlen(canon), ph);
      expected_link(head, ph, 3, kBucket, link);
      CHECK(std::memcmp(p.head, link, 32) == 0);
      CHECK(*p.length == length + 1);
      CHECK(host_prefs().puts == puts + 2);  // head and length
      CHECK(host_prefs().blobs["chain_head"] == std::vector<uint8_t>(link, link + 32));
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
// PART 4 — the sources: every call uses its answer
// ════════════════════════════════════════════════════════════════════════════

static std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
static bool ends_with(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}
static bool contains(const std::string& s, const std::string& part) {
  return s.find(part) != std::string::npos;
}
static size_t count_of(const std::string& s, const std::string& needle) {
  size_t n = 0;
  for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) n++;
  return n;
}
static std::string body_of(const std::string& src, const std::string& head) {
  const size_t at = src.find(head);
  if (at == std::string::npos) return "";
  const size_t end = src.find("\n}\n", at);
  return end == std::string::npos ? "" : src.substr(at, end + 3 - at);
}
// The code lines of a source: comment lines dropped, trailing // comments cut.
static std::vector<std::string> code_lines(const std::string& src) {
  std::vector<std::string> out;
  std::istringstream lines(src);
  std::string line;
  bool in_block = false;
  while (std::getline(lines, line)) {
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) continue;
    if (in_block) {
      if (contains(line, "*/")) in_block = false;
      continue;
    }
    if (line.compare(first, 2, "/*") == 0) {
      if (!contains(line, "*/")) in_block = true;
      continue;
    }
    if (line.compare(first, 2, "//") == 0 || line.compare(first, 1, "*") == 0) continue;
    const size_t cmt = line.find("//");
    out.push_back(cmt == std::string::npos ? line : line.substr(0, cmt));
  }
  return out;
}
// A call that uses its answer: `if (!f(`, `return f(`, `= f(` (a kept
// bool), `&& f(` or `|| f(`.
static bool uses_answer(const std::string& line, size_t at) {
  std::string before = line.substr(0, at);
  while (!before.empty() && (before.back() == ' ' || before.back() == '\t')) before.pop_back();
  return ends_with(before, "if (!") || ends_with(before, "return") || ends_with(before, "=") ||
         ends_with(before, "&&") || ends_with(before, "||") || ends_with(before, "(!");
}

struct CallRule {
  const char* name;  // "sha256_domain(" and so on
  int expected;      // the calls that must exist across the files read
};

// Checks every call of each name in `files` (a definition, a declaration, a
// call qualified by another namespace and the mesh library's own
// sha256_domain are not calls of these) and that the count is as expected.
static void check_calls(const std::vector<std::string>& files, const std::vector<CallRule>& rules,
                        const char* what) {
  for (const CallRule& r : rules) {
    int calls = 0;
    for (const std::string& f : files) {
      if (contains(f, "/securacv_mesh/") || contains(f, "/securacv_ble_scan/")) continue;
      for (const std::string& line : code_lines(read_file(f))) {
        for (size_t at = line.find(r.name); at != std::string::npos; at = line.find(r.name, at + 1)) {
          if (at > 0 && (std::isalnum((unsigned char)line[at - 1]) || line[at - 1] == '_' ||
                         line[at - 1] == ':'))
            continue;  // another name ending in this one, or a qualified call
          const std::string before = line.substr(0, at);
          if (contains(before, "bool ") || contains(before, "void ") || contains(before, "int "))
            if (!contains(before, "=") && !contains(before, "return") && !contains(before, "if ("))
              continue;  // a definition or a declaration
          calls++;
          if (!uses_answer(line, at)) {
            std::printf("  %s: %s drops its answer: %s\n", what, f.c_str(), line.c_str());
            CHECK(false);
          }
        }
      }
    }
    if (calls != r.expected)
      std::printf("  %s: %d calls of %s, expected %d\n", what, calls, r.name, r.expected);
    CHECK(calls == r.expected);
  }
}

static void test_the_sources(const std::vector<std::string>& files) {
  g_case = "sources";
  std::vector<std::string> canary, projects;
  std::string network;
  for (const std::string& f : files) {
    if (contains(f, "/projects/canary-sense/") || contains(f, "/projects/canary-vision/") ||
        contains(f, "/projects/canary-sentinel/"))
      projects.push_back(f);
    else
      canary.push_back(f);
    if (ends_with(f, "lib/securacv_network/src/securacv_network.cpp")) network = read_file(f);
  }
  CHECK(canary.size() > 50 && projects.size() == 3);
  CHECK(!network.empty());

  // The canary tree: each helper's callers use its answer. sha256_raw():
  // tls_cert_fingerprint(); sha256_domain(): crypto_fingerprint(),
  // compute_chain_hash(), the genesis head, the record payload;
  // crypto_fingerprint(): provisioning; compute_chain_hash(): the link, the
  // SD tail, test_chain().
  check_calls(canary,
              {{"sha256_raw(", 1}, {"sha256_domain(", 4}, {"crypto_fingerprint(", 1},
               {"compute_chain_hash(", 3}, {"update_chain(", 1}},
              "canary tree");
  // The three projects: two wrapper calls (fingerprint, payload), the
  // wrapper's own wc_sha256_domain(), one wc_chain_advance() and one
  // wc_genesis() each, and init()'s load_or_start_chain().
  check_calls(projects,
              {{"sha256_domain(", 6}, {"wc_sha256_domain(", 3}, {"wc_chain_advance(", 3},
               {"wc_genesis(", 3}, {"load_or_start_chain(", 3}},
              "projects");

  // initTls(): the pin first, its refusal before the certificate is kept.
  const std::string init = body_of(network, "\nbool ScvNetworkManager::initTls() {\n");
  CHECK(!init.empty());
  const size_t pin = init.find("  if (!tls_cert_fingerprint(cert, cert_len, m_tls_cert_fp_hex)) {\n");
  const size_t kept = init.find("  m_tls_cert_der = cert;\n");
  CHECK(pin != std::string::npos && kept != std::string::npos && pin < kept);
  CHECK(count_of(init, "tls_cert_fingerprint(") == 1);
  if (pin != std::string::npos && kept != std::string::npos && pin < kept) {
    const std::string refused = init.substr(pin, kept - pin);
    for (const char* step : {"    secure_zero(key, key_len);\n", "    secure_zero(cert, cert_len);\n",
                             "    free(key);\n", "    free(cert);\n",
                             "    m_tls_reason = \"certificate fingerprint not computed (SHA-256 failed)",
                             "    return false;\n"})
      CHECK(contains(refused, step));
    CHECK(!contains(refused, "m_tls_cert_der ="));
    CHECK(!contains(refused, "m_tls_key_der ="));
  }
  // The pin is computed nowhere else in the file.
  int raw_calls = 0;
  for (const std::string& line : code_lines(network))
    if (contains(line, "sha256_raw(")) raw_calls++;
  CHECK(raw_calls == 1);
}

int main(int argc, char** argv) {
  std::printf("test_sha_fail_flagship: mbedTLS %s spelling (MBEDTLS_VERSION_NUMBER 0x%08X)\n",
              MBEDTLS_VERSION_NUMBER >= 0x03000000 ? "3.x" : "2.x", (unsigned)MBEDTLS_VERSION_NUMBER);
  test_the_helpers_answer_and_zero();
  test_a_record_whose_hash_fails_is_not_made();
  test_the_sd_tail_is_not_adopted_on_a_failed_recompute();
  test_chain_ok_is_red_on_a_failed_recompute();
  test_provisioning_refuses_a_fingerprint_that_did_not_compute();
  test_provisioning_refuses_a_genesis_head_that_did_not_compute();
  test_the_certificate_pin_is_refused();
  for (const Project& p : kProjects) {
    test_project_init_refuses(p);
    test_project_chain_advance_refuses(p);
  }
  test_the_sources(std::vector<std::string>(argv + 1, argv + argc));
  if (g_failures) {
    std::printf("test_sha_fail_flagship: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("test_sha_fail_flagship: all %d checks passed\n", g_checks);
  return 0;
}
