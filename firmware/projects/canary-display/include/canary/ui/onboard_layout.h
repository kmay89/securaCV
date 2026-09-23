#pragma once

#include <stdint.h>
#include <stdio.h>

#include "canary/ui/round_frame_core.h"

// Onboard Layout — where the first-boot Join scene puts its QR and captions.
//
// The Join scene (onboard_ui.cpp) stacks four things, top to bottom: the
// title line, the white QR card, the credentials line, and the line under
// it (the coach hint; on split glass the password or the hint — what each
// row SAYS is join_lines() below, F45). Each glass used to place
// them with its own literals — the card by an offset from the center, the
// captions by offsets from the bottom edge — and nothing related the two.
// On the 800x480 dash the "or join ... password" line landed 14 px inside
// the card's lower edge, and on the round watch the network-name line 7 px
// inside it (F43): text in the quiet zone a phone's scanner needs empty.
//
// This header is the one place that stacks them, from the panel's size and
// the fonts' line heights, so no row can cross another by construction.
// Pure integer math, no LVGL, no Arduino — host-tested against every shipped
// display panel and both type ladders by tests_host/test_onboard_layout.cpp.
//
// The rule:
//  * The stack lives in a window. On rectangular glass that is the whole
//    panel height. On round glass it is the band between the lowest latitude
//    whose chord still holds kRoundLowRowW for the last line and that
//    latitude's mirror above the equator: there the rim is the margin.
//  * The air the window has left over is shared evenly: by the two margins
//    and the two gaps around the card on rectangular glass (the stack sits
//    centered, the card with air on both sides), by the two gaps alone on
//    round glass (the outer lines ride the window's edges). The two caption
//    lines abut; each line height already carries its leading.
//  * No share drops below kMinGap. When the window is too short for that,
//    the QR canvas gives up the difference — never its white pad (the quiet
//    zone), and never below qr_floor(): a smaller canvas keeps the same
//    module pitch and simply holds a lower-version code with more light
//    margin. `fits` says whether that was enough; when it is not, the lines
//    still never cross — the stack keeps its minimum gaps and runs past the
//    window's bottom.
//
// C++11 on purpose, like round_frame_core.h: the Arduino parity sketch
// compiles this header on esp32 core 2.0.17 (gnu++11). Nothing here is
// constexpr-evaluated, so the functions are plain inline ones.
//
// Coordinates follow LVGL: y grows downward, 0 is the panel's top edge.

