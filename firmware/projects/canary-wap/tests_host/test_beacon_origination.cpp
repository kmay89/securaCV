// Host-side test for the Beacon channel's receive-path invariants.
//
// Mirrors the validation pipeline in beacon_channel.cpp::handle_alert_frame
// (spec/beacon_channel_v0.md §7.1), in pipeline order:
//   1. magic = 0xB1
//   2. frame not already seen (replay dedup, §7.1 step 3 — keyed on the two
//      signatures, because the header nonce is outside the signed bytes)
//   3. scope = Private
//   4. canonical.msg_type == header.msg_type (only the canonical is signed)
//   5. EXERCISE frames carry BCN_FLAG_IS_EXERCISE and nothing else does (§5.4)
//   6. template_id inside the life-safety set (§4)
//   7. both originator_fp and cosigner_fp present in local beacon set
//   8. neither revoked
//   9. originator_fp != cosigner_fp (dual-pubkey path)
//  10. neither signer's selftest older than 36 h (§7.1 step 9)
//  11. both signatures verify (assumed; we don't link Ed25519 here)
//  12. |now - effective| <= BEACON_FRESHNESS_S and now <= expires (§7.1 step 4)
//  13. per-pubkey rate bucket, drills counted separately (AGENTS invariant 10)
// then the state effect keyed off the SIGNED msg_type, where CANCEL and
// UPDATE must name the active alarm's frame nonce (§5.4, §7.2).
//
// We test the *gating logic* without doing real Ed25519 — the protocol
// invariant being asserted is that any frame failing any of these checks is
// rejected at the earliest possible point.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr size_t DEVICE_FP_SIZE = 16;
constexpr size_t BEACON_NONCE_SIZE = 16;
constexpr uint8_t BCN_MAGIC = 0xB1;
constexpr uint8_t BCN_SCOPE_PRIVATE = 2;
constexpr uint8_t BCN_TRUST_REVOKED = 2;

constexpr uint8_t BCN_FLAG_IS_EXERCISE = 0x01;
constexpr uint8_t BCN_FLAG_SOLO_ORIGIN = 0x04;

constexpr uint8_t BCN_CERT_OBSERVED = 0;

constexpr uint8_t MSG_ALERT    = 0;
constexpr uint8_t MSG_UPDATE   = 1;
constexpr uint8_t MSG_CANCEL   = 2;
constexpr uint8_t MSG_EXERCISE = 3;

constexpr uint8_t TPL_FIRE_VISIBLE = 0x20;
constexpr uint8_t TPL_FALSE_ALARM  = 0x82;
// 0x00 is CHIRP_TPL_AUTH_POLICE_ACTIVITY in the adjacent Chirp numbering
// space — a category Beacon excludes by design (spec §4).
constexpr uint8_t TPL_NOT_BEACON   = 0x00;

constexpr uint32_t BEACON_FRESHNESS_S = 300;
constexpr uint32_t SELFTEST_MISSING_MS = 129600000;  // 36 h
constexpr uint8_t  MAX_ORIGINATIONS_PER_PUBKEY_24H = 5;
constexpr size_t   SEEN_FRAME_MAX = 32;
constexpr size_t   FRAME_ID_SIZE = 32;  // 16 bytes of each signature

bool is_valid_beacon_template(uint8_t id) {
  switch (id) {
    case 0x10: case 0x12:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
    case 0x30: case 0x31: case 0x32:
    case 0x80: case 0x81: case 0x82:
      return true;
    default:
      return false;
  }
}

struct SetEntry {
  uint8_t  fp[DEVICE_FP_SIZE];
  uint8_t  trust;
  uint32_t last_selftest_ms;  // 0 = not observed since boot
  bool     valid;
};

struct Frame {
  // Header (unsigned — anything here can be rewritten in flight).
  uint8_t  magic;
  uint8_t  hdr_msg_type;
  uint8_t  flags;
  uint8_t  nonce[BEACON_NONCE_SIZE];
  // Canonical (covered by both signatures).
  uint8_t  canon_msg_type;
  uint8_t  template_id;
  uint8_t  scope;
  uint8_t  certainty;
  uint64_t effective;
  uint64_t expires;
  uint8_t  ref_canceled_nonce[BEACON_NONCE_SIZE];
  uint8_t  originator_fp[DEVICE_FP_SIZE];
  uint8_t  cosigner_fp[DEVICE_FP_SIZE];
  bool     sig_a_valid;
  bool     sig_b_valid;
  // Stands in for the signature bytes: Ed25519 is deterministic, so a copy of
  // a frame carries the same id however its header was rewritten, and a
  // distinct origination carries a distinct one.
  uint8_t  sig_id[FRAME_ID_SIZE];
};

const SetEntry* find_in_set(const std::vector<SetEntry>& s, const uint8_t* fp) {
  for (const auto& e : s) {
    if (!e.valid) continue;
    if (std::memcmp(e.fp, fp, DEVICE_FP_SIZE) == 0) return &e;
  }
  return nullptr;
}

