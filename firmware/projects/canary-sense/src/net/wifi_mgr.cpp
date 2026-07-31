#include "canary/net/wifi_mgr.h"
#include "canary/log.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_random.h>  // esp_random() for reconnect jitter

#include "canary/config.h"
#include "canary/runtime_config.h"  // NVS-backed credentials (OTA-safe)
#include "network/wifi_join_policy.h"  // fleet-wide join/retry rules (common/)

namespace canary::net {

namespace {

// STA supervision state (see wifi_loop). All timestamp math is wrap-safe
// signed-delta, per the firmware idiom for millis() arithmetic.
bool     s_online          = false;
// Have we EVER associated on this power cycle? The outage watchdog may only
// reboot a link that once worked — see common/network/wifi_join_policy.h.
bool     s_ever_online     = false;
uint32_t s_lost_since_ms   = 0;   // start of the current outage
uint32_t s_last_attempt_ms = 0;   // last reconnect attempt
uint32_t s_attempts        = 0;   // consecutive failed attempts this outage
uint32_t s_jitter          = 0;   // re-sampled once per attempt (see wifi_loop)

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

uint32_t next_jitter() { return esp_random(); }

void begin_sta() {
  const auto& cfg = canary::cfg::get();
  WiFi.disconnect();
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
}


// The one board-specific line: this radio's status -> the fleet's shared
// vocabulary. Everything downstream (the words, the retry schedule, whether a
// reboot could possibly help) lives in common/network/wifi_join_policy.h so
// three boards cannot drift apart again.
canary::net::JoinFailure classify(wl_status_t st) {
  switch (st) {
    case WL_NO_SSID_AVAIL:  return canary::net::JoinFailure::NotFound;
    case WL_CONNECT_FAILED: return canary::net::JoinFailure::BadPassword;
    case WL_IDLE_STATUS:    return canary::net::JoinFailure::NoAddress;
    default:                return canary::net::JoinFailure::Unknown;
  }
}

// Defaults come from the shared policy; the per-board config still wins where
// it is set, so a board with a different outage tolerance keeps it.
canary::net::WifiRetryPolicy retry_policy() {
  canary::net::WifiRetryPolicy p;
  p.base_ms          = WIFI_RETRY_BASE_MS;
  p.max_ms           = WIFI_RETRY_MAX_MS;
  p.outage_reboot_ms = WIFI_OUTAGE_REBOOT_MS;
  return p;
}

}  // namespace

void wifi_init_or_reboot(void (*idle_poll)()) {
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
    if (idle_poll) idle_poll();  // keep the cable-side bench alive meanwhile

    if ((canary::ms_now() - start) > WIFI_BOOT_TIMEOUT_MS) {
      canary::dbg_serial().println();
      // Do NOT reboot. A reboot re-runs this exact join against the exact
      // same network with the exact same credentials, so a wrong password, a
      // renamed SSID, or a 5 GHz-only AP this radio cannot see becomes a
      // silent ~30-second reboot cycle with no way out and nothing to look
      // at. Boot is not the retry policy — wifi_loop owns retry with backoff,
      // and the sensor has no reason to be denied a boot because the uplink
      // is unhappy. (This is the same defect that stranded the 4-inch
      // display; the rule now lives in common/, once.)
      log_line("WIFI", canary::net::join_failure_detail(classify(WiFi.status())));
      s_online = false;
      return;
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
  s_ever_online = true;
}

void wifi_loop(uint32_t now_ms) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!s_online) {
      s_online = true;
      s_ever_online = true;
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

  // One shared decision for the whole fleet — see
  // common/network/wifi_join_policy.h, and the host test that proves a link
  // which never associated is never rebooted.
  canary::net::WifiRetry st;
  st.online          = s_online;
  st.ever_online     = s_ever_online;
  st.lost_since_ms   = s_lost_since_ms;
  st.last_attempt_ms = s_last_attempt_ms;
  st.attempts        = s_attempts;

  switch (canary::net::wifi_next_action(retry_policy(), st, now_ms, s_jitter)) {
    case canary::net::WifiAction::Reboot:
      // Only reachable for a link that WAS associated and then dropped, where
      // a reboot plausibly clears a wedged radio or a stale lease.
      log_line("WIFI", "Outage persisted on a link that was working. Rebooting...");
      delay(200);
      ESP.restart();
      break;

    case canary::net::WifiAction::Retry:
      s_last_attempt_ms = now_ms;
      s_attempts++;
      // Re-sample jitter ONCE per attempt, not once per loop iteration. The
      // old code re-rolled it on every pass, so the effective deadline was the
      // minimum of hundreds of samples — which collapses toward zero jitter and
      // quietly defeats the decorrelation it was added for.
      s_jitter = next_jitter();
      log_header("WIFI");
      canary::dbg_serial().printf("Reconnect attempt %lu ...\n",
                                  (unsigned long)s_attempts);
      begin_sta();
      break;

    case canary::net::WifiAction::Wait:
      break;
  }
}


bool wifi_connected() { return WiFi.status() == WL_CONNECTED; }

int wifi_rssi() {
  return (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0;
}

} // namespace canary::net
