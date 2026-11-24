/* Host tests for the constant-time byte compare,
 * firmware/common/core/ct_compare.h, and for the canary (PIO) tree's caller
 * of it: the chain backup's HMAC check in datamgmt_restore_chain().
 *
 * THE FAILURE THIS PREVENTS. The chain-backup restore (both trees) and the
 * canary-wap's pairing CONFIRM check compared their secret-dependent bytes
 * with memcmp(), which returns at the first byte that differs: the time a
 * refusal takes says how many leading bytes of a forged HMAC tag or CONFIRM
 * hash were right. ct_compare::equal() reads every byte of both buffers
 * whatever they hold. (The canary-wap's two callers are pinned and driven by
 * its own suites: test_mesh_pair_crypto and test_mesh_liveness_wap for the
 * CONFIRM, test_wap_hmac_fail for its restore_chain().)
 *
 * What it pins:
 *   - equal()'s answers: equal buffers of many lengths and alignments are
 *     equal; every single-bit flip of a 32-byte buffer (all 256), and a
 *     flip at the first, a middle and the last byte of other lengths, is
 *     not; n == 0 is not equal (a check of no bytes proves nothing), nor is
 *     a null buffer; a buffer is equal to itself;
 *   - that it reads each end of both buffers, whatever they hold: in one
 *     layout each buffer's last byte sits at the start of a page with no
 *     access, in the other its first byte sits at the end of one, and a
 *     fault handler records the read and opens the page. A forward compare
 *     that stops at the first difference, or skips the last byte, never
 *     reads the last byte; a backward one that stops at the first
 *     difference it meets (a last-byte difference) never reads the first.
 *     Either way the record stays empty and the check fails. A compare that
 *     exits early but reads both ends first (a chunked memcmp() can) is
 *     refused by the text pin below, not by the trap;
 *   - its text: both buffers and the accumulator are read through volatile,
 *     and the loop holds no branch (no if, ?:, &&, ||, break, return, goto);
 *   - datamgmt_restore_chain() and datamgmt_backup_chain(), cut verbatim
 *     out of securacv_data_mgmt.cpp (cut_functions.awk) with the backup
 *     format's constants, over the canary-wap suites' fake SD card
 *     (stubs/sd_fake) and OpenSSL's HMAC-SHA256: a backup the real writer
 *     wrote restores (seq, head, one persist); one flipped bit in its HMAC
 *     field (first, middle and last byte) or in its payload, a zeroed HMAC
 *     field and another device's key are each refused, with the same
 *     "HMAC mismatch" line as before, and change nothing;
 *   - the source (argv[1], securacv_data_mgmt.cpp): the restore compares
 *     the HMAC with ct_compare::equal() over the whole 32-byte field, after
 *     the failed-HMAC refusal, and calls no memcmp(); the file includes
 *     core/ct_compare.h. Fails on the code before this change (a memcmp()).
 *
 * What this does not prove: anything about timing. Constant time cannot be
 * measured meaningfully on a host, and nothing here times anything; what is
 * shown is that every byte is read and the loop has no branch on the bytes
 * as written. The ESP32 compiles are firmware.yml's; nothing here ran on a
 * bench.
 *
 * Build & run: make -C firmware/tests_host run-ct-compare
 */
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "core/ct_compare.h"
#include "log_level.h"  // the canary (PIO) tree's LogLevel / LogCategory
#include "FS.h"         // canary-wap tests_host/stubs/sd_fake
#include "SD.h"

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond) CHECK_MSG((cond), #cond)
#define CHECK_MSG(cond, what)                                              \
  do {                                                                     \
    g_checks++;                                                            \
    if (!(cond)) {                                                         \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, std::string(what).c_str()); \
      g_failures++;                                                        \
    }                                                                      \
  } while (0)

using ct_compare::equal;

// ── equal()'s answers ───────────────────────────────────────────────────────

static void fill(std::vector<uint8_t>& v, std::mt19937& rng) {
  for (uint8_t& b : v) b = (uint8_t)(rng() & 0xFF);
}

