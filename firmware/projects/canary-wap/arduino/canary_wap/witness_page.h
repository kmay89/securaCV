/**
 * @file witness_page.h
 * @brief Pure logic for `GET /api/v1/witness?last=N` — the one witness-page
 *        contract (spec/witness_api_v1.md): a RAM ring of the newest signed
 *        records and the byte-exact JSON page rendered from it.
 *
 * WHY THIS EXISTS. The iPhone app's chain verifier reads `/api/v1/witness`
 * in the shape the canary-vision reference device-api serves. This sketch
 * used to answer only `/api/witness` — one record, a different shape, no
 * signature — so the app's headline trust feature could not run against any
 * firmware in this repository (docs/IMPROVEMENT_ROADMAP.md row 6). This
 * header renders the shared contract from what the sketch already has: the
 * per-record Ed25519 signature over the 32-byte chain hash that
 * create_witness_record() makes (the same bytes `witness_store.h` appends to
 * the SD card and `tools/verify_witness_log.py` re-verifies offline). No
 * second signing scheme is invented here.
 *
 * WHAT IS ON THE WIRE, AND WHY EACH FIELD IS THERE
 *   * `hash` / `prev_hash` / `payload_hash` / `seq` / `time_bucket` are the
 *     chain-hash pre-image, so a reader can recompute
 *     `hash = SHA-256("securacv:fw:chain:v1" || 0x00 || prev || payload_hash
 *     || seq(BE32) || time_bucket(BE32))` and bind the signature to THIS
 *     record (the same seq-binding rule witness_store.h documents for the
 *     SD tail). `time_bucket_ms` says how wide the bucket was when the
 *     record was made (a runtime setting, so it rides per record).
 *   * `signature` is Ed25519 over the raw 32-byte `hash`, hex. Absent or
 *     empty means "not individually signed" — a reader must not badge such
 *     a record verified. Every record this sketch renders carries one.
 *   * `timestamp` is COARSE — the record's bucket start, floored to the
 *     10-minute Invariant III grain, and present only when the device has
 *     met a believable wall clock (GPS-seeded; this sketch has no SNTP).
 *     Without a clock the key is simply absent, and the reader anchors
 *     `time_bucket × time_bucket_ms` against the page's `uptime_s`. The
 *     bucket itself must ride the wire (it is hashed), but a reader must
 *     never present time finer than ten minutes.
 *   * `event_type` is the record type as a vocabulary word: the dictionary's
 *     `tamper_detected` for a tamper record (so every reader's tamper
 *     severity fires), and the offline verifier's names for the rest, which
 *     have no dictionary id; `record_type` is the raw enum so nothing is
 *     lost. `zone` is the empty string — this device has no zone concept.
 *   * The device's own post-sign self-check (`WitnessRecord::verified`) is
 *     deliberately NOT on this wire: "verified" means a signature checked
 *     against a pinned key by the reader, nothing looser (AGENTS.md rule 4).
 *
 * The ring is RAM-only and starts empty on every boot — like the events
 * ring, and for the same reason (no SD read in an HTTP handler). A page
 * fetched after a reboot begins at that boot's attestation record; the full
 * history is the SD log.
 *
 * This header is pure hosted C++ (no Arduino/ESP-IDF includes) so the ring
 * arithmetic and the byte-exact page are unit-tested on the host
 * (tests_host/test_witness_page.cpp) against the shared fixture
 * spec/fixtures/witness_page_v1.json, which the iPhone app's XCTest decodes
 * too — one fixture, both ends. The sketch supplies millis()/time()/httpd
 * glue and streams the page record by record (canary_wap.ino
 * handle_witness_v1).
 */

#ifndef WITNESS_PAGE_H
#define WITNESS_PAGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace witness_page {

constexpr const char* SCHEMA       = "securacv/witness_page/v1";
constexpr const char* CHAIN_FORMAT = "wap_v1";

