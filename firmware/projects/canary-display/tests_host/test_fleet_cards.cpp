// Host-side test for the Canary Cards firmware builder (fleet_cards.h/.cpp) —
// the type-aware widget layer the glass renders for a canary-sense witness.
// Pins the model to the schema contract (docs/standard/CANARY_CARDS.md) and to
// parity with the JS reference renderer (canary-local/assets/canary-cards.js
// senseCards): same 11 cards, same order, same ids, same kinds, same privacy
// classes, and the same null-vs-zero / absent honesty. Pure logic, no LVGL.

#include "canary/fleet/fleet_cards.h"
#include "canary/fleet/fleet_model.h"

#include <cstdio>
#include <cstring>

using namespace canary::fleet;

static int g_failures = 0;
#define CHECK(cond, msg)                                          \
  do {                                                            \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; }  \
    else         { printf("ok:   %s\n", (msg)); }                \
  } while (0)

// A canary-sense witness with a wellbeing (P1) build's full state row.
static Witness mk_sense(uint8_t presence, uint8_t occ, uint8_t range,
                        bool radar_ok, bool wb, bool breathing,
                        bool bpm_offered, bool bpm_valid,
                        uint8_t br, uint8_t hr, int lux) {
  Witness w;
  w.used = true;
  strcpy(w.id, "canary_sense_001");
  strcpy(w.device_type, "canary-sense");
  w.sense_present = true;
  w.radar_presence = presence;
  w.radar_occupants = occ;
  w.radar_range = range;
  w.radar_ok = radar_ok;
  w.wb_present = wb;
  w.wb_breathing = breathing;
  w.bpm_offered = bpm_offered;
  w.bpm_valid = bpm_valid;
  w.breath_bpm = bpm_valid ? br : 0;
  w.heart_bpm = bpm_valid ? hr : 0;
  w.lux = (int16_t)lux;
  w.badge = Badge::Verified;
  w.chain_length = 42;
  return w;
}

// A canary-pool witness with a chemistry state row (any field can be absent).
static Witness mk_pool(bool have_ph, int16_t ph_x10, bool have_orp, int16_t orp,
                       bool have_wt, int16_t wt_c10, bool have_tds,
                       int16_t tds) {
  Witness w;
  w.used = true;
  strcpy(w.id, "canary_pool_001");
  strcpy(w.device_type, "canary-pool");
  w.pool_present = true;
  w.have_ph = have_ph; w.ph_x10 = ph_x10;
  w.have_orp = have_orp; w.orp_mv = orp;
  w.have_water_temp = have_wt; w.water_temp_c10 = wt_c10;
  w.have_tds = have_tds; w.tds_ppm = tds;
  w.badge = Badge::Verified;
  w.chain_length = 7;
  return w;
}

static const Card* find(const CardSet& s, const char* id) {
  for (int i = 0; i < s.count; i++)
    if (strcmp(s.cards[i].id, id) == 0) return &s.cards[i];
  return nullptr;
}