// `last_selftest == 0` means "not observed since boot" (the map is RAM-only
// per spec §11), not "stale" — the firmware treats it as unknown for the
// same reason recompute_trouble_reasons skips it.
bool signer_selftest_stale(const SetEntry& e, uint32_t now_ms) {
  if (e.last_selftest_ms == 0) return false;
  const uint32_t age = (now_ms > e.last_selftest_ms) ? (now_ms - e.last_selftest_ms) : 0;
  return age > SELFTEST_MISSING_MS;
}

// The stateless half of handle_alert_frame: everything from the scope check
// through the freshness window. Stateful checks (replay ring, rate buckets,
// alarm reference) live in Receiver below.
bool would_accept(const std::vector<SetEntry>& set, const Frame& f,
                  uint64_t now_unix = 1800000000ULL, uint32_t now_ms = 0) {
  if (f.magic != BCN_MAGIC) return false;
  if (f.scope != BCN_SCOPE_PRIVATE) return false;

  // The signatures cover only the canonical, so the header msg_type must
  // agree with it or the header was rewritten after signing.
  if (f.canon_msg_type != f.hdr_msg_type) return false;

  // spec §5.4: EXERCISE <=> BCN_FLAG_IS_EXERCISE, in both directions.
  const bool exercise_flag = (f.flags & BCN_FLAG_IS_EXERCISE) != 0;
  if ((f.canon_msg_type == MSG_EXERCISE) != exercise_flag) return false;

  if (!is_valid_beacon_template(f.template_id)) return false;

  const bool is_solo = (f.flags & BCN_FLAG_SOLO_ORIGIN) != 0;
  if (is_solo) {
    if (f.certainty != BCN_CERT_OBSERVED) return false;
    if (std::memcmp(f.originator_fp, f.cosigner_fp, DEVICE_FP_SIZE) != 0) return false;
  }

  const SetEntry* a = find_in_set(set, f.originator_fp);
  const SetEntry* b = is_solo ? a : find_in_set(set, f.cosigner_fp);
  if (!a || !b) return false;
  if (a->trust == BCN_TRUST_REVOKED) return false;
  if (b->trust == BCN_TRUST_REVOKED) return false;
  if (!is_solo &&
      std::memcmp(f.originator_fp, f.cosigner_fp, DEVICE_FP_SIZE) == 0) {
    return false;
  }

  if (signer_selftest_stale(*a, now_ms) || signer_selftest_stale(*b, now_ms)) return false;

  if (!f.sig_a_valid || !f.sig_b_valid) return false;

  // Freshness (§7.1 step 4). The unsynced-clock branch — accept but flag —
  // is exercised by test_unsynced_clock_accepts_stale_effective.
  if (now_unix >= 1700000000ULL) {
    if (now_unix > f.expires) return false;
    const uint64_t skew = (f.effective > now_unix) ? (f.effective - now_unix)
                                                   : (now_unix - f.effective);
    if (skew > BEACON_FRESHNESS_S) return false;
  }
  return true;
}

// What handle_alert_frame did with the frame.
enum class Outcome {
  RejectedGate,    // failed a stateless check — never audited
  RejectedReplay,  // nonce already seen; no signature work, no rate charge
  RejectedRate,    // originator's bucket for this frame class is exhausted
  Audited,         // entered the audit log; state effect may still be a no-op
};

// The stateful half: the seen-frame ring, the split rate buckets, and the
// active-alarm reference rules.
struct Receiver {
  std::vector<SetEntry> set;

  struct SeenFrame { uint8_t id[FRAME_ID_SIZE]; uint32_t seen_ms; bool valid; };
  SeenFrame seen[SEEN_FRAME_MAX] = {};
  size_t seen_head = 0;
  // Identity of the latest accepted ALERT; outlives both the ring's horizon
  // and the alarm itself.
  uint8_t last_alert_id[FRAME_ID_SIZE] = {};
  bool    last_alert_id_valid = false;

  struct Rate {
    uint8_t  fp[DEVICE_FP_SIZE];
    uint32_t window_start_ms;
    uint8_t  count;
    uint32_t exercise_window_start_ms;
    uint8_t  exercise_count;
    bool     valid;
  };
  std::vector<Rate> rates;

  bool    alarm_valid = false;
  uint8_t alarm_nonce[BEACON_NONCE_SIZE] = {};
  uint8_t alarm_template = 0;
  uint64_t alarm_expires = 0;
  int     alarm_callbacks = 0;
  int     audited = 0;