static void equal_buffers_are_equal() {
  std::mt19937 rng(0x5ecc0de);
  for (size_t n : {1u, 2u, 3u, 7u, 8u, 15u, 16u, 31u, 32u, 33u, 63u, 64u, 76u, 257u, 4096u}) {
    for (size_t offset = 0; offset < 8; ++offset) {   // every alignment of a
      std::vector<uint8_t> a(n + 8), b(n + 8);        // 64-bit word
      fill(a, rng);
      std::memcpy(b.data() + offset, a.data() + offset, n);
      CHECK_MSG(equal(a.data() + offset, b.data() + offset, n),
                "equal buffers of " + std::to_string(n) + " bytes at offset " +
                    std::to_string(offset) + " answered unequal");
    }
  }
  uint8_t zeros_a[32] = {0}, zeros_b[32] = {0};
  CHECK_MSG(equal(zeros_a, zeros_b, 32), "two all-zero buffers answered unequal");
  uint8_t self[32];
  for (size_t i = 0; i < 32; ++i) self[i] = (uint8_t)(i * 37 + 1);
  CHECK_MSG(equal(self, self, 32), "a buffer answered unequal to itself");
}

static void every_one_bit_flip_is_unequal() {
  std::mt19937 rng(0xf11b);
  std::vector<uint8_t> a(32);
  fill(a, rng);
  int flips = 0;
  for (size_t byte = 0; byte < 32; ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<uint8_t> b = a;
      b[byte] ^= (uint8_t)(1u << bit);
      ++flips;
      CHECK_MSG(!equal(a.data(), b.data(), 32) && !equal(b.data(), a.data(), 32),
                "32 bytes, bit " + std::to_string(bit) + " of byte " + std::to_string(byte) +
                    " flipped: answered equal");
    }
  }
  CHECK_MSG(flips == 256, "flipped " + std::to_string(flips) + " bits, not 256");

  // The first, a middle and the last byte of other lengths, and a 0x00 /
  // 0xFF difference at each, so a compare that reads only some bytes, or
  // stops one short, is caught at every length a caller uses.
  for (size_t n : {1u, 2u, 16u, 31u, 33u, 64u, 76u}) {
    std::vector<uint8_t> x(n);
    fill(x, rng);
    for (size_t at : {(size_t)0, n / 2, n - 1}) {
      for (uint8_t mask : {(uint8_t)0x01, (uint8_t)0x80, (uint8_t)0xFF}) {
        std::vector<uint8_t> y = x;
        y[at] ^= mask;
        CHECK_MSG(!equal(x.data(), y.data(), n),
                  std::to_string(n) + " bytes, byte " + std::to_string(at) + " ^ " +
                      std::to_string(mask) + ": answered equal");
      }
    }
  }
  // Differences in every byte, and at the first and last together.
  uint8_t lo[32], hi[32];
  std::memset(lo, 0x00, sizeof lo);
  std::memset(hi, 0xFF, sizeof hi);
  CHECK(!equal(lo, hi, 32));
  uint8_t ends[32];
  std::memcpy(ends, lo, sizeof ends);
  ends[0] = 1;
  ends[31] = 1;
  CHECK(!equal(lo, ends, 32));
}

static void zero_length_and_null_are_unequal() {
  uint8_t a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 4};
  CHECK_MSG(!equal(a, b, 0), "n == 0 answered equal: a check of no bytes proves nothing");
  CHECK_MSG(!equal(a, a, 0), "n == 0 of one buffer answered equal");
  CHECK_MSG(!equal(nullptr, nullptr, 0), "two null buffers of n == 0 answered equal");
  CHECK_MSG(!equal(nullptr, b, 4), "a null first buffer answered equal");
  CHECK_MSG(!equal(a, nullptr, 4), "a null second buffer answered equal");
  CHECK_MSG(!equal(nullptr, nullptr, 4), "two null buffers answered equal");
  CHECK_MSG(equal(a, b, 4), "the same four bytes answered unequal");
}

// ── equal() reads both ends of both buffers ─────────────────────────────────
//
// Two layouts. Tail: each buffer's last byte is the first byte of a page
// with no access. Head: each buffer's first byte is the last byte of one.
// The first read of the guarded byte faults; the handler records which
// page, opens it for reading and returns, and the read runs again and
// succeeds. So a compare that reads that byte leaves a record, and one that
// never reads it leaves none: a forward early exit at a first or middle
// difference, or a loop one short, misses the tail; a backward early exit
// at a last or middle difference misses the head. An early exit that reads
// both ends before it stops is the loop-text pin's to refuse.

