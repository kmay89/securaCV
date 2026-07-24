/**
 * @file sentinel_config.h
 * @brief Maps a preset's SENT_* and FEATURE_* macros onto a
 *        securacv::fusion::FusionConfig. This is the ONE place the compile-time
 *        preset data crosses into the board-agnostic fusion engine.
 *
 * Pure translation — no Arduino / RTOS dependency — so it (and therefore every
 * preset) is exercised on the host by tests_host/test_sentinel_presets.cpp. A
 * preset must be included (via -I configs/canary-sentinel/<preset>) before this
 * header so the SENT_* and FEATURE_* macros are defined.
 */

#pragma once

#include "fusion/sentinel_fusion.h"

#include <config.h>  // the active preset (configs/canary-sentinel/<preset>/config.h)

namespace canary {

// Build the fusion configuration for the active preset. A channel's ChannelSpec
// is enabled iff its FEATURE_* flag is set; disabled channels never contribute
// (the engine ignores observe() calls for them), so a Lite build with no radar
// simply leaves that slot disabled.
inline securacv::fusion::FusionConfig build_fusion_config() {
  using namespace securacv::fusion;
  FusionConfig c;

  auto set = [&](Channel ch, bool enabled, uint8_t weight, uint8_t evasion,
                 uint16_t stale_ms) {
    ChannelSpec& s = c.channels[static_cast<size_t>(ch)];
    s.enabled = enabled;
    s.weight = weight;
    s.evasion_cost = evasion;
    s.stale_ms = stale_ms;
  };

  set(Channel::Pir,     FEATURE_PIR,           SENT_W_PIR,     SENT_EV_PIR,     SENT_STALE_PIR);
  set(Channel::Radar,   FEATURE_MMWAVE_RADAR,  SENT_W_RADAR,   SENT_EV_RADAR,   SENT_STALE_RADAR);
  set(Channel::WifiCsi, FEATURE_WIFI_CSI,      SENT_W_CSI,     SENT_EV_CSI,     SENT_STALE_CSI);
  set(Channel::WifiRf,  FEATURE_WIFI_RF,       SENT_W_RF,      SENT_EV_RF,      SENT_STALE_RF);
  set(Channel::Ble,     FEATURE_BLE,           SENT_W_BLE,     SENT_EV_BLE,     SENT_STALE_BLE);
  set(Channel::Light,   FEATURE_AMBIENT_LIGHT, SENT_W_LIGHT,   SENT_EV_LIGHT,   SENT_STALE_LIGHT);
  set(Channel::Contact, FEATURE_CONTACT,       SENT_W_CONTACT, SENT_EV_CONTACT, SENT_STALE_CONTACT);
  set(Channel::Vision,  FEATURE_VISION,        SENT_W_VISION,  SENT_EV_VISION,  SENT_STALE_VISION);
  set(Channel::Tamper,  FEATURE_TAMPER,        SENT_W_TAMPER,  SENT_EV_TAMPER,  SENT_STALE_TAMPER);

  c.present_score          = SENT_PRESENT_SCORE;
  c.confirmed_score        = SENT_CONFIRMED_SCORE;
  c.clear_score            = SENT_CLEAR_SCORE;
  c.independence_bonus     = SENT_INDEP_BONUS;
  c.min_confirm_modalities = SENT_MIN_CONFIRM;
  c.denied_suspicion       = SENT_DENIED_SUSPICION;
  c.anomaly_score          = SENT_ANOMALY_SCORE;
  c.silent_body_is_anomaly = (SENT_SILENT_BODY_ANOMALY != 0);

  c.present_debounce_ms = SENT_PRESENT_DEBOUNCE_MS;
  c.clear_debounce_ms   = SENT_CLEAR_DEBOUNCE_MS;
  c.loiter_dwell_ms     = SENT_LOITER_DWELL_MS;
  c.anomaly_latch_ms    = SENT_ANOMALY_LATCH_MS;

  return c;
}

}  // namespace canary