  bool frame_seen(const uint8_t* id, uint32_t now_ms) {
    if (last_alert_id_valid && std::memcmp(last_alert_id, id, FRAME_ID_SIZE) == 0) return true;
    const uint32_t horizon_ms = BEACON_FRESHNESS_S * 1000UL;
    for (size_t i = 0; i < SEEN_FRAME_MAX; i++) {
      if (!seen[i].valid) continue;
      if (now_ms - seen[i].seen_ms > horizon_ms) { seen[i].valid = false; continue; }
      if (std::memcmp(seen[i].id, id, FRAME_ID_SIZE) == 0) return true;
    }
    return false;
  }

  void remember_frame(const uint8_t* id, uint32_t now_ms) {
    std::memcpy(seen[seen_head].id, id, FRAME_ID_SIZE);
    seen[seen_head].seen_ms = now_ms;
    seen[seen_head].valid = true;
    seen_head = (seen_head + 1) % SEEN_FRAME_MAX;
  }

  bool rate_check_and_record(const uint8_t* fp, bool is_exercise, uint32_t now_ms) {
    const uint32_t WINDOW_MS = 86400000;
    Rate* entry = nullptr;
    for (auto& r : rates) {
      if (r.valid && std::memcmp(r.fp, fp, DEVICE_FP_SIZE) == 0) { entry = &r; break; }
    }
    if (!entry) {
      Rate r{};
      std::memcpy(r.fp, fp, DEVICE_FP_SIZE);
      r.valid = true;
      rates.push_back(r);
      entry = &rates.back();
    }
    uint32_t* window = is_exercise ? &entry->exercise_window_start_ms
                                   : &entry->window_start_ms;
    uint8_t* count = is_exercise ? &entry->exercise_count : &entry->count;
    if (*count == 0 || now_ms - *window > WINDOW_MS) {
      *window = now_ms;
      *count = 1;
      return true;
    }
    if (*count >= MAX_ORIGINATIONS_PER_PUBKEY_24H) return false;
    (*count)++;
    return true;
  }

  bool references_active_alarm(const Frame& f) const {
    if (!alarm_valid) return false;
    bool nonzero = false;
    for (size_t i = 0; i < BEACON_NONCE_SIZE; i++) {
      if (f.ref_canceled_nonce[i] != 0) { nonzero = true; break; }
    }
    if (!nonzero) return false;
    return std::memcmp(f.ref_canceled_nonce, alarm_nonce, BEACON_NONCE_SIZE) == 0;
  }

  Outcome receive(const Frame& f, uint64_t now_unix = 1800000000ULL,
                  uint32_t now_ms = 0) {
    if (f.magic != BCN_MAGIC) return Outcome::RejectedGate;
    if (frame_seen(f.sig_id, now_ms)) return Outcome::RejectedReplay;
    if (!would_accept(set, f, now_unix, now_ms)) return Outcome::RejectedGate;

    remember_frame(f.sig_id, now_ms);
    const bool is_exercise = (f.canon_msg_type == MSG_EXERCISE);
    if (!rate_check_and_record(f.originator_fp, is_exercise, now_ms)) {
      return Outcome::RejectedRate;
    }
    audited++;

    if (f.canon_msg_type == MSG_ALERT) {
      alarm_valid = true;
      alarm_template = f.template_id;
      alarm_expires = f.expires;
      std::memcpy(alarm_nonce, f.nonce, BEACON_NONCE_SIZE);
      std::memcpy(last_alert_id, f.sig_id, FRAME_ID_SIZE);
      last_alert_id_valid = true;
      alarm_callbacks++;
    } else if (f.canon_msg_type == MSG_UPDATE) {
      if (references_active_alarm(f)) {
        alarm_template = f.template_id;
        alarm_expires = f.expires;
        alarm_callbacks++;
      }
    } else if (f.canon_msg_type == MSG_CANCEL) {
      if (references_active_alarm(f)) alarm_valid = false;
    }
    return Outcome::Audited;
  }
};

int failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
  } while(0)

SetEntry mk(uint8_t prefix, uint8_t trust = 0, uint32_t last_selftest_ms = 0) {
  SetEntry e{};
  e.valid = true;
  e.trust = trust;
  e.last_selftest_ms = last_selftest_ms;
  std::memset(e.fp, prefix, DEVICE_FP_SIZE);
  return e;
}

uint8_t g_nonce_seq = 1;

Frame mk_frame(uint8_t orig_prefix, uint8_t cosign_prefix,
               bool sa = true, bool sb = true,
               uint8_t magic = BCN_MAGIC, uint8_t scope = BCN_SCOPE_PRIVATE) {
  Frame f{};
  f.magic = magic;
  f.scope = scope;
  f.hdr_msg_type = MSG_ALERT;
  f.canon_msg_type = MSG_ALERT;
  f.template_id = TPL_FIRE_VISIBLE;
  f.certainty = BCN_CERT_OBSERVED;
  f.effective = 1800000000ULL;
  f.expires = 1800000000ULL + 3600ULL;
  // One sequence drives both: every mk_frame is a distinct origination, so it
  // gets a fresh nonce AND fresh signatures. A replay is a copy of the struct.
  std::memset(f.nonce, g_nonce_seq, BEACON_NONCE_SIZE);
  std::memset(f.sig_id, g_nonce_seq, FRAME_ID_SIZE);
  g_nonce_seq++;
  std::memset(f.originator_fp, orig_prefix, DEVICE_FP_SIZE);
  std::memset(f.cosigner_fp,   cosign_prefix, DEVICE_FP_SIZE);
  f.sig_a_valid = sa;
  f.sig_b_valid = sb;
  return f;
}

