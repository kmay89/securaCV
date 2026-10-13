// Host test: Chirp v0.2's wire invariants (spec/chirp_channel_v0.md §3, §12.1),
// held on the REAL chirp_channel.cpp (chirp_host_harness.h), each against the
// spec's words rather than the firmware's own builders: the bytes a frame's
// signature covers are written out here from the spec's field lists and
// verified against what the firmware sent, with OpenSSL's Ed25519, as an
// independent verifier (a CAP gateway, an audit-log reader) would.
//
// This file re-implemented the canonical builders in plain C++ and tested the
// copies, and held two constants to literals of themselves; it said drift
// from the firmware was caught at code review (sweep F271). Each case, and
// what became of it:
//   - protocol_version_is_v02: replaced. The firmware's PROTOCOL_VERSION is
//     the spec's version byte, and a frame with any other version is dropped.
//   - magic_byte: replaced. The firmware's CHIRP_MAGIC is the spec's 0xC4,
//     and a frame with another magic (Opera's 0x0A among them) is dropped.
//   - witness_canonical_layout: replaced. A witness send_chirp() broadcast
//     verifies over the spec's 73 bytes, the session id in its header is the
//     spec's SHA-256 prefix of its key, and a frame whose signature fails or
//     whose session id is not its key's is not stored (C1, C6).
//   - ack_canonical_layout: replaced. A confirmation confirm_chirp()
//     broadcast verifies over the spec's 62 bytes.
//   - suppress_canonical_layout: replaced. A vote dismiss_chirp() broadcast
//     verifies over the spec's 66 bytes.
//   - canonical_distinguishes_signers: replaced. A relay relay_chirp()
//     broadcast is signed by the relayer over the canonical with the
//     relayer's key, not the origin's, and carries the origin's key and
//     signature, which verify over the canonical with the origin's key (C4).
//   - emoji_display_size_lifted: retired. It held a literal 31 to itself;
//     test_chirp_commands_wap.cpp's every_session_display_is_five_emoji_in_its_field
//     renders every session display through the real generate_emoji_string()
//     into the real field (sweep F231).
//   - common_header_matches_the_reference: new (sweep F272). The canonical
//     C-ABI header, firmware/common/chirp/chirp_channel.h, sized the session
//     display for three emoji and differed in five more values; each of its
//     values is held here, by static_assert, to the constant it names.
//   - removed_template_is_refused: new, for the C17 row of
//     docs/audit/mesh_and_chirp_audit_v1.md §10, which cited
//     protocol_version_is_v02: a witness naming the removed 0x04 template is
//     not stored.
//
// Host-tested only: the stubs stand in for the radio and the clock; the
// Arduino compile is CI's.
//
// Run: ./test_chirp_protocol_invariants [name]

#include "chirp_host_harness.h"

#include <openssl/evp.h>

#include <algorithm>
#include <iterator>

// The canonical C-ABI API the ACTIVE tree's port is to implement (sweep
// F272): its values are held to this reference below. After the firmware
// file, whose own names are in namespace chirp_channel and none of which
// these macros spell.
#include "../../../common/chirp/chirp_channel.h"

namespace chirp_protocol {

using namespace chirp_host;

// ── The oracle: the spec's signed inputs, from its own field lists ──────

using Bytes = std::vector<uint8_t>;
void put(Bytes& b, const void* p, size_t n) {
  const uint8_t* q = static_cast<const uint8_t*>(p);
  b.insert(b.end(), q, q + n);
}
void put_str(Bytes& b, const char* s) { put(b, s, std::strlen(s)); }
// A timestamp as the canary-wap signs it: four bytes, little-endian.
void put_le32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i)));
}
void put_be32(Bytes& b, uint32_t v) {
  for (int i = 3; i >= 0; --i) b.push_back((uint8_t)(v >> (8 * i)));
}

