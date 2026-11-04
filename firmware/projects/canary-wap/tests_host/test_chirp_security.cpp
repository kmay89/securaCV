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
//     those thousand for repeats. Since sweep F309 a frame's nonce goes in
//     only once its signatures verify: a thousand frames that do not
//     (unsigned, badly signed, or a relay whose origin envelope fails)
//     leave every byte of the filter, and so its false-positive rate, as it
//     was, and the witness is still refused, by the filter alone.
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
//   - c9_a_forged_nonce_does_not_block_the_chirp, c9_a_refused_frame_counts_once,
//     c9_a_stale_frame_counts_once_a_filter_period (sweep F309): a frame
//     carrying a heard chirp's nonce that fails its signature checks, heard
//     first, no longer keeps the chirp out; one that verifies under any key,
//     an attacker's own or a relay naming the origin with no origin
//     signature (v0.2's soft accept), still does (known limits, pinned);
//     and a verified frame a later check refuses counts against its key's
//     hour once while the filter holds its nonce (331 to 662 s), where
//     played back once a filter period it counts each time (a known limit,
//     pinned; its own item).
//   - c14_two_keys_cannot_take_turns_after_the_wrap,
//     c14_no_key_passes_its_hour_across_the_wrap,
//     c14_the_oldest_live_window_goes_first_across_the_wrap,
//     c14_an_idle_row_does_not_read_young_after_2_32_ms,
//     c14_a_live_hour_goes_only_to_a_full_table,
//     c8_the_oldest_row_goes_first_across_the_wrap (sweep F416): the
//     per-key rate table and the recent table pick the row to give up by
//     age, unsigned elapsed time, and a rate row whose hour has run out is
//     dropped at the next loop pass, so uptime's 49.7-day wrap no longer
//     lets two keys take turns past four an hour, nor puts the newest
//     chirps first out of a full table, nor lets a row nothing touched for
//     2^32 ms read as minutes old. All but the last two fail on the code
//     before F416; an_idle_row fails on F416's first cut (the age alone,
//     no prune), and a_live_hour pins what 32 rows cannot do, with or
//     without the wrap.
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

// `signer`'s witness frame: the header `h` and the payload `p` as given, with
// `signer`'s key carried and its signature over them, as send_chirp() (hop
// 0) or relay_chirp() (hop 1 and on) signs one.
std::vector<uint8_t> signed_by(const Neighbor& signer, const cc::ChirpHeader& h,
                               cc::ChirpWitnessPayload p) {
  memcpy(p.session_pubkey, signer.pub, sizeof p.session_pubkey);
  uint8_t canonical[256];
  const size_t cl = cc::build_witness_canonical(&h, &p, signer.pub, canonical, sizeof canonical);
  CHECK(cl > 0);
  Ed25519::sign(p.signature, signer.priv, signer.pub, canonical, cl);
  return frame_of(h, p);
}
cc::ChirpWitnessPayload payload_of(cc::ChirpTemplate tpl = cc::TPL_INFRA_POWER_OUT,
                                   cc::ChirpUrgency urg = cc::CHIRP_URG_CAUTION) {
  cc::ChirpWitnessPayload p;
  memset(&p, 0, sizeof p);
  p.template_id = (uint8_t)tpl;
  p.detail_slot = (uint8_t)cc::DETAIL_NONE;
  p.urgency = (uint8_t)urg;
  p.ttl_minutes = 15;
  return p;
}
// `relayer`'s hop-1 relay of the witness `w`: its own signature on top, and
// the origin envelope `w`'s signer signed, or `origin_sig` in its place.
std::vector<uint8_t> relay_of(const Neighbor& relayer, const std::vector<uint8_t>& w,
                              const uint8_t* origin_sig = nullptr) {
  cc::ChirpHeader h;
  cc::ChirpWitnessPayload p;
  memcpy(&h, w.data(), sizeof h);
  memcpy(&p, w.data() + sizeof h, sizeof p);
  memcpy(h.session_id, relayer.sid, sizeof h.session_id);
  h.hop_count = 1;
  memcpy(p.signed_origin_pubkey, p.session_pubkey, sizeof p.signed_origin_pubkey);
  memcpy(p.signed_origin_signature, origin_sig != nullptr ? origin_sig : p.signature,
         sizeof p.signed_origin_signature);
  return signed_by(relayer, h, p);
}
// Three forgeries carrying `nonce` in `origin`'s session, none of which
// verifies whole: bytes signed by nobody (the carried key is not the
// header's session id); the right key with a signature that does not
// verify; and a relay whose own signature verifies, under `relayer`'s key,
// around an origin envelope that does not (top-level passes, origin fails).
std::vector<std::vector<uint8_t>> forgeries(const Neighbor& origin, const Neighbor& relayer,
                                            const uint8_t nonce[8], std::mt19937* rng) {
  cc::ChirpHeader h = header_of(origin, cc::CHIRP_MSG_WITNESS, 0);
  memcpy(h.nonce, nonce, 8);
  cc::ChirpWitnessPayload junk;
  memset(&junk, 0xEE, sizeof junk);
  cc::ChirpWitnessPayload bad_sig = payload_of();
  memcpy(bad_sig.session_pubkey, origin.pub, sizeof bad_sig.session_pubkey);
  for (uint8_t& b : bad_sig.signature) b = (uint8_t)(*rng)();
  uint8_t bad_origin[64];
  for (uint8_t& b : bad_origin) b = (uint8_t)(*rng)();
  bad_origin[0] |= 1;   // not the all-zero envelope a v0.2 relay may carry
  const std::vector<uint8_t> genuine = signed_by(origin, h, payload_of());
  return {frame_of(h, junk), frame_of(h, bad_sig), relay_of(relayer, genuine, bad_origin)};
}
// The nonce filter's bytes, both halves.
std::vector<uint8_t> filter_bytes() {
  return std::vector<uint8_t>(cc::g_bloom, cc::g_bloom + 2 * cc::BLOOM_BYTES);
}
// Of `probes` random nonces from `seed`, none of them heard, how many the
// filter reads as repeats: its false-positive count on that sample.
size_t false_repeats(uint32_t seed, int probes) {
  std::mt19937 rng(seed);
  size_t n = 0;
  for (int i = 0; i < probes; ++i) {
    uint8_t nonce[8];
    for (uint8_t& b : nonce) b = (uint8_t)rng();
    n += cc::is_nonce_seen(nonce) ? 1 : 0;
  }
  return n;
}
void drop_all_rows() {
  memset(cc::g_recent_chirps, 0, cc::MAX_RECENT_CHIRPS * sizeof(cc::ReceivedChirp));
  cc::g_recent_chirp_count = 0;
}

