/**
 * @file fleet_scan_cache.h
 * @brief The fleet scan's cache of mDNS adverts: the shortest adverts that
 *        fit, never a cut list (sweep F211).
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
 * The task used to serialize the first eight results with ArduinoJson's
 * sized form and, when they did not fit, terminate the cut text by hand: the
 * handler's deserializeJson() then failed and the route answered an empty
 * `canaries` list, so the Fleet sheet showed this device and no other
 * Canary, whichever device on the LAN sent the long values.
 *
 * Here the task hands fill() every result of the browse. fill() measures each
 * advert's row, keeps the shortest rows that fit in the cache together (at
 * most kMaxAdverts, as before; of two equally long, the one browsed first),
 * and writes them in browse order. So a long advert never costs a shorter
 * one its row: when not every advert fits, the longest are the ones left
 * out, and as many are kept as can fit. (Kept in browse order instead, one
 * long advert answered early would crowd out every advert after it.) Each
 * row is written whole or not at all (wap_json_writer.h rolls a refused one
 * back), and the buffer holds a complete document after begin() and after
 * every add(): the closing `]}` is reserved from the start and rewritten
 * behind each kept row.
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
  size_t skipped;   // adverts offered and not written
};

/* One advert's row, without the comma in front of it. */
inline void write_row(wap_json::Writer& w, const Advert& a) {
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
}

/* The bytes `a`'s row spends, without the comma in front of it. */
inline size_t row_len(const Advert& a) {
  wap_json::Writer m;
  wap_json::begin_measure(m);
  write_row(m, a);
  return m.len;
}

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

/* The bytes left for rows and the commas between them (0 when begin() could
 * not start the list). */
inline size_t room(const Cache& c) {
  if (!wap_json::ok(c.w)) return 0;
  return c.w.cap - 1 - c.w.reserve - c.w.len;
}

/* Appends `a` whole when it fits in what is left (and fewer than kMaxAdverts
 * are kept); otherwise leaves the document as it was. True when kept. */
inline bool add(Cache& c, const Advert& a) {
  if (full(c) || !wap_json::ok(c.w)) return false;
  const size_t at = wap_json::mark(c.w);
  wap_json::Writer& w = c.w;
  if (c.kept > 0) wap_json::raw(w, ",");
  write_row(w, a);
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

/* Which of the browse's results fill() writes: their indexes and row
 * lengths, at most kMaxAdverts. */
struct Pick {
  int    index[kMaxAdverts];
  size_t len[kMaxAdverts];
  size_t count;
};

inline void pick_begin(Pick& p) { p.count = 0; }

/* The entry to drop first: the longest row, of two equally long the one
 * browsed later. */
inline size_t pick_longest(const Pick& p) {
  size_t at = 0;
  for (size_t k = 1; k < p.count; ++k) {
    if (p.len[k] > p.len[at] || (p.len[k] == p.len[at] && p.index[k] > p.index[at])) at = k;
  }
  return at;
}

/* Offers result `index`, whose row is `len` bytes; results are offered in
 * browse order. Keeps the kMaxAdverts shortest offered so far: a new one
 * replaces the longest kept only when it is shorter, so of two equally long
 * the one browsed first stays. */
inline void pick_offer(Pick& p, int index, size_t len) {
  if (p.count < kMaxAdverts) {
    p.index[p.count] = index;
    p.len[p.count] = len;
    p.count++;
    return;
  }
  const size_t at = pick_longest(p);
  if (len < p.len[at]) {
    p.index[at] = index;
    p.len[at] = len;
  }
}

/* The picked rows' bytes, with a comma between each two. */
inline size_t pick_bytes(const Pick& p) {
  size_t n = p.count > 0 ? p.count - 1 : 0;
  for (size_t k = 0; k < p.count; ++k) n += p.len[k];
  return n;
}

/* Drops the longest picked row until the rest fit in `room` bytes, then
 * puts the rest in browse order. The rows left are the shortest that fit
 * together: dropping the longest first leaves as many as can fit. */
inline void pick_finish(Pick& p, size_t room) {
  while (p.count > 0 && pick_bytes(p) > room) {
    const size_t at = pick_longest(p);
    p.count--;
    p.index[at] = p.index[p.count];
    p.len[at] = p.len[p.count];
  }
  for (size_t k = 1; k < p.count; ++k) {
    const int index = p.index[k];
    const size_t len = p.len[k];
    size_t j = k;
    for (; j > 0 && p.index[j - 1] > index; --j) {
      p.index[j] = p.index[j - 1];
      p.len[j] = p.len[j - 1];
    }
    p.index[j] = index;
    p.len[j] = len;
  }
}

/* The task's browse, whole: offers every one of the browse's `n` results
 * (`read(i)` gives result i's Advert, its strings valid until the next call),
 * keeps the shortest rows that fit together, at most kMaxAdverts, and
 * writes them in browse order. A kept result is read twice, once to measure
 * it and once to write it; a result that reads longer the second time is
 * skipped by add(), so the cache stays a complete document whatever the
 * results do. Returns the number kept. */
template <typename Read>
inline size_t fill(Cache& c, int n, Read&& read) {
  Pick p;
  pick_begin(p);
  for (int i = 0; i < n; ++i) pick_offer(p, i, row_len(read(i)));
  pick_finish(p, room(c));
  c.skipped += (n > 0 ? (size_t)n : 0) - p.count;
  for (size_t k = 0; k < p.count; ++k) add(c, read(p.index[k]));
  return c.kept;
}

}  // namespace fleet_scan_cache

#endif  // CANARY_WAP_FLEET_SCAN_CACHE_H
