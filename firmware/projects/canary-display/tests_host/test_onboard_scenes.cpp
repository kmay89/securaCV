// Host test: what the onboarding scenes DRAW (F64, F65, F66, F84).
//
// test_onboard_layout.cpp holds onboard_layout.h's rules: the stack, the
// rows' words, the fitted scene lines, the halo, the bird's seats. This test
// holds onboard_ui.cpp to them. It compiles the REAL src/ui/onboard_ui.cpp,
// src/ui/round_frame.cpp and src/ui/canary_mark.cpp against fake_lvgl/lvgl.h
// (LVGL 8's position rules: where each object's box lands after a layout
// pass, and what each label says in which font at what width), drives every
// scene through the module's own API the way provision.cpp does (Hello,
// Join with and without the stuck-phone hint, PhoneJoined with and without
// its hint, Connecting with a short, a widest and a long network name, Fail
// with every reason, its narrow label and its fix, Fail with no reason,
// Join again, Success), and after each one reads the glass:
//  * every label with text reads whole: its text, measured in the font the
//    label carries with LVGL's own glyph metrics, is no wider than the
//    label, so LV_LABEL_LONG_DOT cuts nothing; and it says one of the
//    scene's forms (the title and body scene_copy() and provision.cpp hand
//    it, the network name whole or around "...", the coach line whole on a
//    row or over two);
//  * no two labels with text overlap, and on rectangular glass each is on
//    the panel;
//  * each credentials, hint and note row with text sits on the row the
//    stack names for it (creds_top, hint_top, note_top), as wide as
//    fit_row makes it and centered on the column, at least as tall as the
//    line of the face it carries, in its role's face (the caption face; on
//    wide glass the label face for the name and the key) and its role's
//    color (the name and the key muted, a hint faint) (F156, F157);
//  * the Join scene's title and rows read exactly what onboard_layout.h
//    says for this panel and the hint provision.cpp handed (the title's
//    forms through fit_line, wide_join_lines / join_lines at fit_row's
//    widths), in the face it chose: onboard_ui.cpp hands each line's forms
//    in their order, so the 800 px glass keeps the whole title and the
//    whole stuck-phone hint (F156);
//  * the halo is the one small_join() places (size, seat, stroke), and
//    every label, the visible QR card (its rounded corners) and the bird
//    over its whole breath are wholly inside the stroke's inner circle or
//    wholly outside its outer one;
//  * the bird draws at the seat onboard_layout.h names for the scene
//    (join_bird_top / scene_bird_top), clear of every label and the card;
//  * the Join scene keeps the network name and the key on the glass.
// on every display env (the round watch in this binary's watch build;
// the 172/180x320 portrait glass, the touch169 and the AMOLED in its
// nightstand build; the 800x480 dash, dash7 and nightstand7 in its dash
// build), and on the panels main.cpp turns before provision_run() (the
// nightlight's 320x180 landscape, F157; the dash line's 480x800 portrait,
// F156), with both type ladders, with the QR rendered and without it.
// On wide glass (F84) the halo is wide_ring()'s, every centered title and
// body is fitted inside it like small glass's, no title box overlaps its
// body's, the Join rows (fitted to the panel's row: the worded line whole
// on the 800 px panel, split over two rows where it does not fit, F156)
// stay on the panel and outside the ring, and the bird keeps bird_seat()'s
// one seat. On landscape small glass the halo, the card and the bird stand
// beside the text column (F157). Not held: the QR card's rounded corners
// on wide glass, which reach past the 300 px ring's stroke (filed).
//
// The fake is held to LVGL by test_canary_mark_seat's pins (boxes the real
// LVGL 8.4.0 drew in a native harness). The Success hop is held on the
// glass (F157), not inside the halo: its apex reaches the touch169's
// (filed); the bird's seat is read once it has settled back to its breath.
//
// Prints "ALL ONBOARD SCENES TESTS PASSED" on success.

#include <config.h>
#include <lvgl.h>

#include "canary/ui/canary_mark.h"
#include "canary/ui/character.h"
#include "canary/ui/onboard_ui.h"
#include "canary/ui/round_frame.h"
#include "canary/ui/theme.h"
#include "network/wifi_join_policy.h"
#include "onboard_test_env.h"

#include <cmath>

#if defined(CD_FLAVOR_WATCH) && !defined(CD_FLAVOR_NIGHTSTAND)
static const bool kRoundBuild = true;
static const bool kWideBuild = false;
#elif defined(CD_FLAVOR_NIGHTSTAND)
static const bool kRoundBuild = false;
static const bool kWideBuild = false;
#elif defined(CD_FLAVOR_DASH)
static const bool kRoundBuild = false;
static const bool kWideBuild = true;
#else
#error "test_onboard_scenes builds against the watch, nightstand or dash config"
#endif
// The labels onboard_ui_create() makes: title, body, credentials, hint and
// the note row (on wide glass too since F156).
static const int kLabels = 5;
// The brand mark's square on this glass family (onboard_layout.h's).
static const int kBirdPx = kWideBuild ? kWideBirdPx : kSmallBirdPx;