Frame mk_cancel(uint8_t orig, uint8_t cosign, const uint8_t* ref_nonce) {
  Frame f = mk_frame(orig, cosign);
  f.hdr_msg_type = MSG_CANCEL;
  f.canon_msg_type = MSG_CANCEL;
  f.template_id = TPL_FALSE_ALARM;
  if (ref_nonce) std::memcpy(f.ref_canceled_nonce, ref_nonce, BEACON_NONCE_SIZE);
  return f;
}

// ───────────────────────────────────────────────────────────────────────────
// Pre-existing gate coverage
// ───────────────────────────────────────────────────────────────────────────

void test_happy_path_two_signatures() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(would_accept(set, f), "happy path: two distinct paired pubkeys, both signatures verify");
}

void test_reject_single_signature() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB, /*sa=*/true, /*sb=*/false);
  EXPECT(!would_accept(set, f),
         "two-pubkey rule: missing cosigner signature must be rejected");
}

void test_reject_originator_equals_cosigner() {
  std::vector<SetEntry> set = { mk(0xAA) };
  Frame f = mk_frame(0xAA, 0xAA);
  EXPECT(!would_accept(set, f),
         "originator and cosigner must be distinct pubkeys");
}

void test_reject_unpaired_originator() {
  std::vector<SetEntry> set = { mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(!would_accept(set, f),
         "originator not in local beacon set → reject");
}

void test_reject_unpaired_cosigner() {
  std::vector<SetEntry> set = { mk(0xAA) };
  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(!would_accept(set, f),
         "cosigner not in local beacon set → reject");
}

void test_reject_revoked_originator() {
  std::vector<SetEntry> set = { mk(0xAA, BCN_TRUST_REVOKED), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(!would_accept(set, f), "revoked originator → reject");
}

void test_reject_revoked_cosigner() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB, BCN_TRUST_REVOKED) };
  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(!would_accept(set, f), "revoked cosigner → reject");
}

void test_reject_wrong_magic() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB, true, true, /*magic=*/0xC4);  // chirp magic
  EXPECT(!would_accept(set, f), "wrong magic byte → reject");
}

void test_reject_wrong_scope() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  // scope=0 ("Public" in CAP), prohibited for Beacon
  Frame f = mk_frame(0xAA, 0xBB, true, true, BCN_MAGIC, /*scope=*/0);
  EXPECT(!would_accept(set, f), "non-Private scope → reject (lint invariant)");
}

// ───────────────────────────────────────────────────────────────────────────
// Finding 0 — the header msg_type is unsigned
// ───────────────────────────────────────────────────────────────────────────

void test_header_msg_type_must_match_canonical() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  // A legitimately dual-signed drill, captured and rebroadcast with the one
  // unsigned header byte rewritten to ALERT. Both signatures still verify.
  Frame f = mk_frame(0xAA, 0xBB);
  f.canon_msg_type = MSG_EXERCISE;
  f.flags = BCN_FLAG_IS_EXERCISE;
  f.hdr_msg_type = MSG_ALERT;
  EXPECT(!would_accept(set, f),
         "drill promoted to ALERT via the unsigned header byte → reject");

  // The same trick with a captured all-clear.
  Frame g = mk_frame(0xAA, 0xBB);
  g.canon_msg_type = MSG_CANCEL;
  g.hdr_msg_type = MSG_ALERT;
  EXPECT(!would_accept(set, g),
         "CANCEL promoted to ALERT via the unsigned header byte → reject");
}

void test_exercise_requires_exercise_flag() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB);
  f.canon_msg_type = MSG_EXERCISE;
  f.hdr_msg_type = MSG_EXERCISE;
  f.flags = 0;  // spec §5.4 requires BCN_FLAG_IS_EXERCISE
  EXPECT(!would_accept(set, f), "EXERCISE without the exercise flag → reject");

  f.flags = BCN_FLAG_IS_EXERCISE;
  EXPECT(would_accept(set, f), "EXERCISE with the exercise flag → accept");
}

void test_real_alert_cannot_wear_the_exercise_flag() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB);  // msg_type ALERT on both sides
  f.flags = BCN_FLAG_IS_EXERCISE;
  EXPECT(!would_accept(set, f),
         "real ALERT flagged as a drill → reject (a genuine alert must not "
         "be demotable to a drill by an unsigned flag)");
}

