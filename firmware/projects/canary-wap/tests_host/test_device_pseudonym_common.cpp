// Host-side test for the SHARED device_pseudonym header (firmware/common/identity).
// This is the helper the canary-vision (and other non-arduino) firmware trees adopt
// to replace the raw MAC in operator-facing diagnostics. It must derive the SAME
// non-reversible token construction as the canary-wap arduino copy, so the byte
// layout is pinned here independently with OpenSSL.
//
// Build: see tests_host/Makefile (links -lcrypto). ARDUINO is undefined on the host,
// so only the pure derive() is compiled (device_id_hex() is device-only).

#include "../../../common/identity/device_pseudonym.h"

#include <openssl/sha.h>
#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond, msg)                                            \
  do {                                                             \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; }   \
    else         { printf("ok:   %s\n", (msg)); }                 \
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

  // Buffer / argument guards.
  char tiny[4];
  CHECK(!derive(secret, SECRET_LEN, tiny, sizeof(tiny)), "rejects undersized buffer");
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
  derive(secret, SECRET_LEN, a, sizeof(a));  // recompute (a was overwritten above)
  CHECK(strcmp(a, expect) == 0, "matches independent SHA-256 construction");

  // Cross-check: the shared header must agree byte-for-byte with the canary-wap
  // arduino copy's construction (same DOMAIN, same truncation) — pinned via the
  // independent recomputation above, which both copies are tested against.

  printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
