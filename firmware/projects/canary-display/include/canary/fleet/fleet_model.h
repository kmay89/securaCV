#pragma once
#include <stdint.h>
#include <string.h>
#include <cstddef>

// Fleet model — the display's single source of truth about the witnesses it
// watches. Pure C++ (no Arduino/JSON dependencies): the MQTT layer parses
// payloads and feeds already-typed values in here; the UI only reads from
// here. Time is injected (millis-domain uint32, wrap-safe signed deltas) so
// the whole model is host-testable.
//
// Design intent (docs/hardware/display_ux_design.md):
//   - Severity is a small ladder aligned with the HA component's taxonomy
//     (const.py: ... warning < error < critical < alert < tamper) collapsed
//     to what a glance can carry.
//   - Alert-fatigue is the enemy: informational events age out on their
//     own; only Alert/Tamper hold attention until acknowledged, and even an
//     ack leaves a residual marker while the underlying condition persists
//     (Nest pattern: quiet it, never silently un-know it).
//   - The trust badge never overclaims: "Verified" strictly means an
//     Ed25519 signature checked out against a TOFU-pinned key on THIS
//     device — same bar as the HA timeline card's strong green tick.

namespace canary::fleet {

// Severity ladder, worst-last. Collapsed from the HA component's taxonomy.
enum class Sev : uint8_t {
  Ok = 0,      // all quiet
  Notice,      // routine witness activity (presence, occupancy)
  Warn,        // needs a look: stale/low battery/restricted-zone/after-hours
  Alert,       // acoustic alarm patterns, chain verify FAILED, witness lost
  Tamper,      // tamper/panic — highest, mirrors const.py's ladder top
};

// Witness-chain trust badge (timeline-card vocabulary; never overclaim).
enum class Badge : uint8_t {
  Unknown = 0, // nothing seen yet
  Unsigned,    // chain head carries no signature fields
  Signed,      // signature present but no pinned key to check against
  Verified,    // Ed25519 verify passed against the TOFU-pinned pubkey
  Failed,      // signature present and did NOT verify — loud red
};

// Liveness, driven by LWT/status plus local staleness deadlines.
enum class Link : uint8_t {
  Unknown = 0,
  Online,
  Stale,       // silent past the stale deadline (amber)
  Lost,        // silent past the lost deadline (red)
  Offline,     // broker said so (retained LWT "offline")
};

// Which band carried a presence beacon here. The SAME beacon bytes reach this
// model over three transports (see firmware/common/fleet_link/), and a witness
// is identified by its fingerprint suffix — never by the radio it arrived on —
// so hearing one Canary on two bands adds coverage, not a second device and
// not a second event. This tag exists only so the event line can name the band
// honestly; every dedupe decision below is deliberately independent of it.
enum class Via : uint8_t {
  Ble  = 0,   // BLE advert — no WiFi at all, radio range
  Mesh = 1,   // ESP-NOW — no router at all, radio range
  Wifi = 2,   // LAN multicast — no broker, reaches across the house
};
constexpr int VIA_COUNT = 3;

// How long a band is still credited with carrying a witness after its last
// datagram. Three missed 5 s refreshes: long enough to ride out a lost frame,
// short enough that a band which genuinely went away stops being claimed.
constexpr uint32_t VIA_FRESH_MS = 16000;

// Decoded fleet-link presence beacon status (canary/net/beacon_parse.h fills
// this from the 11-byte manufacturer blob). Pure values, no wire types — so
// the model stays dependency-free and host-testable. battery_pct/health carry
// their own "present" flags because the wire uses 0xFF as an unknown sentinel.
struct BeaconStatus {
  int16_t  battery_pct = -1;
  bool     battery_present = false;
  int16_t  health = -1;
  bool     health_present = false;
  uint32_t chain_seq = 0;     // low 16 bits of the WAP's chain height
  bool     chain_present = false;
  bool     mic_muted = false;
  bool     degraded = false;
  bool     tamper = false;
  // v2 beacon detection surface (canary-vision): alert = the sender's
  // presence FSM holds a live detection; class/score say what and how
  // confidently. A v1 beacon decodes to false / 0 / -1.
  bool     alert = false;
  uint8_t  detect_class = 0;  // 0 none / 1 person / 2 vehicle / 3 animal / 4 package
  int16_t  detect_score = -1; // 0..100 confidence %, -1 = unknown
};

// Decoded canary-sense radar state row (mqtt_mgr fills this from the retained
// securacv/<id>/state payload). Pure values, no wire types — so the model
// stays dependency-free and host-testable. Sentinels/`have_*` flags keep
// "not published" distinct from a real zero (schema null-vs-zero honesty).
struct SenseState {
  uint8_t  presence  = 0;      // 0 unknown / 1 clear / 2 present
  uint8_t  occupants = 0;      // 0 "0" / 1 "1" / 2 "2+"
  uint8_t  range     = 0;      // 0 unknown / 1 near / 2 mid / 3 far
  bool     radar_ok  = false;
  uint32_t frame_errors = 0;
  bool     have_lux  = false;
  int      lux       = 0;
  bool     have_bpm  = false;  // the build publishes BPM entities (P1 opt-in)
  bool     bpm_valid = false;  // BPM currently valid (locked, single target)
  uint8_t  breath_bpm = 0;
  uint8_t  heart_bpm  = 0;
};

// Decoded canary-pool water-chemistry state row (mqtt_mgr fills this from the
// retained securacv/<id>/state payload of a pool node — see
// docs/research/pool_water_monitor.md §6). Pure values, no wire types, so the
// model stays dependency-free and host-testable. Each field carries its own
// `have_*` flag: a pump-off / not-yet-published reading is honestly unknown
// (—), never a real zero (pH 0 or 0 mV would read as an emergency, not
// silence). Fixed-point mirrors the comfort-temp convention (tenths) so a
// pH 7.4 or 28.5 °C never rounds to a lie on the glass.
struct PoolState {
  bool     have_ph = false;
  int16_t  ph_x10 = 0;          // pH ×10 (7.4 -> 74)
  bool     have_orp = false;
  int16_t  orp_mv = 0;          // oxidation-reduction potential, millivolts
  bool     have_water_temp = false;
  int16_t  water_temp_c10 = 0;  // water temperature, tenths of °C (28.5° = 285)
  bool     have_tds = false;
  int16_t  tds_ppm = 0;         // total dissolved solids / salinity, ppm
};

struct Witness {
  bool     used = false;
  char     id[48] = {0};
  char     device_type[24] = {0};

