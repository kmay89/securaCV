/**
 * @file test_mesh_rekey.cpp
 * @brief Host-build test for the opera_secret rotation state machine
 *        (mesh_rekey, F10-rekey option B). CRYPTO: maintainer review
 *        required before merge; bench-gated (U1 Track C3).
 *
 * mesh_rekey holds no globals, so an initiator and several survivors run
 * the whole exchange side by side in one process — the two-sided test the
 * singleton mesh_session cannot give. (test_mesh_session.cpp drives the
 * session glue against one of these contexts on the far side.)
 *
 * Verifies:
 *   1. Happy path, two survivors: OFFER → ACCEPT → SECRET → ACK → COMMIT;
 *      every survivor installs the SAME new secret the initiator commits,
 *      and learns which device was removed; no survivor dropped.
 *   2. Per-peer ACK: the commit waits for the last ACK; an ACK before an
 *      ACCEPT, from a non-survivor, or with a stale rekey_id counts
 *      nothing.
 *   3. Timeout: at exactly REKEY_TIMEOUT_MS the initiator commits and
 *      reports the silent survivor as dropped; a survivor that never got
 *      its SECRET aborts (rollback: keeps the old secret, context idle).
 *   4. The removed device ignores an OFFER that names it; its ACCEPT (it
 *      is no longer a survivor) is ignored by the initiator.
 *   5. A SECRET for survivor A does not decrypt at survivor B (the key and
 *      the AAD are per-recipient); a tampered SECRET does not install; a
 *      SECRET from anyone but the initiator is ignored.
 *   6. One rotation at a time: a second start() is refused while one is in
 *      flight; a survivor ignores a second initiator's OFFER but answers a
 *      retransmitted OFFER with the same ACCEPT.
 *   7. No survivors: start() commits at once.
 *   8. Wrong payload lengths are ignored everywhere; secret material is
 *      wiped from the contexts after commit / install / abort.
 *   9. Payload sizes pinned (they ride inside the 128-byte envelope cap).
 *  10. Retransmission: the initiator re-broadcasts the same OFFER every
 *      REKEY_RETRY_MS inside the window, which heals a lost OFFER and a
 *      lost SECRET; it stops at commit, and survivors never retransmit.
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/canary/lib/securacv_mesh/test_mesh_rekey.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_rekey.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_crypto.cpp \
 *       -I firmware/canary/lib/securacv_mesh/src \
 *       -o /tmp/test_mesh_rekey && /tmp/test_mesh_rekey
 */

#include "mesh_rekey.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_mesh_rekey_run() { return 0; }
#else

namespace {

using mesh_rekey::Action;
using mesh_rekey::ActionType;
using mesh_rekey::Context;
using mesh_rekey::MsgType;

constexpr size_t FP = mesh_rekey::FP_LEN;

const uint8_t FP_I[FP] = {0x10, 1, 1, 1, 1, 1, 1, 1};   /* initiator */
const uint8_t FP_A[FP] = {0x20, 2, 2, 2, 2, 2, 2, 2};   /* survivor A */
const uint8_t FP_B[FP] = {0x30, 3, 3, 3, 3, 3, 3, 3};   /* survivor B */
const uint8_t FP_X[FP] = {0x40, 4, 4, 4, 4, 4, 4, 4};   /* removed */

bool all_zero(const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) if (b[i] != 0) return false;
  return true;
}

/* Initiator I removes X; survivors A and B. */
Action start_ab(Context& ci, uint32_t rekey_id, uint32_t now) {
  const uint8_t surv[2][FP] = {
    {FP_A[0], FP_A[1], FP_A[2], FP_A[3], FP_A[4], FP_A[5], FP_A[6], FP_A[7]},
    {FP_B[0], FP_B[1], FP_B[2], FP_B[3], FP_B[4], FP_B[5], FP_B[6], FP_B[7]},
  };
  return mesh_rekey::start(ci, FP_I, FP_X, surv, 2, rekey_id, now);
}

Action rx(Context& c, const uint8_t my[FP], MsgType t, const uint8_t from[FP],
          const Action& sent, uint32_t now) {
  return mesh_rekey::receive(c, my, t, from, sent.payload, sent.payload_len, now);
}

