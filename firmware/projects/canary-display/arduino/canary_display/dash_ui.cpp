// src/ui/dash_ui.cpp — the Dash's 800x480 face, LVGL edition.
//
// Quiet Glass on a wall: true-black ground, #141414 cards with hairline
// edges and rounded corners, severity as a rounded spine pill plus label
// (never color alone), soft glow instead of hard strips. Motion budget:
// an unacked-alert card breathes its glow; the hold-to-ack ring sweeps;
// nothing else moves.
//
// Not compiled on the Nightstand 7 (CD_NIGHTSTAND7): that build shares this
// flavor's panel/HAL but its standing face is the bedside one
// (nightstand7_ui.cpp), so the wall dashboard would be dead weight in a
// 16 MB image and dead code in the review.
#include "flavor_config.h"
#if defined(CD_FLAVOR_DASH) && !defined(CD_NIGHTSTAND7)

#include <Arduino.h>
#include <lvgl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "dash_ui.h"
#include "settings_ui.h"
#include "commission_ui.h"
#include "theme.h"
#include "character.h"
#include "canary_mark.h"
#include "fleet_figure.h"
#include "trust.h"
#include "fleet_figures.h"
#include "version.h"
#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
#include "fleet_cards.h"
#include "journal_instance.h"
#endif
#if defined(FEATURE_CARE) && FEATURE_CARE
#include "care_glue.h"
#if defined(FEATURE_HUB_WEATHER) && FEATURE_HUB_WEATHER
#include "bedside.h"
#endif
#endif
#if defined(FEATURE_MIC_ALARM) && FEATURE_MIC_ALARM && \
    defined(HAS_MICROPHONE) && HAS_MICROPHONE
#include "mic_alarm.h"  // 4.3C: live mic state on the honesty sheet
#endif

namespace canary::ui {

using canary::fleet::Fleet;
using canary::fleet::Sev;
using canary::fleet::Witness;

namespace {

constexpr int GRID_COLS = 2, GRID_ROWS = 4;
constexpr int MAX_CARDS = GRID_COLS * GRID_ROWS;
// 8 rows of the larger dash type (20 px name + 16 px meta, 42 px pitch)
// fill the rail to y=437; a 9th ran past the footer at the new sizes.
constexpr int EV_ROWS = 8;

constexpr lv_coord_t HDR_H = 64;
constexpr lv_coord_t TL_W = 272;
constexpr lv_coord_t CARD_W = 248, CARD_H = 92, GAP = 8;

lv_obj_t* s_scr = nullptr;

// Header
lv_obj_t* s_headline = nullptr;
lv_obj_t* s_clock = nullptr;
lv_obj_t* s_glow = nullptr;      // soft severity bar under the header

// Cards
struct Card {
  lv_obj_t* box = nullptr;
  lv_obj_t* spine = nullptr;
  lv_obj_t* name = nullptr;
  lv_obj_t* state = nullptr;
  lv_obj_t* badge = nullptr;
  lv_obj_t* event = nullptr;
  lv_obj_t* meta = nullptr;
};
Card s_cards[MAX_CARDS];
lv_obj_t* s_more = nullptr;      // "+N more"
lv_obj_t* s_today = nullptr;     // time machine v1: the day's story
lv_obj_t* s_empty = nullptr;     // listening state

// Timeline
lv_obj_t* s_tl_title = nullptr;
struct EvRow {
  lv_obj_t* dot = nullptr;
  lv_obj_t* name = nullptr;
  lv_obj_t* meta = nullptr;
};
EvRow s_ev[EV_ROWS];

lv_obj_t* s_footer = nullptr;

// Motion state
lv_anim_t s_breath_anim;
lv_obj_t* s_breathing = nullptr;
lv_obj_t* s_ack_ring = nullptr;
lv_anim_t s_ack_anim;
bool s_ack_holding = false;
bool s_glow_pulsing = false;    // heartbeat owns the header bloom while true

// Proof sheet (trailblazer spec §1)
lv_obj_t* s_proof = nullptr;        // modal container (hidden when closed)
lv_obj_t* s_proof_qr = nullptr;
lv_obj_t* s_proof_title = nullptr;
lv_obj_t* s_proof_state = nullptr;
lv_obj_t* s_proof_cap = nullptr;
char s_proof_id[48] = {0};          // which witness the open sheet shows

#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
// History modal (trailblazer spec §7): the verifiable time machine. Tapping
// the "Past 24h" line opens a proof-carrying log of recent events; tapping any
// row re-opens its signed chain as a QR — history you can still prove.
constexpr int HIST_ROWS = 12;
constexpr lv_coord_t HIST_X = 24, HIST_Y = 20, HIST_W = 752, HIST_H = 440;
constexpr lv_coord_t HIST_ROW_Y0 = 84, HIST_ROW_H = 28;
lv_obj_t* s_hist = nullptr;          // modal container (hidden when closed)
lv_obj_t* s_hist_title = nullptr;
lv_obj_t* s_hist_summary = nullptr;
lv_obj_t* s_hist_rows[HIST_ROWS] = {nullptr};
lv_obj_t* s_hist_erase = nullptr;    // two-tap "erase all" (sovereignty)
lv_obj_t* s_hist_hint = nullptr;
bool s_hist_erase_armed = false;
uint32_t s_hist_erase_ms = 0;
#endif

#if defined(FEATURE_CARE) && FEATURE_CARE
// Roll Call modal (care wave §6): tap the headline to open. Live rows —
// walk the house and watch each canary answer.
constexpr int RC_ROWS = 8;
constexpr lv_coord_t RC_X = 130, RC_Y = 40, RC_W = 540, RC_H = 400;
constexpr int RC_FIG = 26;   // fixed figure slot per row — rows never reflow
lv_obj_t* s_rc = nullptr;
lv_obj_t* s_rc_title = nullptr;
lv_obj_t* s_rc_figs[RC_ROWS] = {nullptr};
lv_obj_t* s_rc_rows[RC_ROWS] = {nullptr};
lv_obj_t* s_rc_hint = nullptr;
#endif

// Transparency sheet (care wave §7): tap the footer to open. What this
// glass consumes/speaks/stores, what it never does — plus the cleaning-mode
// affordance ("wipe the glass" belongs where the honesty lives).
constexpr lv_coord_t AB_X = 200, AB_Y = 70, AB_W = 400, AB_H = 340;
lv_obj_t* s_about = nullptr;
lv_obj_t* s_about_title = nullptr;
lv_obj_t* s_about_body = nullptr;
lv_obj_t* s_about_clean = nullptr;
lv_obj_t* s_about_settings = nullptr;  // gear row -> settings surface
lv_obj_t* s_about_add = nullptr;       // plus row -> commissioning QR
lv_obj_t* s_clean_note = nullptr;    // full-screen countdown while locked
uint32_t s_clean_until_ms = 0;

// Fleet snapshot the tap router needs (updated each dash_ui_update).
const Fleet* s_fleet = nullptr;
uint32_t s_now_ms = 0;

lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* f, lv_color_t c) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, "");
  return l;
}

