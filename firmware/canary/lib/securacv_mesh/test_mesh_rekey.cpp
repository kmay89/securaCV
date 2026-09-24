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
 *  11. F33 part 6 — two removals from two devices at once: a whole
 *      household (six devices, each running this state machine behind the
 *      same receive rules mesh_session applies — unknown sender dropped,
 *      opera_id checked, every verified OFFER's removal deny-listed and
 *      forgotten, the ACK sent before the switch) ends on ONE new secret,
 *      with neither removed device trusted by any member nor holding that
 *      secret, and both on every member's deny-list: simultaneous starts in
 *      both precedence orders, a start two seconds later by a device that
 *      lost the first OFFER, and a second removal refused mid-rotation and
 *      retried. Plus the settle window, the yield, the survivor switch and
 *      drop_survivor() on their own.
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
#include "mesh_revocation.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_mesh_rekey_run() { return 0; }
#else

namespace {

using mesh_rekey::Action;
using mesh_rekey::ActionType;
using mesh_rekey::Context;
using mesh_rekey::MsgType;

constexpr size_t FP = mesh_rekey::FP_LEN;
/* F33 part 6: an initiator answers no ACCEPT with a SECRET inside the
 * settle window; the pre-F33 flows below run their ACCEPTs past it. */
constexpr uint32_t SET = mesh_rekey::REKEY_SETTLE_MS;

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

