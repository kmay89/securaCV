// Host-side test for gps_coarsen_deg() — the operator-facing GPS coarsening that
// keeps precise coordinates out of serial logs and CBOR/JSON status payloads
// (Invariant III / F-03). Pure header, no Arduino glue: the test includes it
// directly and asserts the exact rounding the firmware emits.

#include "../../../common/gnss/gps_privacy.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;
#define CHECK(cond, msg)                                            \
  do {                                                             \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; }   \
    else         { printf("ok:   %s\n", (msg)); }                 \
  } while (0)

static bool approx(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main() {
  // Default precision is 3 dp (≈110 m).
  CHECK(approx(gps_coarsen_deg(37.7749295), 37.775), "rounds positive to 3 dp (half up)");
  CHECK(approx(gps_coarsen_deg(37.7744000), 37.774), "rounds positive down");
  CHECK(approx(gps_coarsen_deg(-122.4194155), -122.419), "rounds negative toward zero");
  CHECK(approx(gps_coarsen_deg(-122.4195500), -122.420), "negative half rounds away from zero");

  // The no-fix sentinel and origin stay exactly 0.
  CHECK(gps_coarsen_deg(0.0) == 0.0, "0.0 sentinel passes through");

  // Already-coarse inputs are unchanged.
  CHECK(approx(gps_coarsen_deg(12.345), 12.345), "already 3 dp is unchanged");

  // Configurable precision.
  CHECK(approx(gps_coarsen_deg(37.7749295, 1), 37.8), "dp=1 coarsens to ~11 km");
  CHECK(approx(gps_coarsen_deg(37.7749295, 0), 38.0), "dp=0 rounds to whole degree");

  // The coarsened value must never carry more precision than requested: a value
  // rounded to 3 dp, re-multiplied by 1000, must be an integer.
  double c = gps_coarsen_deg(51.5073509);
  CHECK(approx(c * 1000.0, std::floor(c * 1000.0 + 0.5)), "no residual precision beyond dp");

  // Non-finite inputs pass through without trapping.
  CHECK(std::isnan(gps_coarsen_deg(std::nan(""))), "NaN passes through");

  printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
