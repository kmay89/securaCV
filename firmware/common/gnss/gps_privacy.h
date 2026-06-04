#pragma once
//
// gps_privacy — operator-facing GPS coordinate coarsening (Invariant III / F-03).
//
// The witness device MUST NOT surface precise coordinates in any operator-visible
// output (serial diagnostics, CBOR/JSON status payloads). gps_coarsen_deg() rounds
// a decimal-degree coordinate to a fixed number of decimal places before it is
// emitted. The canonical precision is 3 dp ≈ 110 m at the equator — enough to place
// an event in a neighbourhood, never at a doorstep.
//
// Pure and header-only: no Arduino / ESP-IDF dependencies, so the exact function the
// firmware compiles is also exercised in the host test harness (tests_host).
//
// The regression guardrail (firmware/scripts/regression_check.sh) and the spec refer
// to this helper by the bare name `gps_coarsen_deg`; structured lat/lon emission that
// does not pass through it is a hard failure.
//
#include <cmath>

namespace securacv {
namespace gps {

// Round `deg` (decimal degrees) to `dp` decimal places, half-away-from-zero so the
// sign is preserved symmetrically. `dp` defaults to 3 (≈110 m). Non-finite inputs
// (NaN/Inf) pass through unchanged and the 0.0 "no fix" sentinel stays exactly 0.0.
inline double coarsen_deg(double deg, int dp = 3) {
  if (!std::isfinite(deg)) return deg;
  if (dp < 0) dp = 0;
  double scale = 1.0;
  for (int i = 0; i < dp; ++i) scale *= 10.0;
  const double v = deg * scale;
  const double r = (v < 0.0) ? -std::floor(-v + 0.5) : std::floor(v + 0.5);
  return r / scale;
}

}  // namespace gps
}  // namespace securacv

// Canonical free-function name used at call sites and by the privacy guardrail.
inline double gps_coarsen_deg(double deg, int dp = 3) {
  return securacv::gps::coarsen_deg(deg, dp);
}