  Action sec_a = rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc_a, 1000 + SET + 20);
  Action sec_b = rx(ci, FP_I, MsgType::ACCEPT, FP_B, acc_b, 1000 + SET + 20);
  assert(sec_a.type == ActionType::SEND_SECRET && sec_b.type == ActionType::SEND_SECRET);
  assert(std::memcmp(sec_a.dest_fp, FP_A, FP) == 0);
  assert(std::memcmp(sec_b.dest_fp, FP_B, FP) == 0);
  assert(sec_a.payload_len == mesh_rekey::SECRET_MSG_LEN);

  Action inst_a = rx(ca, FP_A, MsgType::SECRET, FP_I, sec_a, 1000 + SET + 30);
  assert(inst_a.type == ActionType::ACK_AND_INSTALL);
  assert(inst_a.msg_type == MsgType::ACK);
  assert(std::memcmp(inst_a.dest_fp, FP_I, FP) == 0);
  assert(std::memcmp(inst_a.removed_fp, FP_X, FP) == 0);
  assert(!mesh_rekey::in_progress(ca));

  /* One ACK of two: no commit yet (per-peer ACK). */
  Action none1 = rx(ci, FP_I, MsgType::ACK, FP_A, inst_a, 1000 + SET + 40);
  assert(none1.type == ActionType::NONE);
  assert(mesh_rekey::in_progress(ci));

  Action inst_b = rx(cb, FP_B, MsgType::SECRET, FP_I, sec_b, 1000 + SET + 50);
  assert(inst_b.type == ActionType::ACK_AND_INSTALL);
  Action commit = rx(ci, FP_I, MsgType::ACK, FP_B, inst_b, 1000 + SET + 60);
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

  Action sec_a = rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc_a, SET + 2);
  Action inst_a = rx(ca, FP_A, MsgType::SECRET, FP_I, sec_a, SET + 3);
  assert(inst_a.type == ActionType::ACK_AND_INSTALL);

  /* Stale rekey_id ACK from A: ignored. */
  Action stale = inst_a;
  stale.payload[0] ^= 0xFF;
  assert(rx(ci, FP_I, MsgType::ACK, FP_A, stale, SET + 4).type == ActionType::NONE);
  /* ACK from the removed device / a stranger: ignored. */
  assert(rx(ci, FP_I, MsgType::ACK, FP_X, inst_a, SET + 4).type == ActionType::NONE);
  /* Wrong length: ignored. */
  Action longer = inst_a;
  longer.payload_len = mesh_rekey::ACK_LEN + 1;
  assert(rx(ci, FP_I, MsgType::ACK, FP_A, longer, SET + 4).type == ActionType::NONE);
  /* The genuine one counts; B still outstanding → no commit. */
  assert(rx(ci, FP_I, MsgType::ACK, FP_A, inst_a, SET + 5).type == ActionType::NONE);
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
  Action sec_a = rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc_a, t0 + SET + 20);
  (void)acc_b;   /* B's ACCEPT is lost */
  Action inst_a = rx(ca, FP_A, MsgType::SECRET, FP_I, sec_a, t0 + SET + 30);
  assert(rx(ci, FP_I, MsgType::ACK, FP_A, inst_a, t0 + SET + 40).type == ActionType::NONE);

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
  static_assert(mesh_rekey::REKEY_SETTLE_MS > mesh_rekey::REKEY_RETRY_MS,
                "a competitor must hear a retransmit inside the settle window");
  Action offer = start_ab(ci, 0x5157u, t0);

  /* A hears the OFFER, B does not. A's ACCEPT arrives inside the settle
   * window: kept, not answered yet (F33 part 6). */
  Action acc_a = rx(ca, FP_A, MsgType::OFFER, FP_I, offer, t0 + 10);
  assert(rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc_a, t0 + 20).type == ActionType::NONE);

  /* No retransmission before REKEY_RETRY_MS; then the SAME OFFER, once. */
  assert(mesh_rekey::tick(ci, t0 + R - 1).type == ActionType::NONE);
  Action again = mesh_rekey::tick(ci, t0 + R);
  assert(again.type == ActionType::BROADCAST_OFFER);
  assert(again.payload_len == offer.payload_len);
  assert(std::memcmp(again.payload, offer.payload, offer.payload_len) == 0);
  assert(mesh_rekey::tick(ci, t0 + R + 1).type == ActionType::NONE);

  /* The window closes: tick() answers the ACCEPT it held — and that
   * SECRET is then lost in the air. */
  assert(mesh_rekey::tick(ci, t0 + SET - 1).type == ActionType::NONE);
  Action sec_a_lost = mesh_rekey::tick(ci, t0 + SET);
  assert(sec_a_lost.type == ActionType::SEND_SECRET);
  assert(std::memcmp(sec_a_lost.dest_fp, FP_A, FP) == 0);
  assert(mesh_rekey::handed_out(ci));
  assert(mesh_rekey::tick(ci, t0 + SET + 1).type == ActionType::NONE);

  /* B answers the retransmission and completes (past the window: its
   * SECRET goes out at once). */
  Action acc_b = rx(cb, FP_B, MsgType::OFFER, FP_I, again, t0 + SET + 10);
  assert(acc_b.type == ActionType::SEND_ACCEPT);
  Action sec_b = rx(ci, FP_I, MsgType::ACCEPT, FP_B, acc_b, t0 + SET + 20);
  Action inst_b = rx(cb, FP_B, MsgType::SECRET, FP_I, sec_b, t0 + SET + 30);
  assert(inst_b.type == ActionType::ACK_AND_INSTALL);
  assert(rx(ci, FP_I, MsgType::ACK, FP_B, inst_b, t0 + SET + 40).type == ActionType::NONE);

  /* A, still waiting for its SECRET, re-sends the SAME ACCEPT to the
   * retransmission, which draws a fresh SECRET (new nonce) under the same
   * key — and A completes. */
  Action acc_a2 = rx(ca, FP_A, MsgType::OFFER, FP_I, again, t0 + SET + 10);
  assert(acc_a2.type == ActionType::SEND_ACCEPT);
  assert(std::memcmp(acc_a2.payload, acc_a.payload, acc_a.payload_len) == 0);
  Action sec_a = rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc_a2, t0 + SET + 50);
  assert(sec_a.type == ActionType::SEND_SECRET);
  assert(std::memcmp(sec_a.payload, sec_a_lost.payload, sec_a.payload_len) != 0);
  Action inst_a = rx(ca, FP_A, MsgType::SECRET, FP_I, sec_a, t0 + SET + 60);
  assert(inst_a.type == ActionType::ACK_AND_INSTALL);
  Action commit = rx(ci, FP_I, MsgType::ACK, FP_A, inst_a, t0 + SET + 70);
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
  Action sec_a = rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc_a, SET + 2);
  Action sec_b = rx(ci, FP_I, MsgType::ACCEPT, FP_B, acc_b, SET + 2);

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
  /* A second initiator's OFFER while A is mid-transaction: ignored — it
   * does not precede A's initiator (FP_B > FP_I; F33 precedence). */
  Context cj;
  mesh_rekey::context_init(cj);
  const uint8_t surv[1][FP] = {{FP_A[0], FP_A[1], FP_A[2], FP_A[3], FP_A[4], FP_A[5], FP_A[6], FP_A[7]}};
  Action offer2 = mesh_rekey::start(cj, FP_B, FP_X, surv, 1, 77, 2);
  assert(offer2.type == ActionType::BROADCAST_OFFER);
  assert(rx(ca, FP_A, MsgType::OFFER, FP_B, offer2, 3).type == ActionType::NONE);
  /* An initiator ignores the OFFER of an initiator it precedes. */
  assert(rx(ci, FP_I, MsgType::OFFER, FP_B, offer2, 3).type == ActionType::NONE);
  /* A changed ephemeral key from an already-accepted survivor is ignored. */
  Action sec = rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc1, SET + 4);
  assert(sec.type == ActionType::SEND_SECRET);
  Action acc_swapped = acc1;
  acc_swapped.payload[4] ^= 0x01;
  assert(rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc_swapped, SET + 5).type == ActionType::NONE);
  /* ...while a duplicate of the same ACCEPT re-sends a SECRET that still
   * decrypts (fresh nonce, same key). */
  Action sec_again = rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc1, SET + 5);
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

