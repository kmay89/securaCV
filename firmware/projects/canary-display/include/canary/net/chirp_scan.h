#pragma once
#include <stdint.h>

// Off-grid resilience (trailblazer spec §6): passive BLE scan for the
// Canaries' connectionless Chirp adverts (docs/ble_protocol.md §5 —
// manufacturer data, company id 0xFFFF, 17 bytes total:
// [company(2)|type(1)|hour-bucket ts(4)|chain-hash(8)|fp-prefix(2)]).
//
// Scanning runs ONLY while the broker link is down: MQTT is the richer
// channel when it exists; chirps are the "burglar cut the internet"
// fallback. Bursty (4 s scan every 20 s), passive (never transmits), and
// trust-honest: chirps are unsigned, so they feed liveness and attention
// ("tamper (chirp)") but can never mark anything verified.
//
// Whole module is gated by FEATURE_CHIRP_SCAN (BLE is the one genuinely
// expensive radio decision — flash, heap, and 2.4 GHz coexistence).

namespace canary::net {

// Lazy: the BLE stack initializes on the first broker-down burst, not at
// boot — a healthy display never pays for the radio.
void chirp_scan_loop(uint32_t now_ms, bool broker_down);

// Diagnostics: chirps parsed since boot.
uint32_t chirp_scan_count();

}  // namespace canary::net
