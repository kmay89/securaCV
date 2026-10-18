/**
 * @file mdns_txt.h
 * @brief The `_securacv._tcp` advert — one TXT schema for every node, pure.
 *
 * Four trees advertised the service with four TXT sets (docs/FIRMWARE_MAP.md
 * §4): the PIO canary with a constant hostname and no `host`/`name`/`dt`
 * keys, the headless trio on a dummy port with no server behind it, the WAP
 * and the display each with their own order. A browser — the Witness Wall
 * (`tvos/discovery/DISCOVERY.md` reads TXT `host`), the phone app, the
 * Flasher's fleet book, a sibling Canary — had to special-case each.
 *
 * This header is the schema, enumerated in one order from one struct, so
 * every board publishes the same keys and a reader can rely on them:
 *
 *   device_id   the node's id (never the MAC; the salted pseudonym or a name)
 *   name        the human name, or the id again
 *   host        the per-unit mDNS hostname the Wall dials (`<label>.local`)
 *   fw          firmware version
 *   model       the board family word the build was made for
 *   dt          canonical device type: "canary-sense", "canary-wap", …
 *   role        witness | display | hub
 *   hw          the pins header's CANARY_FIGURE_HARDWARE token (omitted when
 *               the build has none to name — absent means "cannot say")
 *   caps        comma-joined capability words (see Cap): what the node serves
 *   coord       who this node believes coordinates the hub-less fleet:
 *               "hub" | "self" | <8-byte fingerprint hex of a peer> | ""
 *   broker      the broker this node is on, as a referral for a display that
 *   bport       has none ("" is the tombstone: readers skip empty values)
 *   tls         "1"/"0" — whether HTTPS is live (the PIO canary's F15), and
 *   secure_port its port; both omitted on boards with no TLS story
 *
 * The service is advertised on the port the node actually answers HTTP on
 * (PORT_HTTP) — the headless trio now serve GET /api/status and /api/fleet
 * (common/network/node_http.h), so the dummy port 1 is gone.
 *
 * Pure: no Arduino, no heap. firmware/tests_host/test_mdns_txt.cpp.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace canary {
namespace net {
namespace mdns_txt {

static constexpr const char* SERVICE = "securacv";
static constexpr const char* PROTO = "tcp";
static constexpr uint16_t PORT_HTTP = 80;

static constexpr const char* ROLE_WITNESS = "witness";
static constexpr const char* ROLE_DISPLAY = "display";
static constexpr const char* ROLE_HUB = "hub";

static constexpr const char* COORD_HUB = "hub";
static constexpr const char* COORD_SELF = "self";

/// Capability bits → the `caps` words, in this order.
enum Cap : uint16_t {
  CAP_STATUS   = 1u << 0,  ///< GET /api/status
  CAP_FLEET    = 1u << 1,  ///< GET /api/fleet (the Wall can read it directly)
  CAP_OTA      = 1u << 2,  ///< signed pull-OTA engine linked
  CAP_BLE_DOOR = 1u << 3,  ///< Improv Wi-Fi door on a radio
  CAP_BEACON   = 1u << 4,  ///< fleet-link presence beacon transmitter
  CAP_PEER     = 1u << 5,  ///< authenticated peer protocol (common/network/peer_protocol.h)
  CAP_MQTT     = 1u << 6,  ///< publishes to a broker when it has one
  CAP_WEB      = 1u << 7,  ///< a human-facing web page beyond the API
};

static constexpr size_t CAPS_CAP = 72;  ///< every word, comma-joined, plus NUL

inline size_t caps_string(uint16_t caps, char* out, size_t cap) {
  static const struct { uint16_t bit; const char* word; } kWords[] = {
    {CAP_STATUS, "status"}, {CAP_FLEET, "fleet"}, {CAP_OTA, "ota"},
    {CAP_BLE_DOOR, "ble"}, {CAP_BEACON, "beacon"}, {CAP_PEER, "peer"},
    {CAP_MQTT, "mqtt"}, {CAP_WEB, "web"},
  };
  if (out == nullptr || cap == 0) return 0;
  size_t o = 0;
  out[0] = '\0';
  for (const auto& w : kWords) {
    if (!(caps & w.bit)) continue;
    const size_t wl = strlen(w.word);
    const size_t need = (o ? 1 : 0) + wl;
    if (o + need + 1 > cap) { out[0] = '\0'; return 0; }  // never a half list
    if (o) out[o++] = ',';
    memcpy(out + o, w.word, wl);
    o += wl;
    out[o] = '\0';
  }
  return o;
}

inline uint16_t parse_caps(const char* s) {
  static const struct { uint16_t bit; const char* word; } kWords[] = {
    {CAP_STATUS, "status"}, {CAP_FLEET, "fleet"}, {CAP_OTA, "ota"},
    {CAP_BLE_DOOR, "ble"}, {CAP_BEACON, "beacon"}, {CAP_PEER, "peer"},
    {CAP_MQTT, "mqtt"}, {CAP_WEB, "web"},
  };
  uint16_t caps = 0;
  if (s == nullptr) return 0;
  while (*s) {
    const char* end = s;
    while (*end && *end != ',') end++;
    const size_t n = (size_t)(end - s);
    for (const auto& w : kWords) {
      if (n == strlen(w.word) && strncmp(s, w.word, n) == 0) caps |= w.bit;
    }
    s = *end ? end + 1 : end;
  }
  return caps;
}

/// What a node knows about itself. Null or "" = omit where the schema allows.
struct Fields {
  const char* device_id = nullptr;
  const char* name = nullptr;
  const char* host = nullptr;
  const char* fw = nullptr;
  const char* model = nullptr;
  const char* dt = nullptr;
  const char* role = nullptr;
  const char* hw = nullptr;        ///< omitted when null/""
  uint16_t caps = 0;
  const char* coord = nullptr;     ///< "" when unknown (always published)
  const char* broker = nullptr;    ///< "" tombstone when none (always published)
  uint16_t bport = 0;
  int tls = -1;                    ///< -1 omit both tls keys; 0/1 publish
  uint16_t secure_port = 0;        ///< published only with tls == 1 and non-zero
};

struct Kv {
  const char* key;
  const char* value;
};

static constexpr size_t MAX_KEYS = 14;

/// Scratch the caller lends enumerate() for the rendered numbers and words.
struct Scratch {
  char caps[CAPS_CAP];
  char bport[8];
  char sport[8];
};

/**
 * @brief The canonical TXT set, in canonical order. Returns the count, 0 when
 * `out` cannot hold it (never a partial advert).
 */
