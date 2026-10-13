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
//     chirp evicts the next oldest info chirp and never the urgent one, and
//     a table full of urgent chirps refuses an info one.
//   - c9_bloom_flood: replaced. A witness's nonce is still refused after a
//     thousand more nonces reached the real filter, which took very few of
//     those thousand for repeats.
//   - c11_emoji_size_lifted: retired. It compared two literals;
//     test_chirp_commands_wap.cpp's every_session_display_is_five_emoji_in_its_field
//     renders every session display through the real generate_emoji_string()
//     into the real field (sweep F231).
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

const Test kTests[] = {
    {"c2_c3_no_self_count", test_c2_c3_no_self_count},
    {"c5_ack_dedup_by_pubkey", test_c5_ack_dedup_by_pubkey},
    {"c8_priority_storage_eviction", test_c8_priority_storage_eviction},
    {"c9_bloom_flood", test_c9_bloom_flood},
};

}  // namespace chirp_security

int main(int argc, char** argv) {
  using namespace chirp_security;
  return run_tests(kTests, sizeof kTests / sizeof kTests[0], "chirp security", argc, argv);
}
