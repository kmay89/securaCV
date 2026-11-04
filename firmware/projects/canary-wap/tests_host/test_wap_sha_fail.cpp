// canary-wap's SHA-256 fails closed (sweep F364, the sketch's slice).
//
// Before, the sketch's sha256_raw() and sha256_domain() (canary_wap.ino)
// returned void and checked none of mbedtls_sha256_starts(), _update() and
// _finish(). On the S3 and C3 the SHA engine is hardware and each step can
// answer an error (an update over a long input runs by DMA); after one the
// output is not, or not wholly, written, so it keeps whatever the caller's
// stack held. Five callers used those bytes: the public key's fingerprint
// (the device id, the SoftAP and mDNS names), the genesis chain head
// (persisted to NVS on a first boot), the TLS certificate's fingerprint (the
// pin the setup door hands a phone for the life of the pairing), the chain
// link and the record payload hash (every witness record). Now both helpers
// answer whether every step ran, zero their output when one did not and
// free the context on every path, and every caller refuses what the hash
// was for.
//
// This runs the REAL functions, cut verbatim out of the sketch
// (firmware/tests_host/cut_functions.awk): hex_print(), secure_zero(),
// hex_to_str(), sha256_raw(), sha256_domain(), compute_fingerprint(),
// tls_compute_cert_fingerprint(), tls_refuse_unpinned_cert(),
// compute_chain_hash(), update_chain(), persist_chain_blob(),
// persist_chain_state(), witness_hash_failed() and its latch,
// create_witness_record(), witness_recover_from_sd(),
// verify_record_signature(), chain_tail_intact(), the RecordMade enum,
// witness_record_last(), record_boot_attestation(),
// self_verify_last_record(), fingerprint_is_nonzero(),
// ble_discovery_identity_ready(), tls_after_setup(), init_tls_cert()
// (twice: a build that can generate a certificate and one that cannot) and
// provision_device(). Under them is a stand-in for mbedTLS's
// streaming SHA-256 (declared and defined below) that runs a real SHA-256
// over OpenSSL, or fails the step the test names in the hash it names, once
// (a failing finish scribbles first; a failed update feeds nothing; every
// other step runs as it would have, so a helper that drops one answer goes
// on to a finish that answers 0 over the wrong input), the fake SD card
// (stubs/sd_fake) and stand-ins for NVS, the signer and the rest. What it
// pins:
//   - sha256_raw() and sha256_domain(): known answers when every step runs;
//     false and 32 zero bytes at a failing start, each update and the
//     finish, the context freed on every path;
//   - the public key's fingerprint: provisioning answers false, the device
//     is not named (no device id, SoftAP or mDNS name, no fingerprint, no
//     "Public key fingerprint" line), the chain is refused, nothing is
//     written to NVS but this boot's count (no genesis head), and no token
//     or SoftAP key is derived; a stored chain is still loaded, so seq and
//     head are as NVS holds them;
//   - the genesis head: the same refusal, the head never persisted;
//   - a refused chain: the boot attestation record and every later one is
//     not made, and the reboot, safe-mode and deep-sleep persist writes
//     nothing;
//   - the record payload hash and the chain link: no record (no seq or head
//     advance, nothing in the page ring, nothing appended to the card, no
//     signature, no persist, *out as it was), one log line per run of
//     failures, and the next record extends the head as if the failed one
//     had not been tried;
//   - the boot's SD-tail recovery and chain_tail_intact(): a link they
//     cannot recompute is not adopted or vouched for, and a refused chain
//     is not reported intact;
//   - the TLS certificate's fingerprint, loaded or generated: init_tls_cert()
//     answers false, the hex is empty, the certificate is not marked
//     persisted and both buffers are freed, so start_http_server() has no
//     certificate to serve and tls_after_setup() hands out no pin in setup
//     or out of it; no "Cert fingerprint" line is printed;
//   - what that boot shows and broadcasts (the F364 review): BLE discovery's
//     gate, ble_discovery_identity_ready(), is shut after either refusal, so
//     Opera's name and device-info id and every chirp's id and chain head
//     never carry the zeroed fingerprint or head; the boot attestation's
//     line says it was not recorded, never "[OK]" over setup()'s zeroed
//     record; loop()'s records are Refused (no table row) and logged once;
//     and the periodic self-verify says nothing and leaves signing marked
//     unhealthy. On a working unit the gate opens, the "[OK]" line names the
//     record made, and self-verify passes it and catches a bad signature;
//     a record whose hash fails is Refused with g_last_record as it was, and
//     one made and not verified is Unverified and logged;
//   - a certificate generated this boot whose pin did not compute is not
//     stored, so no later boot serves a certificate no setup door pinned;
//   - the sketch's text: every call of the six hashes uses its answer, each
//     is declared [[nodiscard]], setup() turns HTTPS on only behind
//     init_tls_cert()'s true, the HTTPS listener needs both buffers, no
//     mDNS TXT record carries the certificate's fingerprint, the one
//     ble_manager::init() call comes after the gate's refusal branch, and
//     setup()'s boot attestation, loop()'s record, its row, its status bar
//     and its self-verify go through the helpers above.
// Host-tested; not run on a device.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run-wap-sha-fail

#include "arduino_globals.h"

#include <dirent.h>
#include <openssl/evp.h>

#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "FS.h"              // stubs/sd_fake
#include "SD.h"              // stubs/sd_fake
#include "chain_persist.h"   // the sketch's when-to-persist rules, pure
#include "chain_state.h"     // the sketch's {seq, head} blob, pure
#include "log_level.h"       // the sketch's LogLevel / LogCategory
#include "witness_page.h"    // the sketch's page ring, pure
#include "witness_store.h"   // the sketch's /WITNESS line format, pure

#ifndef WAP_SHA_INC
#error "WAP_SHA_INC must name the cut of canary_wap.ino (the Makefile passes it)"
#endif
#ifndef WAP_SHA_TLS_INC
#error "WAP_SHA_TLS_INC must name the cut of init_tls_cert() (the Makefile passes it)"
#endif
#ifndef WAP_SHA_TLS_AFTER_INC
#error "WAP_SHA_TLS_AFTER_INC must name the cut of tls_after_setup() (the Makefile passes it)"
#endif
#ifndef WAP_SHA_PROV_INC
#error "WAP_SHA_PROV_INC must name the cut of provision_device() (the Makefile passes it)"
#endif
#ifndef WAP_SKETCH_DIR
#error "WAP_SKETCH_DIR must name the sketch directory (the Makefile passes it)"
#endif

// ── the test's own bookkeeping ───────────────────────────────────────────────
namespace {
int g_checks = 0;
int g_failures = 0;
#define CHECK_MSG(c, msg)                                                 \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      ++g_failures;                                                       \
      std::printf("FAIL %s:%d: %s -- %s\n", __FILE__, __LINE__, #c,       \
                  std::string(msg).c_str());                              \
    }                                                                     \
  } while (0)
#define CHECK(c) CHECK_MSG(c, "")
}  // namespace

static std::string hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < n; i++) {
    out += d[p[i] >> 4];
    out += d[p[i] & 15];
  }
  return out;
}

static bool all_zero(const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++) {
    if (b[i] != 0) return false;
  }
  return true;
}

// ── mbedTLS's streaming SHA-256, real or failing at a named step ─────────────
// Each mbedtls_sha256_init() starts a hash, numbered from the last arm(). The
// hash arm() names fails at its step, once: the start, the update it names
// (counting from 0) or the finish, which writes 0xA5 over the output first
// (so the zeros a caller sees are the helper's own). Every other step runs
// as it would have, over what it was fed: mbedTLS does not promise that a
// step after a failed one fails, and IDF's DMA port keeps no error in the
// context (the F364 review). So code that drops a failed update's answer
// goes on to a finish that answers 0 over input missing that update (a
// wrong digest, answered true), and code that drops a failed start's answer
// gets the right digest, also answered true: either way the helper's answer
// and its output give it away.
namespace sha {
enum class Step { None, Starts, Update, Finish };
const int kFailed = -0x0037;  // mbedTLS's MBEDTLS_ERR_SHA256_HW_ACCEL_FAILED
Step g_fail = Step::None;
int g_fail_hash = 0;
int g_fail_update = 0;
int g_hashes = 0;  // inits since arm()
int g_inits = 0;
int g_frees = 0;
int g_live = 0;    // contexts inited and not freed
bool g_fired = false;

void arm(Step fail, int hash = 0, int update = 0) {
  g_fail = fail;
  g_fail_hash = hash;
  g_fail_update = update;
  g_hashes = g_inits = g_frees = 0;
  g_fired = false;
}

std::string name_of(Step s, int update = 0) {
  switch (s) {
    case Step::None: return "none";
    case Step::Starts: return "mbedtls_sha256_starts fails";
    case Step::Update: return "mbedtls_sha256_update #" + std::to_string(update + 1) + " fails";
    case Step::Finish: return "mbedtls_sha256_finish fails";
  }
  return "?";
}
}  // namespace sha

