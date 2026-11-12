// The two mesh_crypto::sha256_domain() bodies, on both IDF lines (sweep F364).
//
// The canary library's (firmware/canary/lib/securacv_mesh/src/mesh_crypto.cpp)
// and this sketch's shim (mesh_crypto.cpp beside the sketch) are one
// function in two copies, and neither check_mesh_sync.sh nor
// check_ble_scan_sync.sh covers them. Both used to be void: the library's
// IDF 4 branch checked no step and its IDF 5 branch and the shim zeroed the
// digest without saying so, so every caller took 32 zero bytes for a hash.
// Now each answers whether every mbedTLS step ran, zeroes its output when
// one did not, runs no step after a failed one and frees the context on
// every path; and the library's IDF 4 branch asks the step through the
// names mbedTLS 2.28 answers on (the _ret ones; its plain names return
// void), while its IDF 5 branch and the shim use mbedTLS 3.x's plain names.
//
// This compiles the library's body as the device builds compile it:
// sha256_domain() and secure_zero() cut verbatim out of the library's file
// (firmware/tests_host/cut_functions.awk), CSI_TEST_HOST_BUILD unset, so the
// device branches are the ones built, over firmware/tests_host/stubs/
// sha_fail's mbedTLS stand-in (a real SHA-256 over OpenSSL that fails the
// step a case names, once). The Makefile builds it twice: as IDF 4 over the
// stand-in's 2.28 spelling (ESP_IDF_VERSION_MAJOR=4), where a plain name
// returns void and a call that read its answer would not compile, and as
// IDF 5 over its 3.6 spelling, where the _ret names do not exist. The IDF 5
// build also compiles the sketch's shim against the same stand-in and runs
// the same cases through it, and holds the two copies' answers and outputs
// equal. What it pins, for a failing start, each update and the finish:
// false, 32 zero bytes, no step after the failed one, the context freed;
// the known answer when every step runs; a null output refused.
//
// What this does not prove: the hardware engine, IDF's port, or anything on
// a device. The ESP32 compiles of both lines (the core 2.0.17 legs and the
// core 3 legs) are firmware.yml's; nothing here has run on a bench.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run-mesh-crypto-lines

#include <openssl/evp.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <mbedtls/sha256.h>  // firmware/tests_host/stubs/sha_fail, in the spelling the build names

#ifndef MCLINES_PIO_INC
#error "MCLINES_PIO_INC must name the cut of the canary library's mesh_crypto.cpp (the Makefile passes it)"
#endif
#ifndef ESP_IDF_VERSION_MAJOR
#error "build with -DESP_IDF_VERSION_MAJOR=4 or 5 (the Makefile builds both)"
#endif
#if (ESP_IDF_VERSION_MAJOR >= 5) != (SHA_STUB_MBEDTLS_VERSION >= 0x03000000)
#error "IDF 5 ships mbedTLS 3.x and IDF 4.4 mbedTLS 2.28: build the two together"
#endif

// The canary library's body, in a namespace of its own beside the shim's.
namespace pio_mesh_crypto {
constexpr size_t SHA256_OUT_LEN = 32;
#include MCLINES_PIO_INC
}  // namespace pio_mesh_crypto

#ifdef MCLINES_WAP_SHIM
#include "mesh_crypto.h"  // the sketch's shim: mesh_crypto::sha256_domain()
#endif

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

bool all_zero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (p[i] != 0) return false;
  }
  return true;
}

void openssl_sha(const std::string& domain, const uint8_t* data, size_t n, uint8_t out[32]) {
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  unsigned int olen = 0;
  EVP_DigestInit_ex(md, EVP_sha256(), nullptr);
  if (!domain.empty()) EVP_DigestUpdate(md, domain.data(), domain.size());
  if (n > 0) EVP_DigestUpdate(md, data, n);
  EVP_DigestFinal_ex(md, out, &olen);
  EVP_MD_CTX_free(md);
}

using Hash = bool (*)(const char*, const uint8_t*, size_t, uint8_t*);

// What one call answered and left, and what the stand-in saw.
struct Run {
  bool answer;
  std::vector<uint8_t> out;
  bool fired;
  int after_failure;
  int live;
};

Run run(Hash h, sha_stub::Step step, int update, const char* domain, const uint8_t* data,
        size_t n) {
  const int live_before = sha_stub::g_live;
  uint8_t out[32];
  std::memset(out, 0x5C, sizeof out);
  sha_stub::arm(step, 0, update);
  Run r;
  r.answer = h(domain, data, n, out);
  r.out.assign(out, out + sizeof out);
  r.fired = sha_stub::g_fired;
  r.after_failure = sha_stub::g_steps_after_failure;
  r.live = sha_stub::g_live - live_before;
  sha_stub::disarm();
  return r;
}

