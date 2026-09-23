// Host tests for firmware/common/network/ap_security_policy.h — the SoftAP /
// STA security the firmware asks the Wi-Fi driver for (F16).
//
// The rows that matter most:
//   * transition only when knob && SAE-in-build && passphrase >= 8;
//   * PMF is capable in every row and REQUIRED in none (a required PMF would
//     lock WPA2-only clients out of the setup AP);
//   * a 7-character passphrase is refused (the WPA2 floor);
//   * a driver refusal is reported as WPA2, not as what was asked for;
//   * the label table covers the auth modes a scan can return.

#include "../common/network/ap_security_policy.h"

#include <cstdio>
#include <cstring>

using namespace canary::net::ap_security;

static int g_failures = 0;

#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d: ", __func__, __LINE__);         \
      std::printf(__VA_ARGS__);                                \
      std::printf("\n");                                       \
      ++g_failures;                                            \
    }                                                          \
  } while (0)

static void every_knob_build_length_row() {
  const size_t lengths[] = {0, 7, 8, 15, 63};
  for (int knob = 0; knob < 2; ++knob) {
    for (int sae = 0; sae < 2; ++sae) {
      for (size_t len : lengths) {
        const Decision d = decide(knob != 0, sae != 0, len);
        const bool want = knob && sae && len >= 8;
        CHECK((d.mode == AuthMode::WPA2_WPA3_TRANSITION) == want,
              "knob=%d sae=%d len=%zu", knob, sae, len);
        CHECK(std::strcmp(d.label, want ? "wpa2-wpa3" : "wpa2") == 0,
              "label matches the mode: %s", d.label);
        CHECK(d.pmf_capable, "PMF capable in every row (knob=%d sae=%d len=%zu)", knob, sae, len);
        CHECK(!d.pmf_required, "PMF never required (knob=%d sae=%d len=%zu)", knob, sae, len);
        CHECK(d.reason && d.reason[0], "a reason is always given");
      }
    }
  }
}

static void reasons_name_the_first_blocker() {
  CHECK(std::strstr(decide(false, true, 15).reason, "CANARY_AP_WPA3_TRANSITION=0"), "knob first");
  CHECK(std::strstr(decide(true, false, 15).reason, "SoftAP SAE"), "then the core");
  CHECK(std::strstr(decide(true, true, 7).reason, "8-character"), "then the passphrase");
  CHECK(std::strstr(decide(true, true, 8).reason, "transition"), "transition reason");
}

static void seven_char_passphrase_refused() {
  CHECK(decide(true, true, 7).mode == AuthMode::WPA2_PSK, "7 chars is below the WPA2 floor");
  CHECK(decide(true, true, 8).mode == AuthMode::WPA2_WPA3_TRANSITION,
        "8 chars (the canary's cv-XXXXX) qualifies");
}

static void driver_refusal_reports_wpa2() {
  const Decision asked = decide(true, true, 15);
  const Decision ok = after_driver(asked, true);
  CHECK(ok.mode == AuthMode::WPA2_WPA3_TRANSITION && std::strcmp(ok.label, "wpa2-wpa3") == 0,
        "accepted transition stays transition");
  const Decision refused = after_driver(asked, false);
  CHECK(refused.mode == AuthMode::WPA2_PSK, "refused transition is WPA2 on the air");
  CHECK(std::strcmp(refused.label, "wpa2") == 0, "label says wpa2: %s", refused.label);
  CHECK(std::strstr(refused.reason, "refused"), "reason names the refusal: %s", refused.reason);
  CHECK(!refused.pmf_required, "still never required");
  const Decision plain = decide(false, true, 15);
  const Decision plain_after = after_driver(plain, false);
  CHECK(std::strcmp(plain_after.reason, plain.reason) == 0,
        "a WPA2 request keeps its own reason whatever the driver said");
}

static void label_table() {
  CHECK(std::strcmp(label_for(0), "open") == 0, "open");
  CHECK(std::strcmp(label_for(1), "wep") == 0, "wep");
  CHECK(std::strcmp(label_for(2), "wpa") == 0, "wpa");
  CHECK(std::strcmp(label_for(3), "wpa2") == 0, "wpa2");
  CHECK(std::strcmp(label_for(4), "wpa-wpa2") == 0, "wpa-wpa2");
  CHECK(std::strcmp(label_for(5), "enterprise") == 0, "enterprise");
  CHECK(std::strcmp(label_for(6), "wpa3") == 0, "wpa3");
  CHECK(std::strcmp(label_for(7), "wpa2-wpa3") == 0, "wpa2-wpa3");
  CHECK(std::strcmp(label_for(10), "enterprise") == 0, "wpa3 enterprise 192");
  CHECK(std::strcmp(label_for(-1), "other") == 0, "negative → other");
  CHECK(std::strcmp(label_for(99), "other") == 0, "unknown → other");
  // The setup page hides the lock icon for exactly "open"; nothing else may
  // spell it.
  for (int m = 1; m < 32; ++m) {
    CHECK(std::strcmp(label_for(m), "open") != 0, "only mode 0 is open (m=%d)", m);
  }
  CHECK(std::strcmp(label_for(kAuthWpa2Wpa3Psk), decide(true, true, 8).label) == 0,
        "the transition label and the scan label agree");
  CHECK(std::strcmp(label_for(kAuthWpa2Psk), decide(false, true, 8).label) == 0,
        "the WPA2 label and the scan label agree");
  CHECK(kAuthOpen == 0, "open is 0");
}

int main() {
  every_knob_build_length_row();
  reasons_name_the_first_blocker();
  seven_char_passphrase_refused();
  driver_refusal_reports_wpa2();
  label_table();
  if (g_failures) {
    std::printf("test_ap_security_policy: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_ap_security_policy: all checks passed\n");
  return 0;
}