struct mbedtls_sha256_context {
  EVP_MD_CTX* md;
  int index;
  int updates;
};

static bool failing_here(const mbedtls_sha256_context* ctx, sha::Step step) {
  if (sha::g_fail != step || ctx->index != sha::g_fail_hash) return false;
  if (step == sha::Step::Update && ctx->updates != sha::g_fail_update) return false;
  sha::g_fired = true;
  return true;
}

void mbedtls_sha256_init(mbedtls_sha256_context* ctx) {
  ++sha::g_inits;
  ++sha::g_live;
  ctx->md = EVP_MD_CTX_new();
  ctx->index = sha::g_hashes++;
  ctx->updates = 0;
}

void mbedtls_sha256_free(mbedtls_sha256_context* ctx) {
  ++sha::g_frees;
  if (ctx == nullptr || ctx->md == nullptr) return;
  EVP_MD_CTX_free(ctx->md);
  ctx->md = nullptr;
  --sha::g_live;
}

int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int is224) {
  if (is224 != 0) return sha::kFailed;  // the sketch asks for SHA-256 only
  // A failed start still leaves a started state (an error answered after
  // the engine was set up), so the steps after it run.
  const bool fail = failing_here(ctx, sha::Step::Starts);
  const bool ok = EVP_DigestInit_ex(ctx->md, EVP_sha256(), nullptr) == 1;
  return fail || !ok ? sha::kFailed : 0;
}

int mbedtls_sha256_update(mbedtls_sha256_context* ctx, const unsigned char* in, size_t n) {
  const bool fail = failing_here(ctx, sha::Step::Update);
  ctx->updates++;
  if (fail) return sha::kFailed;  // this update's bytes never reach the hash
  return EVP_DigestUpdate(ctx->md, in, n) == 1 ? 0 : sha::kFailed;
}

int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, unsigned char* out) {
  if (failing_here(ctx, sha::Step::Finish)) {
    std::memset(out, 0xA5, 32);
    return sha::kFailed;
  }
  unsigned int olen = 0;
  return EVP_DigestFinal_ex(ctx->md, out, &olen) == 1 && olen == 32 ? 0 : sha::kFailed;
}

// A SHA-256 that goes through none of the above, for expected values.
static std::string openssl_sha(const std::string& domain, const uint8_t* data, size_t n,
                               bool with_domain) {
  EVP_MD_CTX* c = EVP_MD_CTX_new();
  unsigned char out[32];
  unsigned int olen = 0;
  const uint8_t sep = 0;
  EVP_DigestInit_ex(c, EVP_sha256(), nullptr);
  if (with_domain) {
    EVP_DigestUpdate(c, domain.data(), domain.size());
    EVP_DigestUpdate(c, &sep, 1);
  }
  if (n > 0) EVP_DigestUpdate(c, data, n);
  EVP_DigestFinal_ex(c, out, &olen);
  EVP_MD_CTX_free(c);
  return std::string(reinterpret_cast<const char*>(out), 32);
}

// ── what the cut functions call ─────────────────────────────────────────────
#define HEX 16
struct HostSerial {
  std::string all;
  void println(const char* s = "") {
    all += s;
    all += '\n';
  }
  void print(char c) { all += c; }  // hex_print()'s two calls, as Print's
  void print(unsigned char b, int base) {
    char d[4];
    std::snprintf(d, sizeof(d), base == HEX ? "%X" : "%u", (unsigned)b);
    all += d;
  }
  __attribute__((format(printf, 2, 3))) int printf(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    all += line;
    return n;
  }
};
static HostSerial Serial;

struct HostEsp {
  uint32_t getFreeHeap() { return 123456; }
};
static HostEsp ESP;

static uint32_t millis() { return 5000; }

struct LogLine {
  LogLevel level;
  std::string message;
  std::string detail;
};
static std::vector<LogLine> g_log;
static void log_health(LogLevel level, LogCategory, const char* message, const char* detail) {
  g_log.push_back({level, message ? message : "", detail ? detail : ""});
}
static int logged(const std::string& message) {
  int n = 0;
  for (const auto& l : g_log) n += l.message == message;
  return n;
}

enum RecordType : uint8_t {
  RECORD_BOOT_ATTESTATION = 0,
  RECORD_WITNESS_EVENT = 1,
  RECORD_STATE_CHANGE = 2,
};

struct WitnessRecord {
  uint32_t seq;
  uint32_t time_bucket;
  RecordType type;
  uint8_t payload_hash[32];
  uint8_t prev_hash[32];
  uint8_t chain_hash[32];
  uint8_t signature[64];
  size_t payload_len;
  bool verified;
};

enum class KeyLoad : uint8_t { Loaded, Absent, Refused };

// The members of the sketch's DeviceIdentity the cut functions read.
struct DeviceIdentity {
  uint8_t privkey[32];
  uint8_t pubkey[32];
  uint8_t pubkey_fp[8];
  uint8_t chain_head[32];
  uint32_t seq;
  uint32_t seq_persisted;
  chain_persist::Streak chain_persist_streak;
  uint32_t boot_count;
  uint32_t boot_ms;
  uint32_t log_seq;
  uint32_t born_day;
  bool born_exact;
  bool chain_refused;
  bool initialized;
  bool tamper_active;
  char device_id[32];
  char ap_ssid[33];
  char mdns_hostname[40];
  char api_token_str[36];
  char ap_password[16];
  char fingerprint_hex[17];
  bool first_boot;
};
static DeviceIdentity g_device;

struct SystemHealth {
  uint32_t records_created;
  uint32_t records_verified;
  uint32_t verify_failures;
  uint32_t chain_persists;
  uint32_t chain_persist_failures;
  uint32_t sd_writes;
  uint32_t min_heap;
  bool crypto_healthy;
};
static SystemHealth g_health;

// NVS: the entries the cut functions read and write, which outlive a call
// as NVS outlives a reboot. Every write is counted.
namespace nvs {
std::map<std::string, std::vector<uint8_t>> blobs;
std::map<std::string, uint32_t> words;
int blob_writes = 0;
int word_writes = 0;
void clear() {
  blobs.clear();
  words.clear();
  blob_writes = word_writes = 0;
}
}  // namespace nvs

static const char* NVS_KEY_SEQ = "seq";
static const char* NVS_KEY_BOOTS = "boots";
static const char* NVS_KEY_CHAIN = "chain";
static const char* NVS_KEY_CHAINST = "chain_st";
static const char* NVS_KEY_LOGSEQ = "logseq";
static const char* NVS_KEY_BORN = "born_day";
static const char* NVS_KEY_BORN_EX = "born_exact";

static uint32_t nvs_load_u32(const char* key, uint32_t def = 0) {
  auto it = nvs::words.find(key);
  return it == nvs::words.end() ? def : it->second;
}
static bool nvs_store_u32(const char* key, uint32_t val) {
  ++nvs::word_writes;
  nvs::words[key] = val;
  return true;
}
static bool nvs_load_bytes(const char* key, uint8_t* out, size_t len) {
  auto it = nvs::blobs.find(key);
  if (it == nvs::blobs.end() || it->second.size() != len) return false;
  std::memcpy(out, it->second.data(), len);
  return true;
}
static bool nvs_store_bytes(const char* key, const uint8_t* data, size_t len) {
  ++nvs::blob_writes;
  nvs::blobs[key].assign(data, data + len);
  return true;
}