// §3.3: "securacv:chirp:witness:v0" || nonce(8) || template_id(1) ||
// detail_slot(1) || urgency(1) || ttl_minutes(1) || timestamp(4) ||
// session_pubkey(32). The key is the signer's: the origin's at hop 0, the
// relayer's on a relay (and the origin's for the signed_origin envelope).
Bytes witness_signed_input(const uint8_t nonce[8], uint8_t tpl, uint8_t detail, uint8_t urgency,
                           uint8_t ttl, uint32_t timestamp, const uint8_t key[32],
                           bool big_endian_timestamp = false) {
  Bytes b;
  put_str(b, "securacv:chirp:witness:v0");
  put(b, nonce, 8);
  b.push_back(tpl);
  b.push_back(detail);
  b.push_back(urgency);
  b.push_back(ttl);
  if (big_endian_timestamp) {
    put_be32(b, timestamp);
  } else {
    put_le32(b, timestamp);
  }
  put(b, key, 32);
  return b;
}
// §3.4: "securacv:chirp:ack:v0" || original_nonce(8) || ack_type(1) ||
// confirmer_session_pubkey(32); ack_type in the spec's order (seen 0,
// confirmed 1, resolved 2).
Bytes ack_signed_input(const uint8_t nonce[8], uint8_t ack_type, const uint8_t key[32]) {
  Bytes b;
  put_str(b, "securacv:chirp:ack:v0");
  put(b, nonce, 8);
  b.push_back(ack_type);
  put(b, key, 32);
  return b;
}
// §2.5.6: "securacv:chirp:suppress:v0" || original_nonce || voter_session_pubkey.
Bytes suppress_signed_input(const uint8_t nonce[8], const uint8_t key[32]) {
  Bytes b;
  put_str(b, "securacv:chirp:suppress:v0");
  put(b, nonce, 8);
  put(b, key, 32);
  return b;
}
bool verifies(const uint8_t sig[64], const uint8_t key[32], const Bytes& msg) {
  return Ed25519::verify(sig, key, msg.data(), msg.size());
}
// §3.1: SHA-256("securacv:chirp:session:v0" || session_pubkey)[0:8], with
// OpenSSL, not the firmware's session_id_from_pubkey().
void spec_session_id(const uint8_t key[32], uint8_t out[8]) {
  Bytes b;
  put_str(b, "securacv:chirp:session:v0");
  put(b, key, 32);
  unsigned char md[32];
  unsigned int len = 0;
  CHECK(EVP_Digest(b.data(), b.size(), md, &len, EVP_sha256(), nullptr) == 1 && len == 32);
  memcpy(out, md, 8);
}

template <typename P>
void split(const Bytes& f, cc::ChirpHeader* h, P* p) {
  CHECK(f.size() == sizeof *h + sizeof *p);
  memcpy(h, f.data(), sizeof *h);
  memcpy(p, f.data() + sizeof *h, sizeof *p);
}

// ── The header ──────────────────────────────────────────────────────────

// §3.1: v0.2 frames carry version 1 (v0.1's was 0, and v0.2 receivers drop
// it, audit C1); a witness with version 0 or 2 is not stored, the same frame
// with version 1 is.
void test_protocol_version_is_v02() {
  CHECK(cc::PROTOCOL_VERSION == 1);
  boot_enabled();
  const Neighbor n = neighbor_of(0x21);
  const uint8_t versions[] = {0, 2, 0xFF};
  uint8_t byte = 0x30;
  for (const uint8_t v : versions) {
    Bytes w = witness_of(n, byte++);
    w[offsetof(cc::ChirpHeader, version)] = v;
    deliver(n, w);
    CHECK(cc::g_recent_chirp_count == 0);
  }
  deliver(n, witness_of(n, byte++));
  CHECK(cc::g_recent_chirp_count == 1);
}

// §3.1 and §12.1: the magic is 0xC4 (Opera's is 0x0A); a frame with another
// magic is not Chirp's and is not stored.
void test_magic_byte() {
  CHECK(cc::CHIRP_MAGIC == 0xC4);
  boot_enabled();
  const Neighbor n = neighbor_of(0x22);
  const uint8_t magics[] = {0x0A, 0xC5, 0x00, 0x4C};
  uint8_t byte = 0x30;
  for (const uint8_t m : magics) {
    Bytes w = witness_of(n, byte++);
    w[offsetof(cc::ChirpHeader, magic)] = m;
    deliver(n, w);
    CHECK(cc::g_recent_chirp_count == 0);
  }
  deliver(n, witness_of(n, byte++));
  CHECK(cc::g_recent_chirp_count == 1);
}