static long g_page = 0;
static uint8_t* volatile g_guard[2] = {nullptr, nullptr};
static volatile sig_atomic_t g_faulted[2] = {0, 0};
static volatile sig_atomic_t g_stray = 0;

static void on_fault(int, siginfo_t* info, void*) {
  const uint8_t* at = static_cast<const uint8_t*>(info->si_addr);
  for (int k = 0; k < 2; ++k) {
    if (g_guard[k] != nullptr && at >= g_guard[k] && at < g_guard[k] + g_page) {
      g_faulted[k] = 1;
      mprotect(g_guard[k], (size_t)g_page, PROT_READ);
      return;
    }
  }
  g_stray = 1;   // a fault this test did not set up: stop rather than loop
  signal(SIGSEGV, SIG_DFL);
  signal(SIGBUS, SIG_DFL);
}

struct Guarded {
  uint8_t* map = nullptr;     // two pages: the buffer and its guard
  uint8_t* guard = nullptr;   // the page with no access once armed
  uint8_t* buf = nullptr;     // n bytes, one end on the guard page
};

// tail: the buffer's last byte at the second page's start, the second page
// the guard. head: its first byte at the first page's end, the first page
// the guard.
static Guarded guarded(size_t n, int k, bool tail) {
  Guarded g;
  void* m = mmap(nullptr, 2 * (size_t)g_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return g;
  g.map = static_cast<uint8_t*>(m);
  g.guard = tail ? g.map + g_page : g.map;
  g.buf = tail ? g.map + g_page - (n - 1) : g.map + g_page - 1;
  g_guard[k] = g.guard;
  return g;
}

// Open the guard page for writing (the handler left it read-only, or it is
// new), so the case can write the byte on it.
static void open_guard(Guarded& g) { mprotect(g.guard, (size_t)g_page, PROT_READ | PROT_WRITE); }

static void arm(Guarded& g, int k) {
  g_faulted[k] = 0;
  mprotect(g.guard, (size_t)g_page, PROT_NONE);
}

static void every_byte_is_read() {
  g_page = sysconf(_SC_PAGESIZE);
  CHECK_MSG(g_page >= 64, "no page size");
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);   // macOS reports a protected page as SIGBUS

  for (const bool tail : {true, false})
  for (size_t n : {1u, 2u, 16u, 32u, 64u}) {
    const char* end = tail ? "last" : "first";
    Guarded a = guarded(n, 0, tail), b = guarded(n, 1, tail);
    CHECK_MSG(a.map != nullptr && b.map != nullptr, "mmap failed");
    if (a.map == nullptr || b.map == nullptr) return;
    struct Case {
      const char* what;
      size_t at;        // the byte that differs
      bool differ;
    };
    const Case cases[] = {
        {"equal buffers", 0, false},
        {"the first byte differs", 0, true},
        {"a middle byte differs", n / 2, true},
        {"the last byte differs", n - 1, true},
    };
    for (const Case& c : cases) {
      open_guard(a);
      open_guard(b);
      for (size_t i = 0; i < n; ++i) a.buf[i] = b.buf[i] = (uint8_t)(0xA5 ^ i);
      if (c.differ) b.buf[c.at] ^= 0x5A;
      arm(a, 0);
      arm(b, 1);
      const bool answer = equal(a.buf, b.buf, n);
      CHECK_MSG(g_stray == 0, "a fault outside the guard pages");
      CHECK_MSG(answer == !c.differ, std::to_string(n) + " bytes, " + c.what + ": wrong answer");
      CHECK_MSG(g_faulted[0] == 1, std::to_string(n) + " bytes, " + c.what + ": the first buffer's " + end +
                                       " byte was never read");
      CHECK_MSG(g_faulted[1] == 1, std::to_string(n) + " bytes, " + c.what + ": the second buffer's " + end +
                                       " byte was never read");
      if (g_stray) return;
    }
    g_guard[0] = g_guard[1] = nullptr;
    munmap(a.map, 2 * (size_t)g_page);
    munmap(b.map, 2 * (size_t)g_page);
  }
  signal(SIGSEGV, SIG_DFL);
  signal(SIGBUS, SIG_DFL);
}