/* How many of the newest records the device keeps in RAM for this page.
 * 16 × ~176 B ≈ 2.8 KB of BSS. `last` is clamped to what the ring holds. */
constexpr size_t RING_CAP = 16;

/* `?last=N` bounds — the same 1..100 / default 20 the reference device-api
 * uses, so one client speaks to both. */
constexpr size_t DEFAULT_LAST = 20;
constexpr size_t MAX_LAST     = 100;

/* A wall clock below this is the boot epoch showing through, not a date
 * (the sketch's GPS_CLOCK_FLOOR; birth_day.h uses the same idea). */
constexpr uint32_t CLOCK_FLOOR_S = 1700000000u;

/* Invariant III: nothing on this wire names a time finer than ten minutes. */
constexpr uint32_t COARSE_S = 600u;

/* Buffer sizes. A record is 3×64 + 128 hex chars plus ~250 bytes of keys
 * and integers (~570 worst case), so 768 leaves margin; the header carries
 * a device id (≤ 31 chars, escaped worst case ×2) plus two integers. */
constexpr size_t HEADER_MAX = 256;
constexpr size_t RECORD_MAX = 768;
constexpr size_t FOOTER_MAX = 4;

/* Name of the raw record type on the wire. 2 maps to the dictionary's
 * `tamper_detected` (spec/witness_dictionary.json) so every reader's tamper
 * severity fires; the others are the offline verifier's names
 * (tools/verify_witness_log.py RECORD_TYPES), which have no dictionary id. */
inline const char* event_type_name(uint8_t type) {
  switch (type) {
    case 0:  return "boot_attestation";
    case 1:  return "witness_event";
    case 2:  return "tamper_detected";
    case 3:  return "state_change";
    case 4:  return "power_shutdown";
    default: return "unknown";
  }
}

struct Record {
  uint32_t seq;
  uint32_t time_bucket;   // the bucket the chain hash bound (millis / bucket_ms at creation)
  uint32_t bucket_ms;     // the bucket size in force when the record was made
  uint8_t  type;
  uint8_t  payload_hash[32];
  uint8_t  prev_hash[32];
  uint8_t  chain_hash[32];
  uint8_t  signature[64];
};

/* Fixed-capacity ring of the newest records. Zero-initialized storage is a
 * valid empty ring, so a static instance needs no constructor call. */
struct Ring {
  Record slots[RING_CAP];
  size_t next;    // slot the next push writes
  size_t count;   // 0..RING_CAP

  void clear() { next = 0; count = 0; }

  void push(const Record& r) {
    slots[next] = r;
    next = (next + 1) % RING_CAP;
    if (count < RING_CAP) count++;
  }

  /* Oldest-first pointers to the newest `n` records (n clamped to what is
   * held). `out` must have RING_CAP slots. Returns how many were written. */
  size_t newest(size_t n, const Record** out) const {
    if (n > count) n = count;
    const size_t start = (next + RING_CAP - n) % RING_CAP;
    for (size_t i = 0; i < n; i++) out[i] = &slots[(start + i) % RING_CAP];
    return n;
  }

  /* Oldest-first COPIES of the newest `n` records (n clamped to what is
   * held) into `out`, which must have RING_CAP slots. The sketch takes this
   * under a critical section and renders from the copies, so a push from
   * the main loop can never tear a record the httpd task is still
   * formatting, nor shift the ring under a chunked send that blocked. */
  size_t snapshot(size_t n, Record* out) const {
    if (n > count) n = count;
    const size_t start = (next + RING_CAP - n) % RING_CAP;
    for (size_t i = 0; i < n; i++) out[i] = slots[(start + i) % RING_CAP];
    return n;
  }
};

/* What the renderer needs from the device at the moment of the request. */
struct Context {
  const char* device_id;    // the device's own id; JSON-escaped here
  uint32_t    total;        // chain length: the newest seq this key has issued
  uint32_t    now_ms;       // millis() at render (ages the buckets)
  uint32_t    now_epoch_s;  // time(nullptr) at render; < CLOCK_FLOOR_S ⇒ no clock
};

