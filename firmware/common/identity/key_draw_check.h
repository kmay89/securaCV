/**
 * @file key_draw_check.h
 * @brief Refuse a first-boot identity key draw that came out flat (sweep F38).
 *
 * Every tree draws its Ed25519 identity key once, on the first boot, with
 * esp_fill_random() inside bootloader_random_enable() / _disable(): the
 * canary (PIO) tree's crypto_generate_keypair(), canary-sense's,
 * canary-vision's and canary-sentinel's load_or_generate_keypair() and the
 * canary-wap sketch's generate_keypair(). Nothing looked at what came out,
 * so a buffer the RNG left as it was (32 zero bytes) or a stuck output
 * would have become the unit's identity for good. Each keygen now asks
 * is_flat() of the draw, inside the bootloader_random pair and before
 * anything derives or stores a key, and a flat draw is refused: the keygen
 * answers false, its buffer zeroed, the way a failed draw already answers,
 * so nothing is written and the next boot draws again. What the boot does
 * then is what it already did on a key it could not make: the canary (PIO)
 * tree halts provisioning; the canary-wap's provisioning fails the way a key
 * it could not store does (no halt, F58's open halt-or-ephemeral call; a
 * release build serves no web page); canary-sense, canary-vision and
 * canary-sentinel disable signing for the boot.
 *
 * A draw is flat when
 *   - every byte is the same (all zero, all 0xFF, any constant byte), or
 *   - its popcount is outside [KEY_DRAW_MIN_ONES, KEY_DRAW_MAX_ONES] =
 *     [48, 208] of its 256 bits.
 * The odds that a uniform draw is refused (a false refusal), by the union
 * bound over the two rules:
 *   P(popcount < 48) + P(popcount > 208) = 2 * sum_{k=0}^{47} C(256,k) / 2^256
 *                                        ~= 1.46e-25 ~= 2^-82.5
 *   P(every byte equal)                  = 256 / 2^256 = 2^-248
 * so about once in 2^82 first boots, under the 2^-64 bound the check was
 * set to. firmware/tests_host/test_key_draw_check.cpp computes the sum.
 *
 * What it does not prove: that a draw it passes is random. An RNG that
 * hands back a fixed but varied pattern passes, and so does one seeded the
 * same on every unit. It catches the buffer the draw never filled and an
 * output stuck at or near one value; the entropy itself is the
 * bootloader_random pair's (roadmap item 18) and the bench's (U1 K1).
 *
 * Pure, Arduino-free and host-tested (test_key_draw_check.cpp). The
 * canary-wap sketch carries a byte-identical staged copy, held by
 * firmware/scripts/check_csi_sync.sh; firmware/scripts/regression_check.sh
 * fails a keygen file that stops calling it.
 */

#ifndef SECURACV_KEY_DRAW_CHECK_H
#define SECURACV_KEY_DRAW_CHECK_H

#include <stddef.h>
#include <stdint.h>

namespace key_draw_check {

/** The identity key draw this checks: 32 bytes, 256 bits. */
static constexpr size_t KEY_DRAW_LEN = 32;
/** The fewest and the most one bits a draw may hold (inclusive). */
static constexpr unsigned KEY_DRAW_MIN_ONES = 48;
static constexpr unsigned KEY_DRAW_MAX_ONES = 208;

/** The one bits in a draw. */
inline unsigned ones(const uint8_t draw[KEY_DRAW_LEN]) {
  unsigned n = 0;
  for (size_t i = 0; i < KEY_DRAW_LEN; i++) {
    for (uint8_t b = draw[i]; b != 0; b = (uint8_t)(b & (uint8_t)(b - 1))) n++;
  }
  return n;
}

/** True when the draw is refused: every byte equal, or a popcount outside
 *  [KEY_DRAW_MIN_ONES, KEY_DRAW_MAX_ONES]. A null draw is refused. */
inline bool is_flat(const uint8_t draw[KEY_DRAW_LEN]) {
  if (draw == nullptr) return true;
  bool all_equal = true;
  for (size_t i = 1; i < KEY_DRAW_LEN; i++) {
    if (draw[i] != draw[0]) {
      all_equal = false;
      break;
    }
  }
  if (all_equal) return true;
  const unsigned n = ones(draw);
  return n < KEY_DRAW_MIN_ONES || n > KEY_DRAW_MAX_ONES;
}

}  // namespace key_draw_check

#endif /* SECURACV_KEY_DRAW_CHECK_H */
