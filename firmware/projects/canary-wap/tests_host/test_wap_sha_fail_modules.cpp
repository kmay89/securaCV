// canary-wap's modules use no SHA-256 they were refused (sweep F364, the
// modules' slice; the sketch's slice is test_wap_sha_fail.cpp).
//
// Before, five module hashes checked none of mbedTLS's answers. On the S3
// and C3 the SHA engine is hardware and each step can answer an error (an
// update over a long input runs by DMA); after one the output is not, or not
// wholly, written, so it holds a digest of the wrong bytes or whatever the
// caller's stack held. Each one's caller used those bytes:
//   - the sealed vault's ciphertext digest (vault_snapshot.cpp), 4096-byte
//     PSRAM chunks: a wrong ct_hash_hex16 chained as the snapshot's
//     integrity note, and the operator key's key id (a one-shot) stamped
//     into every sealed header;
//   - the USB update drop's image hash (usb_evidence_drive.cpp's
//     dep_sha256, a one-shot over the whole PSRAM-staged image): verify()
//     read an unwritten digest and told the user to copy both files again;
//   - the familiar-device filters' salted index hash (familiar.cpp): bits
//     no fingerprint owns set, tested and persisted;
//   - Wi-Fi presence's MAC hash (wifi_presence.h, a one-shot): four stack
//     bytes counted as a device;
//   - the ble_scan / pairing shim mesh_crypto::sha256_domain(), whose output
//     kept whatever a failed step left.
// Now each helper answers whether every step ran and zeroes its output when
// one did not, and each caller refuses what the hash was for:
//   - seal_task(): a failed start, update (the first, a middle or the last
//     chunk) or finish fails the seal as "sha": no ct_hash_hex16, the tmp
//     file removed and no final file, the frame buffer returned, both PSRAM
//     buffers and the context freed;
//   - sha256_key_id() answers false with a zeroed id; load_config_from()
//     leaves the stored key unadopted (no key, no key id) and logs it, as
//     it does a stored key that did not read back (rather than adopt what
//     its read buffer held), and
//     set_pubkey_hex() refuses before NVS is written, the key registered
//     before it and its id as they were;
//   - dep_sha256() answers mbedTLS's answer, and verify() fails sha_failed,
//     saying the device could not hash the image, before any compare and
//     without the image signature check that reads the digest — even when
//     the refused call left the true digest behind, which before was an
//     image installed on a hash that never ran;
//   - hash_indices() answers false with zeroed indices: note_fingerprint()
//     learns nothing, is_ambient() and is_always_ignored() answer false and
//     count no query, always_ignore() answers false and persists nothing;
//   - hash_mac() answers false with a zeroed hash and process_queue() does
//     not count the capture;
//   - sha256_domain() keeps its void signature (ble_scan.cpp is byte-
//     identical with the canary tree, whose API is void) and zeroes its
//     output after a failed step, as the canary library's IDF 5 body does.
//   - beacon_channel.cpp's sha256_two() answers false with a zeroed digest:
//     no co-sign session key, no fingerprint, and no audit link (the chain
//     and both logs stay as they were, and a head that did not hash at load
//     is re-derived before the next append); a tampered neighbor is still
//     revoked, found by its key.
// mesh_network.cpp's and chirp_channel.cpp's checked hashes run over the
// whole files in test_mesh_address_wap.cpp and test_chirp_security.cpp.
// And the sources: none of the five modules' files, nor
// evidence_update_verify.cpp nor the three channel files, makes a bare call
// of an mbedTLS SHA step, of
// the helpers above or of verify()'s injected hash. That scan is a line
// tripwire, not the proof: it flags a call that starts a statement on its
// line (at the line's start; after `;`, `{` or `}`; as the body of an
// `if (...)`, `while (...)`, `for (...)`, `else` or `do`; or behind a
// `(void)` cast, which [[nodiscard]] lets through), and holds itself to a
// table of such lines, but a statement split across lines can still pass
// it. The behavioral cases above are the proof.
//
// This runs the REAL code: mesh_crypto.cpp whole, evidence_update_verify.cpp
// and securacv_ota.cpp's pure subset linked, and the rest cut verbatim out of
// the sketch's files (firmware/tests_host/cut_functions.awk for functions; a
// line range for each module's state block): vault_snapshot.cpp's state,
// sha256_key_id(), hex_nibble(), fail_result(), seal_task(),
// load_config_from(), set_pubkey_hex(), has_pubkey() and key_id_hex();
// familiar.cpp's state, secure_wipe(), hash_indices(), the bloom helpers,
// persist_ignore(), note_fingerprint(), is_ambient(), always_ignore() and
// is_always_ignored(); wifi_presence.h's state, hash_mac(), mac_seen() and
// process_queue(); usb_evidence_drive.cpp's dep_sha256(). Under them are
// stubs/mesh_net's OpenSSL SHA-256, whose fail knob fails the step and the
// hash a case names, once (a failed start still sets up, a failed update
// feeds nothing, a failed finish or one-shot leaves 0xA5 bytes, nothing, or
// the true digest, as the case chooses), stubs/pair_crypto's real X25519 and
// HKDF, the fake SD card (stubs/sd_fake), and stand-ins below for the
// camera, PSRAM, ChaCha20-Poly1305, NVS, the health log and the queue.
// The helpers' answers are read through answered(), which takes a helper
// that returns nothing as claiming success: such a helper cannot refuse.
// Host-tested; not run on a device.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run-wap-sha-fail-modules

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include <Arduino.h>         // stubs/mesh_net: millis(), esp_fill_random()
#include <mbedtls/sha256.h>  // stubs/mesh_net: OpenSSL's SHA-256 and its fail knob
#include <mbedtls/hkdf.h>    // stubs/pair_crypto: OpenSSL's HKDF
#include <mbedtls/md.h>      // stubs/pair_crypto
#include <Curve25519.h>      // stubs/pair_crypto: OpenSSL's X25519
#include "SD.h"              // stubs/sd_fake

#include "log_level.h"
#include "health_log.h"
#include "vault_logic.h"
#include "familiar.h"
#include "evidence_update_verify.h"
#include "mesh_crypto.h"
#include "beacon_wire.h"
#include "beacon_cosign_aad.h"
#include "json_strict.h"     // the route's answers, read as JSON.parse reads them

#ifndef WAP_SHAMOD_VAULT_STATE_INC
#error "WAP_SHAMOD_VAULT_STATE_INC must name the cut of vault_snapshot.cpp's state (the Makefile passes it)"
#endif
#ifndef WAP_SHAMOD_VAULT_INC
#error "WAP_SHAMOD_VAULT_INC must name the cut of vault_snapshot.cpp's functions (the Makefile passes it)"
#endif
#ifndef WAP_SHAMOD_FAMILIAR_STATE_INC
#error "WAP_SHAMOD_FAMILIAR_STATE_INC must name the cut of familiar.cpp's state (the Makefile passes it)"
#endif
#ifndef WAP_SHAMOD_FAMILIAR_INC
#error "WAP_SHAMOD_FAMILIAR_INC must name the cut of familiar.cpp's functions (the Makefile passes it)"
#endif
#ifndef WAP_SHAMOD_PRESENCE_STATE_INC
#error "WAP_SHAMOD_PRESENCE_STATE_INC must name the cut of wifi_presence.h's state (the Makefile passes it)"
#endif
#ifndef WAP_SHAMOD_PRESENCE_INC
#error "WAP_SHAMOD_PRESENCE_INC must name the cut of wifi_presence.h's functions (the Makefile passes it)"
#endif
#ifndef WAP_SHAMOD_USB_INC
#error "WAP_SHAMOD_USB_INC must name the cut of usb_evidence_drive.cpp's dep_sha256() (the Makefile passes it)"
#endif
#ifndef WAP_SHAMOD_VAULT_KEY_INC
#error "WAP_SHAMOD_VAULT_KEY_INC must name the cut of canary_wap.ino's handle_vault_key_set() (the Makefile passes it)"
#endif
#ifndef WAP_SHAMOD_BEACON_INC
#error "WAP_SHAMOD_BEACON_INC must name the cut of beacon_channel.cpp's functions (the Makefile passes it)"
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

using stub_sha256::Out;
using stub_sha256::Step;

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

// A SHA-256 that goes through none of the stub, for expected values.
static void openssl_sha(const std::string& domain, const uint8_t* data, size_t n, uint8_t out[32]) {
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  unsigned int olen = 0;
  EVP_DigestInit_ex(md, EVP_sha256(), nullptr);
  if (!domain.empty()) EVP_DigestUpdate(md, domain.data(), domain.size());
  if (n > 0) EVP_DigestUpdate(md, data, n);
  EVP_DigestFinal_ex(md, out, &olen);
  EVP_MD_CTX_free(md);
}

// What a helper answered. A helper that returns nothing cannot refuse, so
// it is read as answering true: every "answers false" check below fails on
// a void helper, as it should.
template <typename F, typename... A>
static bool answered(F f, A... a) {
  if constexpr (std::is_void_v<decltype(f(a...))>) {
    f(a...);
    return true;
  } else {
    return f(a...);
  }
}

// A failing step, by name, for the case labels.
struct FailCase {
  Step step;
  int hash;
  int update;
  Out out;
  const char* name;
};

static const char* out_name(Out o) {
  switch (o) {
    case Out::Scribble: return "0xA5 left";
    case Out::Untouched: return "output untouched";
    case Out::Digest: return "true digest left";
  }
  return "?";
}

// ── the health log (health_log.h's two globals) ──────────────────────────────
static std::vector<std::string> g_log;
void health_log(LogLevel level, LogCategory category, const char* message) {
  g_log.push_back(std::to_string((int)level) + "|" + std::to_string((int)category) + "|" +
                  (message ? message : ""));
}
void log_health(LogLevel level, LogCategory category, const char* message, const char* detail) {
  g_log.push_back(std::to_string((int)level) + "|" + std::to_string((int)category) + "|" +
                  (message ? message : "") + "|" + (detail ? detail : ""));
}
static bool log_has(const std::string& needle) {
  for (const std::string& l : g_log) {
    if (l.find(needle) != std::string::npos) return true;
  }
  return false;
}