// The signer: a stand-in whose signature is the chain hash and the public
// key, so verify_signature() can recompute it. g_sign_garbles makes the next
// signatures fail to verify (a record made and not verified).
static int g_signs = 0;
static bool g_sign_garbles = false;
static void sign_message(const uint8_t*, const uint8_t pub[32], const uint8_t* msg, size_t len,
                         uint8_t sig[64]) {
  ++g_signs;
  std::memset(sig, 0, 64);
  std::memcpy(sig, msg, len < 32 ? len : 32);
  std::memcpy(sig + 32, pub, 32);
  if (g_sign_garbles) sig[63] ^= 0x01;
}
static bool verify_signature(const uint8_t pub[32], const uint8_t* msg, size_t len,
                             const uint8_t sig[64]) {
  uint8_t want[64];
  std::memset(want, 0, 64);
  std::memcpy(want, msg, len < 32 ? len : 32);
  std::memcpy(want + 32, pub, 32);
  return std::memcmp(want, sig, 64) == 0;
}

static uint32_t g_time_bucket_ms = 60000;
static const uint32_t SD_PERSIST_INTERVAL = 10;
static uint32_t time_bucket() { return 77; }
#define DEBUG_CHAIN 0
#define FEATURE_SD_STORAGE 1

typedef int portMUX_TYPE;
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
static witness_page::Ring g_witness_page_ring;
static portMUX_TYPE g_witness_page_mux = 0;

static std::vector<WitnessRecord> g_card;  // what sd_append_witness_record() was handed
static bool sd_append_witness_record(const WitnessRecord* rec) {
  g_card.push_back(*rec);
  return true;
}
static bool g_witness_tail_torn = false;
static WitnessRecord g_last_record;

// The TLS globals the certificate code reads, as the sketch declares them.
static bool g_tls_enabled = false;
static bool g_tls_cert_persisted = false;
static uint8_t* g_tls_cert_der = nullptr;
static size_t g_tls_cert_der_len = 0;
static uint8_t* g_tls_key_der = nullptr;
static size_t g_tls_key_der_len = 0;
static char g_tls_cert_fp_hex[65] = {0};

namespace setup_wizard {
static bool s_active = false;
static bool is_active() { return s_active; }
}  // namespace setup_wizard

// ── the sketch, verbatim ─────────────────────────────────────────────────────
#include WAP_SHA_INC

#define SECURACV_HAS_HTTPS_SERVER 1
#include WAP_SHA_TLS_AFTER_INC
#undef SECURACV_HAS_HTTPS_SERVER

// The certificate's storage and generation: stand-ins. A loaded or generated
// certificate is these bytes; the key is the bytes after them.
static const std::vector<uint8_t> kCertDer = [] {
  std::vector<uint8_t> v(812);
  for (size_t i = 0; i < v.size(); i++) v[i] = (uint8_t)(i * 7 + 3);
  return v;
}();
static bool g_cert_in_nvs = false;
static int g_cert_stores = 0;
static void tls_hold(std::vector<uint8_t>* keep_key) {
  g_tls_cert_der = static_cast<uint8_t*>(std::malloc(kCertDer.size()));
  std::memcpy(g_tls_cert_der, kCertDer.data(), kCertDer.size());
  g_tls_cert_der_len = kCertDer.size();
  g_tls_key_der = static_cast<uint8_t*>(std::malloc(64));
  std::memset(g_tls_key_der, 0x5c, 64);
  g_tls_key_der_len = 64;
  (void)keep_key;
}
static bool tls_load_from_nvs() {
  if (!g_cert_in_nvs) return false;
  tls_hold(nullptr);
  return true;
}
static bool tls_store_to_nvs() {
  ++g_cert_stores;
  g_cert_in_nvs = true;
  return true;
}
static bool tls_generate_self_signed_cert() {
  tls_hold(nullptr);
  return true;
}

namespace certgen {
#define SECURACV_HAS_TLS_CERTGEN 1
#include WAP_SHA_TLS_INC
#undef SECURACV_HAS_TLS_CERTGEN
}  // namespace certgen

namespace no_certgen {
#define SECURACV_HAS_TLS_CERTGEN 0
#include WAP_SHA_TLS_INC
#undef SECURACV_HAS_TLS_CERTGEN
}  // namespace no_certgen

// What provision_device() calls besides the hashes: stand-ins
// (test_wap_hmac_fail.cpp runs the mints and the token over their own seams,
// test_wap_first_boot_nvs.cpp the key).
static int g_token_derives = 0;
static int g_token_stores = 0;
static int g_ap_derives = 0;
static bool establish_identity_key() {
  for (size_t i = 0; i < 32; i++) g_device.privkey[i] = (uint8_t)(i + 1);
  return true;
}
struct Ed25519 {
  static void derivePublicKey(uint8_t pub[32], const uint8_t priv[32]) {
    for (size_t i = 0; i < 32; i++) pub[i] = (uint8_t)(priv[i] ^ 0x5c);
  }
};
static void generate_device_id(char* out, size_t cap) {
  std::snprintf(out, cap, "wap-%02x%02x", g_device.pubkey_fp[0], g_device.pubkey_fp[1]);
}
static void generate_ap_ssid(char* out, size_t cap) {
  std::snprintf(out, cap, "SecuraCV-wap-%02x%02x", g_device.pubkey_fp[0], g_device.pubkey_fp[1]);
}
static void generate_mdns_hostname(char* out, size_t cap) {
  std::snprintf(out, cap, "canary-%02x%02x", g_device.pubkey_fp[0], g_device.pubkey_fp[1]);
}
static void config_load_runtime() {}
static KeyLoad nvs_load_token(char* token_str, size_t max_len) {
  std::memset(token_str, 0, max_len);
  return KeyLoad::Absent;
}
static bool derive_api_token(const uint8_t*, char* token_str, size_t len) {
  ++g_token_derives;
  std::snprintf(token_str, len, "cv_%032d", 7);
  return true;
}
static bool nvs_store_token(const char*) {
  ++g_token_stores;
  return true;
}
static bool derive_ap_password(const uint8_t*, char* out, size_t len) {
  ++g_ap_derives;
  std::snprintf(out, len, "cv-abcdefghjkmn");
  return true;
}

#include WAP_SHA_PROV_INC

// ── fixtures ────────────────────────────────────────────────────────────────
// Every way a hash of `updates` updates can fail, in the hash numbered
// `hash` since the arm.
struct Failure {
  sha::Step step;
  int update;
};
static std::vector<Failure> failures_of(int updates) {
  std::vector<Failure> out = {{sha::Step::Starts, 0}};
  for (int u = 0; u < updates; u++) out.push_back({sha::Step::Update, u});
  out.push_back({sha::Step::Finish, 0});
  return out;
}

static void key_pub(uint8_t (&pub)[32]) {
  for (size_t i = 0; i < 32; i++) pub[i] = (uint8_t)((i + 1) ^ 0x5c);
}

