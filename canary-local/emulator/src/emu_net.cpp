// canary-local/emulator/src/emu_net.cpp — the LAN as a scenario.
//
// Replaces the firmware's ESP-specific net TUs (wifi_mgr, tz_auto,
// ota_mgr, discovery, chirp_scan, provision) with implementations of the
// same canary::net contracts, driven by page-side switches. The
// semantics stay honest to the originals: a display that boots with no
// reachable Wi-Fi finishes booting and the loop owns the retry (never a
// reboot loop), while a link that once worked and stays down for
// WIFI_OUTAGE_REBOOT_MS does reboot, because watching that happen (and
// reading the serial log while it does) is exactly what canary.local is for.
//
// The Wi-Fi decisions are NOT re-implemented here. The shim keeps one
// canary::net::WifiRetry and asks the same common/network/wifi_join_policy.h
// the display's wifi_mgr.cpp asks — the identical header, reached through the
// same -I firmware/common, with a WifiRetryPolicy built from the same
// canary/config.h constants — so the page previews the firmware's own
// schedule (2 s → 4 s → 8 s … with jitter) and its one hard rule: a reboot
// only ever for a link that has been associated at least once. A fourth,
// hand-written copy of that rule used to live in this file under a comment
// calling it "the same as glass"; it had already drifted — it booted
// believing the link had once been up, so a device started with Wi-Fi off
// would have rebooted after five minutes, the one thing the rule forbids.
// scripts/lint_wifi_join_policy.py now holds this file to the header.
//
// mqtt_mgr.cpp is NOT here: the real one compiles against the PubSubClient
// shim in emu_mqtt.cpp.
#include <Arduino.h>

#include <emscripten.h>
#include <esp_random.h>  // esp_random() for reconnect jitter (the shim's — page entropy, seedable)
#include <string.h>
#include <stdio.h>

#include "canary/config.h"
#include "canary/version.h"
#include "canary/log.h"
#include "canary/net/wifi_mgr.h"
#include "canary/net/tz_auto.h"
#include "canary/net/ota_mgr.h"
#include "canary/net/discovery.h"
#include "canary/net/chirp_scan.h"
#include "canary/net/provision.h"
#include "canary/net/mqtt_mgr.h"
#include "canary/net/glass_web.h"
#include "network/wifi_join_policy.h"  // fleet-wide join/retry rules (common/)

#include "emu_bus.h"

// ── Scenario state (set from JS via emu_bindings.cpp) ───────────────────
namespace {
volatile int g_wifi_up = 1;
volatile int g_rssi = -52;
volatile int g_broker_up = 1;

// The shared policy's live state. The shim owns the storage and the header
// owns the rules — the same split wifi_mgr.cpp makes. ever_online starts
// false exactly as on silicon: an association sets it, nothing assumes it. So
// a visitor who flips Wi-Fi off before boot gets the firmware's answer (boot
// completes, the loop retries forever, no reboot), not a five-minute timer.
canary::net::WifiRetry g_retry;
uint32_t g_jitter = 0;  // re-sampled once per attempt, as wifi_mgr.cpp does

char g_tz_from_page[64] = {0};   // browser's zone, POSIX form
bool g_tz_applied = false;

char g_referral_host[64] = {0};  // mDNS fleet-gossip broker referral
uint16_t g_referral_port = 1883;

EM_JS(void, js_net_event, (const char* kind, const char* detail), {
  if (Module.onNetEvent) Module.onNetEvent(UTF8ToString(kind), UTF8ToString(detail));
});

// The per-board tunables still win; only the RULES are shared. These are the
// display's own numbers, read from the same canary/config.h its wifi_mgr.cpp
// reads, so the emulator cannot quietly run a different schedule from the
// glass it previews. The lint checks that the two assignments name the same
// symbols.
canary::net::WifiRetryPolicy retry_policy() {
  canary::net::WifiRetryPolicy p;
  p.base_ms          = WIFI_RETRY_BASE_MS;
  p.max_ms           = WIFI_RETRY_MAX_MS;
  p.outage_reboot_ms = WIFI_OUTAGE_REBOOT_MS;
  return p;
}
}  // namespace

