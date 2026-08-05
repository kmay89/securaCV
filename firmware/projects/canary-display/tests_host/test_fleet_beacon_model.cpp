// Host-side test for FleetModel::on_beacon — the fleet-link ingest. Drives the
// dependency-free template directly (no Arduino/NimBLE), asserting the same
// non-trust rules the passive beacon + GATT pull rely on: suffix-match an
// existing witness, pseudo-create an unknown one, stamp liveness/seen_via_ble,
// store battery/health/chain, leave the trust badge untouched, and dedupe
// tamper on a 60 s edge like a chirp.

#include "canary/fleet/fleet_model.h"

#include <cstdio>
#include <cstring>

using namespace canary::fleet;

static int g_failures = 0;
#define CHECK(cond, msg)                                            \
  do {                                                             \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; }   \
    else         { printf("ok:   %s\n", (msg)); }                 \
  } while (0)

using Model = FleetModel<8, 32>;

// Find a witness by id (linear over the visible slots).
static const Witness* find(const Model& m, const char* id) {
  for (int i = 0; i < m.count(); i++) {
    const Witness* w = m.at(i);
    if (w && strcmp(w->id, id) == 0) return w;
  }
  return nullptr;
}

static BeaconStatus mk_status(int batt, int health, uint32_t chain, bool tamper) {
  BeaconStatus s;
  if (batt >= 0)   { s.battery_pct = (int16_t)batt; s.battery_present = true; }
  if (health >= 0) { s.health = (int16_t)health;    s.health_present = true; }
  s.chain_seq = chain; s.chain_present = true;
  s.tamper = tamper;
  return s;
}