// ── What each signature covers ──────────────────────────────────────────

// A witness this device sends (send_chirp(), as the owner's send runs it):
// its signature verifies over the spec's 73 bytes with the key the frame
// carries, which is this session's; the header's session id is the spec's
// hash of that key; and the timestamp is signed little-endian (big-endian
// bytes do not verify). The firmware's own builder writes the same bytes.
void test_witness_canonical_layout() {
  boot_enabled(/*presence_met=*/true);
  const size_t mark = host_sim::espnow->sent.size();
  cc::SendRefusal why = cc::SEND_REFUSED_NONE;
  uint32_t left = 0;
  CHECK(cc::send_chirp(cc::TPL_EMERG_FIRE_VISIBLE, cc::CHIRP_URG_URGENT, cc::DETAIL_STATUS_SPREADING,
                       30, &why, &left));
  const std::vector<Bytes> sent = sent_of_type(mark, cc::CHIRP_MSG_WITNESS);
  CHECK(sent.size() == 1);
  cc::ChirpHeader h;
  cc::ChirpWitnessPayload p;
  split(sent[0], &h, &p);
  CHECK(h.magic == 0xC4 && h.version == 1 && h.hop_count == 0);
  CHECK(h.timestamp == (uint32_t)host_sim::wall_now);
  CHECK(memcmp(p.session_pubkey, cc::g_session.session_pubkey, 32) == 0);
  uint8_t sid[8];
  spec_session_id(p.session_pubkey, sid);
  CHECK(memcmp(h.session_id, sid, 8) == 0);

  const Bytes oracle = witness_signed_input(h.nonce, (uint8_t)cc::TPL_EMERG_FIRE_VISIBLE,
                                            (uint8_t)cc::DETAIL_STATUS_SPREADING,
                                            (uint8_t)cc::CHIRP_URG_URGENT, 30, h.timestamp,
                                            p.session_pubkey);
  CHECK(oracle.size() == 25 + 8 + 4 + 4 + 32);
  CHECK(verifies(p.signature, p.session_pubkey, oracle));
  CHECK(!verifies(p.signature, p.session_pubkey,
                  witness_signed_input(h.nonce, (uint8_t)cc::TPL_EMERG_FIRE_VISIBLE,
                                       (uint8_t)cc::DETAIL_STATUS_SPREADING,
                                       (uint8_t)cc::CHIRP_URG_URGENT, 30, h.timestamp,
                                       p.session_pubkey, /*big_endian_timestamp=*/true)));
  uint8_t built[256];
  const size_t bl = cc::build_witness_canonical(&h, &p, p.session_pubkey, built, sizeof built);
  CHECK(bl == oracle.size() && memcmp(built, oracle.data(), bl) == 0);
  // Nothing on hop 0 carries an origin envelope.
  static const uint8_t zero[64] = {0};
  CHECK(memcmp(p.signed_origin_pubkey, zero, 32) == 0 && memcmp(p.signed_origin_signature, zero, 64) == 0);
}

// A receiver drops a witness whose signature does not verify (C1) or whose
// header names a session its key does not derive (C6), and stores the
// frame that is right.
void test_c1_c6_a_witness_must_verify() {
  boot_enabled();
  const Neighbor n = neighbor_of(0x23);
  const Neighbor other = neighbor_of(0x24);
  Bytes flipped = witness_of(n, 0x31);
  flipped[sizeof(cc::ChirpHeader) + offsetof(cc::ChirpWitnessPayload, signature) + 9] ^= 0x10;
  deliver(n, flipped);
  CHECK(cc::g_recent_chirp_count == 0);
  Bytes foreign = witness_of(n, 0x32);   // a good signature, another device's session id
  memcpy(&foreign[offsetof(cc::ChirpHeader, session_id)], other.sid, 8);
  deliver(n, foreign);
  CHECK(cc::g_recent_chirp_count == 0);
  deliver(n, witness_of(n, 0x33));
  CHECK(cc::g_recent_chirp_count == 1);
}

