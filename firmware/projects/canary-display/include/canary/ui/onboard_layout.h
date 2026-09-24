#pragma once

#include <stdint.h>
#include <stdio.h>

#include "canary/ui/round_frame_core.h"

// Onboard Layout — where the first-boot Join scene puts its QR and captions.
//
// The Join scene (onboard_ui.cpp) stacks four things, top to bottom: the
// title line, the white QR card, the credentials line, and the line under
// it (the coach hint; on split glass the key — what each row SAYS is
// join_lines() below, F45). Each glass used to place
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
// password / coach line rides, and (the window is symmetric) of the title's
// band. The coach line's lower half rides it (hint_lines: "open 192.168.4.1"
// under "no page?"), and so does the stuck-phone hint on the Join scene's
// note row, the title's band ("forget it on your phone").
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
  // y of the note line's top: where a standing hint goes when the
  // credentials take both low rows (join_lines' `note`). On rectangular
  // glass the row under them, in the bottom margin; round glass has no
  // latitude under them that holds kRoundLowRowW, so the title's band
  // (the window is symmetric: it holds the same chord). Not part of the
  // window `fits` speaks for — the host test proves it on every panel.
  int note_top;
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
  s.note_top = g.round ? s.title_top : s.hint_top + r.hint_h;
  return s;
}

// ── What the text rows say (F45) ──────────────────────────────────────────
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
//  * The network name and the key stay on the glass for as long as the
//    scene is up — QR or no QR, hint or no hint. Nothing displaces them: the
//    glass cannot tell a code that scans from one that does not, and the
//    stuck-phone hint ("forget it on your phone") is exactly when the phone
//    needs the key again. (Round glass used to let a standing hint take the
//    key's row; that was the same dead end.)
//  * Split: the network name on the credentials row, "pass  <key>" on the
//    row under it, and a standing hint on the note row (Stack::note_top: the
//    row under those on rectangular glass, the title's band on round glass,
//    where the title yields while the hint stands). Joined: the hint row
//    holds the hint or nothing, and the note row is empty.
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
  Line creds;  // the credentials row: the joined line or the network name
  Line low;    // the row under it: the key (split), else the hint or nothing
  Line note;   // the note row (split only): the standing hint, or nothing
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

// The Join scene's text rows on small glass (see the rule above).
// creds_w / low_w / note_w are the widths the three rows are fitted to
// (rf_row_width at creds_top, hint_top, note_top); hint is the live coach
// line ("" for none) and hint_narrow its shorter form (may be null).
template <class Measure>
inline JoinLines join_lines(bool round, int creds_w, int low_w, int note_w,
                            const char* ssid, const char* pass,
                            const char* hint, const char* hint_narrow,
                            Measure measure) {
  JoinLines j = JoinLines();
  char joined[kLineCap];
  snprintf(joined, sizeof(joined), kJoinedFmt, ssid, pass);
  j.split = round || measure(joined, false) > creds_w;
  const bool hinted = hint != nullptr && hint[0] != '\0';
  const char* hints[2] = {hint, hint_narrow};
  if (j.split) {
    const char* name[1] = {ssid};
    fit_line(j.creds, name, 1, creds_w, measure);
    char labeled[kLineCap];
    snprintf(labeled, sizeof(labeled), kPassFmt, pass);
    const char* key[2] = {labeled, pass};
    fit_line(j.low, key, 2, low_w, measure);
    if (hinted) {
      fit_line(j.note, hints, 2, note_w, measure);
    } else {
      set_line(j.note, "", false, true);
    }
  } else {
    set_line(j.creds, joined, false, true);
    if (hinted) {
      fit_line(j.low, hints, 2, low_w, measure);
    } else {
      set_line(j.low, "", false, true);
    }
    set_line(j.note, "", false, true);
  }
  return j;
}