// C9 (sweep F309): a flood of frames that do not verify leaves the nonce
// filter as it was. The filter took a witness frame's nonce before it
// checked the session id or a signature, so any device in range filled it
// with made-up frames: a half's false-positive rate is about 0.02 % at 1,000
// nonces and 2.4 % at 4,096 (spec §5.3), each a new witness dropped as a
// repeat. Now only a frame whose signatures verify puts its nonce in. A
// thousand forgeries, a third of each kind forgeries() builds, change no
// byte of the filter, so its false-positive rate on a sample of 4,096
// nonces is what it was before them. And what the filter is for still holds: the real chirp,
// heard again while its row is held, and again with the recent table
// emptied so that only the filter holds it, is refused, and so is a
// neighbor's relay of it; a new witness from the same key is stored.
void test_c9_bloom_flood() {
  boot_enabled();
  const Neighbor origin = neighbor_of(0x16);
  const Neighbor relayer = neighbor_of(0x17);
  const std::vector<uint8_t> target = witness_of(origin, 0x22);
  deliver(origin, target);
  CHECK(holds(target));
  deliver(origin, target);                      // heard again while its row is held
  CHECK(cc::g_recent_chirp_count == 1);
  drop_all_rows();

  const std::vector<uint8_t> before = filter_bytes();
  const size_t fp_before = false_repeats(7, 4096);
  std::mt19937 rng(42);
  for (int i = 0; i < 1000; ++i) {
    uint8_t nonce[8];
    for (uint8_t& b : nonce) b = (uint8_t)rng();
    const std::vector<std::vector<uint8_t>> fs = forgeries(origin, relayer, nonce, &rng);
    deliver(i % 3 == 2 ? relayer : origin, fs[(size_t)(i % 3)]);
  }
  CHECK(cc::g_recent_chirp_count == 0);         // no forgery was stored
  CHECK(filter_bytes() == before);              // and none put its nonce in
  CHECK(false_repeats(7, 4096) == fp_before);   // so the false-positive rate is unchanged

  deliver(origin, target);
  CHECK(cc::g_recent_chirp_count == 0);         // the target: still refused, by the filter
  deliver(relayer, relay_of(relayer, target));
  CHECK(cc::g_recent_chirp_count == 0);         // and a relay of it
  deliver(origin, witness_of(origin, 0x23));
  CHECK(cc::g_recent_chirp_count == 1);
}

