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
//     runs, and derive_api_token() refuses and wipes a buffer shorter than
//     the 35-character token and its NUL before any HMAC runs, so a 1-, 2-,
//     3-, 34- or 35-byte window inside a guarded array is refused with no
//     byte written past it and a 36-byte one mints the known token (no
//     caller passes a short one); format_api_token_string() writes nothing
//     past the length it is given, from 0 to 40 bytes;
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
//   - provision_device(), cut verbatim too, over stand-ins for everything it
//     calls but the two mints: a refused mint (either HMAC at any step) or a
//     token whose store fails answers false with g_device.initialized false,
//     and leaves the chain state as NVS holds it (or the genesis head,
//     persisted), with nothing written over it. setup() goes on after that
//     (sweep F336), and the boot record and loop() sign with the stored key,
//     so a chain restarted at seq 0 from a zero head, which the mints left
//     when they came ahead of the chain load, re-signed seq numbers the key
//     had already used;
//   - the sketch's text (every .ino, .h, .cpp and .c in the sketch directory,
//     comments stripped): every call of hmac_sha256(), derive_api_token() and
//     derive_ap_password() uses its result (the sketch builds without -Werror,
//     so the [[nodiscard]] on data_mgmt_api.h's declaration is a warning
//     there), provision_device() returns false when either mint refuses, and
//     every return false in it but establish_identity_key()'s comes after
//     the chain state is loaded.
// Host-tested; not run on a device.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run-wap-hmac-fail

#include "arduino_globals.h"

#include <dirent.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <sys/wait.h>
#include <unistd.h>

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

#include "chain_state.h"  // the sketch's {seq, head} blob, pure

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
#ifndef WAP_HMAC_PROV_INC
#error "WAP_HMAC_PROV_INC must name the cut of provision_device() (the Makefile passes it)"
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

// The members of the sketch's DeviceIdentity the cut functions read, with
// the sketch's sizes (sketch_text() holds the two buffers the mints fill to
// them).
struct DeviceIdentity {
  uint8_t privkey[32];
  uint8_t pubkey[32];
  uint8_t pubkey_fp[8];
  uint8_t chain_head[32];
  uint32_t seq;
  uint32_t seq_persisted;
  uint32_t boot_count;
  uint32_t boot_ms;
  uint32_t log_seq;
  uint32_t born_day;
  bool born_exact;
  bool initialized;
  char device_id[32];
  char ap_ssid[33];
  char mdns_hostname[40];
  char api_token_str[36];
  char ap_password[16];
  char fingerprint_hex[17];
  bool first_boot;
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
// any HMAC runs, and a token buffer too short for the 35-character token and
// its NUL is refused and wiped before any HMAC runs (no caller passes one:
// the buffers are g_device's, 36 and 16 bytes).
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
  CHECK_MSG(seam::g_lookups == 0, "derive_api_token(): " + std::to_string(seam::g_lookups) +
                                      " HMAC(s) ran for the short buffer");
  CHECK_MSG(all_zero(tok, 20), "derive_api_token(): a short token left in a 20-byte buffer: " +
                                   std::string(tok, strnlen(tok, 20)));
  CHECK_MSG(tok[20] == 'Z', "derive_api_token(): wrote past the 20 bytes it was given");
}

// A window of `len` bytes inside a guarded array: kGuard bytes of 'G' on
// each side, the window itself filled with 'Z'. intact() holds every guard
// byte, so a write before or past the window fails it.
namespace {
constexpr size_t kGuard = 48;
struct Window {
  char bytes[kGuard + 64 + kGuard];
  size_t len;
  explicit Window(size_t n) : len(n) {
    std::memset(bytes, 'G', sizeof(bytes));
    std::memset(bytes + kGuard, 'Z', n);
  }
  char* at() { return bytes + kGuard; }
  bool intact() const {
    for (size_t i = 0; i < kGuard; i++) {
      if (bytes[i] != 'G') return false;
    }
    for (size_t i = kGuard + len; i < sizeof(bytes); i++) {
      if (bytes[i] != 'G') return false;
    }
    return true;
  }
  std::string text() const { return std::string(bytes + kGuard, strnlen(bytes + kGuard, len)); }
};
}  // namespace