// ───────────────────────────────────────────────────────────────────────────
// Finding 1 — replay dedup and the freshness window
// ───────────────────────────────────────────────────────────────────────────

void test_replayed_frame_is_dropped() {
  Receiver rx;
  rx.set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(rx.receive(f) == Outcome::Audited, "first arrival accepted");
  EXPECT(rx.receive(f) == Outcome::RejectedReplay,
         "same frame again → dropped as replay");
  EXPECT(rx.audited == 1, "a replay adds no second audit entry");
  EXPECT(rx.alarm_callbacks == 1, "a replay does not re-fire the alarm callback");
}

// The header nonce is not under either signature, so a replay can carry any
// nonce it likes. Pre-fix the ring was keyed on it: one byte flipped in the
// unsigned header and the copy passed dedup, verified, charged the
// originator's rate bucket, re-fired the alarm callback and overwrote the
// alarm's identity — after which the originator's own CANCEL, naming the
// nonce it actually sent, no longer resolved.
void test_replay_with_a_fresh_header_nonce_is_still_dropped() {
  Receiver rx;
  rx.set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(rx.receive(f) == Outcome::Audited, "genuine ALERT accepted");

  for (int i = 0; i < 4; i++) {
    Frame copy = f;
    std::memset(copy.nonce, 0xE0 + i, BEACON_NONCE_SIZE);  // rewritten in flight
    EXPECT(rx.receive(copy) == Outcome::RejectedReplay,
           "same signatures under a fresh header nonce → still a replay");
  }
  EXPECT(rx.audited == 1, "nonce-rewritten replays add no audit entries");
  EXPECT(rx.alarm_callbacks == 1, "nonce-rewritten replays do not re-fire the alarm");
  EXPECT(std::memcmp(rx.alarm_nonce, f.nonce, BEACON_NONCE_SIZE) == 0,
         "the alarm keeps the nonce of the frame that actually raised it");

  // Had the four replays each charged the bucket, this CANCEL would be the
  // sixth origination and rate-limited; and had any of them become the
  // alarm's identity, the nonce the originator actually sent would no longer
  // name it.
  Frame cancel = mk_cancel(0xAA, 0xBB, f.nonce);
  EXPECT(rx.receive(cancel) == Outcome::Audited,
         "the originator's CANCEL is not rate-limited by the replays");
  EXPECT(!rx.alarm_valid, "the originator's own CANCEL still resolves the alarm");
}

// The ring forgets after the freshness horizon, and with a synced clock the
// freshness window then takes over. A receiver whose clock never synced has
// no freshness window (§7.1 step 4 says accept and flag), so for it the ring
// horizon was the only thing standing between a held copy and a re-raised
// alarm. The latest ALERT's identity outlives both the ring and a CANCEL.
void test_held_copy_past_the_ring_horizon_cannot_re_raise_the_alarm() {
  Receiver rx;
  rx.set = { mk(0xAA), mk(0xBB) };
  const uint64_t unsynced = 1000;  // below MIN_UNIX_TIME: freshness cannot run
  const uint32_t horizon_ms = BEACON_FRESHNESS_S * 1000UL;

  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(rx.receive(f, unsynced, /*now_ms=*/0) == Outcome::Audited, "ALERT accepted");

  Frame held = f;
  std::memset(held.nonce, 0xEE, BEACON_NONCE_SIZE);
  EXPECT(rx.receive(held, unsynced, horizon_ms + 60000) == Outcome::RejectedReplay,
         "a copy held past the ring horizon is still the alarm it would re-raise");
  EXPECT(rx.alarm_callbacks == 1, "no second alarm from the held copy");

  Frame cancel = mk_cancel(0xAA, 0xBB, f.nonce);
  EXPECT(rx.receive(cancel, unsynced, horizon_ms + 61000) == Outcome::Audited, "CANCEL accepted");
  EXPECT(!rx.alarm_valid, "alarm cleared");
  EXPECT(rx.receive(held, unsynced, horizon_ms + 62000) == Outcome::RejectedReplay,
         "the canceled ALERT cannot be re-raised from a held copy either");
  EXPECT(!rx.alarm_valid, "still clear");

  Frame next = mk_frame(0xAA, 0xBB);
  EXPECT(rx.receive(next, unsynced, horizon_ms + 63000) == Outcome::Audited,
         "a genuinely new ALERT from the same pair is not mistaken for the old one");
}

void test_replay_does_not_consume_the_rate_bucket() {
  Receiver rx;
  rx.set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(rx.receive(f) == Outcome::Audited, "genuine ALERT accepted");
  // Four replays of the captured frame: pre-fix these burned the whole
  // 5-per-24h budget and silenced the originator's next real alert.
  for (int i = 0; i < 4; i++) {
    EXPECT(rx.receive(f) == Outcome::RejectedReplay, "replay dropped");
  }
  Frame later = mk_frame(0xAA, 0xBB);  // fresh nonce, same originator
  EXPECT(rx.receive(later) == Outcome::Audited,
         "originator's next genuine ALERT still gets through after 4 replays");
}

