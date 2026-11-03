// Host test: Chirp v0.2's security model (audit C2-C13,
// docs/audit/mesh_and_chirp_audit_v1.md), held on the REAL chirp_channel.cpp
// (chirp_host_harness.h) with real signed frames: what a receiver counts,
// keeps and refuses.
//
// This file re-implemented the set tracking, the priority insert and the
// Bloom filter in plain C++ and tested the copies, saying drift from the
// firmware was caught at code review; its suppress set held 8 keys long after
// the firmware's held 17 (F233), and one case compared two literals (sweep
// F271). Each case, and what became of it:
//   - c2_c3_no_self_count: replaced. A received witness starts with no
//     confirmations (none come off the wire, C2); its originator's own
//     signed confirmation, from a key that holds a nearby row, counts for
//     nothing (C3); another key's validates it and relays it. The copy fed
//     the originator's key into a set and showed the set took it.
//   - c5_ack_dedup_by_pubkey: replaced. One key confirming twice, in two
//     frames, counts once and does not validate a chirp that needs two; a
//     key with no nearby row counts for nothing (C13); a second key
//     validates it.
//   - c7_suppress_dedup: retired. Its copy held an 8-key set the firmware no
//     longer has; test_chirp_suppress_wap.cpp's a_repeated_voter_counts_once
//     drives repeated votes, new frames and the same bytes, through the real
//     handle_suppress_vote() at 7 and 32 rows (sweep F233).
//   - c8_priority_storage_eviction: replaced. A full table of info chirps
//     takes an urgent one by evicting its oldest info chirp, a later info
//     chirp evicts the next oldest info chirp and never the urgent one, info
//     chirps keep evicting info rows once the urgent row is the oldest, and
//     a table full of urgent chirps refuses an info one.
//   - c9_bloom_flood: replaced. A witness's nonce is still refused after a
//     thousand more nonces reached the real filter, which took very few of
//     those thousand for repeats.
//   - c11_emoji_size_lifted: retired. It compared two literals;
//     test_chirp_commands_wap.cpp's every_session_display_is_five_emoji_in_its_field
//     renders every session display through the real generate_emoji_string()
//     into the real field (sweep F231).
//
// Added since:
//   - c13_an_ack_waits_for_ten_minutes_of_presence (sweep F311): spec §3.4
//     rule 4 counts a confirmer seen in the nearby table for at least
//     PRESENCE_REQUIRED_MS, and the check asked only for a row, so a key
//     first heard a second before its ACK counted. Heard 9 minutes, and 10
//     minutes less 1 ms, it is refused; at 10 minutes it counts.
//   - c13_presence_counts_from_the_row (F311): the ten minutes start when the
//     row is put in the table and nothing else moves them. A refresh does
//     not; a row pruned after NEARBY_TIMEOUT_MS of silence and heard again,
//     a new session key, and a row put in a slot an older session's row left
//     behind (a disable and an enable) each start their own; and uptime's
//     wrap neither shortens nor stalls them.
//   - c4_a_relay_keeps_the_signed_ttl (sweep F306): relay_chirp() wrote
//     ttl_minutes 15 over the byte the origin signature covers, so a chirp
//     sent with any other TTL was refused one hop on ("bad origin
//     signature"). A chirp sent with 30, 0 or 255 is relayed with that byte
//     over two hops and stored at each receiver, and lives there as long as
//     any row does (DEFAULT_DISPLAY_MS): nothing on the receive side reads
//     the TTL as a display time.
//
// Host-tested only: the stubs stand in for the radio and the clock; the
// Arduino compile is CI's.
//
// Run: ./test_chirp_security [name]

#include "chirp_host_harness.h"

#include <algorithm>
#include <random>