inline void to_hex(char* out, const uint8_t* in, size_t len) {
  static const char* H = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i]     = H[in[i] >> 4];
    out[2 * i + 1] = H[in[i] & 0x0F];
  }
  out[2 * len] = '\0';
}

/* Minimal JSON string escaping for the device id: quotes and backslashes are
 * escaped, control characters dropped. Returns false when `cap` is too small. */
inline bool json_escape(char* out, size_t cap, const char* in) {
  size_t o = 0;
  if (cap == 0) return false;
  for (const char* p = in ? in : ""; *p; p++) {
    const unsigned char c = (unsigned char)*p;
    if (c < 0x20) continue;
    if (c == '"' || c == '\\') {
      if (o + 2 >= cap) return false;
      out[o++] = '\\';
      out[o++] = (char)c;
    } else {
      if (o + 1 >= cap) return false;
      out[o++] = (char)c;
    }
  }
  out[o] = '\0';
  return true;
}

/* Parse `?last=N` out of a raw query string ("last=5&x=1"). Absent or
 * malformed → DEFAULT_LAST; otherwise clamped to 1..MAX_LAST. */
inline size_t parse_last(const char* query) {
  if (query == NULL) return DEFAULT_LAST;
  const char* p = query;
  while (*p) {
    if (strncmp(p, "last=", 5) == 0 && (p == query || p[-1] == '&')) {
      p += 5;
      if (*p < '0' || *p > '9') return DEFAULT_LAST;
      unsigned long v = 0;
      while (*p >= '0' && *p <= '9') {
        v = v * 10ul + (unsigned long)(*p - '0');
        if (v > MAX_LAST) return MAX_LAST;
        p++;
      }
      if (*p != '\0' && *p != '&') return DEFAULT_LAST;
      return v < 1 ? 1 : (size_t)v;
    }
    const char* amp = strchr(p, '&');
    if (amp == NULL) break;
    p = amp + 1;
  }
  return DEFAULT_LAST;
}

/* The record's bucket start as a coarse (10-minute) Unix time, or false
 * when the device has no believable clock. millis() wrap is tolerated by
 * unsigned subtraction for uptimes under 49.7 days. */
inline bool record_epoch(const Context& c, const Record& r, uint32_t* out) {
  if (c.now_epoch_s < CLOCK_FLOOR_S) return false;
  const uint32_t start_ms = r.time_bucket * r.bucket_ms;  // ≤ millis() when the record was made
  const uint32_t age_s    = (uint32_t)(c.now_ms - start_ms) / 1000u;
  if (age_s > c.now_epoch_s) return false;
  const uint32_t epoch = c.now_epoch_s - age_s;
  *out = epoch - (epoch % COARSE_S);
  return true;
}

/* "YYYY-MM-DDTHH:MM:SSZ" (20 chars + NUL) from a Unix time. Civil-from-days
 * per Howard Hinnant's algorithm — pure integer math, no <time.h>, no
 * long-long format (newlib's printf may not carry %lld). A uint32_t epoch
 * ends in 2106, so the year is always four digits; the modulo only tells
 * the compiler so (it cannot see the input range). */
constexpr size_t ISO8601_CAP = 21;
inline void iso8601_utc(char out[ISO8601_CAP], uint32_t epoch_s) {
  const uint32_t days = epoch_s / 86400u;
  const uint32_t rem  = epoch_s % 86400u;
  const int64_t  z    = (int64_t)days + 719468;
  const int64_t  era  = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe  = (uint32_t)(z - era * 146097);
  const uint32_t yoe  = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t        y    = (int64_t)yoe + era * 400;
  const uint32_t doy  = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp   = (5 * doy + 2) / 153;
  const uint32_t d    = doy - (153 * mp + 2) / 5 + 1;
  const uint32_t m    = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) y++;
  snprintf(out, ISO8601_CAP, "%04u-%02u-%02uT%02u:%02u:%02uZ",
           (unsigned)y % 10000u, (unsigned)m % 100u, (unsigned)d % 100u,
           (unsigned)(rem / 3600u), (unsigned)((rem % 3600u) / 60u),
           (unsigned)(rem % 60u));
}

