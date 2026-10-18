// common/network/wifi_supervisor.cpp — the headless Canaries' one Wi-Fi
// supervisor. See wifi_supervisor.h for the contract; this is the I/O.
//
// Compiled directly by each adopting board's build (build_src_filter, beside
// setup_portal.cpp), so it must stay core-2.x AND core-3.x clean. It knows
// nothing of any board's config.h: every tunable arrives in the Config the
// board's shim builds, and the retry RULES are wifi_join_policy.h's — this
// file keeps a WifiRetry and lets wifi_next_action() decide
// (scripts/lint_wifi_join_policy.py holds it to that).
//
// All timestamp math is wrap-safe signed-delta, per the firmware idiom.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <Preferences.h>
#include <string.h>
#include <time.h>

#include "network/wifi_supervisor.h"
#include "network/setup_portal.h"
#include "network/wifi_join_policy.h"
#include "network/wifi_store.h"
#include "time/clock_sync.h"

namespace canary {
namespace net {
namespace wifi_sup {

namespace {

Config s_cfg{};
bool s_begun = false;

// The saved networks (wifi_store.h) and which one is on the air / being tried.
wifi_store::Store s_store;
int s_current = -1;

// STA supervision state (see loop()).
bool     s_online          = false;
bool     s_configured      = false;
// Have we EVER associated on this power cycle? The outage watchdog may only
// reboot a link that once worked — see common/network/wifi_join_policy.h.
bool     s_ever_online     = false;
uint32_t s_lost_since_ms   = 0;   // start of the current outage
uint32_t s_last_attempt_ms = 0;   // last reconnect attempt
uint32_t s_attempts        = 0;   // consecutive failed attempts this outage
uint32_t s_jitter          = 0;   // re-sampled once per attempt (see loop)
uint32_t s_online_since_ms = 0;
// Why the most recent attempt failed — feeds wifi_should_open_setup, so a
// wrong password or a renamed SSID raises the setup network instead of
// retrying into the void forever.
JoinFailure s_last_failure = JoinFailure::Unknown;
uint32_t s_portal_retry_ms = 0;   // last attempt to (re)raise a failed portal
// `s_tap_portal`: a BOOT tap on a witness that already has a network raises
// the portal underneath the Bluetooth door for the tap's minute, because the
// door's join path IS the portal's (setup_portal_submit_join). No quiet
// retry of the saved network under it: that retry would succeed at once and
// tear the portal down before the phone could use the door; the saved link
// stays up underneath (the AP rides the STA's channel), and
// close_tap_portal() lowers the portal when the door shuts.
bool s_tap_portal = false;
bool s_clock_started = false;

void logln(const char* msg) { Serial.printf("[WIFI] %s\n", msg); }

// ── The saved-network ring in NVS ─────────────────────────────────────────
bool save_store() {
  uint8_t blob[wifi_store::BLOB_BYTES];
  if (wifi_store::serialize(s_store, blob, sizeof(blob)) == 0) return false;
  Preferences prefs;
  if (!prefs.begin("securacv", /*readOnly=*/false)) return false;
  prefs.putBytes(wifi_store::NVS_KEY, blob, sizeof(blob));
  // Trust the readback, not the call (the runtime_config rule).
  uint8_t back[wifi_store::BLOB_BYTES];
  const size_t n = prefs.getBytes(wifi_store::NVS_KEY, back, sizeof(back));
  prefs.end();
  return n == sizeof(blob) && memcmp(blob, back, sizeof(blob)) == 0;
}

void load_store() {
  wifi_store::init(s_store);
  Preferences prefs;
  if (prefs.begin("securacv", /*readOnly=*/true)) {
    uint8_t blob[wifi_store::BLOB_BYTES];
    const size_t n = prefs.getBytes(wifi_store::NVS_KEY, blob, sizeof(blob));
    prefs.end();
    if (n == sizeof(blob) && !wifi_store::parse(blob, n, s_store)) {
      logln("Saved-network store unreadable - starting a fresh ring.");
    }
  }
  // The legacy pair (the flashers' seed, a compiled secret, an older
  // firmware's one slot) folds in once; it stays in NVS as the current pick.
  if (s_cfg.seed_configured &&
      wifi_store::import_legacy(s_store, s_cfg.seed_ssid, s_cfg.seed_pass)) {
    if (!save_store()) logln("Store would not hold the saved-network ring - this boot only.");
  }
}

// Keep the legacy pair equal to the network on the air.
void persist_current(int idx) {
  if (idx < 0 || idx >= (int)s_store.count || s_cfg.persist_current == nullptr) return;
  s_cfg.persist_current(s_store.nets[idx].ssid, s_store.nets[idx].pass);
}

// ── Radio ──────────────────────────────────────────────────────────────────
void apply_power_policy() {
  const wifi_ps_type_t ps = s_cfg.power_save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE;
  if (esp_wifi_set_ps(ps) != ESP_OK) {
    logln("Power-save mode set failed.");
  } else if (s_cfg.power_save) {
    logln("Modem sleep enabled (WIFI_PS_MIN_MODEM).");
  }
  if (s_cfg.tx_power_qdbm >= 0) {
    /* esp_wifi_set_max_tx_power() takes quarter-dBm units (int8_t);
     * valid range 8 (2 dBm) .. 84 (21 dBm) — clamp, don't trust config. */
    int8_t q = s_cfg.tx_power_qdbm;
    if (q < 8)  q = 8;
    if (q > 84) q = 84;
    if (esp_wifi_set_max_tx_power(q) == ESP_OK) {
      Serial.printf("[WIFI] TX power capped at %d x0.25 dBm\n", (int)q);
    } else {
      logln("TX power set failed.");
    }
  }
}

uint32_t next_jitter() { return esp_random(); }

// Start a join of ring slot `idx` (the current pick).
void begin_sta_index(int idx) {
  if (idx < 0 || idx >= (int)s_store.count) return;
  s_current = idx;
  WiFi.disconnect();
  WiFi.begin(s_store.nets[idx].ssid, s_store.nets[idx].pass);
}

// The one board-specific line, now shared: this radio's status -> the
// fleet's shared vocabulary.
JoinFailure classify(wl_status_t st) {
  switch (st) {
    case WL_NO_SSID_AVAIL:  return JoinFailure::NotFound;
    case WL_CONNECT_FAILED: return JoinFailure::BadPassword;
    case WL_IDLE_STATUS:    return JoinFailure::NoAddress;
    default:                return JoinFailure::Unknown;
  }
}

// The join / time / self-test step of onboarding_fsm.h's "joining": once
// the address is in, ask the clock (pool, nist, the hub third) — the
// headless boards never did, so every record they dated was "since boot".
void start_clock() {
  if (s_clock_started) return;
  // Disclosed outbound path 3 (docs/security/SECURITY_MODEL.md): SNTP to the
  // two public pools clock_sync.h names, the hub as the third server when one
  // is provisioned. The destinations are named here, not reached through a
  // table, so regression_check.sh's phone-home scan sees exactly these.
  const char* tz = (s_cfg.posix_tz && s_cfg.posix_tz[0]) ? s_cfg.posix_tz : "UTC0";
  configTzTime(tz, time_sync::NTP_PRIMARY, time_sync::NTP_SECONDARY,
               time_sync::hub_server(s_cfg.hub_host));
  s_clock_started = true;
}

void on_online(uint32_t now_ms, const char* how) {
  s_online = true;
  s_ever_online = true;
  s_attempts = 0;
  s_online_since_ms = now_ms;
  // Two literals, not one "%s" — the Lab pages' serial previews are
  // generated from these lines (gen_sense.py, gen_vision.py), and a literal
  // is what their anchors can hold.
  Serial.printf(how[0] == 'R' ? "[WIFI] Reconnected IP=%s RSSI=%ddBm\n"
                              : "[WIFI] Connected IP=%s RSSI=%ddBm\n",
                WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  apply_power_policy();  // power settings do not survive a link cycle
  if (s_current >= 0) {
    const uint8_t was_good = s_store.last_good;
    wifi_store::note_result(s_store, s_current, true);
    if (was_good != s_store.last_good) {
      save_store();
      persist_current(s_current);
    }
  }
  start_clock();
}

// ── The shared setup portal (common/network/setup_portal) ────────────────
bool portal_save(const char* ssid, const char* pass) {
  // A join through the portal (page or Bluetooth door) that WORKED: into the
  // ring as last good, and the legacy pair in step. The durable verdict
  // flows back to the portal, which logs a store refusal; the session still
  // runs from RAM either way.
  bool durable = wifi_store::add(s_store, ssid, pass);
  if (durable) durable = save_store();
  s_current = wifi_store::find(s_store, ssid);
  if (s_cfg.persist_current != nullptr) {
    durable = s_cfg.persist_current(ssid, pass) && durable;
  }
  s_configured = s_store.count > 0;
  return durable;
}

void portal_begin_saved() { begin_sta_index(wifi_store::pick(s_store, nullptr, nullptr, 0, -1)); }

void open_setup_portal(bool for_tap = false) {
  SetupPortalConfig pc{};
  pc.product_name = s_cfg.product_name;
  pc.node_type = s_cfg.node_type;
  pc.id_suffix = s_cfg.id_suffix;
  pc.ap_channel = s_cfg.ap_channel ? s_cfg.ap_channel : 1;
  pc.have_saved_credentials = s_configured;
  pc.save_credentials = portal_save;
  pc.begin_saved = for_tap ? nullptr : portal_begin_saved;
  pc.save_identity = s_cfg.save_identity;
  pc.keep_sta_link = for_tap;   // the live link is ours; a join through the portal replaces it
  s_portal_retry_ms = millis();
  if (setup_portal_begin(pc) && for_tap) s_tap_portal = true;
}

WifiRetry retry_state() {
  WifiRetry st;
  st.online          = s_online;
  st.ever_online     = s_ever_online;
  st.lost_since_ms   = s_lost_since_ms;
  st.last_attempt_ms = s_last_attempt_ms;
  st.attempts        = s_attempts;
  return st;
}

}  // namespace

void begin(const Config& cfg, void (*idle_poll)()) {
  s_cfg = cfg;
  s_begun = true;
  // Never let esp_wifi persist config to NVS: credentials live in OUR
  // namespace (the ring + the legacy pair), and with Arduino persistence on
  // (the default) every begin()/mode change commits to flash for nothing.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // loop() owns retry policy (backoff)
  load_store();
  s_configured = s_store.count > 0;
  if (!s_configured) {
    // Joining a compiled placeholder would probe for an SSID no router has
    // ever announced, forever. Raise the setup network instead — sensing
    // continues, and a flash-time seed, the Bluetooth door or a phone on the
    // portal is the way onto the network.
    s_lost_since_ms = millis();
    logln("No saved network yet - raising the setup network; sensing continues.");
    open_setup_portal();
    return;
  }
  begin_sta_index(wifi_store::pick(s_store, nullptr, nullptr, 0, -1));
  Serial.printf("[WIFI] Connecting SSID=\"%s\" ...\n", s_store.nets[s_current].ssid);
  if (s_store.count > 1)
    Serial.printf("[WIFI] %u saved networks; the last good one goes first\n",
                  (unsigned)s_store.count);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (idle_poll) idle_poll();  // keep the cable-side bench alive meanwhile
    if ((millis() - start) > s_cfg.boot_timeout_ms) {
      Serial.println();
      // Do NOT reboot. A reboot re-runs this exact join against the exact
      // same network with the exact same credentials, so a wrong password, a
      // renamed SSID, or a 5 GHz-only AP this radio cannot see becomes a
      // silent ~30-second reboot cycle with no way out and nothing to look
      // at. Boot is not the retry policy — loop() owns retry with backoff,
      // and the sensor has no reason to be denied a boot because the uplink
      // is unhappy. (The defect that stranded the 4-inch display; the rule
      // lives in common/, once.)
      s_last_failure = classify(WiFi.status());
      logln(join_failure_detail(s_last_failure));
      s_online = false;
      s_lost_since_ms = millis();
      s_last_attempt_ms = s_lost_since_ms;
      s_attempts = 1;
      wifi_store::note_result(s_store, s_current, false);
      return;
    }
  }
  Serial.println();
  on_online(millis(), "Connected");
}

void loop(uint32_t now_ms) {
  if (!s_begun) return;
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
    s_configured = s_store.count > 0;
    s_attempts = 0;
    s_tap_portal = false;
  }
  if (!s_configured) {
    // No credentials and no portal: the setup AP failed to start (or was
    // stopped). Keep offering it, gently.
    if ((int32_t)(now_ms - s_portal_retry_ms) >= 30000) open_setup_portal();
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    if (!s_online) on_online(now_ms, "Reconnected");
    return;
  }