void test_payload_sizes_pinned() {
  assert(mesh_rekey::OFFER_LEN == 44);
  assert(mesh_rekey::ACCEPT_LEN == 36);
  assert(mesh_rekey::SECRET_MSG_LEN == 64);
  assert(mesh_rekey::ACK_LEN == 4);
  assert(mesh_rekey::MAX_PAYLOAD_LEN <= 128);   /* mesh_envelope::MAX_PAYLOAD_LEN */
  assert(static_cast<uint8_t>(MsgType::OFFER) == 26);
  assert(static_cast<uint8_t>(MsgType::ACK) == 29);
  std::printf("PASS test_payload_sizes_pinned\n");
}

void test_happy_path_two_survivors() {
  Context ci, ca, cb;
  mesh_rekey::context_init(ci);
  mesh_rekey::context_init(ca);
  mesh_rekey::context_init(cb);

  Action offer = start_ab(ci, 0xA1B2C3D4u, 1000);
  assert(offer.type == ActionType::BROADCAST_OFFER);
  assert(offer.msg_type == MsgType::OFFER);
  assert(offer.payload_len == mesh_rekey::OFFER_LEN);
  assert(std::memcmp(offer.payload + 4 + 32, FP_X, FP) == 0);   /* names the removed */
  assert(mesh_rekey::in_progress(ci));

  Action acc_a = rx(ca, FP_A, MsgType::OFFER, FP_I, offer, 1010);
  Action acc_b = rx(cb, FP_B, MsgType::OFFER, FP_I, offer, 1010);
  assert(acc_a.type == ActionType::SEND_ACCEPT && acc_b.type == ActionType::SEND_ACCEPT);
  assert(std::memcmp(acc_a.dest_fp, FP_I, FP) == 0);
  /* Fresh ephemeral key per survivor. */
  assert(std::memcmp(acc_a.payload + 4, acc_b.payload + 4, 32) != 0);

  Action sec_a = rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc_a, 1020);
  Action sec_b = rx(ci, FP_I, MsgType::ACCEPT, FP_B, acc_b, 1020);
  assert(sec_a.type == ActionType::SEND_SECRET && sec_b.type == ActionType::SEND_SECRET);
  assert(std::memcmp(sec_a.dest_fp, FP_A, FP) == 0);
  assert(std::memcmp(sec_b.dest_fp, FP_B, FP) == 0);
  assert(sec_a.payload_len == mesh_rekey::SECRET_MSG_LEN);

  Action inst_a = rx(ca, FP_A, MsgType::SECRET, FP_I, sec_a, 1030);
  assert(inst_a.type == ActionType::ACK_AND_INSTALL);
  assert(inst_a.msg_type == MsgType::ACK);
  assert(std::memcmp(inst_a.dest_fp, FP_I, FP) == 0);
  assert(std::memcmp(inst_a.removed_fp, FP_X, FP) == 0);
  assert(!mesh_rekey::in_progress(ca));

  /* One ACK of two: no commit yet (per-peer ACK). */
  Action none1 = rx(ci, FP_I, MsgType::ACK, FP_A, inst_a, 1040);
  assert(none1.type == ActionType::NONE);
  assert(mesh_rekey::in_progress(ci));

  Action inst_b = rx(cb, FP_B, MsgType::SECRET, FP_I, sec_b, 1050);
  assert(inst_b.type == ActionType::ACK_AND_INSTALL);
  Action commit = rx(ci, FP_I, MsgType::ACK, FP_B, inst_b, 1060);
  assert(commit.type == ActionType::COMMIT);
  assert(commit.dropped_count == 0);
  assert(std::memcmp(commit.removed_fp, FP_X, FP) == 0);

  /* Everyone holds the same new secret, and it is not all-zero. */
  assert(!all_zero(commit.new_secret, sizeof(commit.new_secret)));
  assert(std::memcmp(inst_a.new_secret, commit.new_secret, 32) == 0);
  assert(std::memcmp(inst_b.new_secret, commit.new_secret, 32) == 0);

  /* Contexts are idle and hold no secret material any more. */
  assert(!mesh_rekey::in_progress(ci));
  assert(ci.role == mesh_rekey::Role::IDLE);
  assert(all_zero(ci.new_secret, 32) && all_zero(ci.eph_priv, 32));
  assert(all_zero(ca.eph_priv, 32) && all_zero(cb.eph_priv, 32));

  /* wipe() clears an applied action. */
  mesh_rekey::wipe(commit);
  assert(all_zero(commit.new_secret, 32));
  std::printf("PASS test_happy_path_two_survivors\n");
}