inline size_t enumerate(const Fields& f, Kv* out, size_t cap, Scratch& s) {
  if (out == nullptr || cap < MAX_KEYS) return 0;
  const char* id = (f.device_id && f.device_id[0]) ? f.device_id : "canary";
  size_t n = 0;
  out[n++] = {"device_id", id};
  out[n++] = {"name", (f.name && f.name[0]) ? f.name : id};
  out[n++] = {"host", f.host ? f.host : ""};
  out[n++] = {"fw", f.fw ? f.fw : ""};
  out[n++] = {"model", f.model ? f.model : ""};
  out[n++] = {"dt", f.dt ? f.dt : ""};
  out[n++] = {"role", (f.role && f.role[0]) ? f.role : ROLE_WITNESS};
  if (f.hw && f.hw[0]) out[n++] = {"hw", f.hw};
  caps_string(f.caps, s.caps, sizeof(s.caps));
  out[n++] = {"caps", s.caps};
  out[n++] = {"coord", f.coord ? f.coord : ""};
  const bool have_broker = f.broker && f.broker[0] && f.bport != 0;
  if (have_broker) snprintf(s.bport, sizeof(s.bport), "%u", (unsigned)f.bport);
  else s.bport[0] = '\0';
  out[n++] = {"broker", have_broker ? f.broker : ""};
  out[n++] = {"bport", s.bport};
  if (f.tls >= 0) {
    out[n++] = {"tls", f.tls ? "1" : "0"};
    if (f.tls && f.secure_port) {
      snprintf(s.sport, sizeof(s.sport), "%u", (unsigned)f.secure_port);
      out[n++] = {"secure_port", s.sport};
    }
  }
  return n;
}

/// RFC 1123 host label: 1..63 of [A-Za-z0-9-], no leading/trailing '-'.
inline bool valid_host_label(const char* s) {
  if (s == nullptr || s[0] == '\0') return false;
  size_t n = 0;
  for (; s[n]; n++) {
    const char c = s[n];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
  }
  return n <= 63 && s[0] != '-' && s[n - 1] != '-';
}

}  // namespace mdns_txt
}  // namespace net
}  // namespace canary
