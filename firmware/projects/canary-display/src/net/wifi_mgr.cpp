#include "canary/net/wifi_mgr.h"
#include "canary/log.h"

#include <WiFi.h>
#include <esp_wifi.h>

#include "canary/config.h"
// AFTER config.h — FEATURE_WATCHDOG lives in the flavor config it pulls in;
// gating above it silently skipped the include and the feed below failed to
// compile (caught by the nightstand7 build).
#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
#include <esp_task_wdt.h>
#endif
#include "canary/runtime_config.h"  // NVS-backed credentials (OTA-safe)
#include "network/wifi_join_policy.h"  // fleet-wide join/retry rules (common/)

#include <esp_random.h>  // esp_random() for reconnect jitter

namespace canary::net {

namespace {

// STA supervision state (see wifi_loop). All timestamp math is wrap-safe
// signed-delta, per the firmware idiom for millis() arithmetic.
bool     s_online          = false;
// Have we EVER associated on this power cycle? The outage watchdog below may
// only reboot a link that once worked. Rebooting because we never got on in
// the first place just repeats the same failed join forever.
bool     s_ever_online     = false;
uint32_t s_lost_since_ms   = 0;   // start of the current outage
uint32_t s_last_attempt_ms = 0;   // last reconnect attempt
uint32_t s_attempts        = 0;   // consecutive failed attempts this outage
uint32_t s_jitter          = 0;   // re-sampled once per attempt (see wifi_loop)
// The failure the last attempt hit, so the glass and the setup fallback can
// both name a cause instead of showing a spinner that never resolves.
canary::net::JoinFailure s_last_failure = canary::net::JoinFailure::Unknown;

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

}  // namespace

// This radio's status -> the fleet's shared vocabulary. The WORDS used to live
// here AND, separately, in provision.cpp's sta_failure_reason, under a comment
// asking a future reader to "keep the two in step" by hand. They had already
// drifted. Both now defer to common/network/wifi_join_policy.h.
static canary::net::JoinFailure classify(wl_status_t st) {
  switch (st) {
    case WL_NO_SSID_AVAIL:  return canary::net::JoinFailure::NotFound;
    case WL_CONNECT_FAILED: return canary::net::JoinFailure::BadPassword;
    case WL_IDLE_STATUS:    return canary::net::JoinFailure::NoAddress;
    default:                return canary::net::JoinFailure::Unknown;
  }
}

// The per-board tunables still win; only the RULES are shared.
static canary::net::WifiRetryPolicy retry_policy() {
  canary::net::WifiRetryPolicy p;
  p.base_ms          = WIFI_RETRY_BASE_MS;
  p.max_ms           = WIFI_RETRY_MAX_MS;
  p.outage_reboot_ms = WIFI_OUTAGE_REBOOT_MS;
  return p;
}

void wifi_init_or_reboot() {
  // Never let esp_wifi persist config to NVS: credentials live in OUR
  // namespace (runtime_config), and with Arduino persistence on (the
  // default) every begin()/mode change commits to flash — a write that
  // stalls the RGB glass's non-IRAM bounce-refill mid-scanout and garbles
  // the panel (the dash-family WiFi-vs-scanout lesson, display_dash.cpp).
  // Once, here: this runs before any STA join, and the flag is process-wide.
  WiFi.persistent(false);
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
    s_ever_online = true;
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
#if defined(FEATURE_WATCHDOG) && FEATURE_WATCHDOG
    // This bounded connect also runs AFTER the wizard re-raises from loop()
    // (wifi_wants_setup), i.e. with the TWDT armed — and its 30 s bound
    // equals the WDT timeout, so an unfed wait panics right at the deadline.
    // A no-op before the WDT is armed (boot path).
    esp_task_wdt_reset();
#endif
    delay(300);
    canary::dbg_serial().print(".");

    if ((canary::ms_now() - start) > WIFI_BOOT_TIMEOUT_MS) {
      canary::dbg_serial().println();
      // Do NOT reboot. Restarting re-runs this exact join against the exact
      // same network with the exact same credentials, so a wrong password, a
      // renamed SSID, or an access point that steers us to a band this radio
      // cannot use became a silent ~30-second reboot cycle with nothing on the
      // glass and no way out. Boot is not the retry policy: wifi_loop already
      // owns retry with backoff, and the rest of the device — screen, touch,
      // the fleet it can still see over ESP-NOW — has no reason to be denied a
      // boot because the uplink is unhappy.
      s_last_failure = classify(WiFi.status());
      log_line("WIFI", canary::net::join_failure_detail(s_last_failure));
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

  s_last_failure = classify(WiFi.status());

  // One shared decision for the whole fleet — see
  // common/network/wifi_join_policy.h, and the host test that proves a link
  // which never associated is never rebooted. That defect is exactly what
  // stranded this board: boot timed out, it rebooted, the identical join
  // failed again, and the onboarding wizard that could have fixed the
  // password never got a chance to appear.
  canary::net::WifiRetry st;
  st.online          = s_online;
  st.ever_online     = s_ever_online;
  st.lost_since_ms   = s_lost_since_ms;
  st.last_attempt_ms = s_last_attempt_ms;
  st.attempts        = s_attempts;

  switch (canary::net::wifi_next_action(retry_policy(), st, now_ms, s_jitter)) {
    case canary::net::WifiAction::Reboot:
      log_line("WIFI", "Outage persisted on a link that was working. Rebooting...");
      delay(200);
      ESP.restart();
      break;

    case canary::net::WifiAction::Retry:
      s_last_attempt_ms = now_ms;
      s_attempts++;
      // Re-sample jitter ONCE per attempt, not once per loop iteration.
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

JoinFailure wifi_last_failure() { return s_last_failure; }

bool wifi_wants_setup() {
  WifiRetry st;
  st.online      = s_online;
  st.ever_online = s_ever_online;
  st.attempts    = s_attempts;
  // Three attempts is roughly 15 s on the shared backoff schedule: long enough
  // that a slow-booting router isn't mistaken for a wrong password, short
  // enough that a person standing in front of the device gets an answer.
  return wifi_should_open_setup(st, s_last_failure, /*attempts_before_setup=*/3);
}

int wifi_rssi() {
  return (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0;
}

} // namespace canary::net