  // Rooms & names (trailblazer spec §8; retained securacv/<id>/meta).
  // The glass speaks "Kitchen", never "canary_sense_001", once meta exists.
  char     name[24] = {0};
  char     room[16] = {0};

  // Pubkey fingerprint (16 hex, from the chain payload). Its first 4 hex
  // chars are the 2-byte id Chirp adverts carry — the off-grid correlator.
  char     fp[17] = {0};

  // Wellbeing surface (spec §9; canary-sense wellbeing state topic).
  bool     wb_present = false;    // this witness publishes a breathing lock
  bool     wb_breathing = false;  // lock currently held

  // Radar claim surface (canary-sense state topic) — the coarse vocabulary the
  // glass renders as Canary Cards (docs/standard/CANARY_CARDS.md). Compact by
  // design: a handful of bytes per witness, no per-target data ever. `*_known`
  // / sentinels keep "not published" (—) distinct from a real zero, the
  // schema's null-vs-zero honesty. Set by on_sense_state; drives has_cards().
  bool     sense_present  = false;  // has published at least one radar state row
  uint8_t  radar_presence = 0;      // 0 unknown / 1 clear / 2 present
  uint8_t  radar_occupants = 0;     // 0 "0" / 1 "1" / 2 "2+"
  uint8_t  radar_range    = 0;      // 0 unknown / 1 near / 2 mid / 3 far
  bool     radar_ok       = false;  // false while the radar UART is stalled
  uint16_t frame_errors   = 0;      // UART checksum drops (monotonic, clamped)
  int16_t  lux            = -1;     // BH1750 illuminance; -1 = not published
  bool     bpm_valid      = false;  // P1 numerics currently valid (locked+1 tgt)
  bool     bpm_offered    = false;  // this build publishes BPM entities at all
  uint8_t  breath_bpm     = 0;      // P1 breaths/min (0 unless bpm_valid)
  uint8_t  heart_bpm      = 0;      // P1 beats/min  (0 unless bpm_valid)

  // Pool water-chemistry surface (canary-pool state topic) — the glance
  // vocabulary the glass renders as Canary Cards, exactly like the radar
  // block above. Compact fixed-point; `have_*` keeps "not published / no flow"
  // (—) distinct from a real zero. Set by on_pool_state; drives has_cards().
  bool     pool_present   = false;  // has published at least one chemistry row
  bool     have_ph        = false;
  int16_t  ph_x10         = 0;      // pH ×10 (7.4 -> 74); have_ph gates it
  bool     have_orp       = false;
  int16_t  orp_mv         = 0;      // sanitizer proxy, millivolts
  bool     have_water_temp = false;
  int16_t  water_temp_c10 = 0;      // water temp, tenths of °C (28.5° = 285)
  bool     have_tds       = false;
  int16_t  tds_ppm        = 0;      // total dissolved solids / salinity, ppm

  // Liveness
  Link     link = Link::Unknown;
  uint32_t last_seen_ms = 0;      // any topic activity from this device

  // Direct fleet-link (BLE presence beacon / GATT status) surface. Coarse and
  // UNSIGNED like a chirp, so these feed liveness + diagnostics but never
  // trust (badge is never set from a beacon). ble_health is the WAP's own
  // 0..100 self-report; last_beacon_ms/seen_via_ble mark that we heard it
  // directly over BLE (no broker, no home WiFi).
  uint8_t  ble_health = 0;
  bool     ble_health_present = false;
  bool     ble_mic_muted = false;
  bool     ble_degraded = false;
  uint32_t last_beacon_ms = 0;
  bool     seen_via_ble = false;   // heard directly on ANY band (no broker)
  // Per-band last-heard stamps, indexed by Via. This is the whole "which bands
  // are carrying this witness right now" state: a band is credited when a
  // datagram lands and simply ages out of VIA_FRESH_MS when it stops, so a
  // radio that dies needs no teardown signal and a radio that comes back needs
  // no re-registration. That is the self-healing — nothing to reset by hand.
  uint32_t via_ms[VIA_COUNT] = {0, 0, 0};

  // Is this band carrying the witness right now? Answered from the stamp
  // alone, so a band recovers by simply being heard again — there is no
  // "connected" flag anywhere that could be left set by a radio that died.
  bool carried_by(Via via, uint32_t now) const {
    const int i = (int)via;
    if (i < 0 || i >= VIA_COUNT) return false;
    return via_ms[i] != 0 && (int32_t)(now - via_ms[i]) < (int32_t)VIA_FRESH_MS;
  }

  // How many bands are carrying it — 0 means every direct path has gone quiet
  // (the witness may still be reachable through the broker, which is a
  // different question and a different surface).
  int carrier_count(uint32_t now) const {
    int n = 0;
    for (int i = 0; i < VIA_COUNT; i++) {
      if (carried_by((Via)i, now)) n++;
    }
    return n;
  }
  // Beacon detection-alert dedupe (on_beacon): a CONTINUOUS v2 advert keeps
  // saying "person" for as long as one is present, so the attention edge is
  // re-armed per (witness, class) per minute — same posture as the chirp/
  // tamper dedupe. Class 0xFF = never fired. Keyed on (witness, class) and
  // NOT on the band, so the same sighting arriving over BLE and WiFi within
  // the window is one attention event, not two.
  uint32_t ble_alert_ms = 0;
  uint8_t  ble_alert_class = 0xFF;
  // Tamper edge-dedupe, as a timestamp rather than a label comparison. It used
  // to re-fire by matching last_event against "tamper (ble)"; once the label
  // names the band that carried it, an identical tamper heard on a second band
  // writes a different string and would have fired a second time. The stamp is
  // band-independent, so it can't.
  uint32_t tamper_event_ms = 0;
  bool     tamper_event_seen = false;

  // Last event (edge-triggered attention with per-severity decay)
  char     last_event[40] = {0};
  uint32_t last_event_ms = 0;
  bool     has_event = false;
  Sev      event_sev = Sev::Ok;