void test_rate_bucket_still_caps_distinct_originations() {
  Receiver rx;
  rx.set = { mk(0xAA), mk(0xBB) };
  for (int i = 0; i < MAX_ORIGINATIONS_PER_PUBKEY_24H; i++) {
    EXPECT(rx.receive(mk_frame(0xAA, 0xBB)) == Outcome::Audited,
           "originations inside the 24 h budget are accepted");
  }
  EXPECT(rx.receive(mk_frame(0xAA, 0xBB)) == Outcome::RejectedRate,
         "the sixth distinct origination from one pubkey is rate-limited");
}

void test_stale_effective_is_rejected() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB);
  const uint64_t now = 1800000000ULL;
  f.effective = now - (BEACON_FRESHNESS_S + 1);
  f.expires = now + 3600;  // still inside its TTL
  EXPECT(!would_accept(set, f, now),
         "effective older than the freshness window → reject even inside TTL");
  f.effective = now + (BEACON_FRESHNESS_S + 1);
  EXPECT(!would_accept(set, f, now),
         "effective in the future beyond the freshness window → reject");
  f.effective = now - BEACON_FRESHNESS_S;
  EXPECT(would_accept(set, f, now), "effective at the window edge → accept");
}

void test_expired_frame_is_rejected() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB);
  const uint64_t now = 1800000000ULL;
  f.effective = now;
  f.expires = now - 1;
  EXPECT(!would_accept(set, f, now), "frame past its expires → reject");
}

void test_unsynced_clock_accepts_stale_effective() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB);
  f.effective = 1800000000ULL;
  f.expires = 1800000000ULL + 3600;
  // A receiver whose wall clock has never synced cannot judge freshness;
  // spec §7.1 step 4 says accept and flag rather than go deaf.
  EXPECT(would_accept(set, f, /*now_unix=*/1000),
         "unsynced clock → accept but flag (never drop for freshness)");
}

// ───────────────────────────────────────────────────────────────────────────
// Finding 2 — CANCEL / UPDATE must name the active alarm
// ───────────────────────────────────────────────────────────────────────────

void test_cancel_must_reference_the_active_alarm() {
  Receiver rx;
  rx.set = { mk(0xAA), mk(0xBB), mk(0xCC) };
  Frame alert = mk_frame(0xAA, 0xBB);
  EXPECT(rx.receive(alert) == Outcome::Audited, "alarm raised");
  EXPECT(rx.alarm_valid, "alarm is active");

  // Pre-fix this cleared the alarm: the reference check compared zero bytes.
  Frame blank = mk_cancel(0xAA, 0xBB, nullptr);
  EXPECT(rx.receive(blank) == Outcome::Audited,
         "an unreferenced CANCEL is still audited");
  EXPECT(rx.alarm_valid, "an all-zero ref_canceled_nonce does not clear the alarm");

  uint8_t wrong[BEACON_NONCE_SIZE];
  std::memset(wrong, 0x77, BEACON_NONCE_SIZE);
  Frame stranger = mk_cancel(0xCC, 0xBB, wrong);
  EXPECT(rx.receive(stranger) == Outcome::Audited,
         "a CANCEL naming a different alarm is still audited");
  EXPECT(rx.alarm_valid,
         "a CANCEL from another pair about another event does not clear this alarm");

  Frame proper = mk_cancel(0xAA, 0xBB, alert.nonce);
  EXPECT(rx.receive(proper) == Outcome::Audited, "matching CANCEL accepted");
  EXPECT(!rx.alarm_valid, "CANCEL naming the active alarm clears it (§7.2)");
}

void test_update_must_reference_the_active_alarm() {
  Receiver rx;
  rx.set = { mk(0xAA), mk(0xBB) };

  // Pre-fix an UPDATE installed itself as the active alarm even with no
  // alarm in force and no reference at all.
  Frame orphan = mk_frame(0xAA, 0xBB);
  orphan.hdr_msg_type = MSG_UPDATE;
  orphan.canon_msg_type = MSG_UPDATE;
  EXPECT(rx.receive(orphan) == Outcome::Audited, "orphan UPDATE is audited");
  EXPECT(!rx.alarm_valid, "an UPDATE with no alarm in force raises no alarm");

  Frame alert = mk_frame(0xAA, 0xBB);
  EXPECT(rx.receive(alert) == Outcome::Audited, "alarm raised");

  Frame unref = mk_frame(0xAA, 0xBB);
  unref.hdr_msg_type = MSG_UPDATE;
  unref.canon_msg_type = MSG_UPDATE;
  unref.template_id = 0x23;  // BCN_EMERG_EVACUATION
  EXPECT(rx.receive(unref) == Outcome::Audited, "unreferenced UPDATE is audited");
  EXPECT(rx.alarm_template == TPL_FIRE_VISIBLE,
         "an UPDATE that names no alarm does not amend the active one");

  Frame amend = mk_frame(0xAA, 0xBB);
  amend.hdr_msg_type = MSG_UPDATE;
  amend.canon_msg_type = MSG_UPDATE;
  amend.template_id = 0x23;
  std::memcpy(amend.ref_canceled_nonce, alert.nonce, BEACON_NONCE_SIZE);
  EXPECT(rx.receive(amend) == Outcome::Audited, "referenced UPDATE accepted");
  EXPECT(rx.alarm_template == 0x23, "a referenced UPDATE amends the active alarm");

  // The alarm keeps the ORIGINATING nonce as its identity, so a CANCEL that
  // names the original ALERT still resolves after an amendment.
  Frame cancel = mk_cancel(0xAA, 0xBB, alert.nonce);
  EXPECT(rx.receive(cancel) == Outcome::Audited, "CANCEL after UPDATE accepted");
  EXPECT(!rx.alarm_valid,
         "a CANCEL naming the originating ALERT still clears an amended alarm");
}