// ════════════════════════════════════════════════════════════════════════════
// vault_snapshot.cpp's world
// ════════════════════════════════════════════════════════════════════════════

// The camera: one frame the test sets, handed out and taken back.
struct camera_fb_t {
  uint8_t* buf;
  size_t len;
};
static std::vector<uint8_t> g_frame;
static camera_fb_t g_fb;
static int g_fb_out = 0;  // frames handed out and not returned
camera_fb_t* esp_camera_fb_get() {
  g_fb.buf = g_frame.data();
  g_fb.len = g_frame.size();
  ++g_fb_out;
  return &g_fb;
}
void esp_camera_fb_return(camera_fb_t*) { --g_fb_out; }
bool vault_camera_wake_hook(void) { return true; }

// PSRAM: counted, so a case sees every buffer freed.
#define MALLOC_CAP_SPIRAM 1u
static int g_psram_live = 0;
void* heap_caps_malloc(size_t n, uint32_t /*caps*/) {
  void* p = std::malloc(n);
  if (p != nullptr) ++g_psram_live;
  return p;
}
void heap_caps_free(void* p) {
  if (p != nullptr) --g_psram_live;
  std::free(p);
}

// The seal worker's FreeRTOS calls: the host runs seal_task() on the test's
// own thread, so vTaskDelete(NULL) returns and the test reads g_result.
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(uint32_t) {}
void vTaskDelete(void*) {}

void mbedtls_platform_zeroize(void* p, size_t n) {
  volatile uint8_t* b = static_cast<volatile uint8_t*>(p);
  while (n--) *b++ = 0;
}

// rweather's ChaChaPoly as seal_task() uses it — setKey, setIV,
// addAuthData, encrypt chunk by chunk, computeTag, clear — over OpenSSL's
// streaming ChaCha20-Poly1305.
class ChaChaPoly {
 public:
  ~ChaChaPoly() { clear(); }
  bool setKey(const uint8_t* key, size_t len) {
    if (len != 32) return false;
    std::memcpy(key_, key, 32);
    return true;
  }
  bool setIV(const uint8_t* iv, size_t len) {
    if (len != 12) return false;
    clear_ctx();
    ctx_ = EVP_CIPHER_CTX_new();
    return EVP_EncryptInit_ex(ctx_, EVP_chacha20_poly1305(), nullptr, key_, iv) == 1;
  }
  void addAuthData(const void* data, size_t len) {
    int n = 0;
    EVP_EncryptUpdate(ctx_, nullptr, &n, static_cast<const uint8_t*>(data), (int)len);
  }
  void encrypt(uint8_t* out, const uint8_t* in, size_t len) {
    int n = 0;
    EVP_EncryptUpdate(ctx_, out, &n, in, (int)len);
  }
  void computeTag(void* tag, size_t len) {
    uint8_t tail[16];
    int n = 0;
    EVP_EncryptFinal_ex(ctx_, tail, &n);
    EVP_CIPHER_CTX_ctrl(ctx_, EVP_CTRL_AEAD_GET_TAG, (int)len, tag);
  }
  void clear() {
    clear_ctx();
    std::memset(key_, 0, sizeof key_);
  }

 private:
  void clear_ctx() {
    if (ctx_ != nullptr) EVP_CIPHER_CTX_free(ctx_);
    ctx_ = nullptr;
  }
  EVP_CIPHER_CTX* ctx_ = nullptr;
  uint8_t key_[32] = {};
};

// NVS as the vault reads and writes it through NvsMainSession (nvs_store.h).
struct FakePrefs {
  std::map<std::string, std::vector<uint8_t>> blobs;
  std::map<std::string, bool> bools;
  std::map<std::string, uint32_t> uints;
  int writes = 0;
  // A blob whose length reads back but whose data does not: getBytes()
  // answers 0 and writes nothing, as Preferences::getBytes() does when
  // nvs_get_blob() fails after getBytesLength() answered.
  bool get_fails = false;
  bool getBool(const char* k, bool def) const {
    auto it = bools.find(k);
    return it == bools.end() ? def : it->second;
  }
  uint32_t getUInt(const char* k, uint32_t def) const {
    auto it = uints.find(k);
    return it == uints.end() ? def : it->second;
  }
  size_t getBytesLength(const char* k) const {
    auto it = blobs.find(k);
    return it == blobs.end() ? 0 : it->second.size();
  }
  size_t getBytes(const char* k, void* buf, size_t len) const {
    auto it = blobs.find(k);
    if (get_fails || it == blobs.end() || it->second.size() > len) return 0;
    std::memcpy(buf, it->second.data(), it->second.size());
    return it->second.size();
  }
  // A put NVS refuses (sweep F426): Preferences::putBytes() answers 0 and
  // the stored blob stays as it was. Counted apart from the writes.
  bool put_fails = false;
  int refused_puts = 0;
  size_t putBytes(const char* k, const void* buf, size_t len) {
    if (put_fails) {
      ++refused_puts;
      return 0;
    }
    ++writes;
    blobs[k].assign(static_cast<const uint8_t*>(buf), static_cast<const uint8_t*>(buf) + len);
    return len;
  }
  size_t putBool(const char* k, bool v) {
    ++writes;
    bools[k] = v;
    return 1;
  }
  size_t putUInt(const char* k, uint32_t v) {
    ++writes;
    uints[k] = v;
    return 4;
  }
  bool remove(const char* k) {
    ++writes;
    return blobs.erase(k) + bools.erase(k) + uints.erase(k) > 0;
  }
};
static FakePrefs g_nvs;
// A session that does not open (sweep F426): nvs_open() failed, as
// NvsManager::begin() answers it.
static bool g_nvs_open_fails = false;
class NvsMainSession {
 public:
  explicit NvsMainSession(bool /*read_only*/) {}
  bool isOpen() const { return !g_nvs_open_fails; }
  FakePrefs* operator->() { return &g_nvs; }
};

#include "vault_snapshot.h"   // KeySet, set_pubkey_hex()'s answer (sweep F426)

namespace vault_snapshot {
#include WAP_SHAMOD_VAULT_STATE_INC
#include WAP_SHAMOD_VAULT_INC
}  // namespace vault_snapshot

// POST /api/vault/key's handler, handle_vault_key_set(), cut verbatim out of
// canary_wap.ino (sweep F426), over the vault functions above and
// stand-ins for what it names: esp_http_server's one body read, the
// sketch's http_send_json() (the bytes it sends), g_health's request count
// and ArduinoJson (arduinojson_tree.h, which writes as ArduinoJson does).
#ifndef ESP_OK
typedef int esp_err_t;
#define ESP_OK 0
#endif
struct httpd_req_t {
  std::string body;   // what the client sent
  std::string resp;   // what the handler answered
  int sends = 0;
};
static int httpd_req_recv(httpd_req_t* r, char* buf, size_t len) {
  const size_t n = std::min(len, r->body.size());
  std::memcpy(buf, r->body.data(), n);
  return (int)n;
}
static esp_err_t http_send_json(httpd_req_t* req, const char* json) {
  req->resp = json ? json : "";
  ++req->sends;
  return ESP_OK;
}
static struct {
  uint32_t http_requests;
} g_health;
#include "arduinojson_tree.h"
#include WAP_SHAMOD_VAULT_KEY_INC

// ════════════════════════════════════════════════════════════════════════════
// familiar.cpp's world
// ════════════════════════════════════════════════════════════════════════════

namespace nvs_store {
static int g_blob_writes = 0;
inline bool set_blob(const char* /*key*/, const void* /*buf*/, size_t /*len*/) {
  ++g_blob_writes;
  return true;
}
}  // namespace nvs_store

namespace familiar {
#include WAP_SHAMOD_FAMILIAR_STATE_INC
#include WAP_SHAMOD_FAMILIAR_INC
}  // namespace familiar

// ════════════════════════════════════════════════════════════════════════════
// wifi_presence.h's world
// ════════════════════════════════════════════════════════════════════════════

// FreeRTOS's critical section and queue, as process_queue() uses them: the
// queue holds the captures a case pushes, as bytes.
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define pdTRUE 1
typedef int BaseType_t;
typedef std::deque<std::vector<uint8_t>>* QueueHandle_t;
BaseType_t xQueueReceive(QueueHandle_t q, void* item, uint32_t /*ticks*/) {
  if (q == nullptr || q->empty()) return 0;
  std::memcpy(item, q->front().data(), q->front().size());
  q->pop_front();
  return pdTRUE;
}

namespace wifi_presence {
#include WAP_SHAMOD_PRESENCE_STATE_INC
#include WAP_SHAMOD_PRESENCE_INC
}  // namespace wifi_presence

// ════════════════════════════════════════════════════════════════════════════
// usb_evidence_drive.cpp's dep_sha256()
// ════════════════════════════════════════════════════════════════════════════

namespace usb_evidence_drive {
#include WAP_SHAMOD_USB_INC
}  // namespace usb_evidence_drive

// ════════════════════════════════════════════════════════════════════════════
// The stub itself: unarmed it fails nothing, armed it fails what it names
// ════════════════════════════════════════════════════════════════════════════