// A confirmation this device sends (confirm_chirp(), the owner's confirm)
// verifies over the spec's 62 bytes: the confirmed chirp's nonce, ack_type
// confirmed (1), this session's key.
void test_ack_canonical_layout() {
  boot_enabled(/*presence_met=*/true);
  const Neighbor n = neighbor_of(0x25);
  const Bytes w = witness_of(n, 0x34);
  uint8_t nonce[8];
  nonce_of(w, nonce);
  deliver(n, w);
  CHECK(row_of(nonce) != nullptr);
  const size_t mark = host_sim::espnow->sent.size();
  cc::ConfirmRefusal why = cc::CONFIRM_REFUSED_NONE;
  CHECK(cc::confirm_chirp(nonce, &why));
  const std::vector<Bytes> sent = sent_of_type(mark, cc::CHIRP_MSG_ACK);
  CHECK(sent.size() == 1);
  cc::ChirpHeader h;
  cc::ChirpAckPayload p;
  split(sent[0], &h, &p);
  CHECK(memcmp(p.original_nonce, nonce, 8) == 0 && p.ack_type == 1);
  CHECK(memcmp(p.confirmer_session_pubkey, cc::g_session.session_pubkey, 32) == 0);
  uint8_t sid[8];
  spec_session_id(p.confirmer_session_pubkey, sid);
  CHECK(memcmp(h.session_id, sid, 8) == 0);
  const Bytes oracle = ack_signed_input(nonce, 1, p.confirmer_session_pubkey);
  CHECK(oracle.size() == 21 + 8 + 1 + 32);
  CHECK(verifies(p.signature, p.confirmer_session_pubkey, oracle));
  CHECK(!verifies(p.signature, p.confirmer_session_pubkey, ack_signed_input(nonce, 0, p.confirmer_session_pubkey)));
}

// A suppress vote this device sends (dismiss_chirp(), the owner's dismiss)
// verifies over the spec's 66 bytes: the chirp's nonce and this session's key.
void test_suppress_canonical_layout() {
  boot_enabled(/*presence_met=*/true);
  const Neighbor n = neighbor_of(0x26);
  const Bytes w = witness_of(n, 0x35);
  uint8_t nonce[8];
  nonce_of(w, nonce);
  deliver(n, w);
  const size_t mark = host_sim::espnow->sent.size();
  bool vote_sent = false;
  cc::ConfirmRefusal why = cc::CONFIRM_REFUSED_NONE;
  CHECK(cc::dismiss_chirp(nonce, &vote_sent, &why) && vote_sent);
  const std::vector<Bytes> sent = sent_of_type(mark, cc::CHIRP_MSG_SUPPRESS_VOTE);
  CHECK(sent.size() == 1);
  cc::ChirpHeader h;
  cc::ChirpSuppressVotePayload p;
  split(sent[0], &h, &p);
  CHECK(memcmp(p.original_nonce, nonce, 8) == 0);
  CHECK(memcmp(p.voter_session_pubkey, cc::g_session.session_pubkey, 32) == 0);
  const Bytes oracle = suppress_signed_input(nonce, p.voter_session_pubkey);
  CHECK(oracle.size() == 26 + 8 + 32);
  CHECK(verifies(p.signature, p.voter_session_pubkey, oracle));
}