// C9 (F309): a forgery carrying a heard chirp's nonce does not keep that
// chirp out. A receiver the chirp has not reached yet hears, first, a frame
// with its nonce that does not verify, of each kind forgeries() builds; the
// forgery is refused, and the chirp, when it comes, is stored as its origin
// sent it. With the nonce taken before the checks, the forgery's nonce went
// in and the chirp was refused as a repeat.
//
// What the filter cannot tell apart, and this case pins as known limits:
// it holds nonces, not who signed them, and so does the recent table, so a
// frame that verifies under any key still takes the nonce it carries. An
// attacker who signs its own witness with the heard nonce is stored, under
// its key, and the chirp after it is refused; that costs the attacker a
// signature and a row under a key its hour counts. And a relay that names
// the origin's key and carries no origin signature at all is soft-accepted
// on its relayer's signature (handle_witness(), the v0.2 limitation;
// THREAT_MODEL's Chirp "Spoofed origination" cell): it is stored as the
// origin's chirp, with content the origin never signed, and the real chirp
// after it is refused. Both are the same with the filter before or after
// the checks. A relay whose origin signature does not verify is refused
// (the third of forgeries()), as is one naming no origin key.
void test_c9_a_forged_nonce_does_not_block_the_chirp() {
  const Neighbor origin = neighbor_of(0x18);
  const Neighbor relayer = neighbor_of(0x19);
  std::mt19937 rng(309);
  for (size_t kind = 0; kind < 3; ++kind) {
    boot_enabled();
    const std::vector<uint8_t> real = witness_of(origin, 0x24, cc::TPL_EMERG_FIRE_VISIBLE,
                                                 cc::CHIRP_URG_URGENT);
    uint8_t nonce[8];
    nonce_of(real, nonce);
    const std::vector<std::vector<uint8_t>> fs = forgeries(origin, relayer, nonce, &rng);
    deliver(kind == 2 ? relayer : origin, fs[kind]);
    CHECK(cc::g_recent_chirp_count == 0);
    const bool forgery_took_the_nonce = cc::is_nonce_seen(nonce);
    deliver(origin, real);
    cc::ReceivedChirp* c = row_of(nonce);
    CHECK(c != nullptr);                     // the chirp is stored
    CHECK(!forgery_took_the_nonce);
    CHECK(memcmp(c->sender_pubkey, origin.pub, 32) == 0);
    CHECK(c->template_id == cc::TPL_EMERG_FIRE_VISIBLE && c->urgency == cc::CHIRP_URG_URGENT);
  }

  // The first limit: the attacker's own signed witness with the heard nonce.
  boot_enabled();
  const Neighbor attacker = neighbor_of(0x1A);
  const std::vector<uint8_t> real = witness_of(origin, 0x25);
  uint8_t nonce[8];
  nonce_of(real, nonce);
  cc::ChirpHeader h = header_of(attacker, cc::CHIRP_MSG_WITNESS, 0);
  memcpy(h.nonce, nonce, 8);
  deliver(attacker, signed_by(attacker, h, payload_of(cc::TPL_CLR_FALSE_ALARM)));
  cc::ReceivedChirp* c = row_of(nonce);
  CHECK(c != nullptr && memcmp(c->sender_pubkey, attacker.pub, 32) == 0);
  deliver(origin, real);
  CHECK(cc::g_recent_chirp_count == 1);
  CHECK(memcmp(row_of(nonce)->sender_pubkey, attacker.pub, 32) == 0);

  // The second: a hop-1 relay of the heard fire chirp, re-signed by the
  // attacker, naming the origin's key with an all-zero origin signature and
  // reading "false alarm".
  boot_enabled();
  const std::vector<uint8_t> fire =
      witness_of(origin, 0x2B, cc::TPL_EMERG_FIRE_VISIBLE, cc::CHIRP_URG_URGENT);
  nonce_of(fire, nonce);
  cc::ChirpHeader rh;
  cc::ChirpWitnessPayload rp;
  memcpy(&rh, fire.data(), sizeof rh);
  memcpy(&rp, fire.data() + sizeof rh, sizeof rp);
  memcpy(rh.session_id, attacker.sid, sizeof rh.session_id);
  rh.hop_count = 1;
  rp.template_id = (uint8_t)cc::TPL_CLR_FALSE_ALARM;
  memcpy(rp.signed_origin_pubkey, origin.pub, sizeof rp.signed_origin_pubkey);
  memset(rp.signed_origin_signature, 0, sizeof rp.signed_origin_signature);
  g_health.clear();
  deliver(attacker, signed_by(attacker, rh, rp));
  CHECK(logged("chirp: relayed witness lacks origin signature (v0.2 limitation)"));
  c = row_of(nonce);
  CHECK(c != nullptr && memcmp(c->sender_pubkey, origin.pub, 32) == 0);   // stored as the origin's
  CHECK(c->template_id == cc::TPL_CLR_FALSE_ALARM);                       // saying what it never said
  deliver(origin, fire);
  CHECK(cc::g_recent_chirp_count == 1);
  CHECK(row_of(nonce)->template_id == cc::TPL_CLR_FALSE_ALARM);
}