  if (s_online) {
    // Link just dropped: start the outage clock and kick an immediate retry.
    s_online = false;
    s_lost_since_ms = now_ms;
    s_last_attempt_ms = now_ms;
    s_attempts = 1;
    logln("Link lost. Reconnecting...");
    begin_sta_index(s_current >= 0 ? s_current : wifi_store::pick(s_store, nullptr, nullptr, 0, -1));
    return;
  }

  // One shared decision for the whole fleet — see
  // common/network/wifi_join_policy.h, and the host test that proves a link
  // which never associated is never rebooted.
  const WifiRetry st = retry_state();

  // A never-online link failing for a human-fixable reason (wrong password,
  // renamed SSID) gets the setup network after three attempts — the same
  // shared threshold the display uses. The saved networks keep being
  // retried quietly underneath the portal, so a router that was merely
  // rebooting rejoins with no human involved.
  if (wifi_should_open_setup(st, s_last_failure, /*attempts_before_setup=*/3)) {
    logln("Join keeps failing for a fixable reason - raising the setup network.");
    open_setup_portal();
    return;
  }
  // The other half of recovery (onboarding_fsm.h): a node with saved
  // networks that has been offline ten minutes — whatever the reason, and
  // however often it was online before — re-raises its setup network WITHOUT
  // wiping anything. A moved house, a renamed SSID or a changed password is
  // a human problem a headless node can only hand back to a human; the saved
  // networks keep being retried underneath.
  if (onboard::recovery_reraise_due(now_ms, s_configured, s_online,
                                    setup_portal_active(), s_ever_online,
                                    s_lost_since_ms, onboard::Timing{})) {
    logln("Offline ten minutes with saved networks - raising the setup network (nothing wiped).");
    open_setup_portal();
    return;
  }