// ── the canary (PIO) tree's chain backup and restore, cut verbatim ──────────

struct DeviceIdentity {   // the fields the two functions use
  uint8_t privkey[32];
  uint8_t chain_head[32];
  uint32_t seq;
};
static DeviceIdentity g_dev;
static int g_persists = 0;
static std::vector<std::string> g_log;
static bool g_mounted = true;

DeviceIdentity& witness_get_device() { return g_dev; }
void witness_persist_chain_state() { ++g_persists; }
bool storage_is_mounted() { return g_mounted; }
uint32_t millis() { return 4242; }
void log_health(LogLevel, LogCategory, const char* message, const char*) { g_log.push_back(message); }
bool hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len,
                 uint8_t out[32]) {
  unsigned int len = 0;
  return HMAC(EVP_sha256(), key, (int)key_len, data, data_len, out, &len) != nullptr && len == 32;
}

#define FEATURE_SD_STORAGE 1
#define FEATURE_DIAGNOSTICS 0
namespace datamgmt {
static bool s_backup_exists = false;
static uint32_t s_last_backup_ms = 0;
#include CT_DATAMGMT_CONSTS_INC
}  // namespace datamgmt
#include CT_DATAMGMT_INC

static const char* kBackup = "/CHAIN/backup.bin";
static const char* kMismatch = "Chain restore: HMAC mismatch (tampered?)";

static int times_logged(const char* line) {
  int n = 0;
  for (const std::string& s : g_log) n += s == line;
  return n;
}

static void device(uint8_t key_seed, uint32_t seq, uint8_t head_seed) {
  for (size_t i = 0; i < 32; ++i) {
    g_dev.privkey[i] = (uint8_t)(key_seed + i * 3);
    g_dev.chain_head[i] = (uint8_t)(head_seed + i * 7);
  }
  g_dev.seq = seq;
}

static std::string openssl_hmac(const uint8_t key[32], const std::string& payload) {
  uint8_t out[32];
  unsigned int len = 0;
  HMAC(EVP_sha256(), key, 32, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), out, &len);
  return std::string(reinterpret_cast<const char*>(out), len);
}

// Restores the card's backup into a device whose state is all 0xEE, and
// says whether the state was left as it was.
static bool restore_from_card(bool* untouched) {
  std::memset(g_dev.chain_head, 0xEE, sizeof g_dev.chain_head);
  g_dev.seq = 0xEEEEEEEE;
  g_persists = 0;
  g_log.clear();
  const bool ok = datamgmt_restore_chain();
  bool same = g_dev.seq == 0xEEEEEEEE && g_persists == 0;
  for (uint8_t b : g_dev.chain_head) same = same && b == 0xEE;
  *untouched = same;
  return ok;
}

