/* Host tests for auth_logic.h — which failed-auth shapes feed the global
 * brute-force lockout. Build & run (CI: firmware.yml host tests):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_auth_logic.cpp \
 *       -o /tmp/test_auth_logic && /tmp/test_auth_logic
 */

#include <cstdio>

#include "auth_logic.h"

using auth_logic::Attempt;
using auth_logic::counts_toward_lockout;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

int main() {
  // A credential-less request carries no token guess. It must NOT arm the
  // lockout — otherwise one open dashboard tab whose session died (RAM-only
  // sessions do not survive the post-setup reboot) polls the device into a
  // permanent 429 that rejects even the operator's CORRECT token.
  CHECK(!counts_toward_lockout(Attempt::NO_CREDENTIAL));

  // Anything that presents credential material still counts — these are the
  // shapes a real guesser produces, and exempting them would weaken the
  // token gate.
  CHECK(counts_toward_lockout(Attempt::MALFORMED));
  CHECK(counts_toward_lockout(Attempt::WRONG_TOKEN));

  if (g_failures) {
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL auth_logic tests PASSED\n");
  return 0;
}