lv_obj_t* mk_box(lv_obj_t* parent) {
  lv_obj_t* b = lv_obj_create(parent);
  lv_obj_set_style_bg_color(b, col_surface(), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(b, col_edge(), 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
  return b;
}

void breath_cb(void* var, int32_t v) {
  lv_obj_set_style_shadow_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
}

void breathe(lv_obj_t* card, lv_color_t color, bool on) {
  if (s_breathing && (s_breathing != card || !on)) {
    lv_anim_del(s_breathing, breath_cb);
    lv_obj_set_style_shadow_width(s_breathing, 0, 0);
    s_breathing = nullptr;
  }
  if (!on || !card || s_breathing == card) return;
  s_breathing = card;
  lv_obj_set_style_shadow_color(card, color, 0);
  lv_obj_set_style_shadow_width(card, 24, 0);
  lv_obj_set_style_shadow_spread(card, 2, 0);
  lv_anim_init(&s_breath_anim);
  lv_anim_set_var(&s_breath_anim, card);
  lv_anim_set_exec_cb(&s_breath_anim, breath_cb);
  lv_anim_set_values(&s_breath_anim, LV_OPA_20, LV_OPA_70);
  lv_anim_set_time(&s_breath_anim, MOTION_BREATH_MS / 2);
  lv_anim_set_playback_time(&s_breath_anim, MOTION_BREATH_MS / 2);
  lv_anim_set_repeat_count(&s_breath_anim, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&s_breath_anim, lv_anim_path_ease_in_out);
  lv_anim_start(&s_breath_anim);
}

void ack_cb(void* var, int32_t v) {
  (void)var;
  lv_arc_set_bg_angles(s_ack_ring, 0, (uint16_t)v);
}

void upper(char* s) {
  for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

void proof_close() {
  if (s_proof) lv_obj_add_flag(s_proof, LV_OBJ_FLAG_HIDDEN);
  s_proof_id[0] = '\0';
}

// Render the proof sheet from raw ingredients: the pinned-pubkey key (id), the
// verbatim signed chain payload, a title, and a state line. Shared by the live
// witness sheet and the time-machine's per-record re-proof — the QR body is
// byte-identical in both, which is the whole point: a week-old event proves
// exactly like a live one. Moves the sheet to the foreground so it sits above
// the history modal when opened from it.
void proof_render(const char* title, const char* id, const char* chain_raw,
                  const char* state_line) {
  char pk[65];
  const bool have = chain_raw && chain_raw[0] &&
                    canary::trust::pinned_pubkey_hex(id, pk);
  lv_label_set_text_fmt(s_proof_title, "%.24s", title);
  lv_label_set_text(s_proof_state, state_line);
  if (have) {
    static char body[640];
    const int len = snprintf(body, sizeof(body),
                             "{\"v\":1,\"t\":\"securacv/%s/chain\","
                             "\"pk\":\"%s\",\"p\":%s}",
                             id, pk, chain_raw);
    if (len > 0 && (size_t)len < sizeof(body)) {
      lv_qrcode_update(s_proof_qr, body, (uint32_t)len);
      lv_obj_clear_flag(s_proof_qr, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(s_proof_cap,
                        "Scan to verify this signed chain\n"
                        "no app • no account • no cloud");
    } else {
      lv_obj_add_flag(s_proof_qr, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(s_proof_cap,
                        "This proof is too long to draw as a QR code\n"
                        "the full record is in your hub's event log");
    }
  } else {
    lv_obj_add_flag(s_proof_qr, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_proof_cap,
                      "No proof to show yet\n"
                      "it appears with this canary's first signed event");
  }
  snprintf(s_proof_id, sizeof(s_proof_id), "%s", id);
  lv_obj_move_foreground(s_proof);
  lv_obj_clear_flag(s_proof, LV_OBJ_FLAG_HIDDEN);
}

// Open the proof sheet for one live witness.
void proof_open(const canary::fleet::Witness& w) {
  char state[48];
  snprintf(state, sizeof(state), "%s  •  %s", link_label(w.link),
           badge_text(w.badge));
  proof_render(Fleet::display_name(w), w.id, w.chain_raw, state);
}

#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
// Wall-clock stamp for a journal record: "MM-DD HH:MM", or an em dash when the
// event was logged before SNTP (no honest place on a timeline).
void fmt_stamp(uint32_t epoch, char* out, size_t cap) {
  if (epoch == 0) { snprintf(out, cap, "  --  "); return; }
  const time_t t = (time_t)epoch;
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, cap, "%m-%d %H:%M", &tmv);
}

// Re-open a PAST event's proof from its journaled record.
void proof_open_record(const canary::fleet::JournalRecord& r) {
  char stamp[16];
  fmt_stamp(r.epoch, stamp, sizeof(stamp));
  char state[64];
  snprintf(state, sizeof(state), "%s  •  %s", stamp,
           badge_text((canary::fleet::Badge)r.badge));
  const char* title = r.name[0] ? r.name : r.id;
  proof_render(title, r.id, r.chain_raw, state);
}

void hist_close() {
  if (s_hist) lv_obj_add_flag(s_hist, LV_OBJ_FLAG_HIDDEN);
  s_hist_erase_armed = false;
}

// Populate and open the history modal from the journal ring (newest first).
void hist_open() {
  if (!s_hist) return;
  const auto& j = canary::fleet::the_journal();
  const int n = j.count();

  if (n == 0) {
    lv_label_set_text(s_hist_summary, "Nothing witnessed yet");
  } else {
    const uint32_t oldest = j.oldest_known_epoch();
    char since[24] = "";
    if (oldest) fmt_stamp(oldest, since, sizeof(since));
    lv_label_set_text_fmt(s_hist_summary, "%d event%s%s%s", n,
                          n == 1 ? "" : "s", oldest ? " • since " : "",
                          oldest ? since : "");
  }

  for (int i = 0; i < HIST_ROWS; i++) {
    const auto* r = j.at(i);   // newest at 0
    if (!r) { lv_label_set_text(s_hist_rows[i], ""); continue; }
    char stamp[16], human[40];
    fmt_stamp(r->epoch, stamp, sizeof(stamp));
    humanize_event(r->ev, human, sizeof(human));
    const char* who = r->name[0] ? r->name : r->id;
    // Verified events earn a check; everything else states its verdict word
    // (never a bare color) — honesty travels with the history.
    lv_obj_set_style_text_color(s_hist_rows[i],
                                sev_color((Sev)r->sev, false), 0);
    lv_label_set_text_fmt(s_hist_rows[i], "%s   •   %.12s   •   %.18s   •   %s",
                          stamp, who, human,
                          badge_text((canary::fleet::Badge)r->badge));
  }

  const int shown = n < HIST_ROWS ? n : HIST_ROWS;
  if (n > shown) {
    lv_label_set_text_fmt(s_hist_hint,
                          "showing %d of %d • tap a row to prove it", shown, n);
  } else {
    lv_label_set_text(s_hist_hint, "tap a row to prove it • tap away to close");
  }
  lv_label_set_text(s_hist_erase, "Erase all history");
  s_hist_erase_armed = false;

  lv_obj_move_foreground(s_hist);
  lv_obj_clear_flag(s_hist, LV_OBJ_FLAG_HIDDEN);
}
#endif  // FEATURE_TIME_MACHINE

#if defined(FEATURE_CARE) && FEATURE_CARE
// Roll Call rows re-render every update pass while the modal is open (the
// walk-test needs live ages, unlike the read-only history list).
void rc_render(const Fleet& fleet, uint32_t now) {
  const int n = fleet.count();
  for (int i = 0; i < RC_ROWS; i++) {
    const Witness* w = (i < n) ? fleet.at(i) : nullptr;
    if (!w) {
      lv_label_set_text(s_rc_rows[i], i == 0 && n == 0 ? "No witnesses yet" : "");
      if (s_rc_figs[i]) lv_obj_add_flag(s_rc_figs[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    // The witness's own picture from the shared ledger; an unresolvable
    // wire type keeps the slot hidden — never a guessed product.
    if (s_rc_figs[i]) {
      const auto* fig = canary::figures::figure_for(w->device_type);
      canary::ui::fleet_figure_set(s_rc_figs[i], fig ? fig->figure_id : nullptr);
    }
    const int32_t age_ms = (int32_t)(now - w->last_seen_ms);
    const bool just_answered = age_ms >= 0 && age_ms < 5000;
    char age[8];
    format_age(now, w->last_seen_ms, age, sizeof(age));
    char batt[12] = "-";
    if (w->battery_present && w->battery_pct >= 0) {
      snprintf(batt, sizeof(batt), "%d%%", (int)w->battery_pct);
    }
    char rssi[16] = "-";
    if (w->rssi_present) {
      snprintf(rssi, sizeof(rssi), "%s", signal_word((int)w->rssi_dbm));
    }
    const Sev s = fleet.witness_sev(*w, now);
    lv_obj_set_style_text_color(
        s_rc_rows[i],
        just_answered ? col_ok()
                      : (s >= Sev::Warn ? sev_color(s, false) : col_text()),
        0);
    lv_label_set_text_fmt(s_rc_rows[i],
                          "%.16s   •   %s ago   •   %s   •   %s%s",
                          Fleet::display_name(*w), age, batt, rssi,
                          just_answered ? "   " LV_SYMBOL_OK : "");
  }
}

void rc_open(const Fleet& fleet, uint32_t now) {
  if (!s_rc) return;
  rc_render(fleet, now);
  lv_obj_move_foreground(s_rc);
  lv_obj_clear_flag(s_rc, LV_OBJ_FLAG_HIDDEN);
}

void rc_close() {
  if (s_rc) lv_obj_add_flag(s_rc, LV_OBJ_FLAG_HIDDEN);
}
#endif  // FEATURE_CARE

void about_open(const Fleet& fleet) {
  if (!s_about) return;
  int journal_kept = 0;
#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
  journal_kept = canary::fleet::the_journal().count();
#endif
#if defined(FEATURE_MIC_ALARM) && FEATURE_MIC_ALARM && \
    defined(HAS_MICROPHONE) && HAS_MICROPHONE
  // Mic-bearing board (4.3C, display_mic_variant.md): the sheet must tell
  // the truth this hardware makes possible — and the truth of what the
  // firmware provably does with it. Live state, not a promise.
  lv_label_set_text_fmt(
      s_about_body,
      "Watches: %d %s, through your home hub only\n"
      "Speaks: its own check-ins, and the alerts you handle\n"
      "Keeps: %d events on this device - erasable in History\n"
      "Mic: %s - alarm patterns only, never speech;\n"
      "audio never recorded, never leaves this board\n"
      "Never: cloud, camera, or tracking IDs\n\n"
      "Firmware v%s",
      fleet.count(), fleet.count() == 1 ? "canary" : "canaries", journal_kept,
      canary::io::mic_listening() ? "LISTENING (amber chip lit)"
                                  : "off (driver uninstalled)",
      CANARY_FW_VERSION);
#else
  lv_label_set_text_fmt(
      s_about_body,
      "Watches: %d %s, through your home hub only\n"
      "Speaks: its own check-ins, and the alerts you handle\n"
      "Keeps: %d events on this device - erasable in History\n"
      "Never: cloud, camera, microphone, or tracking IDs\n\n"
      "Firmware v%s",
      fleet.count(), fleet.count() == 1 ? "canary" : "canaries", journal_kept,
      CANARY_FW_VERSION);
#endif
  lv_label_set_text(s_about_clean, LV_SYMBOL_REFRESH "  Wipe the glass - touch turns off for 30 s");
  lv_label_set_text(s_about_settings, LV_SYMBOL_SETTINGS "  Screen settings");
  lv_label_set_text(s_about_add, LV_SYMBOL_PLUS "  Add a canary");
  lv_obj_move_foreground(s_about);
  lv_obj_clear_flag(s_about, LV_OBJ_FLAG_HIDDEN);
}

void about_close() {
  if (s_about) lv_obj_add_flag(s_about, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────

// Bounded append for composed lines: snprintf returns the WOULD-BE length,
// so unchecked `to +=` can sail past the buffer and underflow the next
// call's remaining-space math (review catch). This keeps `to` in bounds.
static size_t appendf(char* buf, size_t cap, size_t to, const char* fmt, ...) {
  if (to >= cap) return cap - 1;
  va_list ap;
  va_start(ap, fmt);
  const int w = vsnprintf(buf + to, cap - to, fmt, ap);
  va_end(ap);
  if (w <= 0) return to;
  to += (size_t)w;
  return to >= cap ? cap - 1 : to;
}

void dash_ui_create() {
  // Re-entrant (ground-change rebuild): the caller cleaned the screen, so
  // every widget pointer is rebuilt below — but motion/modal state that
  // POINTS at old widgets must not survive into the new face
  // (use-after-free in breathe()/ack otherwise).
  s_breathing = nullptr;
  s_ack_holding = false;
  s_glow_pulsing = false;
  s_proof_id[0] = '\0';
#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
  s_hist_erase_armed = false;
#endif
  s_scr = lv_scr_act();
  lv_obj_set_style_bg_color(s_scr, col_bg(), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

  // ── Header ──
  s_headline = mk_label(s_scr, font_title(), col_text());
  lv_obj_align(s_headline, LV_ALIGN_TOP_LEFT, 20, 16);
  s_clock = mk_label(s_scr, font_title(), col_muted());
  lv_obj_align(s_clock, LV_ALIGN_TOP_RIGHT, -20, 16);
  s_glow = lv_obj_create(s_scr);
  lv_obj_set_size(s_glow, 800, 3);
  lv_obj_set_pos(s_glow, 0, HDR_H - 3);
  lv_obj_set_style_border_width(s_glow, 0, 0);
  lv_obj_set_style_radius(s_glow, 0, 0);
  lv_obj_set_style_bg_color(s_glow, col_faint(), 0);
  lv_obj_set_style_bg_opa(s_glow, LV_OPA_COVER, 0);
  // The soft bloom is opt-in per state (dash_ui_update): it stays OFF on the
  // calm, empty, and link-down screens, where a wide shadow only bands on this
  // RGB565 panel. Start as a clean hairline.
  lv_obj_set_style_shadow_width(s_glow, 0, 0);
  lv_obj_set_style_shadow_spread(s_glow, 0, 0);
  lv_obj_set_style_shadow_color(s_glow, col_faint(), 0);
  lv_obj_set_style_shadow_opa(s_glow, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(s_glow, LV_OBJ_FLAG_SCROLLABLE);

  // ── Cards ──
  for (int i = 0; i < MAX_CARDS; i++) {
    Card& c = s_cards[i];
    const int col = i % GRID_COLS, row = i / GRID_COLS;
    c.box = mk_box(s_scr);
    lv_obj_set_size(c.box, CARD_W, CARD_H);
    lv_obj_set_pos(c.box, 12 + col * (CARD_W + GAP),
                   HDR_H + 10 + row * (CARD_H + GAP));
    c.spine = lv_obj_create(c.box);
    lv_obj_set_size(c.spine, 5, CARD_H - 24);
    lv_obj_set_pos(c.spine, 10, 12);
    lv_obj_set_style_radius(c.spine, 3, 0);
    lv_obj_set_style_border_width(c.spine, 0, 0);
    lv_obj_set_style_bg_opa(c.spine, LV_OPA_COVER, 0);
    lv_obj_clear_flag(c.spine, LV_OBJ_FLAG_SCROLLABLE);
    // Four rows inside a 92 px card: label(20) + 3x caption(16) with 3-4 px
    // gaps. font_body (24 on dash) doesn't fit this geometry — the name row
    // would land exactly on the state row (post-#874 review catch).
    c.name = mk_label(c.box, font_label(), col_text());
    lv_obj_set_pos(c.name, 26, 8);
    c.state = mk_label(c.box, font_caption(), col_ok());
    lv_obj_set_style_text_letter_space(c.state, 1, 0);
    lv_obj_set_pos(c.state, 26, 32);
    c.badge = mk_label(c.box, font_caption(), col_muted());
    lv_obj_align(c.badge, LV_ALIGN_TOP_RIGHT, -12, 32);
    c.event = mk_label(c.box, font_caption(), col_muted());
    lv_obj_set_pos(c.event, 26, 51);
    c.meta = mk_label(c.box, font_caption(), col_faint());
    lv_obj_set_pos(c.meta, 26, 70);
    lv_obj_add_flag(c.box, LV_OBJ_FLAG_HIDDEN);
  }
  s_more = mk_label(s_scr, font_label(), col_muted());
  lv_obj_align(s_more, LV_ALIGN_BOTTOM_LEFT, 20, -8);
  s_today = mk_label(s_scr, font_caption(), col_muted());
  lv_obj_align(s_today, LV_ALIGN_BOTTOM_LEFT, 20, -28);
  // Empty-nest hero: the bird and its invitation, centered as one stack in the
  // content column left of the events rail (the rail keeps its own "Nothing
  // witnessed yet"). A fixed column width + centered text lets the pair read as
  // one balanced unit instead of two left-floating fragments.
  constexpr lv_coord_t COL_W = 800 - TL_W - 10;   // content column, left of rail
  lv_obj_t* bird = canary_mark_create(s_scr, 72);
  lv_obj_align(bird, LV_ALIGN_LEFT_MID, (COL_W - 72) / 2, -44);
  s_empty = mk_label(s_scr, font_body(), col_muted());
  lv_obj_set_width(s_empty, COL_W);
  lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_empty, LV_ALIGN_LEFT_MID, 0, 26);

  // ── Timeline ──
  lv_obj_t* rail = lv_obj_create(s_scr);
  lv_obj_set_size(rail, 1, 480 - HDR_H - 20);
  lv_obj_set_pos(rail, 800 - TL_W - 10, HDR_H + 10);
  lv_obj_set_style_bg_color(rail, col_edge(), 0);
  lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(rail, 0, 0);
  lv_obj_set_style_radius(rail, 0, 0);
  lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);

  s_tl_title = mk_label(s_scr, font_caption(), col_muted());
  lv_obj_set_style_text_letter_space(s_tl_title, 2, 0);
  lv_label_set_text(s_tl_title, "EVENTS");
  lv_obj_set_pos(s_tl_title, 800 - TL_W + 14, HDR_H + 12);

  for (int i = 0; i < EV_ROWS; i++) {
    EvRow& r = s_ev[i];
    const lv_coord_t y = HDR_H + 40 + i * 42;
    r.dot = lv_obj_create(s_scr);
    lv_obj_set_size(r.dot, 10, 10);
    lv_obj_set_pos(r.dot, 800 - TL_W + 14, y + 5);
    lv_obj_set_style_radius(r.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(r.dot, 0, 0);
    lv_obj_set_style_bg_opa(r.dot, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(r.dot, 10, 0);
    lv_obj_set_style_shadow_opa(r.dot, LV_OPA_40, 0);
    lv_obj_clear_flag(r.dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r.dot, LV_OBJ_FLAG_HIDDEN);
    r.name = mk_label(s_scr, font_label(), col_text());
    lv_obj_set_pos(r.name, 800 - TL_W + 34, y);
    r.meta = mk_label(s_scr, font_caption(), col_muted());
    // +23: clears the 20 px dash label font (+19 was tuned for 14 px).
    lv_obj_set_pos(r.meta, 800 - TL_W + 34, y + 23);
  }

  // ── Footer + ack ring ──
  s_footer = mk_label(s_scr, font_caption(), col_faint());
  lv_obj_align(s_footer, LV_ALIGN_BOTTOM_RIGHT, -16, -8);

  // ── Proof sheet (hidden until a card is tapped) ──
  s_proof = mk_box(s_scr);
  lv_obj_set_size(s_proof, 460, 400);
  lv_obj_center(s_proof);
  lv_obj_set_style_shadow_width(s_proof, 40, 0);
  lv_obj_set_style_shadow_color(s_proof, lv_color_black(), 0);
  lv_obj_set_style_shadow_opa(s_proof, LV_OPA_60, 0);
  s_proof_title = mk_label(s_proof, font_body(), col_text());
  lv_obj_align(s_proof_title, LV_ALIGN_TOP_MID, 0, 12);
  s_proof_state = mk_label(s_proof, font_caption(), col_muted());
  // 44/68: the 24 px dash title ends at 36 (14/38/60 was the 16 px tuning).
  lv_obj_align(s_proof_state, LV_ALIGN_TOP_MID, 0, 44);
  lv_obj_t* qr_card = lv_obj_create(s_proof);
  lv_obj_set_size(qr_card, 256, 256);
  lv_obj_align(qr_card, LV_ALIGN_TOP_MID, 0, 68);
  lv_obj_set_style_bg_color(qr_card, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(qr_card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(qr_card, 12, 0);
  lv_obj_set_style_border_width(qr_card, 0, 0);
  lv_obj_set_style_pad_all(qr_card, 8, 0);
  lv_obj_clear_flag(qr_card, LV_OBJ_FLAG_SCROLLABLE);
  s_proof_qr = mk_qrcode(qr_card, 240);
  lv_obj_center(s_proof_qr);
  s_proof_cap = mk_label(s_proof, font_caption(), col_muted());
  lv_obj_set_style_text_align(s_proof_cap, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_proof_cap, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_add_flag(s_proof, LV_OBJ_FLAG_HIDDEN);

  s_ack_ring = lv_arc_create(s_scr);
  lv_obj_set_size(s_ack_ring, 120, 120);
  lv_obj_center(s_ack_ring);
  lv_arc_set_rotation(s_ack_ring, 270);
  lv_arc_set_bg_angles(s_ack_ring, 0, 1);
  lv_obj_set_style_arc_width(s_ack_ring, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(s_ack_ring, true, LV_PART_MAIN);
  lv_obj_set_style_arc_color(s_ack_ring, col_text(), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(s_ack_ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_ack_ring, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_clear_flag(s_ack_ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_ack_ring, LV_OBJ_FLAG_HIDDEN);

#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
  // ── History modal (spec §7): the verifiable time machine, hidden until the
  // "Past 24h" line is tapped ──
  s_hist = mk_box(s_scr);
  lv_obj_set_size(s_hist, HIST_W, HIST_H);
  lv_obj_set_pos(s_hist, HIST_X, HIST_Y);
  lv_obj_set_style_shadow_width(s_hist, 40, 0);
  lv_obj_set_style_shadow_color(s_hist, lv_color_black(), 0);
  lv_obj_set_style_shadow_opa(s_hist, LV_OPA_60, 0);
  s_hist_title = mk_label(s_hist, font_body(), col_text());
  lv_label_set_text(s_hist_title, "The story so far");
  lv_obj_align(s_hist_title, LV_ALIGN_TOP_LEFT, 24, 16);
  s_hist_summary = mk_label(s_hist, font_caption(), col_muted());
  lv_obj_align(s_hist_summary, LV_ALIGN_TOP_LEFT, 24, 48);
  for (int i = 0; i < HIST_ROWS; i++) {
    s_hist_rows[i] = mk_label(s_hist, font_label(), col_text());
    lv_obj_set_pos(s_hist_rows[i], 24, HIST_ROW_Y0 + i * HIST_ROW_H);
  }
  s_hist_hint = mk_label(s_hist, font_caption(), col_faint());
  lv_obj_align(s_hist_hint, LV_ALIGN_BOTTOM_RIGHT, -20, -14);
  s_hist_erase = mk_label(s_hist, font_caption(), col_muted());
  lv_obj_align(s_hist_erase, LV_ALIGN_BOTTOM_LEFT, 24, -14);
  lv_obj_add_flag(s_hist, LV_OBJ_FLAG_HIDDEN);
#endif

#if defined(FEATURE_CARE) && FEATURE_CARE
  // ── Roll Call modal (care wave §6), hidden until the headline is tapped ──
  s_rc = mk_box(s_scr);
  lv_obj_set_size(s_rc, RC_W, RC_H);
  lv_obj_set_pos(s_rc, RC_X, RC_Y);
  lv_obj_set_style_shadow_width(s_rc, 40, 0);
  lv_obj_set_style_shadow_color(s_rc, lv_color_black(), 0);
  lv_obj_set_style_shadow_opa(s_rc, LV_OPA_60, 0);
  s_rc_title = mk_label(s_rc, font_body(), col_text());
  lv_label_set_text(s_rc_title, "Roll call");
  lv_obj_align(s_rc_title, LV_ALIGN_TOP_LEFT, 24, 16);
  for (int i = 0; i < RC_ROWS; i++) {
    // Each row leads with the witness's own figure (the ledger's picture,
    // hidden until its wire type resolves), then the text line.
    s_rc_figs[i] = fleet_figure_create(s_rc, nullptr, RC_FIG);
    if (s_rc_figs[i]) lv_obj_set_pos(s_rc_figs[i], 24, 52 + i * 34);
    s_rc_rows[i] = mk_label(s_rc, font_label(), col_text());
    lv_obj_set_pos(s_rc_rows[i], 24 + RC_FIG + 10, 56 + i * 34);
  }
  s_rc_hint = mk_label(s_rc, font_caption(), col_faint());
  lv_label_set_text(s_rc_hint,
                    "walk past a canary - its row lights as it answers • tap away to close");
  lv_obj_align(s_rc_hint, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_obj_add_flag(s_rc, LV_OBJ_FLAG_HIDDEN);
#endif

  // ── Transparency sheet (care wave §7), hidden until the footer is tapped ──
  s_about = mk_box(s_scr);
  lv_obj_set_size(s_about, AB_W, AB_H);
  lv_obj_set_pos(s_about, AB_X, AB_Y);
  lv_obj_set_style_shadow_width(s_about, 40, 0);
  lv_obj_set_style_shadow_color(s_about, lv_color_black(), 0);
  lv_obj_set_style_shadow_opa(s_about, LV_OPA_60, 0);
  s_about_title = mk_label(s_about, font_body(), col_text());
  lv_label_set_text(s_about_title, "What this glass does");
  lv_obj_align(s_about_title, LV_ALIGN_TOP_LEFT, 24, 16);
  s_about_body = mk_label(s_about, font_caption(), col_muted());
  lv_obj_set_style_text_line_space(s_about_body, 7, 0);
  lv_obj_set_pos(s_about_body, 24, 56);
  s_about_add = mk_label(s_about, font_caption(), col_muted());
  lv_obj_align(s_about_add, LV_ALIGN_BOTTOM_LEFT, 24, -76);
  s_about_settings = mk_label(s_about, font_caption(), col_muted());
  lv_obj_align(s_about_settings, LV_ALIGN_BOTTOM_LEFT, 24, -46);
  s_about_clean = mk_label(s_about, font_caption(), col_muted());
  lv_obj_align(s_about_clean, LV_ALIGN_BOTTOM_LEFT, 24, -16);
  lv_obj_add_flag(s_about, LV_OBJ_FLAG_HIDDEN);

  // Cleaning-mode note (full width, replaces the sheet while locked).
  s_clean_note = mk_label(s_scr, font_body(), col_muted());
  lv_obj_align(s_clean_note, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(s_clean_note, "");
}

void dash_ui_update(const Fleet& fleet, uint32_t now, const DashState& st) {
  if (!s_scr) return;
  s_fleet = &fleet;
  s_now_ms = now;
  const int n = fleet.count();
  const Sev worst = fleet.worst(now);
  const lv_color_t tcol = st.night ? ncol_text() : col_text();
  const lv_color_t mcol = st.night ? ncol_muted() : col_muted();
  const lv_color_t fcol = st.night ? ncol_muted() : col_faint();

  // ── Header sentence ──
  lv_obj_set_style_text_color(s_headline, tcol, 0);
  if (n == 0) {
    lv_label_set_text(s_headline,
                      st.mqtt_ok ? "Listening for canaries"
                                 : (st.wifi_ok
                                        ? "Can't reach your hub"
                                        : (st.wifi_reason ? st.wifi_reason
                                                          : "No WiFi")));
  } else if (worst <= Sev::Notice) {
    // The calm sentence speaks in the Character's voice; the trouble
    // branch below never does (sev_name is invariant by rule).
    lv_label_set_text_fmt(s_headline, "%s  •  %d %s%s",
                          active_voice().all_quiet, n,
                          n == 1 ? "canary" : "canaries",
                          fleet.all_verified() ? "  •  verified" : "");
  } else {
    char word[16];
    snprintf(word, sizeof(word), "%s", canary::fleet::sev_name(worst));
    upper(word);
    lv_obj_set_style_text_color(s_headline, sev_color(worst, st.night), 0);
    // Acknowledged carries its attribution — which glass quieted the house.
    if (st.acked && fleet.ack_by()[0]) {
      lv_label_set_text_fmt(s_headline, "%s  •  handled by %.16s", word,
                            fleet.ack_by());
    } else {
      lv_label_set_text_fmt(s_headline, "%s%s", word,
                            st.acked ? "  •  acknowledged" : "");
    }
  }
  if (st.time_valid) {
    lv_label_set_text_fmt(s_clock, "%02d:%02d", st.clock_hh, st.clock_mm);
  } else {
    lv_label_set_text(s_clock, "");
  }
  lv_obj_set_style_text_color(s_clock, mcol, 0);
  // Header underline: a crisp full-width hairline whose color states the
  // house's worst severity. It goes neutral on the empty and link-down screens
  // — a green "all-clear" over an empty or offline house is a false comfort,
  // and absence of alarm is itself information. A soft bloom is spent only on
  // real trouble (and the once-a-minute all-verified heartbeat below): a wide
  // shadow is exactly what bands on the RGB565 panel, so calm states stay a
  // clean line.
  const bool trouble = worst >= Sev::Warn;   // real trouble always colors the line
  lv_color_t glow;
  if (trouble) {
    glow = sev_color(worst, st.night);
  } else if (n == 0 || !st.wifi_ok || !st.mqtt_ok) {
    // Nothing urgent, and the house is empty or a link is down: a neutral
    // hairline, never a green "all-clear" — that would be a false comfort.
    glow = st.night ? ncol_muted() : col_faint();
  } else {
    // Populated, links up, all calm: the honest all-good line (green).
    glow = sev_color(worst, st.night);
  }
  lv_obj_set_style_bg_color(s_glow, glow, 0);
  lv_obj_set_style_shadow_color(s_glow, glow, 0);
  if (!s_glow_pulsing) {
    lv_obj_set_style_shadow_width(s_glow, trouble ? 12 : 0, 0);
    lv_obj_set_style_shadow_opa(s_glow, trouble ? LV_OPA_30 : LV_OPA_TRANSP, 0);
  }

  // ── Cards ──
  lv_obj_t* attention = nullptr;
  lv_color_t attention_col = col_alert();
  for (int i = 0; i < MAX_CARDS; i++) {
    Card& c = s_cards[i];
    const Witness* w = (i < n) ? fleet.at(i) : nullptr;
    if (!w) {
      lv_obj_add_flag(c.box, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(c.box, LV_OBJ_FLAG_HIDDEN);
    const Sev s = fleet.witness_sev(*w, now);
    const lv_color_t sc = sev_color(s, st.night);

    lv_obj_set_style_bg_color(c.box, st.night ? col_bg() : col_surface(), 0);
    lv_obj_set_style_border_color(c.box, st.night ? ncol_muted() : col_edge(), 0);
    lv_obj_set_style_bg_color(c.spine, sc, 0);

    lv_obj_set_style_text_color(c.name, tcol, 0);
    if (w->name[0] && w->room[0]) {
      lv_label_set_text_fmt(c.name, "%.12s • %.9s", w->name, w->room);
    } else {
      lv_label_set_text_fmt(c.name, "%.18s", Fleet::display_name(*w));
    }

    char state[24];
    snprintf(state, sizeof(state), "%s", link_label(w->link));
    upper(state);
    const bool muted = Fleet::mute_active(*w, now);
    if (muted && !w->tamper && s < Sev::Alert) {
      // The honest bypass: card stays, spine goes hairline, state says so.
      lv_obj_set_style_text_color(c.state, fcol, 0);
      lv_label_set_text(c.state, "MUTED • UNTIL MORNING");
      lv_obj_set_style_bg_color(c.spine, st.night ? ncol_muted() : col_edge(), 0);
    } else {
      lv_obj_set_style_text_color(c.state, sc, 0);
      if (w->tamper) {
        lv_label_set_text_fmt(c.state, "%s • TAMPER", state);
      } else if (w->link == canary::fleet::Link::Lost) {
        // A lost canary's next step is physical: it stopped answering, so
        // someone should check the device — say so where the red is.
        lv_label_set_text(c.state, "LOST • CHECK ITS POWER");
      } else {
        lv_label_set_text(c.state, state);
      }
    }

    lv_obj_set_style_text_color(c.badge, badge_color(w->badge, st.night), 0);
    lv_label_set_text(c.badge, badge_text(w->badge));

    lv_obj_set_style_text_color(c.event, mcol, 0);
    if (w->has_event) {
      char human[40], age[8];
      humanize_event(w->last_event, human, sizeof(human));
      format_age(now, w->last_event_ms, age, sizeof(age));
      lv_label_set_text_fmt(c.event, "%s  •  %s", human, age);
    } else {
      lv_label_set_text(c.event, "No events yet");
    }

    lv_obj_set_style_text_color(c.meta, fcol, 0);
    char wb[128] = "";
    // Canary Cards (docs/standard/CANARY_CARDS.md): a card-bearing witness
    // (canary-sense) renders its coarse claim vocabulary as a compact card
    // strip — presence/occupants/range + breathing/BPM — instead of the
    // generic field list. The trust/event cards are dropped (skip_shown): the
    // card's own badge + event row already carry those. Non-card witnesses
    // keep the wellbeing + comfort text below.
    if (canary::fleet::has_cards(*w)) {
      static const canary::fleet::FleetLimits kCardLimits;
      canary::fleet::CardSet cs;
      canary::fleet::build_cards(*w, now, kCardLimits, cs);
      char strip[104];
      if (canary::fleet::format_card_strip(cs, /*skip_shown=*/true, strip,
                                           sizeof(strip)) > 0) {
        snprintf(wb, sizeof(wb), "   %s", strip);
      }
    } else if (w->wb_present) {
      snprintf(wb, sizeof(wb), "   breathing %s",
               w->wb_breathing ? LV_SYMBOL_OK : "-");
    }
    // Room comfort, when the witness reports it (parent-unit table stakes).
    // Sign carried explicitly: -0.5° would otherwise render as 0.5°, since
    // %d has no sign at zero (review catch).
    if (w->temp_present) {
      const size_t off = strlen(wb);
      const char* sign = w->temp_c10 < 0 ? "-" : "";
      const int whole = abs(w->temp_c10 / 10);
      const int frac = abs(w->temp_c10 % 10);
      if (w->humidity_pct >= 0) {
        snprintf(wb + off, sizeof(wb) - off, "   %s%d.%d\xC2\xB0 • %d%%",
                 sign, whole, frac, (int)w->humidity_pct);
      } else {
        snprintf(wb + off, sizeof(wb) - off, "   %s%d.%d\xC2\xB0",
                 sign, whole, frac);
      }
    }
    if (w->battery_present && w->battery_pct >= 0) {
      lv_label_set_text_fmt(c.meta, "%s %d%%   %.12s%s",
                            w->battery_pct < 25 ? LV_SYMBOL_BATTERY_1
                                                : LV_SYMBOL_BATTERY_3,
                            (int)w->battery_pct, w->fw, wb);
    } else {
      lv_label_set_text_fmt(c.meta, "%.14s%s", w->fw, wb);
    }

    if (s >= Sev::Alert && !st.acked && !attention) {
      attention = c.box;
      attention_col = sc;
    }
  }
  breathe(attention, attention_col, attention != nullptr);

  lv_obj_set_style_text_color(s_more, mcol, 0);
  if (n > MAX_CARDS) lv_label_set_text_fmt(s_more, "+%d more", n - MAX_CARDS);
  else lv_label_set_text(s_more, "");

  // Time machine (spec §7): the rolling day in one honest sentence, and — when
  // there's a proof-carrying journal to browse — an affordance to open it.
  // The care wave leads the line: the morning summary of what quiet hours
  // silenced, then the rhythm verdict, then the day's tally.
  lv_obj_set_style_text_color(s_today, fcol, 0);
  const int day_total = fleet.history_total();
  char today[192] = "";
  size_t to = 0;
#if defined(FEATURE_HUB_WEATHER) && FEATURE_HUB_WEATHER
  // Nightstand wave: weather-before-you-rise leads the morning line.
  if (!st.night && st.time_valid && st.clock_hh < 10) {
    char wx[72];
    if (canary::care::bedside_morning_line(wx, sizeof(wx))) {
      to = appendf(today, sizeof(today), to, "%s   •   ", wx);
    }
  }
#endif
#if defined(FEATURE_CARE) && FEATURE_CARE
  if (!st.night && canary::care::night_ledger().count() > 0) {
    char sum[48];
    canary::care::night_ledger().summary(sum, sizeof(sum));
    to = appendf(today, sizeof(today), to, "%s   •   ", sum);
  }
#if defined(FEATURE_RHYTHM) && FEATURE_RHYTHM
  {
    char rl[80];
    if (canary::care::rhythm_line(rl, sizeof(rl)) > 0 && to < sizeof(today)) {
      to = appendf(today, sizeof(today), to, "%s   •   ", rl);
    }
  }
#endif
#endif
  if (!st.time_valid || n == 0) {
    // no day story without a clock/witnesses; care segments may still show
  } else if (day_total == 0) {
    to = appendf(today, sizeof(today), to, "Past 24h • nothing witnessed");
  } else {
    to = appendf(today, sizeof(today), to, "Past 24h • %d %s • worst: %s",
                 day_total, day_total == 1 ? "event" : "events",
                 canary::fleet::sev_name(fleet.history_worst_day()));
  }
#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
  if (today[0] && canary::fleet::the_journal().count() > 0 &&
      to < sizeof(today)) {
    snprintf(today + to, sizeof(today) - to, "   •   tap to review");
  }
#endif
  lv_label_set_text(s_today, today);

  lv_obj_set_style_text_color(s_empty, mcol, 0);
  lv_label_set_text(s_empty, n == 0 ? "Plug in a canary - it finds\nthis display on its own" : "");
  // Living canary: the dash bird only has a perch on the empty-nest face
  // (with witnesses, the cards own the wall); the mood engine picks the
  // face, including asleep at night.
  canary_mark_mood(n == 0 ? st.bird : CanaryMood::Hidden);

  // ── Timeline ──
  lv_obj_set_style_text_color(s_tl_title, mcol, 0);
  const int evn = fleet.events_count();
  for (int i = 0; i < EV_ROWS; i++) {
    EvRow& r = s_ev[i];
    const auto* e = (i < evn) ? fleet.event_at(i) : nullptr;
    if (!e) {
      lv_obj_add_flag(r.dot, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(r.name, i == 0 && evn == 0 ? "Nothing witnessed yet" : "");
      lv_label_set_text(r.meta, "");
      lv_obj_set_style_text_color(r.name, mcol, 0);
      continue;
    }
    char human[40], age[8];
    humanize_event(e->name, human, sizeof(human));
    format_age(now, e->at_ms, age, sizeof(age));
    const lv_color_t ec = sev_color(e->sev, st.night);
    lv_obj_clear_flag(r.dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(r.dot, ec, 0);
    lv_obj_set_style_shadow_color(r.dot, ec, 0);
    lv_obj_set_style_text_color(r.name,
                                e->sev >= Sev::Warn ? ec : tcol, 0);
    lv_label_set_text_fmt(r.name, "%.26s", human);
    lv_obj_set_style_text_color(r.meta, mcol, 0);
    lv_label_set_text_fmt(r.meta, "%s ago  •  %.14s%s", age, e->device,
                          e->signed_flag ? "  •  signed" : "");
  }

  // The heartbeat (spec §4): the header glow swells once a minute, only
  // when everything is reachable AND verified — absence is information. The
  // bloom is armed just for the pulse and recedes to the clean hairline after,
  // so no steady soft shadow lingers to band on the panel.
  static uint32_t s_last_beat_ms = 0;
  if (!st.night && n > 0 && worst <= Sev::Notice && fleet.all_verified() &&
      st.wifi_ok && st.mqtt_ok && !s_glow_pulsing &&
      (int32_t)(now - s_last_beat_ms) >= (int32_t)CD_HEARTBEAT_UI_MS) {
    s_last_beat_ms = now;
    s_glow_pulsing = true;
    lv_obj_set_style_shadow_width(s_glow, 14, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_glow);
    lv_anim_set_exec_cb(&a, [](void* var, int32_t v) {
      lv_obj_set_style_shadow_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
    });
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_50);
    lv_anim_set_time(&a, 800);
    lv_anim_set_playback_time(&a, 800);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&a, [](lv_anim_t* an) {
      // Recede to the clean hairline; dash_ui_update owns the glow again.
      lv_obj_set_style_shadow_width((lv_obj_t*)an->var, 0, 0);
      lv_obj_set_style_shadow_opa((lv_obj_t*)an->var, LV_OPA_TRANSP, 0);
      s_glow_pulsing = false;
    });
    lv_anim_start(&a);
  }

  // ── Footer: emergency contact first, honesty line otherwise ──
  // A panel dispatches; a witness display INFORMS whoever is standing in
  // front of it. During an unacked Tier-1, the footer carries the household
  // emergency contact (secrets.h, never committed) instead of the tagline.
  if (worst >= Sev::Alert && !st.acked && EMERGENCY_CONTACT[0]) {
    lv_obj_set_style_text_color(s_footer, st.night ? ncol_alert() : col_alert(), 0);
    lv_label_set_text_fmt(s_footer, "Need help? %.48s", EMERGENCY_CONTACT);
  } else if (!st.wifi_ok) {
    lv_obj_set_style_text_color(s_footer, st.night ? ncol_alert() : col_alert(), 0);
    // Failure formula: what happened — what it's doing about it · what to
    // try if it persists. Never a dead end.
    lv_label_set_text(s_footer,
                      "No WiFi - reconnecting • if this stays, check your router");
  } else if (!st.mqtt_ok) {
    lv_obj_set_style_text_color(s_footer, st.night ? ncol_alert() : col_warn(), 0);
    lv_label_set_text(s_footer,
                      "Can't reach your hub - retrying • check the hub is on");
  } else {
    lv_obj_set_style_text_color(s_footer, fcol, 0);
    lv_label_set_text(s_footer, "status display • not a life-safety device");
  }

#if defined(FEATURE_CARE) && FEATURE_CARE
  // Roll Call stays live while open (the whole point is watching rows
  // answer as you walk the house).
  if (s_rc && !lv_obj_has_flag(s_rc, LV_OBJ_FLAG_HIDDEN)) rc_render(fleet, now);
#endif

  // Cleaning-mode countdown (the one sanctioned full-screen text).
  if (s_clean_note) {
    if ((int32_t)(now - s_clean_until_ms) < 0) {
      lv_obj_move_foreground(s_clean_note);
      lv_label_set_text_fmt(s_clean_note, "Wipe away - touch wakes in %lu s",
                            (unsigned long)((s_clean_until_ms - now) / 1000 + 1));
    } else if (lv_label_get_text(s_clean_note)[0]) {
      lv_label_set_text(s_clean_note, "");
    }
  }
}

bool dash_ui_handle_tap(int16_t x, int16_t y) {
  if (!s_scr || !s_fleet) return false;

  // An open proof sheet swallows any tap (that's how you close it). It sits
  // above the history modal, so closing it returns you to the list.
  if (s_proof && !lv_obj_has_flag(s_proof, LV_OBJ_FLAG_HIDDEN)) {
    proof_close();
    return true;
  }

  // Transparency sheet: the "wipe the glass" row arms cleaning mode, the
  // gear row opens the settings surface; anywhere else closes.
  if (s_about && !lv_obj_has_flag(s_about, LV_OBJ_FLAG_HIDDEN)) {
    lv_area_t ca;
    lv_obj_get_coords(s_about_clean, &ca);
    if (x >= ca.x1 - 8 && x <= ca.x2 + 8 && y >= ca.y1 - 8 && y <= ca.y2 + 8) {
      s_clean_until_ms = s_now_ms + 30000;
      about_close();
      return true;
    }
    lv_obj_get_coords(s_about_settings, &ca);
    if (x >= ca.x1 - 8 && x <= ca.x2 + 8 && y >= ca.y1 - 8 && y <= ca.y2 + 8) {
      about_close();
      settings_ui_open();
      return true;
    }
    lv_obj_get_coords(s_about_add, &ca);
    if (x >= ca.x1 - 8 && x <= ca.x2 + 8 && y >= ca.y1 - 8 && y <= ca.y2 + 8) {
      about_close();
      commission_ui_open();
      return true;
    }
    about_close();
    return true;
  }

#if defined(FEATURE_CARE) && FEATURE_CARE
  // Open Roll Call swallows the tap to close (no row actions — it's a
  // diagnostics view, not a control surface).
  if (s_rc && !lv_obj_has_flag(s_rc, LV_OBJ_FLAG_HIDDEN)) {
    rc_close();
    return true;
  }
#endif

#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
  // Open history modal: rows re-prove; the erase affordance takes two taps;
  // anywhere else dismisses.
  if (s_hist && !lv_obj_has_flag(s_hist, LV_OBJ_FLAG_HIDDEN)) {
    lv_area_t ea;
    lv_obj_get_coords(s_hist_erase, &ea);
    if (x >= ea.x1 - 8 && x <= ea.x2 + 8 && y >= ea.y1 - 8 && y <= ea.y2 + 8) {
      // Sovereignty: erasing the home's whole record is deliberate — arm on
      // the first tap, confirm on a second within 4 s.
      if (s_hist_erase_armed && (int32_t)(s_now_ms - s_hist_erase_ms) < 4000) {
        canary::fleet::journal_wipe_all();
        hist_close();
      } else {
        s_hist_erase_armed = true;
        s_hist_erase_ms = s_now_ms;
        lv_label_set_text(s_hist_erase, "Tap again to erase everything");
        lv_obj_set_style_text_color(s_hist_erase, col_alert(), 0);
      }
      return true;
    }
    const auto& j = canary::fleet::the_journal();
    const int shown = j.count() < HIST_ROWS ? j.count() : HIST_ROWS;
    for (int i = 0; i < shown; i++) {
      const int ry = HIST_Y + HIST_ROW_Y0 + i * HIST_ROW_H;
      if (x >= HIST_X + 12 && x <= HIST_X + HIST_W - 12 && y >= ry &&
          y < ry + HIST_ROW_H) {
        const auto* r = j.at(i);
        if (r) { proof_open_record(*r); return true; }
      }
    }
    hist_close();
    return true;
  }

  // Base layer: tapping the "Past 24h • tap to review" line opens the machine.
  if (s_today && canary::fleet::the_journal().count() > 0) {
    lv_area_t ta;
    lv_obj_get_coords(s_today, &ta);
    if (x >= ta.x1 - 8 && x <= ta.x2 + 8 && y >= ta.y1 - 8 && y <= ta.y2 + 8) {
      hist_open();
      return true;
    }
  }
#endif  // FEATURE_TIME_MACHINE

#if defined(FEATURE_CARE) && FEATURE_CARE
  // Headline tap -> Roll Call (the "how is everyone, really" view).
  if (s_headline && s_fleet->count() > 0) {
    lv_area_t ha;
    lv_obj_get_coords(s_headline, &ha);
    if (x >= ha.x1 - 8 && x <= ha.x2 + 8 && y >= ha.y1 - 8 && y <= ha.y2 + 8) {
      rc_open(*s_fleet, s_now_ms);
      return true;
    }
  }
#endif

  // Footer tap -> transparency sheet (what this glass does / never does).
  if (s_footer) {
    lv_area_t fa;
    lv_obj_get_coords(s_footer, &fa);
    if (x >= fa.x1 - 8 && x <= fa.x2 + 8 && y >= fa.y1 - 8 && y <= fa.y2 + 8) {
      about_open(*s_fleet);
      return true;
    }
  }

  // Card hit-test (same geometry the create pass laid down).
  const int card = dash_ui_card_at(x, y);
  if (card >= 0) {
    const canary::fleet::Witness* w = s_fleet->at(card);
    if (w) {
      proof_open(*w);
      return true;
    }
  }
  return false;
}

int dash_ui_card_at(int16_t x, int16_t y) {
  if (!s_fleet) return -1;
  const int n = s_fleet->count();
  for (int i = 0; i < MAX_CARDS && i < n; i++) {
    const int col = i % GRID_COLS, row = i / GRID_COLS;
    const int cx = 12 + col * (CARD_W + GAP);
    const int cy = HDR_H + 10 + row * (CARD_H + GAP);
    if (x >= cx && x < cx + CARD_W && y >= cy && y < cy + CARD_H) return i;
  }
  return -1;
}

bool dash_ui_touch_locked(uint32_t now_ms) {
  return (int32_t)(now_ms - s_clean_until_ms) < 0;
}

void dash_ui_ack_hold(bool active) {
  if (!s_ack_ring) return;
  if (active && !s_ack_holding) {
    s_ack_holding = true;
    lv_obj_clear_flag(s_ack_ring, LV_OBJ_FLAG_HIDDEN);
    lv_anim_init(&s_ack_anim);
    lv_anim_set_var(&s_ack_anim, s_ack_ring);
    lv_anim_set_exec_cb(&s_ack_anim, ack_cb);
    lv_anim_set_values(&s_ack_anim, 1, 360);
    lv_anim_set_time(&s_ack_anim, MOTION_ACK_MS);
    lv_anim_set_path_cb(&s_ack_anim, lv_anim_path_linear);
    lv_anim_start(&s_ack_anim);
  } else if (!active && s_ack_holding) {
    s_ack_holding = false;
    lv_anim_del(s_ack_ring, ack_cb);
    lv_obj_add_flag(s_ack_ring, LV_OBJ_FLAG_HIDDEN);
  }
}

}  // namespace canary::ui

#endif  // CD_FLAVOR_DASH