// derive_api_token() over a 1-, 2-, 3-, 34-, 35- and 36-byte window (the
// F359 review). Every window shorter than the token and its NUL (36) is
// refused before any HMAC runs, with the window wiped, and no byte past it
// is written: before, the 1- to 3-byte windows ran all three HMACs and the
// formatter wrote "cv_" and a NUL past their end. The 36-byte window mints
// the known token, with nothing written outside it.
static void token_windows_are_never_overrun() {
  uint8_t k[32];
  key_0x01(k);
  for (size_t len : {size_t(1), size_t(2), size_t(3), size_t(34), size_t(35), size_t(36)}) {
    seam::arm(seam::Step::None);
    Window w(len);
    const bool ok = derive_api_token(k, w.at(), len);
    const std::string what = "derive_api_token(), a " + std::to_string(len) + "-byte window";
    CHECK_MSG(w.intact(), what + ": wrote outside it");
    if (len < 36) {
      CHECK_MSG(!ok, what + ": answered true with " + w.text());
      CHECK_MSG(seam::g_lookups == 0,
                what + ": " + std::to_string(seam::g_lookups) + " HMAC(s) ran");
      CHECK_MSG(all_zero(w.at(), len), what + ": not wiped: " + w.text());
    } else {
      CHECK_MSG(ok, what + ": refused");
      CHECK_MSG(w.text() == "cv_J2mAQ2t94sRjp9AeW8YWqmHt2cyv7sGB", what + ": " + w.text());
      CHECK_MSG(seam::g_lookups == 3, what + ": " + std::to_string(seam::g_lookups) + " HMAC(s)");
    }
  }
}

