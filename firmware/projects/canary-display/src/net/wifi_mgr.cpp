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
#include "canary/runtime_config.h"  // NVS-backed credentials + the saved-network ring (OTA-safe)
#include "network/wifi_join_policy.h"  // fleet-wide join/retry rules (common/)
#include "network/onboarding_fsm.h"    // recovery re-raise + the state every surface reports (common/)
#include "canary/net/provision.h"      // the portal's facts: setup AP up, a phone's join in flight

#include <esp_random.h>  // esp_random() for reconnect jitter

namespace canary::net {

namespace {

// STA supervision state (see wifi_loop). All timestamp math is wrap-safe
// signed-delta, per the firmware idiom for millis() arithmetic.
bool     s_online          = false;
// Which saved network (runtime_config's ring slot) is on the air or being
// tried; -1 = the legacy pair alone (an empty ring, or a link the portal
// just handed over whose slot the next pick names).
int      s_current         = -1;
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

// Start a join of ring slot `idx`; the legacy pair when the ring is empty
// (an NVS that would not hold the ring still holds the pair the flashers
// seed, and that pair is the network).
void begin_sta_index(int idx) {
  WiFi.disconnect();
  if (idx >= 0) {
    s_current = idx;
    WiFi.begin(canary::cfg::wifi_saved_ssid(idx), canary::cfg::wifi_saved_pass(idx));
    return;
  }
  s_current = -1;
  const auto& cfg = canary::cfg::get();
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
}

// The network to try first: the one on the air, else the ring's last good.
int first_pick() {
  return s_current >= 0 ? s_current : canary::cfg::wifi_saved_pick(-1);
}

void begin_sta() { begin_sta_index(first_pick()); }

// The setup portal's facts, for the recovery rule and the onboarding word.
// Defined only in FEATURE_ONBOARDING builds (provision.h); without the
// portal there is no AP to be up and no phone to hand credentials.
bool portal_up() {
#if defined(FEATURE_ONBOARDING) && FEATURE_ONBOARDING
  return provision_active();
#else
  return false;
#endif
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
    // The wizard's network is the ring's last good already (set_wifi_credentials);
    // the next pick names its slot.
    s_current = -1;
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // wifi_loop owns retry policy (backoff)
  begin_sta_index(canary::cfg::wifi_saved_pick(-1));

  log_header("WIFI");
  canary::dbg_serial().printf("Connecting SSID=\"%s\" (%d saved) ...\n",
                              s_current >= 0 ? canary::cfg::wifi_saved_ssid(s_current)
                                             : canary::cfg::get().wifi_ssid,
                              canary::cfg::wifi_saved_networks());

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
      // The outage starts here, for the recovery rule (wifi_wants_setup):
      // a unit that booted into a network that is gone is offline from its
      // first failed join, not from some later drop it never had.
      s_lost_since_ms = canary::ms_now();
      canary::cfg::wifi_saved_note_result(s_current, false);
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
  canary::cfg::wifi_saved_note_result(s_current, true);
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
      // The network that answered becomes the ring's last good (a rotation
      // that landed on the guest network persists it as the one to try first).
      canary::cfg::wifi_saved_note_result(s_current, true);
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

    case canary::net::WifiAction::Retry: {
      canary::cfg::wifi_saved_note_result(s_current, false);
      s_last_attempt_ms = now_ms;
      s_attempts++;
      // Re-sample jitter ONCE per attempt, not once per loop iteration.
      s_jitter = next_jitter();
      // Several saved networks: a NotFound on this one moves on to the next
      // (the house with a main and a guest network, the glass carried to the
      // workshop). A wrong password stays put — that is the setup rule's.
      int idx = first_pick();
      if (s_last_failure == canary::net::JoinFailure::NotFound &&
          canary::cfg::wifi_saved_networks() > 1) {
        idx = canary::cfg::wifi_saved_pick(idx);
      }
      log_header("WIFI");
      canary::dbg_serial().printf("Reconnect attempt %lu (\"%s\") ...\n",
                                  (unsigned long)s_attempts,
                                  idx >= 0 ? canary::cfg::wifi_saved_ssid(idx)
                                           : canary::cfg::get().wifi_ssid);
      begin_sta_index(idx);
      break;
    }

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
  if (wifi_should_open_setup(st, s_last_failure, /*attempts_before_setup=*/3)) {
    return true;
  }
  // The other half of recovery (onboarding_fsm.h): saved networks, offline
  // ten minutes, whatever the reason — raise the wizard over them, wiping
  // nothing. The portal retries them underneath; the outage clock restarts
  // when the boot join after the wizard gives up again, so this cannot spin.
  const bool have_credentials = !canary::cfg::wifi_is_placeholder();
  return onboard::recovery_reraise_due(canary::ms_now(), have_credentials,
                                       s_online, portal_up(), s_ever_online,
                                       s_lost_since_ms, onboard::Timing{});
}

onboard::State wifi_onboarding_state() {
  onboard::Facts f;
  f.have_credentials = !canary::cfg::wifi_is_placeholder();
  f.sta_online = WiFi.status() == WL_CONNECTED;
  f.ap_up = portal_up();
#if defined(FEATURE_ONBOARDING) && FEATURE_ONBOARDING
  f.join_in_flight = provision_join_in_flight();
  f.credentials_submitted = provision_credentials_submitted();
#endif
  return onboard::derive(f);
}

int wifi_rssi() {
  return (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0;
}

} // namespace canary::net
