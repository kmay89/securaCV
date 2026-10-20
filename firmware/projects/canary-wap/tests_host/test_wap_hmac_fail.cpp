// canary-wap's HMAC-SHA256 fails closed (sweep F359).
//
// Before, the sketch's static hmac_sha256() (canary_wap.ino) called
// mbedtls_md_setup(), _hmac_starts(), _hmac_update() and _hmac_finish() and
// checked none of them. When a step fails (mbedtls_md_setup() answers
// MBEDTLS_ERR_MD_ALLOC_FAILED when it cannot allocate the HMAC context, and
// every later step then refuses a context that was never set up), the output
// is not written, or not wholly, so it keeps whatever the caller's stack held,
// or a wrong HMAC when a later step ran on a context a failed start left. The
// SoftAP key (derive_ap_password()) and the API token (derive_api_token(),
// which provision_device() then stores in NVS for every later boot) were both
// rendered from those bytes, and the chain backup (data_mgmt_api.h) was
// written over the last good one with them as its HMAC. Now hmac_sha256()
// answers whether every step ran, zeroes its output when one did not, and
// frees the context on every path, and every caller refuses on false.
//
// This runs the REAL functions, cut verbatim out of the sketch
// (firmware/tests_host/cut_functions.awk): secure_zero(), hmac_sha256(), the
// unambiguous alphabet, format_api_token_string(), derive_api_token(),
// derive_ap_password(), fingerprint_is_nonzero() and resolve_ap_password()
// out of canary_wap.ino (resolve_ap_password() twice, as a release build and
// as a development build), and the backup format's constants, backup_chain()
// and restore_chain() out of data_mgmt_api.h. Under them is a stand-in for
// mbedTLS's streaming HMAC (declared by firmware/tests_host/stubs/mbedtls_md,
// defined below) that runs a real HMAC-SHA256 over OpenSSL, or fails the
// step the test names, and the fake SD card (stubs/sd_fake). What it pins:
//   - hmac_sha256(): RFC 4231's case 2 when every step runs; false and 32
//     zero bytes when the SHA-256 lookup, the setup, the start, the update or
//     the finish fails (a failing finish scribbles first, so the zeros are the
//     helper's own), with every context it set up freed;
//   - derive_ap_password(): the known answers when the HMAC runs (the same as
//     firmware/tests_host/test_ap_password.cpp's); false and "" when it fails,
//     never "cv-222222222222", the key 32 zero bytes render to on every unit;
//   - derive_api_token(): a known answer computed independently (Python's
//     hmac over the same labels, MAC and encoder rule); false and the whole
//     token buffer zeroed when any of its three HMACs fails at any step;
//   - the two mints refuse a null or zero-length buffer before any HMAC
//     runs, and derive_api_token() refuses and wipes a buffer too short for
//     the 35-character token even when every HMAC ran (no caller passes one);
//   - resolve_ap_password(): after a failed provisioning (g_device.initialized
//     false, what provision_device() leaves when a mint refuses) both builds
//     answer from its first branch and run no HMAC: a release build answers
//     false and raises nothing, a development build falls back to its random
//     "dev-" key (the non-release posture sweep F336 describes). With an
//     initialized identity whose key is missing (a state provisioning no
//     longer leaves) and a failing HMAC, its re-derive logs the failure and
//     the same two answers follow, neither build keeping a derived key;
//   - backup_chain(): a failing HMAC writes nothing, so the last good backup
//     survives byte for byte (before, it was replaced by one no restore
//     accepts); restore_chain(): a failing HMAC refuses, so a forged backup
//     whose HMAC field is 32 zero bytes is never accepted against the zeroed
//     output;
//   - the sketch's text (every .ino, .h, .cpp and .c in the sketch directory,
//     comments stripped): every call of hmac_sha256(), derive_api_token() and
//     derive_ap_password() uses its result (the sketch builds without -Werror,
//     so the [[nodiscard]] on data_mgmt_api.h's declaration is a warning
//     there), and provision_device() returns false when either mint refuses.
// Host-tested; not run on a device. provision_device() itself is not driven
// (it needs the whole identity stack); its two refusals are source pins.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run-wap-hmac-fail

#include "arduino_globals.h"

#include <dirent.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "FS.h"           // stubs/sd_fake
#include "SD.h"           // stubs/sd_fake
#include "log_level.h"    // the sketch's LogLevel / LogCategory
#include "mbedtls/md.h"   // firmware/tests_host/stubs/mbedtls_md

