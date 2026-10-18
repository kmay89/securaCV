// src/net/wifi_mgr.cpp — this board's thin shim over the fleet's ONE Wi-Fi
// supervisor (firmware/common/network/wifi_supervisor.cpp).
//
// Three boards used to carry this file as 300 near-identical lines each
// (docs/FIRMWARE_MAP.md §3). Now the rules live once in common/ and this file
// only says what is this board's: its product name, its node type, its
// retry tunables (by name — scripts/lint_wifi_join_policy.py holds every
// shim to `p.base_ms = WIFI_RETRY_BASE_MS;` and friends), its power policy,
// and how its runtime_config persists the current network.
//
// firmware/scripts/check_sentinel_net_sync.sh keeps canary-sentinel's copy of
// this file byte-identical but for the product-name line.

#include "canary/net/wifi_mgr.h"

#include <Preferences.h>

#include "canary/config.h"
#include "canary/runtime_config.h"       // NVS-backed credentials (OTA-safe)
#include "identity/device_pseudonym.h"   // MAC-free setup-AP suffix
#include "network/onboarding_fsm.h"      // NODE_* words / node_type_from_device_type
#include "network/wifi_join_policy.h"    // fleet-wide join/retry rules (common/)
#include "network/wifi_supervisor.h"     // the one supervisor (common/)
#include "time/tz_rule.h"                // IANA → POSIX for the clock

namespace canary::net {

namespace {

char s_suffix[device_pseudonym::HEX_LEN + 1] = {0};
char s_posix_tz[64] = {0};

// Keep the legacy `wifi_ssid`/`wifi_pass` pair equal to the network on the
// air (runtime_config owns those keys; the flashers seed them). An empty
// SSID is the forget half of a factory reset.
bool persist_current(const char* ssid, const char* pass) {
  if (ssid == nullptr || ssid[0] == '\0') return canary::cfg::forget_wifi_credentials();
  return canary::cfg::set_wifi_credentials(ssid, pass);
}

// The wizard's optional name and the phone's zone, persisted beside the
// credentials (namespace "securacv": `dev_name`, `tz`). Readback, not the
// write calls, decides durability — the runtime_config rule.
bool save_identity(const char* name, const char* tz) {
  Preferences prefs;
  if (!prefs.begin("securacv", /*readOnly=*/false)) return false;
  bool ok = true;
  if (name && name[0]) ok = prefs.putString("dev_name", name) > 0 && prefs.getString("dev_name", "") == name && ok;
  if (tz && tz[0]) {
    ok = prefs.putString("tz", tz) > 0 && prefs.getString("tz", "") == tz && ok;
    const char* posix = tz_rule::posix_for_iana(tz);
    if (posix) snprintf(s_posix_tz, sizeof(s_posix_tz), "%s", posix);
  }
  prefs.end();
  return ok;
}

void load_posix_tz() {
  Preferences prefs;
  if (!prefs.begin("securacv", /*readOnly=*/true)) return;
  const String tz = prefs.getString("tz", "");
  prefs.end();
  const char* posix = tz.length() ? tz_rule::posix_for_iana(tz.c_str()) : nullptr;
  if (posix) snprintf(s_posix_tz, sizeof(s_posix_tz), "%s", posix);
}

}  // namespace

void wifi_init_or_reboot(void (*idle_poll)()) {
  device_pseudonym::device_id_hex(s_suffix, sizeof(s_suffix));
  load_posix_tz();
  const auto& cfg = canary::cfg::get();

  // The retry schedule: this board's numbers, the shared header's rules.
  canary::net::WifiRetryPolicy p;
  p.base_ms          = WIFI_RETRY_BASE_MS;
  p.max_ms           = WIFI_RETRY_MAX_MS;
  p.outage_reboot_ms = WIFI_OUTAGE_REBOOT_MS;

  wifi_sup::Config pc{};
  pc.product_name = "Canary Sentinel";
  pc.node_type = onboard::node_type_from_device_type(DEVICE_TYPE);
  pc.id_suffix = s_suffix;
  pc.ap_channel = 1;  // no ESP-NOW band on this board — plain channel 1
  pc.seed_ssid = cfg.wifi_ssid;
  pc.seed_pass = cfg.wifi_pass;
  pc.seed_configured = canary::cfg::wifi_credentials_configured();
  pc.persist_current = persist_current;
  pc.save_identity = save_identity;
  pc.hub_host = canary::cfg::mqtt_credentials_configured() ? cfg.mqtt_host : nullptr;
  pc.posix_tz = s_posix_tz[0] ? s_posix_tz : nullptr;
  pc.boot_timeout_ms = WIFI_BOOT_TIMEOUT_MS;
  pc.retry = p;
  pc.power_save = WIFI_POWER_SAVE;
  pc.tx_power_qdbm = WIFI_TX_POWER_QDBM;
  wifi_sup::begin(pc, idle_poll);
}

void wifi_loop(uint32_t now_ms) { wifi_sup::loop(now_ms); }
bool wifi_connected() { return wifi_sup::connected(); }
bool wifi_configured() { return wifi_sup::configured(); }
void wifi_open_tap_portal() { wifi_sup::open_tap_portal(); }
void wifi_close_tap_portal() { wifi_sup::close_tap_portal(); }
bool wifi_tap_portal_open() { return wifi_sup::tap_portal_open(); }
int wifi_rssi() { return wifi_sup::rssi(); }
canary::net::onboard::State wifi_onboarding_state() { return wifi_sup::state(); }
bool wifi_setup_ap_up() { return wifi_sup::ap_up(); }
int wifi_saved_networks() { return wifi_sup::saved_count(); }
canary::time_sync::Source wifi_clock_source() { return wifi_sup::clock_source(); }
bool wifi_current_network(const char** ssid, const char** pass) { return wifi_sup::current_network(ssid, pass); }
bool wifi_forget_all() { return wifi_sup::forget_all(); }

} // namespace canary::net