// ───────────────────────────────────────────────────────────────────────────
// Finding 3 — drills keep their own rate bucket
// ───────────────────────────────────────────────────────────────────────────

Frame mk_exercise(uint8_t orig, uint8_t cosign) {
  Frame f = mk_frame(orig, cosign);
  f.hdr_msg_type = MSG_EXERCISE;
  f.canon_msg_type = MSG_EXERCISE;
  f.flags = BCN_FLAG_IS_EXERCISE;
  return f;
}

void test_drills_do_not_exhaust_the_alert_bucket() {
  Receiver rx;
  rx.set = { mk(0xAA), mk(0xBB) };
  // A morning of drills — the whole exercise budget from one neighbor.
  for (int i = 0; i < MAX_ORIGINATIONS_PER_PUBKEY_24H; i++) {
    EXPECT(rx.receive(mk_exercise(0xAA, 0xBB)) == Outcome::Audited,
           "drill accepted inside the exercise budget");
  }
  EXPECT(rx.receive(mk_exercise(0xAA, 0xBB)) == Outcome::RejectedRate,
         "the exercise bucket is itself capped");
  EXPECT(!rx.alarm_valid, "drills never raise an alarm");

  // AGENTS.md Beacon invariant 10: that afternoon's fire must still land.
  EXPECT(rx.receive(mk_frame(0xAA, 0xBB)) == Outcome::Audited,
         "a real ALERT is unaffected by a spent drill bucket");
  EXPECT(rx.alarm_valid, "the real alert raises the alarm");
}

void test_alerts_do_not_exhaust_the_drill_bucket() {
  Receiver rx;
  rx.set = { mk(0xAA), mk(0xBB) };
  for (int i = 0; i < MAX_ORIGINATIONS_PER_PUBKEY_24H; i++) {
    EXPECT(rx.receive(mk_frame(0xAA, 0xBB)) == Outcome::Audited,
           "alert accepted inside the alert budget");
  }
  EXPECT(rx.receive(mk_exercise(0xAA, 0xBB)) == Outcome::Audited,
         "a drill still runs after the alert budget is spent");
}

// ───────────────────────────────────────────────────────────────────────────
// Finding 7 — template must be in the life-safety set
// ───────────────────────────────────────────────────────────────────────────

void test_template_outside_life_safety_set_rejected() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB);
  f.template_id = TPL_NOT_BEACON;
  EXPECT(!would_accept(set, f),
         "template 0x00 (police activity in Chirp's numbering) → reject");
  f.template_id = 0x99;
  EXPECT(!would_accept(set, f), "junk template byte → reject");
  f.template_id = TPL_FALSE_ALARM;
  EXPECT(would_accept(set, f), "a life-safety template is accepted");
}

// ───────────────────────────────────────────────────────────────────────────
// Finding 4 — signer supervised health (spec §7.1 step 9)
// ───────────────────────────────────────────────────────────────────────────

void test_stale_signer_selftest_rejected() {
  const uint32_t now_ms = SELFTEST_MISSING_MS + 100000;
  // Originator last checked in more than 36 h ago.
  std::vector<SetEntry> set = { mk(0xAA, 0, /*last_selftest_ms=*/1), mk(0xBB, 0, now_ms) };
  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(!would_accept(set, f, 1800000000ULL, now_ms),
         "signer whose selftest lapsed past 36 h does not authorize alarms");

  std::vector<SetEntry> cosigner_stale = { mk(0xAA, 0, now_ms), mk(0xBB, 0, 1) };
  EXPECT(!would_accept(cosigner_stale, f, 1800000000ULL, now_ms),
         "a stale cosigner is refused the same way");

  std::vector<SetEntry> healthy = { mk(0xAA, 0, now_ms), mk(0xBB, 0, now_ms) };
  EXPECT(would_accept(healthy, f, 1800000000ULL, now_ms),
         "two recently supervised signers are accepted");
}