struct Case {
  sha_stub::Step step;
  int update;
  const char* name;
};
const Case kCases[] = {
    {sha_stub::Step::Starts, 0, "start"},
    {sha_stub::Step::Update, 0, "the domain's update"},
    {sha_stub::Step::Update, 1, "the data's update"},
    {sha_stub::Step::Finish, 0, "finish"},
};

void check_copy(const char* copy, Hash h) {
  const char* domain = "securacv:pair:confirm:v0";
  uint8_t data[36];
  for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(0x3C ^ (5 * i));
  uint8_t want[32];
  openssl_sha(domain, data, sizeof data, want);

  const Run ok = run(h, sha_stub::Step::None, 0, domain, data, sizeof data);
  CHECK_MSG(ok.answer && std::memcmp(ok.out.data(), want, 32) == 0 && ok.live == 0,
            std::string(copy) + ": every step runs: true and SHA-256(domain || data), context freed");
  uint8_t want_data_only[32];
  openssl_sha("", data, sizeof data, want_data_only);
  const Run bare = run(h, sha_stub::Step::None, 0, nullptr, data, sizeof data);
  CHECK_MSG(bare.answer && std::memcmp(bare.out.data(), want_data_only, 32) == 0,
            std::string(copy) + ": no domain: SHA-256(data)");

  for (const Case& c : kCases) {
    const std::string label = std::string(copy) + ", " + c.name + " fails";
    const Run r = run(h, c.step, c.update, domain, data, sizeof data);
    CHECK_MSG(r.fired, label + ": reached");
    CHECK_MSG(!r.answer, label + ": answers false");
    CHECK_MSG(all_zero(r.out.data(), r.out.size()), label + ": 32 zero bytes");
    CHECK_MSG(r.after_failure == 0, label + ": no step after the failed one");
    CHECK_MSG(r.live == 0, label + ": the context freed");
  }
  // With no domain the data is the only update.
  const Run r = run(h, sha_stub::Step::Update, 0, nullptr, data, sizeof data);
  CHECK_MSG(r.fired && !r.answer && all_zero(r.out.data(), r.out.size()),
            std::string(copy) + ", no domain, the data's update fails: false and zeros");

  // No output: false, and nothing begun.
  sha_stub::arm(sha_stub::Step::None);
  CHECK_MSG(!h(domain, data, sizeof data, nullptr) && sha_stub::g_inits == 0,
            std::string(copy) + ": a null output is refused before any hash");
  sha_stub::disarm();
}

#ifdef MCLINES_WAP_SHIM
// The two copies, the same cases: the same answers and the same bytes.
void check_copies_agree() {
  const char* domain = "securacv:ble_scan:hashed_id:v1";
  uint8_t data[38];
  for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(0xC0 ^ i);
  const Case all[] = {{sha_stub::Step::None, 0, "no step"}, kCases[0], kCases[1], kCases[2], kCases[3]};
  for (const Case& c : all) {
    for (const char* d : {domain, (const char*)nullptr, ""}) {
      const Run a = run(pio_mesh_crypto::sha256_domain, c.step, c.update, d, data, sizeof data);
      const Run b = run(mesh_crypto::sha256_domain, c.step, c.update, d, data, sizeof data);
      CHECK_MSG(a.answer == b.answer && a.out == b.out && a.fired == b.fired &&
                    a.after_failure == b.after_failure && a.live == b.live,
                std::string("the library's and the sketch's copies agree: ") + c.name + " fails, domain " +
                    (d == nullptr ? "null" : *d == '\0' ? "empty" : d));
    }
  }
}
#endif

}  // namespace

int main() {
  check_copy(ESP_IDF_VERSION_MAJOR >= 5 ? "the canary library's IDF 5 body"
                                        : "the canary library's IDF 4 body (the _ret names)",
             pio_mesh_crypto::sha256_domain);
#ifdef MCLINES_WAP_SHIM
  check_copy("the sketch's shim", mesh_crypto::sha256_domain);
  check_copies_agree();
#endif
  if (g_failures != 0) {
    std::printf("test_mesh_crypto_lines (IDF %d): %d of %d checks FAILED\n", ESP_IDF_VERSION_MAJOR,
                g_failures, g_checks);
    return 1;
  }
  std::printf("ALL mesh_crypto lines (IDF %d) TESTS PASSED (%d checks)\n", ESP_IDF_VERSION_MAJOR,
              g_checks);
  return 0;
}
