#include "canary/net/wifi_mgr.h"
#include "canary/log.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_random.h>  // esp_random() for reconnect jitter

#include "canary/config.h"
#include "canary/runtime_config.h"  // NVS-backed credentials (OTA-safe)
#include "identity/device_pseudonym.h"   // MAC-free setup-AP suffix
#include "network/setup_portal.h"        // shared recovery portal (common/)
#include "network/wifi_join_policy.h"  // fleet-wide join/retry rules (common/)

namespace canary::net {

namespace {

// STA supervision state (see wifi_loop). All timestamp math is wrap-safe
// signed-delta, per the firmware idiom for millis() arithmetic.
bool     s_online          = false;
bool     s_configured      = false;
// Have we EVER associated on this power cycle? The outage watchdog may only
// reboot a link that once worked — see common/network/wifi_join_policy.h.
bool     s_ever_online     = false;
uint32_t s_lost_since_ms   = 0;   // start of the current outage
uint32_t s_last_attempt_ms = 0;   // last reconnect attempt
uint32_t s_attempts        = 0;   // consecutive failed attempts this outage
uint32_t s_jitter          = 0;   // re-sampled once per attempt (see wifi_loop)
// Why the most recent attempt failed — feeds wifi_should_open_setup, so a
// wrong password or a renamed SSID raises the setup network instead of
// retrying into the void forever.
canary::net::JoinFailure s_last_failure = canary::net::JoinFailure::Unknown;
uint32_t s_portal_retry_ms = 0;   // last attempt to (re)raise a failed portal

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

// ── The shared setup portal (common/network/setup_portal) ────────────────
// This used to be the "no recovery path but re-flashing" board in
// docs/design/onboarding_shared_module.md — worse, an unseeded unit would
// try to join the literal "ci-placeholder" SSID forever. Now: no
// credentials, or a join that keeps failing for a reason a human can fix,
// raises the device-unique SecuraCV-XXXX setup network; the radar keeps
// witnessing underneath.

bool portal_save(const char* ssid, const char* pass) {
  canary::cfg::set_wifi_credentials(ssid, pass);
  s_configured = true;
  return true;
}

void portal_begin_saved() { begin_sta(); }

void open_setup_portal() {
  char token[device_pseudonym::HEX_LEN + 1] = {0};
  device_pseudonym::device_id_hex(token, sizeof(token));
  canary::net::SetupPortalConfig pc{};
  pc.product_name = "Canary Sense";
  pc.id_suffix = token;
  pc.ap_channel = 1;  // no ESP-NOW band on this board — plain channel 1
  pc.have_saved_credentials = s_configured;
  pc.save_credentials = portal_save;
  pc.begin_saved = portal_begin_saved;
  s_portal_retry_ms = canary::ms_now();
  setup_portal_begin(pc);
}

}  // namespace

void wifi_init_or_reboot(void (*idle_poll)()) {
  const auto& cfg = canary::cfg::get();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // wifi_loop owns retry policy (backoff)
  s_configured = canary::cfg::wifi_credentials_configured();
  if (!s_configured) {
    // Joining the compiled placeholder would probe for an SSID no router has
    // ever announced, forever. Raise the setup network instead — the radar
    // keeps sensing, the tuning console stays live, and a flash-time seed or
    // a phone on the portal is the way onto the network.
    s_lost_since_ms = canary::ms_now();
    log_line("WIFI", "No credentials yet — raising the setup network; sensing continues.");
    open_setup_portal();
    return;
  }
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
      s_last_failure = classify(WiFi.status());
      log_line("WIFI", canary::net::join_failure_detail(s_last_failure));
      s_online = false;
      s_lost_since_ms = canary::ms_now();
      s_last_attempt_ms = s_lost_since_ms;
      s_attempts = 1;
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
  // While the setup portal is up it owns the radio — its wizard join (or
  // quiet retry of the saved network) is the retry policy. Pump it and stand
  // down; it tears itself down after a successful join.
  if (setup_portal_active()) {
    setup_portal_loop(now_ms);
    return;
  }
  if (setup_portal_take_joined()) {
    // Portal handed us a live STA link; adopt it (the CONNECTED branch below
    // marks online/ever_online and logs).
    s_configured = true;
    s_attempts = 0;
  }
  if (!s_configured) {
    // No credentials and no portal: the setup AP failed to start (or was
    // stopped). Keep offering it, gently.
    if ((int32_t)(now_ms - s_portal_retry_ms) >= 30000) open_setup_portal();
    return;
  }
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

  // A never-online link failing for a human-fixable reason (wrong password,
  // renamed SSID) gets the setup network after three attempts — the same
  // shared threshold the display uses. The saved network keeps being retried
  // quietly underneath the portal, so a router that was merely rebooting
  // rejoins with no human involved.
  if (canary::net::wifi_should_open_setup(st, s_last_failure,
                                          /*attempts_before_setup=*/3)) {
    log_line("WIFI", "Join keeps failing for a fixable reason — raising the setup network.");
    open_setup_portal();
    return;
  }

  switch (canary::net::wifi_next_action(retry_policy(), st, now_ms, s_jitter)) {
    case canary::net::WifiAction::Reboot:
      // Only reachable for a link that WAS associated and then dropped, where
      // a reboot plausibly clears a wedged radio or a stale lease.
      log_line("WIFI", "Outage persisted on a link that was working. Rebooting...");
      delay(200);
      ESP.restart();
      break;

    case canary::net::WifiAction::Retry:
      // The status we read here is the LAST attempt's verdict — capture it
      // before begin_sta() resets the radio, so the setup-portal decision
      // above always judges a completed attempt, never a half-started one.
      s_last_failure = classify(WiFi.status());
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

bool wifi_configured() { return s_configured; }

int wifi_rssi() {
  return (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0;
}

} // namespace canary::net