// format_api_token_string() itself writes nothing past the length it is
// given, whatever the length: no byte at 0, "" below "cv_" and its NUL, a
// cut token from 4 to 35, the whole token at 36 and above. Before, a 1- to
// 3-byte window took the prefix and a NUL past its end, and a 0-byte one
// wrapped `out_len - 1` and took a whole token. A null output has no window
// to look at, so what that case holds is that the call returns: it runs in a
// child process, and a write through the pointer (the child dying on a
// signal) is a failed check here, not a crash of the whole suite.
static void the_token_formatter_stays_in_its_window() {
  uint8_t in[64];
  for (size_t i = 0; i < sizeof(in); i++) in[i] = (uint8_t)(i * 37 + 11);
  Window whole(36);
  format_api_token_string(in, sizeof(in), whole.at(), 36);
  const std::string token = whole.text();
  CHECK_MSG(token.size() == 35 && token.compare(0, 3, "cv_") == 0, "the whole token: " + token);
  for (size_t len = 0; len <= 40; len++) {
    Window w(len);
    format_api_token_string(in, sizeof(in), w.at(), len);
    const std::string what = "format_api_token_string(), a " + std::to_string(len) + "-byte window";
    CHECK_MSG(w.intact(), what + ": wrote outside it");
    if (len == 0) continue;
    CHECK_MSG(std::memchr(w.at(), '\0', len) != nullptr, what + ": no NUL inside it");
    const std::string want = len < 4 ? "" : token.substr(0, len - 1);
    CHECK_MSG(w.text() == want, what + ": " + w.text());
  }
  std::fflush(nullptr);
  const pid_t child = fork();
  if (child == 0) {
    format_api_token_string(in, sizeof(in), nullptr, 4);
    _exit(0);
  }
  int status = 0;
  std::string how = "fork failed";
  if (child > 0 && waitpid(child, &status, 0) == child) {
    how = WIFSIGNALED(status) ? "signal " + std::to_string(WTERMSIG(status))
                              : "exit " + std::to_string(WEXITSTATUS(status));
  }
  CHECK_MSG(how == "exit 0",
            "format_api_token_string(): a null output did not return (" + how + ")");
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

// ── provision_device() after a refused mint ─────────────────────────────────
// provision_device(), cut verbatim, over stand-ins for everything it calls
// but the two mints, which are the real ones above over the HMAC seam. NVS
// is a map that outlives each call, as NVS outlives a reboot. The key is a
// stand-in's (establish_identity_key() is test_wap_first_boot_nvs.cpp's),
// and so are the public key, the fingerprint and the names derived from it.
namespace prov {
std::map<std::string, std::vector<uint8_t>> blobs;  // nvs_load_bytes()'s entries
std::map<std::string, uint32_t> words;              // nvs_load_u32()'s entries
std::string token;                                  // the stored API token ("" = none)
bool store_token_fails = false;
bool establish_fails = false;
int config_loads = 0;
int persists = 0;
int token_stores = 0;
std::string serial;

void clear() {
  blobs.clear();
  words.clear();
  token.clear();
  store_token_fails = establish_fails = false;
  config_loads = persists = token_stores = 0;
  serial.clear();
}
}  // namespace prov

struct HostSerial {
  void println(const char* s) {
    prov::serial += s;
    prov::serial += '\n';
  }
  __attribute__((format(printf, 2, 3))) int printf(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    prov::serial += line;
    return n;
  }
};
static HostSerial Serial;

struct HostEsp {
  uint32_t getFreeHeap() { return 123456; }
};
static HostEsp ESP;

struct HostHealth {
  bool crypto_healthy;
  uint32_t min_heap;
};
static HostHealth g_health;

static const char* NVS_KEY_SEQ = "seq";
static const char* NVS_KEY_BOOTS = "boots";
static const char* NVS_KEY_CHAIN = "chain";
static const char* NVS_KEY_CHAINST = "chain_st";
static const char* NVS_KEY_LOGSEQ = "logseq";
static const char* NVS_KEY_BORN = "born_day";
static const char* NVS_KEY_BORN_EX = "born_exact";

static uint32_t nvs_load_u32(const char* key, uint32_t def = 0) {
  auto it = prov::words.find(key);
  return it == prov::words.end() ? def : it->second;
}
static bool nvs_store_u32(const char* key, uint32_t val) {
  prov::words[key] = val;
  return true;
}
static bool nvs_load_bytes(const char* key, uint8_t* out, size_t len) {
  auto it = prov::blobs.find(key);
  if (it == prov::blobs.end() || it->second.size() != len) return false;
  std::memcpy(out, it->second.data(), len);
  return true;
}
static bool nvs_load_token(char* token_str, size_t max_len) {
  if (prov::token.empty() || prov::token.size() >= max_len) return false;
  std::memcpy(token_str, prov::token.c_str(), prov::token.size() + 1);
  return std::strlen(token_str) >= 35;
}
static bool nvs_store_token(const char* token_str) {
  if (prov::store_token_fails) return false;
  ++prov::token_stores;
  prov::token = token_str;
  return true;
}

static bool establish_identity_key() {
  if (prov::establish_fails) return false;
  key_0x01(g_device.privkey);
  return true;
}
struct Ed25519 {
  static void derivePublicKey(uint8_t pub[32], const uint8_t priv[32]) {
    for (size_t i = 0; i < 32; i++) pub[i] = (uint8_t)(priv[i] ^ 0x5c);
  }
};
static void compute_fingerprint(const uint8_t pub[32], uint8_t fp[8]) { std::memcpy(fp, pub, 8); }
static void hex_to_str(char* out, const uint8_t* d, size_t n) {
  for (size_t i = 0; i < n; i++) std::snprintf(out + 2 * i, 3, "%02X", d[i]);
}
static void generate_device_id(char* out, size_t cap) {
  std::snprintf(out, cap, "wap-%02x%02x", g_device.pubkey_fp[0], g_device.pubkey_fp[1]);
}
static void generate_ap_ssid(char* out, size_t cap) {
  std::snprintf(out, cap, "SecuraCV-wap-%02x%02x", g_device.pubkey_fp[0], g_device.pubkey_fp[1]);
}
static void generate_mdns_hostname(char* out, size_t cap) {
  std::snprintf(out, cap, "canary-%02x%02x", g_device.pubkey_fp[0], g_device.pubkey_fp[1]);
}
static void config_load_runtime() { ++prov::config_loads; }

// The sketch's layout (domain, a zero byte, the data), over OpenSSL.
static void sha256_domain(const char* domain, const uint8_t* data, size_t n, uint8_t out[32]) {
  EVP_MD_CTX* c = EVP_MD_CTX_new();
  const uint8_t sep = 0x00;
  unsigned int olen = 0;
  EVP_DigestInit_ex(c, EVP_sha256(), nullptr);
  EVP_DigestUpdate(c, domain, std::strlen(domain));
  EVP_DigestUpdate(c, &sep, 1);
  if (data && n > 0) EVP_DigestUpdate(c, data, n);
  EVP_DigestFinal_ex(c, out, &olen);
  EVP_MD_CTX_free(c);
}

// The persist's effect: the {seq, head} blob written, seq_persisted moved.
static void persist_chain_state() {
  ++prov::persists;
  std::vector<uint8_t> blob(chain_state::BLOB_LEN);
  if (!chain_state::encode(g_device.seq, g_device.chain_head, blob.data())) return;
  prov::blobs[NVS_KEY_CHAINST] = blob;
  g_device.seq_persisted = g_device.seq;
}

#include WAP_HMAC_PROV_INC

static const uint32_t kStoredSeq = 1234;
static const uint32_t kStoredBoots = 41;
static const char kStoredToken[] = "cv_StoredTokenStoredTokenStoredToke";  // 35 characters

// A provisioned unit's NVS: the {seq, head} blob (or none, for a chain that
// starts at genesis), the boot count, and a stored API token (or none).
static void stored_unit(bool with_chain, bool with_token, uint8_t (&head)[32]) {
  prov::clear();
  chain_head(head, 0x31);
  if (with_chain) {
    std::vector<uint8_t> blob(chain_state::BLOB_LEN);
    chain_state::encode(kStoredSeq, head, blob.data());
    prov::blobs[NVS_KEY_CHAINST] = blob;
  }
  prov::words[NVS_KEY_BOOTS] = kStoredBoots;
  if (with_token) prov::token = kStoredToken;
}

// One provision_device() as setup() runs it: g_device is a zeroed static.
static bool provision() {
  std::memset(&g_device, 0, sizeof(g_device));
  std::memset(&g_health, 0, sizeof(g_health));
  prov::serial.clear();
  return provision_device();
}

static void provision_device_completes() {
  uint8_t head[32];
  stored_unit(/*with_chain=*/true, /*with_token=*/true, head);
  seam::arm(seam::Step::None);
  CHECK_MSG(provision(), "a working unit: provisioning answered false");
  CHECK_MSG(g_device.initialized && g_health.crypto_healthy, "a working unit: not marked ready");
  CHECK_MSG(g_device.seq == kStoredSeq && g_device.seq_persisted == kStoredSeq,
            "a working unit: seq " + std::to_string(g_device.seq));
  CHECK_MSG(std::memcmp(g_device.chain_head, head, 32) == 0, "a working unit: not the stored head");
  CHECK_MSG(std::string(g_device.api_token_str) == kStoredToken, "a working unit: not the stored token");
  CHECK_MSG(std::string(g_device.ap_password) == "cv-e8Qun72aUeCc",
            std::string("a working unit: the SoftAP key ") + g_device.ap_password);
  CHECK_MSG(g_device.boot_count == kStoredBoots + 1 && prov::words[NVS_KEY_BOOTS] == kStoredBoots + 1,
            "a working unit: the boot not counted");
  CHECK_MSG(prov::config_loads == 1 && prov::persists == 0 && prov::token_stores == 0,
            "a working unit: config, persist or token store");
}

// A refused mint (sweep F359), or a token whose store fails, after the key
// is established: provisioning answers false and leaves g_device.initialized
// false, and setup() goes on (sweep F336), so the boot attestation record
// and loop() sign with this key. The chain they continue must be the stored
// one: seq and head as NVS holds them and nothing persisted over them. With
// the mints ahead of the chain load (F359 as first written), each of these
// left seq 0, a head of 32 zero bytes and seq_persisted 0: the next records
// re-signed seq 1, 2, ... under the stored key, and the persist ten records
// on wrote that branch over the stored {seq, head}.
static void a_refused_mint_keeps_the_stored_chain() {
  struct Case {
    std::string what;
    bool token_stored;     // a unit that minted its token on an earlier boot
    seam::Step fail;       // the failing HMAC's step
    int pass_first;        // HMACs that run before it
    bool store_token_fails;
  };
  std::vector<Case> cases;
  for (seam::Step s : seam::kFailures) {
    cases.push_back({std::string("the SoftAP key, ") + seam::name_of(s), true, s, 0, false});
    for (int which = 0; which < 3; which++) {
      cases.push_back({"the token, HMAC " + std::to_string(which + 1) + " of 3, " + seam::name_of(s),
                       false, s, which, false});
    }
  }
  cases.push_back({"the token's store fails", false, seam::Step::None, 0, true});

  for (const Case& c : cases) {
    for (int genesis = 0; genesis < 2; genesis++) {
      const std::string what = c.what + (genesis ? ", a chain at genesis" : ", a stored chain");
      uint8_t head[32];
      stored_unit(/*with_chain=*/!genesis, c.token_stored, head);
      prov::store_token_fails = c.store_token_fails;
      std::vector<uint8_t> blob_before;
      if (!genesis) blob_before = prov::blobs[NVS_KEY_CHAINST];
      seam::arm(c.fail, c.pass_first);
      const bool ok = provision();
      CHECK_MSG(!ok, what + ": provisioning answered true");
      CHECK_MSG(!g_device.initialized, what + ": the identity marked ready");
      CHECK_MSG(g_device.ap_password[0] == '\0', what + ": a SoftAP key " + g_device.ap_password);
      if (genesis) {
        // The genesis head of this unit's device id, which the boot persists.
        uint8_t expect_head[32];
        sha256_domain("securacv:genesis:v1", reinterpret_cast<const uint8_t*>(g_device.device_id),
                      std::strlen(g_device.device_id), expect_head);
        CHECK_MSG(g_device.seq == 0 && g_device.seq_persisted == 0,
                  what + ": seq " + std::to_string(g_device.seq));
        CHECK_MSG(std::memcmp(g_device.chain_head, expect_head, 32) == 0,
                  what + ": not the genesis head");
        CHECK_MSG(prov::persists == 1, what + ": the genesis head not persisted");
        std::vector<uint8_t> blob(chain_state::BLOB_LEN);
        chain_state::encode(0, expect_head, blob.data());
        CHECK_MSG(prov::blobs[NVS_KEY_CHAINST] == blob, what + ": the persisted blob");
      } else {
        CHECK_MSG(g_device.seq == kStoredSeq && g_device.seq_persisted == kStoredSeq,
                  what + ": seq " + std::to_string(g_device.seq) + ", seq_persisted " +
                      std::to_string(g_device.seq_persisted));
        CHECK_MSG(std::memcmp(g_device.chain_head, head, 32) == 0,
                  what + ": not the stored head" +
                      (all_zero(g_device.chain_head, 32) ? " (32 zero bytes)" : ""));
        CHECK_MSG(prov::persists == 0 && prov::blobs[NVS_KEY_CHAINST] == blob_before,
                  what + ": the stored chain state written");
      }
      CHECK_MSG(g_device.boot_count == kStoredBoots + 1 &&
                    prov::words[NVS_KEY_BOOTS] == kStoredBoots + 1,
                what + ": this boot not counted");
      CHECK_MSG(prov::config_loads == 1, what + ": the runtime settings not loaded");
      if (c.token_stored) {
        CHECK_MSG(std::string(g_device.api_token_str) == kStoredToken,
                  what + ": not the stored token");
      } else if (c.store_token_fails) {
        CHECK_MSG(prov::token.empty(), what + ": a token stored");
      } else {
        CHECK_MSG(prov::token_stores == 0 && prov::token.empty(), what + ": a refused token stored");
        CHECK_MSG(all_zero(g_device.api_token_str, sizeof(g_device.api_token_str)),
                  what + ": the token buffer holds " + g_device.api_token_str);
      }
      CHECK_MSG(seam::g_live == 0, what + ": an HMAC context left allocated");
    }
  }
}

// A key that was not established stops provisioning before anything else
// runs: nothing is named, counted or loaded (setup() halts on a refused key;
// a first boot whose new key could not be stored is F58's pending call).
static void a_key_not_established_stops_first() {
  uint8_t head[32];
  stored_unit(/*with_chain=*/true, /*with_token=*/false, head);
  prov::establish_fails = true;
  seam::arm(seam::Step::None);
  CHECK_MSG(!provision(), "a key not established: provisioning answered true");
  CHECK_MSG(seam::g_lookups == 0 && prov::config_loads == 0 && prov::persists == 0 &&
                prov::words[NVS_KEY_BOOTS] == kStoredBoots && g_device.device_id[0] == '\0',
            "a key not established: provisioning went on past it");
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
  // g_device.initialized false; and after the chain state is loaded, so a
  // refusal leaves the stored chain for the records setup() and loop() go on
  // to sign (a_refused_mint_keeps_the_stored_chain() runs it). Every
  // return false after the key is established comes after the chain load:
  // the only refusal ahead of it is establish_identity_key()'s, which setup()
  // halts on when NVS holds a key (test_wap_first_boot_nvs.cpp).
  const size_t ready = prov.find("g_device.initialized = true;");
  CHECK_MSG(ready != std::string::npos && prov.find("derive_ap_password(") < ready &&
                prov.find("derive_api_token(") < ready,
            "provision_device(): a mint after g_device.initialized = true");
  const std::string est = "if (!establish_identity_key()) return false;";
  const size_t est_at = prov.find(est);
  const size_t loaded = prov.find("g_device.seq_persisted = g_device.seq;");
  CHECK_MSG(est_at != std::string::npos && loaded != std::string::npos && est_at < loaded &&
                prov.find("chain_state::choose(") < loaded,
            "provision_device(): the key, then the chain load");
  for (const char* later : {"derive_api_token(", "nvs_store_token(", "derive_ap_password(",
                            "g_device.initialized = true;"}) {
    const size_t at = prov.find(later);
    CHECK_MSG(at != std::string::npos && loaded < at,
              std::string("provision_device(): ") + later + " ahead of the chain load");
  }
  int refusals = 0;
  for (size_t at = prov.find("return false;"); at != std::string::npos;
       at = prov.find("return false;", at + 1)) {
    ++refusals;
    const bool the_key = at == est_at + est.size() - std::string("return false;").size();
    CHECK_MSG(the_key || at > loaded, "provision_device(): a return false at line " +
                                          std::to_string(line_of(prov, at)) +
                                          " of its body leaves the chain unloaded");
  }
  CHECK_MSG(refusals >= 4, "provision_device(): " + std::to_string(refusals) + " refusal(s)");
  // The buffers the mints fill are the sizes this suite gives them.
  CHECK_MSG(ino.find("char     api_token_str[36];") != std::string::npos &&
                ino.find("char     ap_password[16];") != std::string::npos,
            "canary_wap.ino: DeviceIdentity's api_token_str or ap_password changed size");
}

int main() {
  hmac_known_answer();
  hmac_refuses_each_step();
  ap_password_known_answers();
  ap_password_refuses_a_failed_hmac();
  api_token_known_answer();
  api_token_refuses_any_failed_hmac();
  mints_refuse_a_buffer_they_cannot_fill();
  token_windows_are_never_overrun();
  the_token_formatter_stays_in_its_window();
  resolve_ap_password_release_and_dev();
  resolve_ap_password_after_a_failed_provisioning();
  backup_keeps_the_last_good_one();
  restore_refuses_a_failed_hmac();
  provision_device_completes();
  a_refused_mint_keeps_the_stored_chain();
  a_key_not_established_stops_first();
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