// ── the theme the module reads, from the env's ladder ─────────────────────
namespace {

int g_fam = kStd;      // character.cpp's ladder family for this env
int g_which = 0;       // 0: the default Character, 1: Heirloom
bool g_qr_fails = false;

lv_font_t g_fonts[16];
int g_font_n = 0;

int font_glyph(const lv_font_t* f, uint32_t a, uint32_t b) {
  return glyph_px(*(const Face*)f->dsc, a, b);
}

// The built-in Montserrat face of `size`, with LVGL's own metrics; null
// when montserrat_metrics.h does not carry it (the hero and clock roles; on
// small glass the title and label roles too: it sets its text in the body
// and caption faces).
const lv_font_t* font_or_null(int size) {
  for (int i = 0; i < g_font_n; ++i)
    if (((const Face*)g_fonts[i].dsc)->size == size) return &g_fonts[i];
  const Face* f = face_of(size);
  if (f == nullptr || g_font_n >= 16) return nullptr;
  lv_font_t& out = g_fonts[g_font_n++];
  out.line_height = f->line_height;
  out.glyph_w = font_glyph;
  out.dsc = f;
  return &out;
}

// A face the module measures: it must be carried.
const lv_font_t* font_px(int size) {
  const lv_font_t* f = font_or_null(size);
  CHECK(f != nullptr,
        "montserrat_%d is not carried (re-run gen_montserrat_metrics.py)",
        size);
  return f != nullptr ? f : font_or_null(12);
}

const lv_font_t* role(int which, int r) {
  return font_px(g_ladder[g_fam][which][r]);
}
const lv_font_t* role_or_null(int which, int r) {
  return font_or_null(g_ladder[g_fam][which][r]);
}

}  // namespace

namespace canary::ui {
const lv_font_t* font_hero() { return role_or_null(g_which, kHero); }
const lv_font_t* font_title() {
  return kWideBuild ? role(g_which, kTitle) : role_or_null(g_which, kTitle);
}
const lv_font_t* font_body() { return role(g_which, kBody); }
const lv_font_t* font_label() {
  return kWideBuild ? role(g_which, kLabel) : role_or_null(g_which, kLabel);
}
const lv_font_t* font_caption() { return role(g_which, kCaption); }
const lv_font_t* font_clock() { return role_or_null(g_which, kClock); }
lv_color_t col_edge() { return lv_color_hex(0x262626); }
lv_color_t col_text() { return lv_color_hex(0xEDEDED); }
lv_color_t col_muted() { return lv_color_hex(0x9A9A9A); }
lv_color_t col_faint() { return lv_color_hex(0x5C5C5C); }
lv_color_t col_ok() { return lv_color_hex(0x4CAF50); }
lv_color_t col_warn() { return lv_color_hex(0xFF9800); }
// Only the default Character's ladder is asked for (the floor faces).
const CharacterDef& character_def(Character) {
  static CharacterDef d = CharacterDef();
  d.type.hero = role_or_null(0, kHero);
  d.type.title = kWideBuild ? role(0, kTitle) : role_or_null(0, kTitle);
  d.type.body = role(0, kBody);
  d.type.label = kWideBuild ? role(0, kLabel) : role_or_null(0, kLabel);
  d.type.caption = role(0, kCaption);
  d.type.clock = role_or_null(0, kClock);
  return d;
}
// theme.cpp's QR helper: a canvas of size_px, or nothing (the generator or
// its buffer failed — the module must fall back to the text path).
lv_obj_t* mk_qrcode(lv_obj_t* parent, int32_t size_px) {
  if (g_qr_fails) return nullptr;
  lv_obj_t* o = lv_obj_create(parent);
  o->kind = fake_lvgl::kQr;
  lv_obj_set_size(o, (lv_coord_t)size_px, (lv_coord_t)size_px);
  return o;
}
}  // namespace canary::ui

