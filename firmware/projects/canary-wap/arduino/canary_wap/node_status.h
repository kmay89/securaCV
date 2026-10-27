/*
 * SecuraCV — Node status (the GET /api/status body every node serves)
 *
 * ONE shared builder for the coarse status JSON every networked Canary
 * answers at GET /api/status: what it is, which firmware, how long it has
 * been up, where it is on the network, where it stands in onboarding, with
 * its hub, with its peers, with the Witness Wall, and with its updates. The
 * program brief asked for exactly this list on every node; before this header
 * two trees served a rich, gated status and four served none.
 *
 * Written ONCE here, in the style of fleet_selfreport.h (the /api/fleet body):
 * PURE, no Arduino / no heap, a fixed-size clamp-safe writer that
 * round-trips byte-for-byte in a plain g++ host test
 * (firmware/tests_host/test_node_status.cpp). Each firmware fills a
 * NodeStatus from what it already knows and calls node_status_build().
 *
 * COARSE ON PURPOSE. This is an anyone-on-the-LAN surface on the trees that
 * have no bearer (the display, the headless trio), so nothing here is a
 * secret and nothing is a stable identifier beyond what mDNS already says:
 * no MAC, no key, no broker address, no coordinates. The IP is the address
 * the asker already connected to. (AGENTS.md Invariants II/III.) On the PIO
 * canary and the WAP, whose /api/status is gated and richer (the canary's
 * takes the bearer; the WAP's takes api_auth_check(), the bearer or a
 * landing session any LAN host can mint, F346), these keys are FOLDED INTO
 * that body, same spelling, so one reader works everywhere.
 *
 * Every optional key is OMITTED when the node cannot say — the born_day rule
 * from fleet_selfreport.h: absent means "cannot tell you", and a reader must
 * be able to tell that from any particular value.
 *
 * SAFETY: all string fields are JSON-escaped; the writer never writes past
 * `cap` and always NUL-terminates. Size the buffer with NODE_STATUS_BODY_CAP
 * and truncation is impossible by construction (the host test proves the
 * worst case).
 */
#ifndef NODE_STATUS_H
#define NODE_STATUS_H

#include <stddef.h>
#include <stdint.h>

// Field caps the fixed body budget is computed from. A string longer than its
// cap is still written safely — it just counts toward the escaping worst case
// the caller sized for, so keep callers to these (they match the firmware's
// own buffers: device_id[48], product ids, version strings, IPv4 text).
#define NODE_STATUS_NAME_MAX     48u
#define NODE_STATUS_PRODUCT_MAX  40u
#define NODE_STATUS_FW_MAX       24u
#define NODE_STATUS_HW_MAX       48u
#define NODE_STATUS_WORD_MAX     24u   /* node, onboarding, wall, peer, coord, ota, clock, creds */
#define NODE_STATUS_IP_MAX       16u
#define NODE_STATUS_TZ_MAX       64u
#define NODE_STATUS_OTA_VER_MAX  24u

// Every string at its all-escaping worst (6 bytes per char), every fixed key
// and the widest integers, plus headroom. Pinned by the host test.
#define NODE_STATUS_BODY_CAP                                             \
  (6u * (NODE_STATUS_NAME_MAX + NODE_STATUS_PRODUCT_MAX + NODE_STATUS_FW_MAX \
         + NODE_STATUS_HW_MAX + 8u * NODE_STATUS_WORD_MAX                  \
         + NODE_STATUS_IP_MAX + NODE_STATUS_TZ_MAX + NODE_STATUS_OTA_VER_MAX) \
   + 512u)

// Hub words share fleet_selfreport.h's values so one glue fills both bodies.
#define NS_HUB_UNKNOWN 0
#define NS_HUB_NONE    1
#define NS_HUB_DOWN    2
#define NS_HUB_OK      3