#ifndef WAP_HMAC_KEY_INC
#error "WAP_HMAC_KEY_INC must name the cut of canary_wap.ino (the Makefile passes it)"
#endif
#ifndef WAP_HMAC_RESOLVE_INC
#error "WAP_HMAC_RESOLVE_INC must name the cut of resolve_ap_password() (the Makefile passes it)"
#endif
#ifndef WAP_HMAC_DATAMGMT_INC
#error "WAP_HMAC_DATAMGMT_INC must name the cut of data_mgmt_api.h (the Makefile passes it)"
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

// ── the mbedTLS streaming HMAC, real or failing at a named step ──────────────
// hmac_sha256() looks SHA-256 up once per HMAC, before anything else, so the
// lookup is where each HMAC's fate is decided: it runs for real while
// seam::g_pass_first lasts, then fails at seam::g_fail for seam::g_fail_count
// HMACs, then runs for real again (so one failing HMAC of several is not
// hidden by the ones after it failing too). A step after a failed setup
// refuses as mbedTLS does (the context holds no HMAC state), so code that
// ignores the setup's answer gets no output written either.
namespace seam {
enum class Step { None, NoInfo, Setup, Starts, Update, Finish };
static const Step kFailures[] = {Step::NoInfo, Step::Setup, Step::Starts, Step::Update,
                                 Step::Finish};
Step g_fail = Step::None;
int g_pass_first = 0;
int g_fail_count = 0;
Step g_current = Step::None;
int g_lookups = 0;
int g_inits = 0;
int g_frees = 0;
int g_live = 0;  // contexts set up and not yet freed

const char* name_of(Step s) {
  switch (s) {
    case Step::None: return "none";
    case Step::NoInfo: return "mbedtls_md_info_from_type: no SHA-256";
    case Step::Setup: return "mbedtls_md_setup: MBEDTLS_ERR_MD_ALLOC_FAILED";
    case Step::Starts: return "mbedtls_md_hmac_starts fails";
    case Step::Update: return "mbedtls_md_hmac_update fails";
    case Step::Finish: return "mbedtls_md_hmac_finish fails";
  }
  return "?";
}

// From here on: `pass_first` HMACs run for real, the next one fails at
// `fail` (none does when `fail` is None), and every later one runs for real.
void arm(Step fail, int pass_first = 0) {
  g_fail = fail;
  g_pass_first = pass_first;
  g_fail_count = fail == Step::None ? 0 : 1;
  g_lookups = g_inits = g_frees = 0;
}
}  // namespace seam

struct mbedtls_md_info_t {
  mbedtls_md_type_t type;
};
static const mbedtls_md_info_t kSha256Info = {MBEDTLS_MD_SHA256};

extern "C" const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t md_type) {
  ++seam::g_lookups;
  if (seam::g_pass_first > 0) {
    --seam::g_pass_first;
    seam::g_current = seam::Step::None;
  } else if (seam::g_fail_count > 0) {
    --seam::g_fail_count;
    seam::g_current = seam::g_fail;
  } else {
    seam::g_current = seam::Step::None;
  }
  if (seam::g_current == seam::Step::NoInfo || md_type != MBEDTLS_MD_SHA256) return nullptr;
  return &kSha256Info;
}

extern "C" int mbedtls_md_hmac(const mbedtls_md_info_t*, const unsigned char*, size_t,
                               const unsigned char*, size_t, unsigned char*) {
  return MBEDTLS_ERR_MD_BAD_INPUT_DATA;  // the sketch's helper streams; nothing calls this
}

extern "C" void mbedtls_md_init(mbedtls_md_context_t* ctx) {
  ++seam::g_inits;
  std::memset(ctx, 0, sizeof(*ctx));
}

extern "C" void mbedtls_md_free(mbedtls_md_context_t* ctx) {
  ++seam::g_frees;
  if (ctx == nullptr) return;
  if (ctx->hmac_ctx != nullptr) {
    HMAC_CTX_free(static_cast<HMAC_CTX*>(ctx->hmac_ctx));
    --seam::g_live;
  }
  std::memset(ctx, 0, sizeof(*ctx));
}

extern "C" int mbedtls_md_setup(mbedtls_md_context_t* ctx, const mbedtls_md_info_t* md_info,
                                int hmac) {
  if (ctx == nullptr || md_info != &kSha256Info || hmac != 1) return MBEDTLS_ERR_MD_BAD_INPUT_DATA;
  if (seam::g_current == seam::Step::Setup) return MBEDTLS_ERR_MD_ALLOC_FAILED;
  HMAC_CTX* c = HMAC_CTX_new();
  if (c == nullptr) return MBEDTLS_ERR_MD_ALLOC_FAILED;
  ctx->md_info = md_info;
  ctx->hmac_ctx = c;
  ++seam::g_live;
  return 0;
}