int main() {
  FleetLimits limits;

  // ── has_cards: sense witnesses only ────────────────────────────────────
  {
    Witness sense = mk_sense(2, 1, 1, true, true, true, true, true, 14, 68, 140);
    CHECK(has_cards(sense), "canary-sense witness has a card set");
    Witness cam;
    cam.used = true; strcpy(cam.device_type, "canary-vision");
    CHECK(!has_cards(cam), "a non-sense witness has no card set (generic fallback)");
    Witness by_type;
    by_type.used = true; strcpy(by_type.device_type, "canary-sense");
    CHECK(has_cards(by_type), "device_type alone marks a card-bearing witness");
  }

  // ── the full wellbeing card set: parity with JS senseCards ─────────────
  {
    Witness w = mk_sense(2, 1, 1, true, true, true, true, true, 14, 68, 140);
    CardSet s;
    build_cards(w, 1000, limits, s);

    CHECK(s.count == 11, "wellbeing+P1 build yields all 11 cards");

    // exact id + kind + privacy order (mirrors canary-cards.js senseCards)
    struct Spec { const char* id; CardKind kind; CardPrivacy priv; };
    const Spec want[] = {
      {"presence",     CardKind::Binary,    CardPrivacy::P0},
      {"occupants",    CardKind::Band,      CardPrivacy::P0},
      {"range_band",   CardKind::Band,      CardPrivacy::P2},
      {"radar_link",   CardKind::Binary,    CardPrivacy::P0},
      {"frame_errors", CardKind::Stat,      CardPrivacy::P0},
      {"illuminance",  CardKind::Stat,      CardPrivacy::P0},
      {"breathing",    CardKind::Binary,    CardPrivacy::P0},
      {"breath_rate",  CardKind::Sparkline, CardPrivacy::P1},
      {"heart_rate",   CardKind::Sparkline, CardPrivacy::P1},
      {"last_event",   CardKind::Event,     CardPrivacy::P0},
      {"chain",        CardKind::Trust,     CardPrivacy::P0},
    };
    bool order_ok = (s.count == 11);
    for (int i = 0; i < s.count && i < 11; i++) {
      if (strcmp(s.cards[i].id, want[i].id) != 0 ||
          s.cards[i].kind != want[i].kind ||
          s.cards[i].privacy != want[i].priv) {
        order_ok = false;
        printf("  card %d: got id=%s kind=%s priv=%s\n", i, s.cards[i].id,
               card_kind_name(s.cards[i].kind),
               card_privacy_name(s.cards[i].privacy));
      }
    }
    CHECK(order_ok, "cards match JS senseCards id/kind/privacy in order");

    // value projection per kind
    const Card* pres = find(s, "presence");
    CHECK(pres && pres->b_known && pres->b_state && pres->sev == CardSev::Ok,
          "presence: present + ok severity");
    const Card* occ = find(s, "occupants");
    CHECK(occ && occ->band_sel == 1, "occupants: band lit at '1'");
    const Card* rng = find(s, "range_band");
    CHECK(rng && rng->band_sel == 0, "range_band: band lit at 'near'");
    const Card* link = find(s, "radar_link");
    CHECK(link && link->b_known && link->b_state && link->sev == CardSev::Ok,
          "radar_link: ok");
    const Card* br = find(s, "breath_rate");
    CHECK(br && !br->absent && br->num_known && br->num == 14, "breath_rate: 14 bpm, P1 present");
    const Card* hr = find(s, "heart_rate");
    CHECK(hr && hr->num_known && hr->num == 68, "heart_rate: 68 bpm");
    const Card* chain = find(s, "chain");
    CHECK(chain && chain->chain == 42 && chain->badge == Badge::Verified,
          "chain: length 42, verified");
  }

  // ── presence-only build: BPM cards render provably ABSENT, not missing ──
  {
    Witness w = mk_sense(2, 1, 1, true, /*wb=*/false, false,
                         /*bpm_offered=*/false, false, 0, 0, 10);
    CardSet s;
    build_cards(w, 1000, limits, s);
    CHECK(s.count == 11, "presence-only build still has all 11 card slots");
    const Card* breathing = find(s, "breathing");
    CHECK(breathing && breathing->absent, "breathing card is absent (no vitals build)");
    const Card* br = find(s, "breath_rate");
    const Card* hr = find(s, "heart_rate");
    CHECK(br && br->absent && hr && hr->absent, "BPM cards absent, never silently dropped");
    const Card* pres = find(s, "presence");
    CHECK(pres && !pres->absent, "presence card present in every build");
  }

  // ── null-vs-zero honesty: an unknown radar renders "-", not "0" ─────────
  {
    Witness w = mk_sense(0 /*unknown*/, 0, 0 /*unknown*/, false,
                         false, false, false, false, 0, 0, -1);
    CardSet s;
    build_cards(w, 1000, limits, s);
    const Card* pres = find(s, "presence");
    char v[32];
    format_card_value(*pres, v, sizeof(v));
    CHECK(strcmp(v, "-") == 0, "unknown presence formats as a dash (not clear/present)");
    const Card* rng = find(s, "range_band");
    format_card_value(*rng, v, sizeof(v));
    CHECK(strcmp(v, "-") == 0, "unknown range formats as a dash");
    const Card* lux = find(s, "illuminance");
    format_card_value(*lux, v, sizeof(v));
    CHECK(strcmp(v, "-") == 0, "unpublished lux formats as a dash (not 0 lx)");
  }

  // ── per-kind value formatting ──────────────────────────────────────────
  {
    Witness w = mk_sense(1 /*clear*/, 0, 2 /*mid*/, true, true, false,
                         true, false /*bpm not valid*/, 0, 0, 200);
    CardSet s;
    build_cards(w, 1000, limits, s);
    char v[32];
    format_card_value(*find(s, "presence"), v, sizeof(v));
    CHECK(strcmp(v, "clear") == 0, "binary formats off_label 'clear'");
    format_card_value(*find(s, "range_band"), v, sizeof(v));
    CHECK(strcmp(v, "mid") == 0, "band formats the lit slot 'mid'");
    format_card_value(*find(s, "illuminance"), v, sizeof(v));
    CHECK(strcmp(v, "200 lx") == 0, "stat formats value + unit '200 lx'");
    format_card_value(*find(s, "breath_rate"), v, sizeof(v));
    CHECK(strcmp(v, "-") == 0, "P1 sparkline with invalid bpm formats a dash even when offered");
    format_card_value(*find(s, "chain"), v, sizeof(v));
    CHECK(strcmp(v, "verified 42") == 0, "trust formats badge + chain length");
  }

  // ── the compact strip: skips absent + unknown + (optionally) chrome ─────
  {
    Witness w = mk_sense(2, 1, 1, true, true, true, true, true, 14, 68, 140);
    CardSet s;
    build_cards(w, 1000, limits, s);
    char strip[256];
    int shown = format_card_strip(s, /*skip_shown=*/true, strip, sizeof(strip));
    CHECK(shown >= 5, "strip shows the coarse claims");
    CHECK(strstr(strip, "Presence present") != nullptr, "strip carries presence");
    CHECK(strstr(strip, "Occupants 1") != nullptr, "strip carries occupants");
    CHECK(strstr(strip, "Range band near") != nullptr, "strip carries range");
    CHECK(strstr(strip, "Heart rate 68 bpm") != nullptr, "strip carries P1 heart rate");
    CHECK(strstr(strip, "Witness chain") == nullptr, "skip_shown drops the trust card");
    CHECK(strstr(strip, "Last event") == nullptr, "skip_shown drops the event card");

    // presence-only witness: strip must not mention BPM at all (absent)
    Witness po = mk_sense(2, 1, 1, true, false, false, false, false, 0, 0, 10);
    CardSet ps;
    build_cards(po, 1000, limits, ps);
    format_card_strip(ps, true, strip, sizeof(strip));
    CHECK(strstr(strip, "bpm") == nullptr, "presence-only strip carries no BPM");
    CHECK(strstr(strip, "not in build") == nullptr, "absent cards never reach the strip");
  }

  // ── canary-pool: has_cards by state row and by device_type ─────────────
  {
    Witness pool = mk_pool(true, 74, true, 712, true, 285, true, 3200);
    CHECK(has_cards(pool), "canary-pool witness has a card set");
    Witness by_type;
    by_type.used = true; strcpy(by_type.device_type, "canary-pool");
    CHECK(has_cards(by_type), "canary-pool device_type alone marks card-bearing");
    // A pool witness must NOT be routed to the sense builder (dispatch order).
    CardSet s;
    build_cards(by_type, 1000, limits, s);
    CHECK(find(s, "ph") != nullptr && find(s, "presence") == nullptr,
          "pool witness builds pool cards, not sense cards");
  }

  // ── the pool card set: ids/kinds/privacy, in order ─────────────────────
  {
    Witness w = mk_pool(true, 74, true, 712, true, 285, true, 3200);
    CardSet s;
    build_cards(w, 1000, limits, s);
    CHECK(s.count == 6, "pool build yields 6 cards");
    struct Spec { const char* id; CardKind kind; CardPrivacy priv; };
    const Spec want[] = {
      {"ph",         CardKind::Stat,  CardPrivacy::P0},
      {"orp",        CardKind::Stat,  CardPrivacy::P0},
      {"water_temp", CardKind::Stat,  CardPrivacy::P0},
      {"tds",        CardKind::Stat,  CardPrivacy::P0},
      {"last_event", CardKind::Event, CardPrivacy::P0},
      {"chain",      CardKind::Trust, CardPrivacy::P0},
    };
    bool order_ok = (s.count == 6);
    for (int i = 0; i < s.count && i < 6; i++)
      if (strcmp(s.cards[i].id, want[i].id) != 0 ||
          s.cards[i].kind != want[i].kind ||
          s.cards[i].privacy != want[i].priv)
        order_ok = false;
    CHECK(order_ok, "pool cards match the documented id/kind/privacy order");
  }

  // ── fixed-point formatting: pH + water temp render one decimal ─────────
  {
    Witness w = mk_pool(true, 74, true, 712, true, 285, true, 3200);
    CardSet s;
    build_cards(w, 1000, limits, s);
    char v[32];
    format_card_value(*find(s, "ph"), v, sizeof(v));
    CHECK(strcmp(v, "7.4") == 0, "pH formats as 7.4 (one decimal, no unit)");
    format_card_value(*find(s, "orp"), v, sizeof(v));
    CHECK(strcmp(v, "712 mV") == 0, "ORP formats as integer + unit");
    format_card_value(*find(s, "water_temp"), v, sizeof(v));
    CHECK(strcmp(v, "28.5 C") == 0, "water temp formats as 28.5 C");
    format_card_value(*find(s, "tds"), v, sizeof(v));
    CHECK(strcmp(v, "3200 ppm") == 0, "TDS formats as integer ppm");
  }

  // ── null-vs-zero honesty: an unpublished field is "-", not 0.0 ───────────
  {
    Witness w = mk_pool(false, 0, false, 0, false, 0, false, 0);
    CardSet s;
    build_cards(w, 1000, limits, s);
    char v[32];
    format_card_value(*find(s, "ph"), v, sizeof(v));
    CHECK(strcmp(v, "-") == 0, "absent pH formats as a dash (not 0.0)");
    format_card_value(*find(s, "orp"), v, sizeof(v));
    CHECK(strcmp(v, "-") == 0, "absent ORP formats as a dash (not 0 mV)");
    const Card* ph = find(s, "ph");
    CHECK(ph && ph->sev == CardSev::None,
          "absent pH carries no severity accent (nothing to warn about)");
  }

  // ── severity bands come from the research doc's targets ────────────────
  {
    // pH out of the 7.0–7.8 band warns; inside is quiet.
    Witness lowph = mk_pool(true, 68, true, 712, false, 0, false, 0);
    CardSet s; build_cards(lowph, 1000, limits, s);
    CHECK(find(s, "ph")->sev == CardSev::Warn, "pH 6.8 warns (below 7.0)");
    Witness okph = mk_pool(true, 74, true, 712, false, 0, false, 0);
    build_cards(okph, 1000, limits, s);
    CHECK(find(s, "ph")->sev == CardSev::None, "pH 7.4 is quiet (in band)");
    // ORP below 650 mV = sanitizer low.
    Witness loworp = mk_pool(true, 74, true, 600, false, 0, false, 0);
    build_cards(loworp, 1000, limits, s);
    CHECK(find(s, "orp")->sev == CardSev::Warn, "ORP 600 mV warns (sanitizer low)");
    Witness okorp = mk_pool(true, 74, true, 700, false, 0, false, 0);
    build_cards(okorp, 1000, limits, s);
    CHECK(find(s, "orp")->sev == CardSev::None, "ORP 700 mV is quiet");
    // TDS never self-warns (verdict depends on pool type).
    Witness salt = mk_pool(true, 74, true, 712, false, 0, true, 5200);
    build_cards(salt, 1000, limits, s);
    CHECK(find(s, "tds")->sev == CardSev::None, "TDS carries no verdict severity");
  }

  // ── classify_event: pool threshold events warn, don't fall to Notice ───
  {
    CHECK(classify_event("sanitizer_low") == Sev::Warn, "sanitizer_low -> warn");
    CHECK(classify_event("orp_low") == Sev::Warn, "orp_low -> warn");
    CHECK(classify_event("chlorine_low") == Sev::Warn, "chlorine_low -> warn");
    CHECK(classify_event("ph_high") == Sev::Warn, "ph_high -> warn");
    CHECK(classify_event("ph_low") == Sev::Warn, "ph_low -> warn");
    CHECK(classify_event("ph_out_of_range") == Sev::Warn, "ph_out_of_range -> warn");
    CHECK(classify_event("no_flow") == Sev::Warn, "no_flow -> warn");
    CHECK(classify_event("flow_lost") == Sev::Warn, "flow_lost -> warn");
    // The ladder is still worst-first: a pool tamper isn't downgraded to warn.
    CHECK(classify_event("pool_tamper") == Sev::Tamper, "pool_tamper stays tamper");
    // A cleared / boot event is Ok, unchanged by the new branch.
    CHECK(classify_event("chem_cleared") == Sev::Ok, "chem_cleared -> ok");
  }

  // ── on_pool_state overwrites: a later all-null row clears stale readings ──
  // Flow-gating honesty (docs §8): when flow stops the node republishes the
  // sample as null, so the Dash must drop the last good pH/ORP to unknown
  // rather than keep presenting it as current.
  {
    FleetModel<8, 16> m;
    PoolState valid;
    valid.have_ph = true;  valid.ph_x10 = 74;
    valid.have_orp = true; valid.orp_mv = 712;
    m.on_pool_state("pool1", valid, 1000);
    const Witness* w = m.at(0);
    CHECK(w && w->pool_present && w->have_ph && w->ph_x10 == 74 && w->have_orp,
          "a valid chemistry row stores pH/ORP");
    PoolState cleared;  // all have_* false — the null "flow stopped" snapshot
    m.on_pool_state("pool1", cleared, 2000);
    w = m.at(0);
    CHECK(w && !w->have_ph && !w->have_orp,
          "a flow-stop row clears cached pH/ORP (no stale-good reading)");
    CHECK(w && w->pool_present, "the device stays pool-bearing after the clear");
    CardSet s; build_cards(*w, 3000, limits, s);
    char v[32]; format_card_value(*find(s, "ph"), v, sizeof(v));
    CHECK(strcmp(v, "-") == 0, "cleared pH renders a dash on the card, not a stale value");
  }

  if (g_failures == 0) printf("ALL FLEET CARDS TESTS PASSED\n");
  else printf("%d FAILURE(S)\n", g_failures);
  return g_failures ? 1 : 0;
}
