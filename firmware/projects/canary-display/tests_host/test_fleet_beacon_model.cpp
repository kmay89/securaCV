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

  // ── One Canary on several bands is ONE witness and ONE event ─────────
  // The same beacon bytes reach this model over BLE, ESP-NOW and LAN
  // multicast. Every band must fold into the slot the fingerprint suffix
  // already owns: adding a radio buys coverage, never a duplicate device and
  // never a duplicate alarm. This is the property the whole multi-transport
  // design rests on, so it is asserted rather than assumed.
  {
    Model m;
    BeaconStatus d;
    d.alert = true; d.detect_class = 1; d.detect_score = 87;

    m.on_beacon("dead", d, true, /*now=*/1000, Via::Ble);
    CHECK(m.count() == 1, "first band seats exactly one witness");
    CHECK(m.events_count() == 1, "first band pushes the sighting");

    // The SAME sighting arrives over WiFi 200 ms later — a second radio heard
    // the same advert, not a second person.
    m.on_beacon("dead", d, true, /*now=*/1200, Via::Wifi);
    CHECK(m.count() == 1, "second band does NOT create a second witness");
    CHECK(m.events_count() == 1, "second band does NOT duplicate the event");

    // ...and a third band changes nothing either.
    m.on_beacon("dead", d, true, /*now=*/1400, Via::Mesh);
    CHECK(m.count() == 1, "third band still one witness");
    CHECK(m.events_count() == 1, "third band still one event");

    const Witness* w = m.at(0);
    CHECK(w && w->carrier_count(1400) == 3, "all three bands credited as carrying");
    CHECK(w && w->carried_by(Via::Wifi, 1400), "wifi credited");

    // Bands age out on their own: at +VIA_FRESH_MS nothing is claimed any
    // more, with no teardown call anywhere. That is the self-healing — a
    // radio that dies needs no signal, and one that returns needs no reset.
    CHECK(w && w->carrier_count(1400 + VIA_FRESH_MS + 1) == 0,
          "silent bands stop being credited without any teardown");

    // A band that comes back is simply heard again.
    m.on_beacon("dead", d, true, /*now=*/1400 + VIA_FRESH_MS + 100, Via::Wifi);
    const Witness* w2 = m.at(0);
    CHECK(w2 && w2->carrier_count(1400 + VIA_FRESH_MS + 100) == 1,
          "a returning band re-credits itself with no reset");
  }

  // ── Cross-band tamper is one alarm, not one per radio ────────────────
  // This is the case the label change would have broken: the dedupe used to
  // compare last_event against the literal "tamper (ble)", so a tamper heard
  // on a second band wrote a different string and fired again.
  {
    Model m;
    BeaconStatus t = mk_status(-1, -1, 0, /*tamper=*/true);

    m.on_beacon("f00d", t, true, /*now=*/10000, Via::Ble);
    CHECK(m.events_count() == 1, "tamper on the first band alarms once");

    m.on_beacon("f00d", t, true, /*now=*/10500, Via::Wifi);
    CHECK(m.events_count() == 1, "same tamper on a second band does not re-alarm");

    m.on_beacon("f00d", t, true, /*now=*/11000, Via::Mesh);
    CHECK(m.events_count() == 1, "nor on a third");

    // The window is still a window — it just isn't per-band.
    m.on_beacon("f00d", t, true, /*now=*/10000 + 61000, Via::Wifi);
    CHECK(m.events_count() == 2, "tamper after 60 s still pushes a fresh event");
  }

  // ── The band on the glass is the band that carried it ────────────────
  // "(wifi)" is a claim about how the news arrived with no broker in the
  // path. It must never appear on a BLE advert, or the honesty is decoration.
  {
    Model m;
    BeaconStatus d;
    d.alert = true; d.detect_class = 1; d.detect_score = 87;

    m.on_beacon("aaaa", d, true, /*now=*/1000, Via::Wifi);
    const EventRow* e = m.event_at(0);
    CHECK(e && strcmp(e->name, "person 87% (wifi)") == 0,
          "wifi-carried sighting reads 'person 87% (wifi)'");

    Model m2;
    m2.on_beacon("bbbb", d, true, /*now=*/1000, Via::Mesh);
    const EventRow* e2 = m2.event_at(0);
    CHECK(e2 && strcmp(e2->name, "person 87% (mesh)") == 0,
          "esp-now-carried sighting reads '(mesh)', not '(ble)'");

    Model m3;
    BeaconStatus t = mk_status(-1, -1, 0, /*tamper=*/true);
    m3.on_beacon("cccc", t, true, /*now=*/1000, Via::Wifi);
    const EventRow* e3 = m3.event_at(0);
    CHECK(e3 && strcmp(e3->name, "tamper (wifi)") == 0,
          "tamper names its band too");
  }

  // ── display peers seat in the roster but never trip the witness-lost
  //    alarm ladder (PR #1300: an unplugged sibling screen is ordinary) ──
  {
    Model m;
    // A witness and a sibling display, both alive at t=0.
    m.on_status("wap-1", "canary-wap", true, -1, 0);
    m.on_status("night-1", "canary-nightstand", true, -1, 0);
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
    m2.on_status("night-2", "canary-nightstand", true, -1, 0);
    m2.acknowledge(0);
    m2.tick(late);
    CHECK(m2.ack_active(late), "display going Lost does not un-ack the household");
  }

  // ── every device_type a display flavor really publishes is a sibling
  //    screen (configs/canary-display/*/config.h CD_DEVICE_TYPE) ──
  {
    const char* kTypes[] = {"canary-nightstand", "canary-nightstand7",
                            "canary-dash", "canary-watch",
                            "canary-nightlight", "canary-display"};
    const uint32_t late = 30UL * 60UL * 1000UL;
    for (const char* t : kTypes) {
      Model m;
      m.on_status("screen", t, true, -1, 0);
      m.tick(late);
      const Witness* d = find(m, "screen");
      CHECK(d && m.witness_sev(*d, late) <= Sev::Notice, t);
    }
    // ...and the sensors are not: a lost Sense, Vision or WAP still alarms.
    const char* kSensors[] = {"canary", "canary-wap", "canary-sense",
                              "canary-vision"};
    for (const char* t : kSensors) {
      Model m;
      m.on_status("sensor", t, true, -1, 0);
      m.tick(late);
      const Witness* w = find(m, "sensor");
      CHECK(w && m.witness_sev(*w, late) == Sev::Alert, t);
    }
  }

  // ── A canary-wap's fp arrives in capitals (canary_wap.ino hex_to_str) ──
  // The beacon/chirp fp4 is rendered lowercase (beacon_parse.h), so the model
  // must match it to the WAP's row and retire the WAP's ghost either way.
  {
    Model m;
    m.on_chain("canary-s3-4dC2", 42, Badge::Verified, /*now=*/500,
               nullptr, 0, "7916CA487912FA1B");
    const Witness* w = find(m, "canary-s3-4dC2");
    CHECK(w && strcmp(w->fp, "7916ca487912fa1b") == 0,
          "a capital fp is stored lowercase");
    BeaconStatus s = mk_status(80, 90, 43, false);
    m.on_beacon("fa1b", s, true, /*now=*/1000);
    CHECK(m.count() == 1, "the WAP's beacon lands on its row, no SCV-fa1b twin");
    w = find(m, "canary-s3-4dC2");
    CHECK(w && w->seen_via_ble, "the WAP row is marked seen over BLE");

    Model g;
    g.on_chirp("fa1b", 0x01, /*now=*/400);
    CHECK(find(g, "SCV-fa1b") != nullptr, "an early chirp makes the ghost");
    g.on_chain("canary-s3-4dC2", 42, Badge::Verified, /*now=*/500,
               nullptr, 0, "7916CA487912FA1B");
    CHECK(find(g, "SCV-fa1b") == nullptr, "the WAP's capital fp retires its ghost");
    CHECK(g.count() == 1, "one row for the WAP after the chain lands");
  }

  // ── Audit A2-09: unsigned frames draw attention, never a lasting alarm ──
  // Every receive path above (BLE advert, LAN multicast, ESP-NOW) is
  // unsigned, so a neighbor's Canary through the wall or any host on the
  // Wi-Fi can seat a "SCV-XXXX" pseudo witness. These pin what such a row
  // may and may not do to the household's glass.
  const FleetLimits lim{};   // the shipped defaults: stale 3 min, lost 10 min

  // A ghost heard once over LAN multicast, then silent: Warn while stale, and
  // at the lost deadline it leaves the roster. It never goes red and never
  // cancels the household's ack (the old ladder did both at 10 min).
  {
    Model m;
    BeaconStatus s = mk_status(-1, -1, 0, /*tamper=*/false);
    m.on_beacon("ab12", s, true, /*now=*/0, Via::Wifi);
    m.acknowledge(0);
    const uint32_t stale = lim.stale_after_ms + 1000;
    m.tick(stale);
    CHECK(m.worst(stale) == Sev::Warn, "silent ghost reads Warn while stale");
    const uint32_t lost = lim.lost_after_ms + 60000;
    m.tick(lost);
    CHECK(m.worst(lost) < Sev::Alert, "silent ghost never climbs to the witness-lost Alert");
    CHECK(m.ack_active(lost), "silent ghost does not cancel the household's ack");
    CHECK(find(m, "SCV-ab12") == nullptr, "ghost is retired at the lost deadline");
    CHECK(m.count() == 0, "its slot is free again");
  }

  // A real Canary on the same glass still climbs the ladder exactly as before.
  {
    Model m;
    m.on_status("canary_door", "canary-wap", true, -1, /*now=*/0);
    m.on_chain("canary_door", 7, Badge::Verified, /*now=*/0, nullptr, 0,
               "00112233445566aa");
    m.acknowledge(0);
    const uint32_t lost = lim.lost_after_ms + 60000;
    m.tick(lost);
    const Witness* w = find(m, "canary_door");
    CHECK(w && w->link == Link::Lost, "a silent real Canary still goes Lost");
    CHECK(m.worst(lost) == Sev::Alert, "a lost real Canary still alerts");
    CHECK(!m.ack_active(lost), "a lost real Canary still cancels the ack");
  }

  // A roster full of ghosts: a real Canary arriving later takes the
  // longest-silent ghost's slot instead of being dropped.
  {
    Model m;
    const char* sfx[8] = {"a001", "a002", "a003", "a004",
                          "a005", "a006", "a007", "a008"};
    BeaconStatus s;
    for (int i = 0; i < 8; i++)
      m.on_beacon(sfx[i], s, false, /*now=*/1000 + (uint32_t)i * 100, Via::Wifi);
    CHECK(m.count() == 8, "eight forged suffixes fill an 8-slot roster");
    m.on_chain("canary_real", 3, Badge::Verified, /*now=*/5000, nullptr, 0,
               "0011223344556677");
    CHECK(find(m, "canary_real") != nullptr, "a real Canary still seats on a ghost-full roster");
    CHECK(m.count() == 8, "it takes a slot, the roster does not grow");
    CHECK(find(m, "SCV-a001") == nullptr, "the longest-silent ghost made room");
    CHECK(find(m, "SCV-a002") != nullptr, "the other ghosts stay");
    m.on_status("canary_sense_2", "canary-sense", true, -1, /*now=*/5100);
    CHECK(find(m, "canary_sense_2") != nullptr, "a status row seats a real Canary too");
    CHECK(find(m, "SCV-a002") == nullptr, "...taking the next-oldest ghost");
    CHECK(m.set_mute("canary_cam_3", true, 900000), "a persisted mute re-applies on a ghost-full roster");

    // An unsigned frame never displaces anyone: a ninth suffix is dropped.
    m.on_beacon("ffff", s, false, /*now=*/6000, Via::Wifi);
    CHECK(find(m, "SCV-ffff") == nullptr, "a new forged suffix cannot evict a ghost or a Canary");
    CHECK(find(m, "canary_real") != nullptr, "the real Canary keeps its slot");

    // A roster of real Canaries is still never rotated.
    Model r;
    char id[16];
    for (int i = 0; i < 8; i++) {
      snprintf(id, sizeof(id), "canary_%d", i);
      r.on_status(id, "canary-wap", true, -1, /*now=*/100);
    }
    r.on_status("canary_9", "canary-wap", true, -1, /*now=*/200);
    CHECK(find(r, "canary_9") == nullptr, "over-cap real Canaries are dropped, not rotated");
    CHECK(r.count() == 8, "a real roster keeps its eight");
  }

  // The mDNS discovery browse is unauthenticated LAN input too: it passes
  // may_evict=false, so its rows take a free slot or nothing. On a hub-less
  // glass every household Canary is a pseudo witness; announced role=witness
  // services must not push them off the roster.
  {
    Model m;
    BeaconStatus s;
    m.on_beacon("aa01", s, false, /*now=*/1000, Via::Ble);
    m.on_beacon("aa02", s, false, /*now=*/1100, Via::Ble);
    char id[16];
    for (int i = 0; i < 8; i++) {
      snprintf(id, sizeof(id), "fake_%d", i);
      m.on_status(id, "canary-wap", true, -1, /*now=*/2000, /*may_evict=*/false);
      m.on_meta(id, "Front door", "", /*now=*/2000, /*may_evict=*/false);
    }
    CHECK(m.count() == 8, "mDNS rows fill only the free slots");
    CHECK(find(m, "SCV-aa01") != nullptr && find(m, "SCV-aa02") != nullptr,
          "an mDNS row never evicts a beacon-heard Canary");
    CHECK(find(m, "fake_6") == nullptr && find(m, "fake_7") == nullptr,
          "over-cap mDNS rows are dropped");
    m.on_beacon("aa01", s, false, /*now=*/3000, Via::Ble);
    const Witness* a = find(m, "SCV-aa01");
    CHECK(a && a->last_seen_ms == 3000, "the household Canary's next beacon still lands on its row");

    // A ghost-full roster: a may_evict=false status or meta row seats
    // nothing and evicts nothing.
    Model g;
    const char* sfx[8] = {"b001", "b002", "b003", "b004",
                          "b005", "b006", "b007", "b008"};
    for (int i = 0; i < 8; i++)
      g.on_beacon(sfx[i], s, false, /*now=*/1000 + (uint32_t)i * 100, Via::Wifi);
    g.on_status("lan_host", "canary-wap", true, -1, /*now=*/5000, /*may_evict=*/false);
    g.on_meta("lan_name", "Kitchen", "", /*now=*/5000, /*may_evict=*/false);
    CHECK(find(g, "lan_host") == nullptr && find(g, "lan_name") == nullptr,
          "a may_evict=false row seats nothing on a ghost-full roster");
    CHECK(g.count() == 8 && find(g, "SCV-b001") != nullptr,
          "...and evicts nothing, not even the longest-silent ghost");
  }

  // One LAN datagram with the tamper flag on a real Canary: Tamper now, and
  // it decays on the Warn clock instead of holding until reboot.
  {
    Model m;
    m.on_chain("canary_door", 7, Badge::Verified, /*now=*/0, nullptr, 0,
               "00112233445566aa");
    BeaconStatus t = mk_status(-1, -1, 0, /*tamper=*/true);
    m.on_beacon("66aa", t, true, /*now=*/1000, Via::Wifi);
    CHECK(m.worst(1000) == Sev::Tamper, "a UDP tamper draws Tamper attention");
    CHECK(m.worst(1000 + lim.warn_decay_ms - 1) == Sev::Tamper,
          "it holds for the Warn decay window");
    CHECK(m.worst(1000 + lim.warn_decay_ms) == Sev::Ok,
          "an unsigned tamper decays like a Warn");
    CHECK(m.worst(1000 + 6UL * 3600UL * 1000UL) == Sev::Ok, "and is gone hours later");

    // An unsigned chirp ALERT on the same Canary decays the same way.
    m.on_chirp("66aa", 0x01, /*now=*/2000000);
    CHECK(m.worst(2000000) == Sev::Alert, "a chirp alert draws Alert attention");
    CHECK(m.worst(2000000 + lim.warn_decay_ms) == Sev::Ok, "an unsigned chirp alert decays");

    // A broker-fed tamper event keeps the full rule: no decay.
    Model b;
    b.on_event("canary_door", "enclosure_tamper", /*signed_flag=*/true, /*now=*/1000);
    CHECK(b.worst(1000 + 6UL * 3600UL * 1000UL) == Sev::Tamper,
          "a broker tamper event does not decay");
  }

  printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