int main() {
  // ── Unknown suffix -> pseudo witness "SCV-XXXX" ──────────────────────
  {
    Model m;
    BeaconStatus s = mk_status(80, 90, 1234, false);
    m.on_beacon("dead", s, /*have_status=*/true, /*now=*/1000);
    const Witness* w = find(m, "SCV-dead");
    CHECK(w != nullptr, "pseudo-creates SCV-dead for an unknown suffix");
    if (w) {
      CHECK(w->seen_via_ble, "sets seen_via_ble");
      CHECK(w->link == Link::Online, "sets link Online (liveness)");
      CHECK(w->last_seen_ms == 1000 && w->last_beacon_ms == 1000,
            "stamps last_seen_ms + last_beacon_ms");
      CHECK(w->battery_present && w->battery_pct == 80, "stores battery");
      CHECK(w->ble_health_present && w->ble_health == 90, "stores ble_health");
      CHECK(w->chain_length == 1234, "stores chain -> chain_length");
      CHECK(w->badge == Badge::Unknown, "badge untouched on a pseudo witness");
    }
  }

  // ── Suffix-match an existing (signed) witness; badge untouched ───────
  {
    Model m;
    // A real, verified witness with a 16-hex fingerprint ending "6677".
    m.on_chain("canary_kitchen", 42, Badge::Verified, /*now=*/500,
               nullptr, 0, "0011223344556677");
    const int before = m.count();

    BeaconStatus s = mk_status(55, 60, 99, false);
    m.on_beacon("6677", s, true, /*now=*/2000);

    CHECK(m.count() == before, "suffix-match does NOT create a second witness");
    const Witness* w = find(m, "canary_kitchen");
    CHECK(w != nullptr, "matched the existing witness by fp suffix");
    if (w) {
      CHECK(w->seen_via_ble && w->last_beacon_ms == 2000, "beacon updates the match");
      CHECK(w->battery_present && w->battery_pct == 55, "beacon battery stored on match");
      CHECK(w->ble_health == 60 && w->ble_health_present, "beacon health stored on match");
      CHECK(w->chain_length == 99, "beacon chain overwrites chain_length");
      CHECK(w->badge == Badge::Verified, "badge stays Verified (never set by beacon)");
    }
  }

  // ── have_status=false: liveness only, no stored status ───────────────
  {
    Model m;
    BeaconStatus s;  // ignored
    m.on_beacon("beef", s, /*have_status=*/false, /*now=*/300);
    const Witness* w = find(m, "SCV-beef");
    CHECK(w != nullptr, "liveness-only beacon still creates the witness");
    if (w) {
      CHECK(w->link == Link::Online && w->seen_via_ble, "liveness set without status");
      CHECK(!w->battery_present, "no battery stored when have_status=false");
      CHECK(w->chain_length == 0, "no chain stored when have_status=false");
    }
  }

  // ── Tamper rides a 60 s edge-dedupe, like a chirp ────────────────────
  {
    Model m;
    BeaconStatus t = mk_status(-1, -1, 0, /*tamper=*/true);

    m.on_beacon("f00d", t, true, /*now=*/10000);
    CHECK(m.events_count() == 1, "first tamper (ble) pushes an event");
    CHECK(m.worst(10000) == Sev::Tamper, "tamper raises fleet severity");

    m.on_beacon("f00d", t, true, /*now=*/10000 + 30000);   // < 60 s later
    CHECK(m.events_count() == 1, "repeat tamper within 60 s is deduped");

    m.on_beacon("f00d", t, true, /*now=*/10000 + 61000);   // > 60 s later
    CHECK(m.events_count() == 2, "tamper after 60 s pushes a fresh event");

    // The event log names it "tamper (ble)".
    const EventRow* e = m.event_at(0);
    CHECK(e && strcmp(e->name, "tamper (ble)") == 0, "event named 'tamper (ble)'");
    CHECK(e && e->sev == Sev::Tamper, "event severity is Tamper");
    CHECK(e && e->signed_flag == false, "beacon tamper event is unsigned");
  }

  // ── v2 detection alert: class + confidence on the event line ─────────
  {
    Model m;
    BeaconStatus d = mk_status(-1, -1, 0, /*tamper=*/false);
    d.alert = true;
    d.detect_class = 1;   // person
    d.detect_score = 87;

    m.on_beacon("beef", d, true, /*now=*/10000);
    CHECK(m.events_count() == 1, "first detection beacon pushes an event");
    const EventRow* e = m.event_at(0);
    CHECK(e && strcmp(e->name, "person 87% (ble)") == 0,
          "event named 'person 87% (ble)'");
    CHECK(e && e->sev == Sev::Warn, "detection severity is Warn");
    CHECK(e && e->signed_flag == false, "detection event is unsigned");
    const Witness* w = find(m, "SCV-beef");
    CHECK(w && w->badge == Badge::Unknown, "badge untouched by a detection");
    CHECK(m.worst(10000) >= Sev::Warn, "detection raises fleet attention");

    // Continuous advert: same class within 60 s is deduped — even as the
    // confidence wobbles.
    d.detect_score = 91;
    m.on_beacon("beef", d, true, /*now=*/10000 + 30000);
    CHECK(m.events_count() == 1, "same-class detection within 60 s deduped");

    // A NEW class re-arms immediately (a vehicle after a person is news).
    d.detect_class = 2;   // vehicle
    d.detect_score = -1;  // unknown confidence
    m.on_beacon("beef", d, true, /*now=*/10000 + 31000);
    CHECK(m.events_count() == 2, "class change re-arms within the window");
    e = m.event_at(0);
    CHECK(e && strcmp(e->name, "vehicle (ble)") == 0,
          "unknown confidence drops the percent");

    // After 60 s the same class fires again.
    m.on_beacon("beef", d, true, /*now=*/10000 + 31000 + 61000);
    CHECK(m.events_count() == 3, "same class after 60 s pushes a fresh event");

    // Tamper keeps precedence over a simultaneous detection.
    BeaconStatus both = mk_status(-1, -1, 0, /*tamper=*/true);
    both.alert = true;
    both.detect_class = 1;
    both.detect_score = 50;
    Model m2;
    m2.on_beacon("f00d", both, true, /*now=*/5000);
    const EventRow* e2 = m2.event_at(0);
    CHECK(e2 && strcmp(e2->name, "tamper (ble)") == 0,
          "tamper wins the event line over a detection");
    CHECK(m2.events_count() == 1, "tamper+detection pushes only the tamper");

    // An idle v2 beacon (alert clear) raises no event at all.
    Model m3;
    BeaconStatus idle = mk_status(80, 90, 7, /*tamper=*/false);
    m3.on_beacon("cafe", idle, true, /*now=*/1000);
    CHECK(m3.events_count() == 0, "idle beacon pushes no event");
  }

  // ── display peers seat in the roster but never trip the witness-lost
  //    alarm ladder (PR #1300: an unplugged sibling screen is ordinary) ──
  {
    Model m;
    // A witness and a sibling display, both alive at t=0.
    m.on_status("wap-1", "canary-wap", true, -1, 0);
    m.on_status("night-1", "canary-display-nightstand-c6", true, -1, 0);
    m.acknowledge(0);  // household is acked and quiet
    CHECK(m.worst(1000) == Sev::Ok, "both peers online -> fleet Ok");

    // Long silence: both cross the lost deadline on tick.
    const uint32_t late = 30UL * 60UL * 1000UL;  // 30 min, past any default
    m.tick(late);
    const Witness* d = find(m, "night-1");
    CHECK(d && d->link == Link::Lost, "silent display shows Lost in the roster");
    CHECK(m.witness_sev(*d, late) <= Sev::Notice,
          "lost display is at most a Notice, never the witness-lost Alert");
    const Witness* w = find(m, "wap-1");
    CHECK(w && m.witness_sev(*w, late) == Sev::Alert,
          "lost WITNESS still alerts exactly as before");

    // The un-ack on crossing Lost is witness-only: with just a display lost,
    // a standing ack must survive.
    Model m2;
    m2.on_status("night-2", "canary-display-nightstand-s3", true, -1, 0);
    m2.acknowledge(0);
    m2.tick(late);
    CHECK(m2.ack_active(late), "display going Lost does not un-ack the household");
  }

  printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