static HMAC_CTX* hmac_of(mbedtls_md_context_t* ctx) {
  return ctx == nullptr ? nullptr : static_cast<HMAC_CTX*>(ctx->hmac_ctx);
}

// A start can fail part-way (mbedTLS hashes a long key, then absorbs the
// inner pad, each a digest call a hardware SHA can fail) and leave a context
// the next steps accept. So a failing start here leaves one keyed with
// nothing: code that ignores the start's answer gets a wrong HMAC from the
// update and the finish, not a refusal further on.
extern "C" int mbedtls_md_hmac_starts(mbedtls_md_context_t* ctx, const unsigned char* key,
                                      size_t keylen) {
  HMAC_CTX* c = hmac_of(ctx);
  if (c == nullptr) return MBEDTLS_ERR_MD_BAD_INPUT_DATA;
  if (seam::g_current == seam::Step::Starts) {
    HMAC_Init_ex(c, "", 0, EVP_sha256(), nullptr);
    return MBEDTLS_ERR_MD_BAD_INPUT_DATA;
  }
  return HMAC_Init_ex(c, key, (int)keylen, EVP_sha256(), nullptr) == 1
             ? 0
             : MBEDTLS_ERR_MD_BAD_INPUT_DATA;
}

extern "C" int mbedtls_md_hmac_update(mbedtls_md_context_t* ctx, const unsigned char* input,
                                      size_t ilen) {
  HMAC_CTX* c = hmac_of(ctx);
  if (c == nullptr || seam::g_current == seam::Step::Update) return MBEDTLS_ERR_MD_BAD_INPUT_DATA;
  return HMAC_Update(c, input, ilen) == 1 ? 0 : MBEDTLS_ERR_MD_BAD_INPUT_DATA;
}

extern "C" int mbedtls_md_hmac_finish(mbedtls_md_context_t* ctx, unsigned char* output) {
  HMAC_CTX* c = hmac_of(ctx);
  if (c == nullptr) return MBEDTLS_ERR_MD_BAD_INPUT_DATA;
  if (seam::g_current == seam::Step::Finish) {
    std::memset(output, 0xA5, 32);  // a partial write; the zeros after are the helper's
    return MBEDTLS_ERR_MD_BAD_INPUT_DATA;
  }
  unsigned int olen = 0;
  return HMAC_Final(c, output, &olen) == 1 && olen == 32 ? 0 : MBEDTLS_ERR_MD_BAD_INPUT_DATA;
}

// An HMAC-SHA256 that goes through none of the above, for expected values.
static std::string openssl_hmac(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen) {
  unsigned char out[32];
  unsigned int olen = 0;
  if (HMAC(EVP_sha256(), key, (int)klen, msg, mlen, out, &olen) == nullptr || olen != 32) return "";
  return std::string(reinterpret_cast<const char*>(out), 32);
}

// ── what the cut functions call ─────────────────────────────────────────────
typedef enum { ESP_MAC_WIFI_STA = 0 } esp_mac_type_t;
static const uint8_t kStaMac[6] = {0x02, 0x5e, 0x10, 0x20, 0x30, 0x40};
static int esp_read_mac(uint8_t* mac, esp_mac_type_t) {
  std::memcpy(mac, kStaMac, sizeof(kStaMac));
  return 0;
}

static uint32_t g_random = 0x1234abcd;
static uint32_t esp_random() { return g_random++; }

static uint32_t g_now_ms = 5000;
static uint32_t millis() { return g_now_ms; }

static bool sd_is_available() { return SD.present; }

struct LogLine {
  LogLevel level;
  LogCategory category;
  std::string message;
};
static std::vector<LogLine> g_log;
static void log_health(LogLevel level, LogCategory category, const char* message,
                       const char* /*detail*/) {
  g_log.push_back({level, category, message ? message : ""});
}
static bool logged(const char* message) {
  for (const auto& l : g_log) {
    if (l.message == message) return true;
  }
  return false;
}

// The members of the sketch's DeviceIdentity the cut functions read.
struct DeviceIdentity {
  uint8_t privkey[32];
  uint8_t pubkey_fp[8];
  bool initialized;
  char ap_password[16];
};
static DeviceIdentity g_device;

// ── the sketch, verbatim ─────────────────────────────────────────────────────
#include WAP_HMAC_KEY_INC

namespace release_build {
#define SECURACV_RELEASE_BUILD 1
#include WAP_HMAC_RESOLVE_INC
#undef SECURACV_RELEASE_BUILD
}  // namespace release_build

namespace dev_build {
#define SECURACV_RELEASE_BUILD 0
#include WAP_HMAC_RESOLVE_INC
#undef SECURACV_RELEASE_BUILD
}  // namespace dev_build