  // Level-triggered conditions
  bool     tamper = false;
  char     tamper_kind[24] = {0};
  int16_t  battery_pct = -1;      // -1 = unknown/not present
  bool     battery_present = false;

  // Diagnostics the witness reports about itself (Roll Call page). RSSI is
  // the witness's OWN WiFi link as published in its status row — the display
  // can't measure a remote radio, it can only relay the self-report.
  int16_t  rssi_dbm = 0;
  bool     rssi_present = false;

  // Room comfort, when the witness publishes it (baby-monitor table stakes;
  // parsed from state/health payloads, absent otherwise).
  int16_t  temp_c10 = 0;          // tenths of °C (21.5° = 215)
  bool     temp_present = false;
  int8_t   humidity_pct = -1;     // -1 = not published

  // Per-witness mute (the security panel "bypass" pattern, done honestly):
  // a muted witness stops nagging — its link/battery/event severities cap at
  // Notice — but stays VISIBLE with a muted tag, and tamper or a failed
  // chain verify still punches through at full severity. A silent, hidden
  // bypass is how real alarms get missed; this one can't hide.
  uint32_t mute_until_ms = 0;
  bool     muted = false;

  // Trust surface
  Badge    badge = Badge::Unknown;
  uint32_t chain_length = 0;
  char     fw[16] = {0};
  // Raw retained chain payload, verbatim — the substrate of Proof-on-Glass
  // (trailblazer spec §1): the QR reproduces exactly what the witness
  // published, so any third party can verify it independently.
  char     chain_raw[360] = {0};
};

// One row of the glance event log (newest first via event_at(0)).
struct EventRow {
  char     device[16] = {0};      // truncated id, display-width
  char     name[40] = {0};
  Sev      sev = Sev::Ok;
  uint32_t at_ms = 0;
  bool     signed_flag = false;
};

struct FleetLimits {
  uint32_t stale_after_ms = 180000;
  uint32_t lost_after_ms  = 600000;
  uint32_t ack_hold_ms    = 3600000;
  // Attention decay for edge events that were never acked.
  uint32_t notice_decay_ms = 600000;   // 10 min
  uint32_t warn_decay_ms   = 1800000;  // 30 min
};

// Map an event name from the wire ("presence_detected", "smoke_alarm_t3",
// "silent_panic", ...) onto the display ladder. Substring heuristics on
// purpose: the fleet's variants spell events differently and a display must
// degrade to Notice, not crash, on vocabulary it hasn't met.
Sev classify_event(const char* event_name);

const char* sev_name(Sev s);

template <int MAX_DEVICES, int EVENT_CAP>
class FleetModel {
 public:
  void set_limits(const FleetLimits& l) { limits_ = l; }

  // ── Ingestion (called by the MQTT dispatcher) ────────────────────────

  void on_activity(const char* id, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    // Any traffic proves liveness — including a device coming back from a
    // retained-offline state.
    if (w->link != Link::Online) dirty_ = true;
    w->link = Link::Online;
  }

  void on_availability(const char* id, bool online, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    const Link next = online ? Link::Online : Link::Offline;
    if (w->link != next) dirty_ = true;
    w->link = next;
  }

  void on_status(const char* id, const char* device_type, bool online,
                 int battery_soc, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    copy_str(w->device_type, sizeof(w->device_type), device_type);
    if (battery_soc >= 0) { w->battery_pct = (int16_t)battery_soc; w->battery_present = true; }
    on_availability(id, online, now);
  }

  // Witness self-reported WiFi RSSI (status row) — Roll Call diagnostics.
  void on_rssi(const char* id, int rssi_dbm, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    if (!w->rssi_present || w->rssi_dbm != (int16_t)rssi_dbm) dirty_ = true;
    w->rssi_dbm = (int16_t)rssi_dbm;
    w->rssi_present = true;
  }

  // Room comfort (state/health payloads that carry it). Tenths of °C so a
  // 21.5° room doesn't render as a lie in either direction.
  void on_comfort(const char* id, int temp_c10, bool have_temp,
                  int humidity_pct, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    if (have_temp) {
      if (!w->temp_present || w->temp_c10 != (int16_t)temp_c10) dirty_ = true;
      w->temp_c10 = (int16_t)temp_c10;
      w->temp_present = true;
    }
    if (humidity_pct >= 0 && humidity_pct <= 100) {
      if (w->humidity_pct != (int8_t)humidity_pct) dirty_ = true;
      w->humidity_pct = (int8_t)humidity_pct;
    }
  }

  void on_health(const char* id, int battery, bool battery_present,
                 const char* fw, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    if (battery_present && battery >= 0) {
      w->battery_pct = (int16_t)battery;
      w->battery_present = true;
    }
    if (fw && fw[0]) copy_str(w->fw, sizeof(w->fw), fw);
    dirty_ = true;
  }

  void on_event(const char* id, const char* event_name, bool signed_flag,
                uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    w->link = Link::Online;
    copy_str(w->last_event, sizeof(w->last_event), event_name);
    w->last_event_ms = now;
    w->has_event = true;
    w->event_sev = classify_event(event_name);
    push_event(id, event_name, w->event_sev, signed_flag, now);
    dirty_ = true;
  }

  void on_tamper(const char* id, bool active, const char* kind, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    const bool was = w->tamper;
    if (was != active) dirty_ = true;
    w->tamper = active;
    copy_str(w->tamper_kind, sizeof(w->tamper_kind), kind);
    // Edge-only: the tamper topic is retained, so every broker reconnect
    // replays it — pushing on level would spam the log and cancel acks.
    if (active && !was)
      push_event(id, kind && kind[0] ? kind : "tamper", Sev::Tamper, false, now);
  }

  // Rooms & names (spec §8): retained securacv/<id>/meta. Deliberately
  // does NOT touch last_seen_ms: meta is human-authored (HA, companion
  // app) and retained — renaming a dead witness must not revive it.
  void on_meta(const char* id, const char* name, const char* room,
               uint32_t /*now*/) {
    Witness* w = upsert(id);
    if (!w) return;
    copy_str(w->name, sizeof(w->name), name);
    copy_str(w->room, sizeof(w->room), room);
    dirty_ = true;
  }

