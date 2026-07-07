/* Host tests for firmware/common/witness/witness_chain.h — THE canonical
 * chain construction shared by canary-wap, canary-sense, and the
 * off-device verifier (tools/verify_witness_log.py).
 *
 * Uses OpenSSL's SHA-256 on the host (the header's mbedTLS helpers only
 * compile on-target), so the test hashes the PURE wc_chain_buf layout
 * itself and pins the result against goldens generated with python's
 * hashlib — if any of the three implementations drifts, one of these
 * fixtures fails.
 *
 * Build & run (CI: firmware.yml host tests):
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/common \
 *       firmware/tests_host/test_witness_chain_core.cpp \
 *       -lcrypto -o /tmp/test_witness_chain_core && /tmp/test_witness_chain_core
 */

#include <cstdio>
#include <cstring>

#include <openssl/sha.h>

#include "witness/witness_chain.h"

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static void host_sha256_domain(const char* domain, const uint8_t* data,
                               size_t len, uint8_t out[32]) {
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, domain, strlen(domain));
  const uint8_t sep = 0x00;
  SHA256_Update(&ctx, &sep, 1);
  SHA256_Update(&ctx, data, len);
  SHA256_Final(out, &ctx);
}

static void to_hex(char* out, const uint8_t* in, size_t len) {
  static const char* H = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = H[in[i] >> 4];
    out[2 * i + 1] = H[in[i] & 0x0F];
  }
  out[2 * len] = '\0';
}

/* Golden fixtures generated with python hashlib over the documented
 * construction (the same code path tools/verify_witness_log.py uses):
 *   prev = 0x40..0x5f, payload_hash = 0x00..0x1f, seq = 42, tb = 3000. */
static const char GOLDEN_BUF_HEX[] =
    "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
    "0000002a00000bb8";
static const char GOLDEN_CHAIN_HEX[] =
    "bd929c12a9a794af02df6af9355ab45cf790f9a4a7dfe8f71f96e904c491dbdd";
static const char GOLDEN_GENESIS_HEX[] =
    "4927d18c091c96089c955928c15b0a59c9e4e2f077d530759b14913825a6e77a";

static void test_chain_buf_layout() {
  uint8_t prev[32], ph[32];
  for (int i = 0; i < 32; i++) {
    prev[i] = (uint8_t)(0x40 + i);
    ph[i] = (uint8_t)i;
  }
  uint8_t buf[WC_CHAIN_BUF_SIZE];
  CHECK(wc_chain_buf(prev, ph, 42, 3000, buf) == WC_CHAIN_BUF_SIZE);

  char hex[2 * WC_CHAIN_BUF_SIZE + 1];
  to_hex(hex, buf, sizeof(buf));
  CHECK(strcmp(hex, GOLDEN_BUF_HEX) == 0);

  /* Big-endian field placement, byte by byte (seq=42, tb=3000=0x0BB8). */
  CHECK(buf[64] == 0x00 && buf[65] == 0x00 && buf[66] == 0x00 &&
        buf[67] == 0x2A);
  CHECK(buf[68] == 0x00 && buf[69] == 0x00 && buf[70] == 0x0B &&
        buf[71] == 0xB8);
}

static void test_chain_hash_golden() {
  uint8_t prev[32], ph[32];
  for (int i = 0; i < 32; i++) {
    prev[i] = (uint8_t)(0x40 + i);
    ph[i] = (uint8_t)i;
  }
  uint8_t buf[WC_CHAIN_BUF_SIZE];
  wc_chain_buf(prev, ph, 42, 3000, buf);

  uint8_t ch[32];
  host_sha256_domain(WC_DOMAIN_CHAIN, buf, sizeof(buf), ch);
  char hex[65];
  to_hex(hex, ch, 32);
  CHECK(strcmp(hex, GOLDEN_CHAIN_HEX) == 0);
}

static void test_genesis_golden() {
  const char* device_id = "canary-testdevice";
  uint8_t g[32];
  host_sha256_domain(WC_DOMAIN_GENESIS, (const uint8_t*)device_id,
                     strlen(device_id), g);
  char hex[65];
  to_hex(hex, g, 32);
  CHECK(strcmp(hex, GOLDEN_GENESIS_HEX) == 0);
}

static void test_domain_strings_locked() {
  /* The literals every implementation and the python verifier share. */
  CHECK(strcmp(WC_DOMAIN_CHAIN, "securacv:fw:chain:v1") == 0);
  CHECK(strcmp(WC_DOMAIN_PAYLOAD, "securacv:fw:payload:v1") == 0);
  CHECK(strcmp(WC_DOMAIN_BOOT, "securacv:fw:boot:v1") == 0);
  CHECK(strcmp(WC_DOMAIN_GENESIS, "securacv:genesis:v1") == 0);
}

int main() {
  test_domain_strings_locked();
  test_chain_buf_layout();
  test_chain_hash_golden();
  test_genesis_golden();

  if (g_failures != 0) {
    std::printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL witness_chain_core TESTS PASSED\n");
  return 0;
}