namespace datamgmt {
static bool s_backup_exists = false;
static uint32_t s_last_backup_ms = 0;
#include WAP_HMAC_DATAMGMT_INC
}  // namespace datamgmt

// ── fixtures ────────────────────────────────────────────────────────────────
static void key_0x01(uint8_t (&k)[32]) {
  for (size_t i = 0; i < 32; i++) k[i] = (uint8_t)(i + 1);
}

static bool all_zero(const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++) {
    if (b[i] != 0) return false;
  }
  return true;
}

static std::string hex(const std::string& s) {
  static const char* d = "0123456789abcdef";
  std::string out;
  for (unsigned char c : s) {
    out += d[c >> 4];
    out += d[c & 15];
  }
  return out;
}

// ── hmac_sha256() ───────────────────────────────────────────────────────────
static void hmac_known_answer() {
  seam::arm(seam::Step::None);
  uint8_t out[32];
  std::memset(out, 0xA5, sizeof(out));
  const bool ran = hmac_sha256(reinterpret_cast<const uint8_t*>("Jefe"), 4,
                               reinterpret_cast<const uint8_t*>("what do ya want for nothing?"), 28,
                               out);
  CHECK_MSG(ran, "a working HMAC answers true");
  CHECK_MSG(hex(std::string(reinterpret_cast<const char*>(out), 32)) ==
                "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
            "RFC 4231 case 2: " + hex(std::string(reinterpret_cast<const char*>(out), 32)));
  CHECK_MSG(seam::g_inits == 1 && seam::g_frees == 1 && seam::g_live == 0,
            "one context, set up and freed");
}

static void hmac_refuses_each_step() {
  uint8_t key[32];
  key_0x01(key);
  for (seam::Step s : seam::kFailures) {
    seam::arm(s);
    uint8_t out[32];
    std::memset(out, 0xA5, sizeof(out));  // what the caller's stack held
    const bool ran = hmac_sha256(key, sizeof(key), reinterpret_cast<const uint8_t*>("m"), 1, out);
    CHECK_MSG(!ran, std::string(seam::name_of(s)) + ": answered true");
    CHECK_MSG(all_zero(out, sizeof(out)),
              std::string(seam::name_of(s)) + ": output not zeroed: " +
                  hex(std::string(reinterpret_cast<const char*>(out), 32)));
    CHECK_MSG(seam::g_inits == seam::g_frees,
              std::string(seam::name_of(s)) + ": " + std::to_string(seam::g_inits) +
                  " init(s), " + std::to_string(seam::g_frees) + " free(s)");
    CHECK_MSG(seam::g_live == 0, std::string(seam::name_of(s)) + ": a context left allocated");
  }
  // And the next HMAC runs again.
  seam::arm(seam::Step::None);
  uint8_t out[32];
  CHECK_MSG(hmac_sha256(key, sizeof(key), reinterpret_cast<const uint8_t*>("m"), 1, out) &&
                std::string(reinterpret_cast<const char*>(out), 32) ==
                    openssl_hmac(key, sizeof(key), reinterpret_cast<const uint8_t*>("m"), 1),
            "a working HMAC after the failures");
}

// ── derive_ap_password() ────────────────────────────────────────────────────
static std::string ap_of(const uint8_t (&k)[32], bool* ok) {
  char out[16];
  std::memset(out, 'Z', sizeof(out));
  *ok = derive_ap_password(k, out, sizeof(out));
  return std::string(out, strnlen(out, sizeof(out)));
}

static void ap_password_known_answers() {
  seam::arm(seam::Step::None);
  bool ok = false;
  uint8_t k[32];
  key_0x01(k);
  std::string pw = ap_of(k, &ok);
  CHECK_MSG(ok && pw == "cv-e8Qun72aUeCc", "key 0x01..0x20: " + pw);
  uint8_t z[32] = {0};
  pw = ap_of(z, &ok);
  CHECK_MSG(ok && pw == "cv-3jByJ6rHdhM2", "key of 32 zero bytes: " + pw);
}

static void ap_password_refuses_a_failed_hmac() {
  uint8_t k[32];
  key_0x01(k);
  for (seam::Step s : seam::kFailures) {
    seam::arm(s);
    bool ok = true;
    const std::string pw = ap_of(k, &ok);
    CHECK_MSG(!ok, std::string(seam::name_of(s)) + ": answered true with " + pw);
    CHECK_MSG(pw.empty(), std::string(seam::name_of(s)) + ": wrote \"" + pw + "\"");
    CHECK_MSG(pw != "cv-222222222222", std::string(seam::name_of(s)) + ": the all-units key");
    CHECK_MSG(seam::g_lookups == 1, std::string(seam::name_of(s)) + ": one HMAC");
  }
}