static void flagship_restore_checks_the_whole_hmac() {
  SD.present = true;
  SD.files.clear();
  SD.dirs.clear();
  g_mounted = true;

  device(0x11, 7, 0x40);
  uint8_t head7[32];
  std::memcpy(head7, g_dev.chain_head, 32);
  CHECK_MSG(datamgmt_backup_chain(), "the real writer wrote no backup");
  const std::string good = SD.files[kBackup];
  CHECK_MSG(good.size() == datamgmt::BACKUP_SIZE, "a backup of " + std::to_string(good.size()) + " bytes");
  if (good.size() != datamgmt::BACKUP_SIZE) return;
  CHECK_MSG(good.substr(datamgmt::BACKUP_PAYLOAD) ==
                openssl_hmac(g_dev.privkey, good.substr(0, datamgmt::BACKUP_PAYLOAD)),
            "the backup's HMAC field is not HMAC-SHA256 of its payload under the device key");

  bool untouched = false;
  CHECK_MSG(restore_from_card(&untouched), "the good backup did not restore");
  CHECK_MSG(g_dev.seq == 7 && std::memcmp(g_dev.chain_head, head7, 32) == 0 && g_persists == 1,
            "the good backup restored the wrong state, or did not persist it once");
  CHECK(times_logged("Chain restored from backup") == 1 && times_logged(kMismatch) == 0);

  struct Flip {
    size_t at;
    uint8_t mask;
    const char* what;
  };
  const Flip flips[] = {
      {44, 0x01, "the HMAC's first byte"},   {44, 0x80, "the HMAC's first byte (top bit)"},
      {59, 0x01, "a middle HMAC byte"},      {60, 0x10, "a middle HMAC byte"},
      {75, 0x01, "the HMAC's last byte"},    {75, 0x80, "the HMAC's last byte (top bit)"},
      {8, 0x01, "the payload's seq"},        {12, 0x01, "the payload's first head byte"},
      {43, 0x80, "the payload's last byte"},
  };
  for (const Flip& f : flips) {
    std::string bad = good;
    bad[f.at] = (char)(bad[f.at] ^ f.mask);
    SD.files[kBackup] = bad;
    const bool restored = restore_from_card(&untouched);
    CHECK_MSG(!restored, std::string(f.what) + " flipped: restored");
    CHECK_MSG(untouched, std::string(f.what) + " flipped: the device state moved");
    CHECK_MSG(times_logged(kMismatch) == 1, std::string(f.what) + " flipped: no single mismatch line");
  }

  // A zeroed HMAC field (what a failed HMAC wrote until F359) and a backup
  // another device's key signed are refused the same way.
  std::string zeroed = good;
  for (size_t i = datamgmt::BACKUP_PAYLOAD; i < datamgmt::BACKUP_SIZE; ++i) zeroed[i] = 0;
  SD.files[kBackup] = zeroed;
  CHECK_MSG(!restore_from_card(&untouched) && untouched && times_logged(kMismatch) == 1,
            "a zeroed HMAC field was not refused");
  SD.files[kBackup] = good;
  device(0x22, 0, 0);   // another device's key
  CHECK_MSG(!restore_from_card(&untouched) && untouched && times_logged(kMismatch) == 1,
            "another device's backup was not refused");
}

// ── the source ──────────────────────────────────────────────────────────────

static std::string read_file(const char* path, bool* ok) {
  std::ifstream f(path, std::ios::binary);
  *ok = static_cast<bool>(f);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Comments become spaces (newlines kept); string literals are kept.
static std::string strip_comments(const std::string& s) {
  std::string out = s;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '"' || s[i] == '\'') {
      const char q = s[i];
      size_t j = i + 1;
      while (j < s.size() && s[j] != q && s[j] != '\n') j += (s[j] == '\\') ? 2 : 1;
      i = j + 1;
    } else if (s.compare(i, 2, "//") == 0) {
      while (i < s.size() && s[i] != '\n') out[i++] = ' ';
    } else if (s.compare(i, 2, "/*") == 0) {
      size_t j = s.find("*/", i + 2);
      j = (j == std::string::npos) ? s.size() : j + 2;
      for (; i < j; ++i) {
        if (out[i] != '\n') out[i] = ' ';
      }
    } else {
      ++i;
    }
  }
  return out;
}

static std::string squeeze(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') out += c;
  }
  return out;
}

// The body of the column-0 function whose head line starts with `head`, to
// its first column-0 "}" line.
static std::string body_of(const std::string& text, const std::string& head) {
  const size_t start = text.find("\n" + head);
  if (start == std::string::npos) return "";
  const size_t end = text.find("\n}\n", start + 1);
  if (end == std::string::npos) return "";
  return text.substr(start + 1, end - start + 2);
}

static int count(const std::string& hay, const std::string& needle) {
  int n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
  return n;
}