// C9 (F309): where the nonce goes in. A verified frame that a later check
// refuses (here the freshness window: its timestamp 400 s behind this
// device's clock, a capture played back) is refused at the lookup when it
// comes again while the filter holds its nonce, as before F309, so five
// plays within that time reach the rate limit once: it counts against its
// key's hour once in a filter period, not at every replay, and the key's
// next three witnesses are stored and its fourth refused. Had the nonce
// gone in only with the row, five replays of the one frame would have used
// the key's hour, and the key's own chirps after them been refused. What
// the filter's memory does not cover is the next case.
void test_c9_a_refused_frame_counts_once() {
  boot_enabled();
  const Neighbor origin = neighbor_of(0x1B);
  cc::ChirpHeader h = header_of(origin, cc::CHIRP_MSG_WITNESS, 0x26);
  h.timestamp = (uint32_t)host_sim::wall_now - 400;
  const std::vector<uint8_t> stale = signed_by(origin, h, payload_of());
  g_health.clear();
  for (int i = 0; i < 5; ++i) deliver(origin, stale);
  CHECK(std::count(g_health.begin(), g_health.end(),
                   std::string("chirp: rejected witness \xE2\x80\x94 outside freshness window")) == 1);
  CHECK(cc::g_recent_chirp_count == 0);
  for (uint8_t b = 0x27; b < 0x2A; ++b) {
    deliver(origin, witness_of(origin, b));
    CHECK(holds(witness_of(origin, b)));
  }
  g_health.clear();
  deliver(origin, witness_of(origin, 0x2A));
  CHECK(!holds(witness_of(origin, 0x2A)));
  CHECK(logged("chirp: rate-limited witness from pubkey"));
}

// C9 (F309's review): a known limit, pinned. The filter holds a nonce for
// 331 to 662 s (spec §5.3), and the rate limit runs before the freshness
// window (handle_witness()), so one captured stale frame played back once a
// filter period reaches the rate limit each time: four plays 11.1 min apart
// use up its key's hour, and the key's own fresh witness after them is
// refused. The same on the code before F309. Running the freshness window
// first would close it; that is its own item, not this one.
void test_c9_a_stale_frame_counts_once_a_filter_period() {
  boot_enabled();
  const Neighbor origin = neighbor_of(0x1C);
  cc::ChirpHeader h = header_of(origin, cc::CHIRP_MSG_WITNESS, 0x2C);
  h.timestamp = (uint32_t)host_sim::wall_now - 400;
  const std::vector<uint8_t> stale = signed_by(origin, h, payload_of());
  int reached_the_window = 0;
  for (int play = 0; play < (int)cc::MAX_WITNESSES_PER_PUBKEY_PER_HOUR; ++play) {
    g_health.clear();
    deliver(origin, stale);
    reached_the_window += logged("chirp: rejected witness \xE2\x80\x94 outside freshness window") ? 1 : 0;
    CHECK(cc::g_recent_chirp_count == 0);
    // 11.1 min on, past the 662 s the filter can hold a nonce; the wall
    // clock stays, so the frame stays 400 s stale.
    for (int m = 0; m < 11; ++m) { host_sim::now_ms += 60000; cc::update(); }
    host_sim::now_ms += 6000;
    cc::update();
    CHECK(!cc::is_nonce_seen(h.nonce));
  }
  g_health.clear();
  const std::vector<uint8_t> fresh = witness_of(origin, 0x2D);
  deliver(origin, fresh);
  const bool rate_limited = logged("chirp: rate-limited witness from pubkey");
  std::printf("  one stale frame played %d times, 11.1 min apart: %d reached the freshness window; "
              "the key's own fresh witness then %s\n", (int)cc::MAX_WITNESSES_PER_PUBKEY_PER_HOUR,
              reached_the_window, rate_limited ? "rate-limited" : "stored");
  CHECK(reached_the_window == (int)cc::MAX_WITNESSES_PER_PUBKEY_PER_HOUR);
  CHECK(!holds(fresh));
  CHECK(rate_limited);
}

// C8 (sweep F416): the oldest row goes first across uptime's wrap. Sixteen
// info chirps a second apart, the first eight before millis() wraps and the
// last eight after it: the next info chirp evicts the first one stored, and
// the one after that the second. The tie between rows of one urgency went
// to the smallest received_ms, and a row stored before the wrap holds a
// larger value than one stored after it, so the ninth chirp, the first
// after the wrap, went first, while the eight older ones stayed.
void test_c8_the_oldest_row_goes_first_across_the_wrap() {
  host_sim::now_ms = (uint32_t)(0u - 3600000u - 8500u);   // boot_enabled() adds the hour
  boot_enabled();
  std::vector<std::vector<uint8_t>> info;
  fill_with(cc::TPL_AID_OFFERING_HELP, cc::CHIRP_URG_INFO, 0x30, 0x40, &info);
  CHECK(cc::g_recent_chirps[7].received_ms > cc::g_recent_chirps[8].received_ms);   // the wrap
  CHECK(host_sim::now_ms < 10000u);
  const Neighbor late = neighbor_of(0x39);
  host_sim::now_ms += 1000;
  const std::vector<uint8_t> next = witness_of(late, 0x61, cc::TPL_AID_OFFERING_HELP, cc::CHIRP_URG_INFO);
  deliver(late, next);
  CHECK(holds(next));
  CHECK(!holds(info[0]));
  for (size_t i = 1; i < info.size(); ++i) CHECK(holds(info[i]));
  host_sim::now_ms += 1000;
  const std::vector<uint8_t> after = witness_of(late, 0x62, cc::TPL_AID_OFFERING_HELP, cc::CHIRP_URG_INFO);
  deliver(late, after);
  CHECK(holds(after) && holds(next));
  CHECK(!holds(info[1]));
  for (size_t i = 2; i < info.size(); ++i) CHECK(holds(info[i]));
}