// ── derive_api_token() ──────────────────────────────────────────────────────
static void api_token_known_answer() {
  seam::arm(seam::Step::None);
  uint8_t k[32];
  key_0x01(k);
  char tok[36];
  std::memset(tok, 'Z', sizeof(tok));
  const bool ok = derive_api_token(k, tok, sizeof(tok));
  CHECK_MSG(ok, "a working HMAC mints the token");
  CHECK_MSG(std::string(tok) == "cv_J2mAQ2t94sRjp9AeW8YWqmHt2cyv7sGB",
            std::string("key 0x01..0x20, STA MAC 02:5e:10:20:30:40: ") + tok);
  CHECK_MSG(seam::g_lookups == 3, "three HMACs: " + std::to_string(seam::g_lookups));
}

static void api_token_refuses_any_failed_hmac() {
  uint8_t k[32];
  key_0x01(k);
  for (int which = 0; which < 3; which++) {  // the token key, the first block, the second
    for (seam::Step s : seam::kFailures) {
      seam::arm(s, which);
      char tok[36];
      std::memset(tok, 'Z', sizeof(tok));
      const bool ok = derive_api_token(k, tok, sizeof(tok));
      const std::string what =
          "HMAC " + std::to_string(which + 1) + " of 3, " + seam::name_of(s);
      CHECK_MSG(!ok, what + ": answered true with " + std::string(tok, strnlen(tok, sizeof(tok))));
      CHECK_MSG(all_zero(tok, sizeof(tok)),
                what + ": the token buffer holds " + std::string(tok, strnlen(tok, sizeof(tok))));
      CHECK_MSG(seam::g_inits == seam::g_frees && seam::g_live == 0, what + ": a context leaked");
    }
  }
}

// The two mints' own guards: a null or zero-length buffer is refused before
// any HMAC runs, and a token buffer too short for the 35-character token is
// refused and wiped even when every HMAC ran (no caller passes one: the
// buffers are g_device's, 36 and 16 bytes).
static void mints_refuse_a_buffer_they_cannot_fill() {
  uint8_t k[32];
  key_0x01(k);
  char tok[36];
  seam::arm(seam::Step::None);
  CHECK_MSG(!derive_api_token(k, nullptr, sizeof(tok)), "derive_api_token(): a null buffer");
  CHECK_MSG(seam::g_lookups == 0, "derive_api_token(): an HMAC ran for a null buffer");
  std::memset(tok, 'Z', sizeof(tok));
  CHECK_MSG(!derive_api_token(k, tok, 0), "derive_api_token(): a zero-length buffer");
  CHECK_MSG(seam::g_lookups == 0, "derive_api_token(): an HMAC ran for a zero-length buffer");
  CHECK_MSG(tok[0] == 'Z', "derive_api_token(): wrote into a zero-length buffer");

  char pw[16];
  CHECK_MSG(!derive_ap_password(k, nullptr, sizeof(pw)), "derive_ap_password(): a null buffer");
  std::memset(pw, 'Z', sizeof(pw));
  CHECK_MSG(!derive_ap_password(k, pw, 0), "derive_ap_password(): a zero-length buffer");
  CHECK_MSG(pw[0] == 'Z', "derive_ap_password(): wrote into a zero-length buffer");
  CHECK_MSG(seam::g_lookups == 0, "derive_ap_password(): an HMAC ran for an unusable buffer");

  seam::arm(seam::Step::None);
  std::memset(tok, 'Z', sizeof(tok));
  CHECK_MSG(!derive_api_token(k, tok, 20), "derive_api_token(): a 20-byte buffer answered true");
  CHECK_MSG(seam::g_lookups == 3, "derive_api_token(): three HMACs ran for the short buffer");
  CHECK_MSG(all_zero(tok, 20), "derive_api_token(): a short token left in a 20-byte buffer: " +
                                   std::string(tok, strnlen(tok, 20)));
  CHECK_MSG(tok[20] == 'Z', "derive_api_token(): wrote past the 20 bytes it was given");
}

// ── resolve_ap_password() ───────────────────────────────────────────────────
static void provisioned_without_a_key() {
  std::memset(&g_device, 0, sizeof(g_device));
  key_0x01(g_device.privkey);
  g_device.pubkey_fp[0] = 0x5a;
  g_device.initialized = true;
  g_log.clear();
}

