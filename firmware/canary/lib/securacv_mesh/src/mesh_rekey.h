/*
 * SecuraCV Canary — Opera secret rotation on peer removal (F10-rekey)
 * Version 0.1.0
 *
 * CRYPTO: maintainer review required before merge; bench-gated (U1 Track C3).
 *
 * Spec §5.6 requires that removing a peer rotate the household
 * opera_secret and hand the new one to every remaining member.
 *
 * What the rotation does and does NOT buy on this tree (review fix — so
 * the crypto review weighs the right property): besides being handed on
 * at pairing, opera_secret has exactly one use here, deriving opera_id, and
 * opera_id rides in CLEARTEXT in every frame header; frames are
 * authenticated only by the sender's Ed25519 key.
 * So what excludes the removed device is each survivor UNREGISTERING its
 * pubkey (ACK_AND_INSTALL / COMMIT forget it), not the new secret — the
 * removed device can copy the new opera_id off the air, and it is still
 * accepted by any survivor that did not unregister it: one that missed the
 * whole window, or one that answered the OFFER but aborted at 60 s without
 * its SECRET. Such a survivor trusts the removed device indefinitely, and
 * nothing tells it (the initiator just drops it at commit). What the
 * rotation does buy: every frame signed before the removal carries the old
 * opera_id, so it is dead to every survivor that switched — also across a
 * later re-pair of the removed device — and a survivor that missed the
 * rotation is split onto the old id rather than silently still in step.
 *
 * canary-wap does this with
 * per-peer SESSION keys it keeps for every member. The PlatformIO mesh
 * keeps none — its frames are only Ed25519-signed, and the pairing X25519
 * session key is wiped at PAIRED — so this is option B of the F10-rekey
 * plan: an EPHEMERAL X25519 exchange per rotation, every message inside a
 * signed opera envelope, so each ephemeral public key is authenticated by
 * the sender's long-term Ed25519 key. No new long-lived key material.
 *
 * The exchange (all four are opera-authenticated frames under the CURRENT
 * opera_id; msg_type values 26..29 in mesh_envelope::MsgType):
 *
 *   Initiator (the device a user removed a peer on)     Each survivor
 *   ───────────────────────────────────────────────     ─────────────
 *   new_secret = random(32); eph_i = X25519 keypair
 *   REKEY_OFFER {rekey_id, eph_pub_i, removed_fp} ─────► (broadcast)
 *                                                       eph_s = X25519 keypair
 *                            ◄──────────────────────── REKEY_ACCEPT {rekey_id, eph_pub_s}
 *   k = KDF(X25519(eph_i, eph_pub_s), ...)
 *   REKEY_SECRET {rekey_id, nonce, AEAD_k(new_secret)} ► (to that survivor)
 *                                                       k = KDF(X25519(eph_s, eph_pub_i), ...)
 *                                                       decrypt new_secret
 *                            ◄──────────────────────── REKEY_ACK {rekey_id}
 *                                                         — sent UNDER THE OLD opera_id,
 *                                                         BEFORE the survivor switches
 *                                                         (canary-wap learned this order:
 *                                                         an ACK under the new id would
 *                                                         fail the initiator's opera_id
 *                                                         check), THEN install + drop
 *                                                         removed_fp
 *   all survivors ACKed → COMMIT
 *   every 5 s until then → the same OFFER again (review fix): a lost
 *                         OFFER, ACCEPT or SECRET heals — the survivor
 *                         answers, or re-sends its ACCEPT and draws a fresh
 *                         SECRET. A lost ACK does not (that survivor has
 *                         switched and drops old-id frames).
 *   60 s timeout        → COMMIT, dropping every survivor that did not ACK
 *                         (it keeps the old secret and must re-pair —
 *                         canary-wap's accepted trade-off)
 *
 *   KDF: SHA-256(DOMAIN_REKEY_KEY || shared || rekey_id LE ||
 *                eph_pub_i || eph_pub_s)                 → the AEAD key
 *   AEAD: ChaCha20-Poly1305, random 96-bit nonce, AAD =
 *         rekey_id LE || initiator_fp || survivor_fp     (binds the
 *         ciphertext to this rotation and this recipient)
 *
 * The ephemeral keys come from mesh_crypto::x25519_generate_keypair(),
 * not the Ed25519 generator pairing uses (see that function's comment).
 *
 * This module is the pure state machine (shape = mesh_pairing): it holds
 * no globals, does no I/O and does no signing. mesh_session wraps each
 * payload it returns in a signed envelope, routes inbound REKEY_* frames
 * here only after signature + opera_id + replay checks, and applies
 * INSTALL/COMMIT. Two contexts in one process run the whole exchange host
 * side (test_mesh_rekey.cpp).
 *
 * Deliberate limits (spec §5.6 PIO note): one rotation at a time per
 * device (a second remove is refused while one is in flight, and a
 * survivor ignores a second OFFER); two users removing peers from two
 * devices inside the same 60 s window can leave the household split
 * between two new secrets (the losing side re-pairs). The spec's
 * REVOCATION_GRACE_MS deny-list is not implemented in either tree.
 */

#ifndef SECURACV_MESH_REKEY_H
#define SECURACV_MESH_REKEY_H

#include "mesh_crypto.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace mesh_rekey {

constexpr uint32_t REKEY_TIMEOUT_MS = 60u * 1000u;   /* canary-wap REKEY_TIMEOUT_MS */
constexpr uint32_t REKEY_RETRY_MS   = 5u * 1000u;    /* initiator OFFER re-broadcast */
constexpr size_t   MAX_SURVIVORS    = 8;             /* == mesh_session::MAX_TRUSTED_PEERS */
constexpr size_t   FP_LEN           = mesh_crypto::FINGERPRINT_LEN;
constexpr size_t   EPH_LEN          = mesh_crypto::PUBKEY_LEN;
constexpr size_t   SECRET_LEN       = mesh_crypto::OPERA_SECRET_LEN;
constexpr size_t   REKEY_ID_LEN     = 4;