namespace chirp_security {

using namespace chirp_host;

bool logged(const char* line) {
  return std::find(g_health.begin(), g_health.end(), std::string(line)) != g_health.end();
}

// The relays this device sent since `from`: witness frames at hop 1 or more.
size_t relays_since(size_t from) {
  size_t n = 0;
  for (const std::vector<uint8_t>& f : sent_of_type(from, cc::CHIRP_MSG_WITNESS)) {
    if (f[offsetof(cc::ChirpHeader, hop_count)] > 0) ++n;
  }
  return n;
}

// C2, C3: an emergency chirp, which one confirmation validates (spec §3.4).
void test_c2_c3_no_self_count() {
  boot_enabled();
  const Neighbor origin = neighbor_of(0x10);
  const Neighbor other = neighbor_of(0x11);
  deliver(origin, presence_of(origin));   // the originator holds a row, as a neighbor would
  deliver(other, presence_of(other));
  stay_present({origin, other});          // ten minutes of presence (spec §3.4 rule 4)
  const std::vector<uint8_t> w =
      witness_of(origin, 0x20, cc::TPL_EMERG_FIRE_VISIBLE, cc::CHIRP_URG_URGENT);
  uint8_t nonce[8];
  nonce_of(w, nonce);
  deliver(origin, w);
  cc::ReceivedChirp* c = row_of(nonce);
  CHECK(c != nullptr);
  CHECK(c->confirm_count == 0 && !c->validated && !c->relayed);   // C2

  const size_t mark = host_sim::espnow->sent.size();
  g_health.clear();
  deliver(origin, confirm_of(origin, nonce, 0x71));               // C3: its own
  deliver(origin, confirm_of(origin, nonce, 0x72));
  CHECK(c->confirm_count == 0 && !c->validated && !c->relayed);
  CHECK(relays_since(mark) == 0);
  CHECK(logged("chirp: rejected ACK \xE2\x80\x94 confirmer == originator"));

  deliver(other, confirm_of(other, nonce));
  CHECK(c->confirm_count == 1 && c->validated && c->relayed);
  CHECK(relays_since(mark) == 1);
}

// C5, C13: an infrastructure chirp, which two confirmations validate.
void test_c5_ack_dedup_by_pubkey() {
  boot_enabled();
  const Neighbor origin = neighbor_of(0x12);
  const Neighbor a = neighbor_of(0x13);
  const Neighbor b = neighbor_of(0x14);
  const Neighbor drive_by = neighbor_of(0x15);   // no presence row
  deliver(a, presence_of(a));
  deliver(b, presence_of(b));
  stay_present({a, b});
  const std::vector<uint8_t> w = witness_of(origin, 0x21);
  uint8_t nonce[8];
  nonce_of(w, nonce);
  deliver(origin, w);
  cc::ReceivedChirp* c = row_of(nonce);
  CHECK(c != nullptr);
  const size_t mark = host_sim::espnow->sent.size();

  const std::vector<uint8_t> first = confirm_of(a, nonce, 0x71);
  deliver(a, first);
  deliver(a, confirm_of(a, nonce, 0x72));   // the same key, a new frame
  deliver(a, first);                        // and the same bytes again
  CHECK(c->confirm_count == 1 && !c->validated);
  g_health.clear();
  deliver(drive_by, confirm_of(drive_by, nonce));
  CHECK(c->confirm_count == 1 && !c->validated);
  CHECK(logged("chirp: rejected ACK \xE2\x80\x94 confirmer lacks presence"));
  CHECK(relays_since(mark) == 0);

  deliver(b, confirm_of(b, nonce));
  CHECK(c->confirm_count == 2 && c->validated && c->relayed);
  CHECK(relays_since(mark) == 1);
}

// C8: the recent table (16 rows) keeps urgent chirps through a flood of info
// ones. The originators send four each (the per-key limit is four an hour,
// C14), a second apart, so "oldest" is well defined.
void fill_with(cc::ChirpTemplate tpl, cc::ChirpUrgency urg, uint8_t first_seed, uint8_t nonce_base,
               std::vector<std::vector<uint8_t>>* frames) {
  for (size_t i = 0; i < cc::MAX_RECENT_CHIRPS; ++i) {
    const Neighbor n = neighbor_of((uint8_t)(first_seed + i / 4));
    host_sim::now_ms += 1000;
    const std::vector<uint8_t> w = witness_of(n, (uint8_t)(nonce_base + i), tpl, urg);
    deliver(n, w);
    frames->push_back(w);
  }
  CHECK(cc::g_recent_chirp_count == cc::MAX_RECENT_CHIRPS);
}
bool holds(const std::vector<uint8_t>& w) {
  uint8_t nonce[8];
  nonce_of(w, nonce);
  return row_of(nonce) != nullptr;
}

void test_c8_priority_storage_eviction() {
  boot_enabled();
  std::vector<std::vector<uint8_t>> info;
  fill_with(cc::TPL_AID_OFFERING_HELP, cc::CHIRP_URG_INFO, 0x30, 0x40, &info);
  for (const auto& w : info) CHECK(holds(w));

  host_sim::now_ms += 1000;
  const Neighbor urgent_origin = neighbor_of(0x38);
  const std::vector<uint8_t> urgent =
      witness_of(urgent_origin, 0x60, cc::TPL_EMERG_FIRE_VISIBLE, cc::CHIRP_URG_URGENT);
  deliver(urgent_origin, urgent);
  CHECK(holds(urgent));
  CHECK(!holds(info[0]));                     // the oldest info chirp went
  for (size_t i = 1; i < info.size(); ++i) CHECK(holds(info[i]));
  CHECK(cc::g_recent_chirp_count == cc::MAX_RECENT_CHIRPS);

  host_sim::now_ms += 1000;
  const Neighbor late = neighbor_of(0x39);
  const std::vector<uint8_t> later_info =
      witness_of(late, 0x61, cc::TPL_AID_OFFERING_HELP, cc::CHIRP_URG_INFO);
  deliver(late, later_info);
  CHECK(holds(later_info) && holds(urgent));  // the next oldest info chirp went, not the urgent one
  CHECK(!holds(info[1]));

  // Sixteen more info chirps: each evicts the oldest info row. From the
  // fifteenth on the urgent row is the oldest in the table, so eviction of
  // the oldest row behind the urgency gate would refuse those two; here the
  // urgent row stays, the sixteenth evicts the first of the sixteen, and the
  // other fifteen are held.
  std::vector<std::vector<uint8_t>> more;
  fill_with(cc::TPL_AID_OFFERING_HELP, cc::CHIRP_URG_INFO, 0x50, 0x90, &more);
  CHECK(holds(urgent));
  CHECK(!holds(later_info));
  CHECK(!holds(more[0]));
  for (size_t i = 1; i < more.size(); ++i) CHECK(holds(more[i]));

  // A table of urgent chirps refuses an info one.
  boot_enabled();
  std::vector<std::vector<uint8_t>> urgents;
  fill_with(cc::TPL_EMERG_FIRE_VISIBLE, cc::CHIRP_URG_URGENT, 0x40, 0x70, &urgents);
  host_sim::now_ms += 1000;
  const std::vector<uint8_t> lowly = witness_of(late, 0x62, cc::TPL_AID_OFFERING_HELP, cc::CHIRP_URG_INFO);
  deliver(late, lowly);
  CHECK(!holds(lowly));
  for (const auto& w : urgents) CHECK(holds(w));
}

// C9: a witness's nonce survives a flood. The filter takes a witness frame's
// nonce before it checks the frame, so a thousand made-up frames from any
// device in range reach it. Spec §5.3 puts a half's false-positive rate near
// 0.02 % at 1,000 nonces; at most 5 of these 1,000 lookups may come back as
// repeats, and the target must be refused after them, with the recent table
// emptied so that only the filter holds it.
void test_c9_bloom_flood() {
  boot_enabled();
  const Neighbor origin = neighbor_of(0x16);
  const std::vector<uint8_t> target = witness_of(origin, 0x22);
  deliver(origin, target);
  CHECK(holds(target));
  memset(cc::g_recent_chirps, 0, cc::MAX_RECENT_CHIRPS * sizeof(cc::ReceivedChirp));
  cc::g_recent_chirp_count = 0;

  std::mt19937 rng(42);
  int false_repeats = 0;
  for (int i = 0; i < 1000; ++i) {
    cc::ChirpHeader h = header_of(origin, cc::CHIRP_MSG_WITNESS, 0);
    for (uint8_t& b : h.nonce) b = (uint8_t)rng();
    if (cc::is_nonce_seen(h.nonce)) ++false_repeats;
    cc::ChirpWitnessPayload junk;
    memset(&junk, 0xEE, sizeof junk);   // signed by nobody
    deliver(origin, frame_of(h, junk));
  }
  CHECK(false_repeats <= 5);
  CHECK(cc::g_recent_chirp_count == 0);   // no junk frame was stored
  deliver(origin, target);
  CHECK(cc::g_recent_chirp_count == 0);   // the target: still refused
  // And a new witness from the same origin is stored: the flood did not
  // fill the filter.
  deliver(origin, witness_of(origin, 0x23));
  CHECK(cc::g_recent_chirp_count == 1);
}

// C13 (sweep F311): an ACK counts from a key whose nearby row is at least
// PRESENCE_REQUIRED_MS old (spec §3.4 rule 4), and the key beacons once a
// minute meanwhile, as a canary-wap does. An emergency chirp, which one
// confirmation validates, so a counted ACK shows as a relay.
void test_c13_an_ack_waits_for_ten_minutes_of_presence() {
  boot_enabled();
  const Neighbor origin = neighbor_of(0x17);
  const Neighbor a = neighbor_of(0x18);
  deliver(a, presence_of(a));                  // a's row, first seen now
  stay_present({a}, 9);
  const std::vector<uint8_t> w =
      witness_of(origin, 0x24, cc::TPL_EMERG_FIRE_VISIBLE, cc::CHIRP_URG_URGENT);
  uint8_t nonce[8];
  nonce_of(w, nonce);
  deliver(origin, w);
  cc::ReceivedChirp* c = row_of(nonce);
  CHECK(c != nullptr);
  const size_t mark = host_sim::espnow->sent.size();

  g_health.clear();
  deliver(a, confirm_of(a, nonce, 0x71));      // heard 9 minutes: refused
  CHECK(c->confirm_count == 0 && !c->validated && !c->relayed);
  CHECK(logged("chirp: rejected ACK \xE2\x80\x94 confirmer lacks presence"));
  host_sim::now_ms += 60000 - 1;               // 10 minutes less 1 ms, a beacon just heard
  deliver(a, presence_of(a));
  deliver(a, confirm_of(a, nonce, 0x72));
  CHECK(c->confirm_count == 0 && !c->validated && !c->relayed);
  CHECK(relays_since(mark) == 0);

  host_sim::now_ms += 1;                       // 10 minutes: it counts
  deliver(a, confirm_of(a, nonce, 0x73));
  CHECK(c->confirm_count == 1 && c->validated && c->relayed);
  CHECK(relays_since(mark) == 1);
}

// The nearby row of `n`, or nullptr.
cc::NearbyDevice* nearby_row_of(const Neighbor& n) {
  for (size_t i = 0; i < cc::g_nearby_count; ++i) {
    if (memcmp(cc::g_nearby_devices[i].session_id, n.sid, cc::SESSION_ID_SIZE) == 0) {
      return &cc::g_nearby_devices[i];
    }
  }
  return nullptr;
}
// An infrastructure chirp from `origin` (two confirmations validate one),
// stored here; its nonce in `nonce`.
cc::ReceivedChirp* chirp_from(const Neighbor& origin, uint8_t nonce_byte, uint8_t nonce[8]) {
  const std::vector<uint8_t> w = witness_of(origin, nonce_byte);
  nonce_of(w, nonce);
  deliver(origin, w);
  cc::ReceivedChirp* c = row_of(nonce);
  CHECK(c != nullptr && c->confirm_count == 0);
  return c;
}

// C13 (F311): the ten minutes count from the row's insert, and only that.
void test_c13_presence_counts_from_the_row() {
  // A refresh never moves first_seen_ms: nine of them, a minute apart.
  boot_enabled();
  const Neighbor a = neighbor_of(0x19);
  deliver(a, presence_of(a));
  const uint32_t first = host_sim::now_ms;
  for (int m = 1; m <= 9; ++m) {
    stay_present({a}, 1);
    const cc::NearbyDevice* row = nearby_row_of(a);
    CHECK(row != nullptr && row->first_seen_ms == first && row->last_seen_ms == host_sim::now_ms);
  }

  // Pruned and heard again: a new row, ten minutes of its own. a was heard
  // for 9 minutes, then fell silent until the prune took its row (no beacon
  // for NEARBY_TIMEOUT_MS, 3 minutes); heard again 12 minutes after it was
  // first, it is refused, and counts ten minutes after its return.
  for (int m = 0; m < 4 && nearby_row_of(a) != nullptr; ++m) stay_present({}, 1);
  CHECK(nearby_row_of(a) == nullptr);
  deliver(a, presence_of(a));
  CHECK(nearby_row_of(a) != nullptr && nearby_row_of(a)->first_seen_ms == host_sim::now_ms);
  CHECK(host_sim::now_ms - first >= cc::PRESENCE_REQUIRED_MS);
  const Neighbor origin = neighbor_of(0x1A);
  uint8_t nonce[8];
  cc::ReceivedChirp* c = chirp_from(origin, 0x25, nonce);
  deliver(a, confirm_of(a, nonce, 0x71));
  CHECK(c->confirm_count == 0);
  stay_present({a}, 9);
  deliver(a, confirm_of(a, nonce, 0x72));
  CHECK(c->confirm_count == 0);
  stay_present({a}, 1);
  deliver(a, confirm_of(a, nonce, 0x73));
  CHECK(c->confirm_count == 1);

  // A new session id is a new key with its own ten minutes: a2, a's next
  // session, beacons from the minute a's row qualified, and its ACK is
  // refused until its own row is ten minutes old, while a's counts.
  boot_enabled();
  const Neighbor b = neighbor_of(0x1B);
  const Neighbor b2 = neighbor_of(0x1C);
  deliver(b, presence_of(b));
  stay_present({b}, 10);
  deliver(b2, presence_of(b2));
  c = chirp_from(origin, 0x26, nonce);
  deliver(b2, confirm_of(b2, nonce, 0x71));
  CHECK(c->confirm_count == 0);
  deliver(b, confirm_of(b, nonce, 0x72));
  CHECK(c->confirm_count == 1 && !c->validated);
  stay_present({b, b2}, 9);
  deliver(b2, confirm_of(b2, nonce, 0x73));
  CHECK(c->confirm_count == 1 && !c->validated);
  stay_present({b, b2}, 1);
  deliver(b2, confirm_of(b2, nonce, 0x74));
  CHECK(c->confirm_count == 2 && c->validated);

  // An older session's row in the slot: this device's disable empties the
  // table, and the slot keeps the old row's bytes (a first_seen_ms ten
  // minutes old among them). The next row put there starts now, and its key
  // is refused; b, heard again after the enable, starts again too.
  cc::disable();
  CHECK(cc::g_nearby_count == 0);
  CHECK(host_sim::now_ms - cc::g_nearby_devices[0].first_seen_ms >= cc::PRESENCE_REQUIRED_MS);
  CHECK(cc::enable());
  const Neighbor d = neighbor_of(0x1D);
  deliver(d, presence_of(d));
  deliver(b, presence_of(b));
  CHECK(nearby_row_of(d) == &cc::g_nearby_devices[0]);
  CHECK(nearby_row_of(d)->first_seen_ms == host_sim::now_ms);
  CHECK(nearby_row_of(b)->first_seen_ms == host_sim::now_ms);
  c = chirp_from(origin, 0x27, nonce);
  deliver(d, confirm_of(d, nonce, 0x71));
  deliver(b, confirm_of(b, nonce, 0x72));
  CHECK(c->confirm_count == 0);
  stay_present({b, d}, 10);
  deliver(d, confirm_of(d, nonce, 0x73));
  deliver(b, confirm_of(b, nonce, 0x74));
  CHECK(c->confirm_count == 2 && c->validated);

  // Across uptime's wrap: first heard 5 minutes before millis() wraps, so
  // the moment it qualifies (first_seen_ms + 10 minutes) wraps to 5 minutes.
  // Refused a minute on, before the wrap (a reading of the uptime against
  // that wrapped moment would count it there); refused 9 minutes on, 4 after
  // the wrap, and 10 minutes less 1 ms on; counted at 10.
  host_sim::now_ms = (uint32_t)(0u - 3600000u - 5u * 60000u);   // boot_enabled() adds the hour
  boot_enabled();
  CHECK(host_sim::now_ms == (uint32_t)(0u - 5u * 60000u));
  const Neighbor e = neighbor_of(0x1E);
  deliver(e, presence_of(e));
  stay_present({e}, 1);
  c = chirp_from(origin, 0x28, nonce);
  deliver(e, confirm_of(e, nonce, 0x71));
  CHECK(c->confirm_count == 0);
  stay_present({e}, 8);
  CHECK(host_sim::now_ms == 4u * 60000u);
  deliver(e, confirm_of(e, nonce, 0x72));
  CHECK(c->confirm_count == 0);
  host_sim::now_ms += 60000 - 1;
  deliver(e, confirm_of(e, nonce, 0x73));
  CHECK(c->confirm_count == 0);
  host_sim::now_ms += 1;
  deliver(e, confirm_of(e, nonce, 0x74));
  CHECK(c->confirm_count == 1);

  // And across 2^31 ms (24.9 days), where a signed reading of uptime turns
  // negative: first heard 15 minutes before it, the key still counts 20
  // minutes on, 5 minutes past it.
  host_sim::now_ms = (uint32_t)(0x80000000u - 3600000u - 15u * 60000u);
  boot_enabled();
  const Neighbor f = neighbor_of(0x1F);
  deliver(f, presence_of(f));
  stay_present({f}, 20);
  CHECK(host_sim::now_ms == 0x80000000u + 5u * 60000u);
  c = chirp_from(origin, 0x2D, nonce);
  deliver(f, confirm_of(f, nonce, 0x71));
  CHECK(c->confirm_count == 1);
}

// C13 (F311, F232): what the ten minutes cost. Presence beacons are
// unsigned, so one radio beacons any number of session ids at once, and
// their ten minutes run together: eight keys first heard in the same minute
// are all refused at nine minutes and all count at ten, so eight keys cost
// ten minutes, as one does (docs/FLEET_SEMANTICS.md §6, §8). The table's
// free rows bound it: with MAX_NEARBY_CACHE rows held a 33rd key gets no row
// (no eviction when full), and its ACK counts at no age.
void test_c13_the_ten_minutes_run_together_for_every_key() {
  boot_enabled();
  std::vector<Neighbor> keys;
  for (uint8_t i = 0; i < 8; ++i) keys.push_back(neighbor_of((uint8_t)(0x30 + i)));
  for (const Neighbor& k : keys) deliver(k, presence_of(k));
  const uint32_t t0 = host_sim::now_ms;
  stay_present(keys, 9);
  const Neighbor origin = neighbor_of(0x2F);
  uint8_t nonce[8];
  cc::ReceivedChirp* c = chirp_from(origin, 0x29, nonce);
  uint8_t fb = 0x71;
  for (const Neighbor& k : keys) deliver(k, confirm_of(k, nonce, fb++));
  CHECK(c->confirm_count == 0 && !c->validated);
  stay_present(keys, 1);
  CHECK(host_sim::now_ms - t0 == cc::PRESENCE_REQUIRED_MS);
  for (const Neighbor& k : keys) deliver(k, confirm_of(k, nonce, fb++));
  CHECK(c->confirm_count == keys.size() && c->validated);

  boot_enabled();
  std::vector<Neighbor> full;
  for (size_t i = 0; i < cc::MAX_NEARBY_CACHE + 1; ++i) full.push_back(neighbor_of((uint8_t)(0x40 + i)));
  for (const Neighbor& k : full) deliver(k, presence_of(k));
  CHECK(cc::g_nearby_count == cc::MAX_NEARBY_CACHE);
  stay_present(full, 20);
  const Neighbor& late = full.back();
  CHECK(nearby_row_of(late) == nullptr);
  c = chirp_from(origin, 0x2A, nonce);
  deliver(late, confirm_of(late, nonce, 0x71));
  CHECK(c->confirm_count == 0);
  deliver(full.front(), confirm_of(full.front(), nonce, 0x72));
  CHECK(c->confirm_count == 1);
}

// The relays this device sent since `from`, whole.
std::vector<std::vector<uint8_t>> relay_frames_since(size_t from) {
  std::vector<std::vector<uint8_t>> out;
  for (const std::vector<uint8_t>& f : sent_of_type(from, cc::CHIRP_MSG_WITNESS)) {
    if (f[offsetof(cc::ChirpHeader, hop_count)] > 0) out.push_back(f);
  }
  return out;
}
// A relay's hop count, its TTL byte, and whether its signed_origin envelope
// verifies with the origin's key over the canonical the relay carries.
struct RelaySeen {
  uint8_t hop;
  uint8_t ttl;
  bool origin_verifies;
};
RelaySeen read_relay(const std::vector<uint8_t>& f, const Neighbor& origin) {
  cc::ChirpHeader h;
  cc::ChirpWitnessPayload p;
  CHECK(f.size() == sizeof h + sizeof p);
  memcpy(&h, f.data(), sizeof h);
  memcpy(&p, f.data() + sizeof h, sizeof p);
  CHECK(memcmp(p.signed_origin_pubkey, origin.pub, 32) == 0);
  uint8_t canonical[256];
  const size_t cl = cc::build_witness_canonical(&h, &p, origin.pub, canonical, sizeof canonical);
  CHECK(cl > 0);
  return {h.hop_count, p.ttl_minutes,
          Ed25519::verify(p.signed_origin_signature, origin.pub, canonical, cl)};
}

// C4 (sweep F306): a relay carries the TTL its origin signed. Three
// receivers, each a device that boots with its channel on: A hears the
// origin's emergency chirp and a neighbor present ten minutes confirms it,
// so A relays it (hop 1); B stores A's relay, a neighbor confirms it there,
// and B relays it on (hop 2); C stores B's relay. 15 is the control, the
// TTL relay_chirp() wrote whatever the chirp said; with 30, 0 or 255, B
// refused A's relay ("bad origin signature"). And C keeps the row exactly
// as long as any row, DEFAULT_DISPLAY_MS, whatever the TTL: a TTL of 0 is
// not dropped at the next prune, and 255 does not outlive the 30 minutes.
void test_c4_a_relay_keeps_the_signed_ttl() {
  for (const uint8_t ttl : {15, 30, 0, 255}) {
    const Neighbor origin = neighbor_of(0x2A);
    const Neighbor ca = neighbor_of(0x2B);     // confirms at A
    const Neighbor cb = neighbor_of(0x2C);     // confirms at B

    boot_enabled();                            // A
    deliver(ca, presence_of(ca));
    stay_present({ca});
    const std::vector<uint8_t> w =
        witness_of(origin, 0x29, cc::TPL_EMERG_FIRE_VISIBLE, cc::CHIRP_URG_URGENT, ttl);
    const uint32_t sent_at = (uint32_t)host_sim::wall_now;
    uint8_t nonce[8];
    nonce_of(w, nonce);
    deliver(origin, w);
    CHECK(row_of(nonce) != nullptr);
    size_t mark = host_sim::espnow->sent.size();
    deliver(ca, confirm_of(ca, nonce));
    std::vector<std::vector<uint8_t>> relays = relay_frames_since(mark);
    CHECK(relays.size() == 1);
    const std::vector<uint8_t> r1 = relays[0];
    RelaySeen seen = read_relay(r1, origin);
    CHECK(seen.hop == 1);
    CHECK(seen.origin_verifies);               // the byte the origin signed...
    CHECK(seen.ttl == ttl);                    // ...is the one the relay carries

    boot_enabled();                            // B: the same minute on its clock
    deliver(cb, presence_of(cb));
    stay_present({cb});
    CHECK((uint32_t)host_sim::wall_now == sent_at);
    g_health.clear();
    deliver(ca, r1);                           // A's relay, heard at B
    CHECK(!logged("chirp: rejected relayed witness \xE2\x80\x94 bad origin signature"));
    cc::ReceivedChirp* c = row_of(nonce);
    CHECK(c != nullptr && c->hop_count == 1);
    CHECK(memcmp(c->sender_pubkey, origin.pub, 32) == 0 && c->has_origin_signature);
    mark = host_sim::espnow->sent.size();
    deliver(cb, confirm_of(cb, nonce));
    relays = relay_frames_since(mark);
    CHECK(relays.size() == 1);
    const std::vector<uint8_t> r2 = relays[0];
    seen = read_relay(r2, origin);
    CHECK(seen.hop == 2 && seen.origin_verifies && seen.ttl == ttl);   // B kept the byte

    boot_enabled();                            // C
    host_sim::wall_now = sent_at;
    deliver(cb, r2);
    c = row_of(nonce);
    CHECK(c != nullptr && c->hop_count == 2);
    const uint32_t stored_at = c->received_ms;
    host_sim::now_ms = stored_at + cc::DEFAULT_DISPLAY_MS - 1;
    cc::update();                              // a prune pass (the last ran at the boot)
    CHECK(row_of(nonce) != nullptr);
    host_sim::now_ms = stored_at + cc::DEFAULT_DISPLAY_MS + 30001;
    cc::update();
    CHECK(row_of(nonce) == nullptr);
  }
}

const Test kTests[] = {
    {"c2_c3_no_self_count", test_c2_c3_no_self_count},
    {"c5_ack_dedup_by_pubkey", test_c5_ack_dedup_by_pubkey},
    {"c8_priority_storage_eviction", test_c8_priority_storage_eviction},
    {"c9_bloom_flood", test_c9_bloom_flood},
    {"c13_an_ack_waits_for_ten_minutes_of_presence", test_c13_an_ack_waits_for_ten_minutes_of_presence},
    {"c13_presence_counts_from_the_row", test_c13_presence_counts_from_the_row},
    {"c13_the_ten_minutes_run_together_for_every_key", test_c13_the_ten_minutes_run_together_for_every_key},
    {"c4_a_relay_keeps_the_signed_ttl", test_c4_a_relay_keeps_the_signed_ttl},
};

}  // namespace chirp_security

int main(int argc, char** argv) {
  using namespace chirp_security;
  return run_tests(kTests, sizeof kTests / sizeof kTests[0], "chirp security", argc, argv);
}