/* ── F33 part 6 — concurrent removals converge ────────────────────────── */

/* A device as mesh_session drives mesh_rekey: trusted fingerprints, its
 * current opera secret (frames carry it as their "opera_id"), a deny-list. */
struct SimDev {
  uint8_t fp[FP];
  Context ctx;
  uint8_t secret[32];
  std::vector<std::array<uint8_t, FP>> trusted;
  mesh_revocation::List deny;
  bool    reannounce = false;   /* mesh_session s_reannounce */
  uint8_t reannounce_fp[FP] = {0};
};

struct SimFrame {
  size_t from;
  bool   broadcast;
  uint8_t to_fp[FP];
  MsgType type;
  std::vector<uint8_t> payload;
  uint8_t tag[32];   /* the sender's secret when it signed = its opera_id */
};

struct Household {
  std::vector<SimDev> devs;
  std::deque<SimFrame> q;
  /* Loss model: return true to drop this delivery. */
  std::function<bool(const SimFrame&, size_t to)> lose = [](const SimFrame&, size_t) { return false; };
  uint32_t next_id = 1;

  bool trusts(const SimDev& d, const uint8_t fp[FP]) const {
    for (const auto& t : d.trusted) if (std::memcmp(t.data(), fp, FP) == 0) return true;
    return false;
  }
  void forget(SimDev& d, const uint8_t fp[FP]) {
    for (size_t i = 0; i < d.trusted.size(); ++i) {
      if (std::memcmp(d.trusted[i].data(), fp, FP) == 0) { d.trusted.erase(d.trusted.begin() + i); return; }
    }
  }
  /* mesh_session::revoke_peer. */
  void revoke(SimDev& d, const uint8_t fp[FP], uint32_t now) {
    mesh_revocation::add(d.deny, fp, now);
    forget(d, fp);
    mesh_rekey::drop_survivor(d.ctx, fp);
  }
  void send(size_t from, const Action& a, bool bcast) {
    SimFrame f;
    f.from = from;
    f.broadcast = bcast;
    std::memcpy(f.to_fp, a.dest_fp, FP);
    f.type = a.msg_type;
    f.payload.assign(a.payload, a.payload + a.payload_len);
    std::memcpy(f.tag, devs[from].secret, 32);
    q.push_back(f);
  }
  /* mesh_session::apply_rekey_action. */
  void apply(size_t i, Action& a) {
    SimDev& d = devs[i];
    switch (a.type) {
      case ActionType::BROADCAST_OFFER: send(i, a, true); break;
      case ActionType::SEND_ACCEPT:
      case ActionType::SEND_SECRET:     send(i, a, false); break;
      case ActionType::ACK_AND_INSTALL:
        send(i, a, false);                       /* the ACK, under the OLD secret */
        std::memcpy(d.secret, a.new_secret, 32);
        forget(d, a.removed_fp);
        break;
      case ActionType::COMMIT:
        std::memcpy(d.secret, a.new_secret, 32);
        for (size_t k = 0; k < a.dropped_count; ++k) forget(d, a.dropped[k]);
        forget(d, a.removed_fp);
        break;
      default: break;
    }
    mesh_rekey::wipe(a);
  }
  /* mesh_session's verified-frame path for REKEY_*. */
  void deliver(const SimFrame& f, size_t to, uint32_t now) {
    SimDev& d = devs[to];
    const uint8_t* sender = devs[f.from].fp;
    if (!trusts(d, sender)) return;                      /* unknown sender */
    if (std::memcmp(f.tag, d.secret, 32) != 0) return;   /* opera_id */
    if (f.type == MsgType::OFFER && f.payload.size() == mesh_rekey::OFFER_LEN) {
      const uint8_t* removed = f.payload.data() + 4 + 32;
      if (std::memcmp(removed, sender, FP) == 0) return;
      if (std::memcmp(removed, d.fp, FP) != 0) revoke(d, removed, now);
    }
    const bool was_initiator = d.ctx.role == mesh_rekey::Role::INITIATOR;
    uint8_t our_removed[FP];
    std::memcpy(our_removed, d.ctx.removed_fp, FP);
    Action a = mesh_rekey::receive(d.ctx, d.fp, f.type, sender,
                                   f.payload.data(), f.payload.size(), now);
    if (was_initiator && d.ctx.role == mesh_rekey::Role::SURVIVOR &&
        a.type != ActionType::COMMIT) {
      d.reannounce = true;                       /* we yielded */
      std::memcpy(d.reannounce_fp, our_removed, FP);
    }
    apply(to, a);
  }
  /* mesh_session::reannounce_if_due. */
  void reannounce(size_t i, uint32_t now) {
    SimDev& d = devs[i];
    if (!d.reannounce || mesh_rekey::in_progress(d.ctx)) return;
    uint8_t surv[mesh_rekey::MAX_SURVIVORS][FP];
    size_t n = 0;
    for (const auto& t : d.trusted) std::memcpy(surv[n++], t.data(), FP);
    Action a = mesh_rekey::start(d.ctx, d.fp, d.reannounce_fp, surv, n, next_id++ * 7919u, now);
    if (a.type == ActionType::NONE) return;
    d.reannounce = false;
    apply(i, a);
  }
  /* mesh_session::remove_peer: false when refused (a rotation in flight). */
  bool remove(size_t i, const uint8_t fp[FP], uint32_t now) {
    SimDev& d = devs[i];
    if (mesh_rekey::in_progress(d.ctx) || !trusts(d, fp)) return false;
    uint8_t surv[mesh_rekey::MAX_SURVIVORS][FP];
    size_t n = 0;
    for (const auto& t : d.trusted) {
      if (std::memcmp(t.data(), fp, FP) != 0) std::memcpy(surv[n++], t.data(), FP);
    }
    Action a = mesh_rekey::start(d.ctx, d.fp, fp, surv, n, next_id++ * 7919u, now);
    if (a.type == ActionType::NONE) return false;
    mesh_revocation::add(d.deny, fp, now);
    forget(d, fp);
    apply(i, a);
    return true;
  }
  void step(uint32_t now) {
    /* Deliver what was in flight at the start of this step (50 ms hops). */
    const size_t n = q.size();
    for (size_t k = 0; k < n; ++k) {
      SimFrame f = q.front();
      q.pop_front();
      for (size_t to = 0; to < devs.size(); ++to) {
        if (to == f.from) continue;
        if (!f.broadcast && std::memcmp(devs[to].fp, f.to_fp, FP) != 0) continue;
        if (lose(f, to)) continue;
        deliver(f, to, now);
      }
    }
    for (size_t i = 0; i < devs.size(); ++i) {
      for (int k = 0; k < 16; ++k) {
        Action a = mesh_rekey::tick(devs[i].ctx, now);
        if (a.type == ActionType::NONE) break;
        apply(i, a);
      }
      reannounce(i, now);
    }
  }
};

