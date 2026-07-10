#include "canary/net/wifi_mgr.h"
#include "canary/log.h"

#include <WiFi.h>
#include <esp_wifi.h>

#include "canary/config.h"
#include "canary/runtime_config.h"  // NVS-backed credentials (OTA-safe)

namespace canary::net {

namespace {

// STA supervision state (see wifi_loop). All timestamp math is wrap-safe
// signed-delta, per the firmware idiom for millis() arithmetic.
bool     s_online          = false;
uint32_t s_lost_since_ms   = 0;   // start of the current outage
uint32_t s_last_attempt_ms = 0;   // last reconnect attempt
uint32_t s_attempts        = 0;   // consecutive failed attempts this outage

// Modem sleep + TX power cap, from canary/config.h. Same semantics as the
// ESP32-S3 tree's network_set_wifi_power_save / network_set_tx_power.
void apply_power_policy() {
  const wifi_ps_type_t ps = WIFI_POWER_SAVE ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE;
  if (esp_wifi_set_ps(ps) != ESP_OK) {
    log_line("WIFI", "Power-save mode set failed.");
  } else if (WIFI_POWER_SAVE) {
    log_line("WIFI", "Modem sleep enabled (WIFI_PS_MIN_MODEM).");
  }

  if (WIFI_TX_POWER_QDBM >= 0) {
    /* esp_wifi_set_max_tx_power() takes quarter-dBm units (int8_t);
     * valid range 8 (2 dBm) .. 84 (21 dBm) — clamp, don't trust config. */
    int8_t q = WIFI_TX_POWER_QDBM;
    if (q < 8)  q = 8;
    if (q > 84) q = 84;
    if (esp_wifi_set_max_tx_power(q) == ESP_OK) {
      log_header("WIFI");
      canary::dbg_serial().printf("TX power capped at %d x0.25 dBm\n", (int)q);
    } else {
      log_line("WIFI", "TX power set failed.");
    }
  }
}

void begin_sta() {
  const auto& cfg = canary::cfg::get();
  WiFi.disconnect();
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
}

}  // namespace

void wifi_init_or_reboot() {
  // Already associated — the onboarding wizard just joined the network and
  // handed over a live link. Adopt it instead of bouncing the connection.
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setAutoReconnect(false);  // wifi_loop owns retry policy (backoff)
    log_header("WIFI");
    canary::dbg_serial().printf(
        "Adopting provisioned link IP=%s RSSI=%ddBm\n",
        WiFi.localIP().toString().c_str(), WiFi.RSSI());
    apply_power_policy();
    s_online = true;
    return;
  }

  const auto& cfg = canary::cfg::get();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // wifi_loop owns retry policy (backoff)
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);

  log_header("WIFI");
  canary::dbg_serial().printf("Connecting SSID=\"%s\" ...\n", cfg.wifi_ssid);

  const uint32_t start = canary::ms_now();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    canary::dbg_serial().print(".");

    if ((canary::ms_now() - start) > WIFI_BOOT_TIMEOUT_MS) {
      canary::dbg_serial().println();
      log_line("WIFI", "Timeout. Rebooting...");
      delay(200);
      ESP.restart();
    }
  }

  canary::dbg_serial().println();
  log_header("WIFI");
  canary::dbg_serial().printf(
    "Connected IP=%s RSSI=%ddBm\n",
    WiFi.localIP().toString().c_str(),
    WiFi.RSSI()
  );

  apply_power_policy();
  s_online = true;
}

void wifi_loop(uint32_t now_ms) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!s_online) {
      s_online = true;
      s_attempts = 0;
      log_header("WIFI");
      canary::dbg_serial().printf(
        "Reconnected IP=%s RSSI=%ddBm\n",
        WiFi.localIP().toString().c_str(),
        WiFi.RSSI()
      );
      apply_power_policy();  // power settings do not survive a link cycle
    }
    return;
  }

  if (s_online) {
    // Link just dropped: start the outage clock and kick an immediate retry.
    s_online = false;
    s_lost_since_ms = now_ms;
    s_last_attempt_ms = now_ms;
    s_attempts = 1;
    log_line("WIFI", "Link lost. Reconnecting...");
    begin_sta();
    return;
  }

  // Sustained outage: reboot as a last resort — same recovery philosophy as
  // the boot-time timeout above.
  if ((int32_t)(now_ms - s_lost_since_ms) >= (int32_t)WIFI_OUTAGE_REBOOT_MS) {
    log_line("WIFI", "Outage persisted. Rebooting...");
    delay(200);
    ESP.restart();
  }

  /* Exponential backoff: 2 s → 4 s → 8 s → 16 s → 30 s cap. `s_attempts`
   * counts consecutive failures this outage; a reconnect resets it. Same
   * schedule as the S3 tree's WIFI_PROV_FAILED branch. */
  uint32_t attempt = s_attempts;
  if (attempt > 5) attempt = 5;
  uint32_t backoff_ms = WIFI_RETRY_BASE_MS << (attempt > 0 ? (attempt - 1) : 0);
  if (backoff_ms > WIFI_RETRY_MAX_MS) backoff_ms = WIFI_RETRY_MAX_MS;

  if ((int32_t)(now_ms - s_last_attempt_ms) >= (int32_t)backoff_ms) {
    s_last_attempt_ms = now_ms;
    s_attempts++;
    log_header("WIFI");
    canary::dbg_serial().printf("Reconnect attempt %lu ...\n",
                                (unsigned long)s_attempts);
    begin_sta();
  }
}

bool wifi_connected() { return WiFi.status() == WL_CONNECTED; }

int wifi_rssi() {
  return (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0;
}

} // namespace canary::net