// ── sha256_raw() and sha256_domain() ────────────────────────────────────────
static void helpers_known_answers() {
  sha::arm(sha::Step::None);
  uint8_t out[32];
  std::memset(out, 0xEE, sizeof(out));
  CHECK_MSG(sha256_raw(reinterpret_cast<const uint8_t*>("abc"), 3, out), "sha256_raw: false");
  CHECK_MSG(hex(out, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "sha256_raw(\"abc\"): FIPS 180-2's answer, not " + hex(out, 32));
  const uint8_t data[4] = {1, 2, 3, 4};
  CHECK_MSG(sha256_domain("securacv:test", data, sizeof(data), out), "sha256_domain: false");
  CHECK_MSG(std::string(reinterpret_cast<const char*>(out), 32) ==
                openssl_sha("securacv:test", data, sizeof(data), true),
            "sha256_domain: not domain || 0x00 || data");
  CHECK_MSG(sha256_domain("securacv:test", nullptr, 0, out) &&
                std::string(reinterpret_cast<const char*>(out), 32) ==
                    openssl_sha("securacv:test", nullptr, 0, true),
            "sha256_domain with no data: not domain || 0x00");
  CHECK_MSG(sha::g_inits == 3 && sha::g_frees == 3 && sha::g_live == 0, "contexts not freed");
}

static void helpers_refuse_each_step() {
  const uint8_t data[4] = {1, 2, 3, 4};
  for (const Failure& f : failures_of(1)) {
    sha::arm(f.step, 0, f.update);
    uint8_t out[32];
    std::memset(out, 0xEE, sizeof(out));  // what the caller's stack held
    const bool ran = sha256_raw(data, sizeof(data), out);
    const std::string what = "sha256_raw, " + sha::name_of(f.step, f.update);
    CHECK_MSG(sha::g_fired, what + ": the failure never fired");
    CHECK_MSG(!ran, what + ": answered true");
    CHECK_MSG(all_zero(out, 32), what + ": output not zeroed: " + hex(out, 32));
    CHECK_MSG(sha::g_inits == sha::g_frees && sha::g_live == 0, what + ": a context left allocated");
  }
  for (const Failure& f : failures_of(3)) {
    sha::arm(f.step, 0, f.update);
    uint8_t out[32];
    std::memset(out, 0xEE, sizeof(out));
    const bool ran = sha256_domain("securacv:test", data, sizeof(data), out);
    const std::string what = "sha256_domain, " + sha::name_of(f.step, f.update);
    CHECK_MSG(sha::g_fired, what + ": the failure never fired");
    CHECK_MSG(!ran, what + ": answered true");
    CHECK_MSG(all_zero(out, 32), what + ": output not zeroed: " + hex(out, 32));
    CHECK_MSG(sha::g_inits == sha::g_frees && sha::g_live == 0, what + ": a context left allocated");
  }
}

// ── provision_device(): the fingerprint and the genesis head ────────────────
static const uint32_t kStoredSeq = 1234;
static const uint32_t kStoredBoots = 41;

static void stored_unit(bool with_chain, uint8_t (&head)[32]) {
  nvs::clear();
  for (size_t i = 0; i < 32; i++) head[i] = (uint8_t)(0x31 + i);
  if (with_chain) {
    std::vector<uint8_t> blob(chain_state::BLOB_LEN);
    chain_state::encode(kStoredSeq, head, blob.data());
    nvs::blobs[NVS_KEY_CHAINST] = blob;
  }
  nvs::words[NVS_KEY_BOOTS] = kStoredBoots;
  nvs::blob_writes = nvs::word_writes = 0;
}

static bool provision() {
  std::memset(&g_device, 0, sizeof(g_device));
  std::memset(&g_health, 0, sizeof(g_health));
  Serial.all.clear();
  g_log.clear();
  g_token_derives = g_token_stores = g_ap_derives = 0;
  return provision_device();
}

static void provision_completes() {
  for (int genesis = 0; genesis < 2; genesis++) {
    const std::string what = genesis ? "a working unit at genesis" : "a working unit";
    uint8_t head[32];
    stored_unit(!genesis, head);
    sha::arm(sha::Step::None);
    CHECK_MSG(provision(), what + ": provisioning answered false");
    CHECK_MSG(g_device.initialized && !g_device.chain_refused, what + ": not ready");
    uint8_t pub[32];
    key_pub(pub);
    const std::string fp = openssl_sha("securacv:pubkey:fingerprint", pub, 32, true);
    CHECK_MSG(std::memcmp(g_device.pubkey_fp, fp.data(), 8) == 0, what + ": not the fingerprint");
    char id[32];
    std::snprintf(id, sizeof(id), "wap-%02x%02x", (uint8_t)fp[0], (uint8_t)fp[1]);
    CHECK_MSG(std::string(g_device.device_id) == id, what + ": device id " + g_device.device_id);
    if (genesis) {
      const std::string gh = openssl_sha("securacv:genesis:v1",
                                         reinterpret_cast<const uint8_t*>(id), std::strlen(id), true);
      CHECK_MSG(std::memcmp(g_device.chain_head, gh.data(), 32) == 0, what + ": not the genesis head");
      CHECK_MSG(nvs::blob_writes == 1, what + ": the genesis head not persisted");
    } else {
      CHECK_MSG(g_device.seq == kStoredSeq && std::memcmp(g_device.chain_head, head, 32) == 0 &&
                    nvs::blob_writes == 0,
                what + ": not the stored chain");
    }
    CHECK_MSG(g_token_derives == 1 && g_ap_derives == 1, what + ": the mints did not run");
  }
}

// What a refused identity leaves: nothing named or handed out, nothing in
// NVS but this boot's count, the stored chain (when there is one) as NVS
// holds it, and the chain refused for every record and persist after it.
static void refused_identity_holds(const std::string& what, bool genesis, bool named,
                                   const std::vector<uint8_t>& blob_before, const uint8_t head[32]) {
  CHECK_MSG(!g_device.initialized, what + ": the identity marked ready");
  CHECK_MSG(g_device.chain_refused, what + ": the chain not refused");
  CHECK_MSG(nvs::blob_writes == 0 && nvs::word_writes == 1 &&
                nvs::words[NVS_KEY_BOOTS] == kStoredBoots + 1,
            what + ": " + std::to_string(nvs::blob_writes) + " blob write(s), " +
                std::to_string(nvs::word_writes) + " word write(s)");
  if (genesis) {
    CHECK_MSG(nvs::blobs.count(NVS_KEY_CHAINST) == 0, what + ": a genesis head persisted");
    CHECK_MSG(g_device.seq == 0, what + ": seq " + std::to_string(g_device.seq));
  } else {
    CHECK_MSG(nvs::blobs[NVS_KEY_CHAINST] == blob_before, what + ": the stored chain state written");
    CHECK_MSG(g_device.seq == kStoredSeq && g_device.seq_persisted == kStoredSeq &&
                  std::memcmp(g_device.chain_head, head, 32) == 0,
              what + ": the stored chain not loaded");
  }
  CHECK_MSG(g_token_derives == 0 && g_token_stores == 0 && g_ap_derives == 0 &&
                g_device.api_token_str[0] == '\0' && g_device.ap_password[0] == '\0',
            what + ": a token or a SoftAP key handed out");
  if (!named) {
    CHECK_MSG(all_zero(g_device.pubkey_fp, 8) && g_device.fingerprint_hex[0] == '\0' &&
                  g_device.device_id[0] == '\0' && g_device.ap_ssid[0] == '\0' &&
                  g_device.mdns_hostname[0] == '\0',
              what + ": the device named \"" + std::string(g_device.device_id) + "\"");
    CHECK_MSG(Serial.all.find("[PROV] Public key fingerprint:") == std::string::npos,
              what + ": a fingerprint printed");
  }
  CHECK_MSG(sha::g_live == 0, what + ": a SHA context left allocated");
}

static void a_fingerprint_not_hashed_names_nothing() {
  for (const Failure& f : failures_of(3)) {
    for (int genesis = 0; genesis < 2; genesis++) {
      const std::string what = "the fingerprint, " + sha::name_of(f.step, f.update) +
                               (genesis ? ", a chain at genesis" : ", a stored chain");
      uint8_t head[32];
      stored_unit(!genesis, head);
      const std::vector<uint8_t> blob_before =
          genesis ? std::vector<uint8_t>() : nvs::blobs[NVS_KEY_CHAINST];
      sha::arm(f.step, /*hash=*/0, f.update);
      CHECK_MSG(!provision(), what + ": provisioning answered true");
      CHECK_MSG(sha::g_fired, what + ": the failure never fired");
      refused_identity_holds(what, genesis, /*named=*/false, blob_before, head);
      CHECK_MSG(sha::g_inits == 1, what + ": " + std::to_string(sha::g_inits) +
                                       " hash(es): a genesis head hashed from no name");
      CHECK_MSG(Serial.all.find("[!!] Public key fingerprint not computed (SHA-256 failed): the "
                                "device is not named this boot\n") != std::string::npos,
                what + ": no line says why");
    }
  }
}

static void a_genesis_head_not_hashed_is_not_persisted() {
  for (const Failure& f : failures_of(3)) {
    const std::string what = "the genesis head, " + sha::name_of(f.step, f.update);
    uint8_t head[32];
    stored_unit(/*with_chain=*/false, head);
    nvs::words[NVS_KEY_SEQ] = 0;
    sha::arm(f.step, /*hash=*/1, f.update);  // the fingerprint runs; the genesis head fails
    CHECK_MSG(!provision(), what + ": provisioning answered true");
    CHECK_MSG(sha::g_fired, what + ": the failure never fired");
    refused_identity_holds(what, /*genesis=*/true, /*named=*/true, {}, head);
    CHECK_MSG(all_zero(g_device.chain_head, 32), what + ": a head " + hex(g_device.chain_head, 32));
    CHECK_MSG(Serial.all.find("[!!] Genesis chain head not computed (SHA-256 failed): no record "
                              "this boot\n") != std::string::npos,
              what + ": no line says why");
  }
}

// After either refusal setup() goes on (sweep F336): the boot attestation
// record, loop()'s records and the reboot, safe-mode and deep-sleep persist
// all run. None of them signs, links or writes anything.
static void a_refused_chain_makes_no_record() {
  for (int genesis = 0; genesis < 2; genesis++) {
    const std::string what = genesis ? "a refused genesis head" : "a refused fingerprint";
    uint8_t head[32];
    stored_unit(/*with_chain=*/!genesis, head);
    sha::arm(sha::Step::Starts, genesis ? 1 : 0);
    CHECK_MSG(!provision() && g_device.chain_refused, what + ": provisioning not refused");
    const uint32_t seq = g_device.seq;
    uint8_t chain_head[32];
    std::memcpy(chain_head, g_device.chain_head, 32);
    std::map<std::string, std::vector<uint8_t>> blobs = nvs::blobs;
    g_card.clear();
    g_signs = 0;
    std::memset(&g_witness_page_ring, 0, sizeof(g_witness_page_ring));
    sha::arm(sha::Step::None);
    WitnessRecord rec;
    std::memset(&rec, 0x5a, sizeof(rec));
    const WitnessRecord before = rec;
    const uint8_t payload[] = "boot attestation";
    for (int i = 0; i < 25; i++) {
      CHECK_MSG(!create_witness_record(payload, sizeof(payload), RECORD_BOOT_ATTESTATION, &rec),
                what + ": a record made");
    }
    persist_chain_state();  // the reboot path's
    CHECK_MSG(g_device.seq == seq && std::memcmp(g_device.chain_head, chain_head, 32) == 0,
              what + ": the chain moved");
    CHECK_MSG(std::memcmp(&rec, &before, sizeof(rec)) == 0, what + ": *out written");
    CHECK_MSG(g_card.empty() && g_signs == 0 && g_witness_page_ring.count == 0 &&
                  g_health.records_created == 0,
              what + ": a record signed, appended or listed");
    CHECK_MSG(nvs::blobs == blobs && nvs::blob_writes == 0, what + ": NVS written");
    CHECK_MSG(sha::g_inits == 0, what + ": a record hashed");
    std::memset(&g_last_record, 0, sizeof(g_last_record));
    CHECK_MSG(!chain_tail_intact(), what + ": the chain reported intact");
  }
}

// What setup(), loop() and BLE discovery do on that boot (the F364 review):
// the boot attestation's line names no record, loop()'s records are refused
// and logged once, its self-verify says nothing and leaves signing marked
// unhealthy, and BLE discovery's gate keeps the zeroed fingerprint and head
// off the air (Opera's SCV-0000 name and device-info id, every chirp's id
// and head).
static void a_refused_chain_is_shown_and_broadcast_nowhere() {
  for (int genesis = 0; genesis < 2; genesis++) {
    for (const Failure& f : failures_of(3)) {
      const std::string what = std::string(genesis ? "a refused genesis head, " : "a refused fingerprint, ") +
                               sha::name_of(f.step, f.update);
      uint8_t head[32];
      stored_unit(/*with_chain=*/!genesis, head);
      if (genesis) nvs::words[NVS_KEY_SEQ] = 0;
      sha::arm(f.step, genesis ? 1 : 0, f.update);
      CHECK_MSG(!provision() && g_device.chain_refused, what + ": provisioning not refused");
      CHECK_MSG(!ble_discovery_identity_ready(),
                what + ": BLE discovery would broadcast the id " + hex(g_device.pubkey_fp, 8) +
                    " and the head " + hex(g_device.chain_head, 32));
      std::memset(&g_last_record, 0, sizeof(g_last_record));  // as setup() leaves it
      g_log.clear();
      Serial.all.clear();
      g_witness_hash_warned = false;
      sha::arm(sha::Step::None);
      const uint8_t payload[] = "boot attestation";
      record_boot_attestation(payload, sizeof(payload));
      CHECK_MSG(Serial.all.find("[OK] Boot attestation") == std::string::npos,
                what + ": the boot attestation reported made: " + Serial.all);
      CHECK_MSG(Serial.all.find("[!!] Boot attestation not recorded: no record was made, or its "
                                "signature did not verify\n") != std::string::npos,
                what + ": no line says the boot attestation was not recorded");
      for (int i = 0; i < 25; i++) {
        CHECK_MSG(witness_record_last(payload, sizeof(payload), RECORD_WITNESS_EVENT) ==
                      RecordMade::Refused,
                  what + ": loop()'s record not Refused (its row would print)");
      }
      CHECK_MSG(all_zero(&g_last_record, sizeof(g_last_record)), what + ": g_last_record written");
      CHECK_MSG(g_log.size() == 1 && g_log[0].message == "Witness record not made: SHA-256 failed" &&
                    g_log[0].detail == "identity not provisioned" && g_log[0].level == SCV_LOG_ERROR,
                what + ": not one log line for 26 refused records (" + std::to_string(g_log.size()) + ")");
      self_verify_last_record();
      CHECK_MSG(g_log.size() == 1 && logged("Self-verification FAILED") == 0,
                what + ": self-verify logged a signature failure for a record never made");
      CHECK_MSG(!g_health.crypto_healthy, what + ": signing marked healthy");
    }
  }
  // The gate holds to the fingerprint too, whatever the flag says.
  std::memset(&g_device, 0, sizeof(g_device));
  CHECK_MSG(!ble_discovery_identity_ready(), "BLE discovery: a zero fingerprint passes the gate");
}

// A working unit: the gate opens, the boot attestation's line names the
// record made, and self-verify checks it (and catches a bad signature).
static void a_working_unit_shows_its_records() {
  for (int genesis = 0; genesis < 2; genesis++) {
    const std::string what = genesis ? "a working unit at genesis" : "a working unit";
    uint8_t head[32];
    stored_unit(!genesis, head);
    sha::arm(sha::Step::None);
    CHECK_MSG(provision(), what + ": provisioning answered false");
    CHECK_MSG(ble_discovery_identity_ready(), what + ": BLE discovery kept off");
    const uint32_t seq = g_device.seq;
    std::memset(&g_last_record, 0, sizeof(g_last_record));
    g_log.clear();
    Serial.all.clear();
    const uint8_t payload[] = "boot attestation";
    record_boot_attestation(payload, sizeof(payload));
    char line[96];
    std::snprintf(line, sizeof(line), "[OK] Boot attestation: seq=%u chain=", (unsigned)(seq + 1));
    std::string chain = hex(g_last_record.chain_hash, 8);
    for (char& ch : chain) ch = (char)std::toupper((unsigned char)ch);
    CHECK_MSG(g_last_record.seq == seq + 1 &&
                  Serial.all.find(std::string(line) + chain + "...\n") != std::string::npos,
              what + ": the boot attestation's line: " + Serial.all);
    self_verify_last_record();
    CHECK_MSG(g_health.crypto_healthy && g_log.empty(), what + ": a made record failed self-verify");
    g_last_record.signature[0] ^= 0x01;
    self_verify_last_record();
    CHECK_MSG(!g_health.crypto_healthy && logged("Self-verification FAILED") == 1,
              what + ": a bad signature passed self-verify");
  }
}

// ── create_witness_record(): the payload hash and the chain link ────────────
static void ready_chain(uint8_t (&head)[32]) {
  std::memset(&g_device, 0, sizeof(g_device));
  std::memset(&g_health, 0, sizeof(g_health));
  std::memset(&g_witness_page_ring, 0, sizeof(g_witness_page_ring));
  key_pub(g_device.pubkey);
  for (size_t i = 0; i < 32; i++) head[i] = (uint8_t)(0x90 + i);
  std::memcpy(g_device.chain_head, head, 32);
  g_device.seq = 500;
  g_device.seq_persisted = 500;
  g_device.initialized = true;
  nvs::clear();
  g_card.clear();
  g_log.clear();
  g_signs = 0;
  g_witness_hash_warned = false;
}

// The link create_witness_record() should make from `head` at `seq`.
static std::string expected_link(const uint8_t head[32], const uint8_t* payload, size_t len,
                                 uint32_t seq) {
  const std::string ph = openssl_sha("securacv:fw:payload:v1", payload, len, true);
  uint8_t buf[72];
  std::memcpy(buf, head, 32);
  std::memcpy(buf + 32, ph.data(), 32);
  const uint32_t tb = 77;
  for (int i = 0; i < 4; i++) buf[64 + i] = (uint8_t)(seq >> (24 - 8 * i));
  for (int i = 0; i < 4; i++) buf[68 + i] = (uint8_t)(tb >> (24 - 8 * i));
  return openssl_sha("securacv:fw:chain:v1", buf, sizeof(buf), true);
}

static void a_record_whose_hash_fails_is_not_made() {
  const uint8_t payload[] = "csi motion 1 2 3";
  struct Caller {
    const char* what;
    int hash;  // 0 the payload hash, 1 the chain link
    const char* detail;
  };
  for (const Caller& c : {Caller{"the record payload hash", 0, "payload hash"},
                          Caller{"the chain link", 1, "chain link"}}) {
    for (const Failure& f : failures_of(3)) {
      const std::string what = std::string(c.what) + ", " + sha::name_of(f.step, f.update);
      uint8_t head[32];
      ready_chain(head);
      WitnessRecord rec;
      std::memset(&rec, 0x5a, sizeof(rec));
      const WitnessRecord before = rec;
      sha::arm(f.step, c.hash, f.update);
      CHECK_MSG(!create_witness_record(payload, sizeof(payload), RECORD_WITNESS_EVENT, &rec),
                what + ": a record made");
      CHECK_MSG(sha::g_fired, what + ": the failure never fired");
      CHECK_MSG(g_device.seq == 500 && std::memcmp(g_device.chain_head, head, 32) == 0,
                what + ": the chain moved to seq " + std::to_string(g_device.seq) + ", head " +
                    hex(g_device.chain_head, 32));
      CHECK_MSG(std::memcmp(&rec, &before, sizeof(rec)) == 0, what + ": *out written");
      CHECK_MSG(g_signs == 0 && g_card.empty() && g_witness_page_ring.count == 0 &&
                    g_health.records_created == 0 && nvs::blob_writes == 0,
                what + ": signed, appended, listed or persisted");
      CHECK_MSG(sha::g_live == 0, what + ": a SHA context left allocated");
      CHECK_MSG(g_log.size() == 1 && g_log[0].message == "Witness record not made: SHA-256 failed" &&
                    g_log[0].detail == c.detail && g_log[0].level == SCV_LOG_ERROR,
                what + ": not one log line naming the " + c.detail);

      // A second failure says nothing more; the next good record extends
      // the head as if the failed one had not been tried, and re-arms the
      // line.
      sha::arm(f.step, c.hash, f.update);
      CHECK_MSG(!create_witness_record(payload, sizeof(payload), RECORD_WITNESS_EVENT, &rec) &&
                    g_log.size() == 1,
                what + ": a second failure logged again");
      sha::arm(sha::Step::None);
      CHECK_MSG(create_witness_record(payload, sizeof(payload), RECORD_WITNESS_EVENT, &rec),
                what + ": the next record refused");
      const std::string link = expected_link(head, payload, sizeof(payload), 501);
      CHECK_MSG(rec.seq == 501 && std::memcmp(rec.prev_hash, head, 32) == 0 &&
                    std::memcmp(rec.chain_hash, link.data(), 32) == 0 &&
                    std::memcmp(g_device.chain_head, link.data(), 32) == 0 && g_card.size() == 1 &&
                    rec.verified,
                what + ": the next record is not seq 501 on the stored head");
      sha::arm(f.step, c.hash, f.update);
      CHECK_MSG(!create_witness_record(payload, sizeof(payload), RECORD_WITNESS_EVENT, &rec) &&
                    g_log.size() == 2,
                what + ": a failure after a good record not logged");
    }
  }
}

// loop()'s and setup()'s record into g_last_record (the F364 review): a
// record never made is Refused and leaves g_last_record as it was, so no
// row repeats the last record as new and no "[OK]" names it; a record made
// and not verified is Unverified, logged as before, and shown.
static void a_record_not_made_is_not_shown() {
  const uint8_t payload[] = "csi motion 4 5 6";
  for (int hash = 0; hash < 2; hash++) {
    for (const Failure& f : failures_of(3)) {
      const std::string what = std::string(hash ? "the chain link, " : "the record payload hash, ") +
                               sha::name_of(f.step, f.update);
      uint8_t head[32];
      ready_chain(head);
      sha::arm(sha::Step::None);
      CHECK_MSG(witness_record_last(payload, sizeof(payload), RECORD_WITNESS_EVENT) == RecordMade::Made,
                what + ": the first record not made");
      const WitnessRecord before = g_last_record;
      g_log.clear();
      Serial.all.clear();
      sha::arm(f.step, hash, f.update);
      CHECK_MSG(witness_record_last(payload, sizeof(payload), RECORD_WITNESS_EVENT) ==
                    RecordMade::Refused,
                what + ": not Refused");
      CHECK_MSG(std::memcmp(&g_last_record, &before, sizeof(before)) == 0,
                what + ": g_last_record written");
      CHECK_MSG(logged("Record verification failed") == 0, what + ": logged as a verification failure");
      sha::arm(f.step, hash, f.update);
      record_boot_attestation(payload, sizeof(payload));
      CHECK_MSG(Serial.all.find("[OK] Boot attestation") == std::string::npos &&
                    Serial.all.find("[!!] Boot attestation not recorded") != std::string::npos,
                what + ": the boot attestation's line: " + Serial.all);
    }
  }
  // A record made whose signature does not verify.
  uint8_t head[32];
  ready_chain(head);
  sha::arm(sha::Step::None);
  g_sign_garbles = true;
  Serial.all.clear();
  CHECK_MSG(witness_record_last(payload, sizeof(payload), RECORD_WITNESS_EVENT) == RecordMade::Unverified &&
                g_last_record.seq == 501 && !g_last_record.verified,
            "a record not verified: not Unverified at seq 501");
  CHECK_MSG(logged("Record verification failed") == 1, "a record not verified: not logged");
  record_boot_attestation(payload, sizeof(payload));
  CHECK_MSG(Serial.all.find("[OK] Boot attestation") == std::string::npos,
            "a boot attestation not verified: reported made");
  g_sign_garbles = false;
  // Self-verify before any record this boot: nothing to verify, nothing said.
  ready_chain(head);
  std::memset(&g_last_record, 0, sizeof(g_last_record));
  g_health.crypto_healthy = true;
  self_verify_last_record();
  CHECK_MSG(g_health.crypto_healthy && g_log.empty(),
            "self-verify with no record yet: verified the zeroed record");
}

// ── the SD tail and chain_tail_intact() ─────────────────────────────────────
static void card_with_tail(const uint8_t head[32], uint32_t seq, WitnessRecord* tail) {
  const uint8_t payload[] = "the card's last record";
  const std::string ph = openssl_sha("securacv:fw:payload:v1", payload, sizeof(payload), true);
  const std::string link = expected_link(head, payload, sizeof(payload), seq);
  std::memset(tail, 0, sizeof(*tail));
  tail->seq = seq;
  tail->time_bucket = 77;
  std::memcpy(tail->payload_hash, ph.data(), 32);
  std::memcpy(tail->prev_hash, head, 32);
  std::memcpy(tail->chain_hash, link.data(), 32);
  sign_message(nullptr, g_device.pubkey, tail->chain_hash, 32, tail->signature);
  char line[witness_store::RECORD_LINE_MAX + 1];
  const size_t n = witness_store::line_build(line, sizeof(line), tail->seq, tail->time_bucket, 1,
                                             tail->payload_hash, tail->prev_hash,
                                             tail->chain_hash, tail->signature);
  SD = FakeSD();
  SD.present = true;
  SD.files["/WITNESS/records.jsonl"] = std::string(line, n);
}

static void the_tail_is_not_adopted_on_a_failed_hash() {
  for (const Failure& f : failures_of(3)) {
    const std::string what = "the SD tail's link, " + sha::name_of(f.step, f.update);
    uint8_t head[32];
    ready_chain(head);
    WitnessRecord tail;
    card_with_tail(head, 520, &tail);
    sha::arm(f.step, 0, f.update);
    witness_recover_from_sd();
    CHECK_MSG(sha::g_fired, what + ": the failure never fired");
    CHECK_MSG(g_device.seq == 500 && std::memcmp(g_device.chain_head, head, 32) == 0 &&
                  nvs::blob_writes == 0,
              what + ": the tail adopted");
    CHECK_MSG(logged("Witness SD tail ignored: chain hash not computed (SHA-256 failed)") == 1,
              what + ": no line says why");
  }
  // The same tail with the hash running is adopted.
  uint8_t head[32];
  ready_chain(head);
  WitnessRecord tail;
  card_with_tail(head, 520, &tail);
  sha::arm(sha::Step::None);
  witness_recover_from_sd();
  CHECK_MSG(g_device.seq == 520 && std::memcmp(g_device.chain_head, tail.chain_hash, 32) == 0 &&
                nvs::blob_writes == 1,
            "the SD tail with a working hash: not adopted");
}

static void the_tail_is_not_vouched_for_on_a_failed_hash() {
  uint8_t head[32];
  ready_chain(head);
  const uint8_t payload[] = "csi motion";
  sha::arm(sha::Step::None);
  CHECK_MSG(create_witness_record(payload, sizeof(payload), RECORD_WITNESS_EVENT, &g_last_record),
            "chain_tail_intact: the record not made");
  sha::arm(sha::Step::None);
  CHECK_MSG(chain_tail_intact(), "chain_tail_intact: a working hash does not vouch for the tail");
  for (const Failure& f : failures_of(3)) {
    sha::arm(f.step, 0, f.update);
    CHECK_MSG(!chain_tail_intact(), "chain_tail_intact, " + sha::name_of(f.step, f.update) +
                                        ": vouched for a link it could not recompute");
    CHECK_MSG(sha::g_fired && sha::g_live == 0, "chain_tail_intact: the failure never fired");
  }
}

// ── the TLS certificate's fingerprint ───────────────────────────────────────
static void tls_reset() {
  std::free(g_tls_cert_der);
  std::free(g_tls_key_der);
  g_tls_cert_der = g_tls_key_der = nullptr;
  g_tls_cert_der_len = g_tls_key_der_len = 0;
  g_tls_cert_fp_hex[0] = '\0';
  g_tls_cert_persisted = false;
  g_tls_enabled = false;
  g_cert_stores = 0;
  Serial.all.clear();
}

static void a_certificate_not_hashed_is_not_used() {
  const std::string fp = openssl_sha("", kCertDer.data(), kCertDer.size(), false);
  struct Build {
    const char* what;
    bool (*init)();
    bool stored;  // the certificate is in NVS before this boot
  };
  for (const Build& b : {Build{"a stored certificate", certgen::init_tls_cert, true},
                         Build{"a stored certificate, no generator", no_certgen::init_tls_cert, true},
                         Build{"a certificate generated this boot", certgen::init_tls_cert, false}}) {
    // Working: the pin is the certificate's SHA-256, and it is handed out.
    tls_reset();
    g_cert_in_nvs = b.stored;
    sha::arm(sha::Step::None);
    CHECK_MSG(b.init(), std::string(b.what) + ": a working hash answered false");
    CHECK_MSG(std::string(g_tls_cert_fp_hex) == [&] {
      std::string up = hex(reinterpret_cast<const uint8_t*>(fp.data()), 32);
      for (char& ch : up) ch = (char)std::toupper((unsigned char)ch);
      return up;
    }(), std::string(b.what) + ": the pin " + g_tls_cert_fp_hex);
    setup_wizard::s_active = true;
    CHECK_MSG(tls_after_setup(), std::string(b.what) + ": a working pin not handed out");
    setup_wizard::s_active = false;

    for (const Failure& f : failures_of(1)) {
      const std::string what = std::string(b.what) + ", " + sha::name_of(f.step, f.update);
      tls_reset();
      g_cert_in_nvs = b.stored;
      sha::arm(f.step, 0, f.update);
      CHECK_MSG(!b.init(), what + ": init_tls_cert() answered true");
      CHECK_MSG(sha::g_fired && sha::g_live == 0, what + ": the failure never fired");
      CHECK_MSG(g_tls_cert_fp_hex[0] == '\0', what + std::string(": a pin ") + g_tls_cert_fp_hex);
      CHECK_MSG(!g_tls_cert_persisted, what + ": the certificate marked persisted");
      CHECK_MSG(g_cert_stores == 0 && g_cert_in_nvs == b.stored,
                what + ": a certificate whose pin did not compute stored in NVS");
      CHECK_MSG(g_tls_cert_der == nullptr && g_tls_key_der == nullptr && g_tls_cert_der_len == 0 &&
                    g_tls_key_der_len == 0,
                what + ": a certificate left for the HTTPS listener");
      for (int wizard = 0; wizard < 2; wizard++) {
        for (int enabled = 0; enabled < 2; enabled++) {
          setup_wizard::s_active = wizard;
          g_tls_enabled = enabled;
          CHECK_MSG(!tls_after_setup(), what + ": a pin handed out");
        }
      }
      setup_wizard::s_active = false;
      g_tls_enabled = false;
      CHECK_MSG(Serial.all.find("Cert fingerprint:") == std::string::npos,
                what + ": a fingerprint printed: " + Serial.all);
      CHECK_MSG(Serial.all.find("[TLS] Cert fingerprint not computed (SHA-256 failed) - HTTPS off "
                                "this boot, no pin handed out\n") != std::string::npos,
                what + ": no line says why");
    }
  }
  tls_reset();
}

// ── the sketch's text ───────────────────────────────────────────────────────
static std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static bool is_ident(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Comments become spaces (newlines kept); literals are kept as they are.
static std::string strip_comments(const std::string& s) {
  std::string out = s;
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (c == 'R' && i + 1 < s.size() && s[i + 1] == '"' && (i == 0 || !is_ident(s[i - 1]))) {
      const size_t open = s.find('(', i + 2);
      if (open == std::string::npos) break;
      const std::string close = ")" + s.substr(i + 2, open - i - 2) + "\"";
      const size_t end = s.find(close, open + 1);
      i = (end == std::string::npos) ? s.size() : end + close.size();
    } else if (c == '"' || c == '\'') {
      size_t j = i + 1;
      while (j < s.size() && s[j] != c && s[j] != '\n') j += (s[j] == '\\') ? 2 : 1;
      i = j + 1;
    } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      while (i < s.size() && s[i] != '\n') out[i++] = ' ';
    } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      size_t j = s.find("*/", i + 2);
      j = (j == std::string::npos) ? s.size() : j + 2;
      for (; i < j; i++) {
        if (out[i] != '\n') out[i] = ' ';
      }
    } else {
      i++;
    }
  }
  return out;
}