namespace canary::ui::onboardlayout {

// The least air kept between the card and a line, and at each margin.
constexpr int kMinGap = 2;

// Width the round glass's lowest line keeps: the chord of the band the
// password / coach line rides. provision.cpp writes its round-glass hints
// to it ("forget it on your phone", "open 192.168.4.1").
constexpr int kRoundLowRowW = 142;

// The join code's module count. provision_core.h's payload for the
// display's AP ("WIFI:T:WPA;S:SecuraCV-XXXX;P:<8>;;", 39 bytes; 42 with the
// rare per-session SSID tag) is a version-3 code at lv_qrcode's ECC M, and
// the widget only ever grows the version into spare pixels — so 29 modules
// is the floor a canvas is sized against (the host test pins the payload).
constexpr int kJoinQrModules = 29;

// The card a glass family asks for: the QR canvas and the white pad around
// it (the card is qr + 2 * pad). Small glass is the watch branch of
// onboard_ui.cpp (the round watch and the portrait nightstand line); wide
// glass is the 800x480 dash line.
struct CardSpec {
  int qr;
  int pad;
};
constexpr CardSpec kSmallGlassCard = {112, 8};
constexpr CardSpec kWideGlassCard = {208, 12};

// Smallest canvas that keeps `qr`'s module pitch for the join code.
inline int qr_floor(int qr) {
  return (qr / kJoinQrModules) * kJoinQrModules;
}

struct Glass {
  int w;       // panel width, px
  int h;       // panel height, px
  bool round;  // circular glass (the disc is the panel's short side)
};

struct Rows {
  int title_h;    // title line height
  CardSpec card;  // the card the glass family asks for
  int creds_h;    // credentials line height
  int hint_h;     // hint / password line height
};

struct Stack {
  int title_top;  // y of the title line's top
  int card_top;   // y of the card's top edge
  int card;       // card side granted: qr + 2 * Rows::card.pad
  int qr;         // QR canvas granted (<= Rows::card.qr, >= its qr_floor)
  int creds_top;  // y of the credentials line's top
  int hint_top;   // y of the hint / password line's top
  bool fits;      // everything inside the window with the minimum air
};

// Bottom edge of the lowest band of height h on a disc of diameter dia whose
// chord (house rim margin) still holds `need` px. Walks up from the rim; the
// equator is the answer of last resort (the widest latitude there is).
inline int round_low_row_bottom(int dia, int h, int need) {
  for (int bottom = dia - roundframe::kEdgeMargin; bottom > dia / 2;
       --bottom) {
    if (roundframe::band_chord(dia, roundframe::kEdgeMargin, bottom - h, h) >=
        need) {
      return bottom;
    }
  }
  return dia / 2;
}

// The Join scene's stack on this glass (see the rule above).
inline Stack join_stack(const Glass& g, const Rows& r) {
  int lo = 0;
  int hi = g.h;
  int shares = 4;  // top margin, title|card, card|creds, bottom margin
  if (g.round) {
    const int dia = g.w < g.h ? g.w : g.h;
    const int y0 = (g.h - dia) / 2;  // the disc's top edge on this panel
    hi = y0 + round_low_row_bottom(dia, r.hint_h, kRoundLowRowW);
    lo = y0 + dia - (hi - y0);
    shares = 2;  // title|card, card|creds — the rim is the margin
  }
  const int lines = r.title_h + r.creds_h + r.hint_h;
  const int need = shares * kMinGap;
  int qr = r.card.qr;
  int air = (hi - lo) - lines - (qr + 2 * r.card.pad);
  if (air < need) {
    qr -= need - air;
    if (qr < qr_floor(r.card.qr)) qr = qr_floor(r.card.qr);
    air = (hi - lo) - lines - (qr + 2 * r.card.pad);
  }
  const int card = qr + 2 * r.card.pad;
  Stack s;
  s.fits = air >= need;
  if (!s.fits) air = need;
  const int share = air / shares;
  // The rounding remainder (under one px per share) splits across the
  // stack's two ends.
  s.title_top = lo + (shares == 4 ? share : 0) + (air - shares * share) / 2;
  s.card_top = s.title_top + r.title_h + share;
  s.card = card;
  s.qr = qr;
  s.creds_top = s.card_top + card + share;
  s.hint_top = s.creds_top + r.creds_h;
  return s;
}

// ── What the two low rows say (F45) ───────────────────────────────────────
//
// Round glass has always split the credentials: the network name on the
// credentials row, "pass  <key>" on the row under it — no latitude that low
// holds "SecuraCV-XXXX  •  <key>". Rectangular small glass kept the joined
// line, and on the 172 px nightstand (the 180 px nightlight too) that line
// is ~174 px of text in a 156 px row: LVGL's LONG_DOT cut the key to an
// ellipsis, and so did the stuck-phone hint (205 px). When the QR does not
// scan, that text is the only way in (F45).
//
// The rule, one for every small glass (the watch branch of onboard_ui.cpp):
//  * Rectangular glass keeps the joined line when it fits its row in the
//    row's own face; otherwise it splits the way round glass does. Round
//    glass always splits.
//  * Split: the network name on the credentials row; on the row under it a
//    standing hint (it outranks the key, as it always has on round glass),
//    else "pass  <key>". Joined: the hint row holds the hint or nothing.
//  * Nothing is cut. A row tries its forms longest first in its own face —
//    the hint then its narrow form, "pass  <key>" then the bare key — and
//    only when none fits steps down to the floor face (the default
//    Character's caption: the smallest type this glass sets) and tries them
//    again. `fits` is false only when even that fails; the host test proves
//    it never does for any key or network name the unit can mint.
//
// Widths are measured the way LVGL lays out one line (lv_txt_get_width at
// letter_space 0: each glyph's advance given the letter after it, kerning
// included) through the caller's measure — lv_font_get_glyph_width on the
// glass (identical in LVGL 8 and 9), LVGL's own font data in the host test.
// Never an estimated character width.

// The credential lines, as the glass prints them. The host test measures
// these very formats (U+2022 is in the built-in Montserrat range).
constexpr char kJoinedFmt[] = "%s  •  %s";  // small glass, one row
constexpr char kPassFmt[] = "pass  %s";     // split glass, the row under
// Wide glass (the dash line) has room for words, with and without a QR.
constexpr char kWideScanFmt[] = "or join \"%s\"  •  password  %s";
constexpr char kWideTypeFmt[] = "\"%s\"  •  password  %s";

// Room for any line this scene sets: the coach line (onboard_ui's 96-byte
// buffer) and the joined credentials (a 32-byte SSID, the separator, a
// 16-byte key) both fit.
constexpr int kLineCap = 96;

// The next code point of UTF-8 `s` at byte `*i`, advancing `*i`; 0 at the
// terminator. Well-formed text decodes as LVGL's lv_txt_utf8_next does; a
// broken sequence reads as U+FFFD (no glyph) and never ends the walk early.
inline uint32_t utf8_next(const char* s, int* i) {
  const unsigned char c = (unsigned char)s[*i];
  if (c == 0) return 0;
  ++*i;
  int more = 0;
  uint32_t cp = c;
  if ((c & 0xE0) == 0xC0) {
    more = 1;
    cp = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    more = 2;
    cp = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    more = 3;
    cp = c & 0x07;
  } else if (c >= 0x80) {
    return 0xFFFD;  // a stray continuation byte or an invalid lead
  }
  for (; more > 0; --more) {
    const unsigned char b = (unsigned char)s[*i];
    if ((b & 0xC0) != 0x80) return 0xFFFD;  // truncated: resume at b
    cp = (cp << 6) | (b & 0x3F);
    ++*i;
  }
  return cp;
}

// Width of one line of `s`: glyph_w(letter, next) summed, the next letter of
// the last glyph being 0 — exactly lv_txt_get_width at letter_space 0.
template <class GlyphW>
inline int text_width(const char* s, GlyphW glyph_w) {
  int i = 0;
  int w = 0;
  uint32_t cur = utf8_next(s, &i);
  while (cur != 0) {
    const uint32_t next = utf8_next(s, &i);
    w += glyph_w(cur, next);
    cur = next;
  }
  return w;
}

// One low row: its text and the face it is drawn in.
struct Line {
  char text[kLineCap];
  bool floor;  // stepped down to the floor face
  bool fits;   // the text is inside its row (false: nothing could be)
};

struct JoinLines {
  bool split;  // the credentials span both rows
  Line creds;  // the credentials row
  Line low;    // the row under it: key, hint, or nothing
};

inline void set_line(Line& out, const char* text, bool floor, bool fits) {
  snprintf(out.text, sizeof(out.text), "%s", text ? text : "");
  out.floor = floor;
  out.fits = fits;
}

// The first of `forms` (longest first; null or empty entries skipped) that
// fits `row_w` in the row's own face, else in the floor face. When none
// does, the last form in the floor face with fits = false. `measure` is
// int(const char* text, bool floor_face).
template <class Measure>
inline void fit_line(Line& out, const char* const* forms, int n, int row_w,
                     Measure measure) {
  const char* last = "";
  for (int face = 0; face < 2; ++face) {
    for (int k = 0; k < n; ++k) {
      if (forms[k] == nullptr || forms[k][0] == '\0') continue;
      last = forms[k];
      if (measure(forms[k], face == 1) <= row_w) {
        set_line(out, forms[k], face == 1, true);
        return;
      }
    }
  }
  set_line(out, last, true, last[0] == '\0');
}

// The Join scene's two low rows on small glass (see the rule above).
// creds_w / low_w are the widths the rows are fitted to (rf_row_width);
// hint is the live coach line ("" for none) and hint_narrow its shorter form
// (may be null).
template <class Measure>
inline JoinLines join_lines(bool round, int creds_w, int low_w,
                            const char* ssid, const char* pass,
                            const char* hint, const char* hint_narrow,
                            Measure measure) {
  JoinLines j = JoinLines();
  char joined[kLineCap];
  snprintf(joined, sizeof(joined), kJoinedFmt, ssid, pass);
  j.split = round || measure(joined, false) > creds_w;
  if (j.split) {
    const char* forms[1] = {ssid};
    fit_line(j.creds, forms, 1, creds_w, measure);
  } else {
    set_line(j.creds, joined, false, true);
  }
  if (hint != nullptr && hint[0] != '\0') {
    const char* forms[2] = {hint, hint_narrow};
    fit_line(j.low, forms, 2, low_w, measure);
  } else if (j.split) {
    char labeled[kLineCap];
    snprintf(labeled, sizeof(labeled), kPassFmt, pass);
    const char* forms[2] = {labeled, pass};
    fit_line(j.low, forms, 2, low_w, measure);
  } else {
    set_line(j.low, "", false, true);
  }
  return j;
}

}  // namespace canary::ui::onboardlayout
