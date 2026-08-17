// src/ui/glance_ui.cpp — the Watch Station's round face, LVGL edition.
//
// Design contract (display_ux_design.md §Design language): readable across
// a dark room in under a second; motion is rationed — a 220 ms page fade,
// a 2 s breath on an unacked alert, the 900 ms hold-to-ack sweep, and
// nothing else moves, ever.
#include "flavor_config.h"
#ifdef CD_FLAVOR_WATCH

#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "glance_ui.h"
#include "round_frame.h"
#include "theme.h"
#include "canary_mark.h"
#include "character.h"
#include "trust.h"
#include "version.h"
#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
#include <time.h>
#include "fleet_cards.h"
#include "journal_instance.h"
#endif
#if defined(FEATURE_CARE) && FEATURE_CARE
#include "care_glue.h"
#if (defined(FEATURE_HUB_WEATHER) && FEATURE_HUB_WEATHER) || \
    (defined(FEATURE_COMFORT_WORDS) && FEATURE_COMFORT_WORDS)
#include "bedside.h"
#endif
#endif

namespace canary::ui {

using canary::fleet::Fleet;
using canary::fleet::Sev;
using canary::fleet::Witness;
using canary::fleet::the_fleet;

namespace {

constexpr int MAX_ARCS = CD_FLEET_MAX_DEVICES;

// List pages (events / history / roll call) ride the Round Frame engine:
// four rows on an equator-centered stack. Five rows reached y=202, where the
// disc offers 126 px — the physical glass cut those rows mid-character (the
// emulator's square canvas never showed it). Four readable rows beat five
// clipped ones; every row of this stack keeps a 180+ px chord (the geometry
// is pinned in tests_host/test_round_frame_core.cpp).
constexpr int EV_ROWS = 4;
constexpr int LIST_PITCH = 32;   // label line + caption line + breathing room

// Row i's name-line y (meta rides +16 under it). Bias +6 clears the page
// title without giving up the wide latitudes.
int list_row_y(int i, int bias = 6) {
  return roundframe::row_stack_y(roundframe::kDiscDiameter, EV_ROWS,
                                 LIST_PITCH, i, bias);
}

lv_obj_t* s_scr = nullptr;

// Halo page
lv_obj_t* s_pg_halo = nullptr;
lv_obj_t* s_arcs[MAX_ARCS] = {nullptr};
lv_obj_t* s_hero = nullptr;       // big center word / count
lv_obj_t* s_hero_sub = nullptr;   // "5 canaries" / device demanding a look
lv_obj_t* s_hero_badge = nullptr; // "verified" / "acknowledged"
lv_obj_t* s_clock = nullptr;
lv_obj_t* s_banner = nullptr;     // wifi/broker honesty line

// Detail page
lv_obj_t* s_pg_dev = nullptr;
lv_obj_t* s_dev_ring = nullptr;
lv_obj_t* s_dev_name = nullptr;
lv_obj_t* s_dev_state = nullptr;
lv_obj_t* s_dev_event = nullptr;
lv_obj_t* s_dev_meta = nullptr;   // chain badge + battery
lv_obj_t* s_dev_pos = nullptr;    // "2 of 5"

// Events page
lv_obj_t* s_pg_ev = nullptr;
lv_obj_t* s_ev_title = nullptr;
lv_obj_t* s_ev_name[EV_ROWS] = {nullptr};
lv_obj_t* s_ev_meta[EV_ROWS] = {nullptr};

#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
// History page (trailblazer spec §7): the durable, wall-clock story. Where the
// events page reads the live ring in relative age ("3m ago"), this reads the
// proof-carrying journal in wall time with the verdict that stood when each
// event fired — the record that survives a reboot when persistence is on.
lv_obj_t* s_pg_history = nullptr;
lv_obj_t* s_thist_title = nullptr;
lv_obj_t* s_thist_summary = nullptr;
lv_obj_t* s_thist_name[EV_ROWS] = {nullptr};
lv_obj_t* s_thist_meta[EV_ROWS] = {nullptr};
#endif

#if defined(FEATURE_CARE) && FEATURE_CARE
// Roll Call page (display_care_wave.md §6): per-witness diagnostics — the
// IQ-Panel-grade walk test no ambient display ships. Rows light up live as
// each canary answers.
constexpr int RC_ROWS = EV_ROWS;  // same Round Frame stack as the lists
lv_obj_t* s_pg_rc = nullptr;
lv_obj_t* s_rc_title = nullptr;
lv_obj_t* s_rc_name[RC_ROWS] = {nullptr};
lv_obj_t* s_rc_meta[RC_ROWS] = {nullptr};
lv_obj_t* s_rc_more = nullptr;
#endif

// Transparency page (display_care_wave.md §7): what this glass consumes,
// speaks, stores — and everything it will never do. Research says elders
// accept monitoring they can SEE the shape of; this page is that mirror,
// on the device itself, for everyone in the house.
lv_obj_t* s_pg_about = nullptr;
lv_obj_t* s_about_title = nullptr;
lv_obj_t* s_about_body = nullptr;

// Settings doorway (settings wave): the rotation's last stop. It only
// invites — the long-press opens the real settings surface (settings_ui),
// so a sleepy tap-cycle past it can never rearrange the screen.
lv_obj_t* s_pg_settings = nullptr;
lv_obj_t* s_set_glyph = nullptr;
lv_obj_t* s_set_title = nullptr;
lv_obj_t* s_set_hint = nullptr;

// Proof page (trailblazer spec §1)
lv_obj_t* s_pg_proof = nullptr;
lv_obj_t* s_proof_card = nullptr;   // white ground behind the QR
lv_obj_t* s_proof_qr = nullptr;
lv_obj_t* s_proof_who = nullptr;
lv_obj_t* s_proof_cap = nullptr;

// Heartbeat ring (spec §4) — the sanctioned 4th motion
lv_obj_t* s_beat_ring = nullptr;

// Ack hold ring (overlay, all pages)
lv_obj_t* s_ack_ring = nullptr;
lv_anim_t s_ack_anim;
bool s_ack_holding = false;

// Breathing animation target (the arc demanding attention)
lv_anim_t s_breath_anim;
lv_obj_t* s_breathing = nullptr;

int s_shown_page = -1;

lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* f, lv_color_t c) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, "");
  return l;
}

