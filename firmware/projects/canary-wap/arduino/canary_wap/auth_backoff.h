// auth_backoff.h — the bearer lockout, one backoff per source address.
//
// Pure: <stdint.h> only, no Arduino, no httpd, no heap. Host-tested in
// firmware/tests_host/test_auth_backoff.cpp. Two consumers, one table each:
//   * The canary-wap sketch compiles only its own folder, so it stages a
//     byte-identical copy (firmware/scripts/check_improv_sync.sh holds the
//     two equal), and its api_auth.h holds the one Table every file of the
//     sketch shares, keyed by auth_request_source() (canary_wap.ino).
//   * The canary (PIO) tree reads this file by path (-I firmware/common):
//     its AuthManager (lib/securacv_auth) holds that device's Table, keyed by
//     request_peer_ipv4() (securacv_network.cpp), the same peer mapping
//     (sweep F340). Host-tested over the real library in
//     firmware/tests_host/test_securacv_auth.cpp.
//
// Why per address (sweep F343). The WAP's lockout used to be one backoff for
// every caller, so one host's wrong bearers held every other host's right
// token at 429. The likeliest such host was the owner's own phone, still
// polling a re-flashed unit with its old token (a full erase gives it a new
// key, so a new token and device_id; the 10 s BOOT reset leaves the token as
// it was). A Table keeps one Slot per source address instead, so a host's
// wrong bearers hold only that host's address.
//
// The contract:
//   * The source is the request's socket peer address (the caller reads it
//     with getpeername), never a request header: X-Forwarded-For and the
//     rest are the client's to write. 0 is a real key, the bucket every
//     source the caller cannot name (an unreadable socket, a real IPv6
//     peer) shares.
//   * Addresses live in RAM only. This header never logs, persists or
//     answers one, and neither may its callers.
//   * One httpd task per tree touches a Table, so it takes no lock.
//   * SLOTS sources are tracked, each in its own Slot. A source that finds
//     no free Slot strikes the one shared overflow Slot, and while the
//     overflow's backoff runs it holds EVERY untracked source. So a host
//     rotating through more addresses than there are Slots still meets a
//     bound, which evicting the oldest Slot would lose: the overflow holds
//     the untracked addresses, the owner's included, and never a tracked
//     one.
//   * The backoff is BASE_MS after the first strike, doubling with each
//     strike, capped at the window: 2, 4, 8, 16, 32, then 60 s, and nothing
//     waits longer. WINDOW_MS after a Slot's last strike the Slot is free
//     again (its count cleared). The cap is the window (MAX_MS ==
//     WINDOW_MS) because a longer one never runs: the window clears the
//     count first. The old 5-minute cap was such a cap, and its sixth
//     strike announced 65 s for a hold the window ended at 60 s.
//   * The count saturates at MAX_COUNT, so strikes recorded inside a backoff
//     (a caller that records without asking locked() first) can never wrap
//     it back to a clean Slot.
//   * Ages are read modulo 2^32 ms ((uint32_t)(now - then), and a signed
//     difference for "still running"), so the 49.7-day wrap of millis()
//     neither ends a backoff early nor starts one. A Slot left untouched
//     across a whole wrap can read as recent once, for at most its own
//     remaining backoff.
//   * A success never clears the overflow: record_success(), a right bearer
//     or a tap that hands an address the token frees only that address's
//     own tracked Slot.
//   * grant() never compares while the source is held: a guess sent during
//     a backoff learns nothing and records nothing.
#pragma once

#include <stdint.h>