static void test_stub_knob() {
  const uint8_t data[5] = {'a', 'b', 'c', 'd', 'e'};
  uint8_t want[32];
  openssl_sha("", data, sizeof data, want);

  stub_sha256::disarm();
  uint8_t out[32] = {0};
  CHECK(mbedtls_sha256(data, sizeof data, out, 0) == 0);
  CHECK(std::memcmp(out, want, 32) == 0);
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);
  CHECK(mbedtls_sha256_starts(&c, 0) == 0);
  CHECK(mbedtls_sha256_update(&c, data, 2) == 0);
  CHECK(mbedtls_sha256_update(&c, data + 2, 3) == 0);
  std::memset(out, 0, 32);
  CHECK(mbedtls_sha256_finish(&c, out) == 0);
  mbedtls_sha256_free(&c);
  CHECK(std::memcmp(out, want, 32) == 0);
  CHECK_MSG(stub_sha256::hashes == 0 && stub_sha256::live == 0 && !stub_sha256::fired,
            "the unarmed knob counts nothing");

  // Armed at hash #1's second update: hash #0 runs whole, hash #1's second
  // update answers the error and feeds nothing, once.
  stub_sha256::arm(Step::Update, 1, 1);
  CHECK(mbedtls_sha256(data, sizeof data, out, 0) == 0);
  mbedtls_sha256_init(&c);
  CHECK(stub_sha256::live == 1);
  CHECK(mbedtls_sha256_starts(&c, 0) == 0);
  CHECK(mbedtls_sha256_update(&c, data, 2) == 0);
  CHECK(mbedtls_sha256_update(&c, data + 2, 3) == stub_sha256::kFailed);
  CHECK(stub_sha256::fired);
  CHECK(mbedtls_sha256_finish(&c, out) == 0);  // the step after it runs, over less
  mbedtls_sha256_free(&c);
  uint8_t want_ab[32];
  openssl_sha("", data, 2, want_ab);
  CHECK(std::memcmp(out, want_ab, 32) == 0);
  CHECK(stub_sha256::live == 0);
  CHECK(stub_sha256::hashes == 2);
  stub_sha256::disarm();
}

// ════════════════════════════════════════════════════════════════════════════
// vault_snapshot.cpp
// ════════════════════════════════════════════════════════════════════════════

static const uint8_t kOperatorPub[32] = {
    0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54, 0x74, 0x8b, 0x7d,
    0xdc, 0xb4, 0x3e, 0xf7, 0x5a, 0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38,
    0x1a, 0xf4, 0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a};  // RFC 7748 §6.1, Alice's
static const uint8_t kOtherPub[32] = {
    0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4, 0xd3, 0x5b, 0x61,
    0xc2, 0xec, 0xe4, 0x35, 0x37, 0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78,
    0x67, 0x4d, 0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f};  // RFC 7748 §6.1, Bob's

// Four chunks: 4096, 4096, 4096 and 1234 bytes (updates #0 to #3).
static const size_t kFrameLen = 3 * 4096 + 1234;
static const char* kTmp = "/VAULT/seal_tmp.bin";

static void vault_reset() {
  using namespace vault_snapshot;
  stub_sha256::disarm();
  g_nvs = FakePrefs();
  g_nvs_open_fails = false;
  g_log.clear();
  SD.files.clear();
  SD.dirs.clear();
  SD.present = true;
  SD.writes = 0;
  std::memset(g_pubkey, 0, sizeof g_pubkey);
  std::memset(g_key_id, 0, sizeof g_key_id);
  g_has_pubkey = false;
  g_cfg = VaultConfig{false, false, false, false, false, vault_logic::DEFAULT_COOLDOWN_S};
  std::memset(g_last_capture_ms, 0, sizeof g_last_capture_ms);
  std::memset(g_has_last, 0, sizeof g_has_last);
  g_worker_busy = false;
  g_seal_done = false;
  std::memset(&g_result, 0, sizeof g_result);
  (void)NVS_KEY_SEQ;
}

static std::string key_id_of(const uint8_t pub[32]) {
  uint8_t d[32];
  openssl_sha("", pub, 32, d);
  return hex(d, vault_logic::KEY_ID_SIZE);
}

// One seal of a four-chunk frame for kOperatorPub, as request_capture()
// hands it to the worker.
static void vault_run_seal() {
  using namespace vault_snapshot;
  g_frame.resize(kFrameLen);
  for (size_t i = 0; i < kFrameLen; i++) g_frame[i] = (uint8_t)(i * 7 + 3);
  g_job.trigger = Trigger::T3_SMOKE;
  g_job.time_bucket = 42;
  g_job.seq = 7;
  std::memcpy(g_job.pubkey, kOperatorPub, 32);
  std::memset(g_job.key_id, 0x11, sizeof g_job.key_id);
  g_fb_out = 0;
  g_psram_live = 0;
  seal_task(nullptr);
}

static std::string vault_final_path() {
  char name[32];
  vault_logic::filename_build(7, vault_logic::Trigger::T3_SMOKE, name, sizeof name);
  return std::string("/VAULT/") + name;
}

static void test_vault_seal_ok() {
  using namespace vault_snapshot;
  vault_reset();
  stub_sha256::arm();  // count, fail nothing
  vault_run_seal();
  CHECK(g_seal_done);
  CHECK_MSG(g_result.ok, g_result.fail_reason);
  CHECK(SD.files.count(kTmp) == 0);
  const std::string path = vault_final_path();
  CHECK(SD.files.count(path) == 1);
  if (SD.files.count(path) == 1) {
    const std::string& f = SD.files[path];
    CHECK(f.size() == vault_logic::HEADER_SIZE + kFrameLen + vault_logic::TAG_SIZE);
    uint8_t d[32];
    openssl_sha("", reinterpret_cast<const uint8_t*>(f.data()) + vault_logic::HEADER_SIZE,
                kFrameLen, d);
    CHECK_MSG(std::string(g_result.ct_hash_hex16) == hex(d, 8),
              "ct_hash_hex16 is SHA-256(ciphertext)'s first 8 bytes");
  }
  CHECK(g_result.ct_len == kFrameLen);
  CHECK(stub_sha256::hashes == 1 && stub_sha256::live == 0);
  CHECK(g_psram_live == 0 && g_fb_out == 0);

  // The frame takes exactly four updates: #3 is reached, #4 is not.
  vault_reset();
  stub_sha256::arm(Step::Update, 0, 4);
  vault_run_seal();
  CHECK_MSG(g_result.ok && !stub_sha256::fired, "a four-chunk frame has no fifth update");
}

static void test_vault_seal_refused() {
  using namespace vault_snapshot;
  const FailCase cases[] = {
      {Step::Starts, 0, 0, Out::Scribble, "start"},
      {Step::Update, 0, 0, Out::Scribble, "update at the first chunk"},
      {Step::Update, 0, 1, Out::Scribble, "update at a middle chunk (2nd)"},
      {Step::Update, 0, 2, Out::Scribble, "update at a middle chunk (3rd)"},
      {Step::Update, 0, 3, Out::Scribble, "update at the last chunk"},
      {Step::Finish, 0, 0, Out::Scribble, "finish"},
      {Step::Finish, 0, 0, Out::Untouched, "finish"},
      {Step::Finish, 0, 0, Out::Digest, "finish"},
  };
  for (const FailCase& fc : cases) {
    const std::string label = std::string("vault seal, ") + fc.name + ", " + out_name(fc.out);
    vault_reset();
    stub_sha256::arm(fc.step, fc.hash, fc.update, fc.out);
    vault_run_seal();
    CHECK_MSG(stub_sha256::fired, label + ": the armed step was reached");
    CHECK_MSG(g_seal_done, label);
    CHECK_MSG(!g_result.ok, label + ": the seal failed");
    CHECK_MSG(std::string(g_result.fail_reason) == "sha",
              label + ": fail_reason \"" + g_result.fail_reason + "\"");
    CHECK_MSG(all_zero(g_result.ct_hash_hex16, sizeof g_result.ct_hash_hex16),
              label + ": no ct_hash_hex16");
    CHECK_MSG(g_result.ct_len == 0, label + ": no ct_len");
    CHECK_MSG(SD.files.count(kTmp) == 0, label + ": the tmp file was removed");
    CHECK_MSG(SD.files.count(vault_final_path()) == 0, label + ": no sealed file");
    CHECK_MSG(SD.files.empty(), label + ": nothing left on the card");
    CHECK_MSG(stub_sha256::live == 0, label + ": the context was freed");
    CHECK_MSG(g_psram_live == 0, label + ": both PSRAM buffers were freed");
    CHECK_MSG(g_fb_out == 0, label + ": the frame was returned");
  }
}

