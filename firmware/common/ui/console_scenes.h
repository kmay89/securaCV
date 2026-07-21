/*
 * SecuraCV — console scenes (host-testable, no Arduino).
 *
 * The content, composed from the pure engine (console_theme.h) + randomart.h.
 * The centrepiece is the TRUST CARD: not decoration, but a framed identity the
 * operator can verify by eye — device id, firmware+git, chain head, self-test
 * score, and the device public key rendered as drunken-bishop randomart. A
 * swapped board or tampered key draws a visibly different picture. The polish
 * signals mastery; the content proves identity.
 *
 * Everything here is 7-bit ASCII content inside engine-drawn borders, so it
 * renders correctly at every capability tier (see console_theme.h).
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */
#ifndef SECURACV_CONSOLE_SCENES_H
#define SECURACV_CONSOLE_SCENES_H

#include "ui/console_theme.h"
#include "ui/randomart.h"

namespace scene {

struct TrustInfo {
  const char* device_id;
  const char* firmware;        // "2.2.0"
  const char* git;             // "abc1234" or "not-embedded"
  const char* built;           // "2026-07-21"
  const char* chain_head_hex;  // short hex (e.g. 8 chars)
  const char* key_fp_hex;      // 16 hex chars (pubkey_fp)
  uint32_t seq;
  uint32_t boots;
  int health;                  // 0..100, <0 = unknown
  bool tamper;
  const uint8_t* key_bytes;    // fed to randomart (the device public key)
  size_t key_len;
};

static const int TRUST_INNER = 52;  // interior columns; total width 54

// The title block + verifiable identity, drawn as one framed panel.
inline void trust_card(const Renderer& r, const TrustInfo& t) {
  const int inner = TRUST_INNER;
  char line[128];

  hrule(r, "SecuraCV  -  Canary witness", inner, 0);

  // A tiny 7-bit canary (renders on every terminal), centred + moss-green.
  static const char* const art[3] = { " ,_,", "(o.o)", "/) )\\" };
  for (int i = 0; i < 3; ++i) {
    center_into(line, sizeof line, art[i], inner - 2);
    row(r, inner, COL_MOSS, line);
  }
  row_blank(r, inner);

  // Identity — plain rows.
  rowf(r, inner, COL_NONE, "Device    %s", t.device_id ? t.device_id : "?");
  rowf(r, inner, COL_NONE, "Firmware  %s (%s)   built %s",
       t.firmware ? t.firmware : "?", t.git ? t.git : "?", t.built ? t.built : "?");
  rowf(r, inner, COL_NONE, "Chain     %s  seq %u  boots %u",
       t.chain_head_hex ? t.chain_head_hex : "?",
       (unsigned)t.seq, (unsigned)t.boots);
  // Health gets a colour AND a word (never colour alone).
  {
    int hc = (t.health < 0) ? COL_DIM : (t.health >= 100 ? COL_MOSS : COL_AMBER);
    char hl[64];
    if (t.health < 0) snprintf(hl, sizeof hl, "Health    n/a  (unknown)");
    else snprintf(hl, sizeof hl, "Health    %d%%  %s", t.health,
                  t.health >= 100 ? "nominal" : "degraded");
    row(r, inner, hc, hl);
  }
  rowf(r, inner, COL_NONE, "Key FP    %s", t.key_fp_hex ? t.key_fp_hex : "?");

  hrule(r, "public-key randomart  (eyeball this)", inner, 1);

  // The randomart, in its own OpenSSH-style ASCII box, centred in the panel.
  uint8_t field[RANDOMART_H][RANDOMART_W];
  randomart_field(t.key_bytes, t.key_len, field);
  char border[RANDOMART_W + 3];
  border[0] = '+';
  for (int i = 0; i < RANDOMART_W; ++i) border[1 + i] = '-';
  border[1 + RANDOMART_W] = '+';
  border[2 + RANDOMART_W] = '\0';

  center_into(line, sizeof line, border, inner - 2);
  row(r, inner, COL_DIM, line);
  for (int y = 0; y < RANDOMART_H; ++y) {
    char rr[RANDOMART_W + 1];
    randomart_row(field, y, rr);
    char boxed[RANDOMART_W + 3];
    boxed[0] = '|';
    memcpy(boxed + 1, rr, RANDOMART_W);
    boxed[1 + RANDOMART_W] = '|';
    boxed[2 + RANDOMART_W] = '\0';
    center_into(line, sizeof line, boxed, inner - 2);
    row(r, inner, COL_BRAND, line);
  }
  center_into(line, sizeof line, border, inner - 2);
  row(r, inner, COL_DIM, line);

  row_blank(r, inner);
  row(r, inner, COL_DIM, "Memorise this shape - it changes if the key does.");
  if (t.tamper)
    row(r, inner, COL_AMBER, "!! TAMPER FLAG SET - investigate before trusting");
  hrule(r, "", inner, 2);
}

}  // namespace scene

#endif  // SECURACV_CONSOLE_SCENES_H