  // Wellbeing surface (spec §9): breathing lock riding the state topic.
  void on_wellbeing(const char* id, bool breathing, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    if (!w->wb_present || w->wb_breathing != breathing) dirty_ = true;
    w->wb_present = true;
    w->wb_breathing = breathing;
  }

  // Radar claim surface (canary-sense state topic): the coarse presence/count/
  // range vocabulary plus lux and the P1-gated vitals numerics. Stored for the
  // Canary Cards renderer (fleet_cards.h). Marks the witness as card-bearing
  // (sense_present) so a detail page renders type-aware cards instead of the
  // generic field list. Never carries raw distance or per-target data — the
  // device already coarsened those at its own privacy chokepoint.
  void on_sense_state(const char* id, const SenseState& s, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    if (!w->sense_present ||
        w->radar_presence != s.presence || w->radar_occupants != s.occupants ||
        w->radar_range != s.range || w->radar_ok != s.radar_ok ||
        w->bpm_valid != s.bpm_valid || w->breath_bpm != s.breath_bpm ||
        w->heart_bpm != s.heart_bpm) {
      dirty_ = true;
    }
    w->sense_present   = true;
    w->radar_presence  = s.presence;
    w->radar_occupants = s.occupants;
    w->radar_range     = s.range;
    w->radar_ok        = s.radar_ok;
    w->frame_errors    = (s.frame_errors > 0xFFFFu) ? 0xFFFFu
                                                    : (uint16_t)s.frame_errors;
    if (s.have_lux) w->lux = (s.lux < 0) ? -1 : (s.lux > 32767 ? 32767
                                                              : (int16_t)s.lux);
    if (s.have_bpm) w->bpm_offered = true;
    w->bpm_valid  = s.bpm_valid;
    w->breath_bpm = s.bpm_valid ? s.breath_bpm : 0;
    w->heart_bpm  = s.bpm_valid ? s.heart_bpm : 0;
  }

  // Pool water-chemistry surface (canary-pool state topic): pH / ORP / water
  // temp / TDS. Marks the witness pool-bearing (pool_present) so a detail page
  // renders chemistry cards instead of the generic field list.
  //
  // The state row is the device's CURRENT full snapshot, so this is
  // authoritative, NOT sticky: each field's have_* is overwritten every row.
  // A reading is trustworthy only under confirmed flow — when flow stops the
  // node republishes the sample as null (docs/research/pool_water_monitor.md
  // §8), so PoolState arrives with have_* cleared, and we clear the witness's
  // flag too. Rendering pH 0 / 0 mV, or a *stale* pre-pump-off pH presented as
  // current, would both read as a lie; clearing to have_*==false renders "—"
  // (honestly unknown). The stale value bytes are left in place but are gated
  // by have_*, so they can never reach the glass.
  void on_pool_state(const char* id, const PoolState& s, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    const bool changed =
        !w->pool_present ||
        w->have_ph != s.have_ph || (s.have_ph && w->ph_x10 != s.ph_x10) ||
        w->have_orp != s.have_orp || (s.have_orp && w->orp_mv != s.orp_mv) ||
        w->have_water_temp != s.have_water_temp ||
            (s.have_water_temp && w->water_temp_c10 != s.water_temp_c10) ||
        w->have_tds != s.have_tds || (s.have_tds && w->tds_ppm != s.tds_ppm);
    w->pool_present = true;
    w->have_ph = s.have_ph;                 if (s.have_ph) w->ph_x10 = s.ph_x10;
    w->have_orp = s.have_orp;               if (s.have_orp) w->orp_mv = s.orp_mv;
    w->have_water_temp = s.have_water_temp; if (s.have_water_temp)
                                              w->water_temp_c10 = s.water_temp_c10;
    w->have_tds = s.have_tds;               if (s.have_tds) w->tds_ppm = s.tds_ppm;
    if (changed) dirty_ = true;
  }

  // Chirp ingest (spec §6): an off-grid BLE advert — coarse and UNSIGNED,
  // so it feeds liveness and attention but never trust ("via chirp" names,
  // badge untouched). fp4 = 4 hex chars (the advert's 2-byte fingerprint
  // prefix); an unknown prefix becomes a pseudo witness "SCV-XXXX" so the
  // glass still shows *something* screamed in the dark.
  void on_chirp(const char fp4[5], uint8_t chirp_type, uint32_t now) {
    Witness* w = nullptr;
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (slots_[i].used && fp_suffix_match(slots_[i].fp, fp4)) { w = &slots_[i]; break; }
    }
    if (!w) {
      char pseudo[12];
      pseudo[0] = 'S'; pseudo[1] = 'C'; pseudo[2] = 'V'; pseudo[3] = '-';
      for (int i = 0; i < 4; i++) pseudo[4 + i] = fp4[i];
      pseudo[8] = '\0';
      w = upsert(pseudo);
      if (!w) return;
    }
    w->last_seen_ms = now;
    if (w->link != Link::Online) dirty_ = true;
    w->link = Link::Online;

    const char* name = nullptr;
    Sev sev = Sev::Ok;
    switch (chirp_type) {
      case 0x01: name = "alert (chirp)";  sev = Sev::Alert;  break;
      case 0x03: name = "tamper (chirp)"; sev = Sev::Tamper; break;
      default: return;  // heartbeat/witness/boot = liveness only
    }
    // Chirp timestamps are hour-coarse; dedupe locally instead — one
    // attention edge per (witness, type) per minute, so a 2 s broadcast
    // burst can't spam the log or re-cancel acks.
    if (str_eq(w->last_event, name) &&
        (int32_t)(now - w->last_event_ms) < 60000) {
      return;
    }
    copy_str(w->last_event, sizeof(w->last_event), name);
    w->last_event_ms = now;
    w->has_event = true;
    w->event_sev = sev;
    push_event(w->id, name, sev, false, now);
  }

