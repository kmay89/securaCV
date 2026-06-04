// Host-side test for device_pseudonym::derive() — the salted, non-reversible
// device identifier used in operator-facing diagnostics (replaces raw MAC).
//
// Uses OpenSSL SHA-256 (host) to (a) link the same derive() the firmware compiles
// with mbedtls, and (b) independently recompute the construction to pin the bytes.
//
// Build: see tests_host/Makefile (links -lcrypto).

#include "../arduino/canary_wap/device_pseudonym.h"

#include <openssl/sha.h>
#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond, msg)                          \
  do {                                            \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; } \
    else         { printf("ok:   %s\n", (msg)); } \
  } while (0)

int main() {
  using namespace device_pseudonym;

  uint8_t secret[SECRET_LEN];  memset(secret,  0x11, sizeof(secret));
  uint8_t secret2[SECRET_LEN]; memset(secret2, 0x22, sizeof(secret2));

  char a[32], b[32], d[32];

  CHECK(derive(secret, SECRET_LEN, a, sizeof(a)), "derive succeeds");
  CHECK(strlen(a) == HEX_LEN, "token is HEX_LEN (16) chars");

  derive(secret, SECRET_LEN, b, sizeof(b));
  CHECK(strcmp(a, b) == 0, "deterministic for same secret");

  derive(secret2, SECRET_LEN, d, sizeof(d));
  CHECK(strcmp(a, d) != 0, "different secret -> different token");

  // Non-leakage: the token is a hash, not an echo of the salt's leading bytes.
  char sec_lead[HEX_LEN + 1];
  for (size_t i = 0; i < TOKEN_BYTES; i++) snprintf(sec_lead + i * 2, 3, "%02x", secret[i]);
  CHECK(strcmp(a, sec_lead) != 0, "token is not the raw secret prefix");

  // Charset: lowercase hex only.
  bool all_hex = true;
  for (size_t i = 0; i < strlen(a); i++) {
    char ch = a[i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) all_hex = false;
  }
  CHECK(all_hex, "token is lowercase hex");

  // Buffer guard: too-small output is rejected, not truncated.
  char tiny[4];
  CHECK(!derive(secret, SECRET_LEN, tiny, sizeof(tiny)), "rejects undersized buffer");

  // Null/empty guards.
  CHECK(!derive(nullptr, SECRET_LEN, a, sizeof(a)), "rejects null secret");
  CHECK(!derive(secret, 0, a, sizeof(a)), "rejects empty secret");

  // Independent recomputation of SHA256("canary:device-id:v1:" || secret)[0..8].
  uint8_t input[20 + SECRET_LEN];
  memcpy(input, "canary:device-id:v1:", 20);
  memcpy(input + 20, secret, SECRET_LEN);
  uint8_t h[32];
  SHA256(input, 20 + SECRET_LEN, h);
  char expect[HEX_LEN + 1];
  for (size_t i = 0; i < TOKEN_BYTES; i++) snprintf(expect + i * 2, 3, "%02x", h[i]);
  // recompute `a` (it was overwritten by the null-guard checks above)
  derive(secret, SECRET_LEN, a, sizeof(a));
  CHECK(strcmp(a, expect) == 0, "matches independent SHA-256 construction");

  printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