// A relay (C4): this device relays a neighbor's emergency chirp once a
// confirmation validates it. The relay keeps the chirp's nonce, timestamp
// and fields at hop 1, carries this device's key at the top, signed by it
// over the canonical with its own key (the origin's key in that slot does
// not verify), and the origin's key and signature in the signed_origin
// envelope, which verify over the canonical with the origin's key: two
// signers, two different signed inputs over one body.
void test_canonical_distinguishes_signers() {
  boot_enabled();
  const Neighbor origin = neighbor_of(0x27);
  const Neighbor confirmer = neighbor_of(0x28);
  deliver(confirmer, presence_of(confirmer));
  const Bytes w = witness_of(origin, 0x36, cc::TPL_EMERG_FIRE_VISIBLE, cc::CHIRP_URG_URGENT);
  cc::ChirpHeader oh;
  cc::ChirpWitnessPayload op;
  split(w, &oh, &op);
  deliver(origin, w);
  const size_t mark = host_sim::espnow->sent.size();
  deliver(confirmer, confirm_of(confirmer, oh.nonce));
  const std::vector<Bytes> relays = sent_of_type(mark, cc::CHIRP_MSG_WITNESS);
  CHECK(relays.size() == 1);
  cc::ChirpHeader h;
  cc::ChirpWitnessPayload p;
  split(relays[0], &h, &p);
  CHECK(h.hop_count == 1 && memcmp(h.nonce, oh.nonce, 8) == 0 && h.timestamp == oh.timestamp);
  CHECK(p.template_id == op.template_id && p.detail_slot == op.detail_slot && p.urgency == op.urgency);
  CHECK(memcmp(p.session_pubkey, cc::g_session.session_pubkey, 32) == 0);
  uint8_t sid[8];
  spec_session_id(p.session_pubkey, sid);
  CHECK(memcmp(h.session_id, sid, 8) == 0);
  const Bytes as_relayer = witness_signed_input(h.nonce, p.template_id, p.detail_slot, p.urgency,
                                                p.ttl_minutes, h.timestamp, p.session_pubkey);
  const Bytes as_origin = witness_signed_input(h.nonce, p.template_id, p.detail_slot, p.urgency,
                                               p.ttl_minutes, h.timestamp, origin.pub);
  CHECK(as_relayer != as_origin);
  CHECK(verifies(p.signature, p.session_pubkey, as_relayer));
  CHECK(!verifies(p.signature, p.session_pubkey, as_origin));
  CHECK(memcmp(p.signed_origin_pubkey, origin.pub, 32) == 0);
  CHECK(memcmp(p.signed_origin_signature, op.signature, 64) == 0);
  CHECK(verifies(p.signed_origin_signature, origin.pub, as_origin));
  CHECK(!verifies(p.signed_origin_signature, origin.pub, as_relayer));
}

// C17: TPL_AUTH_FEDERAL_PRESENCE (0x04) is removed and its slot reserved; a
// witness naming it, properly signed, is not stored, and the template next
// to it is.
void test_removed_template_is_refused() {
  boot_enabled();
  const Neighbor n = neighbor_of(0x29);
  deliver(n, witness_of(n, 0x37, (cc::ChirpTemplate)0x04));
  CHECK(cc::g_recent_chirp_count == 0);
  deliver(n, witness_of(n, 0x38, cc::TPL_AUTH_HELICOPTER));
  CHECK(cc::g_recent_chirp_count == 1);
}

// ── The canonical C-ABI header (sweep F272) ─────────────────────────────