// ── C14: a key's witnesses, four an hour (sweep F416) ──────────────────────

// `n`'s signed witness with the nonce that `serial` names (a key's
// witnesses here outnumber the 256 nonce_byte spells).
std::vector<uint8_t> witness_no(const Neighbor& n, uint32_t serial) {
  cc::ChirpHeader h = header_of(n, cc::CHIRP_MSG_WITNESS, 0);
  h.nonce[1] = 0xF4;
  memcpy(h.nonce + 2, &serial, 4);
  h.nonce[6] = 0x16;
  h.nonce[7] = 0x41;
  return signed_by(n, h, payload_of());
}
// Whether the witness `w` from `n` was taken: the log says it was accepted.
bool accepted(const Neighbor& n, const std::vector<uint8_t>& w) {
  g_health.clear();
  deliver(n, w);
  for (const std::string& line : g_health) {
    if (line.rfind("chirp: witness accepted", 0) == 0) return true;
  }
  return false;
}
// The rate table's row for `n`'s key, or nullptr.
const cc::PubkeyRateEntry* rate_row_of(const Neighbor& n) {
  for (size_t i = 0; i < cc::MAX_PUBKEY_RATE_TRACK; ++i) {
    const cc::PubkeyRateEntry& e = cc::g_pubkey_rate[i];
    if (e.valid && memcmp(e.pubkey, n.pub, 32) == 0) return &e;
  }
  return nullptr;
}
size_t rate_rows_held() {
  size_t n = 0;
  for (size_t i = 0; i < cc::MAX_PUBKEY_RATE_TRACK; ++i) n += cc::g_pubkey_rate[i].valid ? 1 : 0;
  return n;
}
// The receiver boots so that its uptime reads `at`, and MAX_PUBKEY_RATE_TRACK
// keys (seeds from 0x40) each send it one witness then: a full rate table.
void boot_with_a_full_rate_table(uint32_t at, uint32_t serial_base) {
  host_sim::now_ms = at - 3600000u;   // boot_enabled() adds the hour
  boot_enabled();
  CHECK(host_sim::now_ms == at);
  for (size_t i = 0; i < cc::MAX_PUBKEY_RATE_TRACK; ++i) {
    const Neighbor k = neighbor_of((uint8_t)(0x40 + i));
    CHECK(accepted(k, witness_no(k, serial_base)));
  }
  CHECK(rate_rows_held() == cc::MAX_PUBKEY_RATE_TRACK);
}

// The review's probe of F416: a full table written 2 h before millis()
// wraps, then keys A and B taking turns, a second apart, twelve witnesses
// each. A day after the wrap and 30 days after it, each is taken 4 times of
// 12, MAX_WITNESSES_PER_PUBKEY_PER_HOUR, as with no wrap (the control:
// the table written on day 10, the turns on day 11); with a loop pass
// between the table and the turns, which drops the 32 rows whose hour has
// run out (prune_expired_rate_rows()), and with none, where the turns take
// those rows as the oldest (that scene first: it is the one the old table
// failed). The rate table evicted the row with the
// smallest start time, and after the wrap that is the row written since
// it: A's row went for B's, B's for A's, each witness was its key's first,
// and A was taken 12 of 12 while 31 of the 32 rows still held keys from
// before the wrap (nothing but init() dropped a row then). Each key now
// takes a row whose hour has run out, or a free one, and keeps it.
void test_c14_two_keys_cannot_take_turns_after_the_wrap() {
  const uint32_t before_wrap = (uint32_t)(0u - 2u * 3600000u);
  const uint32_t DAY = 24u * 3600000u;
  struct Scene { uint32_t table_at; uint32_t turns_at; bool pass; };
  const Scene scenes[] = {
      {before_wrap, DAY, false},        {before_wrap, DAY, true},         // a day after the wrap
      {before_wrap, 30u * DAY, false},  {before_wrap, 30u * DAY, true},   // 30 days after it
      {10u * DAY, 11u * DAY, false},    {10u * DAY, 11u * DAY, true},     // the control: no wrap
  };
  const Neighbor a = neighbor_of(0x60);
  const Neighbor b = neighbor_of(0x61);
  for (const Scene& s : scenes) {
    boot_with_a_full_rate_table(s.table_at, 0);
    host_sim::now_ms = s.turns_at;
    if (s.pass) {
      cc::update();                               // the pass drops the 32 run-out rows
      CHECK(rate_rows_held() == 0);
    }
    int took_a = 0, took_b = 0;
    for (uint32_t i = 1; i <= 12; ++i) {
      took_a += accepted(a, witness_no(a, i)) ? 1 : 0;
      took_b += accepted(b, witness_no(b, i)) ? 1 : 0;
      host_sim::now_ms += 1000;
    }
    std::printf("  turns at %u ms, %s: A took %d of 12, B %d of 12\n", (unsigned)s.turns_at,
                s.pass ? "after a pass" : "with no pass", took_a, took_b);
    CHECK(took_a == cc::MAX_WITNESSES_PER_PUBKEY_PER_HOUR);
    CHECK(took_b == cc::MAX_WITNESSES_PER_PUBKEY_PER_HOUR);
    CHECK(rate_row_of(a) != nullptr && rate_row_of(b) != nullptr);
    CHECK(rate_rows_held() == (s.pass ? 2u : cc::MAX_PUBKEY_RATE_TRACK));
  }
}