/* Six devices; fp[i][0] orders them (lower = precedes). All share secret S0
 * and trust each other. */
Household make_household(const uint8_t first_bytes[6]) {
  Household h;
  h.devs.resize(6);
  for (size_t i = 0; i < 6; ++i) {
    for (size_t b = 0; b < FP; ++b) h.devs[i].fp[b] = (uint8_t)(first_bytes[i] + b);
    h.devs[i].fp[0] = first_bytes[i];
    mesh_rekey::context_init(h.devs[i].ctx);
    std::memset(h.devs[i].secret, 0x5A, 32);
    mesh_revocation::init(h.devs[i].deny);
  }
  for (size_t i = 0; i < 6; ++i) {
    for (size_t j = 0; j < 6; ++j) {
      if (i == j) continue;
      std::array<uint8_t, FP> t;
      std::memcpy(t.data(), h.devs[j].fp, FP);
      h.devs[i].trusted.push_back(t);
    }
  }
  return h;
}

/* A (0) removes X (4) at tA; B (1) removes Y (5) at tB; a refused removal is
 * retried every second, as a user would after a 409. C (2) and D (3) are
 * bystanders. Returns true when the household converged: every member on
 * one new secret, X and Y trusted by no member, holding neither that secret,
 * on every member's deny-list; every rotation finished. */