// ── The coach line in a scene without credentials (F50) ──────────────────
//
// PhoneJoined ("no page? open 192.168.4.1") and Fail (join_failure_hint's
// fix for what went wrong) set a coach line under their title and body while
// the credentials rows stand empty. It used to go on the hint row alone,
// whole, in the row's own face, and narrow glass cut it: the Fail hints are
// 175-219 px at 12 px against the round watch's 142 px low band and the
// 156/164 px portrait rows, and the PhoneJoined hint is 182 px under
// Heirloom there (F50).
//
// The rule, one for every small glass (the watch branch of onboard_ui.cpp):
//  * The coach line has both rows the credentials would take: the
//    credentials row (upper) and the hint row under it (lower). Nothing else
//    is on them in these scenes.
//  * Its forms, longest first, in the rows' own face and then the floor
//    face: the whole hint on the hint row; the whole hint over both rows;
//    the narrow form on the hint row; the narrow form over both rows. A form
//    that spans both rows breaks at a space (split_line below). The words the
//    portal and the log use stay whole wherever the glass has the room; the
//    narrow form is the rung under them, and it still names the fix.
//  * Nothing is cut. `fits` is false only when no form fits in any face;
//    the host test proves that never happens on a shipped glass.

struct HintLines {
  bool split;  // the coach line spans both rows
  Line upper;  // the credentials row: its first half when split, else empty
  Line lower;  // the hint row: the whole form, or its second half
};

// Break `text` at one space into `head` (for the upper row, upper_w) and
// `tail` (the lower row, lower_w), measured in one face. A number keeps the
// word after it ("2.4 GHz" never breaks). Of the breaks where both halves
// fit, one that ends a clause (the head ends in '?', ':' or ',') wins —
// "no page?" over "open 192.168.4.1" — else the one whose wider half is
// narrowest; on a tie the later, so the head takes the upper row (on round
// glass the wider of the two). False when no break fits.
template <class Measure>
inline bool split_line(const char* text, int upper_w, int lower_w,
                       bool floor, Measure measure, char* head, char* tail) {
  int best = -1;
  bool best_clause = false;
  int best_w = 0;
  char h[kLineCap];
  if (text == nullptr || text[0] == '\0') return false;
  int word = 0;  // where the word before text[i] starts
  for (int i = 1; i < kLineCap && text[i] != '\0'; ++i) {
    if (text[i] != ' ') continue;
    const bool number = text[word] >= '0' && text[word] <= '9';
    word = i + 1;
    if (number || text[i + 1] == '\0') continue;
    snprintf(h, sizeof(h), "%.*s", i, text);
    const int hw = measure(h, floor);
    const int tw = measure(text + i + 1, floor);
    if (hw > upper_w || tw > lower_w) continue;
    const char end = text[i - 1];
    const bool clause = end == '?' || end == ':' || end == ',';
    const int wide = hw > tw ? hw : tw;
    if (best < 0 || (clause && !best_clause) ||
        (clause == best_clause && wide <= best_w)) {
      best = i;
      best_clause = clause;
      best_w = wide;
    }
  }
  if (best < 0) return false;
  snprintf(head, kLineCap, "%.*s", best, text);
  snprintf(tail, kLineCap, "%s", text + best + 1);
  return true;
}

// The coach line of a scene without credentials (see the rule above).
// upper_w / lower_w are the widths the credentials and hint rows are fitted
// to (rf_row_width at creds_top and hint_top); hint is the live coach line
// ("" for none) and narrow its shorter form (may be null).
template <class Measure>
inline HintLines hint_lines(int upper_w, int lower_w, const char* hint,
                            const char* narrow, Measure measure) {
  HintLines out = HintLines();
  set_line(out.upper, "", false, true);
  set_line(out.lower, "", false, true);
  const char* forms[2] = {hint, narrow};
  const char* last = "";
  char head[kLineCap];
  char tail[kLineCap];
  for (int face = 0; face < 2; ++face) {
    const bool fl = face == 1;
    for (int k = 0; k < 2; ++k) {
      if (forms[k] == nullptr || forms[k][0] == '\0') continue;
      last = forms[k];
      if (measure(forms[k], fl) <= lower_w) {
        set_line(out.lower, forms[k], fl, true);
        return out;
      }
      if (split_line(forms[k], upper_w, lower_w, fl, measure, head, tail)) {
        out.split = true;
        set_line(out.upper, head, fl, true);
        set_line(out.lower, tail, fl, true);
        return out;
      }
    }
  }
  set_line(out.lower, last, true, last[0] == '\0');
  return out;
}

}  // namespace canary::ui::onboardlayout
