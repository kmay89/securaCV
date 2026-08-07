/*
 * SecuraCV — the device self-manifest (host-testable, no Arduino).
 *
 * One machine-readable line the Canary emits on request ('j' over USB serial,
 * and — later — a BLE characteristic): a compact JSON object that describes the
 * device to a program instead of a human. It is the device's *live* source of
 * truth, and it is deliberately public-only:
 *
 *   board, firmware+git, protocol, device id, the 32-byte Ed25519 PUBLIC key
 *   (hex), its fingerprint, the chain head, seq/boots, self-test health, the
 *   tamper flag, the enabled feature set, and the help URL.
 *
 * No private key, no secret, nothing that isn't already printed on the trust
 * card. Its whole reason to exist is that a *browser* can read it over WebSerial
 * and then (a) draw the SAME drunken-bishop randomart from the pubkey — so you
 * can eyeball that the shape on your screen matches the shape on your device,
 * with no vendor in the loop — and (b) render exactly the tools THIS unit has,
 * instead of guessing from a static catalog.
 *
 * Pure string composition into a caller buffer (no heap, no ArduinoJson, no
 * Arduino types) so the exact bytes are proven in host tests and the schema
 * can't drift out from under the website. Compiles hosted for tests_host with
 * -Wall -Wextra -Werror.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */
#ifndef SECURACV_SELF_MANIFEST_H
#define SECURACV_SELF_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

namespace manifest {

// Bump the minor when adding fields (additive), the major on a breaking change.
// The website pins this exact string so a breaking change fails CI loudly.
static constexpr const char* SCHEMA = "securacv.canary.manifest/v1";

// One interactive command the device answers, for the commands[] array. Key is
// a single serial character; name is the short registry label ("identity").
struct Cmd {
  char key;
  const char* name;
};

// One OTHER Canary this device has heard over the air, for the fleet[] array —
// the machine-readable twin of the `n`/"nearby" console card. Public-only and
// UNSIGNED (liveness/self-report, never a verified trust claim): a peer's short
// fingerprint, how long ago it was last heard, its self-reported health/battery
// (<0 → emitted as null), the low bits of its chain height, and a worded status
// ("" when nothing is set). Single-sourced from the same fleet roster the `n`
// card renders, so the browser /fleet page shows the live truth.
struct Peer {
  const char* fp;      // 4 hex chars — the peer id / short fingerprint
  uint32_t age_s;      // seconds since last heard
  int health;          // 0..100, <0 = unknown → null
  int battery;         // 0..100, <0 = unknown → null
  uint32_t chain;      // low 16 bits of the peer's chain height
  const char* flags;   // worded status, comma-joined ("" = nothing set)
};

// Everything the device knows about itself, as plain pointers/values the caller
// fills from live state. Hex fields are lower-case, NUL-terminated, exactly as
// long as their byte count implies (pubkey = 64, fp = 16, chain_head = 64).
struct Facts {
  const char* board;          // "canary", "canary-vision", ...
  const char* firmware;       // "2.2.0"
  const char* git;            // "abc1234" or "not-embedded"
  const char* protocol;       // "pwk:v0.3.0"
  const char* device_id;      // "canary-7fA3"
  const char* pubkey_hex;     // 64 hex chars — the randomart source of truth
  const char* pubkey_fp_hex;  // 16 hex chars
  const char* chain_head_hex; // 64 hex chars
  uint32_t seq;
  uint32_t boots;
  // When this device's KEY was born, in days since the Unix epoch — a fact
  // about the Canary rather than about whoever is holding a phone near it.
  // 0 = never dated (an offline device that has yet to meet a clock), emitted
  // as null so a reader can tell "not known" from "day zero".
  // `born_exact` false means the day is when the device was first DATED, not
  // when it was born; readers must not call that a birthday. See
  // common/identity/birth_day.h for the three rules that produce these.
  uint32_t born_day = 0;
  bool born_exact = false;
  int health;                 // 0..100, or <0 = unknown → emitted as null
  int temp_c = -1000;         // die temperature °C; <= -273 = unknown → null
  bool tamper;
  const char* const* features; // enabled feature short-names
  size_t feature_count;
  const Cmd* commands;        // the interactive keys THIS image answers
  size_t command_count;
  const Peer* fleet;          // OTHER Canaries this one has heard (may be empty)
  size_t fleet_count;
  const char* help_url;       // "https://securacv.com/canary"
};

// ── a bounded writer: appends until it would overflow, then latches !ok ──────
namespace detail {
struct Writer {
  char* p;
  size_t left;   // bytes remaining INCLUDING the terminating NUL
  bool ok;

