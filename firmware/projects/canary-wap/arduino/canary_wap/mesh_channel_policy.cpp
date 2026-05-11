/*
 * SecuraCV Canary — Mesh Channel Policy (implementation)
 *
 * The decision logic in `decide()` is pure and host-testable. The radio
 * sampling glue at the bottom is gated on the firmware build.
 */

#include "mesh_channel_policy.h"

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#define SECURACV_MCP_HAS_ESP32 1
#endif

#if SECURACV_MCP_HAS_ESP32
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#endif

namespace mesh_channel_policy {

// ────────────────────────────────────────────────────────────────────────────
// Pure decision logic
// ────────────────────────────────────────────────────────────────────────────

ChannelDecision decide(const RadioState& state) {
  ChannelDecision out{};
  if (state.sta_connected && state.sta_channel > 0) {
    out.channel = state.sta_channel;
    out.locked_to_sta = true;
    return out;
  }
  if (state.ap_active && state.ap_channel > 0) {
    out.channel = state.ap_channel;
    out.locked_to_ap = true;
    return out;
  }
  out.channel = MESH_FALLBACK_CHANNEL;
  out.fallback = true;
  return out;
}

// ────────────────────────────────────────────────────────────────────────────
// Module state (listeners + last decision)
// ────────────────────────────────────────────────────────────────────────────

static constexpr size_t MAX_LISTENERS = 4;
static ChannelChangedCallback g_listeners[MAX_LISTENERS] = {};
static size_t g_listener_count = 0;
static uint8_t g_last_channel = 0;

#if !SECURACV_MCP_HAS_ESP32
// Host-test backing store
static RadioState g_test_state{};
#endif

bool register_listener(ChannelChangedCallback cb) {
  if (cb == nullptr || g_listener_count >= MAX_LISTENERS) return false;
  for (size_t i = 0; i < g_listener_count; i++) {
    if (g_listeners[i] == cb) return true;  // already registered, no-op
  }
  g_listeners[g_listener_count++] = cb;
  return true;
}

static void notify(uint8_t old_ch, uint8_t new_ch) {
  for (size_t i = 0; i < g_listener_count; i++) {
    g_listeners[i](old_ch, new_ch);
  }
}

// ────────────────────────────────────────────────────────────────────────────
// Radio sampling
// ────────────────────────────────────────────────────────────────────────────

static RadioState sample_radio() {
#if SECURACV_MCP_HAS_ESP32
  RadioState s{};
  // STA: WiFi.status() returns WL_CONNECTED (3) when associated and IP-ready.
  s.sta_connected = (WiFi.status() == WL_CONNECTED);
  s.sta_channel = s.sta_connected ? WiFi.channel() : 0;
  // AP: softAPgetStationNum() > 0 isn't a proof of "AP up", so query the
  // mode bit instead.
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&mode);
  s.ap_active = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
  if (s.ap_active) {
    wifi_config_t cfg{};
    if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK) {
      s.ap_channel = cfg.ap.channel;
    }
  }
  return s;
#else
  return g_test_state;
#endif
}

ChannelDecision current() {
  return decide(sample_radio());
}

void poll_radio() {
  ChannelDecision d = current();
  if (d.channel == g_last_channel) return;

  uint8_t old_ch = g_last_channel;
  g_last_channel = d.channel;

#if SECURACV_MCP_HAS_ESP32
  // If we're in fallback (neither STA nor AP locks us) we are free to retune
  // the radio so peers and chirp converge. When STA or AP is up, we MUST NOT
  // retune — the OS owns the channel.
  if (d.fallback) {
    esp_wifi_set_channel(d.channel, WIFI_SECOND_CHAN_NONE);
  }
#endif

  notify(old_ch, d.channel);
}

void set_state_for_tests(const RadioState& state) {
#if SECURACV_MCP_HAS_ESP32
  (void)state;
#else
  g_test_state = state;
#endif
}

} // namespace mesh_channel_policy