static void the_helper_has_no_branch_on_the_bytes(const char* header_path) {
  bool ok = false;
  const std::string text = strip_comments(read_file(header_path, &ok));
  CHECK_MSG(ok, std::string("could not read ") + header_path);
  const std::string sq = squeeze(text);
  const std::string head = "[[nodiscard]]inlineboolequal(constvoid*a,constvoid*b,size_tn){";
  const size_t at = sq.find(head);
  CHECK_MSG(at != std::string::npos, "ct_compare.h: no equal(const void*, const void*, size_t)");
  if (at == std::string::npos) return;
  const size_t end = sq.find("returndiff==0;}", at);
  CHECK_MSG(end != std::string::npos, "ct_compare.h: equal() does not end in `return diff == 0;`");
  if (end == std::string::npos) return;
  const std::string body = sq.substr(at + head.size(), end - at - head.size());
  CHECK_MSG(body.find("if(a==nullptr||b==nullptr||n==0)returnfalse;"
                      "constvolatileuint8_t*pa=static_cast<constvolatileuint8_t*>(a);"
                      "constvolatileuint8_t*pb=static_cast<constvolatileuint8_t*>(b);"
                      "volatileuint8_tdiff=0;") == 0,
            "ct_compare.h: equal() no longer reads both buffers and its accumulator through volatile, "
            "after refusing null and n == 0");
  const size_t loop = body.find("for(size_ti=0;i<n;++i){");
  CHECK_MSG(loop != std::string::npos, "ct_compare.h: equal()'s loop is not `for (size_t i = 0; i < n; ++i)`");
  if (loop == std::string::npos) return;
  const std::string inside = body.substr(loop);
  CHECK_MSG(inside == "for(size_ti=0;i<n;++i){diff=static_cast<uint8_t>(diff|(pa[i]^pb[i]));}",
            "ct_compare.h: equal()'s loop is not the one branch-free fold: " + inside);
  for (const char* banned : {"if(", "?", "&&", "||", "break", "return", "goto", "while"}) {
    CHECK_MSG(inside.find(banned) == std::string::npos,
              std::string("ct_compare.h: equal()'s loop holds `") + banned + "`");
  }
}

static void the_flagship_restore_calls_it(const char* path) {
  bool ok = false;
  const std::string raw = read_file(path, &ok);
  CHECK_MSG(ok, std::string("could not read ") + path);
  const std::string code = strip_comments(raw);
  const std::string sq = squeeze(code);
  CHECK_MSG(count(sq, squeeze("#include \"core/ct_compare.h\"")) == 1,
            "securacv_data_mgmt.cpp does not include core/ct_compare.h once");
  const std::string restore = squeeze(body_of(code, "bool datamgmt_restore_chain(void) {"));
  CHECK_MSG(!restore.empty(), "securacv_data_mgmt.cpp: no datamgmt_restore_chain()");
  // Pins are written as the source reads and squeezed like it (squeeze()
  // takes the spaces out of string literals too).
  const std::string refused_hmac = squeeze("if (!hmac_sha256(dev.privkey, 32, buf, BACKUP_PAYLOAD, calc_hmac)) {");
  const std::string ct = squeeze(
      "if (!ct_compare::equal(buf + BACKUP_PAYLOAD, calc_hmac, sizeof(calc_hmac))) {"
      "  log_health(LOG_LEVEL_ERROR, LOG_CAT_STORAGE, \"Chain restore: HMAC mismatch (tampered?)\", nullptr);"
      "  return false;"
      "}");
  CHECK_MSG(count(restore, ct) == 1,
            "datamgmt_restore_chain() does not compare the HMAC with ct_compare::equal() over the "
            "whole field, refusing with the mismatch line");
  CHECK_MSG(count(restore, "memcmp(") == 0, "datamgmt_restore_chain() calls memcmp()");
  CHECK_MSG(count(restore, squeeze("uint8_t calc_hmac[32];")) == 1,
            "datamgmt_restore_chain(): calc_hmac is not 32 bytes");
  CHECK_MSG(restore.find(refused_hmac) != std::string::npos &&
                restore.find(refused_hmac) < restore.find(ct),
            "datamgmt_restore_chain(): the HMAC is compared before a failed HMAC is refused");
  CHECK_MSG(count(sq, squeeze("static_assert(BACKUP_PAYLOAD + sizeof(calc_hmac) == BACKUP_SIZE,")) == 1,
            "datamgmt_restore_chain(): the HMAC field's extent is not asserted");
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::printf("usage: %s <securacv_data_mgmt.cpp> <ct_compare.h>\n", argv[0]);
    return 2;
  }
  equal_buffers_are_equal();
  every_one_bit_flip_is_unequal();
  zero_length_and_null_are_unequal();
  every_byte_is_read();
  flagship_restore_checks_the_whole_hmac();
  the_helper_has_no_branch_on_the_bytes(argv[2]);
  the_flagship_restore_calls_it(argv[1]);
  if (g_failures) {
    std::printf("test_ct_compare: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("test_ct_compare: ALL %d CHECKS PASSED (equal()'s answers, both ends of both buffers "
              "read, no branch in its loop, the canary (PIO) chain restore over the fake card, its "
              "call site)\n",
              g_checks);
  return 0;
}