// The lens on F416 over three hours: 31 rows held by keys an attacker sent
// 50 min before, a legitimate key L that used its four in the minute
// before, and three keys the attacker rotates, a witness every 10 s round
// the three for three hours, with the loop's pass once a minute. Run where
// the three hours lie wholly before the wrap, through it, where the 31 rows
// are from before it and L and the rotation after it, a day after it and 30
// days after it. In every run the rotation's first three witnesses take
// the three oldest rows, the attacker's first three, and never L's; no key
// is taken more than MAX_WITNESSES_PER_PUBKEY_PER_HOUR times in any hour
// (each sliding 3,600 s); each rotating key is taken that many in its
// first hour; and L's fifth witness in its hour is refused, its window
// start unmoved. With the rows from before the wrap and L's from after it,
// the old table gave up L's row, the smallest start time, then each
// rotating key's row for the next key's, and every witness of the three
// hours was its key's first.
void test_c14_no_key_passes_its_hour_across_the_wrap() {
  const uint32_t HOUR = 3600000u, MIN = 60000u;
  const uint32_t DAY = 24u * HOUR;
  const uint32_t WRAP = 0u;
  struct Scene { uint32_t rotation_at; const char* name; };
  const Scene scenes[] = {
      {(uint32_t)(WRAP - 5u * HOUR), "before the wrap"},
      {(uint32_t)(WRAP - 30u * MIN), "through the wrap"},
      {WRAP + 2u * MIN, "the rows before the wrap, L after it"},
      {WRAP + DAY, "a day after the wrap"},
      {WRAP + 30u * DAY, "30 days after the wrap"},
  };
  const Neighbor legit = neighbor_of(0x70);
  const Neighbor rot[3] = {neighbor_of(0x71), neighbor_of(0x72), neighbor_of(0x73)};
  for (const Scene& s : scenes) {
    // 31 attacker rows 50 min before the rotation, L's four a minute before it.
    host_sim::now_ms = s.rotation_at - 50u * MIN - HOUR;   // boot_enabled() adds the hour
    boot_enabled();
    for (size_t i = 0; i + 1 < cc::MAX_PUBKEY_RATE_TRACK; ++i) {
      const Neighbor k = neighbor_of((uint8_t)(0x40 + i));
      CHECK(accepted(k, witness_no(k, 0)));
    }
    host_sim::now_ms = s.rotation_at - MIN;
    cc::update();
    for (uint32_t i = 1; i <= cc::MAX_WITNESSES_PER_PUBKEY_PER_HOUR; ++i) {
      CHECK(accepted(legit, witness_no(legit, i)));
    }
    CHECK(rate_rows_held() == cc::MAX_PUBKEY_RATE_TRACK);
    const uint32_t legit_start = rate_row_of(legit)->window_start_ms;

    std::vector<uint32_t> taken[3];   // ms since the rotation began
    uint32_t serial = 100;
    for (uint32_t t = 0; t < 3u * HOUR; t += 10000u) {
      host_sim::now_ms = s.rotation_at + t;
      if (t % MIN == 0) cc::update();                    // the loop task's pass, once a minute
      const size_t k = (t / 10000u) % 3;
      if (accepted(rot[k], witness_no(rot[k], serial++))) taken[k].push_back(t);
      if (t == 20000u) {                                 // three rows went: the attacker's first three
        const bool legit_kept = rate_row_of(legit) != nullptr;
        std::printf("  %s: after the rotation's first three, L's row %s\n", s.name,
                    legit_kept ? "kept" : "EVICTED");
        CHECK(legit_kept && rate_row_of(legit)->window_start_ms == legit_start);
        for (size_t i = 0; i < 3; ++i) CHECK(rate_row_of(neighbor_of((uint8_t)(0x40 + i))) == nullptr);
        CHECK(rate_row_of(neighbor_of(0x43)) != nullptr);
        for (size_t i = 0; i < 3; ++i) CHECK(rate_row_of(rot[i]) != nullptr);
      }
      if (t == 50u * MIN) {                              // L, inside its hour: refused, its row kept
        CHECK(!accepted(legit, witness_no(legit, 99)));
        CHECK(rate_row_of(legit) != nullptr && rate_row_of(legit)->window_start_ms == legit_start);
      }
    }
    for (size_t k = 0; k < 3; ++k) {
      const std::vector<uint32_t>& ts = taken[k];
      size_t first_hour = 0;
      for (uint32_t t : ts) first_hour += t < HOUR ? 1 : 0;
      CHECK(first_hour == cc::MAX_WITNESSES_PER_PUBKEY_PER_HOUR);
      for (size_t i = 0; i < ts.size(); ++i) {
        size_t in_hour = 0;
        for (size_t j = i; j < ts.size() && ts[j] - ts[i] < HOUR; ++j) ++in_hour;
        CHECK(in_hour <= cc::MAX_WITNESSES_PER_PUBKEY_PER_HOUR);
      }
    }
    std::printf("  %s: the three rotating keys taken %zu, %zu and %zu times in 3 h; L kept its hour\n",
                s.name, taken[0].size(), taken[1].size(), taken[2].size());
  }
}