  switch (wifi_next_action(s_cfg.retry, st, now_ms, s_jitter)) {
    case WifiAction::Reboot:
      // Only reachable for a link that WAS associated and then dropped, where
      // a reboot plausibly clears a wedged radio or a stale lease.
      logln("Outage persisted on a link that was working. Rebooting...");
      delay(200);
      ESP.restart();
      break;

    case WifiAction::Retry: {
      // The status we read here is the LAST attempt's verdict — capture it
      // before begin() resets the radio, so the setup-portal decision above
      // always judges a completed attempt, never a half-started one.
      s_last_failure = classify(WiFi.status());
      wifi_store::note_result(s_store, s_current, false);
      s_last_attempt_ms = now_ms;
      s_attempts++;
      // Re-sample jitter ONCE per attempt, not once per loop iteration (the
      // old code re-rolled it every pass, which collapsed toward zero).
      s_jitter = next_jitter();
      // Several saved networks: a NotFound on this one moves on to the next
      // (the house with a main and a guest network, the node carried to the
      // workshop). A wrong password stays put — that is the setup rule's.
      int idx = s_current;
      if (s_last_failure == JoinFailure::NotFound && s_store.count > 1) {
        idx = wifi_store::pick(s_store, nullptr, nullptr, 0, s_current);
      }
      Serial.printf("[WIFI] Reconnect attempt %lu (\"%s\") ...\n",
                    (unsigned long)s_attempts,
                    idx >= 0 ? s_store.nets[idx].ssid : "");
      begin_sta_index(idx);
      break;
    }

    case WifiAction::Wait:
      break;
  }
}

void open_tap_portal() {
  if (setup_portal_active()) return;   // the first-boot or recovery portal is already the door's path
  logln("BOOT tap - raising the setup network under the Bluetooth door for a minute.");
  open_setup_portal(/*for_tap=*/true);
}

void close_tap_portal() {
  if (!s_tap_portal) return;
  if (!setup_portal_active()) { s_tap_portal = false; return; }   // it tore itself down (a join)
  // A join in flight is owed its verdict, and a succeeded one its linger
  // and its own teardown (which hands the new link to loop()).
  if (setup_portal_join_in_flight() ||
      setup_portal_join_state() == SetupPortalJoin::Success) return;
  setup_portal_stop();
  s_tap_portal = false;
  logln("Tap window over - setup network lowered.");
  if (WiFi.status() != WL_CONNECTED) {
    // The saved link did not survive the mode flips: retry it now, and let
    // loop()'s policy take over from there.
    const uint32_t now = millis();
    s_online = false;
    s_lost_since_ms = now;
    s_last_attempt_ms = now;
    s_attempts = 1;
    begin_sta_index(s_current >= 0 ? s_current : wifi_store::pick(s_store, nullptr, nullptr, 0, -1));
  }
}

bool tap_portal_open() { return s_tap_portal && setup_portal_active(); }

bool connected() { return WiFi.status() == WL_CONNECTED; }

bool configured() { return s_configured; }

int rssi() { return (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0; }

onboard::State state() {
  onboard::Facts f;
  f.have_credentials = s_configured;
  f.ap_up = setup_portal_active();
  f.join_in_flight = setup_portal_join_in_flight();
  f.sta_online = WiFi.status() == WL_CONNECTED;
  f.credentials_submitted = setup_portal_credentials_submitted();
  return onboard::derive(f);
}

bool ap_up() { return setup_portal_active(); }

int saved_count() { return (int)s_store.count; }

time_sync::Source clock_source() {
  if (!s_clock_started) return time_sync::Source::None;
  const time_t now = time(nullptr);
  return time_sync::believable((int64_t)now) ? time_sync::Source::Ntp : time_sync::Source::None;
}

bool ever_online() { return s_ever_online; }

bool credentials_submitted() { return setup_portal_credentials_submitted(); }

bool current_network(const char** ssid, const char** pass) {
  if (s_current < 0 || s_current >= (int)s_store.count) return false;
  if (ssid) *ssid = s_store.nets[s_current].ssid;
  if (pass) *pass = s_store.nets[s_current].pass;
  return true;
}

bool forget_all() {
  wifi_store::init(s_store);
  s_current = -1;
  s_configured = false;
  bool gone = true;
  Preferences prefs;
  if (prefs.begin("securacv", /*readOnly=*/false)) {
    prefs.remove(wifi_store::NVS_KEY);
    prefs.remove("ap_pass");
    // Trust the readback: a remove that silently failed is exactly the case
    // where rebooting into "setup will open" would be a lie.
    gone = !prefs.isKey(wifi_store::NVS_KEY) && !prefs.isKey("ap_pass");
    prefs.end();
  } else {
    gone = false;
  }
  if (s_cfg.persist_current != nullptr) gone = s_cfg.persist_current("", "") && gone;
  logln(gone ? "Saved networks forgotten - setup opens on next boot."
             : "Store would not release the saved networks.");
  return gone;
}

}  // namespace wifi_sup
}  // namespace net
}  // namespace canary