static size_t line_of(const std::string& s, size_t pos) {
  size_t n = 1;
  for (size_t i = 0; i < pos && i < s.size(); i++) n += s[i] == '\n';
  return n;
}

// Every call of `name(` in `text` takes its result: the token before it is
// `!`, `=`, `&&`, `||`, `(`, `,` or `return`. A declaration or definition
// (the token before is `bool`) is skipped. Returns the calls seen.
static int calls_use_their_result(const std::string& file, const std::string& text,
                                  const std::string& name) {
  int calls = 0;
  for (size_t pos = text.find(name); pos != std::string::npos; pos = text.find(name, pos + 1)) {
    if (pos > 0 && (is_ident(text[pos - 1]) || text[pos - 1] == ':' || text[pos - 1] == '.')) continue;
    size_t after = pos + name.size();
    while (after < text.size() && (text[after] == ' ' || text[after] == '\t')) after++;
    if (after >= text.size() || text[after] != '(') continue;
    size_t b = pos;
    while (b > 0 && std::isspace(static_cast<unsigned char>(text[b - 1]))) b--;
    size_t w = b;
    while (w > 0 && is_ident(text[w - 1])) w--;
    const std::string word = text.substr(w, b - w);
    if (word == "bool" || word == "void") continue;  // its declaration or definition
    ++calls;
    const char prev = b > 0 ? text[b - 1] : '\0';
    const bool used = word == "return" || prev == '!' || prev == '=' || prev == '(' ||
                      prev == ',' || (prev == '&' && b > 1 && text[b - 2] == '&') ||
                      (prev == '|' && b > 1 && text[b - 2] == '|');
    CHECK_MSG(used, file + ":" + std::to_string(line_of(text, pos)) + ": " + name +
                        "() is called and its result dropped");
  }
  return calls;
}