  void raw(char c) {
    if (!ok) return;
    if (left <= 1) { ok = false; return; }  // keep room for NUL
    *p++ = c;
    --left;
  }
  void raw(const char* s) {
    if (!s) return;
    while (*s) raw(*s++);
  }
  // A JSON string value with escaping (defensive: device_id etc. are ASCII in
  // practice, but never trust an input into a wire format).
  void str(const char* s) {
    raw('"');
    for (const char* c = s ? s : ""; *c; ++c) {
      unsigned char u = (unsigned char)*c;
      switch (u) {
        case '"':  raw('\\'); raw('"');  break;
        case '\\': raw('\\'); raw('\\'); break;
        case '\b': raw('\\'); raw('b');  break;
        case '\f': raw('\\'); raw('f');  break;
        case '\n': raw('\\'); raw('n');  break;
        case '\r': raw('\\'); raw('r');  break;
        case '\t': raw('\\'); raw('t');  break;
        default:
          if (u < 0x20) {                  // other control chars → \u00XX
            static const char hexd[] = "0123456789abcdef";
            raw('\\'); raw('u'); raw('0'); raw('0');
            raw(hexd[(u >> 4) & 0xF]); raw(hexd[u & 0xF]);
          } else {
            raw((char)u);
          }
      }
    }
    raw('"');
  }
  void u32(uint32_t v) {
    char b[11]; int n = 0;
    if (v == 0) { raw('0'); return; }
    while (v && n < (int)sizeof b) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) raw(b[--n]);
  }
  void i32(int32_t v) {
    if (v < 0) { raw('-'); u32((uint32_t)(-(int64_t)v)); }
    else u32((uint32_t)v);
  }
  // A quoted "key": prefix, with the leading comma for all but the first pair.
  void key(const char* k, bool first) {
    if (!first) raw(',');
    str(k);
    raw(':');
  }
};
}  // namespace detail

// Build the one-line manifest into `out` (NUL-terminated). Returns the number
// of bytes written (excluding the NUL), or 0 if it didn't fit — the caller then
// emits an explicit error rather than a truncated, unparseable object.
inline size_t build(const Facts& f, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  detail::Writer w{out, cap, true};

  w.raw('{');
  w.key("schema", true);        w.str(SCHEMA);
  w.key("board", false);        w.str(f.board ? f.board : "");
  w.key("firmware", false);     w.str(f.firmware ? f.firmware : "");
  w.key("git", false);          w.str(f.git ? f.git : "");
  w.key("protocol", false);     w.str(f.protocol ? f.protocol : "");
  w.key("device_id", false);    w.str(f.device_id ? f.device_id : "");
  w.key("pubkey", false);       w.str(f.pubkey_hex ? f.pubkey_hex : "");
  w.key("pubkey_fp", false);    w.str(f.pubkey_fp_hex ? f.pubkey_fp_hex : "");
  w.key("chain_head", false);   w.str(f.chain_head_hex ? f.chain_head_hex : "");
  w.key("seq", false);          w.u32(f.seq);
  w.key("boots", false);        w.u32(f.boots);
  w.key("born_day", false);
  if (f.born_day == 0) w.raw("null");
  else w.u32(f.born_day);
  w.key("born_exact", false);   w.raw(f.born_exact ? "true" : "false");
  w.key("health", false);
  if (f.health < 0) w.raw("null");
  else { int h = f.health > 100 ? 100 : f.health; w.u32((uint32_t)h); }
  // Die temperature: how warm the chip itself runs. Optional — a build (or a
  // chip) without the sensor leaves the default and readers see null.
  w.key("temp_c", false);
  if (f.temp_c <= -273) w.raw("null");
  else w.i32(f.temp_c);
  w.key("tamper", false);       w.raw(f.tamper ? "true" : "false");
  w.key("features", false);
  w.raw('[');
  for (size_t i = 0; i < f.feature_count; ++i) {
    if (i) w.raw(',');
    w.str(f.features[i] ? f.features[i] : "");
  }
  w.raw(']');
  w.key("commands", false);
  w.raw('[');
  for (size_t i = 0; i < f.command_count; ++i) {
    if (i) w.raw(',');
    char keybuf[2] = { f.commands[i].key, '\0' };
    w.raw('{');
    w.key("key", true);  w.str(keybuf);
    w.key("name", false); w.str(f.commands[i].name ? f.commands[i].name : "");
    w.raw('}');
  }
  w.raw(']');
  w.key("fleet", false);
  w.raw('[');
  for (size_t i = 0; i < f.fleet_count; ++i) {
    if (i) w.raw(',');
    const Peer& p = f.fleet[i];
    w.raw('{');
    w.key("fp", true);       w.str(p.fp ? p.fp : "");
    w.key("age", false);     w.u32(p.age_s);
    w.key("health", false);
    if (p.health < 0) w.raw("null");
    else { int h = p.health > 100 ? 100 : p.health; w.u32((uint32_t)h); }
    w.key("battery", false);
    if (p.battery < 0) w.raw("null");
    else { int b = p.battery > 100 ? 100 : p.battery; w.u32((uint32_t)b); }
    w.key("chain", false);   w.u32(p.chain);
    w.key("flags", false);   w.str(p.flags ? p.flags : "");
    w.raw('}');
  }
  w.raw(']');
  w.key("help", false);         w.str(f.help_url ? f.help_url : "");
  w.raw('}');

  if (!w.ok) { out[0] = '\0'; return 0; }
  *w.p = '\0';
  return cap - w.left;
}

}  // namespace manifest

#endif  // SECURACV_SELF_MANIFEST_H