bool run_two_removals(Household& h, uint32_t tA, uint32_t tB, const char* label) {
  const size_t A = 0, B = 1, X = 4, Y = 5;
  bool a_done = false, b_done = false;
  for (uint32_t now = 0; now <= 3 * mesh_rekey::REKEY_TIMEOUT_MS; now += 50) {
    if (!a_done && now >= tA && (now - tA) % 1000 == 0) a_done = h.remove(A, h.devs[X].fp, now);
    if (!b_done && now >= tB && (now - tB) % 1000 == 0) b_done = h.remove(B, h.devs[Y].fp, now);
    h.step(now);
  }
  bool ok = a_done && b_done;
  const uint8_t* final_secret = h.devs[A].secret;
  uint8_t s0[32];
  std::memset(s0, 0x5A, 32);
  ok = ok && std::memcmp(final_secret, s0, 32) != 0;
  for (size_t m = 0; m < 4; ++m) {
    const SimDev& d = h.devs[m];
    ok = ok && std::memcmp(d.secret, final_secret, 32) == 0;
    ok = ok && !h.trusts(d, h.devs[X].fp) && !h.trusts(d, h.devs[Y].fp);
    ok = ok && mesh_revocation::contains(d.deny, h.devs[X].fp, 3 * mesh_rekey::REKEY_TIMEOUT_MS);
    ok = ok && mesh_revocation::contains(d.deny, h.devs[Y].fp, 3 * mesh_rekey::REKEY_TIMEOUT_MS);
    ok = ok && !mesh_rekey::in_progress(d.ctx);
    for (size_t o = 0; o < 4; ++o) {
      if (o != m) ok = ok && h.trusts(d, h.devs[o].fp);   /* no member dropped */
    }
  }
  ok = ok && std::memcmp(h.devs[X].secret, final_secret, 32) != 0;
  ok = ok && std::memcmp(h.devs[Y].secret, final_secret, 32) != 0;
  std::printf("  %-44s %s\n", label, ok ? "converged" : "SPLIT");
  return ok;
}