extern "C" {
int emu_bus_wifi_up(void) { return g_wifi_up; }
int emu_bus_wifi_rssi(void) { return g_rssi; }
int emu_bus_broker_up(void) { return g_broker_up; }

EMSCRIPTEN_KEEPALIVE void emu_set_wifi(int up, int rssi_dbm) {
  g_wifi_up = up;
  g_rssi = rssi_dbm;
}
EMSCRIPTEN_KEEPALIVE void emu_set_broker(int up) { g_broker_up = up; }
EMSCRIPTEN_KEEPALIVE void emu_set_tz(const char* posix_tz) {
  strncpy(g_tz_from_page, posix_tz ? posix_tz : "", sizeof(g_tz_from_page) - 1);
}
EMSCRIPTEN_KEEPALIVE void emu_set_broker_referral(const char* host, int port) {
  strncpy(g_referral_host, host ? host : "", sizeof(g_referral_host) - 1);
  g_referral_port = (uint16_t)port;
}
}

namespace canary::net {

// ── wifi_mgr contract ───────────────────────────────────────────────────
void wifi_init_or_reboot() {
  log_line("WiFi", "Connecting (emulated STA)...");
  const uint32_t t0 = millis();
  while (!g_wifi_up) {
    if ((int32_t)(millis() - t0) >= (int32_t)WIFI_BOOT_TIMEOUT_MS) {
      // Same as glass: no reboot. Rebooting on a join that has never
      // succeeded re-runs the identical join forever, so the device never
      // finishes booting and the setup wizard that could fix the credentials
      // never appears. Boot completes; wifi_loop owns retry, through the
      // shared policy, with ever_online still false.
      log_line("WiFi",
               canary::net::join_failure_detail(canary::net::JoinFailure::Unknown));
      js_net_event("wifi-boot-timeout", "");
      g_retry.online = false;
      return;
    }
    delay(250);
  }
  log_line("WiFi", "Connected (emulated).");
  g_retry.online = true;
  g_retry.ever_online = true;
  js_net_event("wifi-up", "");
}

void wifi_loop(uint32_t now_ms) {
  if (g_wifi_up) {
    if (!g_retry.online) {
      g_retry.online = true;
      g_retry.ever_online = true;
      g_retry.attempts = 0;
      log_line("WiFi", "Reconnected (emulated).");
      js_net_event("wifi-up", "reconnected");
    }
    return;
  }

  if (g_retry.online) {
    // Link just dropped: start the outage clock and count the immediate retry
    // the firmware kicks off here (its begin_sta(); the page's switch is the
    // only radio there is).
    g_retry.online = false;
    g_retry.lost_since_ms = now_ms;
    g_retry.last_attempt_ms = now_ms;
    g_retry.attempts = 1;
    log_line("WiFi", "Link lost. Reconnecting...");
    js_net_event("wifi-down", "");
    return;
  }

  // One shared decision for the whole fleet — see
  // common/network/wifi_join_policy.h and firmware/tests_host/
  // test_wifi_join_policy.cpp. The policy only ever returns Reboot for a link
  // that has been online, so the boot-timeout path above (ever_online false)
  // can wait here forever and never reboot — which is the point.
  char msg[48];
  switch (canary::net::wifi_next_action(retry_policy(), g_retry, now_ms, g_jitter)) {
    case canary::net::WifiAction::Reboot:
      log_line("WiFi", "Outage persisted on a link that was working. Rebooting...");
      js_net_event("wifi-outage-reboot", "");
      delay(200);
      ESP.restart();
      break;

    case canary::net::WifiAction::Retry:
      g_retry.last_attempt_ms = now_ms;
      g_retry.attempts++;
      // Re-sample jitter ONCE per attempt, not once per loop iteration.
      g_jitter = esp_random();
      snprintf(msg, sizeof(msg), "Reconnect attempt %lu ...",
               (unsigned long)g_retry.attempts);
      log_line("WiFi", msg);
      // Nothing to poke: the attempt succeeds iff the page's switch is up,
      // which the next pass observes. The wire log still shows the cadence.
      snprintf(msg, sizeof(msg), "attempt %lu", (unsigned long)g_retry.attempts);
      js_net_event("wifi-retry", msg);
      break;

    case canary::net::WifiAction::Wait:
      break;
  }
}

bool wifi_connected() { return g_wifi_up != 0; }
int wifi_rssi() { return g_wifi_up ? g_rssi : 0; }

// The emulator's uplink is a toggle on the page, not a radio, so there is no
// authentic failure cause to report. Unknown is the honest answer: it renders
// "Couldn't connect" on the glass rather than inventing a wrong password the
// visitor never typed.
JoinFailure wifi_last_failure() { return JoinFailure::Unknown; }

// The setup fallback exists for credentials that are set but WRONG. The
// emulator has no credentials to be wrong, and raising a SoftAP wizard in a
// browser tab would be theater, so this is honestly false. (The shared rule
// agrees: wifi_should_open_setup() never opens setup for JoinFailure::Unknown,
// which is all this shim can ever report.)
bool wifi_wants_setup() { return false; }

// ── tz_auto contract ────────────────────────────────────────────────────
// The page hands over the browser's own zone; "learning" it once Wi-Fi is
// up mirrors the real module's flow (fetch → apply → remember) without
// any network fetch — the emulator's household is the visitor's.
void tz_boot_string(const char* seed, char* out, unsigned cap) {
  const char* src = g_tz_from_page[0] ? g_tz_from_page : seed;
  snprintf(out, cap, "%s", src ? src : "UTC0");
}

bool tz_learned() { return g_tz_applied || g_tz_from_page[0] != 0; }

void tz_auto_tick(uint32_t) {
  if (g_tz_applied || !g_tz_from_page[0] || !g_wifi_up) return;
  configTzTime(g_tz_from_page, "");
  g_tz_applied = true;
  log_line("TZ", "Wall-clock zone learned from the page (emulated tz_auto).");
}

// ── ota_mgr contract ────────────────────────────────────────────────────
// The signed pull-OTA engine needs real flash slots; here it reports the
// installed version and politely declines installs, so the HA update
// entity wiring stays visible without pretending to flash anything.
void ota_boot_validate() {}

static Topics g_ota_topics{};

void ota_init(const Topics& topics) {
  g_ota_topics = topics;
  char state[192];
  snprintf(state, sizeof(state),
           "{\"installed_version\":\"%s\",\"latest_version\":\"%s\","
           "\"in_progress\":false,\"emulated\":true}",
           CANARY_FW_VERSION, CANARY_FW_VERSION);
  publish_update_state_retained(g_ota_topics, state);
  publish_update_auto_retained(g_ota_topics, false);
}

void ota_loop(uint32_t) {
  if (take_pending_install()) {
    log_line("OTA", "Install requested — declined: emulator has no flash slots.");
    js_net_event("ota-install-declined", "emulator");
  }
  (void)take_pending_auto();
}

// On-glass settings facade — the Settings "firmware" page reads these. With
// no flash slots the emulator reports the installed version, shows "up to
// date", and declines an install the same way ota_loop does.
OtaStatus ota_status() {
  OtaStatus s{};
  s.installed = CANARY_FW_VERSION;
  s.latest = CANARY_FW_VERSION;
  s.update_available = false;
  s.busy = false;
  s.progress = 0;
  s.auto_update = false;
  s.dev_channel = false;
  s.state_text = "up to date";
  return s;
}

void ota_request_check() {
  log_line("OTA", "Check requested — emulator reports up to date.");
  js_net_event("ota-check", "emulator");
}

void ota_request_install() {
  log_line("OTA", "Install requested — declined: emulator has no flash slots.");
  js_net_event("ota-install-declined", "emulator");
}

void ota_set_auto_update(bool on) {
  publish_update_auto_retained(g_ota_topics, on);
}

// ── discovery contract ──────────────────────────────────────────────────
bool discovery_init(const char* device_id, const char* device_type,
                    const char* role) {
  char txt[160];
  snprintf(txt, sizeof(txt), "id=%s dt=%s role=%s fw=%s", device_id,
           device_type, role, CANARY_FW_VERSION);
  js_net_event("mdns-advertise", txt);
  return true;
}

void discovery_advertise_broker(const char* host, uint16_t port) {
  char txt[96];
  snprintf(txt, sizeof(txt), "broker=%s bport=%u", host, (unsigned)port);
  js_net_event("mdns-broker-gossip", txt);
}

void discovery_clear_broker() {
  js_net_event("mdns-broker-gossip", "(retracted)");
}

bool discovery_find_broker(char* host_out, size_t host_cap,
                           uint16_t* port_out) {
  if (!g_referral_host[0]) return false;
  snprintf(host_out, host_cap, "%s", g_referral_host);
  if (port_out) *port_out = g_referral_port;
  js_net_event("mdns-referral-adopted", g_referral_host);
  return true;
}

// Broker-free mDNS fleet enumeration: on silicon this browses _securacv._tcp
// and feeds witnesses straight into the fleet. The emulator has no LAN to
// browse, so it's an honest no-op.
// The second arg is broker_up, which on silicon only picks the browse cadence
// (the browse itself always runs — a hub must never hide devices from you).
void discovery_scan_witnesses(uint32_t, bool) {}

// ── chirp_scan contract ─────────────────────────────────────────────────
// Off-grid BLE fallback: scenario wave 2 (documented in the architecture
// README). Today it reports honestly that no radio is scanning. The third
// arg (wifi_up) picks continuous vs bursty scan on silicon; no radio here.
void chirp_scan_loop(uint32_t, bool, bool) {}
uint32_t chirp_scan_count() { return 0; }

// ── fleet_udp contract (presence beacons over LAN multicast) ────────────
// On silicon this joins a multicast group on the STA interface and hears
// presence beacons from across the house with no broker in the path. The
// emulator has no network stack at all — and must not acquire one: Invariant
// IV is that nothing leaves the machine, so an in-browser display that could
// actually join a group would be a hole, not a feature. Honest no-ops, and
// fleet_udp_ready() answers false rather than pretending a band is up.
void fleet_udp_begin(uint32_t) {}
void fleet_udp_loop(uint32_t) {}
bool fleet_udp_ready() { return false; }
uint32_t fleet_udp_seen() { return 0; }

// ── fleet_link contract (direct BLE GATT pull, FEATURE_FLEET_LINK) ───────
// On silicon this opens a NimBLE central to a nearby WAP's status service.
// The emulator has no radio, so these are honest no-ops.
void fleet_link_loop(uint32_t, bool, bool) {}
void fleet_link_request(const char*) {}
uint32_t fleet_link_count() { return 0; }

// ── glass_web contract (the on-device phone mirror, PR #903) ────────────
// On silicon this serves the display's own page (live mirror + 3D model +
// help + settings). Here, the page you are already looking at IS that
// idea at full strength — the emulator only notes where the on-device
// twin would answer.
void glass_web_init() {
  js_net_event("glass-web", "phone mirror would serve at http://<hostname>.local/");
}
void glass_web_tick(uint32_t) {}
void glass_web_publish(const canary::fleet::Fleet&, uint32_t, bool, bool,
                       int, int, bool, bool, canary::ui::CanaryMood) {}

// ── provision contract ──────────────────────────────────────────────────
// The first-boot SoftAP + captive-portal walk is scenario wave 2 (it
// needs a WebServer/DNSServer shim and a fake phone sheet). The emulated
// device ships provisioned; commissioning (add-a-canary QR) is the live
// pairing surface today.
bool provision_needed() { return false; }
void provision_run(bool) {}

}  // namespace canary::net