  // Beacon ingest (fleet link): a direct BLE presence beacon from a WAP —
  // coarse and UNSIGNED, same trust rules as on_chirp (badge untouched, "via
  // ble" liveness). fp4 = 4 hex chars (the beacon's 2-byte fingerprint
  // suffix); an unknown suffix becomes a pseudo witness "SCV-XXXX", retired
  // by on_chain once a real identity claims the suffix (same as on_chirp's
  // ghosts). When have_status the beacon's battery/health/chain/mic/degraded
  // fields are stored; tamper rides the same 60 s edge-dedupe as a chirp so a
  // CONTINUOUS beacon can't spam the log or re-cancel acks.
  void on_beacon(const char fp4[5], const BeaconStatus& s, bool have_status,
                 uint32_t now, Via via = Via::Ble) {
    Witness* w = nullptr;
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (slots_[i].used && fp_suffix_match(slots_[i].fp, fp4)) { w = &slots_[i]; break; }
    }
    if (!w) {
      char pseudo[12];
      pseudo[0] = 'S'; pseudo[1] = 'C'; pseudo[2] = 'V'; pseudo[3] = '-';
      for (int i = 0; i < 4; i++) pseudo[4 + i] = fp4[i];
      pseudo[8] = '\0';
      w = upsert(pseudo);
      if (!w) return;
    }
    // Identity is the fingerprint suffix, so the lookup above already collapsed
    // every band onto one witness. A second transport carrying the same Canary
    // lands HERE, in the same slot — it refreshes liveness and credits its own
    // band, and that is the entire cost of adding a radio.
    w->last_seen_ms = now;
    w->last_beacon_ms = now;
    const int via_idx = (int)via;
    if (via_idx >= 0 && via_idx < VIA_COUNT) {
      if (!w->via_ms[via_idx]) dirty_ = true;   // a band newly carrying it
      w->via_ms[via_idx] = now;
    }
    if (!w->seen_via_ble) dirty_ = true;
    w->seen_via_ble = true;
    if (w->link != Link::Online) dirty_ = true;
    w->link = Link::Online;

    if (have_status) {
      if (s.battery_present && s.battery_pct >= 0) {
        w->battery_pct = s.battery_pct;
        w->battery_present = true;
      }
      if (s.health_present && s.health >= 0) {
        w->ble_health = (uint8_t)(s.health > 100 ? 100 : s.health);
        w->ble_health_present = true;
      }
      if (s.chain_present) {
        // Only the low 16 bits of the WAP's chain height ride the beacon; keep
        // it as a diagnostics surface (chain_length), NEVER as trust — the
        // badge stays exactly where the signed chain-verify path left it.
        w->chain_length = s.chain_seq;
      }
      w->ble_mic_muted = s.mic_muted;
      w->ble_degraded  = s.degraded;
      dirty_ = true;
    }

    // Tamper: 60 s edge-dedupe, Sev::Tamper. The window is keyed on a
    // band-independent stamp — NOT on comparing last_event to a literal, the
    // way this read before the label started naming the band. Two bands
    // carrying one tamper are one alarm.
    if (have_status && s.tamper) {
      if (w->tamper_event_seen &&
          (int32_t)(now - w->tamper_event_ms) < 60000) {
        return;
      }
      w->tamper_event_seen = true;
      w->tamper_event_ms = now;
      char name[32];
      via_label(name, sizeof(name), "tamper", via);
      copy_str(w->last_event, sizeof(w->last_event), name);
      w->last_event_ms = now;
      w->has_event = true;
      w->event_sev = Sev::Tamper;
      push_event(w->id, name, Sev::Tamper, false, now);
    }

