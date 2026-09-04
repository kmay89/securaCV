/*
 * SecuraCV — console scenes (host-testable, no Arduino).
 *
 * The content, composed from the pure engine (console_theme.h) + randomart.h.
 * The centerpiece is the TRUST CARD: not decoration, but a framed identity the
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

// THE Canary logo — one silhouette, single-sourced, so the device you plug in
// and the site it sends you to (securacv.com/canary) show the identical bird.
// Each line is 7-bit and exactly 5 columns wide, so the three stack in a clean
// vertical column at any center. Change it here and every scene follows.
inline constexpr const char* const CANARY_LOGO[3] = { ",___,", "(o.o)", "/)_/)" };

// Draw the logo centered in the panel interior, moss-green (renders on every
// terminal; color is a bonus, never the meaning).
inline void draw_canary(const Renderer& r, int inner) {
  char line[128];
  for (int i = 0; i < 3; ++i) {
    center_into(line, sizeof line, CANARY_LOGO[i], inner - 2);
    row(r, inner, COL_MOSS, line);
  }
}

// Tiny, honest mood theater for the USB serial monitor. These are not alerts
// and never identify people; they make every operator-facing state feel tended
// without changing the facts. The bird's expression is ASCII-only and each
// scene includes plain guidance so warmth never replaces usability.
enum class CanaryScene : uint8_t {
  Setup, Pairing, AllQuiet, SelfTest, Updating, LinkDown, LateWitness,
  LostWitness, AlarmHandoff, Night,
};

struct CanarySceneDef {
  CanaryScene scene;
  const char* title;
  const char* face;
  const char* body;
  const char* line1;
  const char* line2;
  int color;
};

inline const CanarySceneDef& canary_scene_def(CanaryScene s) {
  static constexpr CanarySceneDef defs[] = {
      {CanaryScene::Setup, "Let's find the perch", "(^.^)", "/)_/)",
       "I will only ask for what I need.", "You stay in charge of setup.", COL_MOSS},
      {CanaryScene::Pairing, "Making a new friend", "(o.o)", "/)>/)",
       "Check the code on both screens.", "Two yeses make one trusted fleet.", COL_BRAND},
      {CanaryScene::AllQuiet, "All quiet", "(-.-)", "/)_/)",
       "Everyone checked in and the chain verifies.", "Nothing needs you right now.", COL_MOSS},
      {CanaryScene::SelfTest, "Stretching my wings", "(o.o)", "/)>/)",
       "Running local checks only.", "I will show words, not just colors.", COL_BRAND},
      {CanaryScene::Updating, "Fresh feathers", "(o.o)", "/)~/)",
       "Saving a signed update.", "I keep the old copy until this proves out.", COL_BRAND},
      {CanaryScene::LinkDown, "Listening for home", "(?.?)", "/)_/)",
       "The hub or Wi-Fi link is quiet.", "Records stay local while I wait.", COL_AMBER},
      {CanaryScene::LateWitness, "Looking for a fleetmate", "(o.?)", "/)_/)",
       "One witness is late past grace.", "No people, places, or secrets are shown.", COL_AMBER},
      {CanaryScene::LostWitness, "Calling softly", "(o!)", "/)>/)",
       "A witness is missing.", "Check power nearby when you have a moment.", COL_AMBER},
      {CanaryScene::AlarmHandoff, "Instrument panel has the stage", "     ", "     ",
       "No cute face during a real alarm.", "Read the signed facts above.", COL_TEXT},
      {CanaryScene::Night, "Nestled for quiet hours", "(-.-)", "/)__)",
       "Night mode means fewer flourishes.", "Still watching the system, not watching you.", COL_DIM},
  };
  for (const auto& d : defs) if (d.scene == s) return d;
  return defs[0];
}

inline void draw_canary_pose(const Renderer& r, int inner, const char* face,
                             const char* body, int color) {
  char line[128];
  center_into(line, sizeof line, CANARY_LOGO[0], inner - 2); row(r, inner, color, line);
  center_into(line, sizeof line, face ? face : CANARY_LOGO[1], inner - 2); row(r, inner, color, line);
  center_into(line, sizeof line, body ? body : CANARY_LOGO[2], inner - 2); row(r, inner, color, line);
}

inline void mood_card(const Renderer& r, CanaryScene s, const char* detail) {
  const int inner = TRUST_INNER;
  const CanarySceneDef& d = canary_scene_def(s);
  hrule(r, d.title, inner, 0);
  draw_canary_pose(r, inner, d.face, d.body, d.color);
  row_blank(r, inner);
  row(r, inner, d.color, d.line1);
  row(r, inner, COL_NONE, d.line2);
  if (detail && *detail) {
    row_blank(r, inner);
    rowf(r, inner, COL_DIM, "Note      %s", detail);
  }
  hrule(r, "privacy-safe local scene", inner, 2);
}

// The title block + verifiable identity, drawn as one framed panel.
inline void trust_card(const Renderer& r, const TrustInfo& t) {
  const int inner = TRUST_INNER;
  char line[128];

  hrule(r, "SecuraCV  -  Canary witness", inner, 0);

  draw_canary(r, inner);  // the shared logo (see CANARY_LOGO)
  row_blank(r, inner);

  // Identity — plain rows.
  rowf(r, inner, COL_NONE, "Device    %s", t.device_id ? t.device_id : "?");
  rowf(r, inner, COL_NONE, "Firmware  %s (%s)   built %s",
       t.firmware ? t.firmware : "?", t.git ? t.git : "?", t.built ? t.built : "?");
  rowf(r, inner, COL_NONE, "Chain     %s  seq %u  boots %u",
       t.chain_head_hex ? t.chain_head_hex : "?",
       (unsigned)t.seq, (unsigned)t.boots);
  // Health gets a color AND a word (never color alone).
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

  // The randomart, in its own OpenSSH-style ASCII box, centered in the panel.
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
  row(r, inner, COL_DIM, "Memorize this shape - it changes if the key does.");
  if (t.tamper)
    row(r, inner, COL_AMBER, "!! TAMPER FLAG SET - investigate before trusting");
  hrule(r, "", inner, 2);
}

// The friendly hello the device prints on connect: the canary logo speaking in
// a char box, a one-line explanation, and the ONE thing to do — open the help
// site. This is first contact; it should feel welcoming and lead somewhere, not
// dump a spec. ASCII-safe like the rest of the engine.
inline void welcome_card(const Renderer& r, const char* device_id,
                         const char* help_url) {
  const int inner = TRUST_INNER;
  char line[128];

  hrule(r, "Hi, I'm your Canary!", inner, 0);
  draw_canary(r, inner);                       // the shared logo (see CANARY_LOGO)
  center_into(line, sizeof line, "chirp!", inner - 2);
  row(r, inner, COL_MOSS, line);               // a little hello, centered under the bird
  row_blank(r, inner);
  row(r, inner, COL_NONE, "I make tamper-proof records of what I see -");
  row(r, inner, COL_NONE, "so nobody can change the story later.");
  row_blank(r, inner);
  // The one thing to do, made obvious with an arrow — works for a first-timer.
  row(r, inner, COL_TEXT, "New here? Everything I do, explained:");
  rowf(r, inner, COL_BRAND, "  -> %s", help_url ? help_url : "securacv.com/canary");
  if (device_id && *device_id)
    rowf(r, inner, COL_DIM, "     (that's me: %s)", device_id);
  row_blank(r, inner);
  // ...and a few keys for the curious and the devs, on one tidy line.
  row(r, inner, COL_DIM, "Or press a key:  h help   l ID card   t self-test");
  hrule(r, "", inner, 2);
}

}  // namespace scene

#endif  // SECURACV_CONSOLE_SCENES_H
