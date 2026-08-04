/*
 * SecuraCV — drunken-bishop randomart (host-testable, no Arduino).
 *
 * A public key or firmware hash is 32 unmemorable bytes. Randomart turns those
 * bytes into a small, STABLE picture a human can eyeball — the same primitive
 * OpenSSH prints for `ssh-keygen -lv` (sshkey.c fingerprint_randomart, the
 * "drunken bishop" walk by Loss/Limmer/von Gernler). For a *witness* device
 * this is not decoration: the operator memorises the shape once, and a swapped
 * board or tampered key draws a visibly different picture. It's identity you can
 * check with your eyes, needing no tool.
 *
 * This is a faithful reimplementation of the OpenSSH walk (BSD). It is
 * hash-agnostic: feed it any fixed byte string. We feed the device public key.
 *
 * Pure C++: compiles hosted for tests_host with -Wall -Wextra -Werror.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */
#ifndef SECURACV_RANDOMART_H
#define SECURACV_RANDOMART_H

#include <stddef.h>
#include <stdint.h>

namespace scene {

// The field is a fixed 17×9 grid of visit counters (OpenSSH FLDSIZE).
static const int RANDOMART_W = 17;
static const int RANDOMART_H = 9;

// Glyph ramp: index = a cell's visit count, clamped. The last two glyphs are
// the Start and End markers. Exactly OpenSSH's augmentation_string.
static const char RANDOMART_RAMP[] = " .o+=*BOX@%&#/^SE";

// OpenSSH's `len` = strlen(ramp) - 1 = 16. Counts grow while < len-2 (14, glyph
// '^'); the two reserved values are len-1 (15, 'S') and len (16, 'E').
static const int RANDOMART_LEN = (int)(sizeof(RANDOMART_RAMP) - 1) - 1;  // 16

// Walk the bishop over `data` and fill `field[row][col]` with clamped visit
// counts, then stamp the Start (center) and End (last square) markers. Pure and
// deterministic: identical bytes always draw the identical field.
inline void randomart_field(const uint8_t* data, size_t len,
                            uint8_t field[RANDOMART_H][RANDOMART_W]) {
  for (int r = 0; r < RANDOMART_H; ++r)
    for (int c = 0; c < RANDOMART_W; ++c) field[r][c] = 0;
  if (!data) len = 0;

  int x = RANDOMART_W / 2;  // 8
  int y = RANDOMART_H / 2;  // 4
  for (size_t i = 0; i < len; ++i) {
    int input = data[i];
    for (int b = 0; b < 4; ++b) {          // 2 bits per step, low bits first
      x += (input & 0x1) ? 1 : -1;         // bit 0 → horizontal
      y += (input & 0x2) ? 1 : -1;         // bit 1 → vertical
      if (x < 0) x = 0;                    // the bishop bumps the walls
      if (y < 0) y = 0;
      if (x > RANDOMART_W - 1) x = RANDOMART_W - 1;
      if (y > RANDOMART_H - 1) y = RANDOMART_H - 1;
      if (field[y][x] < RANDOMART_LEN - 2) field[y][x]++;
      input >>= 2;
    }
  }
  field[RANDOMART_H / 2][RANDOMART_W / 2] = (uint8_t)(RANDOMART_LEN - 1);  // 'S'
  field[y][x] = (uint8_t)RANDOMART_LEN;                                    // 'E'
}

// Map a cell's counter to its glyph (clamped into the ramp).
inline char randomart_glyph(uint8_t count) {
  int i = count;
  if (i > RANDOMART_LEN) i = RANDOMART_LEN;
  return RANDOMART_RAMP[i];
}

// Render one field row (col 0..W-1) into `out` (must hold W+1 bytes incl. NUL).
inline void randomart_row(const uint8_t field[RANDOMART_H][RANDOMART_W], int row,
                          char* out) {
  for (int c = 0; c < RANDOMART_W; ++c) out[c] = randomart_glyph(field[row][c]);
  out[RANDOMART_W] = '\0';
}

}  // namespace scene

#endif  // SECURACV_RANDOMART_H
