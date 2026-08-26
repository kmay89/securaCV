#pragma once
#include <stdint.h>
#include <stddef.h>

// Fleet discovery — the "how does the second device feel like magic" layer
// (docs/hardware/display_discovery_and_resilience.md).
//
// Every SecuraCV device on the LAN advertises an mDNS service
// `_securacv._tcp` with TXT records (role, device type, firmware — and,
// once it has a working broker link, broker=<host> / bport=<port>). A
// device that doesn't know where the broker is asks the fleet: one
// hand-provisioned device seeds the answer for every device that joins
// after it. Fallback: any plain `_mqtt._tcp` advert on the LAN.
//
// The same query path is the self-healing rebind: when the broker link
// stays down past a deadline (e.g. the broker host picked up a new DHCP
// lease), the display re-asks the fleet and rebinds without re-flashing.

namespace canary::net {

// Bring up mDNS (call once WiFi is connected): registers the device
// hostname and the _securacv._tcp presence advert with role/dt/fw TXT.
// Safe to call when it fails (returns false; discovery degrades to off).
bool discovery_init(const char* device_id, const char* device_type,
                    const char* role);

// True once discovery_init actually registered the hostname this boot.
// The surfaces that PRINT the glass's .local address (settings network
// page, transparency sheet) must check this before claiming the name
// resolves — a failed MDNS.begin means the address would be a lie, and
// the honest fallback is the numeric IP.
bool discovery_up();

// Once the broker link is UP, advertise where the fleet's broker lives.
// Only ever called with a broker we are actually connected to — the fleet
// must not gossip guesses. Idempotent (TXT update).
void discovery_advertise_broker(const char* host, uint16_t port);

// The moment the broker link drops, retract the referral (empty TXT
// tombstone) so a dead or moved endpoint never re-seeds the LAN's
// rediscovery. Ground truth only, in both directions.
void discovery_clear_broker();

// Ask the fleet (then any _mqtt._tcp advert) for a broker. Blocking for a
// few seconds of mDNS query — call it only when the link is already down
// or unconfigured. `.local` broker names are resolved to an IP here, since
// plain DNS can't resolve them at connect time. Returns false if the LAN
// offered nothing.
bool discovery_find_broker(char* host_out, size_t host_cap, uint16_t* port_out);

// Enumerate the fleet DIRECTLY from mDNS: browse `_securacv._tcp`, and feed
// every OTHER Canary's advert into the fleet model (on_status/on_meta) so a
// display shows every nearby device — the WiFi analog of the BLE presence
// beacon.
//
// Call this EVERY pass, hub or no hub. It used to be gated on the broker being
// down, which quietly made a hub a precondition for seeing devices rather than
// an upgrade: MQTT only ever reports Canaries configured to talk to that
// broker, so any Canary on the LAN that wasn't pointed at the hub stayed
// invisible for as long as the hub was healthy.
//
// `broker_up` only sets the cadence (60 s when the broker is carrying the load,
// 20 s when it isn't) — the ESPmDNS query blocks a few seconds, so it is kept
// off a healthy display's critical path without being skipped. Self-rate-
// limiting: passing a `now` earlier than the internal next-due returns at once.
//
// SECURITY: mDNS TXT is UNAUTHENTICATED LAN input (like the broker gossip), so
// this only ever marks devices seen/named — never trusted. It never sets the
// Verified badge.
void discovery_scan_witnesses(uint32_t now, bool broker_up);

}  // namespace canary::net