// firmware/common/chirp/chirp_channel.h, the API the ACTIVE tree's port is
// to implement, says what the reference does: each of its values against the
// constant it names (the relay delay excepted: the reference has none). It
// sized the session display for three emoji in 20 bytes, kept 32 recent and
// 16 nearby rows, beaconed every 15 s and named a 64-entry nonce cache.
static_assert(CHIRP_WIRE_VERSION == cc::PROTOCOL_VERSION, "wire version");
static_assert(CHIRP_WIRE_MAGIC == cc::CHIRP_MAGIC, "magic");
static_assert(CHIRP_SESSION_ID_SIZE == cc::SESSION_ID_SIZE, "session id");
static_assert(CHIRP_SESSION_PUBKEY_SIZE == cc::SESSION_PUBKEY_SIZE, "session key");
static_assert(CHIRP_NONCE_SIZE == sizeof(cc::ChirpHeader{}.nonce), "nonce");
static_assert(CHIRP_EMOJI_DISPLAY_SIZE == cc::EMOJI_DISPLAY_SIZE, "five emoji in 31 bytes");
static_assert(CHIRP_EMOJI_DISPLAY_SIZE == sizeof(cc::ChirpPresencePayload{}.emoji), "the beacon's field");
static_assert(CHIRP_MAX_HOPS == cc::MAX_HOP_COUNT, "hops");
static_assert(CHIRP_WITNESS_MAX_AGE_S == cc::WITNESS_MAX_AGE_S, "freshness, behind");
static_assert(CHIRP_WITNESS_MAX_AHEAD_S == cc::WITNESS_MAX_AHEAD_S, "freshness, ahead");
static_assert(CHIRP_MIN_UNIX_TIME == cc::MIN_UNIX_TIME, "clock floor");
static_assert(CHIRP_MAX_RECENT == cc::MAX_RECENT_CHIRPS, "recent rows");
static_assert(CHIRP_MAX_NEARBY == cc::MAX_NEARBY_CACHE, "nearby rows");
static_assert(CHIRP_NONCE_HOLD_MS == cc::NONCE_FILTER_HALF_MS, "nonce hold");
static_assert(CHIRP_PRESENCE_INTERVAL_MS == cc::PRESENCE_INTERVAL_MS, "beacon interval");
static_assert(CHIRP_PRESENCE_TIMEOUT_MS == cc::NEARBY_TIMEOUT_MS, "nearby timeout");
static_assert(CHIRP_RECENT_TTL_MS == cc::DEFAULT_DISPLAY_MS, "display time");
static_assert(CHIRP_PRESENCE_REQ_MS == cc::PRESENCE_REQUIRED_MS, "presence requirement");
static_assert(CHIRP_COOLDOWN_TIER1_MS == cc::COOLDOWN_TIER_1_MS, "cooldown 1");
static_assert(CHIRP_COOLDOWN_TIER2_MS == cc::COOLDOWN_TIER_2_MS, "cooldown 2");
static_assert(CHIRP_COOLDOWN_TIER3_MS == cc::COOLDOWN_TIER_3_MS, "cooldown 3");
static_assert(CHIRP_COOLDOWN_TIER4_MS == cc::COOLDOWN_TIER_4_MS, "cooldown 4");
static_assert(CHIRP_COOLDOWN_RESET_MS == cc::COOLDOWN_RESET_MS, "cooldown reset");