void test_two_removals_converge() {
  /* A precedes B, and B precedes A. */
  const uint8_t a_first[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  const uint8_t b_first[6] = {0x20, 0x10, 0x30, 0x40, 0x50, 0x60};
  for (int order = 0; order < 2; ++order) {
    const uint8_t* fb = order == 0 ? a_first : b_first;
    const char* who = order == 0 ? "A<B" : "B<A";
    char label[64];
    {
      Household h = make_household(fb);
      std::snprintf(label, sizeof(label), "%s simultaneous", who);
      assert(run_two_removals(h, 0, 0, label));
    }
    {
      Household h = make_household(fb);
      std::snprintf(label, sizeof(label), "%s one hop apart", who);
      assert(run_two_removals(h, 0, 50, label));
    }
    {
      /* B hears A's OFFER, so its own removal is refused (it is a survivor)
       * until A's rotation installs — then it runs, under the new secret. */
      Household h = make_household(fb);
      std::snprintf(label, sizeof(label), "%s B 2 s later, heard A", who);
      assert(run_two_removals(h, 0, 2000, label));
    }
    {
      /* B lost A's first OFFER and starts its own 2 s later: two rotations
       * in flight at once, resolved inside the settle windows. */
      Household h = make_household(fb);
      bool first = true;
      h.lose = [&](const SimFrame& f, size_t to) {
        if (to == 1 && f.from == 0 && f.type == MsgType::OFFER && first) { first = false; return true; }
        return false;
      };
      std::snprintf(label, sizeof(label), "%s B lost A's first OFFER, +2 s", who);
      assert(run_two_removals(h, 0, 2000, label));
    }
    {
      /* The same with C and D also missing A's first OFFER: they join B's
       * rotation first, and must follow the precedence. */
      Household h = make_household(fb);
      int lost = 0;
      h.lose = [&](const SimFrame& f, size_t to) {
        if (f.from == 0 && f.type == MsgType::OFFER && to != 4 && to != 5 && lost < 3) { ++lost; return true; }
        return false;
      };
      std::snprintf(label, sizeof(label), "%s B,C,D lost A's first OFFER, +1 s", who);
      assert(run_two_removals(h, 0, 1000, label));
    }
  }
  std::printf("PASS test_two_removals_converge\n");
}

/* The mechanisms one by one, on the pure contexts. */
void test_settle_yield_switch_drop() {
  const uint8_t FP_LOW[FP]  = {0x01, 1, 1, 1, 1, 1, 1, 1};   /* precedes FP_I */
  Context ci, clow, ca;
  mesh_rekey::context_init(ci);
  mesh_rekey::context_init(clow);
  mesh_rekey::context_init(ca);

  /* I starts; A joins; I holds A's ACCEPT through the settle window. */
  Action offer_i = start_ab(ci, 100, 0);
  Action acc_a = rx(ca, FP_A, MsgType::OFFER, FP_I, offer_i, 10);
  assert(rx(ci, FP_I, MsgType::ACCEPT, FP_A, acc_a, 20).type == ActionType::NONE);
  assert(!mesh_rekey::handed_out(ci));

  /* A preceding initiator's OFFER inside I's window: I yields — its own
   * rotation ends uncommitted — and answers LOW as a survivor. */
  const uint8_t surv_low[2][FP] = {
    {FP_I[0], FP_I[1], FP_I[2], FP_I[3], FP_I[4], FP_I[5], FP_I[6], FP_I[7]},
    {FP_A[0], FP_A[1], FP_A[2], FP_A[3], FP_A[4], FP_A[5], FP_A[6], FP_A[7]},
  };
  Action offer_low = mesh_rekey::start(clow, FP_LOW, FP_B, surv_low, 2, 200, 30);
  Action yield = rx(ci, FP_I, MsgType::OFFER, FP_LOW, offer_low, 40);
  assert(yield.type == ActionType::SEND_ACCEPT);
  assert(std::memcmp(yield.dest_fp, FP_LOW, FP) == 0);
  assert(ci.role == mesh_rekey::Role::SURVIVOR && ci.rekey_id == 200);
  assert(all_zero(ci.new_secret, 32));                     /* I's secret is gone */

  /* A, still waiting on I (no SECRET yet), switches to the preceding one. */
  Action sw = rx(ca, FP_A, MsgType::OFFER, FP_LOW, offer_low, 50);
  assert(sw.type == ActionType::SEND_ACCEPT && ca.rekey_id == 200);
  /* A non-preceding OFFER does not move it back. */
  assert(rx(ca, FP_A, MsgType::OFFER, FP_I, offer_i, 60).type == ActionType::NONE);

  /* An initiator that has handed out does not yield. */
  Context ch, cs;
  mesh_rekey::context_init(ch);
  mesh_rekey::context_init(cs);
  Action offer_h = start_ab(ch, 300, 0);
  Action acc = rx(cs, FP_A, MsgType::OFFER, FP_I, offer_h, 10);
  assert(rx(ch, FP_I, MsgType::ACCEPT, FP_A, acc, SET).type == ActionType::SEND_SECRET);
  assert(mesh_rekey::handed_out(ch));
  assert(rx(ch, FP_I, MsgType::OFFER, FP_LOW, offer_low, SET + 1).type == ActionType::NONE);
  assert(ch.role == mesh_rekey::Role::INITIATOR && ch.rekey_id == 300);

  /* An OFFER that names this device as removed ends its own rotation. */
  Context cr;
  mesh_rekey::context_init(cr);
  Action offer_r = start_ab(cr, 400, 0);
  (void)offer_r;
  Context cx;
  mesh_rekey::context_init(cx);
  const uint8_t surv_x[1][FP] = {{FP_A[0], FP_A[1], FP_A[2], FP_A[3], FP_A[4], FP_A[5], FP_A[6], FP_A[7]}};
  Action offer_rm_i = mesh_rekey::start(cx, FP_B, FP_I, surv_x, 1, 500, 5);   /* B removes I */
  assert(rx(cr, FP_I, MsgType::OFFER, FP_B, offer_rm_i, 10).type == ActionType::NONE);
  assert(!mesh_rekey::in_progress(cr) && all_zero(cr.new_secret, 32));
  /* ...and a survivor drops a rotation whose initiator is named removed. */
  Context cw;
  mesh_rekey::context_init(cw);
  Context ci2;
  mesh_rekey::context_init(ci2);
  Action offer_i2 = start_ab(ci2, 600, 0);
  assert(rx(cw, FP_A, MsgType::OFFER, FP_I, offer_i2, 10).type == ActionType::SEND_ACCEPT);
  Action to_b = rx(cw, FP_A, MsgType::OFFER, FP_B, offer_rm_i, 20);   /* B (> I) removes I */
  assert(to_b.type == ActionType::SEND_ACCEPT && cw.rekey_id == 500);

  /* drop_survivor: a removed survivor gets no SECRET and is not waited for. */
  Context cd, cda, cdb;
  mesh_rekey::context_init(cd);
  mesh_rekey::context_init(cda);
  mesh_rekey::context_init(cdb);
  Action od = start_ab(cd, 700, 0);
  Action ada = rx(cda, FP_A, MsgType::OFFER, FP_I, od, 10);
  Action adb = rx(cdb, FP_B, MsgType::OFFER, FP_I, od, 10);
  assert(rx(cd, FP_I, MsgType::ACCEPT, FP_A, ada, 20).type == ActionType::NONE);
  assert(rx(cd, FP_I, MsgType::ACCEPT, FP_B, adb, 20).type == ActionType::NONE);
  assert(mesh_rekey::drop_survivor(cd, FP_B));
  assert(!mesh_rekey::drop_survivor(cd, FP_B));
  Action only = mesh_rekey::tick(cd, SET);
  assert(only.type == ActionType::SEND_SECRET && std::memcmp(only.dest_fp, FP_A, FP) == 0);
  /* Nothing for B: the next tick is only the due OFFER retransmit. */
  assert(mesh_rekey::tick(cd, SET + 1).type == ActionType::BROADCAST_OFFER);
  assert(mesh_rekey::tick(cd, SET + 2).type == ActionType::NONE);
  Action inst = rx(cda, FP_A, MsgType::SECRET, FP_I, only, SET + 2);
  Action done = rx(cd, FP_I, MsgType::ACK, FP_A, inst, SET + 3);
  assert(done.type == ActionType::COMMIT && done.dropped_count == 0);   /* B not "dropped" */
  /* Dropping the last one outstanding lets tick() commit. */
  Context ce, cea;
  mesh_rekey::context_init(ce);
  mesh_rekey::context_init(cea);
  Action oe = start_ab(ce, 800, 0);
  Action aea = rx(cea, FP_A, MsgType::OFFER, FP_I, oe, 10);
  Action se = rx(ce, FP_I, MsgType::ACCEPT, FP_A, aea, SET);
  Action ie = rx(cea, FP_A, MsgType::SECRET, FP_I, se, SET + 1);
  assert(rx(ce, FP_I, MsgType::ACK, FP_A, ie, SET + 2).type == ActionType::NONE);   /* B out */
  assert(mesh_rekey::drop_survivor(ce, FP_B));
  assert(mesh_rekey::tick(ce, SET + 3).type == ActionType::COMMIT);
  mesh_rekey::wipe(done);
  std::printf("PASS test_settle_yield_switch_drop\n");
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
  test_settle_yield_switch_drop();
  test_two_removals_converge();
  std::printf("\nALL MESH_REKEY TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