void test_ack_gating() {
  Context ci, ca, cb;
  mesh_rekey::context_init(ci);
  mesh_rekey::context_init(ca);
  mesh_rekey::context_init(cb);
  Action offer = start_ab(ci, 7, 0);
  Action acc_a = rx(ca, FP_A, MsgType::OFFER, FP_I, offer, 1);
  Action acc_b = rx(cb, FP_B, MsgType::OFFER, FP_I, offer, 1);
  (void)acc_b;

  /* B ACKs before its ACCEPT was processed: counts nothing. */
  Action fake_ack = {};
  fake_ack.payload[0] = 7;
  fake_ack.payload_len = mesh_rekey::ACK_LEN;
  assert(rx(ci, FP_I, MsgType::ACK, FP_B, fake_ack, 2).type == ActionType::NONE);

  Action sec_a = rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc_a, 2);
  Action inst_a = rx(ca, FP_A, MsgType::SECRET, FP_I, sec_a, 3);
  assert(inst_a.type == ActionType::ACK_AND_INSTALL);

  /* Stale rekey_id ACK from A: ignored. */
  Action stale = inst_a;
  stale.payload[0] ^= 0xFF;
  assert(rx(ci, FP_I, MsgType::ACK, FP_A, stale, 4).type == ActionType::NONE);
  /* ACK from the removed device / a stranger: ignored. */
  assert(rx(ci, FP_I, MsgType::ACK, FP_X, inst_a, 4).type == ActionType::NONE);
  /* Wrong length: ignored. */
  Action longer = inst_a;
  longer.payload_len = mesh_rekey::ACK_LEN + 1;
  assert(rx(ci, FP_I, MsgType::ACK, FP_A, longer, 4).type == ActionType::NONE);
  /* The genuine one counts; B still outstanding → no commit. */
  assert(rx(ci, FP_I, MsgType::ACK, FP_A, inst_a, 5).type == ActionType::NONE);
  assert(mesh_rekey::in_progress(ci));
  std::printf("PASS test_ack_gating\n");
}

void test_timeout_commits_and_drops_silent_peer() {
  Context ci, ca, cb;
  mesh_rekey::context_init(ci);
  mesh_rekey::context_init(ca);
  mesh_rekey::context_init(cb);
  const uint32_t t0 = 0xFFFFF000u;   /* crosses the u32 wrap on purpose */
  Action offer = start_ab(ci, 99, t0);
  Action acc_a = rx(ca, FP_A, MsgType::OFFER, FP_I, offer, t0 + 10);
  Action acc_b = rx(cb, FP_B, MsgType::OFFER, FP_I, offer, t0 + 10);
  Action sec_a = rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc_a, t0 + 20);
  (void)acc_b;   /* B's ACCEPT is lost */
  Action inst_a = rx(ca, FP_A, MsgType::SECRET, FP_I, sec_a, t0 + 30);
  assert(rx(ci, FP_I, MsgType::ACK, FP_A, inst_a, t0 + 40).type == ActionType::NONE);

  /* Just inside the window B is still out, so the initiator re-offers
   * (retransmission — see test_retransmit_heals_lost_frames); it does not
   * commit early. */
  assert(mesh_rekey::tick(ci, t0 + mesh_rekey::REKEY_TIMEOUT_MS - 1).type ==
         ActionType::BROADCAST_OFFER);
  Action commit = mesh_rekey::tick(ci, t0 + mesh_rekey::REKEY_TIMEOUT_MS);
  assert(commit.type == ActionType::COMMIT);
  assert(commit.dropped_count == 1);
  assert(std::memcmp(commit.dropped[0], FP_B, FP) == 0);
  assert(std::memcmp(commit.new_secret, inst_a.new_secret, 32) == 0);
  assert(!mesh_rekey::in_progress(ci));

  /* Rollback on the survivor side: B never got its SECRET, so it aborts
   * at its own timeout and keeps the old secret (nothing to install). */
  assert(mesh_rekey::in_progress(cb));
  assert(mesh_rekey::tick(cb, t0 + 10 + mesh_rekey::REKEY_TIMEOUT_MS - 1).type == ActionType::NONE);
  Action abort_b = mesh_rekey::tick(cb, t0 + 10 + mesh_rekey::REKEY_TIMEOUT_MS);
  assert(abort_b.type == ActionType::ABORT);
  assert(all_zero(abort_b.new_secret, 32));
  assert(!mesh_rekey::in_progress(cb));
  assert(all_zero(cb.eph_priv, 32));
  std::printf("PASS test_timeout_commits_and_drops_silent_peer\n");
}