static void resolve_ap_password_release_and_dev() {
  // A working HMAC: both builds derive and keep the device key.
  for (int dev = 0; dev < 2; dev++) {
    provisioned_without_a_key();
    seam::arm(seam::Step::None);
    char out[32];
    std::memset(out, 'Z', sizeof(out));
    const bool ok = dev ? dev_build::resolve_ap_password(out, sizeof(out))
                        : release_build::resolve_ap_password(out, sizeof(out));
    const char* b = dev ? "development build" : "release build";
    CHECK_MSG(ok && std::string(out) == "cv-e8Qun72aUeCc", std::string(b) + ": " + out);
    CHECK_MSG(std::string(g_device.ap_password) == "cv-e8Qun72aUeCc",
              std::string(b) + ": kept " + g_device.ap_password);
  }
  for (seam::Step s : seam::kFailures) {
    // Release: no key, no SoftAP.
    provisioned_without_a_key();
    seam::arm(s);
    char out[32];
    std::memset(out, 'Z', sizeof(out));
    bool ok = release_build::resolve_ap_password(out, sizeof(out));
    const std::string what = seam::name_of(s);
    CHECK_MSG(!ok, "release build, " + what + ": answered true with " +
                       std::string(out, strnlen(out, sizeof(out))));
    CHECK_MSG(g_device.ap_password[0] == '\0',
              "release build, " + what + ": kept \"" + g_device.ap_password + "\"");
    CHECK_MSG(logged("AP password not derived"), "release build, " + what + ": no log line");
    CHECK_MSG(logged("AP password unavailable"), "release build, " + what + ": no refusal line");

    // Development: the random fallback, and no derived key kept.
    provisioned_without_a_key();
    seam::arm(s);
    std::memset(out, 'Z', sizeof(out));
    ok = dev_build::resolve_ap_password(out, sizeof(out));
    CHECK_MSG(ok && std::strncmp(out, "dev-", 4) == 0 && std::strlen(out) == 12,
              "development build, " + what + ": " + std::string(out, strnlen(out, sizeof(out))));
    CHECK_MSG(g_device.ap_password[0] == '\0',
              "development build, " + what + ": kept \"" + g_device.ap_password + "\"");
    CHECK_MSG(logged("AP password not derived"), "development build, " + what + ": no log line");
  }
}

// What a failed provisioning leaves (provision_device() refused a mint, so
// g_device.initialized stays false): resolve_ap_password() answers from its
// first branch and runs no HMAC. A release build refuses with "AP password
// unavailable" and raises nothing; a development build uses its random
// "dev-" key. The re-derive further down (checked and logged since F359) is
// for an initialized identity whose key is missing, a state provisioning no
// longer leaves.
static void resolve_ap_password_after_a_failed_provisioning() {
  for (int dev = 0; dev < 2; dev++) {
    std::memset(&g_device, 0, sizeof(g_device));
    key_0x01(g_device.privkey);
    g_device.pubkey_fp[0] = 0x5a;
    g_device.initialized = false;
    g_log.clear();
    seam::arm(seam::Step::None);
    char out[32];
    std::memset(out, 'Z', sizeof(out));
    const bool ok = dev ? dev_build::resolve_ap_password(out, sizeof(out))
                        : release_build::resolve_ap_password(out, sizeof(out));
    const std::string b = dev ? "development build" : "release build";
    CHECK_MSG(seam::g_lookups == 0, b + ": an HMAC ran for an identity not provisioned");
    CHECK_MSG(g_device.ap_password[0] == '\0', b + ": kept \"" + g_device.ap_password + "\"");
    CHECK_MSG(!logged("AP password not derived"), b + ": took the derive branch");
    if (dev) {
      CHECK_MSG(ok && std::strncmp(out, "dev-", 4) == 0 && std::strlen(out) == 12,
                b + ": " + std::string(out, strnlen(out, sizeof(out))));
      CHECK_MSG(logged("AP debug password generated"), b + ": no fallback line");
    } else {
      CHECK_MSG(!ok, b + ": answered true with " + std::string(out, strnlen(out, sizeof(out))));
      CHECK_MSG(out[0] == 'Z', b + ": wrote a key");
      CHECK_MSG(logged("AP password unavailable"), b + ": no refusal line");
    }
  }
}

// ── backup_chain() / restore_chain() ────────────────────────────────────────
static const char* kBackup = "/CHAIN/backup.bin";

static void fresh_card() {
  FakeSD& sd = SD;
  sd.present = true;
  sd.files.clear();
  sd.dirs.clear();
  sd.writes = 0;
  datamgmt::s_backup_exists = false;
  datamgmt::s_last_backup_ms = 0;
  g_log.clear();
}

static void chain_head(uint8_t (&h)[32], uint8_t seed) {
  for (size_t i = 0; i < 32; i++) h[i] = (uint8_t)(seed + i * 7);
}