    // Detection alert (v2 beacon, canary-vision): the witness says its
    // optical pipeline currently sees something — a class token plus a
    // confidence percentage, never anything identifying. UNSIGNED like the
    // whole channel, so it draws attention (Sev::Warn — amber glow, chime
    // tier 2) but never touches the badge. Tamper keeps precedence on the
    // event line. The advert is CONTINUOUS while presence holds, so the
    // attention edge re-arms per (witness, class) per minute — the same
    // spam/ack-cancel posture as the chirp and tamper dedupe above.
    if (have_status && s.alert && !s.tamper) {
      if (w->ble_alert_class == s.detect_class &&
          (int32_t)(now - w->ble_alert_ms) < 60000) {
        return;
      }
      w->ble_alert_ms = now;
      w->ble_alert_class = s.detect_class;

      char name[32];
      detection_label(name, sizeof(name), s.detect_class, s.detect_score, via);
      copy_str(w->last_event, sizeof(w->last_event), name);
      w->last_event_ms = now;
      w->has_event = true;
      w->event_sev = Sev::Warn;
      push_event(w->id, name, Sev::Warn, false, now);
    }
  }

  void on_chain(const char* id, uint32_t length, Badge verdict, uint32_t now,
                const char* raw = nullptr, size_t raw_len = 0,
                const char* fp = nullptr) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    if (w->badge != verdict || w->chain_length != length) dirty_ = true;
    const Badge was = w->badge;
    w->chain_length = length;
    w->badge = verdict;
    if (fp && fp[0]) {
      copy_str(w->fp, sizeof(w->fp), fp);
      // An off-grid chirp may have created a pseudo witness ("SCV-XXXX",
      // named by the fp's last-4 suffix like the canary's own BLE name)
      // for this very device before its fingerprint was known. Now that a
      // real identity owns the suffix, retire the ghost — left behind it
      // would go Lost once scanning stops and raise a false alarm.
      size_t n = 0;
      while (fp[n]) n++;
      if (n >= 4) {
        const char* s = fp + n - 4;
        const char pseudo[9] = {'S', 'C', 'V', '-', s[0], s[1], s[2], s[3],
                                '\0'};
        for (int i = 0; i < MAX_DEVICES; i++) {
          Witness& g = slots_[i];
          if (g.used && &g != w && str_eq(g.id, pseudo)) {
            g.used = false;
            dirty_ = true;
          }
        }
      }
    }
    if (raw && raw_len > 0 && raw_len < sizeof(w->chain_raw)) {
      memcpy_str(w->chain_raw, raw, raw_len);
    } else {
      // Missing/oversize payload: drop the cached proof rather than let the
      // proof sheet present a stale chain head as current state — the sheet
      // then honestly says "no signed chain to prove". (review catch)
      w->chain_raw[0] = '\0';
    }
    // Edge-only, same retained-replay reasoning as on_tamper.
    if (verdict == Badge::Failed && was != Badge::Failed) {
      push_event(id, "chain_verify_failed", Sev::Alert, false, now);
    }
  }

  // ── Per-witness mute (honest bypass) ─────────────────────────────────

  // Mute/unmute one witness until `until_ms` (millis domain — the caller
  // owns wall-clock/NVS translation). Upserts: re-applying a persisted mute
  // before the witness has spoken this boot creates its slot (link Unknown —
  // honest), rather than dropping the promise. Returns the new muted state,
  // or false when the fleet is full.
  bool set_mute(const char* id, bool muted, uint32_t until_ms) {
    Witness* w = upsert(id);
    if (!w) return false;
    w->muted = muted;
    w->mute_until_ms = until_ms;
    dirty_ = true;
    return w->muted;
  }

  static bool mute_active(const Witness& w, uint32_t now) {
    return w.muted && (int32_t)(now - w.mute_until_ms) < 0;
  }

  // ── Time ─────────────────────────────────────────────────────────────

  // Staleness deadlines. Call every loop pass; cheap.
  void tick(uint32_t now) {
    for (int i = 0; i < MAX_DEVICES; i++) {
      Witness& w = slots_[i];
      if (!w.used) continue;
      // Expired mutes clear themselves — the nag comes back by design.
      if (w.muted && (int32_t)(now - w.mute_until_ms) >= 0) {
        w.muted = false;
        dirty_ = true;
      }
      if (w.link == Link::Offline || w.link == Link::Unknown) continue;
      const int32_t silent = (int32_t)(now - w.last_seen_ms);
      Link next = Link::Online;
      if (silent >= (int32_t)limits_.lost_after_ms)       next = Link::Lost;
      else if (silent >= (int32_t)limits_.stale_after_ms) next = Link::Stale;
      if (w.link != next) {
        // Crossing into Lost is a new Alert-grade condition: it must cancel
        // any standing ack so the display re-demands attention (G5/G8) —
        // unless this witness is deliberately muted (a flaky unit someone
        // bypassed must not keep un-acking the household all night).
        // Display peers are exempt: an unplugged sibling screen must not
        // re-demand the household's attention (see is_display_peer).
        if (next == Link::Lost && !mute_active(w, now) && !is_display_peer(w))
          acked_ = false;
        w.link = next;
        dirty_ = true;
      }
    }
  }

  // ── Acknowledge (long-press) ─────────────────────────────────────────
  // Quiets the current emphasis; conditions stay visible as residual chips
  // until they actually clear. The ack itself expires after ack_hold_ms so
  // a persisting Alert re-demands attention rather than rotting silently.

  void acknowledge(uint32_t now) { acknowledge_by(now, nullptr); }

  // Attribution rides along (panels log who disarmed; we log which glass
  // quieted the house): `by` is the acking display's device_id — our own for
  // a local long-press, the sibling's for a synced ack.
  void acknowledge_by(uint32_t now, const char* by) {
    ack_ms_ = now;
    acked_ = true;
    copy_str(ack_by_, sizeof(ack_by_), by ? by : "");
    dirty_ = true;
  }

  bool ack_active(uint32_t now) const {
    return acked_ && (int32_t)(now - ack_ms_) < (int32_t)limits_.ack_hold_ms;
  }

  // Who acked (empty = unknown/legacy payload) and when, millis domain.
  const char* ack_by() const { return ack_by_; }
  uint32_t ack_at_ms() const { return ack_ms_; }

  // ── Queries (UI) ─────────────────────────────────────────────────────

  int count() const {
    int n = 0;
    for (int i = 0; i < MAX_DEVICES; i++) if (slots_[i].used) n++;
    return n;
  }

  const Witness* at(int idx) const {
    int n = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (!slots_[i].used) continue;
      if (n == idx) return &slots_[i];
      n++;
    }
    return nullptr;
  }

  // Effective severity of one witness right now (link + conditions + the
  // decaying edge event). A muted witness contributes ONLY tamper and a
  // failed chain verify: mute quiets nags (staleness, battery, events), it
  // never un-knows an attack. The UI renders the muted tag in place of the
  // suppressed state, so the bypass is always visible.
  // A display peer is a sibling SCREEN, not a life-safety sensor: it seats in
  // the roster (it has its own I2C sensor data and screen state) but its
  // link-loss must never climb the witness-lost alarm ladder — unplugging a
  // nightstand is ordinary, not an emergency. Matched on the device_type
  // prefix every display flavor reports — plus the nightlight, which
  // reports what it is ("canary-nightlight") and is every bit a sibling
  // screen: unplugging a kid's nightlight must never alarm the household.
  static bool is_display_peer(const Witness& w) {
    return strncmp(w.device_type, "canary-display", 14) == 0 ||
           strncmp(w.device_type, "canary-nightlight", 17) == 0;
  }

  Sev witness_sev(const Witness& w, uint32_t now) const {
    Sev s = Sev::Ok;
    if (w.tamper) s = worst_of(s, Sev::Tamper);
    if (w.badge == Badge::Failed) s = worst_of(s, Sev::Alert);
    if (mute_active(w, now)) return s;  // punch-through conditions only
    if (is_display_peer(w)) {
      // Show the outage (the roster row reads offline) without the alarm:
      // at most a Notice for any link state below Online.
      if (w.link == Link::Lost || w.link == Link::Offline ||
          w.link == Link::Stale) {
        s = worst_of(s, Sev::Notice);
      }
    } else if (w.link == Link::Lost) {
      s = worst_of(s, Sev::Alert);
    } else if (w.link == Link::Offline) {
      // A retained LWT is an explicit "died on the wire" — it follows the
      // same amber-then-red ladder as plain silence instead of parking at
      // Warn forever (#843 review catch). last_seen_ms was stamped when
      // the LWT arrived, so the clock starts at the drop.
      const int32_t gone = (int32_t)(now - w.last_seen_ms);
      s = worst_of(s, gone >= (int32_t)limits_.lost_after_ms ? Sev::Alert
                                                             : Sev::Warn);
    } else if (w.link == Link::Stale) {
      s = worst_of(s, Sev::Warn);
    }
    if (w.battery_present && w.battery_pct >= 0) {
      if (w.battery_pct < 10)      s = worst_of(s, Sev::Warn);
      else if (w.battery_pct < 25) s = worst_of(s, Sev::Notice);
    }
    if (w.has_event) {
      const int32_t age = (int32_t)(now - w.last_event_ms);
      Sev es = w.event_sev;
      // Informational attention decays; Alert/Tamper only clear via their
      // level-triggered condition or the fleet-wide ack residual rules.
      if (es == Sev::Notice && age >= (int32_t)limits_.notice_decay_ms) es = Sev::Ok;
      if (es == Sev::Warn   && age >= (int32_t)limits_.warn_decay_ms)   es = Sev::Ok;
      s = worst_of(s, es);
    }
    return s;
  }

  Sev worst(uint32_t now) const {
    Sev s = Sev::Ok;
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (!slots_[i].used) continue;
      s = worst_of(s, witness_sev(slots_[i], now));
    }
    return s;
  }

  // True when every tracked witness chain shows the strong tick.
  bool all_verified() const {
    bool any = false;
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (!slots_[i].used) continue;
      any = true;
      if (slots_[i].badge != Badge::Verified) return false;
    }
    return any;
  }

  int events_count() const { return ev_count_ < EVENT_CAP ? ev_count_ : EVENT_CAP; }

  // idx 0 = newest.
  const EventRow* event_at(int idx) const {
    if (idx < 0 || idx >= events_count()) return nullptr;
    const int pos = (ev_head_ - 1 - idx + 2 * EVENT_CAP) % EVENT_CAP;
    return &events_[pos];
  }

  // What the glass calls a witness: its meta name once one exists, its id
  // until then (spec §8).
  static const char* display_name(const Witness& w) {
    return w.name[0] ? w.name : w.id;
  }

  // ── Time machine v1 (spec §7): the day's story, in RAM ────────────────
  // 24 hour-of-day buckets binned at push_event time using the wall hour
  // main feeds in (unknown clock = unbinned). The SD-backed verifiable
  // journal remains the bench-phase §7 follow-up; this is the honest
  // subset: "Today: 14 events · worst was warn".

  void set_wall_hour(int hour) {
    if (hour < 0 || hour > 23) { wall_hour_ = -1; return; }
    if (wall_hour_ == -1) {
      // Coming from "clock unknown" (boot, or SNTP lost for an unknowable
      // span): the buckets' ages are unknowable too. Start the day fresh
      // rather than present stale counts as "Past 24h" — no guessed history.
      for (int h = 0; h < 24; h++) {
        hist_count_[h] = 0;
        hist_worst_[h] = (uint8_t)Sev::Ok;
      }
      wall_hour_ = hour;
      return;
    }
    if (wall_hour_ != hour) {
      // Entering a new hour reclaims that bucket (rolling 24 h window).
      hist_count_[hour] = 0;
      hist_worst_[hour] = (uint8_t)Sev::Ok;
      wall_hour_ = hour;
    }
  }

  uint8_t history_count(int hour) const {
    return (hour >= 0 && hour < 24) ? hist_count_[hour] : 0;
  }
  Sev history_worst(int hour) const {
    return (hour >= 0 && hour < 24) ? (Sev)hist_worst_[hour] : Sev::Ok;
  }
  int history_total() const {
    int t = 0;
    for (int h = 0; h < 24; h++) t += hist_count_[h];
    return t;
  }
  Sev history_worst_day() const {
    uint8_t w = (uint8_t)Sev::Ok;
    for (int h = 0; h < 24; h++) if (hist_worst_[h] > w) w = hist_worst_[h];
    return (Sev)w;
  }

  // Render-needed flag; consuming it resets it.
  bool take_dirty() { const bool d = dirty_; dirty_ = false; return d; }
  void mark_dirty() { dirty_ = true; }

  // Durable-history hook (spec §7). Fired once per pushed event with the
  // owning witness, so the firmware can build a proof-carrying JournalRecord
  // (epoch, chain_raw, verdict) and persist it — WITHOUT this template ever
  // touching Arduino/time/storage. Host tests leave it null (no-op), keeping
  // the model dependency-free and testable.
  using EventSink = void (*)(const char* id, const char* name, Sev sev,
                             bool signed_flag, uint32_t now, const Witness* w);
  void set_event_sink(EventSink s) { event_sink_ = s; }

 private:
  static Sev worst_of(Sev a, Sev b) { return (uint8_t)a >= (uint8_t)b ? a : b; }

  static void memcpy_str(char* dst, const char* src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = '\0';
  }

  static void copy_str(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
  }

  static bool str_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
  }

  // Chirp fingerprint match: the advert carries the LAST 4 hex chars of
  // the witness's 16-hex fingerprint (ble_chirp.h encodes the deviceIdHash
  // *suffix* — the same 4 chars as its "SCV-XXXX" BLE name), so compare
  // suffix-to-suffix. A short/absent fp never matches and never reads out
  // of bounds. (review catch: this originally compared the prefix, which
  // would have filed every known witness's chirps under a pseudo twin.)
  static bool fp_suffix_match(const char* fp, const char* fp4) {
    if (!fp || !fp4) return false;
    size_t n = 0;
    while (fp[n]) n++;
    if (n < 4) return false;
    const char* s = fp + n - 4;
    for (int i = 0; i < 4; i++) {
      if (!fp4[i] || s[i] != fp4[i]) return false;
    }
    return true;
  }

  // Suffix naming the band that actually carried an event. Honesty, not
  // decoration: "(wifi)" on the glass means a broker-free datagram crossed the
  // LAN, and it must never appear because a BLE advert was relabeled.
  static const char* via_suffix(Via via) {
    switch (via) {
      case Via::Wifi: return " (wifi)";
      case Via::Mesh: return " (mesh)";
      case Via::Ble:  return " (ble)";
    }
    return " (ble)";
  }

  // "<stem> (band)" — the shared tail for every beacon-sourced event label.
  //
  // Each copy measures its source first and then bounds the loop by BOTH that
  // length and the destination capacity. Testing `src[i] && i + 1 < cap` reads
  // src[i] before the capacity check has any bearing on the source, so with a
  // short literal and a roomy buffer the index can range past the literal —
  // which is exactly what cppcheck flags (arrayIndexOutOfBoundsCond). The
  // NUL always arrives first in practice, so this is a latent read rather than
  // a live bug, but "provably in range" beats "terminates for a reason the
  // analyzer can't see". Same `while (s[n]) n++;` idiom as fp_suffix_match.
  static void via_label(char* out, size_t cap, const char* stem, Via via) {
    if (!out || cap == 0) return;
    size_t stem_len = 0;
    while (stem[stem_len]) stem_len++;
    const char* tail = via_suffix(via);
    size_t tail_len = 0;
    while (tail[tail_len]) tail_len++;

    size_t i = 0;
    for (size_t s = 0; s < stem_len && i + 1 < cap; s++) out[i++] = stem[s];
    for (size_t t = 0; t < tail_len && i + 1 < cap; t++) out[i++] = tail[t];
    out[i] = '\0';
  }

  // Event label for a beacon detection alert: "person 87% (ble)" / "person 87%
  // (wifi)" for the same sighting on a different band, falling back to
  // "person (ble)" when the confidence is unknown and "detection (ble)" for a
  // class token this build hasn't met (degrade, don't crash — same posture as
  // classify_event). Pure by hand — this header stays free of <cstdio>.
  static void detection_label(char* out, size_t cap, uint8_t detect_class,
                              int score, Via via = Via::Ble) {
    const char* cls = "detection";
    switch (detect_class) {
      case 1: cls = "person";  break;
      case 2: cls = "vehicle"; break;
      case 3: cls = "animal";  break;
      case 4: cls = "package"; break;
      default: break;
    }
    size_t cls_len = 0;
    while (cls[cls_len]) cls_len++;
    size_t i = 0;
    for (size_t c = 0; c < cls_len && i + 1 < cap; c++) out[i++] = cls[c];
    if (score >= 0 && score <= 100 && i + 6 < cap) {  // " 100%" worst case
      out[i++] = ' ';
      if (score == 100) { out[i++] = '1'; out[i++] = '0'; out[i++] = '0'; }
      else {
        if (score >= 10) out[i++] = (char)('0' + score / 10);
        out[i++] = (char)('0' + score % 10);
      }
      out[i++] = '%';
    }
    // Same bounded-copy shape as via_label above, for the same reason.
    const char* tail = via_suffix(via);
    size_t tail_len = 0;
    while (tail[tail_len]) tail_len++;
    for (size_t t = 0; t < tail_len && i + 1 < cap; t++) out[i++] = tail[t];
    out[i] = '\0';
  }

  Witness* find(const char* id) {
    if (!id || !id[0]) return nullptr;
    for (int i = 0; i < MAX_DEVICES; i++)
      if (slots_[i].used && str_eq(slots_[i].id, id)) return &slots_[i];
    return nullptr;
  }

  Witness* upsert(const char* id) {
    if (!id || !id[0]) return nullptr;
    for (int i = 0; i < MAX_DEVICES; i++)
      if (slots_[i].used && str_eq(slots_[i].id, id)) return &slots_[i];
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (slots_[i].used) continue;
      slots_[i] = Witness{};
      slots_[i].used = true;
      copy_str(slots_[i].id, sizeof(slots_[i].id), id);
      dirty_ = true;
      return &slots_[i];
    }
    return nullptr;  // fleet full — over-cap devices are dropped, not rotated
  }

  void push_event(const char* id, const char* name, Sev sev, bool signed_flag,
                  uint32_t now) {
    // Time machine v1: bin into the current wall hour when the clock is known.
    if (wall_hour_ >= 0) {
      if (hist_count_[wall_hour_] < 255) hist_count_[wall_hour_]++;
      if ((uint8_t)sev > hist_worst_[wall_hour_]) hist_worst_[wall_hour_] = (uint8_t)sev;
    }
    EventRow& r = events_[ev_head_];
    // The log speaks the witness's friendly name once meta gave it one.
    const Witness* w = nullptr;
    for (int i = 0; i < MAX_DEVICES; i++)
      if (slots_[i].used && str_eq(slots_[i].id, id)) { w = &slots_[i]; break; }
    copy_str(r.device, sizeof(r.device), w ? display_name(*w) : id);
    copy_str(r.name, sizeof(r.name), name);
    r.sev = sev;
    r.at_ms = now;
    r.signed_flag = signed_flag;
    ev_head_ = (ev_head_ + 1) % EVENT_CAP;
    if (ev_count_ < EVENT_CAP) ev_count_++;
    // A newly ingested Alert/Tamper cancels a standing ack: the ack covered
    // what was known when the user pressed, not whatever comes next. This
    // is what lets a fresh 3 a.m. tamper punch through the night floor even
    // inside the ack-hold window (G5/G8; #843 review catch). Callers only
    // push on edges (see on_tamper/on_chain), so retained replays at broker
    // reconnect cannot cancel acks. Exception: a muted witness's ALERT does
    // not re-wake the house (that's what mute is for) — but its TAMPER
    // still does, matching the witness_sev punch-through rule.
    if ((uint8_t)sev >= (uint8_t)Sev::Alert) {
      const bool muted_alert = w && mute_active(*w, now) && sev == Sev::Alert;
      if (!muted_alert) acked_ = false;
    }
    // Durable history (spec §7): hand the event + its owning witness to the
    // journal sink, if the firmware wired one. w may be null for an event on
    // a device that isn't in the fleet slots (shouldn't happen post-upsert,
    // but the sink tolerates it).
    if (event_sink_) event_sink_(id, name, sev, signed_flag, now, w);
    dirty_ = true;
  }

  EventSink event_sink_ = nullptr;
  Witness  slots_[MAX_DEVICES] = {};
  char     ack_by_[24] = {0};
  EventRow events_[EVENT_CAP] = {};
  int      wall_hour_ = -1;
  uint8_t  hist_count_[24] = {0};
  uint8_t  hist_worst_[24] = {0};
  int      ev_head_ = 0;
  int      ev_count_ = 0;
  bool     dirty_ = true;
  bool     acked_ = false;
  uint32_t ack_ms_ = 0;
  FleetLimits limits_{};
};

}  // namespace canary::fleet
