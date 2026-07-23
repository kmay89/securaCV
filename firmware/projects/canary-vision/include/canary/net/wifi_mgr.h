#pragma once
#include <stdint.h>

namespace canary::net {

// Boot-time connect: bounded. A generic/unprovisioned release continues
// offline immediately; a configured network that misses the timeout also
// continues offline so USB serial, identity, and Vision I2C proof stay usable.
// wifi_loop() owns later reconnect attempts.
void wifi_init_or_reboot();

// Steady-state STA supervision — call every loop() pass. Non-blocking:
// detects link loss, retries with exponential backoff (2 s → 4 s → 8 s →
// 16 s → 30 s cap, reset after a sustained reconnect), and reboots as a
// last resort after WIFI_OUTAGE_REBOOT_MS of continuous outage. Ported from
// the ESP32-S3 canary tree's securacv_network STA supervisor.
void wifi_loop(uint32_t now_ms);

// True while the STA link is up.
bool wifi_connected();

// False when the generic release placeholders are still active.
bool wifi_configured();

// Current RSSI in dBm (0 when not connected) — surfaced as an HA
// diagnostic sensor via the status heartbeat.
int wifi_rssi();

} // namespace canary::net