// The same in one step: 31 live rows written 50 min before millis() wraps,
// a legitimate key L whose four witnesses come a minute after it, and one
// new key K a minute later. K takes the row whose window started longest
// ago, the first attacker row, and L's row, its hour live and full, stays:
// L's fifth witness is refused. The table evicted the smallest start time,
// and L's row, written after the wrap, held the smallest of all: K took
// L's row, and L's fifth witness was its first again. The control runs the
// same minutes on day 10.
void test_c14_the_oldest_live_window_goes_first_across_the_wrap() {
  const uint32_t MIN = 60000u;
  const uint32_t wrap = 0u, control = 10u * 24u * 3600000u;
  for (const uint32_t at : {wrap, control}) {
    host_sim::now_ms = at - 50u * MIN - 3600000u;   // boot_enabled() adds the hour
    boot_enabled();
    CHECK(host_sim::now_ms == at - 50u * MIN);
    for (size_t i = 0; i + 1 < cc::MAX_PUBKEY_RATE_TRACK; ++i) {
      const Neighbor k = neighbor_of((uint8_t)(0x40 + i));
      CHECK(accepted(k, witness_no(k, 0)));
    }
    host_sim::now_ms = at + MIN;
    cc::update();                                    // a pass: every row is live, none goes
    CHECK(rate_rows_held() == cc::MAX_PUBKEY_RATE_TRACK - 1);
    const Neighbor legit = neighbor_of(0x70);
    for (uint32_t i = 1; i <= cc::MAX_WITNESSES_PER_PUBKEY_PER_HOUR; ++i) {
      CHECK(accepted(legit, witness_no(legit, i)));
    }
    CHECK(!accepted(legit, witness_no(legit, 5)));
    CHECK(rate_rows_held() == cc::MAX_PUBKEY_RATE_TRACK);
    host_sim::now_ms = at + 2u * MIN;
    cc::update();
    const Neighbor k = neighbor_of(0x74);
    CHECK(accepted(k, witness_no(k, 0)));
    const bool legit_kept = rate_row_of(legit) != nullptr;
    std::printf("  %s: one new key into a full live table, L's row %s\n",
                at == wrap ? "the rows before the wrap, L after it" : "the control", legit_kept ? "kept" : "EVICTED");
    CHECK(legit_kept);
    CHECK(rate_row_of(neighbor_of(0x40)) == nullptr);   // the oldest live window went
    for (size_t i = 1; i + 1 < cc::MAX_PUBKEY_RATE_TRACK; ++i) {
      CHECK(rate_row_of(neighbor_of((uint8_t)(0x40 + i))) != nullptr);
    }
    CHECK(!accepted(legit, witness_no(legit, 6)));     // and L's count stays
  }
}