// The enums the API names, value for value, and the template table: every
// template the reference takes, and no other (0x04 stays reserved).
void test_common_header_matches_the_reference() {
  CHECK((int)CHIRP_CAT_AUTHORITY == (int)cc::CHIRP_CAT_AUTHORITY);
  CHECK((int)CHIRP_CAT_INFRA == (int)cc::CHIRP_CAT_INFRA);
  CHECK((int)CHIRP_CAT_EMERGENCY == (int)cc::CHIRP_CAT_EMERGENCY);
  CHECK((int)CHIRP_CAT_WEATHER == (int)cc::CHIRP_CAT_WEATHER);
  CHECK((int)CHIRP_CAT_MUTUAL_AID == (int)cc::CHIRP_CAT_MUTUAL_AID);
  CHECK((int)CHIRP_CAT_ALL_CLEAR == (int)cc::CHIRP_CAT_ALL_CLEAR);
  CHECK((int)CHIRP_URG_INFO == (int)cc::CHIRP_URG_INFO);
  CHECK((int)CHIRP_URG_CAUTION == (int)cc::CHIRP_URG_CAUTION);
  CHECK((int)CHIRP_URG_URGENT == (int)cc::CHIRP_URG_URGENT);
  CHECK((int)CHIRP_ACK_SEEN == (int)cc::CHIRP_ACK_SEEN);
  CHECK((int)CHIRP_ACK_CONFIRMED == (int)cc::CHIRP_ACK_CONFIRMED);
  CHECK((int)CHIRP_ACK_RESOLVED == (int)cc::CHIRP_ACK_RESOLVED);
  const struct {
    int api;
    int ref;
  } details[] = {
      {CHIRP_DETAIL_NONE, cc::DETAIL_NONE},
      {CHIRP_DETAIL_SCALE_FEW, cc::DETAIL_SCALE_FEW},
      {CHIRP_DETAIL_SCALE_MANY, cc::DETAIL_SCALE_MANY},
      {CHIRP_DETAIL_SCALE_MASSIVE, cc::DETAIL_SCALE_MASSIVE},
      {CHIRP_DETAIL_STATUS_ONGOING, cc::DETAIL_STATUS_ONGOING},
      {CHIRP_DETAIL_STATUS_CONTAINED, cc::DETAIL_STATUS_CONTAINED},
      {CHIRP_DETAIL_STATUS_SPREADING, cc::DETAIL_STATUS_SPREADING},
      {CHIRP_DETAIL_DIR_NORTH, cc::DETAIL_DIR_NORTH},
      {CHIRP_DETAIL_DIR_SOUTH, cc::DETAIL_DIR_SOUTH},
      {CHIRP_DETAIL_DIR_EAST, cc::DETAIL_DIR_EAST},
      {CHIRP_DETAIL_DIR_WEST, cc::DETAIL_DIR_WEST},
  };
  for (const auto& d : details) CHECK(d.api == d.ref);
  const int api_templates[] = {
      CHIRP_TPL_AUTH_POLICE_ACTIVITY, CHIRP_TPL_AUTH_HEAVY_RESPONSE, CHIRP_TPL_AUTH_ROAD_BLOCKED,
      CHIRP_TPL_AUTH_HELICOPTER,      CHIRP_TPL_INFRA_POWER_OUT,     CHIRP_TPL_INFRA_WATER_ISSUE,
      CHIRP_TPL_INFRA_GAS_SMELL,      CHIRP_TPL_INFRA_INTERNET_DOWN, CHIRP_TPL_INFRA_ROAD_CLOSED,
      CHIRP_TPL_EMERG_FIRE,           CHIRP_TPL_EMERG_MEDICAL,       CHIRP_TPL_EMERG_MULTI_AMBULANCE,
      CHIRP_TPL_EMERG_EVACUATION,     CHIRP_TPL_EMERG_SHELTER,       CHIRP_TPL_WX_SEVERE,
      CHIRP_TPL_WX_TORNADO,           CHIRP_TPL_WX_FLOOD,            CHIRP_TPL_WX_LIGHTNING,
      CHIRP_TPL_AID_WELFARE,          CHIRP_TPL_AID_SUPPLIES,        CHIRP_TPL_AID_OFFERING,
      CHIRP_TPL_CLR_RESOLVED,         CHIRP_TPL_CLR_SAFE,            CHIRP_TPL_CLR_FALSE_ALARM,
  };
  size_t taken = 0;
  for (int id = 0; id < 256; ++id) {
    const bool in_api = std::find(std::begin(api_templates), std::end(api_templates), id) !=
                        std::end(api_templates);
    const bool in_ref = cc::is_valid_template((cc::ChirpTemplate)id);
    CHECK(in_api == in_ref);
    taken += in_ref ? 1 : 0;
  }
  CHECK(taken == sizeof api_templates / sizeof api_templates[0]);
  CHECK(!cc::is_valid_template((cc::ChirpTemplate)0x04));
  // The longest display there is (five suns, each 6 bytes with its variation
  // selector) and its NUL fit the API's field.
  uint8_t sid[cc::SESSION_ID_SIZE];
  memset(sid, 0x0B, sizeof sid);   // nibble 11 picks the sun everywhere
  char display[CHIRP_EMOJI_DISPLAY_SIZE];
  memset(display, 0x7F, sizeof display);
  cc::generate_emoji_string(sid, display);
  CHECK(std::strlen(display) == 30 && display[30] == '\0');
}

const Test kTests[] = {
    {"protocol_version_is_v02", test_protocol_version_is_v02},
    {"magic_byte", test_magic_byte},
    {"witness_canonical_layout", test_witness_canonical_layout},
    {"c1_c6_a_witness_must_verify", test_c1_c6_a_witness_must_verify},
    {"ack_canonical_layout", test_ack_canonical_layout},
    {"suppress_canonical_layout", test_suppress_canonical_layout},
    {"canonical_distinguishes_signers", test_canonical_distinguishes_signers},
    {"removed_template_is_refused", test_removed_template_is_refused},
    {"common_header_matches_the_reference", test_common_header_matches_the_reference},
};

}  // namespace chirp_protocol

int main(int argc, char** argv) {
  using namespace chirp_protocol;
  return run_tests(kTests, sizeof kTests / sizeof kTests[0], "chirp protocol invariant", argc, argv);
}
