/**
 * @file fleet_scan_cache.h
 * @brief The fleet scan's cache of mDNS adverts: the adverts that fit, never a
 *        cut list (sweep F211).
 *
 * GET /api/fleet/scan (canary_wap.ino handle_fleet_scan) answers the
 * _securacv._tcp adverts a short-lived worker task browsed (fleet_scan_task),
 * from a FLEET_SCAN_CACHE_SIZE-byte cache the handler deserializes:
 *
 *   {"canaries":[{"device_id":"…","name":"…","mdns_host":"…","fw":"…",
 *                 "model":"…","dt":"…","role":"…","ip":"…","port":N},…]}
 *
 * Each advert carries seven TXT values, and a TXT value is up to 255 bytes of
 * whatever the advertising device chose (`"` and `\` escape to two bytes, a
 * control byte to six), so eight adverts can need several times the cache.
 * The task used to serialize all of them with ArduinoJson's sized form and,
 * when the list did not fit, terminate the cut text by hand: the handler's
 * deserializeJson() then failed and the route answered an empty `canaries`
 * list, so the Fleet sheet showed no device at all, whichever device on the
 * LAN sent the long values.
 *
 * Here each advert is written whole or not at all (wap_json_writer.h rolls a
 * refused one back), in browse order, and the buffer holds a complete
 * document after begin() and after every add(): the closing `]}` is reserved
 * from the start and rewritten behind each kept advert. An advert that does
 * not fit in what is left is skipped and the next one is still tried, so one
 * device's long values cost that device's row, not the others'. At most
 * kMaxAdverts are kept, as before.
 *
 * Pure hosted C++ (no Arduino, ESP-IDF or heap): host-tested in
 * tests_host/test_fleet_scan_cache.cpp at the sketch's own cache size, read
 * from canary_wap.ino.
 */

#ifndef CANARY_WAP_FLEET_SCAN_CACHE_H
#define CANARY_WAP_FLEET_SCAN_CACHE_H

#include <stddef.h>
#include <stdint.h>

#include "wap_json_writer.h"

namespace fleet_scan_cache {

/* The most adverts the cache keeps (the browse cap the task always had). */
static constexpr size_t kMaxAdverts = 8;

/* One advert, as the task reads it from the mDNS results: the seven TXT
 * values (NULL reads as ""), the address and the port. */
struct Advert {
  const char* device_id;
  const char* name;
  const char* mdns_host;
  const char* fw;
  const char* model;
  const char* dt;
  const char* role;
  const char* ip;
  uint16_t    port;
};

struct Cache {
  wap_json::Writer w;
  size_t kept;      // adverts written
  size_t skipped;   // adverts that did not fit in what was left
};

/* `]}` behind the text, in the two bytes the writer reserves for it. */
inline void close_list(Cache& c) {
  if (c.w.cap < 3 || c.w.len > c.w.cap - 3) return;
  c.w.out[c.w.len] = ']';
  c.w.out[c.w.len + 1] = '}';
  c.w.out[c.w.len + 2] = '\0';
}

/* Starts `{"canaries":[]}` in out[cap]. A buffer too small for even that
 * holds "" (the handler's deserializeJson() then answers an empty list). */
inline void begin(Cache& c, char* out, size_t cap) {
  c.kept = 0;
  c.skipped = 0;
  wap_json::begin(c.w, out, cap, 2);
  if (!wap_json::raw(c.w, "{\"canaries\":[")) {
    if (cap > 0) out[0] = '\0';
    return;
  }
  close_list(c);
}

inline bool full(const Cache& c) { return c.kept >= kMaxAdverts; }

/* Appends `a` whole when it fits in what is left (and fewer than kMaxAdverts
 * are kept); otherwise leaves the document as it was. True when kept. */
inline bool add(Cache& c, const Advert& a) {
  if (full(c) || !wap_json::ok(c.w)) return false;
  const size_t at = wap_json::mark(c.w);
  wap_json::Writer& w = c.w;
  if (c.kept > 0) wap_json::raw(w, ",");
  wap_json::raw(w, "{\"device_id\":");
  wap_json::str(w, a.device_id);
  wap_json::raw(w, ",\"name\":");
  wap_json::str(w, a.name);
  wap_json::raw(w, ",\"mdns_host\":");
  wap_json::str(w, a.mdns_host);
  wap_json::raw(w, ",\"fw\":");
  wap_json::str(w, a.fw);
  wap_json::raw(w, ",\"model\":");
  wap_json::str(w, a.model);
  wap_json::raw(w, ",\"dt\":");
  wap_json::str(w, a.dt);
  wap_json::raw(w, ",\"role\":");
  wap_json::str(w, a.role);
  wap_json::raw(w, ",\"ip\":");
  wap_json::str(w, a.ip);
  wap_json::raw(w, ",\"port\":");
  wap_json::number(w, a.port);
  wap_json::raw(w, "}");
  if (!wap_json::ok(w)) {
    wap_json::rollback(w, at);
    close_list(c);
    c.skipped++;
    return false;
  }
  close_list(c);
  c.kept++;
  return true;
}

}  // namespace fleet_scan_cache

#endif  // CANARY_WAP_FLEET_SCAN_CACHE_H