static void sketch_text() {
  const std::string dir = WAP_SKETCH_DIR;
  std::vector<std::string> names;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      const std::string n = e->d_name;
      const size_t dot = n.rfind('.');
      if (dot == std::string::npos) continue;
      const std::string ext = n.substr(dot);
      if (ext == ".ino" || ext == ".h" || ext == ".cpp" || ext == ".c") names.push_back(n);
    }
    closedir(d);
  }
  CHECK_MSG(names.size() > 100, "read " + std::to_string(names.size()) + " sketch files from " + dir);

  // The six are the .ino's own statics, so only the .ino calls them (the
  // sketch's other SHA-256 users are namespaced, F364's next slice).
  struct Hash {
    const char* name;
    int at_least;  // the calls this suite drives
  };
  std::string ino;
  for (const std::string& n : names) {
    if (n == "canary_wap.ino") ino = strip_comments(read_file(dir + "/" + n));
  }
  CHECK_MSG(!ino.empty(), "canary_wap.ino not read");
  for (const Hash& h : {Hash{"sha256_raw", 1}, Hash{"sha256_domain", 4},
                        Hash{"compute_fingerprint", 1}, Hash{"tls_compute_cert_fingerprint", 2},
                        Hash{"compute_chain_hash", 3}, Hash{"update_chain", 1}}) {
    const int calls = calls_use_their_result("canary_wap.ino", ino, h.name);
    CHECK_MSG(calls >= h.at_least, std::string(h.name) + "() called " + std::to_string(calls) +
                                       " time(s), expected at least " + std::to_string(h.at_least));
    CHECK_MSG(ino.find(std::string("\n[[nodiscard]] static bool ") + h.name + "(") != std::string::npos,
              std::string(h.name) + "() is not declared [[nodiscard]] static bool");
  }
  // HTTPS comes on only behind a certificate whose pin is known, and the
  // listener needs both buffers tls_refuse_unpinned_cert() frees.
  const size_t known = ino.find("const bool cert_known = init_tls_cert();");
  const size_t on = ino.find("g_tls_enabled = true;");
  CHECK_MSG(known != std::string::npos && on != std::string::npos &&
                ino.find("g_tls_enabled = true;", on + 1) == std::string::npos,
            "setup(): HTTPS turned on somewhere else");
  const std::string between = known == std::string::npos || on == std::string::npos || on < known
                                  ? std::string()
                                  : ino.substr(known, on - known);
  CHECK_MSG(between.find("if (cert_known) {") != std::string::npos,
            "setup(): HTTPS turned on without init_tls_cert()'s true");
  CHECK_MSG(ino.find("if (g_tls_enabled && g_tls_cert_der && g_tls_key_der) {") != std::string::npos,
            "start_http_server(): the HTTPS listener does not need both buffers");
  // BLE discovery broadcasts the identity only behind its gate (the F364
  // review): the one ble_manager::init() call, and the id it is handed, come
  // after the gate's refusal branch.
  {
    const size_t gate = ino.find("if (!ble_discovery_identity_ready()) {");
    const size_t other = gate == std::string::npos ? gate : ino.find("} else {", gate);
    const size_t init = ino.find("ble_manager::init(");
    const size_t id = ino.find("hex_to_str(ble_device_id_hex, g_device.pubkey_fp, 8);");
    CHECK_MSG(gate != std::string::npos && other != std::string::npos && init != std::string::npos &&
                  id != std::string::npos && gate < other && other < id && id < init &&
                  ino.find("ble_manager::", gate) > other,
              "the BLE bring-up: ble_manager::init() not behind ble_discovery_identity_ready()");
    int named = 0;
    for (size_t at = ino.find("ble_discovery_identity_ready()"); at != std::string::npos;
         at = ino.find("ble_discovery_identity_ready()", at + 1)) {
      named++;
    }
    CHECK_MSG(ino.find("ble_manager::init(", init + 1) == std::string::npos && named == 2,
              "the BLE bring-up: a second ble_manager::init() call, or the gate named " +
                  std::to_string(named) + " time(s) (its definition and the bring-up's)");
    for (const std::string& n : names) {
      if (n == "canary_wap.ino") continue;
      const std::string text = strip_comments(read_file(dir + "/" + n));
      CHECK_MSG(text.find("ble_manager::init(") == std::string::npos,
                n + ": calls ble_manager::init() outside the gated bring-up");
    }
  }
  // setup()'s and loop()'s records into g_last_record go through the helpers
  // the suite drives, and the self-verify through its own.
  {
    struct Once {
      const char* text;
      int count;
    };
    for (const Once& o : {Once{"record_boot_attestation(boot_payload, boot_len);", 1},
                          Once{"Boot attestation:", 1},
                          Once{"const RecordMade made = witness_record_last(payload, payload_len, "
                               "RECORD_WITNESS_EVENT);", 1},
                          Once{"if (made != RecordMade::Refused) print_table_row(&g_last_record, "
                               "&g_fix, g_state);", 1},
                          Once{"print_table_row(", 2},  // the definition and that call
                          Once{"if (made == RecordMade::Made && g_health.records_created % 20 == 0) {", 1},
                          Once{"self_verify_last_record();", 1},
                          Once{"verify_record_signature(&g_last_record)", 2}}) {
      int n = 0;
      for (size_t at = ino.find(o.text); at != std::string::npos; at = ino.find(o.text, at + 1)) n++;
      CHECK_MSG(n == o.count, std::string("canary_wap.ino: \"") + o.text + "\" " + std::to_string(n) +
                                  " time(s), expected " + std::to_string(o.count));
    }
  }
  // No mDNS TXT record carries the certificate's pin.
  for (size_t at = ino.find("addServiceTxt("); at != std::string::npos;
       at = ino.find("addServiceTxt(", at + 1)) {
    const std::string call = ino.substr(at, ino.find(';', at) - at);
    CHECK_MSG(call.find("tls") == std::string::npos && call.find("fp") == std::string::npos,
              "canary_wap.ino:" + std::to_string(line_of(ino, at)) + ": an mDNS TXT record " + call);
  }
}

int main() {
  helpers_known_answers();
  helpers_refuse_each_step();
  provision_completes();
  a_fingerprint_not_hashed_names_nothing();
  a_genesis_head_not_hashed_is_not_persisted();
  a_refused_chain_makes_no_record();
  a_refused_chain_is_shown_and_broadcast_nowhere();
  a_working_unit_shows_its_records();
  a_record_whose_hash_fails_is_not_made();
  a_record_not_made_is_not_shown();
  the_tail_is_not_adopted_on_a_failed_hash();
  the_tail_is_not_vouched_for_on_a_failed_hash();
  a_certificate_not_hashed_is_not_used();
  sketch_text();
  if (g_failures) {
    std::printf("test_wap_sha_fail: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("test_wap_sha_fail: ALL %d CHECKS PASSED (sha256_raw and sha256_domain at every "
              "failing step, the fingerprint, the genesis head, the record payload hash, the chain "
              "link, the SD tail, the TLS pin, the sketch's call sites)\n",
              g_checks);
  return 0;
}