static void backup_keeps_the_last_good_one() {
  uint8_t key[32];
  key_0x01(key);
  uint8_t head7[32], head8[32];
  chain_head(head7, 7);
  chain_head(head8, 8);

  for (seam::Step s : seam::kFailures) {
    fresh_card();
    seam::arm(seam::Step::None);
    CHECK_MSG(datamgmt::backup_chain(head7, 7, key), "a working HMAC writes the backup");
    const std::string good = SD.files[kBackup];
    CHECK_MSG(good.size() == datamgmt::BACKUP_SIZE, "a backup of " + std::to_string(good.size()));
    CHECK_MSG(good.substr(datamgmt::BACKUP_PAYLOAD) ==
                  openssl_hmac(key, 32, reinterpret_cast<const uint8_t*>(good.data()),
                               datamgmt::BACKUP_PAYLOAD),
              "the backup's HMAC is HMAC-SHA256 of its payload under the device key");

    const size_t writes = SD.writes;
    g_log.clear();
    seam::arm(s);
    const bool wrote = datamgmt::backup_chain(head8, 8, key);
    const std::string what = seam::name_of(s);
    CHECK_MSG(!wrote, what + ": answered true");
    CHECK_MSG(SD.files[kBackup] == good, what + ": the last good backup was replaced");
    CHECK_MSG(SD.writes == writes, what + ": " + std::to_string(SD.writes - writes) +
                                       " write(s) to the card");
    CHECK_MSG(logged("Chain backup: HMAC failed"), what + ": no log line");
    CHECK_MSG(!logged("Chain backup written"), what + ": logged a backup");

    // The good one still restores.
    seam::arm(seam::Step::None);
    uint8_t got[32] = {0};
    uint32_t seq = 0;
    CHECK_MSG(datamgmt::restore_chain(got, &seq, key) && seq == 7 &&
                  std::memcmp(got, head7, 32) == 0,
              what + ": the surviving backup does not restore seq 7");
  }
}

// A backup an attacker with the card could write: an older seq, and an HMAC
// field of 32 zero bytes, which is what a failed HMAC's zeroed output equals.
static std::string forged_backup(uint32_t seq, const uint8_t (&head)[32]) {
  uint8_t buf[76] = {0};
  std::memcpy(buf + 0, &datamgmt::BACKUP_MAGIC, 4);
  std::memcpy(buf + 4, &datamgmt::BACKUP_VERSION, 4);
  std::memcpy(buf + 8, &seq, 4);
  std::memcpy(buf + 12, head, 32);
  return std::string(reinterpret_cast<const char*>(buf), sizeof(buf));
}

static void restore_refuses_a_failed_hmac() {
  uint8_t key[32];
  key_0x01(key);
  uint8_t head1[32];
  chain_head(head1, 1);

  fresh_card();
  SD.dirs.insert("/CHAIN");
  SD.files[kBackup] = forged_backup(1, head1);

  seam::arm(seam::Step::None);
  uint8_t got[32];
  std::memset(got, 0xEE, sizeof(got));
  uint32_t seq = 0xEEEEEEEE;
  CHECK_MSG(!datamgmt::restore_chain(got, &seq, key), "a working HMAC refuses the forgery");
  CHECK_MSG(logged("Chain restore: HMAC mismatch (tampered?)"), "no mismatch line");

  for (seam::Step s : seam::kFailures) {
    g_log.clear();
    seam::arm(s);
    std::memset(got, 0xEE, sizeof(got));
    seq = 0xEEEEEEEE;
    const bool restored = datamgmt::restore_chain(got, &seq, key);
    const std::string what = seam::name_of(s);
    CHECK_MSG(!restored, what + ": restored the forged backup (seq " + std::to_string(seq) + ")");
    CHECK_MSG(seq == 0xEEEEEEEE, what + ": wrote the seq");
    bool untouched = true;
    for (uint8_t b : got) untouched = untouched && b == 0xEE;
    CHECK_MSG(untouched, what + ": wrote the chain head");
    CHECK_MSG(logged("Chain restore: HMAC compute failed"), what + ": no log line");
  }
}

