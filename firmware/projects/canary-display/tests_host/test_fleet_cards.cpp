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

  // ── null-vs-zero honesty: an unknown radar renders "—", not "0" ─────────
  {
    Witness w = mk_sense(0 /*unknown*/, 0, 0 /*unknown*/, false,
                         false, false, false, false, 0, 0, -1);
    CardSet s;
    build_cards(w, 1000, limits, s);
    const Card* pres = find(s, "presence");
    char v[32];
    format_card_value(*pres, v, sizeof(v));
    CHECK(strcmp(v, "—") == 0, "unknown presence formats as — (not clear/present)");
    const Card* rng = find(s, "range_band");
    format_card_value(*rng, v, sizeof(v));
    CHECK(strcmp(v, "—") == 0, "unknown range formats as —");
    const Card* lux = find(s, "illuminance");
    format_card_value(*lux, v, sizeof(v));
    CHECK(strcmp(v, "—") == 0, "unpublished lux formats as — (not 0 lx)");
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
    CHECK(strcmp(v, "—") == 0, "P1 sparkline with invalid bpm formats — even when offered");
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

  if (g_failures == 0) printf("ALL FLEET CARDS TESTS PASSED\n");
  else printf("%d FAILURE(S)\n", g_failures);
  return g_failures ? 1 : 0;
}
