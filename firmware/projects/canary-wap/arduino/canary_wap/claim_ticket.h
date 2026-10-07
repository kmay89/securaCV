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
 * __atomic builtins exactly as provisioning_gate.h's State does, and take()
 * claims the ticket with one exchange BEFORE it compares — a second taker
 * on any task reads 0 and is refused, whatever it presented.
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

struct Ticket {
  char hex[HEX_LEN + 1];
  /// millis() of the mint; 0 = no claim outstanding. Atomic access only.
  uint32_t minted_at_ms;
};

/// 0 is the "none" sentinel, so a mint at millis()==0 is stored as 1.
inline uint32_t stamp(uint32_t now_ms) { return now_ms == 0 ? 1u : now_ms; }

inline void wipe(Ticket& t) {
  __atomic_store_n(&t.minted_at_ms, 0u, __ATOMIC_RELEASE);
  volatile char* b = t.hex;
  for (size_t i = 0; i < sizeof(t.hex); ++i) b[i] = 0;
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

/// Spend the claim. The ticket is consumed by this call whatever the
/// outcome — a match serves the receipt, a mismatch burns the claim (a
/// guesser gets one guess per join, not thousands), an expired claim is
/// burned too. Exactly one caller, on any task, can get true.
inline bool take(Ticket& t, const char* presented, uint32_t now_ms, uint32_t ttl_ms = TTL_MS) {
  const uint32_t minted = __atomic_exchange_n(&t.minted_at_ms, 0u, __ATOMIC_ACQ_REL);
  if (minted == 0) return false;
  const bool fresh = (uint32_t)(now_ms - minted) < ttl_ms;
  const bool match = hex_equal(t.hex, presented);
  volatile char* b = t.hex;
  for (size_t i = 0; i < sizeof(t.hex); ++i) b[i] = 0;
  return fresh && match;
}

}  // namespace claim_ticket
}  // namespace net
}  // namespace canary
