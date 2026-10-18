#pragma once
#include <stdint.h>

#include "network/onboarding_fsm.h"   // the state word GET /api/status reports
#include "time/clock_sync.h"          // the clock source word

namespace canary::net {

// This board's Wi-Fi, supervised by the fleet's ONE supervisor
// (firmware/common/network/wifi_supervisor.cpp). The functions below keep
// the names every main.cpp already calls; src/net/wifi_mgr.cpp is the thin
// shim that builds the shared supervisor's Config from this board's
// canary/config.h and runtime_config, and forwards.

// Boot-time connect: bounded, then the board continues offline and
// wifi_loop() owns retry (the name is historical — it deliberately does NOT
// reboot; see wifi_join_policy.h). An unprovisioned board skips the join and
// raises the shared setup portal instead, sensing all the while.
//
// `idle_poll` (optional) is invoked on every 300 ms wait tick so cable-side
// services stay alive while the link comes up.
void wifi_init_or_reboot(void (*idle_poll)() = nullptr);

// Steady-state STA supervision — call every loop() pass. Non-blocking:
// detects link loss, retries with the shared exponential backoff, rotates
// through the saved networks on a NotFound, re-raises the setup network
// after ten minutes offline (nothing wiped), and reboots as a last resort
// only for a link that once worked.
void wifi_loop(uint32_t now_ms);

// True while the STA link is up.
bool wifi_connected();

// False while no network is saved — the board is waiting for its setup
// network (or the Bluetooth door, or a flash-time seed) to be used.
bool wifi_configured();

// The Bluetooth door's tap on a witness that already has a network: raise
// the setup portal (the door's join path) for the tap's minute, and lower
// it again when the door shuts without a join. Idempotent; the first-boot
// and recovery portals are left alone.
void wifi_open_tap_portal();
void wifi_close_tap_portal();
bool wifi_tap_portal_open();

// Current RSSI in dBm (0 when not connected).
int wifi_rssi();

// Facts for GET /api/status (common/network/node_status.h).
canary::net::onboard::State wifi_onboarding_state();
bool wifi_setup_ap_up();
int wifi_saved_networks();
canary::time_sync::Source wifi_clock_source();
// The network on the air, for the household fleet key (common/network/fleet_key.h).
bool wifi_current_network(const char** ssid, const char** pass);

// The credentials half of a factory reset (common/io/reset_hold.h): every
// saved network, the setup AP's key. The identity key is NOT touched. The
// caller reboots. Returns whether the store verifiably let go.
bool wifi_forget_all();

} // namespace canary::net
