#pragma once
#include <stdint.h>
#include <stddef.h>

// Flock discovery — the "how does the second device feel like magic" layer
// (docs/hardware/display_discovery_and_resilience.md).
//
// Every SecuraCV device on the LAN advertises an mDNS service
// `_securacv._tcp` with TXT records (role, device type, firmware — and,
// once it has a working broker link, broker=<host> / bport=<port>). A
// device that doesn't know where the broker is asks the flock: one
// hand-provisioned device seeds the answer for every device that joins
// after it. Fallback: any plain `_mqtt._tcp` advert on the LAN.
//
// The same query path is the self-healing rebind: when the broker link
// stays down past a deadline (e.g. the broker host picked up a new DHCP
// lease), the display re-asks the flock and rebinds without re-flashing.

namespace canary::net {

// Bring up mDNS (call once WiFi is connected): registers the device
// hostname and the _securacv._tcp presence advert with role/dt/fw TXT.
// Safe to call when it fails (returns false; discovery degrades to off).
bool discovery_init(const char* device_id, const char* device_type,
                    const char* role);

// Once the broker link is UP, advertise where the flock's broker lives.
// Only ever called with a broker we are actually connected to — the flock
// must not gossip guesses. Idempotent (TXT update).
void discovery_advertise_broker(const char* host, uint16_t port);

// The moment the broker link drops, retract the referral (empty TXT
// tombstone) so a dead or moved endpoint never re-seeds the LAN's
// rediscovery. Ground truth only, in both directions.
void discovery_clear_broker();

// Ask the flock (then any _mqtt._tcp advert) for a broker. Blocking for a
// few seconds of mDNS query — call it only when the link is already down
// or unconfigured. `.local` broker names are resolved to an IP here, since
// plain DNS can't resolve them at connect time. Returns false if the LAN
// offered nothing.
bool discovery_find_broker(char* host_out, size_t host_cap, uint16_t* port_out);

}  // namespace canary::net