lv_obj_t* mk_page(lv_obj_t* parent) {
  lv_obj_t* p = lv_obj_create(parent);
  lv_obj_set_size(p, 240, 240);
  lv_obj_set_pos(p, 0, 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(p, 0, 0);
  lv_obj_set_style_pad_all(p, 0, 0);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
  return p;
}

// A bare, non-interactive arc: Quiet Glass draws with the background arc
// only (smooth AA stroke, rounded caps); indicator and knob are silenced.
lv_obj_t* mk_ring(lv_obj_t* parent, int16_t d, int16_t width) {
  lv_obj_t* a = lv_arc_create(parent);
  lv_obj_set_size(a, d, d);
  lv_obj_center(a);
  lv_arc_set_rotation(a, 270);           // 12 o'clock start
  lv_arc_set_bg_angles(a, 0, 360);
  lv_arc_set_value(a, 0);
  lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  return a;
}

void breath_cb(void* var, int32_t v) {
  lv_obj_set_style_arc_opa((lv_obj_t*)var, (lv_opa_t)v, LV_PART_MAIN);
}

void breathe(lv_obj_t* arc, bool on) {
  if (s_breathing == arc && on) return;
  if (s_breathing) {
    lv_anim_del(s_breathing, breath_cb);
    lv_obj_set_style_arc_opa(s_breathing, LV_OPA_COVER, LV_PART_MAIN);
    s_breathing = nullptr;
  }
  if (!on || !arc) return;
  s_breathing = arc;
  lv_anim_init(&s_breath_anim);
  lv_anim_set_var(&s_breath_anim, arc);
  lv_anim_set_exec_cb(&s_breath_anim, breath_cb);
  lv_anim_set_values(&s_breath_anim, LV_OPA_COVER, LV_OPA_40);
  lv_anim_set_time(&s_breath_anim, MOTION_BREATH_MS / 2);
  lv_anim_set_playback_time(&s_breath_anim, MOTION_BREATH_MS / 2);
  lv_anim_set_repeat_count(&s_breath_anim, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&s_breath_anim, lv_anim_path_ease_in_out);
  lv_anim_start(&s_breath_anim);
}

void fade_cb(void* var, int32_t v) {
  lv_obj_set_style_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
}

// Page order after the per-device details: events, [history], [roll call],
// proof, transparency, settings. One map so the count and the dispatch
// can't drift.
struct PageMap {
  int events = -1, history = -1, rollcall = -1, proof = -1, about = -1;
  int settings = -1;
  int count = 0;
};

PageMap page_map(int devices) {
  PageMap m;
  int idx = 1 + devices;
  m.events = idx++;
#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
  m.history = idx++;
#endif
#if defined(FEATURE_CARE) && FEATURE_CARE
  m.rollcall = idx++;
#endif
  m.proof = idx++;
  m.about = idx++;
  m.settings = idx++;
  m.count = idx;
  return m;
}

void show_page(lv_obj_t* page) {
  // Leaving the halo also parks its breathing anim — no animation may keep
  // ticking against a hidden object (review catch: rationed motion includes
  // rationed CPU).
  if (page != s_pg_halo) breathe(nullptr, false);
  lv_obj_t* pages[] = {s_pg_halo, s_pg_dev, s_pg_ev, s_pg_proof, s_pg_about,
                       s_pg_settings,
#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
                       s_pg_history,
#endif
#if defined(FEATURE_CARE) && FEATURE_CARE
                       s_pg_rc,
#endif
  };
  for (lv_obj_t* p : pages) {
    if (p == page) {
      lv_obj_clear_flag(p, LV_OBJ_FLAG_HIDDEN);
      // lv_anim_start dedups same var+exec_cb in v8; the explicit del makes
      // the no-stacking intent visible rather than folklore.
      lv_anim_del(p, fade_cb);
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, p);
      lv_anim_set_exec_cb(&a, fade_cb);
      lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
      lv_anim_set_time(&a, MOTION_PAGE_MS);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
      lv_anim_start(&a);
    } else {
      lv_anim_del(p, fade_cb);
      lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void upper(char* s) {
  for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

// ── Page renderers ───────────────────────────────────────────────────────

void update_halo(const Fleet& fleet, uint32_t now, const GlanceState& st) {
  const int n = fleet.count();
  const Sev worst = fleet.worst(now);
  const lv_color_t tcol = st.night ? ncol_text() : col_text();
  const lv_color_t mcol = st.night ? ncol_muted() : col_muted();

  // Witness halo: one smooth arc per device, 12 o'clock start, 4° gaps.
  lv_obj_t* attention = nullptr;
  for (int i = 0; i < MAX_ARCS; i++) {
    if (i >= n || n == 0) {
      lv_obj_add_flag(s_arcs[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const Witness* w = fleet.at(i);
    if (!w) continue;
    lv_obj_clear_flag(s_arcs[i], LV_OBJ_FLAG_HIDDEN);
    // Signed math on purpose: the fleet caps at 8 (span >= 45°), but if
    // that cap ever grows, a tiny span must degrade to a sliver — never
    // underflow uint16 into a full-circle arc (review catch).
    const int span = 360 / n;
    const int a0 = i * span + 2;
    const int a1 = (i + 1) * span - 2;
    lv_arc_set_bg_angles(s_arcs[i], (uint16_t)a0,
                         (uint16_t)(a1 > a0 ? a1 : a0 + 1));
    const Sev s = fleet.witness_sev(*w, now);
    // A muted witness's arc goes hairline-faint: present, deliberately
    // quieted, impossible to mistake for healthy OR to forget entirely.
    if (Fleet::mute_active(*w, now) && s < Sev::Alert) {
      lv_obj_set_style_arc_color(s_arcs[i],
                                 st.night ? ncol_muted() : col_edge(),
                                 LV_PART_MAIN);
      continue;
    }
    lv_obj_set_style_arc_color(s_arcs[i], sev_color(s, st.night), LV_PART_MAIN);
    if (s >= Sev::Alert && !st.acked && !attention) attention = s_arcs[i];
  }
  // Empty fleet: a single faint full ring says "listening".
  if (n == 0) {
    lv_obj_clear_flag(s_arcs[0], LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_bg_angles(s_arcs[0], 0, 360);
    lv_obj_set_style_arc_color(s_arcs[0], st.night ? ncol_muted() : col_edge(),
                               LV_PART_MAIN);
  }
  breathe(attention, attention != nullptr);

  // Hero: the one thing that matters.
  lv_obj_set_style_text_color(s_hero, tcol, 0);
  lv_obj_set_style_text_color(s_hero_sub, mcol, 0);
  // Living canary: the engine owns the perch. It hides the bird during a
  // live unacked alarm (the instrument-grade handoff), and during Warn-band
  // trouble it now shows the CAUSE — Searching above a "quiet too long"
  // hero (looking for the late bird), Calling when one is lost. Only the
  // no-clock-with-witnesses fallback stays a UI decision.
  canary_mark_mood(n == 0 || st.time_valid ? st.bird : CanaryMood::Hidden);
  if (n == 0 && !st.time_valid) {
    // Nothing at all yet: no witnesses AND no clock. The only honest face
    // is the listening state.
    lv_obj_set_style_text_font(s_hero, font_title(), 0);
    lv_label_set_text(s_hero, "Listening");
    lv_label_set_text(s_hero_sub,
                      st.mqtt_ok ? "for canaries" :
                      (st.wifi_ok ? "finding your hub"
                                  : (st.wifi_reason ? st.wifi_reason
                                                    : "waiting for wifi")));
    lv_label_set_text(s_hero_badge, "");
  } else if (worst <= Sev::Notice) {
    // Standalone-first (nightstand wave): with a clock but no canaries yet,
    // the glass is already a great bedside clock — time hero, weather and
    // sun lines below. The fleet story joins when the first canary does.
    // All quiet: the TIME becomes the hero (display_care_wave.md §1) — a
    // bedside glance is a clock check 20x a day, and every one of them
    // absorbs the security state peripherally. Falls back to the words
    // when the clock isn't valid yet.
    lv_obj_set_style_text_font(s_hero, font_title(), 0);
    if (st.time_valid && n == 0) {
      lv_label_set_text_fmt(s_hero, "%02d:%02d", st.clock_hh, st.clock_mm);
      lv_label_set_text(s_hero_sub,
                        st.mqtt_ok ? "no canaries yet • hold to add"
                                   : (st.wifi_ok ? "still looking for your hub"
                                                 : "waiting for wifi"));
    } else if (st.time_valid) {
      lv_label_set_text_fmt(s_hero, "%02d:%02d", st.clock_hh, st.clock_mm);
      // The calm word speaks in the Character's voice (ambient copy only
      // — trouble words never come from the Voice table).
      lv_label_set_text_fmt(s_hero_sub, "%s • %d %s",
                            active_voice().all_quiet_low, n,
                            n == 1 ? "canary" : "canaries");
    } else {
      lv_label_set_text(s_hero, active_voice().all_quiet);
      lv_label_set_text_fmt(s_hero_sub, "%d %s", n,
                            n == 1 ? "canary" : "canaries");
    }
    // Badge line, best story first: the morning summary (what the night
    // silenced), else the rhythm line, else the earned verified tick.
    char care_line[80] = "";
#if defined(FEATURE_CARE) && FEATURE_CARE
    if (!st.night && canary::care::night_ledger().count() > 0) {
      canary::care::night_ledger().summary(care_line, sizeof(care_line));
    }
#endif
    // Nightstand wave: at night the badge is the bedroom (peek = time +
    // comfort); in the morning it is the day ahead (weather before you
    // rise); in the evening, the sun going down.
#if defined(FEATURE_COMFORT_WORDS) && FEATURE_COMFORT_WORDS
    if (!care_line[0] && st.night) {
      canary::care::bedside_comfort_line(fleet, care_line, sizeof(care_line));
    }
#endif
#if defined(FEATURE_HUB_WEATHER) && FEATURE_HUB_WEATHER
    if (!care_line[0] && !st.night && st.time_valid && st.clock_hh < 10) {
      canary::care::bedside_morning_line(care_line, sizeof(care_line));
    }
    if (!care_line[0] && !st.night && st.time_valid && st.clock_hh >= 17) {
      canary::care::bedside_evening_line(care_line, sizeof(care_line));
    }
#endif
#if defined(FEATURE_CARE) && FEATURE_CARE
#if defined(FEATURE_RHYTHM) && FEATURE_RHYTHM
    if (!care_line[0]) {
      canary::care::rhythm_line(care_line, sizeof(care_line));
    }
#endif
#endif
    if (care_line[0]) {
      lv_label_set_text_fmt(s_hero_badge, "%.34s", care_line);
      lv_obj_set_style_text_color(s_hero_badge, mcol, 0);
    } else if (fleet.all_verified()) {
      lv_label_set_text(s_hero_badge, LV_SYMBOL_OK "  verified");
      lv_obj_set_style_text_color(s_hero_badge,
                                  st.night ? ncol_muted() : col_ok(), 0);
    } else {
      lv_label_set_text(s_hero_badge, "");
    }
  } else {
    char word[16];
    snprintf(word, sizeof(word), "%s", canary::fleet::sev_name(worst));
    upper(word);
    lv_obj_set_style_text_font(s_hero, font_title(), 0);
    lv_obj_set_style_text_color(s_hero, sev_color(worst, st.night), 0);
    lv_label_set_text(s_hero, word);
    // Which witness demands the look (worst first hit).
    lv_label_set_text(s_hero_sub, "");
    for (int i = 0; i < n; i++) {
      const Witness* w = fleet.at(i);
      if (w && fleet.witness_sev(*w, now) == worst) {
        lv_label_set_text_fmt(s_hero_sub, "%.20s", Fleet::display_name(*w));
        break;
      }
    }
    // Acknowledged carries its attribution — which glass quieted the house.
    if (st.acked && fleet.ack_by()[0]) {
      lv_label_set_text_fmt(s_hero_badge, "handled • %.16s", fleet.ack_by());
    } else {
      lv_label_set_text(s_hero_badge,
                        st.acked ? "acknowledged" : "hold to acknowledge");
    }
    lv_obj_set_style_text_color(s_hero_badge, mcol, 0);
  }

  // Small clock (redundant while the time IS the hero; honest otherwise).
  // n==0 counts: standalone clock mode also puts the time in the hero slot
  // (review catch: without this the corner clock doubled it).
  const bool clock_is_hero = st.time_valid && worst <= Sev::Notice;
  if (st.time_valid && !clock_is_hero) {
    lv_label_set_text_fmt(s_clock, "%02d:%02d", st.clock_hh, st.clock_mm);
  } else {
    lv_label_set_text(s_clock, "");
  }
  lv_obj_set_style_text_color(s_clock, mcol, 0);
  if (n == 0) {
    // Standalone clock mode: the hero sub already carries the link story
    // ("still looking for your hub" / "waiting for wifi"), and with no
    // witnesses there is no "last known" to be honest about — a duplicate
    // red banner would just be noise on a nightstand.
    lv_label_set_text(s_banner, "");
  } else if (!st.wifi_ok) {
    // Banner copy is sized to its low-latitude chord (140 px at +84) — the
    // fit would ellipsize a longer line rather than let the rim cut it.
    lv_label_set_text(s_banner, LV_SYMBOL_WIFI " no wifi • retrying");
    lv_obj_set_style_text_color(s_banner,
                                st.night ? ncol_alert() : col_alert(), 0);
  } else if (!st.mqtt_ok) {
    lv_label_set_text(s_banner, "hub lost • last known");
    lv_obj_set_style_text_color(s_banner,
                                st.night ? ncol_alert() : col_warn(), 0);
  } else {
    lv_label_set_text(s_banner, "");
  }
}

void update_device(const Fleet& fleet, uint32_t now, const GlanceState& st,
                   int idx) {
  const Witness* w = fleet.at(idx);
  if (!w) return;
  const Sev s = fleet.witness_sev(*w, now);
  const bool muted = Fleet::mute_active(*w, now);
  const lv_color_t tcol = st.night ? ncol_text() : col_text();
  const lv_color_t mcol = st.night ? ncol_muted() : col_muted();

  lv_obj_set_style_arc_color(
      s_dev_ring,
      muted && s < Sev::Alert ? (st.night ? ncol_muted() : col_edge())
                              : sev_color(s, st.night),
      LV_PART_MAIN);

  lv_obj_set_style_text_color(s_dev_name, tcol, 0);
  if (w->name[0] && w->room[0]) {
    lv_label_set_text_fmt(s_dev_name, "%.12s • %.10s", w->name, w->room);
  } else {
    lv_label_set_text_fmt(s_dev_name, "%.18s", Fleet::display_name(*w));
  }

  if (muted && s < Sev::Alert) {
    // The honest bypass: state says muted (never hides), and the same
    // gesture that muted it un-mutes it.
    lv_obj_set_style_text_color(s_dev_state, mcol, 0);
    lv_label_set_text(s_dev_state, "muted");
  } else {
    lv_obj_set_style_text_color(s_dev_state, sev_color(s, st.night), 0);
    lv_label_set_text(s_dev_state, link_label(w->link));
  }

  lv_obj_set_style_text_color(s_dev_event, mcol, 0);
  if (w->has_event) {
    char human[48], age[8];
    humanize_event(w->last_event, human, sizeof(human));
    format_age(now, w->last_event_ms, age, sizeof(age));
    lv_label_set_text_fmt(s_dev_event, "%s\n%s ago", human, age);
  } else {
    lv_label_set_text(s_dev_event, "no events yet");
  }

  char batt[128] = "";  // "•"/"-"/LVGL symbols are multi-byte; keep headroom
  // Canary Cards (docs/standard/CANARY_CARDS.md): a card-bearing witness
  // (canary-sense) shows its coarse claim vocabulary — presence/occupants/
  // range + breathing/BPM — from the same card model the wall dash renders,
  // instead of the wellbeing-only line. skip_shown drops the trust/event
  // cards the badge + event row above already carry.
  if (canary::fleet::has_cards(*w)) {
    static const canary::fleet::FleetLimits kCardLimits;
    canary::fleet::CardSet cs;
    canary::fleet::build_cards(*w, now, kCardLimits, cs);
    char strip[96];
    if (canary::fleet::format_card_strip(cs, /*skip_shown=*/true, strip,
                                         sizeof(strip)) > 0) {
      snprintf(batt, sizeof(batt), "  •  %s", strip);
    }
  } else if (w->wb_present) {
    snprintf(batt, sizeof(batt), "  •  breathing %s",
             w->wb_breathing ? LV_SYMBOL_OK : "-");
  }
  if (w->battery_present && w->battery_pct >= 0) {
    const size_t off = strlen(batt);
    snprintf(batt + off, sizeof(batt) - off, "  •  %s %d%%",
             w->battery_pct < 25 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_3,
             (int)w->battery_pct);
  }
  // Room comfort, when the witness reports it (parent-unit table stakes).
  // Sign carried explicitly: -0.5° would otherwise render as 0.5°, since
  // %d has no sign at zero (review catch).
  if (w->temp_present) {
    const size_t off = strlen(batt);
    const char* sign = w->temp_c10 < 0 ? "-" : "";
    const int whole = abs(w->temp_c10 / 10);
    const int frac = abs(w->temp_c10 % 10);
    if (w->humidity_pct >= 0) {
      snprintf(batt + off, sizeof(batt) - off, "  •  %s%d.%d\xC2\xB0 %d%%",
               sign, whole, frac, (int)w->humidity_pct);
    } else {
      snprintf(batt + off, sizeof(batt) - off, "  •  %s%d.%d\xC2\xB0",
               sign, whole, frac);
    }
  }
  lv_obj_set_style_text_color(s_dev_meta, badge_color(w->badge, st.night), 0);
  lv_label_set_text_fmt(s_dev_meta, "%s%s", badge_text(w->badge), batt);

  lv_obj_set_style_text_color(s_dev_pos, st.night ? ncol_muted() : col_faint(), 0);
#if defined(FEATURE_CARE) && FEATURE_CARE
  lv_label_set_text_fmt(s_dev_pos, "%d of %d • hold to %s", idx + 1,
                        fleet.count(), muted ? "unmute" : "mute");
#else
  lv_label_set_text_fmt(s_dev_pos, "%d of %d", idx + 1, fleet.count());
#endif
}

void update_events(const Fleet& fleet, uint32_t now, const GlanceState& st) {
  const lv_color_t tcol = st.night ? ncol_text() : col_text();
  const lv_color_t mcol = st.night ? ncol_muted() : col_muted();
  lv_obj_set_style_text_color(s_ev_title, mcol, 0);

  const int n = fleet.events_count();
  for (int i = 0; i < EV_ROWS; i++) {
    const auto* e = (i < n) ? fleet.event_at(i) : nullptr;
    if (!e) {
      lv_label_set_text(s_ev_name[i], i == 0 && n == 0 ? "Nothing witnessed" : "");
      lv_label_set_text(s_ev_meta[i], "");
      lv_obj_set_style_text_color(s_ev_name[i], mcol, 0);
      continue;
    }
    char human[40], age[8];
    humanize_event(e->name, human, sizeof(human));
    format_age(now, e->at_ms, age, sizeof(age));
    lv_obj_set_style_text_color(
        s_ev_name[i], e->sev >= Sev::Warn ? sev_color(e->sev, st.night) : tcol, 0);
    lv_label_set_text_fmt(s_ev_name[i], "%.24s", human);
    lv_obj_set_style_text_color(s_ev_meta[i], mcol, 0);
    lv_label_set_text_fmt(s_ev_meta[i], "%s • %.14s%s", age, e->device,
                          e->signed_flag ? " • signed" : "");
  }
}

#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
// History page: the durable, wall-clock story from the proof-carrying journal
// (spec §7). Read-only on the round face — deep re-proof of a single past
// event is a dash gesture (it has room to hit-test a list); here the value is
// the honest narrative, wall-stamped and verdict-carrying, that survives a
// reboot when persistence is on.
void update_history(const Fleet& fleet, uint32_t now, const GlanceState& st) {
  (void)fleet;
  (void)now;
  const lv_color_t tcol = st.night ? ncol_text() : col_text();
  const lv_color_t mcol = st.night ? ncol_muted() : col_muted();
  lv_obj_set_style_text_color(s_thist_title, mcol, 0);
  lv_obj_set_style_text_color(s_thist_summary, mcol, 0);

  const auto& j = canary::fleet::the_journal();
  const int n = j.count();
  if (n == 0) {
    lv_label_set_text(s_thist_summary, "nothing yet");
  } else {
    lv_label_set_text_fmt(s_thist_summary, "%d kept", n);
  }

  for (int i = 0; i < EV_ROWS; i++) {
    const auto* r = (i < n) ? j.at(i) : nullptr;
    if (!r) {
      lv_label_set_text(s_thist_name[i], "");
      lv_label_set_text(s_thist_meta[i], "");
      continue;
    }
    char human[40], stamp[16];
    humanize_event(r->ev, human, sizeof(human));
    if (r->epoch == 0) {
      snprintf(stamp, sizeof(stamp), "--:--");
    } else {
      const time_t t = (time_t)r->epoch;
      struct tm tmv;
      localtime_r(&t, &tmv);
      strftime(stamp, sizeof(stamp), "%m-%d %H:%M", &tmv);
    }
    lv_obj_set_style_text_color(
        s_thist_name[i],
        r->sev >= (uint8_t)Sev::Warn ? sev_color((Sev)r->sev, st.night) : tcol,
        0);
    lv_label_set_text_fmt(s_thist_name[i], "%.22s", human);
    lv_obj_set_style_text_color(s_thist_meta[i], mcol, 0);
    lv_label_set_text_fmt(s_thist_meta[i], "%s • %s", stamp,
                          badge_text((canary::fleet::Badge)r->badge));
  }
}
#endif  // FEATURE_TIME_MACHINE

#if defined(FEATURE_CARE) && FEATURE_CARE
// Roll Call: every witness answers for itself — last word, battery, its own
// WiFi signal. Walk-test built in: a row that reported in the last few
// seconds lights green, so you can walk the house and watch each canary
// answer (display_care_wave.md §6).
void update_rollcall(const Fleet& fleet, uint32_t now, const GlanceState& st) {
  const lv_color_t tcol = st.night ? ncol_text() : col_text();
  const lv_color_t mcol = st.night ? ncol_muted() : col_muted();
  lv_obj_set_style_text_color(s_rc_title, mcol, 0);

  const int n = fleet.count();
  for (int i = 0; i < RC_ROWS; i++) {
    const Witness* w = (i < n) ? fleet.at(i) : nullptr;
    if (!w) {
      lv_label_set_text(s_rc_name[i], i == 0 && n == 0 ? "No witnesses yet" : "");
      lv_label_set_text(s_rc_meta[i], "");
      lv_obj_set_style_text_color(s_rc_name[i], mcol, 0);
      continue;
    }
    const int32_t age_ms = (int32_t)(now - w->last_seen_ms);
    const bool just_answered = age_ms >= 0 && age_ms < 5000;
    char age[8];
    format_age(now, w->last_seen_ms, age, sizeof(age));
    lv_obj_set_style_text_color(
        s_rc_name[i],
        just_answered ? col_ok()
                      : (fleet.witness_sev(*w, now) >= Sev::Warn
                             ? sev_color(fleet.witness_sev(*w, now), st.night)
                             : tcol),
        0);
    lv_label_set_text_fmt(s_rc_name[i], "%.16s%s", Fleet::display_name(*w),
                          just_answered ? "  " LV_SYMBOL_OK : "");
    char meta[64];
    size_t o = (size_t)snprintf(meta, sizeof(meta), "%s ago", age);
    if (w->battery_present && w->battery_pct >= 0 && o < sizeof(meta)) {
      o += (size_t)snprintf(meta + o, sizeof(meta) - o, " • %d%%",
                            (int)w->battery_pct);
    }
    if (w->rssi_present && o < sizeof(meta)) {
      snprintf(meta + o, sizeof(meta) - o, " • %s",
               signal_word((int)w->rssi_dbm));
    }
    lv_obj_set_style_text_color(s_rc_meta[i], mcol, 0);
    lv_label_set_text(s_rc_meta[i], meta);
  }
  lv_obj_set_style_text_color(s_rc_more, st.night ? ncol_muted() : col_faint(), 0);
  if (n > RC_ROWS) {
    lv_label_set_text_fmt(s_rc_more, "+%d more", n - RC_ROWS);
  } else {
    // Short enough for its low-latitude chord (134 px at -26; the engine
    // ellipsizes anything longer rather than letting the rim cut it).
    lv_label_set_text(s_rc_more, n > 0 ? "walk by - it lights up" : "");
  }
}
#endif  // FEATURE_CARE

// Transparency page: the mirror. Everything this glass consumes, speaks,
// and stores — and what it never does — with live numbers, on the device,
// for anyone in the house to read.
void update_about(const Fleet& fleet, uint32_t now, const GlanceState& st) {
  (void)now;
  const lv_color_t mcol = st.night ? ncol_muted() : col_muted();
  lv_obj_set_style_text_color(s_about_title, mcol, 0);
  lv_obj_set_style_text_color(s_about_body, mcol, 0);
  int journal_kept = 0;
#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
  journal_kept = canary::fleet::the_journal().count();
#endif
  lv_label_set_text_fmt(
      s_about_body,
      "watches %d %s\n"
      "hears: your home hub only\n"
      "speaks: check-ins • your taps\n"
      "keeps: %d events, on-device\n"
      "never: cloud • camera • mic\n"
      "v%s",
      fleet.count(), fleet.count() == 1 ? "canary" : "canaries", journal_kept,
      CANARY_FW_VERSION);
}

// Settings doorway: recolors with the night palette like every page.
void update_settings_page(const GlanceState& st) {
  const lv_color_t tcol = st.night ? ncol_text() : col_text();
  const lv_color_t mcol = st.night ? ncol_muted() : col_muted();
  lv_obj_set_style_text_color(s_set_glyph, mcol, 0);
  lv_obj_set_style_text_color(s_set_title, tcol, 0);
  lv_obj_set_style_text_color(s_set_hint, mcol, 0);
}

// Proof page: QR of the most urgent witness's signed chain head — the
// exact bytes it published, plus the pinned pubkey (spec §1). Dark-on-
// light on purpose: scanners want it, and scanning implies the user is
// awake and active.
void update_proof(const Fleet& fleet, uint32_t now, const GlanceState& st) {
  (void)st;
  const int n = fleet.count();
  const Witness* pick = nullptr;
  for (int i = 0; i < n; i++) {
    const Witness* w = fleet.at(i);
    if (w && fleet.witness_sev(*w, now) >= Sev::Alert) { pick = w; break; }
  }
  if (!pick && n > 0) pick = fleet.at(0);

  char pk[65];
  if (!pick || !pick->chain_raw[0] ||
      !canary::trust::pinned_pubkey_hex(pick->id, pk)) {
    lv_obj_add_flag(s_proof_card, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_proof_who, pick ? pick->id : "");
    // With the card hidden the equator is free — the explanation takes the
    // widest band instead of squeezing into the bottom-caption chord.
    rf_fit_center(s_proof_cap, 0);
    lv_label_set_text(s_proof_cap,
                      pick ? "no proof yet • after first event"
                           : "No witnesses yet");
    return;
  }

  static char body[640];
  const int len = snprintf(body, sizeof(body),
                           "{\"v\":1,\"t\":\"securacv/%s/chain\",\"pk\":\"%s\","
                           "\"p\":%s}",
                           pick->id, pk, pick->chain_raw);
  if (len <= 0 || (size_t)len >= sizeof(body)) {
    lv_obj_add_flag(s_proof_card, LV_OBJ_FLAG_HIDDEN);
    rf_fit_center(s_proof_cap, 0);
    lv_label_set_text(s_proof_cap, "too long for a QR • see hub log");
    return;
  }
  lv_obj_clear_flag(s_proof_card, LV_OBJ_FLAG_HIDDEN);
  lv_qrcode_update(s_proof_qr, body, (uint32_t)len);
  lv_label_set_text_fmt(s_proof_who, "%.18s", pick->id);
  rf_fit_bottom(s_proof_cap, -26);
  lv_label_set_text(s_proof_cap, "verify • no cloud");
}

void ack_cb(void* var, int32_t v) {
  (void)var;
  lv_arc_set_bg_angles(s_ack_ring, 0, (uint16_t)v);
}

void beat_cb(void* var, int32_t v) {
  lv_obj_set_style_arc_opa((lv_obj_t*)var, (lv_opa_t)v, LV_PART_MAIN);
}

// The heartbeat (spec §4): one soft swell, cryptographically earned — only
// fired by glance_ui_update when everything is reachable AND verified.
void heartbeat_pulse() {
  if (!s_beat_ring) return;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_beat_ring);
  lv_anim_set_exec_cb(&a, beat_cb);
  lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_30);
  lv_anim_set_time(&a, 800);
  lv_anim_set_playback_time(&a, 800);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────

int glance_page_count() { return page_map(the_fleet().count()).count; }

int glance_settings_page() { return page_map(the_fleet().count()).settings; }

void glance_ui_create() {
  // Re-entrant (ground-change rebuild): the caller cleaned the screen, so
  // every widget pointer is rebuilt below — but motion state that POINTS
  // at old widgets must not survive into the new face (use-after-free in
  // breathe()/ack otherwise).
  s_breathing = nullptr;
  s_ack_holding = false;
  s_scr = lv_scr_act();
  lv_obj_set_style_bg_color(s_scr, col_bg(), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

  // ── Halo page ──
  s_pg_halo = mk_page(s_scr);
  for (int i = 0; i < MAX_ARCS; i++) {
    s_arcs[i] = mk_ring(s_pg_halo, 232, 10);
    lv_obj_add_flag(s_arcs[i], LV_OBJ_FLAG_HIDDEN);
  }
  // The brand canary perches above the hero while the glass has no fleet
  // to speak for (listening / standalone clock). Hidden the moment the
  // first witness arrives — the bird yields to the job.
  lv_obj_t* bird = canary_mark_create(s_pg_halo, 40);
  lv_obj_align(bird, LV_ALIGN_TOP_MID, 0, 26);
  s_hero = mk_label(s_pg_halo, font_title(), col_text());
  lv_obj_align(s_hero, LV_ALIGN_CENTER, 0, -26);
  s_hero_sub = mk_label(s_pg_halo, font_label(), col_muted());
  rf_fit_center(s_hero_sub, 6);
  s_hero_badge = mk_label(s_pg_halo, font_caption(), col_muted());
  lv_obj_set_style_text_letter_space(s_hero_badge, 1, 0);
  rf_fit_center(s_hero_badge, 28);
  s_clock = mk_label(s_pg_halo, font_clock(), col_muted());
  lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, 62);
  // The honesty banner sits low on the disc; +84 buys it a 140 px chord
  // (+88 offered 128) and the fit keeps a Character's wider caption honest.
  s_banner = mk_label(s_pg_halo, font_caption(), col_alert());
  rf_fit_center(s_banner, 84);

  // ── Device page ──
  s_pg_dev = mk_page(s_scr);
  s_dev_ring = mk_ring(s_pg_dev, 232, 6);
  s_dev_name = mk_label(s_pg_dev, font_body(), col_text());
  rf_fit_center(s_dev_name, -62);
  s_dev_state = mk_label(s_pg_dev, font_title(), col_ok());
  lv_obj_align(s_dev_state, LV_ALIGN_CENTER, 0, -28);
  s_dev_event = mk_label(s_pg_dev, font_label(), col_muted());
  lv_obj_set_style_text_align(s_dev_event, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_dev_event, LV_ALIGN_CENTER, 0, 16);
  s_dev_meta = mk_label(s_pg_dev, font_caption(), col_muted());
  rf_fit_center(s_dev_meta, 56);
  s_dev_pos = mk_label(s_pg_dev, font_caption(), col_faint());
  rf_fit_center(s_dev_pos, 84);

  // ── Events page ── (rows on the Round Frame stack — see EV_ROWS above)
  s_pg_ev = mk_page(s_scr);
  s_ev_title = mk_label(s_pg_ev, font_caption(), col_muted());
  lv_obj_set_style_text_letter_space(s_ev_title, 2, 0);
  lv_label_set_text(s_ev_title, "RECENT");
  rf_fit_top(s_ev_title, 32);
  for (int i = 0; i < EV_ROWS; i++) {
    s_ev_name[i] = mk_label(s_pg_ev, font_label(), col_text());
    rf_fit_top(s_ev_name[i], list_row_y(i));
    s_ev_meta[i] = mk_label(s_pg_ev, font_caption(), col_muted());
    rf_fit_top(s_ev_meta[i], list_row_y(i) + 16);
  }

#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
  // ── History page (spec §7) ──
  s_pg_history = mk_page(s_scr);
  s_thist_title = mk_label(s_pg_history, font_caption(), col_muted());
  lv_obj_set_style_text_letter_space(s_thist_title, 2, 0);
  lv_label_set_text(s_thist_title, "HISTORY");
  rf_fit_top(s_thist_title, 28);
  s_thist_summary = mk_label(s_pg_history, font_caption(), col_muted());
  rf_fit_top(s_thist_summary, 44);
  for (int i = 0; i < EV_ROWS; i++) {
    // The title+summary pair above needs the deeper bias (+10) the events
    // page doesn't.
    s_thist_name[i] = mk_label(s_pg_history, font_label(), col_text());
    rf_fit_top(s_thist_name[i], list_row_y(i, 10));
    s_thist_meta[i] = mk_label(s_pg_history, font_caption(), col_muted());
    rf_fit_top(s_thist_meta[i], list_row_y(i, 10) + 16);
  }
  lv_obj_add_flag(s_pg_history, LV_OBJ_FLAG_HIDDEN);
#endif

#if defined(FEATURE_CARE) && FEATURE_CARE
  // ── Roll Call page (care wave §6) ──
  s_pg_rc = mk_page(s_scr);
  s_rc_title = mk_label(s_pg_rc, font_caption(), col_muted());
  lv_obj_set_style_text_letter_space(s_rc_title, 2, 0);
  lv_label_set_text(s_rc_title, "ROLL CALL");
  rf_fit_top(s_rc_title, 32);
  for (int i = 0; i < RC_ROWS; i++) {
    s_rc_name[i] = mk_label(s_pg_rc, font_label(), col_text());
    rf_fit_top(s_rc_name[i], list_row_y(i));
    s_rc_meta[i] = mk_label(s_pg_rc, font_caption(), col_muted());
    rf_fit_top(s_rc_meta[i], list_row_y(i) + 16);
  }
  s_rc_more = mk_label(s_pg_rc, font_caption(), col_faint());
  rf_fit_bottom(s_rc_more, -26);
  lv_obj_add_flag(s_pg_rc, LV_OBJ_FLAG_HIDDEN);
#endif

  // ── Transparency page (care wave §7) ──
  s_pg_about = mk_page(s_scr);
  s_about_title = mk_label(s_pg_about, font_caption(), col_muted());
  lv_obj_set_style_text_letter_space(s_about_title, 2, 0);
  lv_label_set_text(s_about_title, "THIS GLASS");
  rf_fit_top(s_about_title, 36);
  s_about_body = mk_label(s_pg_about, font_caption(), col_muted());
  lv_obj_set_style_text_align(s_about_body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(s_about_body, 8, 0);
  lv_obj_align(s_about_body, LV_ALIGN_CENTER, 0, 8);
  lv_obj_add_flag(s_pg_about, LV_OBJ_FLAG_HIDDEN);

  // ── Proof page ──
  s_pg_proof = mk_page(s_scr);
  s_proof_who = mk_label(s_pg_proof, font_caption(), col_muted());
  rf_fit_top(s_proof_who, 26);
  s_proof_card = lv_obj_create(s_pg_proof);
  lv_obj_set_size(s_proof_card, 156, 156);
  lv_obj_align(s_proof_card, LV_ALIGN_CENTER, 0, 2);
  lv_obj_set_style_bg_color(s_proof_card, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(s_proof_card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_proof_card, 10, 0);
  lv_obj_set_style_border_width(s_proof_card, 0, 0);
  lv_obj_set_style_pad_all(s_proof_card, 8, 0);
  lv_obj_clear_flag(s_proof_card, LV_OBJ_FLAG_SCROLLABLE);
  s_proof_qr = mk_qrcode(s_proof_card, 140);
  lv_obj_center(s_proof_qr);
  s_proof_cap = mk_label(s_pg_proof, font_caption(), col_muted());
  rf_fit_bottom(s_proof_cap, -26);

  // ── Settings doorway (settings wave) ──
  s_pg_settings = mk_page(s_scr);
  s_set_glyph = mk_label(s_pg_settings, font_hero(), col_muted());
  lv_label_set_text(s_set_glyph, LV_SYMBOL_SETTINGS);
  lv_obj_align(s_set_glyph, LV_ALIGN_CENTER, 0, -30);
  s_set_title = mk_label(s_pg_settings, font_body(), col_text());
  lv_label_set_text(s_set_title, "screen settings");
  lv_obj_align(s_set_title, LV_ALIGN_CENTER, 0, 18);
  s_set_hint = mk_label(s_pg_settings, font_caption(), col_muted());
  lv_label_set_text(s_set_hint, "hold to open");
  lv_obj_align(s_set_hint, LV_ALIGN_CENTER, 0, 44);
  lv_obj_add_flag(s_pg_settings, LV_OBJ_FLAG_HIDDEN);

  // ── Heartbeat ring (starts invisible; pulses only when earned) ──
  s_beat_ring = mk_ring(s_scr, 238, 2);
  lv_obj_set_style_arc_color(s_beat_ring, col_ok(), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(s_beat_ring, LV_OPA_TRANSP, LV_PART_MAIN);

  // ── Ack hold ring (overlay) ──
  s_ack_ring = mk_ring(s_scr, 220, 4);
  lv_obj_set_style_arc_color(s_ack_ring, col_text(), LV_PART_MAIN);
  lv_obj_add_flag(s_ack_ring, LV_OBJ_FLAG_HIDDEN);

  lv_obj_add_flag(s_pg_dev, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_pg_ev, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_pg_proof, LV_OBJ_FLAG_HIDDEN);
  s_shown_page = 0;
}

void glance_ui_update(const Fleet& fleet, uint32_t now, const GlanceState& st) {
  if (!s_scr) return;
  const int devices = fleet.count();
  const PageMap m = page_map(devices);
  int page = st.page;
  if (page >= m.count || page < 0) page = 0;

  if (page != s_shown_page) {
    s_shown_page = page;
    if (page == 0)             show_page(s_pg_halo);
    else if (page <= devices)  show_page(s_pg_dev);
    else if (page == m.events) show_page(s_pg_ev);
#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
    else if (page == m.history) show_page(s_pg_history);
#endif
#if defined(FEATURE_CARE) && FEATURE_CARE
    else if (page == m.rollcall) show_page(s_pg_rc);
#endif
    else if (page == m.proof)  show_page(s_pg_proof);
    else if (page == m.about)  show_page(s_pg_about);
    else                       show_page(s_pg_settings);
  }

  if (page == 0)             update_halo(fleet, now, st);
  else if (page <= devices)  update_device(fleet, now, st, page - 1);
  else if (page == m.events) update_events(fleet, now, st);
#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE
  else if (page == m.history) update_history(fleet, now, st);
#endif
#if defined(FEATURE_CARE) && FEATURE_CARE
  else if (page == m.rollcall) update_rollcall(fleet, now, st);
#endif
  else if (page == m.proof)  update_proof(fleet, now, st);
  else if (page == m.about)  update_about(fleet, now, st);
  else                       update_settings_page(st);

  // The heartbeat: earned, daytime, once a minute (spec §4). Absence is
  // information — any lesser state and the ring stays dark.
  static uint32_t s_last_beat_ms = 0;
  if (!st.night && page == 0 && fleet.count() > 0 &&
      fleet.worst(now) <= Sev::Notice && fleet.all_verified() &&
      st.wifi_ok && st.mqtt_ok &&
      (int32_t)(now - s_last_beat_ms) >= (int32_t)CD_HEARTBEAT_UI_MS) {
    s_last_beat_ms = now;
    heartbeat_pulse();
  }
}

void glance_ui_ack_hold(bool active) {
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

#endif  // CD_FLAVOR_WATCH