/* `{"schema":…,"chain_format":"wap_v1","device_id":"…","total":N,
 *  "uptime_s":N,"records":[` — returns the length, 0 when the buffer is
 *  too small. */
inline size_t header_build(char* buf, size_t cap, const Context& c) {
  char id[64];
  if (!json_escape(id, sizeof(id), c.device_id)) return 0;
  const int n = snprintf(buf, cap,
                         "{\"schema\":\"%s\",\"chain_format\":\"%s\","
                         "\"device_id\":\"%s\",\"total\":%u,\"uptime_s\":%u,"
                         "\"records\":[",
                         SCHEMA, CHAIN_FORMAT, id, (unsigned)c.total,
                         (unsigned)(c.now_ms / 1000u));
  if (n <= 0 || (size_t)n >= cap) return 0;
  return (size_t)n;
}

/* One record object, comma-prefixed unless `first`. Field order is fixed
 * (the fixture is byte-compared). Returns the length, 0 on overflow. */
inline size_t record_build(char* buf, size_t cap, const Record& r,
                           const Context& c, bool first) {
  char hash[65], prev[65], ph[65], sig[129];
  to_hex(hash, r.chain_hash, 32);
  to_hex(prev, r.prev_hash, 32);
  to_hex(ph,   r.payload_hash, 32);
  to_hex(sig,  r.signature, 64);

  char when[80];  // 45 fixed chars + a 20-char ISO stamp, with margin
  uint32_t epoch = 0;
  if (record_epoch(c, r, &epoch)) {
    char iso[ISO8601_CAP];
    iso8601_utc(iso, epoch);
    snprintf(when, sizeof(when),
             "\"time_source\":\"device_clock\",\"timestamp\":\"%s\",", iso);
  } else {
    when[0] = '\0';
  }

  const int n = snprintf(buf, cap,
                         "%s{\"seq\":%u,\"hash\":\"%s\",\"prev_hash\":\"%s\","
                         "\"payload_hash\":\"%s\",\"time_bucket\":%u,"
                         "\"time_bucket_ms\":%u,\"record_type\":%u,"
                         "\"event_type\":\"%s\",\"zone\":\"\",%s"
                         "\"signature\":\"%s\"}",
                         first ? "" : ",", (unsigned)r.seq, hash, prev, ph,
                         (unsigned)r.time_bucket, (unsigned)r.bucket_ms,
                         (unsigned)r.type, event_type_name(r.type), when, sig);
  if (n <= 0 || (size_t)n >= cap) return 0;
  return (size_t)n;
}

inline size_t footer_build(char* buf, size_t cap) {
  const int n = snprintf(buf, cap, "]}");
  if (n <= 0 || (size_t)n >= cap) return 0;
  return (size_t)n;
}

/* Whole page into one buffer (the host test's view; the sketch streams the
 * same three pieces as HTTP chunks). Returns the length, 0 on overflow. */
inline size_t page_build(char* buf, size_t cap, const Ring& ring, size_t last,
                         const Context& c) {
  const Record* rows[RING_CAP];
  const size_t n = ring.newest(last, rows);
  size_t pos = header_build(buf, cap, c);
  if (pos == 0) return 0;
  for (size_t i = 0; i < n; i++) {
    const size_t w = record_build(buf + pos, cap - pos, *rows[i], c, i == 0);
    if (w == 0) return 0;
    pos += w;
  }
  const size_t f = footer_build(buf + pos, cap - pos);
  if (f == 0) return 0;
  return pos + f;
}

}  // namespace witness_page

#endif  // WITNESS_PAGE_H
