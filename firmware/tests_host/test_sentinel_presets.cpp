/**
 * @file test_sentinel_presets.cpp
 * @brief Host check that a preset's SENT_* and FEATURE_* macros translate into a
 *        sane, working securacv::fusion::FusionConfig via the project's
 *        sentinel_config.h — proving the preset -> engine pipeline end to end
 *        without hardware.
 *
 * Compiled once per preset by tests_host/Makefile: the preset directory is put
 * on the include path (so <config.h> resolves to that preset) and the preset's
 * name is passed as SENTINEL_PRESET_NAME. Two presets are checked — the
 * `door` reference (radar present) and `mailbox-lite` (radar/CSI disabled) —
 * which together exercise both the full-stack and reduced-tier mappings.
 */

#include "canary/sentinel_config.h"
#include "fusion/sentinel_channels.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace securacv::fusion;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg)                                                      \
  do {                                                                        \
    ++g_checks;                                                               \
    if (!(cond)) { ++g_fails; std::printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
  } while (0)

#ifndef SENTINEL_PRESET_NAME
#define SENTINEL_PRESET_NAME "unknown"
#endif

template <typename ObserveFn>
static FusionResult run_until(FusionEngine& e, ObserveFn observe, uint32_t until,
                              uint32_t step) {
  FusionResult r = e.last();
  for (uint32_t t = 0; t <= until; t += step) { observe(e, t); r = e.evaluate(t); }
  return r;
}

int main() {
  std::printf("== sentinel preset check: %s ==\n", SENTINEL_PRESET_NAME);
  const FusionConfig cfg = canary::build_fusion_config();

  // Thresholds are ordered and in-range for any preset.
  CHECK(cfg.clear_score < cfg.present_score, "clear < present threshold");
  CHECK(cfg.present_score <= cfg.confirmed_score, "present <= confirmed threshold");
  CHECK(cfg.confirmed_score <= 100, "confirmed threshold in range");
  CHECK(cfg.min_confirm_modalities >= 1, "at least one modality to confirm");

  // At least PIR + the two aggregate RF channels + light are always enabled
  // (present in every tier, including Lite).
  CHECK(cfg.channels[static_cast<size_t>(Channel::Pir)].enabled, "PIR enabled");
  CHECK(cfg.channels[static_cast<size_t>(Channel::WifiRf)].enabled, "WiFi-RF enabled");
  CHECK(cfg.channels[static_cast<size_t>(Channel::Ble)].enabled, "BLE enabled");
  CHECK(cfg.channels[static_cast<size_t>(Channel::Light)].enabled, "light enabled");

  // A body corroborated by two independent modalities Confirms under whatever
  // this preset's thresholds are. Use PIR (thermal) + WiFi-RF/BLE (carried
  // radio) so the check holds even on Lite, which has no radar/CSI.
  FusionEngine e(cfg);
  using namespace securacv::fusion::channels;
  auto obs = [](FusionEngine& en, uint32_t t) {
    en.observe(Channel::Pir, pir_vote(true, 0, 1500), 95, t);
    en.observe(Channel::WifiRf, count_vote(4, 1, 3), 100, t);  // a device at the door
    en.observe(Channel::Ble,    count_vote(4, 1, 3), 100, t);
  };
  FusionResult r = run_until(e, obs, 6000, 200);
  CHECK(r.level == Level::Confirmed || r.level == Level::Loiter,
        "PIR + carried-radio corroboration reaches Confirmed/Loiter");
  CHECK(r.strong_modalities >= 2, "two independent modality classes agreed");

  std::printf("%d checks, %d failures\n", g_checks, g_fails);
  if (g_fails) { std::printf("PRESET CHECK FAILED\n"); return EXIT_FAILURE; }
  std::printf("preset %s ok\n", SENTINEL_PRESET_NAME);
  return EXIT_SUCCESS;
}