void test_unobserved_selftest_is_not_treated_as_stale() {
  const uint32_t now_ms = SELFTEST_MISSING_MS + 100000;
  // last_selftest == 0 means the RAM-only map has no reading yet (fresh
  // boot), not that the neighbor is dead. Gating on it would deafen a
  // just-rebooted receiver for a full selftest cadence.
  std::vector<SetEntry> set = { mk(0xAA, 0, 0), mk(0xBB, 0, 0) };
  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(would_accept(set, f, 1800000000ULL, now_ms),
         "no selftest observed since boot → unknown, not stale");
}

// ───────────────────────────────────────────────────────────────────────────
// Finding 8 — SELFTEST_OK replay (mirrors handle_selftest_frame)
// ───────────────────────────────────────────────────────────────────────────

// The signed selftest payload carries a wall-clock timestamp (spec §5.3).
// Everything else in it repeats between emissions, so monotonicity is the
// only thing separating a live heartbeat from a captured frame rebroadcast.
struct SelfTestReceiver {
  uint64_t last_accepted_ts = 0;
  uint32_t last_seen_ms = 0;
  bool accept(uint64_t signed_ts, uint64_t now_unix, uint32_t now_ms) {
    if (now_unix >= 1700000000ULL) {
      const uint64_t skew = (signed_ts > now_unix) ? (signed_ts - now_unix)
                                                   : (now_unix - signed_ts);
      if (skew > BEACON_FRESHNESS_S) return false;
    }
    if (signed_ts <= last_accepted_ts) return false;
    last_accepted_ts = signed_ts;
    last_seen_ms = now_ms;
    return true;
  }
};

void test_selftest_replay_does_not_refresh_a_dead_neighbor() {
  SelfTestReceiver rx;
  const uint64_t t0 = 1800000000ULL;
  EXPECT(rx.accept(t0, t0, 1000), "first selftest accepted");
  EXPECT(!rx.accept(t0, t0 + 10, 11000),
         "the same signed frame replayed → refused (timestamp did not advance)");
  EXPECT(rx.last_seen_ms == 1000,
         "a replay does not move last_selftest, so the 36 h gap still opens");
  EXPECT(rx.accept(t0 + 86400, t0 + 86400, 90000000),
         "the neighbor's next genuine daily selftest advances the timestamp");
}

void test_selftest_outside_freshness_window_rejected() {
  SelfTestReceiver rx;
  const uint64_t now = 1800000000ULL;
  EXPECT(!rx.accept(now - (BEACON_FRESHNESS_S + 1), now, 0),
         "a selftest replayed hours later carries a stale signed timestamp");
  EXPECT(rx.accept(now, now, 0), "a current selftest is accepted");
}

void test_selftest_monotonic_when_clock_unsynced() {
  SelfTestReceiver rx;
  // An unsynced receiver cannot judge the window, but monotonicity still
  // refuses the replay.
  EXPECT(rx.accept(1800000000ULL, 1000, 0), "unsynced receiver accepts the first");
  EXPECT(!rx.accept(1800000000ULL, 1000, 5000),
         "unsynced receiver still refuses the replay");
}

} // namespace

int main() {
  test_happy_path_two_signatures();
  test_reject_single_signature();
  test_reject_originator_equals_cosigner();
  test_reject_unpaired_originator();
  test_reject_unpaired_cosigner();
  test_reject_revoked_originator();
  test_reject_revoked_cosigner();
  test_reject_wrong_magic();
  test_reject_wrong_scope();

  test_header_msg_type_must_match_canonical();
  test_exercise_requires_exercise_flag();
  test_real_alert_cannot_wear_the_exercise_flag();

  test_replayed_frame_is_dropped();
  test_replay_with_a_fresh_header_nonce_is_still_dropped();
  test_held_copy_past_the_ring_horizon_cannot_re_raise_the_alarm();
  test_replay_does_not_consume_the_rate_bucket();
  test_rate_bucket_still_caps_distinct_originations();
  test_stale_effective_is_rejected();
  test_expired_frame_is_rejected();
  test_unsynced_clock_accepts_stale_effective();

  test_cancel_must_reference_the_active_alarm();
  test_update_must_reference_the_active_alarm();

  test_drills_do_not_exhaust_the_alert_bucket();
  test_alerts_do_not_exhaust_the_drill_bucket();

  test_template_outside_life_safety_set_rejected();

  test_stale_signer_selftest_rejected();
  test_unobserved_selftest_is_not_treated_as_stale();

  test_selftest_replay_does_not_refresh_a_dead_neighbor();
  test_selftest_outside_freshness_window_rejected();
  test_selftest_monotonic_when_clock_unsynced();

  if (failures == 0) {
    std::printf("All beacon origination invariants passed.\n");
    return 0;
  } else {
    std::printf("%d failures.\n", failures);
    return 1;
  }
}
