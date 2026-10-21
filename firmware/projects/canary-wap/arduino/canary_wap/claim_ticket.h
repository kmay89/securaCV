/**
 * @file claim_ticket.h
 * @brief The claim ticket: how a phone that just handed a Canary its Wi-Fi
 *        over the Bluetooth setup door ends up PAIRED without the bearer
 *        token ever riding Bluetooth.
 *
 * The setup door's link is encrypted but not authenticated (LE Secure
 * Connections Just Works — the only kind a phone with no prior bond can
 * open in one tap), so nothing that lasts belongs on it. The bearer token
 * lasts. So the device does not send it; it sends a CLAIM instead: 16
 * random bytes, minted on the loop task the moment the join the phone asked
 * for succeeded, readable once by the link that asked, good for three
 * minutes, spent by one HTTP request on the home LAN —
 * GET /api/provisioning-receipt?claim=<hex> — which answers the same receipt
 * the BOOT-tap route serves. Two factors, then: the encrypted link that
 * provisioned, AND presence on the Wi-Fi the device just joined. A phone
 * that was only near the device gets a string that opens nothing from the
 * street.
 *
 * Pure: no Arduino, no entropy source (the caller hands in the random
 * bytes — esp_fill_random on the device, a fixed pattern in the tests),
 * no clock (millis() is a parameter). Touched from the loop task (mint)
 * and the httpd task (take), so the minted-at word goes through the
 * __atomic builtins exactly as provisioning_gate.h's State does. A claim is
 * spent only by a MATCH (sweep F342): take() loads the minted-at word,
 * compares in constant time, and spends with one compare-and-swap of the
 * word it loaded (spend_if_match()), so exactly one matching taker on any
 * task gets true, and a wrong, short or truncated value changes nothing.
 * It used to exchange the word to 0 before comparing, so any ?claim= from a
 * host on the network spent the phone's claim: 128 random bits cannot be
 * guessed in the TTL, so burning on a guess bounded nothing and handed that
 * host a one-request denial. An expired claim is still burned. And the
 * owner sweeps it every loop pass with expire() on the task that mints it
 * (sweep F347): unsigned subtraction ages the minted-at word correctly only
 * below 2^32 ms, so a claim read but never spent would read as pending
 * again 49.7 days later; swept, none outlives its TTL by more than a pass.
 *
 * Consumed by the canary-wap sketch (and, in a follow-up, the flagship's
 * securacv_network); the WAP keeps a staged byte-identical copy next to its
 * Arduino sketch, held by firmware/scripts/check_improv_sync.sh.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace canary {
namespace net {
namespace claim_ticket {

/// 16 random bytes, spelled as 32 lowercase hex characters.
inline constexpr size_t RAW_LEN = 16;
inline constexpr size_t HEX_LEN = 2 * RAW_LEN;
/// How long a minted claim may be spent. The phone spends it within a
/// second or two of reading it; three minutes covers a slow .local resolve
/// and one retry, and is short enough that a claim read by a phone that
/// then walked away dies on its own.
inline constexpr uint32_t TTL_MS = 180000;
/// How far AHEAD of the taker's clock a minted-at word may read and still be
/// compared like a fresh claim (spend_if_match()): the gap between the
/// httpd task's millis() read and its load of the word, where a mint on the
/// loop task can land. A few milliseconds in practice; a second is far past
/// any preemption, and far short of an age a claim reaches unswept.
inline constexpr uint32_t AHEAD_SKEW_MS = 1000;

struct Ticket {
  char hex[HEX_LEN + 1];
  /// millis() of the mint; 0 = no claim outstanding. Atomic access only.
  uint32_t minted_at_ms;
};

/// 0 is the "none" sentinel, so a mint at millis()==0 is stored as 1.
inline uint32_t stamp(uint32_t now_ms) { return now_ms == 0 ? 1u : now_ms; }

/// The hex zeroed through a volatile pointer, so the compiler cannot drop
/// the stores. Leaves minted_at_ms alone: the caller has already cleared it
/// (wipe()) or won the compare-and-swap that did (clear_if_unchanged()).
inline void wipe_hex(Ticket& t) {
  volatile char* b = t.hex;
  for (size_t i = 0; i < sizeof(t.hex); ++i) b[i] = 0;
}

/// Clear the claim minted at `seen` (a minted-at word the caller loaded):
/// one compare-and-swap of `seen` to 0, and the hex wiped only when that
/// swap won. False, and nothing touched, when the word moved since the load
/// (a re-mint, another taker, a sweep) or `seen` is 0. The one spend that
/// spend_if_match() and expire() both make.
inline bool clear_if_unchanged(Ticket& t, uint32_t seen) {
  if (seen == 0) return false;
  uint32_t expect = seen;
  if (!__atomic_compare_exchange_n(&t.minted_at_ms, &expect, 0u, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    return false;
  }
  wipe_hex(t);
  return true;
}

inline void wipe(Ticket& t) {
  __atomic_store_n(&t.minted_at_ms, 0u, __ATOMIC_RELEASE);
  wipe_hex(t);
}

/// Mint a claim from `raw` (RAW_LEN random bytes the caller drew) at
/// `now_ms`. Replaces any outstanding claim. The hex is written before the
/// minted-at word is published, so a taker that sees the word sees the hex.
inline void mint(Ticket& t, const uint8_t raw[RAW_LEN], uint32_t now_ms) {
  __atomic_store_n(&t.minted_at_ms, 0u, __ATOMIC_RELEASE);
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < RAW_LEN; ++i) {
    t.hex[2 * i]     = digits[(raw[i] >> 4) & 0x0F];
    t.hex[2 * i + 1] = digits[raw[i] & 0x0F];
  }
  t.hex[HEX_LEN] = '\0';
  __atomic_store_n(&t.minted_at_ms, stamp(now_ms), __ATOMIC_RELEASE);
}

/// Peek: a claim is outstanding and inside its TTL. Status and tests only
/// — never a grant (take() is the grant).
inline bool pending(const Ticket& t, uint32_t now_ms, uint32_t ttl_ms = TTL_MS) {
  const uint32_t minted = __atomic_load_n(&t.minted_at_ms, __ATOMIC_ACQUIRE);
  if (minted == 0) return false;
  return (uint32_t)(now_ms - minted) < ttl_ms;
}

/// Constant-time equality over the whole HEX_LEN, so a wrong guess costs
/// the same whether it was wrong in the first character or the last.
/// `presented` must be exactly HEX_LEN characters (anything else is
/// refused before the compare, which leaks nothing a length check does
/// not already say).
inline bool hex_equal(const char* expected, const char* presented) {
  if (!expected || !presented) return false;
  size_t n = 0;
  while (n <= HEX_LEN && presented[n] != '\0') ++n;
  if (n != HEX_LEN) return false;
  unsigned diff = 0;
  for (size_t i = 0; i < HEX_LEN; ++i) {
    const unsigned a = (unsigned char)expected[i];
    unsigned b = (unsigned char)presented[i];
    if (b >= 'A' && b <= 'F') b += 'a' - 'A';   // the phone may upper-case it
    diff |= a ^ b;
  }
  return diff == 0;
}

/// Spend the claim minted at `seen` (the minted-at word the caller loaded),
/// and only on a match:
///   * `seen` 0: nothing outstanding, false;
///   * `seen` past its TTL: the claim is burned (a compare-and-swap of `seen`
///     to 0, the hex wiped only when that swap won), false. A `seen` a
///     moment AHEAD of `now_ms` (less than AHEAD_SKEW_MS) is not past it:
///     take() is handed a clock its caller read before the load, and a mint
///     on the loop task can land in between (the sketch's take(...,
///     millis()) on the httpd task). Unsigned, that word reads 2^32 ms old
///     and a wrong ?claim= would burn the phone's fresh claim, so it is
///     compared like a fresh one. Every other age, from the TTL up to
///     2^32 - AHEAD_SKEW_MS, is past it, so take() alone refuses a claim
///     left unswept for any of that span (an earlier rule read every word
///     up to 2^31 ms "ahead" and served one unswept for 24.8 days); only
///     the wrap itself needs the owner's sweep;
///   * `presented` does not match (hex_equal(): constant time, exactly
///     HEX_LEN characters): false, and NOTHING changes;
///   * a match: one compare-and-swap of `seen` to 0. It loses when the word
///     moved since the load (a re-mint, an expiry sweep or another matching
///     taker got there first): false. It wins exactly once: the hex is wiped
///     and the answer is true.
/// So a mismatch spends nothing and two matching takers on two tasks cannot
/// both get true. The seam for a caller (and a test) that loads the word
/// itself; take() is this with the word it loads now.
inline bool spend_if_match(Ticket& t, uint32_t seen, const char* presented,
                           uint32_t now_ms, uint32_t ttl_ms = TTL_MS) {
  if (seen == 0) return false;
  const uint32_t age = now_ms - seen;
  const uint32_t ahead = seen - now_ms;
  if (age >= ttl_ms && ahead >= AHEAD_SKEW_MS) {
    clear_if_unchanged(t, seen);   // burned; a re-mint since the load is left
    return false;
  }
  if (!hex_equal(t.hex, presented)) return false;
  return clear_if_unchanged(t, seen);
}

/// Spend the claim, on a match only (sweep F342): a match serves the
/// receipt and spends the claim; a wrong, short, empty or truncated value
/// is refused and spends nothing (128 random bits cannot be guessed inside
/// the TTL, so burning on a guess bounded nothing and let one stray request
/// end the phone's pairing); an expired claim is refused and burned.
/// Exactly one caller, on any task, can get true.
inline bool take(Ticket& t, const char* presented, uint32_t now_ms, uint32_t ttl_ms = TTL_MS) {
  return spend_if_match(t, __atomic_load_n(&t.minted_at_ms, __ATOMIC_ACQUIRE), presented,
                        now_ms, ttl_ms);
}

/// The sweep's step on a minted-at word `seen` its caller loaded: burn the
/// claim when `seen` is `ttl_ms` old or older, with one compare-and-swap of
/// `seen` (clear_if_unchanged()), so a re-mint stored since the load is
/// never cleared. True when it burned one. The seam a test drives with a
/// word it loaded before a re-mint; expire() is this with the word it loads
/// now.
inline bool expire_seen(Ticket& t, uint32_t seen, uint32_t now_ms, uint32_t ttl_ms = TTL_MS) {
  if (seen == 0 || (uint32_t)(now_ms - seen) < ttl_ms) return false;
  return clear_if_unchanged(t, seen);
}

/// The sweep (sweep F347): burn a claim that is `ttl_ms` old or older, with
/// one compare-and-swap of the minted-at word it read (expire_seen()), so a
/// fresh re-mint is never cleared. True when it burned one. Its owner calls
/// it every loop pass, ON THE TASK THAT MINTS (the WAP's loop task): a
/// `now_ms` read before another task's mint() would see that newer word as
/// 2^32 ms old. (So the sweep, unlike spend_if_match(), reads no word as
/// ahead of its clock, and a word it misses for any reason is still burned
/// once its age passes the TTL.) With the sweep, no claim outlives its TTL by
/// more than one pass, so none can age into the wrap of millis() and read as
/// pending again.
inline bool expire(Ticket& t, uint32_t now_ms, uint32_t ttl_ms = TTL_MS) {
  return expire_seen(t, __atomic_load_n(&t.minted_at_ms, __ATOMIC_ACQUIRE), now_ms, ttl_ms);
}

}  // namespace claim_ticket
}  // namespace net
}  // namespace canary
