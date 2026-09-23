#pragma once

#include "round_frame_core.h"

// Onboard Layout — where the first-boot Join scene puts its QR and captions.
//
// The Join scene (onboard_ui.cpp) stacks four things, top to bottom: the
// title line, the white QR card, the credentials line, and the line under
// it (the coach hint; on split glass the password). Each glass used to place
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

}  // namespace canary::ui::onboardlayout
