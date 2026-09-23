/*
 * SecuraCV Canary — Opera secret rotation (F10-rekey) — implementation
 *
 * CRYPTO: maintainer review required before merge; bench-gated (U1 Track C3).
 *
 * Pure state machine; see mesh_rekey.h for the exchange. Every path that
 * ends a transaction wipes the ephemeral private key and the new secret
 * from the context (secure_zero: volatile stores + a compiler barrier).
 */

#include "mesh_rekey.h"

#include <string.h>

namespace mesh_rekey {

namespace {

inline void secure_zero(void* p, size_t n) {
  volatile uint8_t* b = static_cast<volatile uint8_t*>(p);
  while (n--) *b++ = 0;
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" ::: "memory");
#endif
}

inline void put_u32(uint8_t* out, uint32_t v) {
  out[0] = (uint8_t)v;
  out[1] = (uint8_t)(v >> 8);
  out[2] = (uint8_t)(v >> 16);
  out[3] = (uint8_t)(v >> 24);
}

inline uint32_t get_u32(const uint8_t* in) {
  return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
         ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

inline bool fp_eq(const uint8_t* a, const uint8_t* b) {
  return mesh_crypto::ct_equal(a, b, FP_LEN);
}

Action none() {
  Action a;
  memset(&a, 0, sizeof(a));
  a.type = ActionType::NONE;
  return a;
}

/* AEAD key for one (rotation, survivor) pair. Both sides feed the same
 * ordered inputs: the initiator's ephemeral pub first, the survivor's
 * second. The raw X25519 output never leaves this function. */
bool derive_key(const uint8_t our_eph_priv[mesh_crypto::PRIVKEY_LEN],
                const uint8_t peer_eph_pub[EPH_LEN],
                uint32_t      rekey_id,
                const uint8_t initiator_eph_pub[EPH_LEN],
                const uint8_t survivor_eph_pub[EPH_LEN],
                uint8_t       key_out[mesh_crypto::AEAD_KEY_LEN]) {
  uint8_t shared[mesh_crypto::X25519_SHARED_LEN];
  if (!mesh_crypto::x25519_derive(our_eph_priv, peer_eph_pub, shared)) {
    secure_zero(shared, sizeof(shared));
    return false;
  }
  uint8_t kdf_in[mesh_crypto::X25519_SHARED_LEN + REKEY_ID_LEN + 2 * EPH_LEN];
  size_t off = 0;
  memcpy(kdf_in + off, shared, sizeof(shared));            off += sizeof(shared);
  put_u32(kdf_in + off, rekey_id);                         off += REKEY_ID_LEN;
  memcpy(kdf_in + off, initiator_eph_pub, EPH_LEN);        off += EPH_LEN;
  memcpy(kdf_in + off, survivor_eph_pub, EPH_LEN);         off += EPH_LEN;
  uint8_t digest[mesh_crypto::SHA256_OUT_LEN];
  mesh_crypto::sha256_domain(DOMAIN_REKEY_KEY, kdf_in, off, digest);
  memcpy(key_out, digest, mesh_crypto::AEAD_KEY_LEN);
  secure_zero(shared, sizeof(shared));
  secure_zero(kdf_in, sizeof(kdf_in));
  secure_zero(digest, sizeof(digest));
  return true;
}

/* rekey_id LE || initiator_fp || survivor_fp */
constexpr size_t AAD_LEN = REKEY_ID_LEN + 2 * FP_LEN;
void build_aad(uint32_t rekey_id, const uint8_t initiator_fp[FP_LEN],
               const uint8_t survivor_fp[FP_LEN], uint8_t out[AAD_LEN]) {
  put_u32(out, rekey_id);
  memcpy(out + REKEY_ID_LEN, initiator_fp, FP_LEN);
  memcpy(out + REKEY_ID_LEN + FP_LEN, survivor_fp, FP_LEN);
}

Survivor* find_survivor(Context& ctx, const uint8_t fp[FP_LEN]) {
  for (size_t i = 0; i < ctx.survivor_count; ++i) {
    if (fp_eq(ctx.survivors[i].fp, fp)) return &ctx.survivors[i];
  }
  return nullptr;
}

/* COMMIT action from the initiator's context, then wipe the context. */
Action commit(Context& ctx) {
  Action a = none();
  a.type = ActionType::COMMIT;
  memcpy(a.new_secret, ctx.new_secret, SECRET_LEN);
  memcpy(a.removed_fp, ctx.removed_fp, FP_LEN);
  for (size_t i = 0; i < ctx.survivor_count; ++i) {
    if (!ctx.survivors[i].acked) {
      memcpy(a.dropped[a.dropped_count++], ctx.survivors[i].fp, FP_LEN);
    }
  }
  context_init(ctx);
  return a;
}

/* The initiator's OFFER, rebuilt from the context — the first broadcast
 * and every retransmission carry the same bytes. */
Action offer_action(const Context& ctx) {
  Action a = none();
  a.type = ActionType::BROADCAST_OFFER;
  a.msg_type = MsgType::OFFER;
  put_u32(a.payload, ctx.rekey_id);
  memcpy(a.payload + REKEY_ID_LEN, ctx.eph_pub, EPH_LEN);
  memcpy(a.payload + REKEY_ID_LEN + EPH_LEN, ctx.removed_fp, FP_LEN);
  a.payload_len = OFFER_LEN;
  return a;
}

Action on_offer(Context& ctx, const uint8_t my_fp[FP_LEN],
                const uint8_t sender_fp[FP_LEN],
                const uint8_t* p, size_t len, uint32_t now_ms) {
  if (len != OFFER_LEN) return none();
  const uint32_t rekey_id = get_u32(p);
  const uint8_t* eph_pub_i  = p + REKEY_ID_LEN;
  const uint8_t* removed_fp = p + REKEY_ID_LEN + EPH_LEN;

  /* The device being removed takes no part: it keeps the old secret,
   * which stops working the moment the survivors switch. */
  if (fp_eq(removed_fp, my_fp)) return none();

  if (ctx.role == Role::SURVIVOR) {
    /* A retransmitted OFFER for the transaction we already joined gets
     * the same ACCEPT again (same ephemeral key); anything else waits. */
    if (ctx.rekey_id == rekey_id && fp_eq(ctx.initiator_fp, sender_fp) &&
        memcmp(ctx.initiator_eph_pub, eph_pub_i, EPH_LEN) == 0) {
      Action a = none();
      a.type = ActionType::SEND_ACCEPT;
      a.msg_type = MsgType::ACCEPT;
      memcpy(a.dest_fp, sender_fp, FP_LEN);
      put_u32(a.payload, rekey_id);
      memcpy(a.payload + REKEY_ID_LEN, ctx.eph_pub, EPH_LEN);
      a.payload_len = ACCEPT_LEN;
      return a;
    }
    return none();
  }
  if (ctx.role != Role::IDLE) return none();   /* we are initiating our own */

  context_init(ctx);
  if (!mesh_crypto::x25519_generate_keypair(ctx.eph_pub, ctx.eph_priv)) {
    context_init(ctx);
    return none();
  }
  ctx.role       = Role::SURVIVOR;
  ctx.rekey_id   = rekey_id;
  ctx.started_ms = now_ms;
  memcpy(ctx.initiator_fp, sender_fp, FP_LEN);
  memcpy(ctx.initiator_eph_pub, eph_pub_i, EPH_LEN);
  memcpy(ctx.removed_fp, removed_fp, FP_LEN);

  Action a = none();
  a.type = ActionType::SEND_ACCEPT;
  a.msg_type = MsgType::ACCEPT;
  memcpy(a.dest_fp, sender_fp, FP_LEN);
  put_u32(a.payload, rekey_id);
  memcpy(a.payload + REKEY_ID_LEN, ctx.eph_pub, EPH_LEN);
  a.payload_len = ACCEPT_LEN;
  return a;
}

Action on_accept(Context& ctx, const uint8_t sender_fp[FP_LEN],
                 const uint8_t* p, size_t len) {
  if (ctx.role != Role::INITIATOR || len != ACCEPT_LEN) return none();
  if (get_u32(p) != ctx.rekey_id) return none();   /* stale or foreign rotation */
  Survivor* s = find_survivor(ctx, sender_fp);
  if (s == nullptr || s->acked) return none();      /* not a survivor / already done */
  const uint8_t* eph_pub_s = p + REKEY_ID_LEN;
  if (s->accepted && memcmp(s->eph_pub, eph_pub_s, EPH_LEN) != 0) {
    return none();   /* a different key mid-transaction: ignore, never re-key */
  }
  memcpy(s->eph_pub, eph_pub_s, EPH_LEN);
  s->accepted = true;

  uint8_t key[mesh_crypto::AEAD_KEY_LEN];
  if (!derive_key(ctx.eph_priv, s->eph_pub, ctx.rekey_id,
                  ctx.eph_pub, s->eph_pub, key)) {
    s->accepted = false;
    return none();
  }
  uint8_t aad[AAD_LEN];
  build_aad(ctx.rekey_id, ctx.initiator_fp, s->fp, aad);

  Action a = none();
  a.type = ActionType::SEND_SECRET;
  a.msg_type = MsgType::SECRET;
  memcpy(a.dest_fp, s->fp, FP_LEN);
  uint8_t* nonce = a.payload + REKEY_ID_LEN;
  uint8_t* ct    = nonce + mesh_crypto::AEAD_NONCE_LEN;
  uint8_t* tag   = ct + SECRET_LEN;
  put_u32(a.payload, ctx.rekey_id);
  mesh_crypto::aead_generate_nonce(nonce);
  const bool ok = mesh_crypto::aead_encrypt(key, nonce, aad, sizeof(aad),
                                            ctx.new_secret, SECRET_LEN, ct, tag);
  secure_zero(key, sizeof(key));
  if (!ok) {
    s->accepted = false;
    return none();
  }
  a.payload_len = SECRET_MSG_LEN;
  return a;
}

Action on_secret(Context& ctx, const uint8_t my_fp[FP_LEN],
                 const uint8_t sender_fp[FP_LEN],
                 const uint8_t* p, size_t len) {
  if (ctx.role != Role::SURVIVOR || len != SECRET_MSG_LEN) return none();
  if (!fp_eq(sender_fp, ctx.initiator_fp)) return none();
  if (get_u32(p) != ctx.rekey_id) return none();

  uint8_t key[mesh_crypto::AEAD_KEY_LEN];
  if (!derive_key(ctx.eph_priv, ctx.initiator_eph_pub, ctx.rekey_id,
                  ctx.initiator_eph_pub, ctx.eph_pub, key)) {
    return none();
  }
  uint8_t aad[AAD_LEN];
  build_aad(ctx.rekey_id, ctx.initiator_fp, my_fp, aad);
  const uint8_t* nonce = p + REKEY_ID_LEN;
  const uint8_t* ct    = nonce + mesh_crypto::AEAD_NONCE_LEN;
  const uint8_t* tag   = ct + SECRET_LEN;

  Action a = none();
  const bool ok = mesh_crypto::aead_decrypt(key, nonce, aad, sizeof(aad),
                                            ct, SECRET_LEN, tag, a.new_secret);
  secure_zero(key, sizeof(key));
  if (!ok) {
    wipe(a);
    return none();   /* keep waiting; the timeout aborts cleanly */
  }
  a.type = ActionType::ACK_AND_INSTALL;
  a.msg_type = MsgType::ACK;
  memcpy(a.dest_fp, ctx.initiator_fp, FP_LEN);
  put_u32(a.payload, ctx.rekey_id);
  a.payload_len = ACK_LEN;
  memcpy(a.removed_fp, ctx.removed_fp, FP_LEN);
  context_init(ctx);
  return a;
}

Action on_ack(Context& ctx, const uint8_t sender_fp[FP_LEN],
              const uint8_t* p, size_t len) {
  if (ctx.role != Role::INITIATOR || len != ACK_LEN) return none();
  if (get_u32(p) != ctx.rekey_id) return none();
  Survivor* s = find_survivor(ctx, sender_fp);
  /* An ACK only counts from a survivor we actually sent a secret to. */
  if (s == nullptr || !s->accepted) return none();
  s->acked = true;
  for (size_t i = 0; i < ctx.survivor_count; ++i) {
    if (!ctx.survivors[i].acked) return none();
  }
  return commit(ctx);   /* everyone has it */
}

}  // namespace

void context_init(Context& ctx) {
  secure_zero(&ctx, sizeof(ctx));
  ctx.role = Role::IDLE;
}

bool in_progress(const Context& ctx) { return ctx.role != Role::IDLE; }

void wipe(Action& a) { secure_zero(&a, sizeof(a)); }

Action start(Context&      ctx,
             const uint8_t my_fp[FP_LEN],
             const uint8_t removed_fp[FP_LEN],
             const uint8_t (*survivor_fps)[FP_LEN],
             size_t        n_survivors,
             uint32_t      rekey_id,
             uint32_t      now_ms) {
  if (in_progress(ctx)) return none();
  if (my_fp == nullptr || removed_fp == nullptr) return none();
  if (n_survivors > MAX_SURVIVORS) return none();
  if (n_survivors > 0 && survivor_fps == nullptr) return none();

  context_init(ctx);
  ctx.role       = Role::INITIATOR;
  ctx.rekey_id   = rekey_id;
  ctx.started_ms = now_ms;
  memcpy(ctx.initiator_fp, my_fp, FP_LEN);
  memcpy(ctx.removed_fp, removed_fp, FP_LEN);
  mesh_crypto::fill_random(ctx.new_secret, SECRET_LEN);

  if (n_survivors == 0) return commit(ctx);   /* nobody to tell */

  if (!mesh_crypto::x25519_generate_keypair(ctx.eph_pub, ctx.eph_priv)) {
    context_init(ctx);
    return none();
  }
  for (size_t i = 0; i < n_survivors; ++i) {
    memcpy(ctx.survivors[i].fp, survivor_fps[i], FP_LEN);
  }
  ctx.survivor_count = n_survivors;
  ctx.last_offer_ms  = now_ms;
  return offer_action(ctx);
}

Action receive(Context&      ctx,
               const uint8_t my_fp[FP_LEN],
               MsgType       type,
               const uint8_t sender_fp[FP_LEN],
               const uint8_t* payload,
               size_t        payload_len,
               uint32_t      now_ms) {
  if (my_fp == nullptr || sender_fp == nullptr || payload == nullptr) return none();
  switch (type) {
    case MsgType::OFFER:  return on_offer(ctx, my_fp, sender_fp, payload, payload_len, now_ms);
    case MsgType::ACCEPT: return on_accept(ctx, sender_fp, payload, payload_len);
    case MsgType::SECRET: return on_secret(ctx, my_fp, sender_fp, payload, payload_len);
    case MsgType::ACK:    return on_ack(ctx, sender_fp, payload, payload_len);
    default:              return none();
  }
}

Action tick(Context& ctx, uint32_t now_ms) {
  if (!in_progress(ctx)) return none();
  if ((uint32_t)(now_ms - ctx.started_ms) >= REKEY_TIMEOUT_MS) {
    if (ctx.role == Role::INITIATOR) return commit(ctx);
    Action a = none();
    a.type = ActionType::ABORT;
    context_init(ctx);
    return a;
  }
  /* Initiator inside the window: re-broadcast the same OFFER every
   * REKEY_RETRY_MS. While the role is INITIATOR some survivor has not
   * ACKed yet (the last ACK commits at once), and the same OFFER heals
   * each loss that leaves the survivor on the old opera_id: a lost OFFER
   * (it answers now), a lost ACCEPT (it re-sends the same one) and a lost
   * SECRET (its repeated ACCEPT draws a fresh SECRET). A lost ACK does not
   * heal — that survivor already switched and drops old-id frames. */
  if (ctx.role == Role::INITIATOR &&
      (uint32_t)(now_ms - ctx.last_offer_ms) >= REKEY_RETRY_MS) {
    ctx.last_offer_ms = now_ms;
    return offer_action(ctx);
  }
  return none();
}

}  /* namespace mesh_rekey */