/* Review finding (fw-mesh #7): nothing was retransmitted, so one lost
 * OFFER, ACCEPT or SECRET dropped a healthy survivor at 60 s. The
 * initiator now re-broadcasts the same OFFER every REKEY_RETRY_MS. */
void test_retransmit_heals_lost_frames() {
  Context ci, ca, cb;
  mesh_rekey::context_init(ci);
  mesh_rekey::context_init(ca);
  mesh_rekey::context_init(cb);
  const uint32_t t0 = 0xFFFFE000u;   /* the retry clock crosses the u32 wrap too */
  const uint32_t R = mesh_rekey::REKEY_RETRY_MS;
  Action offer = start_ab(ci, 0x5157u, t0);

  /* A hears the OFFER, B does not. A's SECRET is then lost in the air. */
  Action acc_a = rx(ca, FP_A, MsgType::OFFER, FP_I, offer, t0 + 10);
  Action sec_a_lost = rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc_a, t0 + 20);
  assert(sec_a_lost.type == ActionType::SEND_SECRET);

  /* No retransmission before REKEY_RETRY_MS; then the SAME OFFER, once. */
  assert(mesh_rekey::tick(ci, t0 + R - 1).type == ActionType::NONE);
  Action again = mesh_rekey::tick(ci, t0 + R);
  assert(again.type == ActionType::BROADCAST_OFFER);
  assert(again.payload_len == offer.payload_len);
  assert(std::memcmp(again.payload, offer.payload, offer.payload_len) == 0);
  assert(mesh_rekey::tick(ci, t0 + R + 1).type == ActionType::NONE);

  /* B answers the retransmission and completes. */
  Action acc_b = rx(cb, FP_B, MsgType::OFFER, FP_I, again, t0 + R + 10);
  assert(acc_b.type == ActionType::SEND_ACCEPT);
  Action sec_b = rx(ci, FP_I, MsgType::ACCEPT, FP_B, acc_b, t0 + R + 20);
  Action inst_b = rx(cb, FP_B, MsgType::SECRET, FP_I, sec_b, t0 + R + 30);
  assert(inst_b.type == ActionType::ACK_AND_INSTALL);
  assert(rx(ci, FP_I, MsgType::ACK, FP_B, inst_b, t0 + R + 40).type == ActionType::NONE);

  /* A, still waiting for its SECRET, re-sends the SAME ACCEPT, which draws
   * a fresh SECRET (new nonce) under the same key — and A completes. */
  Action acc_a2 = rx(ca, FP_A, MsgType::OFFER, FP_I, again, t0 + R + 10);
  assert(acc_a2.type == ActionType::SEND_ACCEPT);
  assert(std::memcmp(acc_a2.payload, acc_a.payload, acc_a.payload_len) == 0);
  Action sec_a = rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc_a2, t0 + R + 50);
  assert(sec_a.type == ActionType::SEND_SECRET);
  assert(std::memcmp(sec_a.payload, sec_a_lost.payload, sec_a.payload_len) != 0);
  Action inst_a = rx(ca, FP_A, MsgType::SECRET, FP_I, sec_a, t0 + R + 60);
  assert(inst_a.type == ActionType::ACK_AND_INSTALL);
  Action commit = rx(ci, FP_I, MsgType::ACK, FP_A, inst_a, t0 + R + 70);
  assert(commit.type == ActionType::COMMIT);
  assert(commit.dropped_count == 0);            /* nobody dropped */
  assert(std::memcmp(inst_a.new_secret, commit.new_secret, 32) == 0);
  assert(std::memcmp(inst_b.new_secret, commit.new_secret, 32) == 0);

  /* Committed: no more retransmissions. A survivor never retransmits. */
  assert(mesh_rekey::tick(ci, t0 + 3 * R).type == ActionType::NONE);
  Context cs;
  mesh_rekey::context_init(cs);
  Action offer2 = start_ab(ci, 0x5158u, t0);
  assert(rx(cs, FP_A, MsgType::OFFER, FP_I, offer2, t0).type == ActionType::SEND_ACCEPT);
  assert(mesh_rekey::tick(cs, t0 + R).type == ActionType::NONE);
  mesh_rekey::wipe(commit);
  std::printf("PASS test_retransmit_heals_lost_frames\n");
}