static void test_vault_key_id() {
  using namespace vault_snapshot;
  // The helper: the key id when the hash runs; false and zeros when not.
  {
    vault_reset();
    uint8_t id[vault_logic::KEY_ID_SIZE];
    stub_sha256::arm();
    CHECK(answered(sha256_key_id, kOperatorPub, id));
    CHECK(hex(id, sizeof id) == key_id_of(kOperatorPub));
    for (Out o : {Out::Scribble, Out::Untouched, Out::Digest}) {
      std::memset(id, 0x5C, sizeof id);
      stub_sha256::arm(Step::OneShot, 0, 0, o);
      const bool ok = answered(sha256_key_id, kOperatorPub, id);
      CHECK_MSG(stub_sha256::fired, std::string("key id one-shot, ") + out_name(o));
      CHECK_MSG(!ok, std::string("sha256_key_id answers false, ") + out_name(o));
      CHECK_MSG(all_zero(id, sizeof id), std::string("a refused key id is zeros, ") + out_name(o));
    }
  }

  // The boot load: a stored key whose id did not compute is not adopted.
  {
    vault_reset();
    g_nvs.blobs["vault_pub"].assign(kOperatorPub, kOperatorPub + 32);
    stub_sha256::arm();
    NvsMainSession nvs(true);
    load_config_from(nvs);
    CHECK(has_pubkey());
    char kid[17] = {0};
    key_id_hex(kid);
    CHECK(std::string(kid) == key_id_of(kOperatorPub));
  }
  for (Out o : {Out::Scribble, Out::Untouched, Out::Digest}) {
    const std::string label = std::string("vault key load, one-shot fails, ") + out_name(o);
    vault_reset();
    g_nvs.blobs["vault_pub"].assign(kOperatorPub, kOperatorPub + 32);
    stub_sha256::arm(Step::OneShot, 0, 0, o);
    NvsMainSession nvs(true);
    load_config_from(nvs);
    CHECK_MSG(stub_sha256::fired, label);
    CHECK_MSG(!has_pubkey(), label + ": the key is not adopted");
    char kid[17] = {'x', 0};
    key_id_hex(kid);
    CHECK_MSG(kid[0] == '\0', label + ": no key id is shown");
    CHECK_MSG(all_zero(g_key_id, sizeof g_key_id) && all_zero(g_pubkey, sizeof g_pubkey),
              label + ": neither the key nor an id is held");
    CHECK_MSG(g_nvs.blobs.count("vault_pub") == 1 && g_nvs.writes == 0,
              label + ": the stored key is left as it is");
    CHECK_MSG(log_has("Vault unlock key not loaded|key id hash failed"),
              label + ": the refusal is logged");
  }
  // A stored key whose bytes did not read back is not adopted either: the
  // vault must not take whatever its read buffer held as the operator key
  // (stack bytes would seal evidence to a key nobody holds), and nothing is
  // hashed from it.
  {
    const std::string label = "vault key load, the key does not read back";
    vault_reset();
    g_nvs.blobs["vault_pub"].assign(kOperatorPub, kOperatorPub + 32);
    g_nvs.get_fails = true;
    stub_sha256::arm();  // count, fail nothing
    NvsMainSession nvs(true);
    load_config_from(nvs);
    CHECK_MSG(!has_pubkey(), label + ": the key is not adopted");
    char kid[17] = {'x', 0};
    key_id_hex(kid);
    CHECK_MSG(kid[0] == '\0', label + ": no key id is shown");
    CHECK_MSG(all_zero(g_key_id, sizeof g_key_id) && all_zero(g_pubkey, sizeof g_pubkey),
              label + ": neither a key nor an id is held");
    CHECK_MSG(stub_sha256::hashes == 0, label + ": nothing was hashed from it");
    CHECK_MSG(g_nvs.blobs.count("vault_pub") == 1 && g_nvs.writes == 0,
              label + ": the stored key is left as it is");
    CHECK_MSG(log_has("Vault unlock key not loaded|key read failed"),
              label + ": the refusal is logged");
  }

  // Registering: a key whose id did not compute is refused before NVS, and
  // the key registered before it keeps its id.
  {
    vault_reset();
    stub_sha256::arm();
    CHECK(set_pubkey_hex(hex(kOtherPub, 32).c_str()) == KeySet::Stored);
    CHECK(has_pubkey());
  }
  for (Out o : {Out::Scribble, Out::Untouched, Out::Digest}) {
    const std::string label = std::string("vault key register, one-shot fails, ") + out_name(o);
    vault_reset();
    stub_sha256::arm();
    CHECK(set_pubkey_hex(hex(kOtherPub, 32).c_str()) == KeySet::Stored);
    const int writes_before = g_nvs.writes;
    const std::vector<uint8_t> stored_before = g_nvs.blobs["vault_pub"];
    stub_sha256::arm(Step::OneShot, 0, 0, o);
    const KeySet set = set_pubkey_hex(hex(kOperatorPub, 32).c_str());
    CHECK_MSG(stub_sha256::fired, label);
    CHECK_MSG(set == KeySet::NotSaved, label + ": set_pubkey_hex answers NotSaved (the device's refusal)");
    CHECK_MSG(g_nvs.writes == writes_before && g_nvs.blobs["vault_pub"] == stored_before,
              label + ": nothing was written to NVS");
    CHECK_MSG(has_pubkey() && std::memcmp(g_pubkey, kOtherPub, 32) == 0,
              label + ": the key registered before is kept");
    char kid[17] = {0};
    key_id_hex(kid);
    CHECK_MSG(std::string(kid) == key_id_of(kOtherPub), label + ": with its own key id");
    CHECK_MSG(log_has("Vault unlock key refused"), label + ": the refusal is logged");
  }
}

// ════════════════════════════════════════════════════════════════════════════
// POST /api/vault/key says which refusal it was (sweep F426)
// ════════════════════════════════════════════════════════════════════════════
//
// set_pubkey_hex() answered one false for a key that is not 64 hex
// characters and for a well-formed key the device could not keep (its key
// id did not compute, NVS did not open, or NVS refused the write), and the
// route answered every false with the shape text, so an owner who pasted
// the right key read that it was the wrong shape. Now the route answers the
// device's refusal in its own words, saying nothing changed, and the key
// registered before stays the one, in RAM and NVS. The page shows the
// answer's error as it is (web_ui.h's registerVaultKey(), unchanged).

static const char* const kShapeText =
    "pubkey must be the 64-hex X25519 public key printed by unseal_snapshot.py gen-key";
static const char* const kNotSavedText =
    "The device could not save this key (its key id did not compute, or storage refused the "
    "write). Nothing changed. Try again, and check the health log if it repeats.";

// POST /api/vault/key with `body`; its answer, parsed under JSON.parse's rules.
static json_strict::Value post_vault_key(const std::string& body, const std::string& label) {
  httpd_req_t req;
  req.body = body;
  handle_vault_key_set(&req);
  CHECK_MSG(req.sends == 1, label + ": one answer");
  json_strict::Value v;
  CHECK_MSG(json_strict::parse(req.resp, &v), label + ": the answer parses: " + req.resp);
  return v;
}

static std::string error_of(const json_strict::Value& v) {
  const json_strict::Value* ok = v.get("ok");
  const json_strict::Value* e = v.get("error");
  if (ok == nullptr || ok->kind != json_strict::Value::Bool || ok->b) return "(ok is not false)";
  return e != nullptr && e->kind == json_strict::Value::String ? e->text : "(no error)";
}

static std::string key_body(const std::string& hex64) { return "{\"pubkey\":\"" + hex64 + "\"}"; }

// The key registered before a refused post: kOtherPub, in RAM and NVS.
static void register_other_key() {
  using namespace vault_snapshot;
  vault_reset();
  stub_sha256::arm();
  const json_strict::Value v = post_vault_key(key_body(hex(kOtherPub, 32)), "register the first key");
  const json_strict::Value* ok = v.get("ok");
  const json_strict::Value* kid = v.get("key_id");
  CHECK_MSG(ok != nullptr && ok->kind == json_strict::Value::Bool && ok->b, "the first key is registered");
  CHECK_MSG(kid != nullptr && kid->text == key_id_of(kOtherPub), "with its key id");
  g_log.clear();
}

static void check_other_key_kept(const std::string& label) {
  using namespace vault_snapshot;
  CHECK_MSG(has_pubkey() && std::memcmp(g_pubkey, kOtherPub, 32) == 0,
            label + ": the key registered before is still the one in RAM");
  char kid[17] = {0};
  key_id_hex(kid);
  CHECK_MSG(std::string(kid) == key_id_of(kOtherPub), label + ": with its own key id");
  const std::vector<uint8_t> stored(kOtherPub, kOtherPub + 32);
  CHECK_MSG(g_nvs.blobs.count("vault_pub") == 1 && g_nvs.blobs["vault_pub"] == stored,
            label + ": and in NVS");
}

static void test_vault_key_route() {
  using namespace vault_snapshot;

  // A well-formed key the device could not keep: the key id did not compute
  // (each way the one-shot can leave its output), NVS refused the write, or
  // NVS did not open. Each answers the device's refusal, not the shape text.
  struct DeviceSide {
    const char* name;
    Out out;          // the hash's failure, when this case fails it
    bool hash_fails;
    bool put_fails;
    bool open_fails;
    const char* logged;
  };
  const DeviceSide cases[] = {
      {"the key id hash fails, 0xA5 left", Out::Scribble, true, false, false,
       "Vault unlock key refused|key id hash failed"},
      {"the key id hash fails, output untouched", Out::Untouched, true, false, false,
       "Vault unlock key refused|key id hash failed"},
      {"the key id hash fails, true digest left", Out::Digest, true, false, false,
       "Vault unlock key refused|key id hash failed"},
      {"NVS refuses the write", Out::Scribble, false, true, false,
       "Vault unlock key refused|NVS refused the write"},
      {"NVS does not open", Out::Scribble, false, false, true, "Vault unlock key refused|NVS did not open"},
  };
  for (const DeviceSide& c : cases) {
    const std::string label = std::string("vault key route, ") + c.name;
    register_other_key();
    const int writes_before = g_nvs.writes;
    if (c.hash_fails) stub_sha256::arm(Step::OneShot, 0, 0, c.out);
    else stub_sha256::arm();
    g_nvs.put_fails = c.put_fails;
    g_nvs_open_fails = c.open_fails;
    const json_strict::Value v = post_vault_key(key_body(hex(kOperatorPub, 32)), label);
    g_nvs.put_fails = false;
    g_nvs_open_fails = false;
    if (c.hash_fails) CHECK_MSG(stub_sha256::fired, label + ": the armed hash was reached");
    else CHECK_MSG(stub_sha256::hashes == 1 && !stub_sha256::fired, label + ": the key id computed");
    const std::string err = error_of(v);
    CHECK_MSG(err == kNotSavedText, label + ": answers the device's refusal, not the shape text: " + err);
    CHECK_MSG(err.find("Nothing changed") != std::string::npos, label + ": and says nothing changed");
    CHECK_MSG(v.get("key_id") == nullptr, label + ": no key id is answered");
    CHECK_MSG(g_nvs.writes == writes_before, label + ": nothing was written to NVS");
    if (c.put_fails) CHECK_MSG(g_nvs.refused_puts == 1, label + ": the one put was tried and refused");
    else CHECK_MSG(g_nvs.refused_puts == 0, label + ": no put was tried");
    check_other_key_kept(label);
    CHECK_MSG(log_has(c.logged), label + ": the health log says why (" + c.logged + ")");
  }

  // Not a 64-hex key: the shape text, as before, and nothing is tried.
  const struct {
    const char* name;
    std::string body;
  } shapes[] = {
      {"63 hex characters", key_body(hex(kOperatorPub, 32).substr(0, 63))},
      {"65 hex characters", key_body(hex(kOperatorPub, 32) + "0")},
      {"64 characters, one not hex", key_body(hex(kOperatorPub, 32).substr(0, 63) + "g")},
      {"no pubkey", "{\"key\":\"" + hex(kOperatorPub, 32) + "\"}"},
      {"a pubkey that is not a string", "{\"pubkey\":12}"},
  };
  for (const auto& c : shapes) {
    const std::string label = std::string("vault key route, ") + c.name;
    register_other_key();
    const int writes_before = g_nvs.writes;
    stub_sha256::arm();
    const json_strict::Value v = post_vault_key(c.body, label);
    const std::string err = error_of(v);
    CHECK_MSG(err == kShapeText, label + ": answers the shape text: " + err);
    CHECK_MSG(stub_sha256::hashes == 0, label + ": nothing was hashed");
    CHECK_MSG(g_nvs.writes == writes_before && g_nvs.refused_puts == 0, label + ": nothing was written");
    check_other_key_kept(label);
    CHECK_MSG(!log_has("Vault unlock key refused"), label + ": no device refusal is logged");
  }

  // A key the device keeps: ok, its key id, in RAM and NVS.
  {
    const std::string label = "vault key route, a key the device keeps";
    register_other_key();
    stub_sha256::arm();
    const json_strict::Value v = post_vault_key(key_body(hex(kOperatorPub, 32)), label);
    const json_strict::Value* ok = v.get("ok");
    const json_strict::Value* kid = v.get("key_id");
    CHECK_MSG(ok != nullptr && ok->kind == json_strict::Value::Bool && ok->b, label + ": ok");
    CHECK_MSG(kid != nullptr && kid->text == key_id_of(kOperatorPub), label + ": its key id");
    CHECK_MSG(has_pubkey() && std::memcmp(g_pubkey, kOperatorPub, 32) == 0, label + ": in RAM");
    const std::vector<uint8_t> stored(kOperatorPub, kOperatorPub + 32);
    CHECK_MSG(g_nvs.blobs["vault_pub"] == stored, label + ": in NVS");
    CHECK_MSG(log_has("Vault unlock key registered"), label + ": logged");
  }
  stub_sha256::disarm();
}