// ── reading the glass ─────────────────────────────────────────────────────
namespace {

const char kSsid[] = "SecuraCV-A7K2";
const char kPass[] = "p7Rm2Kqf";

struct Run {
  const Env* env;
  int which;
  bool qr;
  bool turned;  // the panel a quarter turned (main.cpp's saved rotation)
  onboardlayout::Glass g;
  // What onboard_layout.h says this glass gets: small_join's stack, halo
  // and column on small glass; wide_join's on wide glass.
  onboardlayout::SmallJoin j;
  lv_obj_t* scr;
  lv_obj_t* ring;
  lv_obj_t* bird;
  lv_obj_t* card;
  lv_obj_t* labels[5];  // title, body, creds, hint, note (creation order)
  int frames;
};

const char* const kLabelName[5] = {"title", "body", "credentials row",
                                   "hint row", "note row"};

std::string who(const Run& G, const char* scene) {
  char b[200];
  std::snprintf(b, sizeof(b), "%s %dx%d/%s/%s: %s", G.env->name.c_str() + 15,
                G.env->w, G.env->h, G.which == 0 ? "default" : "heirloom",
                G.qr ? "qr" : "no-qr", scene);
  return b;
}

// The objects onboard_ui_create() built, found by what they are: the arc on
// the screen; on the content layer the labels (in creation order: five on
// small glass, four on wide), the QR card (the object holding the canvas,
// or the card-sized one when no canvas rendered) and the bird (the mark's
// square, onboard_layout.h's kSmallBirdPx / kWideBirdPx).
bool find_objects(Run* G) {
  G->ring = nullptr;
  G->bird = nullptr;
  G->card = nullptr;
  lv_obj_t* content = nullptr;
  for (size_t i = 0; i < G->scr->children.size(); ++i) {
    lv_obj_t* c = G->scr->children[i];
    if (c->kind == fake_lvgl::kArc) G->ring = c;
    else if (c->kind == fake_lvgl::kObj) content = c;
  }
  int n = 0;
  if (content != nullptr) {
    for (size_t i = 0; i < content->children.size(); ++i) {
      lv_obj_t* c = content->children[i];
      if (c->kind == fake_lvgl::kLabel) {
        if (n < kLabels) G->labels[n] = c;
        n++;
      } else if (c->w == kBirdPx && c->h == kBirdPx) {
        G->bird = c;
      } else if (c->radius == kCardRadius) {
        G->card = c;
      }
    }
  }
  CHECK(G->ring && G->bird && G->card && n == kLabels,
        "%s: the onboarding screen is not ring + content{bird, card, %d "
        "labels} (ring %d, bird %d, card %d, %d labels)",
        who(*G, "create").c_str(), kLabels, G->ring != nullptr,
        G->bird != nullptr, G->card != nullptr, n);
  return G->ring && G->bird && G->card && n == kLabels;
}

struct Box {
  std::string what;
  double x0, y0, x1, y1;
};

Box box_of(const char* what, const lv_obj_t* o) {
  Box b = {what, (double)o->x1, (double)o->y1, (double)(o->x1 + o->w),
           (double)(o->y1 + o->h)};
  return b;
}

bool overlap(const Box& a, const Box& b) {
  return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

// 1: wholly inside the stroke's inner circle; 2: wholly outside its outer
// one; 0: across it.
int ring_side(double cx, double cy, double r_in, double r_out, const Box& b) {
  double far2 = 0;
  const double xs[2] = {b.x0, b.x1}, ys[2] = {b.y0, b.y1};
  for (int a = 0; a < 2; ++a)
    for (int c = 0; c < 2; ++c) {
      const double d2 = (xs[a] - cx) * (xs[a] - cx) + (ys[c] - cy) * (ys[c] - cy);
      if (d2 > far2) far2 = d2;
    }
  const double nx = cx < b.x0 ? b.x0 : cx > b.x1 ? b.x1 : cx;
  const double ny = cy < b.y0 ? b.y0 : cy > b.y1 ? b.y1 : cy;
  const double near2 = (nx - cx) * (nx - cx) + (ny - cy) * (ny - cy);
  if (far2 <= r_in * r_in) return 1;
  if (near2 >= r_out * r_out) return 2;
  return 0;
}

// What a scene may say, label by label (index as Run::labels).
struct Says {
  std::vector<std::string> title, body;  // allowed texts ("" if it may be empty)
  const char* name;                      // Connecting: the name the body shows
  std::vector<std::string> coach;        // a coach line's forms (else none)
  bool join;                             // the Join scene's credentials rows
  std::vector<std::string> stuck;        // the Join scene's standing hint forms
  bool bird_shown;                       // the mood puts the bird on stage
  bool settle_hop;                       // read the bird after the hop
};

bool one_of(const std::string& t, const std::vector<std::string>& v) {
  for (size_t i = 0; i < v.size(); ++i)
    if (t == v[i]) return true;
  return false;
}

std::string at(const Run& G, int k) {
  return G.labels[k] != nullptr ? G.labels[k]->text : std::string();
}

int face_px(const lv_font_t* f) {
  return f != nullptr ? ((const Face*)f->dsc)->size : 0;
}

// A role's face and its floor (the default Character's face of the role),
// measured the way onboard_ui.cpp's text_w measures: what its fit_line and
// wide_join_lines / join_lines calls take.
struct Pair {
  const lv_font_t* own;
  const lv_font_t* floor;
  int operator()(const char* t, bool fl) const {
    return fake_lvgl::text_width(fl ? floor : own, t);
  }
  const lv_font_t* face(const Line& l) const { return l.floor ? floor : own; }
};

// What onboard_layout.h says the Join scene's title and rows read on this
// panel, in which face (F45, F65, F156, F157): the title's forms through
// fit_line on its row, the rows through wide_join_lines (wide glass) or
// join_lines (small glass) at the widths fit_row gives them, with the
// stuck-phone hint provision.cpp handed the glass (`stuck`, none if empty).
// Index as Run::labels: 0 the title, 2..4 the credentials, hint and note
// rows (1, the body, is empty in the Join scene).
struct JoinWant {
  Line line[5];
  const lv_font_t* face[5];
};

JoinWant join_want(const Run& G, const std::vector<std::string>& stuck) {
  JoinWant w = JoinWant();
  const CharacterDef& qg = character_def(Character::QuietGlass);
  const char* hint = stuck.empty() ? "" : stuck[0].c_str();
  const char* narrow = stuck.size() > 1 ? stuck[1].c_str() : nullptr;
  set_line(w.line[1], "", false, true);
  w.face[1] = nullptr;
  if (kWideBuild) {
    const Pair title = {font_title(), qg.type.title};
    const Pair creds = {font_label(), qg.type.label};
    const Pair cap = {font_caption(), qg.type.caption};
    const Forms tf = wide_join_title(G.qr);
    const char* forms[2] = {tf.full, tf.narrow};
    fit_line(w.line[0], forms, 2, G.j.col.w, title);
    w.face[0] = title.face(w.line[0]);
    const JoinLines j = wide_join_lines(G.qr, G.j.col.w, kSsid, kPass, hint,
                                        narrow, creds, cap);
    w.line[2] = j.creds;
    w.face[2] = creds.face(j.creds);
    w.line[3] = j.low;
    w.face[3] = j.split ? creds.face(j.low) : cap.face(j.low);
    w.line[4] = j.note;
    w.face[4] = cap.face(j.note);
    return w;
  }
  const Pair title = {font_body(), qg.type.body};
  const Pair cap = {font_caption(), qg.type.caption};
  // The rows' widths: fit_row's at each row, for the caption face the
  // labels are made in (the disc's chord on round glass).
  const int lh = font_caption()->line_height;
  const int creds_w =
      G.g.round ? rf_row_width(G.j.stack.creds_top, lh) : G.j.col.w;
  const int low_w = G.g.round ? rf_row_width(G.j.stack.hint_top, lh) : G.j.col.w;
  const int note_w = G.g.round ? rf_row_width(G.j.stack.note_top, lh) : G.j.col.w;
  const JoinLines j = join_lines(G.g.round, creds_w, low_w, note_w, kSsid,
                                 kPass, hint, narrow, cap);
  w.line[2] = j.creds;
  w.line[3] = j.low;
  w.line[4] = j.note;
  for (int k = 2; k < 5; ++k) w.face[k] = cap.face(w.line[k]);
  // The title yields its band to the note row on round glass.
  if (G.g.round && j.note.text[0] != '\0') {
    set_line(w.line[0], "", false, true);
    w.face[0] = nullptr;
    return w;
  }
  const int title_w =
      G.g.round ? rf_row_width(G.j.stack.title_top, font_body()->line_height)
                : G.j.col.w;
  const char* forms[1] = {join_title(G.qr)};
  fit_line(w.line[0], forms, 1, title_w, title);
  w.face[0] = title.face(w.line[0]);
  return w;
}

// Run the scene's motion with a layout pass every 5 ms (a refresh), and
// read the bird's drawn box over it.
struct Span {
  int x_lo, x_hi, y_lo, y_hi;
  bool shown;
};

Span settle(Run& G, uint32_t skip_ms, uint32_t ms) {
  Span s = {1 << 30, -(1 << 30), 1 << 30, -(1 << 30), false};
  for (uint32_t t = 0; t < skip_ms + ms; t += 5) {
    fake_lvgl::run(5);
    lv_obj_update_layout(G.scr);
    if (t < skip_ms || lv_obj_has_flag(G.bird, LV_OBJ_FLAG_HIDDEN)) continue;
    s.shown = true;
    if (G.bird->x1 < s.x_lo) s.x_lo = G.bird->x1;
    if (G.bird->x1 > s.x_hi) s.x_hi = G.bird->x1;
    if (G.bird->y1 < s.y_lo) s.y_lo = G.bird->y1;
    if (G.bird->y1 > s.y_hi) s.y_hi = G.bird->y1;
  }
  return s;
}

void check_frame(Run& G, const char* scene, const Says& says) {
  G.frames++;
  const std::string w = who(G, scene);
  const char* n = w.c_str();
  // Success: the one earned hop first (on the glass, F157: the landscape
  // nightlight's lifted the bird's box to y -7), then the bird once it has
  // settled back to its breath.
  Span hop = {0, 0, 0, 0, false};
  if (says.settle_hop) hop = settle(G, 0, 1200);
  const Span sp = settle(G, 0, 2600);
  const int pw = G.env->w, ph = G.env->h;

  // The halo: small_join's (wide_ring's on wide glass) size and seat, the
  // stroke onboard_layout.h fits the lines inside.
  const onboardlayout::Ring& halo = G.j.halo;
  CHECK(G.ring->w == halo.d && G.ring->h == halo.d &&
            G.ring->x1 == pw / 2 - halo.d / 2 + halo.x &&
            G.ring->y1 == halo.top && G.ring->arc_w_ind == kRingStroke,
        "%s: the halo is %d x %d at (%d, %d), stroke %d; %s places "
        "%d at (%d, %d), stroke %d", n, G.ring->w, G.ring->h, G.ring->x1,
        G.ring->y1, G.ring->arc_w_ind,
        kWideBuild ? "wide_join" : "small_join", halo.d,
        pw / 2 - halo.d / 2 + halo.x, halo.top, kRingStroke);
  const double cx = G.ring->x1 + G.ring->w / 2.0;
  const double cy = G.ring->y1 + G.ring->h / 2.0;
  const double r_out = G.ring->w / 2.0;
  const double r_in = r_out - G.ring->arc_w_ind;

  // The labels: whole, on the panel, clear of the stroke and of each other.
  std::vector<Box> drawn;
  for (int k = 0; k < kLabels; ++k) {
    const lv_obj_t* l = G.labels[k];
    if (l->text.empty()) continue;
    const lv_font_t* f = lv_obj_get_style_text_font(l, LV_PART_MAIN);
    const int tw = f ? fake_lvgl::text_width(f, l->text) : 1 << 20;
    CHECK(tw <= l->w,
          "%s: the %s \"%s\" is %d px in montserrat_%d on a %d px label — "
          "LV_LABEL_LONG_DOT cuts it", n, kLabelName[k], l->text.c_str(), tw,
          f ? ((const Face*)f->dsc)->size : 0, (int)l->w);
    const Box b = box_of(kLabelName[k], l);
    if (!G.g.round) {
      CHECK(b.x0 >= 0 && b.x1 <= pw && b.y0 >= 0 && b.y1 <= ph,
            "%s: the %s (%.0f..%.0f x %.0f..%.0f) leaves the %dx%d panel", n,
            kLabelName[k], b.x0, b.x1, b.y0, b.y1, pw, ph);
    }
    CHECK(ring_side(cx, cy, r_in, r_out, b) != 0,
          "%s: the %s \"%s\" (%.0f..%.0f x %.0f..%.0f) crosses the halo's "
          "stroke (center %.1f,%.1f radius %.1f..%.1f)", n, kLabelName[k],
          l->text.c_str(), b.x0, b.x1, b.y0, b.y1, cx, cy, r_in, r_out);
    for (size_t i = 0; i < drawn.size(); ++i) {
      CHECK(!overlap(b, drawn[i]), "%s: the %s overlaps the %s", n,
            kLabelName[k], drawn[i].what.c_str());
    }
    drawn.push_back(b);
  }

  // The text rows (F156, F157): each of the credentials, hint and note
  // labels that shows text sits on the row onboard_layout.h's stack names
  // for it, as wide as fit_row makes it and centered on the column (the
  // panel's row; the column beside the halo on landscape small glass; the
  // disc's chord at that row on round glass), at least as tall as the line
  // of the face it carries (a shorter box clips its descenders), in its
  // role's face or the default Character's (the caption face on small
  // glass; on wide glass the label face for the network name and the key,
  // the caption face for a hint or a coach line), and in its role's color
  // (the name and the key muted, a hint faint).
  const int row_top[3] = {G.j.stack.creds_top, G.j.stack.hint_top,
                          G.j.stack.note_top};
  const CharacterDef& qg = character_def(Character::QuietGlass);
  for (int k = 2; k < kLabels; ++k) {
    const lv_obj_t* l = G.labels[k];
    if (l->text.empty()) continue;
    const int top = row_top[k - 2];
    const int rw = G.g.round ? rf_row_width(top, l->h) : G.j.col.w;
    const int rx = pw / 2 - rw / 2 + G.j.col.x;
    CHECK(l->y1 == top && l->x1 == rx && l->w == rw,
          "%s: the %s \"%s\" is %d px at (%d, %d); its row is %d px at "
          "(%d, %d)", n, kLabelName[k], l->text.c_str(), (int)l->w,
          (int)l->x1, (int)l->y1, rw, rx, top);
    const lv_font_t* f = lv_obj_get_style_text_font(l, LV_PART_MAIN);
    CHECK(f != nullptr && l->h >= f->line_height,
          "%s: the %s \"%s\" is %d px tall in montserrat_%d (its line is "
          "%d): the box clips the line", n, kLabelName[k], l->text.c_str(),
          (int)l->h, face_px(f), f ? f->line_height : 0);
    const bool cred = says.join && (l->text.find(kSsid) != std::string::npos ||
                                    l->text.find(kPass) != std::string::npos);
    const lv_font_t* own = kWideBuild && cred ? font_label() : font_caption();
    const lv_font_t* fl = kWideBuild && cred ? qg.type.label : qg.type.caption;
    CHECK(f == own || f == fl,
          "%s: the %s \"%s\" is in montserrat_%d; its role's face is "
          "montserrat_%d (montserrat_%d as the floor)", n, kLabelName[k],
          l->text.c_str(), face_px(f), face_px(own), face_px(fl));
    const uint32_t c = cred ? col_muted().full : col_faint().full;
    CHECK(l->text_color_set && l->text_color == c,
          "%s: the %s \"%s\" is #%06X; a%s is #%06X", n, kLabelName[k],
          l->text.c_str(), (unsigned)l->text_color,
          cred ? " name or a key (muted)" : " hint (faint)", (unsigned)c);
  }

  // What they say.
  CHECK(one_of(at(G, 0), says.title), "%s: the title says \"%s\"", n,
        at(G, 0).c_str());
  if (says.name != nullptr) {
    const std::string body = at(G, 1), name = says.name;
    const size_t dots = body.find("...");
    const bool whole = body == name;
    // find() put the dots inside body, so dots + 3 <= body.size(): the
    // tail lengths below never wrap.
    const bool cut = dots != std::string::npos && dots > 0 &&
                     name.compare(0, dots, body, 0, dots) == 0 &&
                     body.size() > dots + 3 &&
                     name.size() >= body.size() - dots - 3 &&
                     name.compare(name.size() - (body.size() - dots - 3),
                                  std::string::npos, body, dots + 3,
                                  std::string::npos) == 0;
    CHECK(whole || cut,
          "%s: the network name \"%s\" shows as \"%s\" (not whole, nor its "
          "head and tail around \"...\")", n, says.name, body.c_str());
  } else {
    CHECK(one_of(at(G, 1), says.body), "%s: the body says \"%s\"", n,
          at(G, 1).c_str());
  }
  if (says.join) {
    // Exactly what onboard_layout.h says, in the face it chose (join_want):
    // the forms in their order, longest first, so the 800 px glass keeps
    // the whole title and the whole stuck-phone hint (F156) and a split
    // sets the key in the credentials' face.
    const JoinWant want = join_want(G, says.stuck);
    const int order[4] = {0, 2, 3, 4};
    for (int i = 0; i < 4; ++i) {
      const int k = order[i];
      const lv_font_t* f = lv_obj_get_style_text_font(G.labels[k], LV_PART_MAIN);
      const bool empty = want.line[k].text[0] == '\0';
      CHECK(at(G, k) == want.line[k].text && (empty || f == want.face[k]),
            "%s: the %s reads \"%s\" in montserrat_%d; onboard_layout.h "
            "says \"%s\" in montserrat_%d", n, kLabelName[k],
            at(G, k).c_str(), face_px(f), want.line[k].text,
            face_px(want.face[k]));
    }
  }
  if (says.join && kWideBuild) {
    // Wide glass: one worded credentials row (kWideScanFmt, or
    // kWideTypeFmt when no QR rendered) and the hint row under it where the
    // panel's row holds the line — every 800 px glass, as before F156 —
    // else the name over the key, and a standing hint on the note row.
    char creds[160];
    std::snprintf(creds, sizeof(creds),
                  G.qr ? kWideScanFmt : kWideTypeFmt, kSsid, kPass);
    const bool joined = at(G, 2) == creds;
    bool ssid = false, key = false, hint = says.stuck.empty();
    for (int k = 2; k < 5; ++k) {
      ssid = ssid || at(G, k).find(kSsid) != std::string::npos;
      key = key || at(G, k).find(kPass) != std::string::npos;
      hint = hint || one_of(at(G, k), says.stuck);
    }
    // One row where the panel's row holds the worded line in the label
    // face (every 800 px glass does, for any name and key: the layout test
    // proves it), else split.
    const int row = pw - 2 * roundframe::kRectSidePad;
    const bool one_row = fake_lvgl::text_width(font_label(), creds) <= row;
    CHECK(ssid && key && hint && joined == one_row &&
              (joined || pw < 800) && (!joined || at(G, 4).empty()),
          "%s: the Join rows say \"%s\" | \"%s\" | \"%s\" (the name %s, the "
          "key %s, the hint %s; the worded line %s)", n, at(G, 2).c_str(),
          at(G, 3).c_str(), at(G, 4).c_str(), ssid ? "on" : "MISSING",
          key ? "on" : "MISSING", hint ? "whole" : "MISSING",
          joined ? "whole on one row" : "split");
  } else if (says.join) {
    // The name and the key stay on the glass (F45), and a standing hint
    // reads whole on its row.
    bool ssid = false, key = false, hint = says.stuck.empty();
    for (int k = 2; k < 5; ++k) {
      ssid = ssid || at(G, k).find(kSsid) != std::string::npos;
      key = key || at(G, k).find(kPass) != std::string::npos;
      hint = hint || one_of(at(G, k), says.stuck);
    }
    CHECK(ssid && key && hint,
          "%s: the Join rows say \"%s\" | \"%s\" | \"%s\" (the name %s, the "
          "key %s, the hint %s)", n, at(G, 2).c_str(), at(G, 3).c_str(),
          at(G, 4).c_str(), ssid ? "on" : "MISSING", key ? "on" : "MISSING",
          hint ? "whole" : "MISSING");
  } else if (kWideBuild) {
    // The coach line: whole on the hint row (refresh_bottom's wide branch).
    const bool ok = says.coach.empty() ? at(G, 2).empty() && at(G, 3).empty()
                                       : at(G, 2).empty() &&
                                             one_of(at(G, 3), says.coach);
    CHECK(ok && at(G, 4).empty(), "%s: the coach rows say \"%s\" | \"%s\" | "
          "\"%s\"", n, at(G, 2).c_str(), at(G, 3).c_str(), at(G, 4).c_str());
  } else {
    // The coach line: whole on the hint row, or over both rows.
    std::string line = at(G, 2);
    if (!line.empty() && !at(G, 3).empty()) line += " ";
    line += at(G, 3);
    const bool ok = says.coach.empty() ? line.empty() && at(G, 4).empty()
                                       : one_of(line, says.coach) &&
                                             at(G, 4).empty();
    CHECK(ok, "%s: the coach rows say \"%s\" | \"%s\" | \"%s\"", n,
          at(G, 2).c_str(), at(G, 3).c_str(), at(G, 4).c_str());
  }

  // The QR card, while it is up: small_join's (join_stack's on wide glass),
  // clear of every label; on small glass its rounded corners inside the
  // halo (on wide glass they reach past the stroke: not held, filed).
  const bool card_up = !lv_obj_has_flag(G.card, LV_OBJ_FLAG_HIDDEN);
  CHECK(card_up == (says.join && G.qr), "%s: the QR card is %s", n,
        card_up ? "up" : "hidden");
  CHECK(G.card->w == G.j.stack.card && G.card->y1 == G.j.stack.card_top &&
            G.card->x1 == pw / 2 - G.j.stack.card / 2 + halo.x,
        "%s: the card is %d px at (%d, %d); the stack places %d at (%d, %d)",
        n, G.card->w, G.card->x1, G.card->y1, G.j.stack.card,
        pw / 2 - G.j.stack.card / 2 + halo.x, G.j.stack.card_top);
  if (card_up) {
    const lv_obj_t* qr = G.card->children.empty() ? nullptr : G.card->children[0];
    const onboardlayout::CardSpec spec =
        kWideBuild ? kWideGlassCard : kSmallGlassCard;
    CHECK(qr != nullptr && qr->w == G.j.stack.qr &&
              qr->w / kJoinQrModules == spec.qr / kJoinQrModules,
          "%s: the QR canvas is %d px (the stack's: %d; the join code's "
          "pitch %d px a module)", n, qr ? (int)qr->w : -1, G.j.stack.qr,
          spec.qr / kJoinQrModules);
    const double r = G.card->radius;
    double reach = 0;
    const double ax[2] = {G.card->x1 + r, G.card->x1 + G.card->w - r};
    const double ay[2] = {G.card->y1 + r, G.card->y1 + G.card->h - r};
    for (int a = 0; a < 2; ++a)
      for (int c = 0; c < 2; ++c) {
        const double d = std::sqrt((ax[a] - cx) * (ax[a] - cx) +
                                   (ay[c] - cy) * (ay[c] - cy)) + r;
        if (d > reach) reach = d;
      }
    CHECK(kWideBuild || reach + kMinGap <= r_in,
          "%s: the QR card's corners reach %.1f px from the halo's center; "
          "its stroke starts at %.1f", n, reach, r_in);
    const Box cb = box_of("QR card", G.card);
    for (size_t i = 0; i < drawn.size(); ++i)
      CHECK(!overlap(cb, drawn[i]), "%s: the %s is drawn on the QR card", n,
            drawn[i].what.c_str());
  }

  // The bird: on stage when the mood says so, at the scene's seat, its
  // whole breath inside the halo and clear of the text and the card.
  CHECK(sp.shown == says.bird_shown, "%s: the bird is %s", n,
        sp.shown ? "on stage" : "hidden");
  if (sp.shown) {
    const int bird = kBirdPx;
    const bool join_seat = says.join && !G.qr;
    // The seat bird_seat() names (join_bird_top / scene_bird_top on small
    // glass, the one wide seat on wide glass), checked against the parts
    // it is made of.
    const onboardlayout::Seat st = bird_seat(
        G.g, kWideBuild, G.j.stack, G.j.halo,
        says.join ? ObStage::Join : ObStage::Hello);
    // Landscape small glass seats it at the card's seat, the halo's
    // center, in every scene (F157).
    const bool land = !kWideBuild && small_landscape(G.g);
    const int seat = kWideBuild  ? ph / 2 - bird / 2 + kWideBirdOff
                     : (join_seat || land) ? join_bird_top(G.j.stack, bird)
                                 : scene_bird_top(G.g, G.j.halo, bird);
    const int seat_x = pw / 2 - bird / 2 + (land ? halo.x : 0);
    CHECK(st.d == bird && st.x == seat_x && st.y == seat,
          "%s: bird_seat() names %d px at (%d, %d); the scene's seat is %d "
          "px at (%d, %d)", n, st.d, st.x, st.y, bird, seat_x, seat);
    CHECK(sp.x_lo == seat_x && sp.x_hi == sp.x_lo &&
              sp.y_lo >= seat - kBirdBreath && sp.y_hi <= seat + kBirdBreath,
          "%s: the bird draws at x %d..%d y %d..%d; its seat is x %d y %d "
          "(%s), breathing %d px", n, sp.x_lo, sp.x_hi, sp.y_lo, sp.y_hi,
          seat_x, seat,
          kWideBuild ? "the wide seat"
          : land ? "the halo's center"
          : join_seat ? "join_bird_top" : "scene_bird_top", kBirdBreath);
    CHECK(!says.settle_hop || (hop.shown && hop.y_lo >= 0 &&
                               hop.y_hi + bird <= ph && hop.y_lo < sp.y_lo),
          "%s: the Success hop draws the bird at y %d..%d on the %d px "
          "glass (seat %d)", n, hop.y_lo, hop.y_hi + bird, ph, seat);
    int sx = 0, sy = 0, sd = 0;
    CHECK(onboard_ui_bird_seat(&sx, &sy, &sd) && sx == st.x && sy == st.y &&
              sd == st.d,
          "%s: onboard_ui_bird_seat() says %d px at (%d, %d); bird_seat() "
          "names %d at (%d, %d)", n, sd, sx, sy, st.d, st.x, st.y);
    const Box bb = {"bird", (double)sp.x_lo, (double)sp.y_lo,
                    (double)(sp.x_hi + bird), (double)(sp.y_hi + bird)};
    CHECK(ring_side(cx, cy, r_in, r_out, bb) == 1,
          "%s: the bird (%.0f..%.0f x %.0f..%.0f over its breath) is not "
          "inside the halo (center %.1f,%.1f, stroke from %.1f)", n, bb.x0,
          bb.x1, bb.y0, bb.y1, cx, cy, r_in);
    for (size_t i = 0; i < drawn.size(); ++i)
      CHECK(!overlap(bb, drawn[i]), "%s: the bird is drawn on the %s", n,
            drawn[i].what.c_str());
    if (card_up)
      CHECK(!overlap(bb, box_of("QR card", G.card)),
            "%s: the bird is drawn on the QR card", n);
  }
}

std::vector<std::string> v1(const char* a) { return std::vector<std::string>(1, a); }
std::vector<std::string> v2(const char* a, const char* b) {
  std::vector<std::string> v;
  if (a) v.push_back(a);
  if (b) v.push_back(b);
  return v;
}

Says scene_says(ObStage st) {
  const SceneCopy c = scene_copy(st);
  Says s;
  s.title = v2(c.title.full, c.title.narrow);
  s.body = v2(c.body.full, c.body.narrow);
  s.name = nullptr;
  s.join = false;
  s.bird_shown = true;
  s.settle_hop = false;
  return s;
}

// One glass, one ladder, QR or not: every scene, in provision.cpp's order.
void run_glass(const Env& e, int which, bool qr, bool turned) {
  fake_lvgl::disp_w() = e.w;
  fake_lvgl::disp_h() = e.h;
  g_fam = (e.dash || e.amoled) ? kBig : e.lean ? kLean : kStd;
  g_which = which;
  g_qr_fails = !qr;

  Run G;
  G.env = &e;
  G.which = which;
  G.qr = qr;
  G.turned = turned;
  G.frames = 0;
  G.g.w = e.w;
  G.g.h = e.h;
  G.g.round = kRoundBuild;
  onboardlayout::Rows r;
  if (kWideBuild) {
    // The dash line's Join stack: the title face, the card it asks for,
    // the label and caption faces; and its 300 px halo (F84).
    r.title_h = font_title()->line_height;
    r.card = kWideGlassCard;
    r.creds_h = font_label()->line_height;
    r.hint_h = font_caption()->line_height;
    G.j = wide_join(G.g, r);
  } else {
    r.title_h = font_body()->line_height;
    r.card = kSmallGlassCard;
    r.creds_h = font_caption()->line_height;
    r.hint_h = font_caption()->line_height;
    G.j = small_join(G.g, r);
  }

  lv_obj_t* home = lv_obj_create(nullptr);  // the normal UI underneath
  lv_scr_load(home);
  onboard_ui_create(kSsid, kPass);
  G.scr = lv_scr_act();
  CHECK(G.scr != home, "%s: onboard_ui_create loaded no screen",
        who(G, "create").c_str());
  if (G.scr == home || !find_objects(&G)) {
    lv_obj_del(home);
    return;
  }

  // The stuck-phone and PhoneJoined hints as provision.cpp hands them.
  const std::vector<std::string>& stuck =
      kWideBuild    ? g_minted.hint_wide
      : kRoundBuild ? g_minted.hint_round
                    : g_minted.hint_small;
  const std::vector<std::string>& phone =
      kWideBuild ? g_minted.phone_wide : g_minted.phone_small;

  Says hello = scene_says(ObStage::Hello);
  check_frame(G, "Hello", hello);

  Says join = scene_says(ObStage::Join);
  join.title = kWideBuild ? v2(wide_join_title(qr).full,
                               wide_join_title(qr).narrow)
                          : v2(join_title(qr), "");
  join.body = v1("");
  join.join = true;
  join.bird_shown = !qr;
  onboard_ui_stage(ObStage::Join, nullptr);
  check_frame(G, "Join", join);
  onboard_ui_hint(stuck[0].c_str(), stuck.size() > 1 ? stuck[1].c_str() : nullptr);
  join.stuck = stuck;
  check_frame(G, "Join, stuck-phone hint", join);

  Says pj = scene_says(ObStage::PhoneJoined);
  onboard_ui_stage(ObStage::PhoneJoined, nullptr);
  check_frame(G, "PhoneJoined", pj);
  onboard_ui_hint(phone[0].c_str(),
                  phone.size() > 1 ? phone[1].c_str() : nullptr);
  pj.coach = phone;
  check_frame(G, "PhoneJoined, no-page hint", pj);

  const std::string widest(32, 'W');
  const char* names[3] = {"HomeNet", widest.c_str(),
                          "Basement-Mesh-Extender-Office-5G"};
  for (int k = 0; k < 3; ++k) {
    Says cn = scene_says(ObStage::Connecting);
    cn.name = names[k];
    onboard_ui_stage(ObStage::Connecting, names[k]);
    char what[96];
    std::snprintf(what, sizeof(what), "Connecting \"%s\"", names[k]);
    check_frame(G, what, cn);
  }

  using canary::net::JoinFailure;
  const JoinFailure fails[] = {JoinFailure::NotFound, JoinFailure::BadPassword,
                               JoinFailure::NoAddress, JoinFailure::Unknown};
  for (size_t k = 0; k < sizeof(fails) / sizeof(fails[0]); ++k) {
    Says fl = scene_says(ObStage::Fail);
    fl.title = v2(canary::net::join_failure_label(fails[k]),
                  canary::net::join_failure_label_narrow(fails[k]));
    fl.bird_shown = false;
    onboard_ui_stage(ObStage::Fail, canary::net::join_failure_label(fails[k]),
                     canary::net::join_failure_label_narrow(fails[k]));
    onboard_ui_hint(canary::net::join_failure_hint(fails[k]),
                    canary::net::join_failure_hint_narrow(fails[k]));
    // Wide glass sets the whole fix on its hint row; small glass may take
    // the narrow form (hint_lines).
    fl.coach = v2(canary::net::join_failure_hint(fails[k]),
                  kWideBuild ? nullptr
                             : canary::net::join_failure_hint_narrow(fails[k]));
    char what[96];
    std::snprintf(what, sizeof(what), "Fail (%s)",
                  canary::net::join_failure_label(fails[k]));
    check_frame(G, what, fl);
  }
  Says bare = scene_says(ObStage::Fail);
  bare.bird_shown = false;
  onboard_ui_stage(ObStage::Fail, nullptr);
  check_frame(G, "Fail (no reason)", bare);

  join.stuck.clear();
  onboard_ui_stage(ObStage::Join, nullptr);
  check_frame(G, "Join again", join);

  Says ok = scene_says(ObStage::Success);
  ok.settle_hop = true;
  onboard_ui_stage(ObStage::Success, nullptr);
  check_frame(G, "Success", ok);

  onboard_ui_finish();
  CHECK(lv_scr_act() == home, "%s: onboard_ui_finish did not go home",
        who(G, "finish").c_str());
  std::printf("  %-18s %3dx%-3d %s %-8s %-5s %2d scenes  halo %3d at (%3d, "
              "%3d)  card %3d (qr %3d)%s\n",
              e.name.c_str() + 15, e.w, e.h,
              kRoundBuild ? "round" : kWideBuild ? "wide " : "rect ",
              which == 0 ? "default" : "heirloom", qr ? "qr" : "no-qr",
              G.frames, G.j.halo.d, e.w / 2 - G.j.halo.d / 2 + G.j.halo.x,
              G.j.halo.top, G.j.stack.card, G.j.stack.qr,
              turned ? "  (turned)" : "");
  lv_obj_del(home);
}

}  // namespace

int main() {
  load_ladders();
  load_minted_core();
  const std::vector<Env> envs = load_envs();
  std::printf("onboarding scenes, as onboard_ui.cpp draws them (%s build):\n",
              kRoundBuild  ? "round watch"
              : kWideBuild ? "wide glass"
                           : "rectangular small glass");
  // The panels main.cpp turns before provision_run() (F156, F157): the
  // nightlight's saved landscape rotation (320x180) and the dash line's
  // saved portrait one (480x800). Read off main.cpp, not typed.
  bool nl_turns = false, dash_turns = false;
  check_boot_rotation("canary::net::provision_run(", &nl_turns, &dash_turns);
  int glass = 0, turned = 0;
  for (size_t i = 0; i < envs.size(); ++i) {
    const Env& e = envs[i];
    const bool mine = kWideBuild    ? e.dash
                      : kRoundBuild ? (e.watch && !e.nightstand)
                                    : e.nightstand;
    if (!mine) continue;
    bool seen = false;
    for (size_t j = 0; j < i; ++j)
      seen = seen || (envs[j].w == e.w && envs[j].h == e.h &&
                      envs[j].cfg == e.cfg && envs[j].lean == e.lean);
    if (seen) continue;
    glass++;
    const std::string cfg = slurp(std::string(FW_DIR) +
                                  "/configs/canary-display/" + e.cfg +
                                  "/config.h");
    const bool turns = (define_int(cfg, "CD_NIGHTLIGHT") == 1 && nl_turns) ||
                       (e.dash && dash_turns);
    for (int t = 0; t < (turns ? 2 : 1); ++t) {
      Env c = e;
      if (t) {
        c.w = e.h;
        c.h = e.w;
        turned++;
      }
      for (int which = 0; which < 2; ++which)
        for (int qr = 1; qr >= 0; --qr) run_glass(c, which, qr != 0, t == 1);
    }
  }
  CHECK(glass >= (kRoundBuild ? 1 : kWideBuild ? 3 : 4),
        "only %d glass envs for this build", glass);
  // The wide build turns the dash, dash7 and nightstand7; the small one the
  // nightlight. The round watch turns nothing.
  CHECK(turned == (kRoundBuild ? 0 : kWideBuild ? 3 : 1),
        "%d turned panels for this build", turned);
  if (g_fail == 0) {
    std::printf("ALL ONBOARD SCENES TESTS PASSED\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", g_fail);
  return 1;
}