typedef struct {
  // ── what ──
  const char* node;        // device type word: "canary-sense", "canary-wap", … (required)
  const char* product;     // OTA product id, e.g. "securacv-canary-sense"; NULL/"" = omit
  const char* fw;          // firmware version string (required; "" allowed)
  const char* hardware;    // CANARY_FIGURE_HARDWARE board token; NULL/"" = omit
  const char* name;        // the human name, if any; NULL/"" = omit
  // ── uptime / network ──
  uint32_t    uptime_s;
  const char* ip;          // dotted IPv4 while online; NULL/"" = omit
  int         rssi;        // dBm while online; 0 = omit
  const char* onboarding;  // canary::net::onboard::state_label(); NULL = omit
  int         setup_ap;    // 1 = the setup AP is up right now; 0 = down; <0 = omit
  int         ble_door;    // 1 = the Improv door is open; 0 = closed; <0 = omit (no radio)
  int         saved_networks; // count in the wifi_store ring; <0 = omit
  // ── hub / peers / wall ──
  int         hub;         // NS_HUB_* ; UNKNOWN omits
  int         peers_seen;  // fleet roster size (presence beacons / mDNS); <0 = omit
  const char* wall;        // "direct" (polled our /api/fleet lately) | "hub" (the hub carries us) | "none"; NULL = omit
  int         wall_polled_s_ago; // seconds since the last /api/fleet GET; <0 = omit
  const char* peer;        // the peer protocol: "on" | "off" (no fleet key: open network); NULL = omit (not linked)
  const char* coord;       // who coordinates the hub-less fleet: "hub" | "self" | <fp hex>; NULL/"" = omit
  // ── ota ──
  const char* ota_state;   // the OTA engine's state word (docs/OTA.md); NULL = omit
  const char* ota_target;  // version available / downloading / pending; NULL/"" = omit
  int         ota_pending_verify; // 1 = this boot is the unconfirmed new image; 0 = no; <0 = omit
  // ── posture / clock ──
  const char* creds_at_rest; // key_at_rest wire label ("plaintext-nvs", …); NULL = omit
  const char* clock;       // canary::time_sync::source_label(); NULL = omit
  const char* tz;          // IANA zone the node keeps; NULL/"" = omit
} NodeStatus;

// ── clamp-safe primitives (never write past cap-1; always keep a NUL) ──
static inline size_t ns__raw(char* out, size_t cap, size_t o, const char* s) {
  if (!out || cap == 0 || !s) return o;
  while (*s && o + 1 < cap) out[o++] = *s++;
  out[o < cap ? o : cap - 1] = '\0';
  return o;
}
static inline size_t ns__jstr(char* out, size_t cap, size_t o, const char* s) {
  // Arduino's Print.h #defines HEX; hence the ns__ prefix (fleet_selfreport lesson).
  static const char* ns__hex = "0123456789abcdef";
  if (!out || cap == 0 || !s) return o;
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    const char* rep = 0;
    char ubuf[7];
    switch (c) {
      case '"':  rep = "\\\""; break;
      case '\\': rep = "\\\\"; break;
      case '\n': rep = "\\n";  break;
      case '\r': rep = "\\r";  break;
      case '\t': rep = "\\t";  break;
      default:
        if (c < 0x20) {
          ubuf[0] = '\\'; ubuf[1] = 'u'; ubuf[2] = '0'; ubuf[3] = '0';
          ubuf[4] = ns__hex[(c >> 4) & 0xf]; ubuf[5] = ns__hex[c & 0xf]; ubuf[6] = '\0';
          rep = ubuf;
        }
        break;
    }
    if (rep) {
      o = ns__raw(out, cap, o, rep);
    } else if (o + 1 < cap) {
      out[o++] = (char)c;
      out[o] = '\0';
    } else {
      break;
    }
  }
  return o;
}
static inline size_t ns__int(char* out, size_t cap, size_t o, long v) {
  if (!out || cap == 0) return o;
  char b[24];
  int n = 0, neg = v < 0;
  unsigned long uv = neg ? (unsigned long)(-(v + 1)) + 1ul : (unsigned long)v;
  do { b[n++] = (char)('0' + (uv % 10)); uv /= 10; } while (uv && n < 22);
  if (neg && n < 23) b[n++] = '-';
  while (n-- > 0 && o + 1 < cap) out[o++] = b[n];
  if (o < cap) out[o] = '\0';
  return o;
}
static inline size_t ns__kv_str(char* out, size_t cap, size_t o, const char* key, const char* v) {
  if (!v || !v[0]) return o;
  o = ns__raw(out, cap, o, ",\"");
  o = ns__raw(out, cap, o, key);
  o = ns__raw(out, cap, o, "\":\"");
  o = ns__jstr(out, cap, o, v);
  o = ns__raw(out, cap, o, "\"");
  return o;
}
static inline size_t ns__kv_int(char* out, size_t cap, size_t o, const char* key, long v) {
  o = ns__raw(out, cap, o, ",\"");
  o = ns__raw(out, cap, o, key);
  o = ns__raw(out, cap, o, "\":");
  o = ns__int(out, cap, o, v);
  return o;
}
static inline size_t ns__kv_bool(char* out, size_t cap, size_t o, const char* key, int v) {
  o = ns__raw(out, cap, o, ",\"");
  o = ns__raw(out, cap, o, key);
  o = ns__raw(out, cap, o, v ? "\":true" : "\":false");
  return o;
}