constexpr size_t OFFER_LEN  = REKEY_ID_LEN + EPH_LEN + FP_LEN;                /* 44 */
constexpr size_t ACCEPT_LEN = REKEY_ID_LEN + EPH_LEN;                         /* 36 */
constexpr size_t SECRET_MSG_LEN = REKEY_ID_LEN + mesh_crypto::AEAD_NONCE_LEN
                                + SECRET_LEN + mesh_crypto::AEAD_TAG_LEN;     /* 64 */
constexpr size_t ACK_LEN    = REKEY_ID_LEN;                                   /*  4 */
constexpr size_t MAX_PAYLOAD_LEN = SECRET_MSG_LEN;

constexpr const char* DOMAIN_REKEY_KEY = "securacv:opera:rekey:key:v0";

/* Wire msg_type of each message — equal to mesh_envelope::MsgType
 * REKEY_OFFER..REKEY_ACK (mesh_session static_asserts it). */
enum class MsgType : uint8_t {
  OFFER  = 26,
  ACCEPT = 27,
  SECRET = 28,
  ACK    = 29,
};

enum class Role : uint8_t {
  IDLE = 0,
  INITIATOR,   /* removed a peer; distributing a new secret */
  SURVIVOR,    /* answered an OFFER; waiting for our SECRET */
};

enum class ActionType : uint8_t {
  NONE = 0,
  BROADCAST_OFFER,   /* sign + broadcast `payload` (msg_type OFFER) */
  SEND_ACCEPT,       /* sign + send `payload` to dest_fp (the initiator) */
  SEND_SECRET,       /* sign + send `payload` to dest_fp (one survivor) */
  ACK_AND_INSTALL,   /* survivor: sign + send the ACK `payload` to dest_fp
                      * under the CURRENT opera_id FIRST, then install
                      * new_secret and drop removed_fp */
  COMMIT,            /* initiator: install new_secret; drop the `dropped`
                      * fingerprints (survivors that never ACKed) */
  ABORT,             /* survivor timed out: nothing sent, old secret kept */
};

/* One step of output. Carries secret material on ACK_AND_INSTALL and
 * COMMIT — the caller MUST wipe() it once applied. */
struct Action {
  ActionType type;
  MsgType    msg_type;
  uint8_t    dest_fp[FP_LEN];
  uint8_t    payload[MAX_PAYLOAD_LEN];
  size_t     payload_len;
  uint8_t    new_secret[SECRET_LEN];
  uint8_t    removed_fp[FP_LEN];
  uint8_t    dropped[MAX_SURVIVORS][FP_LEN];
  size_t     dropped_count;
};

struct Survivor {
  uint8_t fp[FP_LEN];
  uint8_t eph_pub[EPH_LEN];
  bool    accepted;
  bool    acked;
};

/* Per-device context. Treat as opaque; zeroed (secret material wiped) by
 * context_init() and after every terminal transition. */
struct Context {
  Role     role;
  uint32_t rekey_id;
  uint32_t started_ms;
  uint32_t last_offer_ms;            /* initiator: last OFFER (re)broadcast */
  uint8_t  eph_pub [EPH_LEN];
  uint8_t  eph_priv[mesh_crypto::PRIVKEY_LEN];
  uint8_t  removed_fp[FP_LEN];
  /* initiator */
  uint8_t  initiator_fp[FP_LEN];     /* ours, as initiator; the peer's, as survivor */
  uint8_t  new_secret[SECRET_LEN];
  Survivor survivors[MAX_SURVIVORS];
  size_t   survivor_count;
  /* survivor */
  uint8_t  initiator_eph_pub[EPH_LEN];
};

void context_init(Context& ctx);
bool in_progress(const Context& ctx);
void wipe(Action& a);

/* Initiator: begin a rotation after `removed_fp` was dropped locally.
 * `survivor_fps` are the remaining trusted peers. Returns
 *   BROADCAST_OFFER — n > 0: the transaction is open;
 *   COMMIT          — n == 0: nobody to tell, rotate locally right now;
 *   NONE            — refused: a transaction is already in flight, n >
 *                     MAX_SURVIVORS, a null pointer, or key generation
 *                     failed. */
Action start(Context&      ctx,
             const uint8_t my_fp[FP_LEN],
             const uint8_t removed_fp[FP_LEN],
             const uint8_t (*survivor_fps)[FP_LEN],
             size_t        n_survivors,
             uint32_t      rekey_id,
             uint32_t      now_ms);

/* Feed one VERIFIED inbound REKEY_* payload (the caller has already
 * checked the envelope signature against sender_fp's pinned key, the
 * opera_id and the replay counter). Anything unexpected for the current
 * role/rekey_id/sender, or of the wrong length, returns NONE. */
Action receive(Context&      ctx,
               const uint8_t my_fp[FP_LEN],
               MsgType       type,
               const uint8_t sender_fp[FP_LEN],
               const uint8_t* payload,
               size_t        payload_len,
               uint32_t      now_ms);

/* Timeout + retransmit driver. Once REKEY_TIMEOUT_MS has passed:
 * initiator → COMMIT (dropping the non-ACKed), survivor → ABORT. Before
 * that, an initiator gets BROADCAST_OFFER — the same OFFER again — every
 * REKEY_RETRY_MS. NONE otherwise. */
Action tick(Context& ctx, uint32_t now_ms);

}  /* namespace mesh_rekey */

#endif  /* SECURACV_MESH_REKEY_H */
