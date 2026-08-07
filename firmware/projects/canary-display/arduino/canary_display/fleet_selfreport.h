/*
 * SecuraCV — Fleet self-report (the /api/fleet body)
 *
 * ONE shared builder for the coarse fleet JSON every networked Canary answers
 * at GET /api/fleet — the contract in docs/tvos + tvos/discovery/DISCOVERY.md,
 * the same shape the Witness Wall emulator's "connect" reads and the Flasher's
 * post-flash LAN discovery finds. Written ONCE here so every board serves it
 * identically (parity by architecture, not copy-paste). A fleet-wide change to
 * the wire shape is a change to this one header.
 *
 * PURE: no Arduino / no heap — a fixed-size, clamp-safe writer, so it
 * round-trips byte-for-byte in a plain g++ host test (firmware/tests_host/
 * test_fleet_selfreport.cpp), exactly like fleet_beacon.h / fleet_roster.h.
 *
 * Each firmware fills a FleetSelfDevice from its runtime config + fleet_beacon
 * status and calls fleet_selfreport_build(); a hub/aggregator may append peer
 * rows with fleet_selfreport_append_device() before closing the array.
 *
 * SAFETY: all string fields are JSON-escaped, so an attacker-influenced device
 * name can never break out of the JSON (same discipline as the web UI's
 * HTML-escaping). The writer never writes past `cap` and always NUL-terminates.
 *
 * Capability gate (see common/core/feature_sanity.h): the *endpoint* is only
 * registered where `HAS_WIFI && FEATURE_HTTP_SERVER`. This header is pure data
 * and compiles everywhere; the per-board glue is what gates.
 */
#ifndef FLEET_SELFREPORT_H
#define FLEET_SELFREPORT_H

#include <stddef.h>

// Worst-case buffer size for a SINGLE-device body whose name is at most
// name_max bytes and product at most product_max bytes. Every name/product
// byte can JSON-escape to 6 bytes (\u00XX), and the name is written TWICE
// (as "kernel" and as devices[0].name) — so an under-sized buffer would make
// the clamp-safe writer truncate a stored-and-accepted device name into
// invalid JSON served with a 200. Size the glue's buffer with this macro and
// truncation is impossible by construction (pinned by the host test's
// worst-case check). The constant covers every fixed byte of the skeleton
// ({"kernel":"…","verified_through":"now","devices":[{…}]}), the widest
// chain_height, the widest born_day plus its exactness flag, and the NUL, with
// headroom.
#define FLEET_SELFREPORT_BODY_CAP(name_max, product_max) \
  (12u * (size_t)(name_max) + 6u * (size_t)(product_max) + 208u)

// Coarse, non-extractive self-state — presence + health only, never raw media.
typedef struct {
  const char* name;         // human name, e.g. "Front Door" or "SCV-1A2B"
  const char* product;      // OTA product string, e.g. "canary-wap"
  int         online;       // 1 = up, 0 = offline/unknown
  int         chain_ok;     // 1 = witness chain verifies, 0 = degraded/unknown
  int         chain_height; // low bits of chain height; <0 = omit
  // When this device's KEY was born, in days since the Unix epoch — a fact
  // about the Canary rather than about whoever paired it, which is why it
  // belongs on the surface every Canary answers to anyone who asks. 0 = omit
  // (a device that has never met a believable clock, or one with no witness
  // key of its own, such as a display). `born_exact` 0 means the day is when
  // the device was first DATED rather than born; readers must not call that a
  // birthday. A day carries no time of day, on purpose — see
  // firmware/common/identity/birth_day.h for the three rules that produce it.
  unsigned    born_day;
  int         born_exact;
} FleetSelfDevice;