// ════════════════════════════════════════════════════════════════════════════
// usb_evidence_drive.cpp's dep_sha256() under evidence_update::verify()
// ════════════════════════════════════════════════════════════════════════════

static int g_ed25519_calls = 0;
static bool dep_ed25519_true(const uint8_t*, const uint8_t*, const uint8_t*, size_t) {
  ++g_ed25519_calls;
  return true;  // the signatures are not what this suite tests
}

static void test_evidence_verify() {
  std::vector<uint8_t> image(150000);
  for (size_t i = 0; i < image.size(); i++) image[i] = (uint8_t)(i * 131 + 7);
  uint8_t sha[32];
  openssl_sha("", image.data(), image.size(), sha);

  securacv_ota_manifest_t m;
  std::memset(&m, 0, sizeof m);
  std::snprintf(m.product, sizeof m.product, "securacv-canary-wap");
  std::snprintf(m.version, sizeof m.version, "2.3.0-wap");
  std::snprintf(m.url, sizeof m.url, "https://example.invalid/wap.bin");
  m.size = (uint32_t)image.size();
  std::snprintf(m.sha256, sizeof m.sha256, "%s", hex(sha, 32).c_str());
  const std::string sig(128, 'a');
  std::snprintf(m.signature, sizeof m.signature, "%s", sig.c_str());
  std::snprintf(m.manifest_signature, sizeof m.manifest_signature, "%s", sig.c_str());

  const uint8_t release_key[32] = {1};
  evidence_update::VerifyDeps deps;
  deps.ed25519_verify = dep_ed25519_true;
  deps.sha256 = usb_evidence_drive::dep_sha256;
  deps.release_pubkey = release_key;
  deps.product = "securacv-canary-wap";
  deps.running_version = "2.2.0-wap";
  deps.nvs_floor = "";

  {
    stub_sha256::arm();
    g_ed25519_calls = 0;
    const evidence_update::Verdict v =
        evidence_update::verify(m, image.data(), (uint32_t)image.size(), deps);
    CHECK_MSG(v.ok && std::strcmp(v.code, "ok") == 0, v.code);
    CHECK(g_ed25519_calls == 2);  // the manifest, then the image over size||sha
    CHECK(stub_sha256::hashes == 1);
    uint8_t out[32];
    stub_sha256::arm();
    CHECK(answered(usb_evidence_drive::dep_sha256, image.data(), image.size(), out));
    CHECK(std::memcmp(out, sha, 32) == 0);
  }
  for (Out o : {Out::Scribble, Out::Untouched, Out::Digest}) {
    const std::string label = std::string("USB update drop, image hash fails, ") + out_name(o);
    uint8_t out[32];
    stub_sha256::arm(Step::OneShot, 0, 0, o);
    CHECK_MSG(!answered(usb_evidence_drive::dep_sha256, image.data(), image.size(), out),
              label + ": dep_sha256 answers false");

    stub_sha256::arm(Step::OneShot, 0, 0, o);
    g_ed25519_calls = 0;
    const evidence_update::Verdict v =
        evidence_update::verify(m, image.data(), (uint32_t)image.size(), deps);
    CHECK_MSG(stub_sha256::fired, label);
    CHECK_MSG(!v.ok, label + ": refused (answered " + v.code + ")");
    CHECK_MSG(std::strcmp(v.code, "sha_failed") == 0, label + ": code " + v.code);
    CHECK_MSG(std::strstr(v.msg, "could not hash the image") != nullptr &&
                  std::strstr(v.msg, "copy both files again") == nullptr,
              label + ": the message blames the hash, not the files: " + v.msg);
    CHECK_MSG(g_ed25519_calls == 1,
              label + ": the image signature over the digest was never checked");
  }
  stub_sha256::disarm();
}

// ════════════════════════════════════════════════════════════════════════════
// familiar.cpp
// ════════════════════════════════════════════════════════════════════════════

static void familiar_reset() {
  using namespace familiar;
  stub_sha256::disarm();
  g_log.clear();
  nvs_store::g_blob_writes = 0;
  s_initialized = true;
  for (size_t i = 0; i < sizeof s_salt; i++) s_salt[i] = (uint8_t)(0x30 + i);
  std::memset(s_today, 0, sizeof s_today);
  std::memset(s_yesterday, 0, sizeof s_yesterday);
  std::memset(s_ignore, 0, sizeof s_ignore);
  s_yesterday_valid = false;
  s_last_rotation_ms = 0;
  s_total_notes = s_total_rotations = 0;
  s_total_ambient_queries = s_total_ambient_matches = 0;
  s_total_always_ignored_queries = s_total_always_ignored_matches = 0;
  (void)NVS_KEY_SALT;
  (void)NVS_KEY_YESTERDAY;
  (void)NVS_KEY_ROT_UP_MS;
}

// The bits hash_indices() should name, computed apart from it.
static std::vector<uint16_t> familiar_expected(uint16_t fp, size_t bit_width, uint8_t k) {
  uint8_t in[35];
  std::memcpy(in, familiar::s_salt, 32);
  in[32] = (uint8_t)(fp & 0xFF);
  in[33] = (uint8_t)(fp >> 8);
  in[34] = 0;
  uint8_t d[32];
  openssl_sha("", in, sizeof in, d);
  std::vector<uint16_t> out;
  for (uint8_t i = 0; i < k; i++) {
    out.push_back((uint16_t)((d[2 * i] | (d[2 * i + 1] << 8)) & (bit_width - 1)));
  }
  return out;
}

static void test_familiar() {
  using namespace familiar;
  const uint16_t fp = 0x2A5;

  // Every step runs: the indices are SHA-256(salt || fp || 0)'s slices.
  familiar_reset();
  stub_sha256::arm();
  {
    uint16_t idx[BLOOM_HASHES];
    CHECK(answered(hash_indices, fp, BLOOM_BITS, BLOOM_HASHES, idx));
    const std::vector<uint16_t> want = familiar_expected(fp, BLOOM_BITS, BLOOM_HASHES);
    CHECK(std::vector<uint16_t>(idx, idx + BLOOM_HASHES) == want);
    note_fingerprint(fp);
    CHECK(s_total_notes == 1);
    for (uint16_t b : want) CHECK((s_today[b >> 3] >> (b & 7)) & 1);
    CHECK(stub_sha256::live == 0);
  }
  // A filter that holds fp, for the queries below.
  uint8_t learned[BLOOM_BYTES];
  std::memcpy(learned, s_today, sizeof learned);
  uint8_t ignored[IGNORE_BYTES];
  CHECK(always_ignore(fp));
  CHECK(nvs_store::g_blob_writes == 1);
  std::memcpy(ignored, s_ignore, sizeof ignored);

  const FailCase cases[] = {
      {Step::Starts, 0, 0, Out::Scribble, "start"},
      {Step::Update, 0, 0, Out::Scribble, "update (its only one)"},
      {Step::Finish, 0, 0, Out::Scribble, "finish"},
      {Step::Finish, 0, 0, Out::Untouched, "finish"},
      {Step::Finish, 0, 0, Out::Digest, "finish"},
  };
  for (const FailCase& fc : cases) {
    const std::string label = std::string("familiar, ") + fc.name + " fails, " + out_name(fc.out);

    familiar_reset();
    uint16_t idx[BLOOM_HASHES];
    std::fill(idx, idx + BLOOM_HASHES, 0x7FF);
    stub_sha256::arm(fc.step, fc.hash, fc.update, fc.out);
    const bool ok = answered(hash_indices, fp, BLOOM_BITS, BLOOM_HASHES, idx);
    CHECK_MSG(stub_sha256::fired, label + ": hash_indices");
    CHECK_MSG(!ok, label + ": hash_indices answers false");
    CHECK_MSG(all_zero(idx, sizeof idx), label + ": its indices are zeros");
    CHECK_MSG(stub_sha256::live == 0, label + ": the context was freed");

    familiar_reset();
    stub_sha256::arm(fc.step, fc.hash, fc.update, fc.out);
    note_fingerprint(fp);
    CHECK_MSG(stub_sha256::fired, label + ": note_fingerprint");
    CHECK_MSG(all_zero(s_today, sizeof s_today), label + ": nothing learned");
    CHECK_MSG(s_total_notes == 0, label + ": no note counted");

    familiar_reset();
    std::memcpy(s_yesterday, learned, sizeof learned);
    s_yesterday_valid = true;
    stub_sha256::arm(fc.step, fc.hash, fc.update, fc.out);
    const bool ambient = is_ambient(fp);
    CHECK_MSG(stub_sha256::fired, label + ": is_ambient");
    CHECK_MSG(!ambient, label + ": not ambient, so nothing is suppressed on its say-so");
    CHECK_MSG(s_total_ambient_queries == 0 && s_total_ambient_matches == 0,
              label + ": no query counted");

    familiar_reset();
    std::memcpy(s_ignore, ignored, sizeof ignored);
    stub_sha256::arm(fc.step, fc.hash, fc.update, fc.out);
    const bool ignored_now = is_always_ignored(fp);
    CHECK_MSG(stub_sha256::fired, label + ": is_always_ignored");
    CHECK_MSG(!ignored_now, label + ": not ignored");
    CHECK_MSG(s_total_always_ignored_queries == 0 && s_total_always_ignored_matches == 0,
              label + ": no query counted");

    familiar_reset();
    stub_sha256::arm(fc.step, fc.hash, fc.update, fc.out);
    const bool added = always_ignore(fp);
    CHECK_MSG(stub_sha256::fired, label + ": always_ignore");
    CHECK_MSG(!added, label + ": always_ignore answers false");
    CHECK_MSG(all_zero(s_ignore, sizeof s_ignore), label + ": nothing added");
    CHECK_MSG(nvs_store::g_blob_writes == 0, label + ": nothing persisted");
    CHECK_MSG(g_log.empty(), label + ": no \"always-ignore added\" line");
  }

  // Not initialized: always_ignore() still answers false and hashes nothing.
  familiar_reset();
  s_initialized = false;
  stub_sha256::arm();
  CHECK(!always_ignore(fp));
  CHECK(stub_sha256::hashes == 0);
  stub_sha256::disarm();
}

