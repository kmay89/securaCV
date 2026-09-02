// Host tests for csi_subcarriers.h — the L-LTF tone canonicalization every
// CSI frame passes through before csi_features sees it.
//
// Covered:
//   1. 20 MHz FFT-order frame (64 tones) → 52 tones, ascending frequency,
//      nulls (DC + 11 guards) gone.
//   2. An HT frame (L-LTF + HT-LTF, 128 tones) reduces to the SAME 52 tones
//      as its non-HT sibling — the mixed-count bug this file exists for.
//   3. A buffer shorter than one L-LTF section is refused (0), never padded.
//   4. first_word_invalid patches tone +1 from tone +2 (FFT order) and is a
//      no-op in linear order (pairs 0/1 are guards there).
//   5. 40 MHz frame, linear order with DC at pair 32 → the same 52 tones.
//   6. Order detection: a frame whose zeros contradict its bandwidth flag is
//      decoded by the map its zeros say; an ambiguous frame (no zeros) falls
//      back to the flag's map.
//   7. Null-score tolerance: |I|+|Q| <= 2 still reads as a null tone.
//
// Build/run: make (this dir). Header-only unit under test, no stubs needed.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "csi_subcarriers.h"

namespace {

// Tone t ∈ [-26..-1, 1..26] → a distinct [imag, real] pair we can recognize
// after reordering: imag = t, real = 100 - |t|.
void tone_value(int t, int8_t* pair) {
  pair[0] = (int8_t)t;
  pair[1] = (int8_t)(100 - (t < 0 ? -t : t));
}

int tone_of_fft_pair(size_t k) {         // FFT order: 0..31, -32..-1
  return k < 32 ? (int)k : (int)k - 64;
}
int tone_of_linear_pair(size_t k) {      // linear order: -32..31, DC at 32
  return (int)k - 32;
}

// Fill 64 pairs in the given order; null tones are exactly zero.
void build_lltf(csi_lltf_order_t order, int8_t* buf) {
  memset(buf, 0, CSI_LLTF_BYTES);
  for (size_t k = 0; k < CSI_LLTF_PAIRS; k++) {
    const int t = order == CSI_LLTF_ORDER_FFT ? tone_of_fft_pair(k) : tone_of_linear_pair(k);
    if (t == 0 || t < -26 || t > 26) continue;   // DC / guard → stays zero
    tone_value(t, buf + 2 * k);
  }
}

void assert_canonical(const int8_t* out, uint8_t n) {
  assert(n == CSI_LLTF_DATA_TONES);
  size_t i = 0;
  for (int t = -26; t <= 26; t++) {
    if (t == 0) continue;
    int8_t expect[2];
    tone_value(t, expect);
    if (out[2 * i] != expect[0] || out[2 * i + 1] != expect[1]) {
      printf("tone slot %zu: expected tone %d [%d,%d], got [%d,%d]\n",
             i, t, expect[0], expect[1], out[2 * i], out[2 * i + 1]);
      assert(false);
    }
    i++;
  }
  assert(i == CSI_LLTF_DATA_TONES);
}

void test_fft_20mhz_selects_52_in_frequency_order() {
  int8_t buf[CSI_LLTF_BYTES];
  build_lltf(CSI_LLTF_ORDER_FFT, buf);
  int8_t out[CSI_LLTF_DATA_TONES * 2];
  memset(out, 0x55, sizeof(out));
  const uint8_t n = csi_lltf_select(buf, CSI_LLTF_BYTES, false, false, out);
  assert_canonical(out, n);
  // Sanity on the null map itself.
  size_t nulls = 0;
  for (size_t k = 0; k < CSI_LLTF_PAIRS; k++) if (csi_lltf_pair_is_null(CSI_LLTF_ORDER_FFT, k)) nulls++;
  assert(nulls == CSI_LLTF_NULL_TONES);
  assert(csi_lltf_null_score(buf, CSI_LLTF_ORDER_FFT) == CSI_LLTF_NULL_TONES);
  printf("  fft 20 MHz → 52 canonical tones: ok\n");
}

void test_ht_frame_matches_non_ht_sibling() {
  int8_t ht[CSI_LLTF_BYTES * 2];
  build_lltf(CSI_LLTF_ORDER_FFT, ht);
  // The HT-LTF section: any junk — it must be ignored.
  for (size_t i = CSI_LLTF_BYTES; i < sizeof(ht); i++) ht[i] = (int8_t)(i * 7);
  int8_t out_ht[CSI_LLTF_DATA_TONES * 2], out_nonht[CSI_LLTF_DATA_TONES * 2];
  const uint8_t n1 = csi_lltf_select(ht, (uint16_t)sizeof(ht), false, false, out_ht);
  const uint8_t n2 = csi_lltf_select(ht, CSI_LLTF_BYTES, false, false, out_nonht);
  assert(n1 == n2 && n1 == CSI_LLTF_DATA_TONES);
  assert(memcmp(out_ht, out_nonht, sizeof(out_ht)) == 0);
  assert_canonical(out_ht, n1);
  printf("  HT (256 B) and non-HT (128 B) frames → identical 52 tones: ok\n");
}

void test_short_buffer_refused() {
  int8_t buf[CSI_LLTF_BYTES];
  build_lltf(CSI_LLTF_ORDER_FFT, buf);
  int8_t out[CSI_LLTF_DATA_TONES * 2];
  assert(csi_lltf_select(buf, CSI_LLTF_BYTES - 2, false, false, out) == 0);
  assert(csi_lltf_select(buf, 0, false, false, out) == 0);
  assert(csi_lltf_select(nullptr, CSI_LLTF_BYTES, false, false, out) == 0);
  assert(csi_lltf_select(buf, CSI_LLTF_BYTES, false, false, nullptr) == 0);
  assert(!csi_lltf_available(127) && csi_lltf_available(128));
  printf("  short / null buffers refused: ok\n");
}

void test_first_word_invalid_patch() {
  int8_t buf[CSI_LLTF_BYTES];
  build_lltf(CSI_LLTF_ORDER_FFT, buf);
  buf[2] = 0x7F; buf[3] = (int8_t)0x80;         // corrupt tone +1 (pair 1)
  int8_t out[CSI_LLTF_DATA_TONES * 2];
  const uint8_t n = csi_lltf_select(buf, CSI_LLTF_BYTES, false, true, out);
  assert(n == CSI_LLTF_DATA_TONES);
  // slot 26 is tone +1; it must now carry tone +2's value.
  int8_t t2[2]; tone_value(2, t2);
  assert(out[26 * 2] == t2[0] && out[26 * 2 + 1] == t2[1]);
  // Everything else untouched.
  int8_t tm1[2]; tone_value(-1, tm1);
  assert(out[25 * 2] == tm1[0] && out[25 * 2 + 1] == tm1[1]);
  // Without the flag the corrupted value passes through (caller's call).
  csi_lltf_select(buf, CSI_LLTF_BYTES, false, false, out);
  assert(out[26 * 2] == 0x7F);
  // Linear order: pairs 0/1 are guards; flag is a no-op.
  int8_t lin[CSI_LLTF_BYTES];
  build_lltf(CSI_LLTF_ORDER_LINEAR, lin);
  int8_t a[CSI_LLTF_DATA_TONES * 2], b[CSI_LLTF_DATA_TONES * 2];
  csi_lltf_select(lin, CSI_LLTF_BYTES, true, true, a);
  csi_lltf_select(lin, CSI_LLTF_BYTES, true, false, b);
  assert(memcmp(a, b, sizeof(a)) == 0);
  printf("  first_word_invalid patch: ok\n");
}

void test_linear_40mhz_selects_same_52() {
  int8_t buf[CSI_LLTF_BYTES * 3];               // 40 MHz HT: 128 + 256 bytes
  build_lltf(CSI_LLTF_ORDER_LINEAR, buf);
  for (size_t i = CSI_LLTF_BYTES; i < sizeof(buf); i++) buf[i] = (int8_t)(i * 3);
  int8_t out[CSI_LLTF_DATA_TONES * 2];
  const uint8_t n = csi_lltf_select(buf, (uint16_t)sizeof(buf), true, false, out);
  assert_canonical(out, n);
  size_t nulls = 0;
  for (size_t k = 0; k < CSI_LLTF_PAIRS; k++) if (csi_lltf_pair_is_null(CSI_LLTF_ORDER_LINEAR, k)) nulls++;
  assert(nulls == CSI_LLTF_NULL_TONES);
  printf("  linear 40 MHz → the same 52 canonical tones: ok\n");
}

void test_order_detection() {
  int8_t fft[CSI_LLTF_BYTES], lin[CSI_LLTF_BYTES];
  build_lltf(CSI_LLTF_ORDER_FFT, fft);
  build_lltf(CSI_LLTF_ORDER_LINEAR, lin);
  // Flag says the expected map; zeros agree.
  assert(csi_lltf_detect_order(fft, false) == CSI_LLTF_ORDER_FFT);
  assert(csi_lltf_detect_order(lin, true)  == CSI_LLTF_ORDER_LINEAR);
  // Flag contradicts the frame; zeros win.
  assert(csi_lltf_detect_order(fft, true)  == CSI_LLTF_ORDER_FFT);
  assert(csi_lltf_detect_order(lin, false) == CSI_LLTF_ORDER_LINEAR);
  int8_t out[CSI_LLTF_DATA_TONES * 2];
  assert_canonical(out, csi_lltf_select(lin, CSI_LLTF_BYTES, false, false, out));
  assert_canonical(out, csi_lltf_select(fft, CSI_LLTF_BYTES, true,  false, out));
  // Ambiguous frame (no zeros anywhere): the flag decides.
  int8_t amb[CSI_LLTF_BYTES];
  for (size_t i = 0; i < sizeof(amb); i++) amb[i] = 5;
  assert(csi_lltf_detect_order(amb, false) == CSI_LLTF_ORDER_FFT);
  assert(csi_lltf_detect_order(amb, true)  == CSI_LLTF_ORDER_LINEAR);
  printf("  order detection (agree / contradict / ambiguous): ok\n");
}

void test_null_score_tolerance() {
  int8_t buf[CSI_LLTF_BYTES];
  build_lltf(CSI_LLTF_ORDER_FFT, buf);
  // Smoothing bleed: |I|+|Q| == 2 at every null still counts as null…
  for (size_t k = 0; k < CSI_LLTF_PAIRS; k++) {
    if (!csi_lltf_pair_is_null(CSI_LLTF_ORDER_FFT, k)) continue;
    buf[2 * k] = 1; buf[2 * k + 1] = -1;
  }
  assert(csi_lltf_null_score(buf, CSI_LLTF_ORDER_FFT) == CSI_LLTF_NULL_TONES);
  // …but 3 does not.
  for (size_t k = 0; k < CSI_LLTF_PAIRS; k++) {
    if (!csi_lltf_pair_is_null(CSI_LLTF_ORDER_FFT, k)) continue;
    buf[2 * k] = 2; buf[2 * k + 1] = -1;
  }
  assert(csi_lltf_null_score(buf, CSI_LLTF_ORDER_FFT) == 0);
  printf("  null-score tolerance: ok\n");
}

}  // namespace

int main() {
  printf("test_csi_subcarriers\n");
  test_fft_20mhz_selects_52_in_frequency_order();
  test_ht_frame_matches_non_ht_sibling();
  test_short_buffer_refused();
  test_first_word_invalid_patch();
  test_linear_40mhz_selects_same_52();
  test_order_detection();
  test_null_score_tolerance();
  printf("test_csi_subcarriers: ALL PASSED\n");
  return 0;
}
