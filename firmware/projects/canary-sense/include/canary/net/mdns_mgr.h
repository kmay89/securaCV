#pragma once
#include <stdint.h>
#include <stddef.h>

// Fleet LAN presence — the _securacv._tcp mDNS advert.
//
// Every SecuraCV device on the LAN advertises the same service with the
// same TXT identity vocabulary so the companion app, canary-display, and
// sibling Canaries can find and label it without a broker round-trip:
//
//   device_id  stable id (NVS-backed, survives OTA)
//   name       friendly name (device_id until a rename lands)
//   host       unique mDNS host label ("canary-vision-001-a1b2c3")
//   fw         CANARY_FW_VERSION
//   model      human model string
//   dt         canonical device type — lowercase, hyphenated
//              ("canary-vision" / "canary-sense" / "canary-wap")
//   role       "witness" (sensors) or "display" (glance surfaces)
//   hw         the pins header's board token (omitted when none)
//   caps       what this build serves: status,fleet,ota,ble,beacon,peer,mqtt
//   coord      who coordinates the hub-less fleet (peer_link)
//   broker     MQTT broker gossip — ground-truth-only self-heal referral
//   bport      (empty tombstone while the broker link is down)
//
// The set is enumerated by common/network/mdns_txt.h, in that order, on
// every node — one schema, one browser.
//
// This variant is advertise-only: it never browses. The display and the
// companion app do the querying (and the WAP relays the fleet into the
// app's discovered-peers list via /api/fleet/scan).

namespace canary::net {

// Start the fleet advert. Call once after the boot WiFi connect.
bool mdns_init();

// Drain deferred work: the STA_GOT_IP handler only latches a re-announce
// flag (event handlers never run mDNS logic); this drains it on the main
// task. Call every loop pass — cheap no-op when nothing is pending.
void mdns_loop(uint32_t now_ms);

// Broker gossip — same TXT contract as canary-wap's
// FEATURE_MDNS_BROKER_GOSSIP and canary-display: advertise the working
// broker while the link is up, tombstone it the moment it drops so a
// dead endpoint is never re-seeded onto the LAN.
void mdns_advertise_broker(const char* host, uint16_t port);
void mdns_clear_broker();

// The hub-less fleet's coordinator, as the peer link last elected it
// ("hub" | "self" | a peer's fingerprint hex | ""): the `coord` TXT key.
// Cheap when unchanged; re-applies the key when it is not.
void mdns_set_coord(const char* word);

// The unique host label claimed at init ("canary-vision-001-a1b2c3");
// empty string until mdns_init() succeeds.
const char* mdns_host_label();

}  // namespace canary::net