// ════════════════════════════════════════════════════════════════════════════
// wifi_presence.h
// ════════════════════════════════════════════════════════════════════════════

static std::deque<std::vector<uint8_t>> g_queue;

static void presence_reset() {
  using namespace wifi_presence;
  stub_sha256::disarm();
  g_queue.clear();
  std::memset(&g_state, 0, sizeof g_state);
  g_state.enabled = true;
  g_state.initialized = true;
  g_state.bucket_start_ms = millis();
  g_probe_queue = &g_queue;
  g_isr_probe_count = 0;
  g_isr_drop_count = 0;
  (void)g_state_mux;
  (void)DEFAULT_ALERT_THRESHOLD;
}

static void presence_push(uint8_t last) {
  wifi_presence::ProbeCapture cap;
  const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, last};
  std::memcpy(cap.mac, mac, 6);
  cap.timestamp_ms = millis();
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&cap);
  g_queue.emplace_back(p, p + sizeof cap);
}

static std::string presence_expected(uint8_t last) {
  uint8_t in[17];
  std::memcpy(in, "scv:wp:", 7);
  const uint32_t salt = wifi_presence::g_state.bucket_start_ms;
  std::memcpy(in + 7, &salt, 4);
  const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, last};
  std::memcpy(in + 11, mac, 6);
  uint8_t d[32];
  openssl_sha("", in, sizeof in, d);
  return hex(d, wifi_presence::HASH_LEN);
}

static void test_presence() {
  using namespace wifi_presence;
  // Every hash runs: three captures of two MACs count two devices, each by
  // its own salted hash.
  presence_reset();
  stub_sha256::arm();
  presence_push(1);
  presence_push(2);
  presence_push(1);
  process_queue();
  CHECK(g_state.count == 2);
  CHECK(hex(g_state.mac_hashes[0], HASH_LEN) == presence_expected(1));
  CHECK(hex(g_state.mac_hashes[1], HASH_LEN) == presence_expected(2));
  CHECK(stub_sha256::hashes == 3);

  for (Out o : {Out::Scribble, Out::Untouched, Out::Digest}) {
    const std::string label = std::string("Wi-Fi presence, MAC hash fails, ") + out_name(o);
    uint8_t out[HASH_LEN] = {9, 9, 9, 9};
    const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 3};
    presence_reset();
    stub_sha256::arm(Step::OneShot, 0, 0, o);
    CHECK_MSG(!answered(hash_mac, mac, (uint32_t)7, out), label + ": hash_mac answers false");
    CHECK_MSG(all_zero(out, sizeof out), label + ": its hash is zeros");

    // Three MACs, the second's hash refused: two devices counted, neither
    // of them the refused one.
    presence_reset();
    presence_push(1);
    presence_push(3);
    presence_push(2);
    stub_sha256::arm(Step::OneShot, 1, 0, o);
    process_queue();
    CHECK_MSG(stub_sha256::fired, label);
    CHECK_MSG(g_state.count == 2, label + ": " + std::to_string(g_state.count) +
                                      " counted, the refused capture among them");
    CHECK_MSG(hex(g_state.mac_hashes[0], HASH_LEN) == presence_expected(1) &&
                  hex(g_state.mac_hashes[1], HASH_LEN) == presence_expected(2),
              label + ": the two hashed MACs, in order");
    CHECK_MSG(g_queue.empty(), label + ": the queue was drained");
  }
  stub_sha256::disarm();
}

// ════════════════════════════════════════════════════════════════════════════
// mesh_crypto.cpp
// ════════════════════════════════════════════════════════════════════════════

static void test_mesh_crypto() {
  const char* domain = "securacv:ble_scan:hashed_id:v1";
  uint8_t data[38];
  for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(0xC0 ^ i);
  uint8_t want[32];
  openssl_sha(domain, data, sizeof data, want);

  stub_sha256::arm();
  uint8_t out[32];
  std::memset(out, 0x5C, sizeof out);
  mesh_crypto::sha256_domain(domain, data, sizeof data, out);
  CHECK(std::memcmp(out, want, 32) == 0);
  uint8_t want_data_only[32];
  openssl_sha("", data, sizeof data, want_data_only);
  mesh_crypto::sha256_domain(nullptr, data, sizeof data, out);
  CHECK(std::memcmp(out, want_data_only, 32) == 0);
  mesh_crypto::sha256_domain("", data, sizeof data, out);
  CHECK(std::memcmp(out, want_data_only, 32) == 0);
  CHECK(stub_sha256::live == 0);

  const FailCase cases[] = {
      {Step::Starts, 0, 0, Out::Scribble, "start"},
      {Step::Update, 0, 0, Out::Scribble, "update at the first (the domain)"},
      {Step::Update, 0, 1, Out::Scribble, "update at the last (the data)"},
      {Step::Finish, 0, 0, Out::Scribble, "finish"},
      {Step::Finish, 0, 0, Out::Untouched, "finish"},
      {Step::Finish, 0, 0, Out::Digest, "finish"},
  };
  for (const FailCase& fc : cases) {
    const std::string label = std::string("mesh_crypto::sha256_domain, ") + fc.name + " fails, " +
                              out_name(fc.out);
    std::memset(out, 0x5C, sizeof out);
    stub_sha256::arm(fc.step, fc.hash, fc.update, fc.out);
    mesh_crypto::sha256_domain(domain, data, sizeof data, out);
    CHECK_MSG(stub_sha256::fired, label);
    CHECK_MSG(all_zero(out, sizeof out), label + ": the digest is zeros, not " + hex(out, 4) + "...");
    CHECK_MSG(stub_sha256::live == 0, label + ": the context was freed");
  }
  // With no domain the data is the only update, so it is the first.
  std::memset(out, 0x5C, sizeof out);
  stub_sha256::arm(Step::Update, 0, 0);
  mesh_crypto::sha256_domain(nullptr, data, sizeof data, out);
  CHECK(stub_sha256::fired && all_zero(out, sizeof out));
  stub_sha256::disarm();
}

// ════════════════════════════════════════════════════════════════════════════
// beacon_channel.cpp
// ════════════════════════════════════════════════════════════════════════════
//
// Its hash helper and what stands on it, cut verbatim: the co-sign session
// key, the fingerprint, the audit chain's head at load and at each append,
// and the tamper auto-revoke. A hash that fails gives no key, no
// fingerprint and no chain link; the revoke still happens.

