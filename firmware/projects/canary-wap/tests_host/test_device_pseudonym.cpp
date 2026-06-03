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

  uint8_t mac[6]  = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  uint8_t mac2[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x00};
  uint8_t secret[SECRET_LEN];  memset(secret,  0x11, sizeof(secret));
  uint8_t secret2[SECRET_LEN]; memset(secret2, 0x22, sizeof(secret2));

  char a[32], b[32], c[32], d[32];

  CHECK(derive(mac, 6, secret, SECRET_LEN, a, sizeof(a)), "derive succeeds");
  CHECK(strlen(a) == HEX_LEN, "token is HEX_LEN (16) chars");

  derive(mac, 6, secret, SECRET_LEN, b, sizeof(b));
  CHECK(strcmp(a, b) == 0, "deterministic for same (mac, secret)");

  derive(mac2, 6, secret, SECRET_LEN, c, sizeof(c));
  CHECK(strcmp(a, c) != 0, "different mac -> different token");

  derive(mac, 6, secret2, SECRET_LEN, d, sizeof(d));
  CHECK(strcmp(a, d) != 0, "different secret -> different token");

  // Non-leakage: the raw MAC (as hex) must not appear in the token.
  char machex[13];
  for (int i = 0; i < 6; i++) snprintf(machex + i * 2, 3, "%02x", mac[i]);
  CHECK(strstr(a, machex) == nullptr, "raw mac hex absent from token");

  // Charset: lowercase hex only.
  bool all_hex = true;
  for (size_t i = 0; i < strlen(a); i++) {
    char ch = a[i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) all_hex = false;
  }
  CHECK(all_hex, "token is lowercase hex");

  // Buffer guard: too-small output is rejected, not truncated.
  char tiny[4];
  CHECK(!derive(mac, 6, secret, SECRET_LEN, tiny, sizeof(tiny)), "rejects undersized buffer");

  // Null/empty guards.
  CHECK(!derive(nullptr, 6, secret, SECRET_LEN, a, sizeof(a)), "rejects null mac");
  CHECK(!derive(mac, 0, secret, SECRET_LEN, a, sizeof(a)), "rejects empty mac");

  // Independent recomputation of SHA256("canary:device-id:v1:" || secret || mac)[0..8].
  uint8_t input[20 + SECRET_LEN + 6];
  memcpy(input, "canary:device-id:v1:", 20);
  memcpy(input + 20, secret, SECRET_LEN);
  memcpy(input + 20 + SECRET_LEN, mac, 6);
  uint8_t h[32];
  SHA256(input, 20 + SECRET_LEN + 6, h);
  char expect[HEX_LEN + 1];
  for (size_t i = 0; i < TOKEN_BYTES; i++) snprintf(expect + i * 2, 3, "%02x", h[i]);
  // recompute `a` (it was overwritten by the null-guard checks above)
  derive(mac, 6, secret, SECRET_LEN, a, sizeof(a));
  CHECK(strcmp(a, expect) == 0, "matches independent SHA-256 construction");

  printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