// F416's review: a row nothing touched for 2^32 ms read as minutes old. Its
// age is unsigned elapsed time, so a row left in the table that long read
// younger than a live window, and one new key then evicted the live window
// instead of it. 31 rows written at P and left alone, L's four witnesses 30
// min before P + 2^32, and one new key K 5 min after it: the loop's pass
// drops a row the moment its hour has run out (prune_expired_rate_rows()),
// so K takes a free row and L's row, live and full, stays. With the rows
// kept (nothing but init() dropped one), K read them as 5 min old and L's
// as 35, took L's row, and L's fifth witness in its hour was taken.
void test_c14_an_idle_row_does_not_read_young_after_2_32_ms() {
  const uint32_t MIN = 60000u;
  const uint32_t P = 7u * 24u * 3600000u;
  host_sim::now_ms = P - 3600000u;   // boot_enabled() adds the hour
  boot_enabled();
  for (size_t i = 0; i + 1 < cc::MAX_PUBKEY_RATE_TRACK; ++i) {
    const Neighbor k = neighbor_of((uint8_t)(0x40 + i));
    CHECK(accepted(k, witness_no(k, 0)));
  }
  host_sim::now_ms = P + 3600000u + 1;   // the loop runs meanwhile: a pass just past the hour
  cc::update();
  host_sim::now_ms = P - 30u * MIN;      // 2^32 - 30 min after P
  cc::update();
  const Neighbor legit = neighbor_of(0x70);
  for (uint32_t i = 1; i <= cc::MAX_WITNESSES_PER_PUBKEY_PER_HOUR; ++i) {
    CHECK(accepted(legit, witness_no(legit, i)));
  }
  CHECK(!accepted(legit, witness_no(legit, 5)));
  host_sim::now_ms = P + 5u * MIN;
  cc::update();
  const Neighbor k = neighbor_of(0x74);
  CHECK(accepted(k, witness_no(k, 0)));
  const bool legit_kept = rate_row_of(legit) != nullptr;
  std::printf("  31 rows idle for 2^32 ms, then one new key: L's row %s, %zu rows held\n",
              legit_kept ? "kept" : "EVICTED", rate_rows_held());
  CHECK(legit_kept);
  CHECK(!accepted(legit, witness_no(legit, 6)));
  CHECK(rate_rows_held() == 2);          // L's and K's: the idle rows went at the pass
}

// What the table cannot do, and the threat model says (the Chirp Sybil
// cell): 32 rows hold 32 keys' hours. A live hour is evicted only when every
// row is live, so a key keeps its count until 32 other keys start an hour
// within its own; the 32nd evicts the oldest live row, here L's, and L's
// next witness is its first again. One device makes any number of session
// keys, so this is the per-key limit's price, with or without the wrap.
void test_c14_a_live_hour_goes_only_to_a_full_table() {
  host_sim::now_ms = 5u * 24u * 3600000u;
  boot_enabled();
  const Neighbor legit = neighbor_of(0x70);
  for (uint32_t i = 1; i <= cc::MAX_WITNESSES_PER_PUBKEY_PER_HOUR; ++i) {
    CHECK(accepted(legit, witness_no(legit, i)));
  }
  for (size_t i = 0; i + 1 < cc::MAX_PUBKEY_RATE_TRACK; ++i) {
    host_sim::now_ms += 1000;
    const Neighbor k = neighbor_of((uint8_t)(0x40 + i));
    CHECK(accepted(k, witness_no(k, 0)));
    CHECK(rate_row_of(legit) != nullptr);
  }
  CHECK(rate_rows_held() == cc::MAX_PUBKEY_RATE_TRACK);
  CHECK(!accepted(legit, witness_no(legit, 5)));     // its hour, full
  host_sim::now_ms += 1000;
  const Neighbor k32 = neighbor_of(0x5F);
  CHECK(accepted(k32, witness_no(k32, 0)));
  CHECK(rate_row_of(legit) == nullptr);              // the oldest live row went
  CHECK(accepted(legit, witness_no(legit, 6)));      // and L's count with it
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
    {"c8_the_oldest_row_goes_first_across_the_wrap", test_c8_the_oldest_row_goes_first_across_the_wrap},
    {"c9_bloom_flood", test_c9_bloom_flood},
    {"c9_a_forged_nonce_does_not_block_the_chirp", test_c9_a_forged_nonce_does_not_block_the_chirp},
    {"c9_a_refused_frame_counts_once", test_c9_a_refused_frame_counts_once},
    {"c9_a_stale_frame_counts_once_a_filter_period", test_c9_a_stale_frame_counts_once_a_filter_period},
    {"c14_two_keys_cannot_take_turns_after_the_wrap", test_c14_two_keys_cannot_take_turns_after_the_wrap},
    {"c14_no_key_passes_its_hour_across_the_wrap", test_c14_no_key_passes_its_hour_across_the_wrap},
    {"c14_the_oldest_live_window_goes_first_across_the_wrap", test_c14_the_oldest_live_window_goes_first_across_the_wrap},
    {"c14_an_idle_row_does_not_read_young_after_2_32_ms", test_c14_an_idle_row_does_not_read_young_after_2_32_ms},
    {"c14_a_live_hour_goes_only_to_a_full_table", test_c14_a_live_hour_goes_only_to_a_full_table},
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