namespace beacon_world {
using namespace beacon_channel;

struct Prefs {
  std::map<std::string, std::vector<uint8_t>> blobs;
  std::map<std::string, uint32_t> ulongs;
  int puts = 0;
  void begin(const char*, bool) {}
  void end() {}
  uint32_t getULong(const char* k, uint32_t def) { return ulongs.count(k) ? ulongs[k] : def; }
  size_t getBytes(const char* k, void* out, size_t len) {
    if (!blobs.count(k) || blobs[k].size() > len) return 0;
    std::memcpy(out, blobs[k].data(), blobs[k].size());
    return blobs[k].size();
  }
  size_t putBytes(const char* k, const void* in, size_t len) {
    ++puts;
    const uint8_t* b = static_cast<const uint8_t*>(in);
    blobs[k].assign(b, b + len);
    return len;
  }
  size_t putULong(const char* k, uint32_t v) {
    ulongs[k] = v;
    return 4;
  }
};
static Prefs g_prefs;
static const char* NVS_NS = "beacon";
static const char* NVS_AUDIT_COUNT = "audit_cnt";
static const char* NVS_AUDIT_PREFIX = "aud_";
static const char* NVS_AUDIT_HEAD = "aud_head";
static bool flash_encryption_enabled() { return true; }

static uint8_t g_x25519_privkey[32];
static BeaconSetEntry g_beacon_set[MAX_BEACON_SET];
static uint8_t g_beacon_set_count = 0;
static const size_t AUDIT_LOG_MAX = 64;
static BeaconAuditEntry g_audit_log[AUDIT_LOG_MAX];
static size_t g_audit_log_count = 0;
static size_t g_audit_head = 0;
static uint8_t g_audit_chain_head[32];
static bool g_audit_head_known = true;

// The SD log of record: each appended entry with the head it was given.
static std::vector<std::pair<BeaconAuditEntry, std::string>> g_sd;
static bool sd_append_audit_entry(const BeaconAuditEntry* entry, const uint8_t* new_chain_head) {
  g_sd.emplace_back(*entry, hex(new_chain_head, 32));
  return true;
}
static int g_set_persists = 0;
static bool persist_beacon_set() {
  ++g_set_persists;
  return true;
}
static void recompute_trouble_reasons() {}

#include WAP_SHAMOD_BEACON_INC

static void reset() {
  stub_sha256::disarm();
  g_log.clear();
  g_prefs = Prefs();
  g_sd.clear();
  g_set_persists = 0;
  std::memset(g_beacon_set, 0, sizeof g_beacon_set);
  g_beacon_set_count = 0;
  std::memset(g_audit_log, 0, sizeof g_audit_log);
  std::memset(g_audit_chain_head, 0, sizeof g_audit_chain_head);
  g_audit_log_count = g_audit_head = 0;
  g_audit_head_known = true;
}

// The head the chain must hold after `e`: SHA-256(prev || entry).
static std::string head_after(const BeaconAuditEntry& e) {
  uint8_t out[32];
  openssl_sha(std::string(reinterpret_cast<const char*>(e.prev_audit_hash), 32),
              reinterpret_cast<const uint8_t*>(&e), sizeof e, out);
  return hex(out, 32);
}
static BeaconAuditEntry entry_of(uint8_t seed) {
  BeaconAuditEntry e;
  std::memset(&e, 0, sizeof e);
  e.received_at = 1760000000u + seed;
  std::memset(e.sig_originator, seed, sizeof e.sig_originator);
  e.hop_count = seed & 3;
  return e;
}

// Every step a two-part hash can fail at, and what a failed finish leaves.
static const FailCase kTwoPart[] = {
    {Step::Starts, 0, 0, Out::Scribble, "start"},
    {Step::Update, 0, 0, Out::Scribble, "the first update"},
    {Step::Update, 0, 1, Out::Scribble, "the second update"},
    {Step::Finish, 0, 0, Out::Scribble, "finish"},
    {Step::Finish, 0, 0, Out::Untouched, "finish"},
    {Step::Finish, 0, 0, Out::Digest, "finish"},
};
static std::string label_of(const char* what, const FailCase& fc) {
  return std::string(what) + ", " + fc.name + " fails, " + out_name(fc.out);
}

static void test_hash_and_fingerprint() {
  reset();
  uint8_t pub[DEVICE_PUBKEY_SIZE];
  for (size_t i = 0; i < sizeof pub; i++) pub[i] = (uint8_t)(0x30 + i);
  uint8_t want[32];
  openssl_sha("", pub, sizeof pub, want);
  uint8_t fp[DEVICE_FP_SIZE];
  stub_sha256::arm();
  CHECK(answered(compute_fingerprint, pub, fp));
  CHECK(std::memcmp(fp, want, DEVICE_FP_SIZE) == 0);
  CHECK(stub_sha256::live == 0);
  for (const FailCase& fc : kTwoPart) {
    if (fc.step == Step::Update && fc.update == 1) continue;  // one part
    const std::string label = label_of("beacon fingerprint", fc);
    std::memset(fp, 0x5C, sizeof fp);
    stub_sha256::arm(fc.step, fc.hash, fc.update, fc.out);
    CHECK_MSG(!answered(compute_fingerprint, pub, fp), label + ": answers false");
    CHECK_MSG(stub_sha256::fired && stub_sha256::live == 0, label + ": reached, context freed");
    CHECK_MSG(all_zero(fp, sizeof fp), label + ": the fingerprint is zeros");
  }
  uint8_t out[32];
  for (const FailCase& fc : kTwoPart) {
    const std::string label = label_of("beacon sha256_two", fc);
    std::memset(out, 0x5C, sizeof out);
    stub_sha256::arm(fc.step, fc.hash, fc.update, fc.out);
    CHECK_MSG(!answered(sha256_two, pub, (size_t)16, pub + 16, (size_t)16, out), label + ": answers false");
    CHECK_MSG(stub_sha256::fired && stub_sha256::live == 0, label + ": reached, context freed");
    CHECK_MSG(all_zero(out, sizeof out), label + ": the digest is zeros");
  }
  stub_sha256::disarm();
  CHECK(answered(sha256_two, pub, (size_t)16, pub + 16, (size_t)16, out));
  CHECK(std::memcmp(out, want, 32) == 0);
}

static void test_cosign_key() {
  reset();
  uint8_t their_priv[32], their_pub[32], shared[32], want[32], key[32];
  for (size_t i = 0; i < 32; i++) {
    g_x25519_privkey[i] = (uint8_t)(0x11 + i);
    their_priv[i] = (uint8_t)(0x77 - i);
  }
  CHECK(Curve25519::eval(their_pub, their_priv, nullptr));
  CHECK(Curve25519::eval(shared, g_x25519_privkey, their_pub));
  openssl_sha("securacv:beacon:cosign:v0", shared, 32, want);
  stub_sha256::arm();
  CHECK(ecdh_session_key(their_pub, key));
  CHECK(std::memcmp(key, want, 32) == 0);
  CHECK(stub_sha256::live == 0);
  for (const FailCase& fc : kTwoPart) {
    const std::string label = label_of("beacon co-sign key", fc);
    std::memset(key, 0x5C, sizeof key);
    stub_sha256::arm(fc.step, fc.hash, fc.update, fc.out);
    CHECK_MSG(!ecdh_session_key(their_pub, key), label + ": no key");
    CHECK_MSG(stub_sha256::fired && stub_sha256::live == 0, label + ": reached, context freed");
    CHECK_MSG(all_zero(key, sizeof key), label + ": the key is zeros");
  }
  stub_sha256::disarm();
}

static void test_audit_chain() {
  reset();
  // Three entries chain, in RAM, on SD and in NVS.
  std::string head = hex(g_audit_chain_head, 32);
  for (uint8_t i = 1; i <= 3; i++) {
    BeaconAuditEntry e = entry_of(i);
    CHECK(chain_audit_entry(&e));
    CHECK(hex(e.prev_audit_hash, 32) == head);
    head = head_after(e);
    CHECK(hex(g_audit_chain_head, 32) == head);
    CHECK(g_sd.size() == i && g_sd.back().second == head);
  }
  CHECK(g_audit_log_count == 3 && g_audit_head == 3 && g_prefs.puts == 3);

  // A link that does not hash: nothing recorded, the chain as it was.
  for (const FailCase& fc : kTwoPart) {
    const std::string label = label_of("beacon audit append", fc);
    g_log.clear();
    BeaconAuditEntry e = entry_of(9);
    stub_sha256::arm(fc.step, fc.hash, fc.update, fc.out);
    CHECK_MSG(!chain_audit_entry(&e), label + ": answers false");
    CHECK_MSG(stub_sha256::fired && stub_sha256::live == 0, label + ": reached, context freed");
    CHECK_MSG(hex(g_audit_chain_head, 32) == head, label + ": the head is unchanged");
    CHECK_MSG(g_audit_log_count == 3 && g_audit_head == 3, label + ": the ring is unchanged");
    CHECK_MSG(g_sd.size() == 3 && g_prefs.puts == 3, label + ": nothing written to SD or NVS");
    CHECK_MSG(log_has("beacon audit: entry not recorded"), label + ": logged");
  }
  stub_sha256::disarm();
  BeaconAuditEntry fourth = entry_of(4);
  CHECK(chain_audit_entry(&fourth));
  CHECK(hex(fourth.prev_audit_hash, 32) == head);
  head = head_after(fourth);
  CHECK(hex(g_audit_chain_head, 32) == head);

  // A reboot: the head is re-derived from the ring's newest entry.
  const Prefs stored = g_prefs;
  auto reboot = [&stored] {
    std::memset(g_audit_log, 0, sizeof g_audit_log);
    std::memset(g_audit_chain_head, 0, sizeof g_audit_chain_head);
    g_audit_log_count = g_audit_head = 0;
    g_audit_head_known = true;
    g_prefs = stored;
    g_sd.clear();
    g_log.clear();
  };
  reboot();
  CHECK(load_audit_log());
  CHECK(g_audit_head_known && hex(g_audit_chain_head, 32) == head);
  CHECK(g_audit_log_count == 4 && g_audit_head == 4);

  for (const FailCase& fc : kTwoPart) {
    const std::string label = label_of("beacon audit load", fc);
    // The head does not hash at load: it is unknown, not a digest.
    reboot();
    stub_sha256::arm(fc.step, fc.hash, fc.update, fc.out);
    CHECK(load_audit_log());
    CHECK_MSG(stub_sha256::fired && stub_sha256::live == 0, label + ": reached, context freed");
    CHECK_MSG(!g_audit_head_known, label + ": the head is unknown");
    CHECK_MSG(all_zero(g_audit_chain_head, 32), label + ": and zeroed");
    // Nor at the next append: no entry is chained from an unknown head.
    BeaconAuditEntry e = entry_of(5);
    stub_sha256::arm(fc.step, fc.hash, fc.update, fc.out);
    CHECK_MSG(!chain_audit_entry(&e), label + ": an append is refused while it stays unknown");
    CHECK_MSG(stub_sha256::fired && g_sd.empty() && g_audit_log_count == 4, label + ": nothing recorded");
    // Once it hashes, the append chains from the true head.
    stub_sha256::disarm();
    CHECK_MSG(chain_audit_entry(&e), label + ": the next append is taken");
    CHECK_MSG(hex(e.prev_audit_hash, 32) == head, label + ": chained from the stored chain's head");
    CHECK_MSG(hex(g_audit_chain_head, 32) == head_after(e), label + ": the new head");
    CHECK_MSG(g_sd.size() == 1 && g_audit_log_count == 5, label + ": recorded once");
  }
}

static void test_tamper_revoke() {
  reset();
  uint8_t pub[DEVICE_PUBKEY_SIZE], stranger[DEVICE_PUBKEY_SIZE];
  for (size_t i = 0; i < sizeof pub; i++) {
    pub[i] = (uint8_t)(0xA0 + i);
    stranger[i] = (uint8_t)(0x40 + i);
  }
  auto set_up = [&pub] {
    std::memset(g_beacon_set, 0, sizeof g_beacon_set);
    g_beacon_set_count = 2;  // slot 0 a hole (all zeros), slot 1 the neighbor
    std::memcpy(g_beacon_set[1].device_pubkey, pub, sizeof pub);
    uint8_t h[32];
    openssl_sha("", pub, sizeof pub, h);
    std::memcpy(g_beacon_set[1].fingerprint, h, DEVICE_FP_SIZE);
    g_beacon_set[1].trust_level = BCN_TRUST_COSIGNER;
    g_beacon_set[1].valid = true;
    g_set_persists = 0;
  };
  set_up();
  CHECK(on_peer_tampered(pub));
  CHECK(g_beacon_set[1].trust_level == BCN_TRUST_REVOKED && g_set_persists == 1);
  for (const FailCase& fc : kTwoPart) {
    if (fc.step == Step::Update && fc.update == 1) continue;  // one part
    const std::string label = label_of("beacon tamper revoke", fc);
    set_up();
    stub_sha256::arm(fc.step, fc.hash, fc.update, fc.out);
    CHECK_MSG(on_peer_tampered(pub), label + ": the neighbor is revoked all the same");
    CHECK_MSG(stub_sha256::fired && stub_sha256::live == 0, label + ": reached, context freed");
    CHECK_MSG(g_beacon_set[1].trust_level == BCN_TRUST_REVOKED && g_set_persists == 1, label);
    // A key in no entry revokes nothing, and an all-zero key does not find
    // the all-zero hole.
    const uint8_t zero_key[DEVICE_PUBKEY_SIZE] = {};
    const uint8_t* keys[] = {stranger, zero_key};
    for (const uint8_t* key : keys) {
      set_up();
      stub_sha256::arm(fc.step, fc.hash, fc.update, fc.out);
      CHECK_MSG(!on_peer_tampered(key), label + ": a key in no entry revokes nothing");
      CHECK_MSG(g_beacon_set[0].trust_level != BCN_TRUST_REVOKED &&
                    g_beacon_set[1].trust_level == BCN_TRUST_COSIGNER && g_set_persists == 0,
                label + ": no entry changed");
    }
  }
  stub_sha256::disarm();
}
}  // namespace beacon_world

