/*
 * SecuraCV Canary — CSI subcarrier canonicalization (L-LTF tone selection)
 *
 * WHY
 * ---
 * The ESP-IDF CSI buffer is not one thing. With lltf_en + htltf_en +
 * ltf_merge_en a 20 MHz frame delivers 128 bytes (L-LTF, 64 tones) for a
 * non-HT PPDU — every router beacon, most management traffic, ICMP replies
 * from many routers — and 256 bytes (L-LTF + HT-LTF, 128 tones) for an HT
 * PPDU. Each tone is two int8 bytes, imaginary part first. Before this file
 * the HAL copied `len/2` pairs and csi_features locked the tone count on the
 * FIRST frame of each 1 s window: a window that opened on an HT frame read
 * the following beacons' missing upper half as all-zero (bands 4..7 → large
 * fake "motion"), and a window that opened on a beacon silently truncated HT
 * frames to their L-LTF half. Both the amplitude-variance bands and the
 * CFO-corrected rotation bands were mixing two different measurements of
 * the same channel — and the twelve null tones (DC + guards) of every frame
 * were feeding the AGC mean and the per-band variance.
 *
 * WHAT
 * ----
 * Every frame is reduced to the 52 data+pilot tones of its L-LTF section, in
 * ascending frequency order (-26..-1, +1..+26). The L-LTF is present on EVERY
 * 802.11a/g/n/ax frame (it is what the receiver locks to), so the tone count
 * is now the same for a beacon, an ACK-less ping reply, and an HT data frame
 * — deterministic across traffic mixes and bandwidths, and the same 52-tone
 * canonical set the host physics tests (test_csi_features.cpp) model. This is
 * also what espressif/esp-csi does for router-driven sensing ("only LLTF
 * sub-carriers are selected" in examples/get-started/csi_recv_router).
 *
 * LAYOUT (ESP-IDF "Wi-Fi Channel State Information" table)
 * --------------------------------------------------------
 *   20 MHz PPDU, secondary channel none: the 64 L-LTF tones arrive in FFT
 *   order — tone index 0..31 then -32..-1. Null tones: 0 (DC) and ±27..±32
 *   (guards) → buffer pairs 0 and 27..37. Data+pilot tones: +1..+26 = pairs
 *   1..26; -26..-1 = pairs 38..63.
 *
 *   40 MHz PPDU: the L-LTF section is still 64 pairs but is indexed on the
 *   128-tone grid in LINEAR order (0..63 for a below-secondary, -64..-1 for
 *   above) — the primary 20 MHz sits centered at pair 32. Null tones: pairs
 *   0..5, 32 (the primary's DC) and 59..63. Data+pilot tones: -26..-1 =
 *   pairs 6..31; +1..+26 = pairs 33..58.
 *
 *   Hardware writes exact zeros at null tones, so the two maps are also
 *   DETECTABLE from the frame itself. csi_lltf_select() defaults to the map
 *   the bandwidth flag implies and only switches when the frame's own zeros
 *   contradict it — a safety net for a driver revision that changes the
 *   ordering, not something a healthy frame ever exercises.
 *
 * first_word_invalid: IDF flags frames whose first four bytes (pairs 0 and 1)
 * are hardware-corrupted. In FFT order those are DC (dropped anyway) and tone
 * +1, which is patched from tone +2 so the count and ordering never change.
 * In linear order both are guard tones and need no patch.
 *
 * Byte order is preserved as delivered ([imag, real]); csi_features'
 * magnitude is order-invariant and its rotation path only uses the sign for
 * slow-drift direction, so nothing downstream re-learns a convention.
 *
 * Header-only, <stdint.h>/<string.h> only: compiled into both HALs
 * (firmware/common/csi and firmware/canary/lib/securacv_csi) and the host
 * tests (tests_host/test_csi_subcarriers.cpp).
 *
 * License: MIT (matches the library).
 */
#ifndef SECURACV_CSI_SUBCARRIERS_H
#define SECURACV_CSI_SUBCARRIERS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define CSI_LLTF_PAIRS       64u                    /* tones in one L-LTF section */
#define CSI_LLTF_BYTES       (CSI_LLTF_PAIRS * 2u)  /* [imag, real] int8 per tone */
#define CSI_LLTF_DATA_TONES  52u                    /* after dropping DC + 11 guards */
#define CSI_LLTF_NULL_TONES  12u