// ── clamp-safe primitives (never write past cap-1; always keep a NUL) ──
static inline size_t fsr__raw(char* out, size_t cap, size_t o, const char* s) {
  if (!out || cap == 0 || !s) return o;
  while (*s && o + 1 < cap) out[o++] = *s++;
  out[o < cap ? o : cap - 1] = '\0';
  return o;
}
static inline size_t fsr__jstr(char* out, size_t cap, size_t o, const char* s) {
  // JSON-escaped string *contents* (caller writes the surrounding quotes).
  // NOTE the name: Arduino's Print.h does `#define HEX 16` (and DEC/OCT/BIN),
  // so a local called HEX expands to `static const char* 16 = ...` and every
  // Arduino/ESP32 build of this header fails to compile. A plain g++ host test
  // has no such macro and cannot catch it — hence the fsr__ prefix here and the
  // Arduino-macro simulation at the top of test_fleet_selfreport.cpp.
  static const char* fsr__hex = "0123456789abcdef";
  if (!out || cap == 0) return o;
  if (!s) return o;
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
          ubuf[4] = fsr__hex[(c >> 4) & 0xf]; ubuf[5] = fsr__hex[c & 0xf]; ubuf[6] = '\0';
          rep = ubuf;
        }
        break;
    }
    if (rep) {
      o = fsr__raw(out, cap, o, rep);
    } else if (o + 1 < cap) {
      out[o++] = (char)c;
      out[o] = '\0';
    } else {
      break; // no room
    }
  }
  return o;
}
static inline size_t fsr__int(char* out, size_t cap, size_t o, int v) {
  if (!out || cap == 0) return o;
  char b[16];
  int n = 0, neg = v < 0;
  unsigned uv = neg ? (unsigned)(-(long)v) : (unsigned)v;
  do { b[n++] = (char)('0' + (uv % 10)); uv /= 10; } while (uv && n < 15);
  if (neg && n < 15) b[n++] = '-';
  while (n-- > 0 && o + 1 < cap) out[o++] = b[n];
  if (o < cap) out[o] = '\0';
  return o;
}

// One device object: {"name":..,"online":..,"chain":..,"product":..[,"chain_height":..]}
static inline size_t fleet_selfreport_append_device(char* out, size_t cap, size_t o,
                                                    const FleetSelfDevice* d) {
  o = fsr__raw(out, cap, o, "{\"name\":\"");
  o = fsr__jstr(out, cap, o, (d && d->name) ? d->name : "Canary");
  o = fsr__raw(out, cap, o, "\",\"online\":");
  o = fsr__raw(out, cap, o, (d && d->online) ? "true" : "false");
  o = fsr__raw(out, cap, o, ",\"chain\":\"");
  o = fsr__raw(out, cap, o, (d && d->chain_ok) ? "ok" : "unknown");
  o = fsr__raw(out, cap, o, "\",\"product\":\"");
  o = fsr__jstr(out, cap, o, (d && d->product) ? d->product : "");
  o = fsr__raw(out, cap, o, "\"");
  if (d && d->chain_height >= 0) {
    o = fsr__raw(out, cap, o, ",\"chain_height\":");
    o = fsr__int(out, cap, o, d->chain_height);
  }
  // Omitted entirely until the device has met a clock, so a reader can tell
  // "not known" from any particular day — an absent key and a zero are
  // different answers, and only one of them is honest here.
  if (d && d->born_day > 0u) {
    o = fsr__raw(out, cap, o, ",\"born_day\":");
    o = fsr__int(out, cap, o, (int)d->born_day);
    o = fsr__raw(out, cap, o, ",\"born_exact\":");
    o = fsr__raw(out, cap, o, d->born_exact ? "true" : "false");
  }
  o = fsr__raw(out, cap, o, "}");
  return o;
}

// Full body for a single device: {"kernel":..,"verified_through":"now","devices":[<self>]}.
// Returns the number of bytes written (excluding the NUL). For a hub, call the
// opener/append/closer variants below instead.
static inline size_t fleet_selfreport_open(char* out, size_t cap, const char* kernel_name) {
  size_t o = 0;
  o = fsr__raw(out, cap, o, "{\"kernel\":\"");
  o = fsr__jstr(out, cap, o, kernel_name ? kernel_name : "Canary");
  o = fsr__raw(out, cap, o, "\",\"verified_through\":\"now\",\"devices\":[");
  return o;
}
static inline size_t fleet_selfreport_close(char* out, size_t cap, size_t o) {
  return fsr__raw(out, cap, o, "]}");
}
static inline size_t fleet_selfreport_build(char* out, size_t cap, const FleetSelfDevice* self) {
  size_t o = fleet_selfreport_open(out, cap, (self && self->name) ? self->name : "Canary");
  o = fleet_selfreport_append_device(out, cap, o, self);
  return fleet_selfreport_close(out, cap, o);
}

#endif  // FLEET_SELFREPORT_H