// ════════════════════════════════════════════════════════════════════════════
// The sources: no bare call of a SHA step or of the helpers above
// ════════════════════════════════════════════════════════════════════════════

static std::string read_file(const std::string& path) {
  std::ifstream f(path);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// The identifier that ends at body[end - 1], or "" if none does.
static std::string word_before(const std::string& body, size_t end) {
  size_t k = end;
  while (k > 0 && (body[k - 1] == ' ' || body[k - 1] == '\t')) --k;
  size_t w = k;
  while (w > 0 && (std::isalnum(static_cast<unsigned char>(body[w - 1])) || body[w - 1] == '_')) --w;
  return body.substr(w, k - w);
}

// True when `body` (one line, leading whitespace removed, not a comment
// line) holds `call` as a bare statement whose answer nothing reads: at the
// line's start; after `;`, `{` or `}`; as the body of an `if (...)`,
// `while (...)`, `for (...)`, `else` or `do` on the same line; or behind a
// `(void)` cast. A call whose answer is used follows `if (`, `=`, `return`,
// `!`, a value cast or a type (its definition). An occurrence after a `//`
// on the line is a comment.
static bool bare_call(const std::string& body, const std::string& call) {
  const size_t comment = body.find("//");
  for (size_t at = body.find(call); at != std::string::npos; at = body.find(call, at + 1)) {
    if (comment != std::string::npos && at > comment) break;
    if (at > 0) {
      // `d.sha256(` is the whole name; `x.mbedtls_sha256(` is not a call of it.
      const char prev = body[at - 1];
      if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_' || prev == '.') continue;
    }
    size_t k = at;
    while (k > 0 && (body[k - 1] == ' ' || body[k - 1] == '\t')) --k;
    if (k == 0) return true;
    const char before = body[k - 1];
    if (before == ';' || before == '{' || before == '}') return true;
    if (before == ')') {
      // The '(' that closes: a `(void)` cast, or an if/while/for condition.
      size_t open = k - 1;
      int depth = 0;
      for (size_t j = k; j-- > 0;) {
        if (body[j] == ')') ++depth;
        if (body[j] == '(' && --depth == 0) {
          open = j;
          break;
        }
      }
      if (depth == 0 && open < k - 1) {
        std::string inside = body.substr(open + 1, k - 2 - open);
        inside.erase(std::remove(inside.begin(), inside.end(), ' '), inside.end());
        if (inside == "void") return true;
        const std::string kw = word_before(body, open);
        if (kw == "if" || kw == "while" || kw == "for") return true;
      }
    }
    const std::string kw = word_before(body, k);
    if (kw == "else" || kw == "do") return true;
  }
  return false;
}

static void test_sources() {
  // The tripwire itself, on lines written to pass and to fail it.
  struct ScanCase {
    const char* line;
    const char* call;
    bool bare;
  };
  const ScanCase scan_cases[] = {
      {"mbedtls_sha256_update(&c, p, n);", "mbedtls_sha256_update(", true},
      {"(void)hash_mac(mac, h);", "hash_mac(", true},
      {"(void) mbedtls_sha256_finish(&c, out);", "mbedtls_sha256_finish(", true},
      {"x = 1; mbedtls_sha256_update(&c, p, n);", "mbedtls_sha256_update(", true},
      {"if (ok) { mbedtls_sha256_starts(&c, 0); }", "mbedtls_sha256_starts(", true},
      {"} mbedtls_sha256(in, n, out, 0);", "mbedtls_sha256(", true},
      {"if (ready) mbedtls_sha256_update(&c, p, n);", "mbedtls_sha256_update(", true},
      {"for (size_t i = 0; i < n; i++) hash_mac(m[i], h);", "hash_mac(", true},
      {"else hash_indices(fp, bits, k, idx);", "hash_indices(", true},
      {"rc = (int)mbedtls_sha256(in, n, out, 0);", "mbedtls_sha256(", false},
      {"if (rc == 0 && (n > 0)) rc = mbedtls_sha256_update(&c, p, n);", "mbedtls_sha256_update(",
       false},
      {"if (mbedtls_sha256_update(&c, p, n) != 0) { sha_ok = false; break; }",
       "mbedtls_sha256_update(", false},
      {"int ret = mbedtls_sha256_starts(&ctx, 0);", "mbedtls_sha256_starts(", false},
      {"if (ret == 0) ret = mbedtls_sha256_finish(&ctx, digest);", "mbedtls_sha256_finish(", false},
      {"return mbedtls_sha256(data, len, out, 0) == 0;", "mbedtls_sha256(", false},
      {"if (!d.sha256(image, len, digest)) {", "d.sha256(", false},
      {"[[nodiscard]] static bool hash_mac(const uint8_t mac[6], uint8_t out[4]) {", "hash_mac(",
       false},
      {"if (!hash_mac(mac, h)) continue;  // x; hash_mac(", "hash_mac(", false},
      {"if (!rehash_mac(mac, h)) return; my_hash_mac(m, h);", "hash_mac(", false},
  };
  for (const ScanCase& sc : scan_cases) {
    CHECK_MSG(bare_call(sc.line, sc.call) == sc.bare,
              std::string("the scan reads `") + sc.line + "` as " + (sc.bare ? "not " : "") +
                  "a bare call");
  }

  const char* files[] = {"vault_snapshot.cpp", "usb_evidence_drive.cpp", "familiar.cpp",
                         "wifi_presence.h", "mesh_crypto.cpp", "evidence_update_verify.cpp"};
  const char* calls[] = {"mbedtls_sha256(", "mbedtls_sha256_starts(", "mbedtls_sha256_update(",
                         "mbedtls_sha256_finish(", "sha256_key_id(", "hash_indices(",
                         "hash_mac(", "d.sha256("};
  // The channel files: the SHA steps, and each file's own checked helpers.
  struct FileCalls {
    const char* file;
    std::vector<const char*> calls;
  };
  const FileCalls channel_files[] = {
      {"beacon_channel.cpp",
       {"sha256_two(", "compute_fingerprint(", "audit_head_after(", "rederive_audit_head(",
        "ecdh_session_key("}},
      {"mesh_network.cpp", {"sign_message(", "verify_signature("}},
      {"chirp_channel.cpp", {"session_id_from_pubkey(", "generate_session_identity("}},
  };
  int seen = 0;
  std::vector<std::pair<const char*, std::vector<const char*>>> scans;
  for (const char* name : files) scans.push_back({name, {std::begin(calls), std::end(calls)}});
  for (const FileCalls& fc : channel_files) {
    std::vector<const char*> all(std::begin(calls), std::begin(calls) + 4);  // the four SHA steps
    all.insert(all.end(), fc.calls.begin(), fc.calls.end());
    scans.push_back({fc.file, all});
  }
  for (const auto& scan : scans) {
    const char* name = scan.first;
    const std::vector<const char*>& calls = scan.second;
    const std::string path = std::string(WAP_SKETCH_DIR) + "/" + name;
    const std::string text = read_file(path);
    CHECK_MSG(!text.empty(), path + " is readable");
    size_t line_start = 0;
    int line_no = 0;
    while (line_start < text.size()) {
      size_t end = text.find('\n', line_start);
      if (end == std::string::npos) end = text.size();
      const std::string line = text.substr(line_start, end - line_start);
      ++line_no;
      line_start = end + 1;
      const size_t first = line.find_first_not_of(" \t");
      if (first == std::string::npos) continue;
      const std::string body = line.substr(first);
      if (body.rfind("//", 0) == 0 || body.rfind("*", 0) == 0 || body.rfind("/*", 0) == 0) continue;
      for (const char* c : calls) {
        if (body.find(c) == std::string::npos) continue;
        ++seen;
        CHECK_MSG(!bare_call(body, c),
                  std::string(name) + ":" + std::to_string(line_no) + ": a bare call: " + body);
      }
    }
  }
  CHECK_MSG(seen >= 45, "the scan found the calls (" + std::to_string(seen) + ")");
}

int main() {
  test_stub_knob();
  test_vault_seal_ok();
  test_vault_seal_refused();
  test_vault_key_id();
  test_vault_key_route();
  test_evidence_verify();
  test_familiar();
  test_presence();
  test_mesh_crypto();
  beacon_world::test_hash_and_fingerprint();
  beacon_world::test_cosign_key();
  beacon_world::test_audit_chain();
  beacon_world::test_tamper_revoke();
  test_sources();

  if (g_failures != 0) {
    std::printf("test_wap_sha_fail_modules: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL wap_sha_fail_modules TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