namespace canary {
namespace net {
namespace auth_backoff {

inline constexpr uint32_t BASE_MS = 2000;     // the first strike's backoff
inline constexpr uint32_t WINDOW_MS = 60000;  // a Slot clean this long is free
inline constexpr uint32_t MAX_MS = WINDOW_MS; // no backoff is longer
static_assert(MAX_MS <= WINDOW_MS, "no backoff outlives the window that clears its count");
inline constexpr uint8_t SLOTS = 8;           // tracked sources
inline constexpr uint8_t MAX_COUNT = 10;      // the count saturates here

// One source's record. failures == 0 means free (source and the stamps are
// then meaningless).
struct Slot {
  uint32_t source;
  uint32_t last_failure_ms;
  uint32_t until_ms;
  uint8_t failures;
};

struct Table {
  Slot slot[SLOTS];
  Slot overflow;  // every untracked source, while it runs
};

// grant()'s answer. NONE: no credential presented, nothing recorded. OK: the
// right credential, the source's Slot freed. WRONG: a wrong or malformed one,
// one strike. HELD: the source's backoff runs, nothing compared or recorded.
enum class BearerResult : uint8_t { NONE, OK, WRONG, HELD };

// True while `s` holds its source: a live Slot whose backoff ends after now.
inline bool running(const Slot& s, uint32_t now) {
  return s.failures != 0 && (int32_t)(s.until_ms - now) > 0;
}

// Frees every Slot, the overflow included, whose last strike is more than
// WINDOW_MS old. Every function below that takes `now` calls it first.
inline void settle(Table& t, uint32_t now) {
  for (uint8_t i = 0; i < SLOTS; ++i) {
    Slot& s = t.slot[i];
    if (s.failures != 0 && (uint32_t)(now - s.last_failure_ms) > WINDOW_MS) s = Slot{};
  }
  if (t.overflow.failures != 0 && (uint32_t)(now - t.overflow.last_failure_ms) > WINDOW_MS) {
    t.overflow = Slot{};
  }
}

// The live Slot that tracks `source`, or nullptr. Never the overflow.
inline Slot* tracked(Table& t, uint32_t source) {
  for (uint8_t i = 0; i < SLOTS; ++i) {
    if (t.slot[i].failures != 0 && t.slot[i].source == source) return &t.slot[i];
  }
  return nullptr;
}

// Whether `source` is held now: by its own Slot when it has one, else by the
// overflow.
inline bool locked(Table& t, uint32_t source, uint32_t now) {
  settle(t, now);
  const Slot* s = tracked(t, source);
  return running(s ? *s : t.overflow, now);
}

// The milliseconds left of the backoff locked() reads for `source`, 0 once it
// has ended. Never a wrapped difference.
inline uint32_t remaining_ms(Table& t, uint32_t source, uint32_t now) {
  settle(t, now);
  const Slot* s = tracked(t, source);
  const Slot& r = s ? *s : t.overflow;
  return running(r, now) ? (uint32_t)(r.until_ms - now) : 0u;
}

// One strike for `source`: its tracked Slot, else the first free one, else
// the overflow. Returns the backoff it armed, in ms.
inline uint32_t record_failure(Table& t, uint32_t source, uint32_t now) {
  settle(t, now);
  Slot* s = tracked(t, source);
  if (!s) {
    for (uint8_t i = 0; i < SLOTS && !s; ++i) {
      if (t.slot[i].failures == 0) {
        s = &t.slot[i];
        *s = Slot{};
        s->source = source;
      }
    }
  }
  if (!s) s = &t.overflow;
  if (s->failures < MAX_COUNT) ++s->failures;
  s->last_failure_ms = now;
  uint32_t ms = BASE_MS;
  for (uint8_t i = 1; i < s->failures && ms < MAX_MS; ++i) ms *= 2;
  if (ms > MAX_MS) ms = MAX_MS;
  s->until_ms = now + ms;
  return ms;
}

// The right credential from `source`: its tracked Slot is freed. The overflow
// is never cleared by a success.
inline void record_success(Table& t, uint32_t source) {
  Slot* s = tracked(t, source);
  if (s) *s = Slot{};
}

// The strikes `source`'s own Slot holds now, 0 when it has none.
inline uint8_t failures_of(Table& t, uint32_t source, uint32_t now) {
  settle(t, now);
  const Slot* s = tracked(t, source);
  return s ? s->failures : (uint8_t)0;
}

// The whole bearer decision for one request. `presented`: the request
// carries a credential at all (an Authorization header). `valid`: a callable
// returning whether it is the right one, called at most once, and never
// while the source is held.
template <class Valid>
inline BearerResult grant(Table& t, uint32_t source, uint32_t now, bool presented, Valid valid) {
  if (!presented) return BearerResult::NONE;
  if (locked(t, source, now)) return BearerResult::HELD;
  if (valid()) {
    record_success(t, source);
    return BearerResult::OK;
  }
  record_failure(t, source, now);
  return BearerResult::WRONG;
}

}  // namespace auth_backoff
}  // namespace net
}  // namespace canary
