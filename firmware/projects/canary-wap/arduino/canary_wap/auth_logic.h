/*
 * SecuraCV Canary WAP — pure auth-throttle decisions (host-testable)
 *
 * Arduino-free: stdint only. The brute-force lockout exists to make token
 * GUESSING expensive. A request that presents no credential at all carries
 * no guess — counting it lets any credential-less client (a dashboard tab
 * left open after its session cookie died, a captive-portal probe, an
 * unauthenticated page load) arm the global lockout and 429 the legitimate
 * operator's CORRECT token. That is a self-DoS, not brute-force protection.
 *
 * Malformed and wrong-token attempts still count: those are the shapes an
 * actual guesser produces, and exempting them would weaken the token gate.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_AUTH_LOGIC_H
#define SECURACV_AUTH_LOGIC_H

#include <stdint.h>

namespace auth_logic {

enum class Attempt : uint8_t {
  NO_CREDENTIAL = 0,  // no Authorization header AND no token query param
  MALFORMED     = 1,  // credential material present but unusable
                      // (oversized header, non-Bearer scheme)
  WRONG_TOKEN   = 2,  // a token value was presented and did not match
};

// Should this failed attempt advance the exponential-backoff lockout?
// Only attempts that could be probing the token space count.
inline bool counts_toward_lockout(Attempt a) {
  return a != Attempt::NO_CREDENTIAL;
}

}  // namespace auth_logic

#endif  // SECURACV_AUTH_LOGIC_H