typedef enum {
  CSI_LLTF_ORDER_FFT    = 0,  /* 0..31, -32..-1 (20 MHz PPDU)             */
  CSI_LLTF_ORDER_LINEAR = 1,  /* -32..31 with DC at pair 32 (40 MHz PPDU) */
} csi_lltf_order_t;

/* True if a frame carries at least one full L-LTF section. */
static inline bool csi_lltf_available(uint16_t len_bytes) {
  return len_bytes >= CSI_LLTF_BYTES;
}

/* Null-tone positions of one 64-tone L-LTF section, per ordering. */
static inline bool csi_lltf_pair_is_null(csi_lltf_order_t order, size_t k) {
  if (order == CSI_LLTF_ORDER_LINEAR) return k <= 5 || k == 32 || k >= 59;
  return k == 0 || (k >= 27 && k <= 37);
}

/*
 * How many of the 12 positions a layout calls null are (near-)zero in this
 * frame. A per-tone |I|+|Q| <= 2 tolerance survives the driver's optional
 * adjacent-tone smoothing filter, which can bleed a unit or two into a
 * guard tone.
 */
static inline uint8_t csi_lltf_null_score(const int8_t* buf, csi_lltf_order_t order) {
  uint8_t n = 0;
  for (size_t k = 0; k < CSI_LLTF_PAIRS; k++) {
    if (!csi_lltf_pair_is_null(order, k)) continue;
    const int i = buf[2 * k], q = buf[2 * k + 1];
    const int a = (i < 0 ? -i : i) + (q < 0 ? -q : q);
    if (a <= 2) n++;
  }
  return n;
}

/*
 * Pick the tone ordering for a frame: the one its bandwidth implies, unless
 * the frame's own null tones clearly (>= 10 of 12, and strictly more than
 * the expected map) say otherwise.
 */
static inline csi_lltf_order_t csi_lltf_detect_order(const int8_t* buf, bool bw40) {
  const csi_lltf_order_t expected = bw40 ? CSI_LLTF_ORDER_LINEAR : CSI_LLTF_ORDER_FFT;
  const csi_lltf_order_t other    = bw40 ? CSI_LLTF_ORDER_FFT    : CSI_LLTF_ORDER_LINEAR;
  const uint8_t e = csi_lltf_null_score(buf, expected);
  const uint8_t o = csi_lltf_null_score(buf, other);
  return (o >= 10 && o > e) ? other : expected;
}

/*
 * Reduce one ESP-IDF CSI buffer to the canonical 52 L-LTF data+pilot tones,
 * ascending frequency order, [imag, real] int8 pairs.
 *
 *   buf / len_bytes      the driver's info->buf / info->len
 *   bw40                 rx_ctrl.cwb == 1 (40 MHz PPDU)
 *   first_word_invalid   info->first_word_invalid
 *   out                  receives CSI_LLTF_DATA_TONES * 2 bytes
 *
 * Returns the tone count written (always CSI_LLTF_DATA_TONES), or 0 when the
 * buffer is too short for an L-LTF section — the caller drops that frame and
 * counts it, rather than feeding a half-measurement into the window.
 */
static inline uint8_t csi_lltf_select(const int8_t* buf, uint16_t len_bytes,
                                      bool bw40, bool first_word_invalid,
                                      int8_t* out) {
  if (buf == NULL || out == NULL || !csi_lltf_available(len_bytes)) return 0;

  const csi_lltf_order_t order = csi_lltf_detect_order(buf, bw40);
  if (order == CSI_LLTF_ORDER_LINEAR) {
    memcpy(out,          buf + 6  * 2, 26 * 2);  /* tones -26..-1 */
    memcpy(out + 26 * 2, buf + 33 * 2, 26 * 2);  /* tones +1..+26 */
    /* pairs 0 and 1 are guard tones here — first_word_invalid needs no patch */
    return (uint8_t)CSI_LLTF_DATA_TONES;
  }

  memcpy(out,          buf + 38 * 2, 26 * 2);    /* tones -26..-1 */
  memcpy(out + 26 * 2, buf + 1  * 2, 26 * 2);    /* tones +1..+26 */
  if (first_word_invalid) {
    /* tone +1 (pair 1) is corrupted: patch from tone +2 (pair 2). */
    out[26 * 2]     = buf[2 * 2];
    out[26 * 2 + 1] = buf[2 * 2 + 1];
  }
  return (uint8_t)CSI_LLTF_DATA_TONES;
}

#endif /* SECURACV_CSI_SUBCARRIERS_H */