// ── the sketch's text ───────────────────────────────────────────────────────
static std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static bool is_ident(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Comments become spaces (newlines kept); string, raw-string and character
// literals are kept as they are, so a "//" inside one is not a comment.
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
    if (pos > 0 && is_ident(text[pos - 1])) continue;
    size_t after = pos + name.size();
    while (after < text.size() && (text[after] == ' ' || text[after] == '\t')) after++;
    if (after >= text.size() || text[after] != '(') continue;
    size_t b = pos;
    while (b > 0 && std::isspace(static_cast<unsigned char>(text[b - 1]))) b--;
    size_t w = b;
    while (w > 0 && is_ident(text[w - 1])) w--;
    const std::string word = text.substr(w, b - w);
    if (word == "bool") continue;  // its declaration or definition
    ++calls;
    const char prev = b > 0 ? text[b - 1] : '\0';
    const bool used = word == "return" || prev == '!' || prev == '=' || prev == '(' ||
                      prev == ',' || (prev == '&' && b > 1 && text[b - 2] == '&') ||
                      (prev == '|' && b > 1 && text[b - 2] == '|');
    // `(void)name(` ends in `(`'s partner `)`, so it is not `(`: a discard.
    CHECK_MSG(used, file + ":" + std::to_string(line_of(text, pos)) + ": " + name +
                        "() is called and its result dropped");
  }
  return calls;
}

// The body of the column-0 function whose head line starts with `head`, up
// to its first column-0 "}" line.
static std::string body_of(const std::string& text, const std::string& head) {
  const size_t start = text.find("\n" + head);
  if (start == std::string::npos) return "";
  const size_t end = text.find("\n}\n", start + 1);
  if (end == std::string::npos) return "";
  return text.substr(start + 1, end - start + 2);
}

// `stmt` occurs once in `body`, and the block it opens returns false.
static void refuses_on(const std::string& body, const std::string& stmt, const std::string& what) {
  const size_t at = body.find(stmt);
  CHECK_MSG(at != std::string::npos, what + ": no `" + stmt + "`");
  if (at == std::string::npos) return;
  CHECK_MSG(body.find(stmt, at + 1) == std::string::npos, what + ": `" + stmt + "` twice");
  const size_t close = body.find('}', at + stmt.size());
  const std::string block = body.substr(at, close == std::string::npos ? 0 : close - at);
  CHECK_MSG(block.find("return false;") != std::string::npos,
            what + ": the block after `" + stmt + "` does not return false");
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

  int hmac_calls = 0, token_calls = 0, ap_calls = 0;
  std::string ino;
  for (const std::string& n : names) {
    const std::string text = strip_comments(read_file(dir + "/" + n));
    if (n == "canary_wap.ino") ino = text;
    hmac_calls += calls_use_their_result(n, text, "hmac_sha256");
    token_calls += calls_use_their_result(n, text, "derive_api_token");
    ap_calls += calls_use_their_result(n, text, "derive_ap_password");
  }
  // At least the calls this suite drives: four in the two mints, two in the
  // chain backup; provision_device()'s token mint; provision_device()'s and
  // resolve_ap_password()'s key.
  CHECK_MSG(hmac_calls >= 6, "hmac_sha256() called " + std::to_string(hmac_calls) + " time(s)");
  CHECK_MSG(token_calls >= 1, "derive_api_token() called " + std::to_string(token_calls) + " time(s)");
  CHECK_MSG(ap_calls >= 2, "derive_ap_password() called " + std::to_string(ap_calls) + " time(s)");

  const std::string prov = body_of(ino, "static bool provision_device() {");
  CHECK_MSG(!prov.empty(), "canary_wap.ino: no provision_device()");
  refuses_on(prov,
             "if (!derive_api_token(g_device.privkey, g_device.api_token_str, "
             "sizeof(g_device.api_token_str))) {",
             "provision_device()'s token mint");
  refuses_on(prov,
             "if (!derive_ap_password(g_device.privkey, g_device.ap_password, "
             "sizeof(g_device.ap_password))) {",
             "provision_device()'s SoftAP key");
  // Both mints run before the identity is marked ready, so a refusal leaves
  // g_device.initialized false.
  const size_t ready = prov.find("g_device.initialized = true;");
  CHECK_MSG(ready != std::string::npos && prov.find("derive_ap_password(") < ready &&
                prov.find("derive_api_token(") < ready,
            "provision_device(): a mint after g_device.initialized = true");
}

int main() {
  hmac_known_answer();
  hmac_refuses_each_step();
  ap_password_known_answers();
  ap_password_refuses_a_failed_hmac();
  api_token_known_answer();
  api_token_refuses_any_failed_hmac();
  mints_refuse_a_buffer_they_cannot_fill();
  resolve_ap_password_release_and_dev();
  resolve_ap_password_after_a_failed_provisioning();
  backup_keeps_the_last_good_one();
  restore_refuses_a_failed_hmac();
  sketch_text();
  if (g_failures) {
    std::printf("test_wap_hmac_fail: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("test_wap_hmac_fail: ALL %d CHECKS PASSED (hmac_sha256 at every failing step, "
              "the SoftAP key, the API token, resolve_ap_password in both builds, the chain "
              "backup and restore, the sketch's call sites)\n",
              g_checks);
  return 0;
}