void test_removed_device_is_excluded() {
  Context ci, cx;
  mesh_rekey::context_init(ci);
  mesh_rekey::context_init(cx);
  Action offer = start_ab(ci, 5, 0);
  /* X hears the broadcast OFFER naming itself: it takes no part. */
  assert(rx(cx, FP_X, MsgType::OFFER, FP_I, offer, 1).type == ActionType::NONE);
  assert(!mesh_rekey::in_progress(cx));
  /* Even a hand-made ACCEPT from X gets no SECRET: X is not a survivor. */
  Context cfake;
  mesh_rekey::context_init(cfake);
  Action acc_fake = rx(cfake, FP_A, MsgType::OFFER, FP_I, offer, 1);   /* a valid-looking ACCEPT */
  assert(rx(ci, FP_I, MsgType::ACCEPT, FP_X, acc_fake, 2).type == ActionType::NONE);
  std::printf("PASS test_removed_device_is_excluded\n");
}

void test_secret_is_per_recipient_and_authenticated() {
  Context ci, ca, cb;
  mesh_rekey::context_init(ci);
  mesh_rekey::context_init(ca);
  mesh_rekey::context_init(cb);
  Action offer = start_ab(ci, 11, 0);
  Action acc_a = rx(ca, FP_A, MsgType::OFFER, FP_I, offer, 1);
  Action acc_b = rx(cb, FP_B, MsgType::OFFER, FP_I, offer, 1);
  Action sec_a = rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc_a, 2);
  Action sec_b = rx(ci, FP_I, MsgType::ACCEPT, FP_B, acc_b, 2);

  /* A's SECRET delivered to B: wrong key, wrong AAD → nothing installs. */
  assert(rx(cb, FP_B, MsgType::SECRET, FP_I, sec_a, 3).type == ActionType::NONE);
  assert(mesh_rekey::in_progress(cb));
  /* A tampered ciphertext / tag / nonce → nothing installs. */
  const size_t offsets[] = {4, 4 + 12, 4 + 12 + 32};   /* nonce, ciphertext, tag */
  for (size_t off : offsets) {
    Action bad = sec_b;
    bad.payload[off] ^= 0x01;
    assert(rx(cb, FP_B, MsgType::SECRET, FP_I, bad, 3).type == ActionType::NONE);
    assert(mesh_rekey::in_progress(cb));
  }
  /* The right SECRET from someone other than the initiator → ignored. */
  assert(rx(cb, FP_B, MsgType::SECRET, FP_A, sec_b, 3).type == ActionType::NONE);
  /* Wrong length → ignored. */
  Action shorter = sec_b;
  shorter.payload_len -= 1;
  assert(rx(cb, FP_B, MsgType::SECRET, FP_I, shorter, 3).type == ActionType::NONE);
  /* And the genuine one still installs afterwards. */
  assert(rx(cb, FP_B, MsgType::SECRET, FP_I, sec_b, 4).type == ActionType::ACK_AND_INSTALL);
  std::printf("PASS test_secret_is_per_recipient_and_authenticated\n");
}

void test_one_rotation_at_a_time() {
  Context ci, ca;
  mesh_rekey::context_init(ci);
  mesh_rekey::context_init(ca);
  Action offer = start_ab(ci, 21, 0);
  /* A second remove on the same device while in flight: refused. */
  assert(start_ab(ci, 22, 1).type == ActionType::NONE);
  assert(ci.rekey_id == 21);

  Action acc1 = rx(ca, FP_A, MsgType::OFFER, FP_I, offer, 1);
  assert(acc1.type == ActionType::SEND_ACCEPT);
  /* A retransmitted OFFER gets the SAME ACCEPT (same ephemeral key). */
  Action acc2 = rx(ca, FP_A, MsgType::OFFER, FP_I, offer, 2);
  assert(acc2.type == ActionType::SEND_ACCEPT);
  assert(std::memcmp(acc1.payload, acc2.payload, mesh_rekey::ACCEPT_LEN) == 0);
  /* A second initiator's OFFER while A is mid-transaction: ignored. */
  Context cj;
  mesh_rekey::context_init(cj);
  const uint8_t surv[1][FP] = {{FP_A[0], FP_A[1], FP_A[2], FP_A[3], FP_A[4], FP_A[5], FP_A[6], FP_A[7]}};
  Action offer2 = mesh_rekey::start(cj, FP_B, FP_X, surv, 1, 77, 2);
  assert(offer2.type == ActionType::BROADCAST_OFFER);
  assert(rx(ca, FP_A, MsgType::OFFER, FP_B, offer2, 3).type == ActionType::NONE);
  /* An initiator ignores OFFERs while it is initiating. */
  assert(rx(ci, FP_I, MsgType::OFFER, FP_B, offer2, 3).type == ActionType::NONE);
  /* A changed ephemeral key from an already-accepted survivor is ignored. */
  Action sec = rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc1, 4);
  assert(sec.type == ActionType::SEND_SECRET);
  Action acc_swapped = acc1;
  acc_swapped.payload[4] ^= 0x01;
  assert(rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc_swapped, 5).type == ActionType::NONE);
  /* ...while a duplicate of the same ACCEPT re-sends a SECRET that still
   * decrypts (fresh nonce, same key). */
  Action sec_again = rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc1, 5);
  assert(sec_again.type == ActionType::SEND_SECRET);
  assert(std::memcmp(sec.payload + 4, sec_again.payload + 4, 12) != 0);
  assert(rx(ca, FP_A, MsgType::SECRET, FP_I, sec_again, 6).type == ActionType::ACK_AND_INSTALL);
  std::printf("PASS test_one_rotation_at_a_time\n");
}