/*
 * {"node":"…","fw":"…","uptime_s":N
 *  [,"product":"…"][,"hw":"…"][,"name":"…"]
 *  [,"ip":"…"][,"rssi":N][,"onboarding":"…"][,"setup_ap":b][,"ble_door":b][,"saved_networks":N]
 *  [,"hub":"ok|down|none"][,"peers_seen":N][,"wall":"…"][,"wall_polled_s_ago":N][,"peer":"on|off"][,"coord":"…"]
 *  [,"ota":{"state":"…"[,"target":"…"][,"pending_verify":b]}]
 *  [,"creds_at_rest":"…"][,"clock":"…"][,"tz":"…"]}
 * Returns bytes written (excluding the NUL).
 */
static inline size_t node_status_build(char* out, size_t cap, const NodeStatus* s) {
  size_t o = 0;
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!s) return 0;
  o = ns__raw(out, cap, o, "{\"node\":\"");
  o = ns__jstr(out, cap, o, s->node ? s->node : "canary");
  o = ns__raw(out, cap, o, "\",\"fw\":\"");
  o = ns__jstr(out, cap, o, s->fw ? s->fw : "");
  o = ns__raw(out, cap, o, "\",\"uptime_s\":");
  o = ns__int(out, cap, o, (long)s->uptime_s);
  o = ns__kv_str(out, cap, o, "product", s->product);
  o = ns__kv_str(out, cap, o, "hw", s->hardware);
  o = ns__kv_str(out, cap, o, "name", s->name);
  o = ns__kv_str(out, cap, o, "ip", s->ip);
  if (s->rssi != 0) o = ns__kv_int(out, cap, o, "rssi", s->rssi);
  o = ns__kv_str(out, cap, o, "onboarding", s->onboarding);
  if (s->setup_ap >= 0) o = ns__kv_bool(out, cap, o, "setup_ap", s->setup_ap);
  if (s->ble_door >= 0) o = ns__kv_bool(out, cap, o, "ble_door", s->ble_door);
  if (s->saved_networks >= 0) o = ns__kv_int(out, cap, o, "saved_networks", s->saved_networks);
  if (s->hub != NS_HUB_UNKNOWN) {
    o = ns__kv_str(out, cap, o, "hub",
                   s->hub == NS_HUB_OK ? "ok" : s->hub == NS_HUB_DOWN ? "down"
                   : s->hub == NS_HUB_NONE ? "none" : "unknown");
  }
  if (s->peers_seen >= 0) o = ns__kv_int(out, cap, o, "peers_seen", s->peers_seen);
  o = ns__kv_str(out, cap, o, "wall", s->wall);
  if (s->wall_polled_s_ago >= 0) o = ns__kv_int(out, cap, o, "wall_polled_s_ago", s->wall_polled_s_ago);
  o = ns__kv_str(out, cap, o, "peer", s->peer);
  o = ns__kv_str(out, cap, o, "coord", s->coord);
  if (s->ota_state && s->ota_state[0]) {
    o = ns__raw(out, cap, o, ",\"ota\":{\"state\":\"");
    o = ns__jstr(out, cap, o, s->ota_state);
    o = ns__raw(out, cap, o, "\"");
    o = ns__kv_str(out, cap, o, "target", s->ota_target);
    if (s->ota_pending_verify >= 0) o = ns__kv_bool(out, cap, o, "pending_verify", s->ota_pending_verify);
    o = ns__raw(out, cap, o, "}");
  }
  o = ns__kv_str(out, cap, o, "creds_at_rest", s->creds_at_rest);
  o = ns__kv_str(out, cap, o, "clock", s->clock);
  o = ns__kv_str(out, cap, o, "tz", s->tz);
  o = ns__raw(out, cap, o, "}");
  return o;
}

#endif /* NODE_STATUS_H */
