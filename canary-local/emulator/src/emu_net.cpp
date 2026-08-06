// canary-local/emulator/src/emu_net.cpp — the LAN as a scenario.
//
// Replaces the firmware's ESP-specific net TUs (wifi_mgr, tz_auto,
// ota_mgr, discovery, chirp_scan, provision) with implementations of the
// same canary::net contracts, driven by page-side switches. The
// semantics stay honest to the originals — including the uncomfortable
// ones: a display that boots with no reachable Wi-Fi really does give up
// and reboot after the same timeout, because watching that happen (and
// reading the serial log while it does) is exactly what canary.local is
// for. mqtt_mgr.cpp is NOT here: the real one compiles against the
// PubSubClient shim in emu_mqtt.cpp.
#include <Arduino.h>

#include <emscripten.h>
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

#include "emu_bus.h"

// ── Scenario state (set from JS via emu_bindings.cpp) ───────────────────
namespace {
volatile int g_wifi_up = 1;
// Has the emulated link ever been up? Gates the outage reboot, mirroring the
// firmware's rule that a link which never associated must never be rebooted.
// Starts at 1 because the emulator boots connected by default.
volatile int g_wifi_ever_up = 1;
volatile int g_rssi = -52;
volatile int g_broker_up = 1;

char g_tz_from_page[64] = {0};   // browser's zone, POSIX form
bool g_tz_applied = false;

char g_referral_host[64] = {0};  // mDNS fleet-gossip broker referral
uint16_t g_referral_port = 1883;

uint32_t g_wifi_down_since = 0;

EM_JS(void, js_net_event, (const char* kind, const char* detail), {
  if (Module.onNetEvent) Module.onNetEvent(UTF8ToString(kind), UTF8ToString(detail));
});
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
      // Same as glass — and the glass no longer reboots here. Rebooting on a
      // join that has never succeeded re-runs the identical join forever, so
      // the device never finishes booting and the setup wizard that could fix
      // the credentials never appears. Boot completes; wifi_loop owns retry.
      // (firmware/common/network/wifi_join_policy.h)
      log_line("WiFi",
               canary::net::join_failure_detail(canary::net::JoinFailure::Unknown));
      js_net_event("wifi-boot-timeout", "");
      return;
    }
    delay(250);
  }
  log_line("WiFi", "Connected (emulated).");
  g_wifi_ever_up = 1;
  js_net_event("wifi-up", "");
}

void wifi_loop(uint32_t now_ms) {
  if (g_wifi_up) {
    if (g_wifi_down_since) js_net_event("wifi-up", "reconnected");
    g_wifi_down_since = 0;
    g_wifi_ever_up = 1;
    return;
  }
  if (!g_wifi_down_since) {
    g_wifi_down_since = now_ms ? now_ms : 1;
    js_net_event("wifi-down", "");
  }
  // Only a link that WAS up may be rebooted — the shared rule the firmware
  // now follows. A link that never associated is a wrong configuration, not a
  // wedged radio, and rebooting it is the same failed join on a timer.
  if (g_wifi_ever_up &&
      (int32_t)(now_ms - g_wifi_down_since) >= (int32_t)WIFI_OUTAGE_REBOOT_MS) {
    log_line("WiFi", "Outage past deadline on a link that was working — rebooting.");
    js_net_event("wifi-outage-reboot", "");
    ESP.restart();
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
// browser tab would be theater, so this is honestly false.
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
