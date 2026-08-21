#pragma once
#include <stdint.h>

namespace canary::net {

// Boot-time connect: blocking with a hard timeout, then the board continues
// offline and wifi_loop() owns retry (the name is historical — it deliberately
// does NOT reboot; see the 4-inch-display lesson inline). An unprovisioned
// board (generic release placeholders) skips the join entirely and raises the
// shared setup portal instead, sensing all the while. Applies the WiFi power
// policy (modem sleep / TX power cap from canary/config.h) once the link is up.
//
// `idle_poll` (optional) is invoked on every 300 ms wait tick so cable-side
// services stay alive while the link comes up — main.cpp passes the serial
// tuning console's tick, which keeps the post-flash bench responsive while
// the join is still in flight.
void wifi_init_or_reboot(void (*idle_poll)() = nullptr);

// Steady-state STA supervision — call every loop() pass. Non-blocking:
// detects link loss, retries with exponential backoff (2 s → 4 s → 8 s →
// 16 s → 30 s cap, reset after a sustained reconnect), and reboots as a
// last resort after WIFI_OUTAGE_REBOOT_MS of continuous outage. Ported from
// the ESP32-S3 canary tree's securacv_network STA supervisor.
void wifi_loop(uint32_t now_ms);

// True while the STA link is up.
bool wifi_connected();

// False when the generic release placeholders are still active — the board
// is waiting for its setup network to be used (or a flash-time seed).
bool wifi_configured();

// Current RSSI in dBm (0 when not connected) — surfaced as an HA
// diagnostic sensor via the status heartbeat.
int wifi_rssi();

} // namespace canary::net
