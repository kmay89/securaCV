// Canary Cards — firmware card builder (see fleet_cards.h). Pure C++, host
// tested in tests_host/test_fleet_cards.cpp against the JS reference
// (canary-local/assets/canary-cards.js senseCards) so the two surfaces render
// the same entities, in the same order, with the same privacy classes.

#include "fleet_cards.h"

#include <cstdio>
#include <cstring>

namespace canary::fleet {

// Static option tables for band cards (pointed at by Card::band_options).
static const char* const OCCUPANT_OPTS[] = {"0", "1", "2+"};
static const char* const RANGE_OPTS[]    = {"near", "mid", "far"};

const char* card_kind_name(CardKind k) {
  switch (k) {
    case CardKind::Binary:    return "binary";
    case CardKind::Stat:      return "stat";
    case CardKind::Band:      return "band";
    case CardKind::Sparkline: return "sparkline";
    case CardKind::Event:     return "event";
    case CardKind::Trust:     return "trust";
  }
  return "binary";
}

const char* card_privacy_name(CardPrivacy p) {
  switch (p) {
    case CardPrivacy::P0: return "P0";
    case CardPrivacy::P1: return "P1";
    case CardPrivacy::P2: return "P2";
    case CardPrivacy::None: default: return "";
  }
}

static const char* badge_word(Badge b) {
  switch (b) {
    case Badge::Verified: return "verified";
    case Badge::Signed:   return "signed";
    case Badge::Unsigned: return "unsigned";
    case Badge::Failed:   return "FAILED";
    case Badge::Unknown: default: return "...";
  }
}

// A canary-sense witness: has published a radar row, or claims the type
// (hyphen or underscore, matching both wire spellings the fleet uses).
static bool is_sense_witness(const Witness& w) {
  return w.sense_present ||
         strcmp(w.device_type, "canary-sense") == 0 ||
         strcmp(w.device_type, "canary_sense") == 0;
}

// A canary-pool witness: has published a chemistry row, or claims the type.
static bool is_pool_witness(const Witness& w) {
  return w.pool_present ||
         strcmp(w.device_type, "canary-pool") == 0 ||
         strcmp(w.device_type, "canary_pool") == 0;
}

bool has_cards(const Witness& w) {
  return is_sense_witness(w) || is_pool_witness(w);
}

// ── the canary-sense card set (mirrors JS senseCards ordering) ─────────────
//
// One entity, one card. `absent` marks entities compiled out of a build (the
// presence-only BPM story); `*_known`/band_sel == -1 marks a value the device
// has not published yet, rendered "-" not zero.
static void build_sense_cards(const Witness& w, CardSet& out) {
  const bool have_state = w.sense_present;
  // Reset each slot on push so a reused CardSet can't leak stale fields from a
  // prior witness (callers pass a fresh stack instance today; belt-and-braces).
  auto push = [&out]() -> Card& {
    Card& c = out.cards[out.count++];
    c = Card{};
    return c;
  };

  // 1. Presence (binary, P0)
  {
    Card& c = push();
    c.kind = CardKind::Binary; c.id = "presence"; c.title = "Presence";
    c.icon = "radar"; c.privacy = CardPrivacy::P0;
    c.on_label = "present"; c.off_label = "clear";
    c.b_known = have_state && w.radar_presence != 0;   // 0 == unknown
    c.b_state = (w.radar_presence == 2);
    c.sev = c.b_state ? CardSev::Ok : CardSev::None;
  }
  // 2. Occupants (band 0/1/2+, P0)
  {
    Card& c = push();
    c.kind = CardKind::Band; c.id = "occupants"; c.title = "Occupants";
    c.icon = "people"; c.privacy = CardPrivacy::P0;
    c.band_options = OCCUPANT_OPTS; c.band_count = 3;
    c.band_sel = (have_state && w.radar_presence != 0) ? (int8_t)w.radar_occupants : -1;
  }
  // 3. Range band (band near/mid/far, P2 — raw cm never leaves the device)
  {
    Card& c = push();
    c.kind = CardKind::Band; c.id = "range_band"; c.title = "Range band";
    c.icon = "ruler"; c.privacy = CardPrivacy::P2;
    c.band_options = RANGE_OPTS; c.band_count = 3;
    c.band_sel = (w.radar_range == 0) ? -1 : (int8_t)(w.radar_range - 1);
  }
  // 4. Radar link (binary problem, P0)
  {
    Card& c = push();
    c.kind = CardKind::Binary; c.id = "radar_link"; c.title = "Radar link";
    c.icon = "link"; c.privacy = CardPrivacy::P0;
    c.on_label = "ok"; c.off_label = "stalled";
    c.b_known = have_state; c.b_state = w.radar_ok;
    c.sev = !have_state ? CardSev::None : (w.radar_ok ? CardSev::Ok : CardSev::Alert);
  }
  // 5. Frame errors (stat, P0)
  {
    Card& c = push();
    c.kind = CardKind::Stat; c.id = "frame_errors"; c.title = "Frame errors";
    c.icon = "pulse"; c.privacy = CardPrivacy::P0; c.unit = "";
    c.num_known = have_state; c.num = w.frame_errors;
    c.sev = (have_state && w.frame_errors > 0) ? CardSev::Notice : CardSev::None;
  }
  // 6. Illuminance (stat lx, P0)
  {
    Card& c = push();
    c.kind = CardKind::Stat; c.id = "illuminance"; c.title = "Illuminance";
    c.icon = "sun"; c.privacy = CardPrivacy::P0; c.unit = "lx";
    c.num_known = (w.lux >= 0); c.num = w.lux;
    // lights-out while present is the tamper-corroboration shape.
    c.sev = (w.lux >= 0 && w.lux < 5 && w.radar_presence == 2) ? CardSev::Warn
                                                               : CardSev::None;
  }
  // 7. Breathing confirmed (binary P0) — absent unless the build offers vitals
  {
    Card& c = push();
    c.kind = CardKind::Binary; c.id = "breathing"; c.title = "Breathing";
    c.icon = "lungs"; c.privacy = CardPrivacy::P0;
    c.on_label = "locked"; c.off_label = "-";
    c.absent = !w.wb_present;
    c.b_known = w.wb_present; c.b_state = w.wb_breathing;
    c.sev = (w.wb_present && w.wb_breathing) ? CardSev::Ok : CardSev::None;
  }
  // 8. Breath rate (sparkline bpm, P1) — absent unless P1 BPM entities exist
  {
    Card& c = push();
    c.kind = CardKind::Sparkline; c.id = "breath_rate"; c.title = "Breath rate";
    c.icon = "lungs"; c.privacy = CardPrivacy::P1; c.unit = "bpm";
    c.absent = !w.bpm_offered;
    c.num_known = w.bpm_offered && w.bpm_valid; c.num = w.breath_bpm;
  }
  // 9. Heart rate (sparkline bpm, P1) — same P1 gate
  {
    Card& c = push();
    c.kind = CardKind::Sparkline; c.id = "heart_rate"; c.title = "Heart rate";
    c.icon = "heart"; c.privacy = CardPrivacy::P1; c.unit = "bpm";
    c.absent = !w.bpm_offered;
    c.num_known = w.bpm_offered && w.bpm_valid; c.num = w.heart_bpm;
  }
  // 10. Last event (event, P0)
  {
    Card& c = push();
    c.kind = CardKind::Event; c.id = "last_event"; c.title = "Last event";
    c.icon = "clock"; c.privacy = CardPrivacy::P0;
    c.ev = w.has_event ? w.last_event : nullptr;
    c.signed_known = w.has_event;
    // The model doesn't retain a per-event signed flag on the Witness; the
    // chain badge is the trust surface, so leave signed_flag false here and
    // let the trust card carry verification (parity with the UI chrome).
  }
  // 11. Witness chain (trust, P0)
  {
    Card& c = push();
    c.kind = CardKind::Trust; c.id = "chain"; c.title = "Witness chain";
    c.icon = "shield"; c.privacy = CardPrivacy::P0;
    c.chain = w.chain_length; c.badge = w.badge;
  }
}

// ── the canary-pool card set (water chemistry) ─────────────────────────────
//
// One entity, one card, same rule as sense — pH / ORP / water temp / TDS, the
// four keys a pool node publishes (docs/research/pool_water_monitor.md §6). The
// severity accents are a glance heuristic only: the authoritative alerting is
// event-driven (the node's threshold/hysteresis machine emits named events that
// classify_event maps). Bands come straight from the research doc's targets so
// nothing here invents a number: pH 7.2–7.6 (warn outside 7.0–7.8), sanitizer
// healthy at ORP ≥ 650 mV. A field the node didn't publish this row renders "-"
// (have_* false), never a zero — pH 0 or 0 mV would read as an emergency.
static const int16_t POOL_PH_LO  = 70;   // pH ×10 — below this, warn
static const int16_t POOL_PH_HI  = 78;   // pH ×10 — above this, warn
static const int16_t POOL_ORP_LO = 650;  // mV — below this, sanitizer low

static void build_pool_cards(const Witness& w, CardSet& out) {
  auto push = [&out]() -> Card& {
    Card& c = out.cards[out.count++];
    c = Card{};
    return c;
  };

  // 1. pH (stat, one decimal, P0)
  {
    Card& c = push();
    c.kind = CardKind::Stat; c.id = "ph"; c.title = "pH";
    c.icon = "pulse"; c.privacy = CardPrivacy::P0; c.unit = "";
    c.num_decimals = 1;
    c.num_known = w.have_ph; c.num = w.ph_x10;
    c.sev = (w.have_ph && (w.ph_x10 < POOL_PH_LO || w.ph_x10 > POOL_PH_HI))
                ? CardSev::Warn : CardSev::None;
  }
  // 2. ORP / sanitizer proxy (stat mV, P0)
  {
    Card& c = push();
    c.kind = CardKind::Stat; c.id = "orp"; c.title = "ORP";
    c.icon = "pulse"; c.privacy = CardPrivacy::P0; c.unit = "mV";
    c.num_known = w.have_orp; c.num = w.orp_mv;
    c.sev = (w.have_orp && w.orp_mv < POOL_ORP_LO) ? CardSev::Warn
                                                   : CardSev::None;
  }
  // 3. Water temperature (stat °C, one decimal, P0). ASCII "C" — LVGL symbol
  //    substitution is the caller's job (see format_card_value contract).
  {
    Card& c = push();
    c.kind = CardKind::Stat; c.id = "water_temp"; c.title = "Water temp";
    c.icon = "sun"; c.privacy = CardPrivacy::P0; c.unit = "C";
    c.num_decimals = 1;
    c.num_known = w.have_water_temp; c.num = w.water_temp_c10;
  }
  // 4. TDS / salinity (stat ppm, P0). Informational: a healthy value depends on
  //    pool type (salt vs fresh), so the glance carries the number, not a verdict.
  {
    Card& c = push();
    c.kind = CardKind::Stat; c.id = "tds"; c.title = "TDS";
    c.icon = "chip"; c.privacy = CardPrivacy::P0; c.unit = "ppm";
    c.num_known = w.have_tds; c.num = w.tds_ppm;
  }
  // 5. Last event (event, P0) — same trust chrome the sense set trails with.
  {
    Card& c = push();
    c.kind = CardKind::Event; c.id = "last_event"; c.title = "Last event";
    c.icon = "clock"; c.privacy = CardPrivacy::P0;
    c.ev = w.has_event ? w.last_event : nullptr;
    c.signed_known = w.has_event;
  }
  // 6. Witness chain (trust, P0)
  {
    Card& c = push();
    c.kind = CardKind::Trust; c.id = "chain"; c.title = "Witness chain";
    c.icon = "shield"; c.privacy = CardPrivacy::P0;
    c.chain = w.chain_length; c.badge = w.badge;
  }
}

void build_cards(const Witness& w, uint32_t now, const FleetLimits& limits,
                 CardSet& out) {
  (void)now;
  (void)limits;
  out.count = 0;
  // Dispatch on device type. Pool first so a witness that somehow claimed both
  // surfaces still gets a deterministic set; in practice a device is one type.
  if (is_pool_witness(w)) {
    build_pool_cards(w, out);
    return;
  }
  if (is_sense_witness(w)) {
    build_sense_cards(w, out);
    return;
  }
  // Other device types fall back to the generic field list (count 0). The
  // builder dispatches on device_type; adding a vision/wap card set is one
  // more build_*_cards() here, no UI change.
}

// ── value formatting: the per-kind projection (where the UI learns the kinds)
size_t format_card_value(const Card& c, char* buf, size_t cap) {
  if (!buf || cap == 0) return 0;
  buf[0] = '\0';
  if (c.absent) { snprintf(buf, cap, "not in build"); return strlen(buf); }

  switch (c.kind) {
    case CardKind::Binary:
      if (!c.b_known) snprintf(buf, cap, "-");
      else snprintf(buf, cap, "%s", c.b_state ? c.on_label : c.off_label);
      break;
    case CardKind::Stat:
    case CardKind::Sparkline:
      if (!c.num_known) {
        snprintf(buf, cap, "-");
      } else if (c.num_decimals > 0) {
        // Fixed-point: num is scaled by 10^decimals (pH 74 -> "7.4"). Split so
        // a negative value keeps its sign on the whole part and a magnitude
        // fraction ("-1.5", not "-1.-5"). Decimals are clamped to a small max
        // (no card needs more, and it bounds the field width for the buffer).
        int dec = c.num_decimals > 6 ? 6 : (int)c.num_decimals;
        long pow = 1;
        for (int i = 0; i < dec; i++) pow *= 10;
        const long whole = c.num / pow;
        long frac = c.num % pow;
        if (frac < 0) frac = -frac;
        const bool neg_zero_whole = (c.num < 0 && whole == 0);
        char num[48];
        snprintf(num, sizeof(num), "%s%ld.%0*ld", neg_zero_whole ? "-" : "",
                 whole, dec, frac);
        if (c.unit && c.unit[0]) snprintf(buf, cap, "%s %s", num, c.unit);
        else snprintf(buf, cap, "%s", num);
      } else if (c.unit && c.unit[0]) {
        snprintf(buf, cap, "%ld %s", c.num, c.unit);
      } else {
        snprintf(buf, cap, "%ld", c.num);
      }
      break;
    case CardKind::Band:
      if (c.band_sel < 0 || c.band_sel >= (int)c.band_count) snprintf(buf, cap, "-");
      else snprintf(buf, cap, "%s", c.band_options[c.band_sel]);
      break;
    case CardKind::Event:
      snprintf(buf, cap, "%s", c.ev ? c.ev : "-");
      break;
    case CardKind::Trust:
      snprintf(buf, cap, "%s %lu", badge_word(c.badge), (unsigned long)c.chain);
      break;
  }
  return strlen(buf);
}

int format_card_strip(const CardSet& set, bool skip_shown, char* buf,
                      size_t cap) {
  if (!buf || cap == 0) return 0;
  buf[0] = '\0';
  size_t off = 0;
  int shown = 0;
  for (int i = 0; i < set.count; i++) {
    const Card& c = set.cards[i];
    if (c.absent) continue;                       // provably-absent: not on the strip
    if (skip_shown && (c.kind == CardKind::Event || c.kind == CardKind::Trust))
      continue;                                   // chrome already shows these
    // Skip cards whose value is unknown so the strip stays glanceable — a "-"
    // for every unpublished field is noise on a wall dash.
    char val[32];
    format_card_value(c, val, sizeof(val));
    if (strcmp(val, "-") == 0) continue;

    const char* sep = (shown == 0) ? "" : "  \xE2\x80\xA2  ";
    int n = snprintf(buf + off, cap - off, "%s%s %s", sep, c.title, val);
    if (n < 0 || (size_t)n >= cap - off) break;   // out of room — stop cleanly
    off += (size_t)n;
    shown++;
  }
  return shown;
}

}  // namespace canary::fleet