void test_no_survivors_commits_immediately() {
  Context ci;
  mesh_rekey::context_init(ci);
  Action a = mesh_rekey::start(ci, FP_I, FP_X, nullptr, 0, 3, 0);
  assert(a.type == ActionType::COMMIT);
  assert(a.dropped_count == 0);
  assert(!all_zero(a.new_secret, 32));
  assert(!mesh_rekey::in_progress(ci));
  assert(all_zero(ci.new_secret, 32));
  /* Two rotations never produce the same secret. */
  Action b = mesh_rekey::start(ci, FP_I, FP_X, nullptr, 0, 4, 0);
  assert(std::memcmp(a.new_secret, b.new_secret, 32) != 0);
  std::printf("PASS test_no_survivors_commits_immediately\n");
}

void test_bad_inputs() {
  Context c;
  mesh_rekey::context_init(c);
  assert(mesh_rekey::start(c, nullptr, FP_X, nullptr, 0, 1, 0).type == ActionType::NONE);
  assert(mesh_rekey::start(c, FP_I, nullptr, nullptr, 0, 1, 0).type == ActionType::NONE);
  assert(mesh_rekey::start(c, FP_I, FP_X, nullptr, 1, 1, 0).type == ActionType::NONE);
  uint8_t many[mesh_rekey::MAX_SURVIVORS + 1][FP] = {};
  assert(mesh_rekey::start(c, FP_I, FP_X, many, mesh_rekey::MAX_SURVIVORS + 1, 1, 0).type
         == ActionType::NONE);
  assert(!mesh_rekey::in_progress(c));
  /* An OFFER of the wrong length, and inbound frames while idle. */
  Action junk = {};
  junk.payload_len = mesh_rekey::OFFER_LEN - 1;
  assert(rx(c, FP_A, MsgType::OFFER, FP_I, junk, 0).type == ActionType::NONE);
  junk.payload_len = mesh_rekey::ACCEPT_LEN;
  assert(rx(c, FP_A, MsgType::ACCEPT, FP_I, junk, 0).type == ActionType::NONE);
  junk.payload_len = mesh_rekey::SECRET_MSG_LEN;
  assert(rx(c, FP_A, MsgType::SECRET, FP_I, junk, 0).type == ActionType::NONE);
  junk.payload_len = mesh_rekey::ACK_LEN;
  assert(rx(c, FP_A, MsgType::ACK, FP_I, junk, 0).type == ActionType::NONE);
  assert(mesh_rekey::receive(c, FP_A, MsgType::ACK, FP_I, nullptr, 4, 0).type == ActionType::NONE);
  assert(mesh_rekey::tick(c, 1000000).type == ActionType::NONE);
  std::printf("PASS test_bad_inputs\n");
}

}  /* namespace */

int main() {
  std::srand(0x5EC5E7);
  test_payload_sizes_pinned();
  test_happy_path_two_survivors();
  test_ack_gating();
  test_timeout_commits_and_drops_silent_peer();
  test_retransmit_heals_lost_frames();
  test_removed_device_is_excluded();
  test_secret_is_per_recipient_and_authenticated();
  test_one_rotation_at_a_time();
  test_no_survivors_commits_immediately();
  test_bad_inputs();
  std::printf("\nALL MESH_REKEY TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
