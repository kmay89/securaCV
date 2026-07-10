/*
 * SecuraCV Canary — Mesh pairing — Implementation
 *
 * PR 2d: confirmation-code derivation, wire-format payloads,
 *        confirmation hash helper.
 * PR 2e: full state-machine driver (5-message handshake, timeouts,
 *        AEAD-wrapped opera_secret transfer).
 *
 * The state machine is pure — it holds NO globals. The integration
 * layer (PR 2f) keeps one PairingContext per device, feeds it bytes
 * from mesh_transport, and acts on the Actions returned.
 */

#include "mesh_pairing.h"

#include <string.h>

namespace mesh_pairing {

uint32_t compute_confirmation_code(const uint8_t session_key[SESSION_KEY_LEN]) {
  if (session_key == nullptr) return 0;

  uint8_t code_hash[mesh_crypto::SHA256_OUT_LEN];
  mesh_crypto::sha256_domain(DOMAIN_PAIR_CONFIRM,
                             session_key, SESSION_KEY_LEN,
                             code_hash);

  /* canary-wap mesh_network.cpp:800:
   *   code = ((h[0] << 16) | (h[1] << 8) | h[2]) % 1000000
   * Exact same byte order and modulus so both lanes display the same
   * 6 digits for the same session_key. */
  const uint32_t top24 = ((uint32_t)code_hash[0] << 16) |
                         ((uint32_t)code_hash[1] << 8)  |
                         ((uint32_t)code_hash[2]);
  return top24 % CONFIRMATION_CODE_MODULUS;
}

void compute_confirmation_hash(const uint8_t session_key[SESSION_KEY_LEN],
                               uint32_t      code,
                               uint8_t       out[mesh_crypto::SHA256_OUT_LEN]) {
  if (session_key == nullptr || out == nullptr) return;

  /* Wire-compat with canary-wap mesh_network.cpp:856-860 / 1448-1450:
   * the hash input is session_key (32 bytes) followed by the
   * confirmation_code stored as a 4-byte little-endian uint32. We
   * construct the LE bytes explicitly so this is correct regardless
   * of host endianness (the test build is x86 LE; the device is
   * Xtensa LE; the wire is fixed LE). */
  uint8_t buf[SESSION_KEY_LEN + 4];
  for (size_t i = 0; i < SESSION_KEY_LEN; ++i) buf[i] = session_key[i];
  buf[SESSION_KEY_LEN + 0] = (uint8_t)(code         & 0xFF);
  buf[SESSION_KEY_LEN + 1] = (uint8_t)((code >> 8)  & 0xFF);
  buf[SESSION_KEY_LEN + 2] = (uint8_t)((code >> 16) & 0xFF);
  buf[SESSION_KEY_LEN + 3] = (uint8_t)((code >> 24) & 0xFF);

  mesh_crypto::sha256_domain(DOMAIN_PAIR_CONFIRM, buf, sizeof(buf), out);
}

const char* mesh_state_name(bool   enabled,
                            bool   has_opera,
                            State  pairing_state,
                            size_t peers_online) {
  /* Pairing precedence: an in-progress flow always overrides the
   * steady-state classification so the UI shows the pairing screen.
   * PAIRED / FAILED are terminal — by the time the integration layer
   * has reacted to them the context is back to IDLE for the steady
   * states, so they do not map to a PAIRING_* string here. */
  switch (pairing_state) {
    case State::DISCOVERING_INITIATOR:
    case State::AWAITING_ACCEPT:
      return "PAIRING_INIT";
    case State::DISCOVERING_JOINER:
    case State::AWAITING_COMPLETE:
      return "PAIRING_JOIN";
    case State::AWAITING_CONFIRM:
    case State::AWAITING_CONFIRM_PEER:
      return "PAIRING_CONFIRM";
    default:
      break;   /* IDLE / PAIRED / FAILED → fall through to steady state */
  }

  if (!enabled)   return "DISABLED";
  if (!has_opera) return "NO_OPERA";
  return peers_online > 0 ? "ACTIVE" : "CONNECTING";
}

/* ──────────────────────────────────────────────────────────────────────────
 * STATE-MACHINE INTERNALS
 * ────────────────────────────────────────────────────────────────────────── */

namespace {

/* Bytewise memcpy alias — string.h is included via mesh_pairing.h's
 * transitive includes. Use memcpy for clarity. */

/* secure_zero: volatile-loop + asm barrier so the compiler can't
 * dead-store-eliminate the wipe. Same pattern as
 * firmware/canary/lib/securacv_mesh/src/mesh_crypto.cpp:secure_zero
 * (file-local to keep this module standalone). */
inline void secure_zero(void* p, size_t n) {
  volatile uint8_t* b = static_cast<volatile uint8_t*>(p);
  while (n--) *b++ = 0;
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" ::: "memory");
#endif
}

inline Action make_action(ActionType t) {
  Action a{};
  a.type = t;
  return a;
}

inline Action make_send_action(ActionType t,
                                const uint8_t peer_mac[6],
                                const void*   payload,
                                size_t        payload_len) {
  Action a{};
  a.type = t;
  if (peer_mac) memcpy(a.peer_mac, peer_mac, 6);
  if (payload != nullptr && payload_len > 0 && payload_len <= MAX_ACTION_PAYLOAD) {
    memcpy(a.payload, payload, payload_len);
    a.payload_len = payload_len;
  }
  return a;
}

/* Generate the ephemeral X25519 keypair into ctx.ephem_{pub,priv}key.
 * Uses mesh_crypto::ed25519_generate_keypair to source a random 32-byte
 * private scalar — Curve25519::eval clamps internally, so the same
 * 32-byte privkey works for X25519 as for Ed25519 in the host shim;
 * on device the rweather Curve25519 takes the priv directly. */
inline bool generate_ephemeral(PairingContext& ctx) {
  return mesh_crypto::ed25519_generate_keypair(ctx.ephem_pubkey, ctx.ephem_privkey);
}

/* Derive session_key + confirmation_code once we know both ephemeral
 * pubs. Both sides do this with their own (priv, peer_pub) and arrive
 * at the same session_key. */
inline bool derive_session_state(PairingContext& ctx) {
  if (!mesh_crypto::x25519_derive(ctx.ephem_privkey, ctx.peer_ephem_pubkey, ctx.session_key)) {
    return false;
  }
  ctx.confirmation_code = compute_confirmation_code(ctx.session_key);
  return true;
}

inline void copy_name(char* dst, size_t dst_cap, const char* src) {
  if (dst == nullptr || dst_cap == 0) return;
  size_t i = 0;
  if (src != nullptr) {
    for (; i + 1 < dst_cap && src[i] != '\0'; ++i) dst[i] = src[i];
  }
  dst[i] = '\0';
}

inline void fail(PairingContext& ctx) {
  ctx.state = State::FAILED;
  secure_zero(ctx.ephem_privkey, sizeof(ctx.ephem_privkey));
  secure_zero(ctx.session_key, sizeof(ctx.session_key));
  secure_zero(ctx.opera_secret, sizeof(ctx.opera_secret));
  ctx.opera_secret_present = false;
}

}  /* namespace */

/* ──────────────────────────────────────────────────────────────────────────
 * STATE-MACHINE API
 * ────────────────────────────────────────────────────────────────────────── */

void context_init(PairingContext& ctx) {
  secure_zero(&ctx, sizeof(ctx));
  ctx.state = State::IDLE;
  ctx.role = ROLE_NONE;
}

Action start_initiator(PairingContext& ctx,
                       const uint8_t  device_pub[mesh_crypto::PUBKEY_LEN],
                       const uint8_t  device_priv[mesh_crypto::PRIVKEY_LEN],
                       const uint8_t  opera_secret[mesh_crypto::OPERA_SECRET_LEN],
                       const char*    opera_name,
                       uint32_t       now_ms) {
  if (ctx.state != State::IDLE) return make_action(ActionType::NONE);
  if (device_pub == nullptr || device_priv == nullptr || opera_secret == nullptr) {
    return make_action(ActionType::NONE);
  }

  context_init(ctx);
  ctx.role = ROLE_INITIATOR;
  memcpy(ctx.device_pubkey,  device_pub,  mesh_crypto::PUBKEY_LEN);
  memcpy(ctx.device_privkey, device_priv, mesh_crypto::PRIVKEY_LEN);
  memcpy(ctx.opera_secret,   opera_secret, mesh_crypto::OPERA_SECRET_LEN);
  ctx.opera_secret_present = true;
  copy_name(ctx.opera_name, sizeof(ctx.opera_name), opera_name);
  if (!generate_ephemeral(ctx)) { fail(ctx); return make_action(ActionType::NOTIFY_FAILED); }
  ctx.started_ms = now_ms;
  ctx.state = State::DISCOVERING_INITIATOR;

  /* Broadcast DISCOVER(role=INITIATOR). canary-wap broadcasts every
   * 2 s for the duration; this initial broadcast is sufficient for a
   * one-shot pairing flow because the joiner's DISCOVER(role=JOINER)
   * is what actually triggers the OFFER on the initiator side. */
  PairDiscoverPayload disc{};
  memcpy(disc.pubkey, ctx.device_pubkey, mesh_crypto::PUBKEY_LEN);
  copy_name(disc.device_name, sizeof(disc.device_name), ctx.opera_name);
  disc.role = ROLE_INITIATOR;

  static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  return make_send_action(ActionType::BROADCAST_DISCOVER, BCAST, &disc, sizeof(disc));
}

Action start_joiner(PairingContext& ctx,
                    const uint8_t  device_pub[mesh_crypto::PUBKEY_LEN],
                    const uint8_t  device_priv[mesh_crypto::PRIVKEY_LEN],
                    uint32_t       now_ms) {
  if (ctx.state != State::IDLE) return make_action(ActionType::NONE);
  if (device_pub == nullptr || device_priv == nullptr) {
    return make_action(ActionType::NONE);
  }
  context_init(ctx);
  ctx.role = ROLE_JOINER;
  memcpy(ctx.device_pubkey,  device_pub,  mesh_crypto::PUBKEY_LEN);
  memcpy(ctx.device_privkey, device_priv, mesh_crypto::PRIVKEY_LEN);
  if (!generate_ephemeral(ctx)) { fail(ctx); return make_action(ActionType::NOTIFY_FAILED); }
  ctx.started_ms = now_ms;
  ctx.state = State::DISCOVERING_JOINER;

  /* Broadcast DISCOVER(role=JOINER). The initiator's handle_pair_discover
   * only acts when it sees a JOINER-role discover (canary-wap
   * mesh_network.cpp:756); without this broadcast the initiator stays
   * silent and pairing never starts. */
  PairDiscoverPayload disc{};
  memcpy(disc.pubkey, ctx.device_pubkey, mesh_crypto::PUBKEY_LEN);
  copy_name(disc.device_name, sizeof(disc.device_name), "");
  disc.role = ROLE_JOINER;

  static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  return make_send_action(ActionType::BROADCAST_DISCOVER, BCAST, &disc, sizeof(disc));
}

/* Internal helpers for each phase of receive(). Role-semantics match
 * canary-wap mesh_network.cpp:748-866:
 *   • Initiator handles DISCOVER (role=JOINER)  → sends OFFER
 *   • Joiner    handles OFFER                   → sends ACCEPT
 *   • Initiator handles ACCEPT                  → derives session, NOTIFY_CODE_READY
 *   • Both      handle CONFIRM                   → SEND_COMPLETE (initiator) or move to AWAITING_COMPLETE (joiner)
 *   • Joiner    handles COMPLETE                → NOTIFY_PAIRED + opera_secret available
 */
namespace {

Action initiator_handle_discover(PairingContext& ctx,
                                 const uint8_t from_mac[6],
                                 const uint8_t* payload, size_t payload_len) {
  if (ctx.state != State::DISCOVERING_INITIATOR) return make_action(ActionType::NONE);
  if (ctx.role  != ROLE_INITIATOR) return make_action(ActionType::NONE);
  if (payload_len != sizeof(PairDiscoverPayload)) return make_action(ActionType::NONE);
  const PairDiscoverPayload* disc = (const PairDiscoverPayload*)payload;
  /* canary-wap only acts on a JOINER discover; an initiator-role
   * discover is another initiator and is silently ignored. */
  if (disc->role != ROLE_JOINER) return make_action(ActionType::NONE);

  memcpy(ctx.peer_mac, from_mac, 6);
  memcpy(ctx.peer_pubkey, disc->pubkey, mesh_crypto::PUBKEY_LEN);

  /* Send OFFER carrying OUR (initiator's) ephemeral pub + device pub.
   * Matches canary-wap mesh_network.cpp:765-769. */
  PairOfferPayload offer{};
  memcpy(offer.ephemeral_pubkey, ctx.ephem_pubkey,  mesh_crypto::PUBKEY_LEN);
  memcpy(offer.device_pubkey,    ctx.device_pubkey, mesh_crypto::PUBKEY_LEN);
  copy_name(offer.opera_name, sizeof(offer.opera_name), ctx.opera_name);
  offer.opera_member_count = 0;

  ctx.state = State::AWAITING_ACCEPT;
  return make_send_action(ActionType::SEND_OFFER, ctx.peer_mac, &offer, sizeof(offer));
}

Action joiner_handle_offer(PairingContext& ctx,
                           const uint8_t from_mac[6],
                           const uint8_t* payload, size_t payload_len) {
  if (ctx.state != State::DISCOVERING_JOINER) return make_action(ActionType::NONE);
  if (ctx.role  != ROLE_JOINER) return make_action(ActionType::NONE);
  if (payload_len != sizeof(PairOfferPayload)) return make_action(ActionType::NONE);
  const PairOfferPayload* offer = (const PairOfferPayload*)payload;

  memcpy(ctx.peer_mac, from_mac, 6);
  memcpy(ctx.peer_pubkey,       offer->device_pubkey,    mesh_crypto::PUBKEY_LEN);
  memcpy(ctx.peer_ephem_pubkey, offer->ephemeral_pubkey, mesh_crypto::PUBKEY_LEN);
  /* Remember the opera_name the initiator advertised so the UI can show it. */
  copy_name(ctx.opera_name, sizeof(ctx.opera_name), offer->opera_name);

  if (!derive_session_state(ctx)) { fail(ctx); return make_action(ActionType::NOTIFY_FAILED); }

  /* Send ACCEPT carrying OUR (joiner's) ephemeral pub + device pub.
   * Matches canary-wap mesh_network.cpp:803-815 (reuses PairOfferPayload). */
  PairAcceptPayload accept{};
  memcpy(accept.ephemeral_pubkey, ctx.ephem_pubkey,  mesh_crypto::PUBKEY_LEN);
  memcpy(accept.device_pubkey,    ctx.device_pubkey, mesh_crypto::PUBKEY_LEN);
  copy_name(accept.opera_name, sizeof(accept.opera_name), "");
  accept.opera_member_count = 0;

  /* Joiner has derived the session and code — surface to UI. */
  ctx.state = State::AWAITING_CONFIRM;
  Action a = make_send_action(ActionType::SEND_ACCEPT, ctx.peer_mac, &accept, sizeof(accept));
  a.confirmation_code = ctx.confirmation_code;
  return a;
}

Action initiator_handle_accept(PairingContext& ctx,
                               const uint8_t from_mac[6],
                               const uint8_t* payload, size_t payload_len) {
  if (ctx.state != State::AWAITING_ACCEPT) return make_action(ActionType::NONE);
  if (ctx.role  != ROLE_INITIATOR) return make_action(ActionType::NONE);
  if (payload_len != sizeof(PairAcceptPayload)) return make_action(ActionType::NONE);
  /* The MAC in the ACCEPT must match the peer we sent the OFFER to. */
  if (memcmp(from_mac, ctx.peer_mac, 6) != 0) return make_action(ActionType::NONE);
  const PairAcceptPayload* accept = (const PairAcceptPayload*)payload;
  memcpy(ctx.peer_ephem_pubkey, accept->ephemeral_pubkey, mesh_crypto::PUBKEY_LEN);

  if (!derive_session_state(ctx)) { fail(ctx); return make_action(ActionType::NOTIFY_FAILED); }

  /* Initiator now has the session_key + code — surface to UI. */
  ctx.state = State::AWAITING_CONFIRM;
  Action a = make_action(ActionType::NOTIFY_CODE_READY);
  a.confirmation_code = ctx.confirmation_code;
  return a;
}

Action either_handle_confirm(PairingContext& ctx,
                             const uint8_t from_mac[6],
                             const uint8_t* payload, size_t payload_len,
                             uint32_t now_ms) {
  if (ctx.state != State::AWAITING_CONFIRM_PEER) return make_action(ActionType::NONE);
  if (payload_len != sizeof(PairConfirmPayload)) return make_action(ActionType::NONE);
  if (memcmp(from_mac, ctx.peer_mac, 6) != 0) return make_action(ActionType::NONE);
  const PairConfirmPayload* cf = (const PairConfirmPayload*)payload;

  uint8_t expected[mesh_crypto::SHA256_OUT_LEN];
  compute_confirmation_hash(ctx.session_key, ctx.confirmation_code, expected);
  if (!mesh_crypto::ct_equal(cf->confirmation_hash, expected, mesh_crypto::SHA256_OUT_LEN)) {
    fail(ctx);
    return make_action(ActionType::NOTIFY_FAILED);
  }

  if (ctx.role == ROLE_INITIATOR) {
    /* Send COMPLETE: AEAD-encrypt the opera_secret under the session
     * key with a fresh random nonce. Layout: ct || tag || nonce in the
     * payload buffer. */
    PairCompletePayload complete{};
    uint8_t nonce[mesh_crypto::AEAD_NONCE_LEN];
    mesh_crypto::aead_generate_nonce(nonce);
    uint8_t ct[mesh_crypto::OPERA_SECRET_LEN];
    uint8_t tag[mesh_crypto::AEAD_TAG_LEN];
    if (!mesh_crypto::aead_encrypt(ctx.session_key, nonce, nullptr, 0,
                                   ctx.opera_secret, mesh_crypto::OPERA_SECRET_LEN,
                                   ct, tag)) {
      fail(ctx);
      return make_action(ActionType::NOTIFY_FAILED);
    }
    memcpy(complete.encrypted_secret, ct, mesh_crypto::OPERA_SECRET_LEN);
    memcpy(complete.encrypted_secret + mesh_crypto::OPERA_SECRET_LEN, tag,
           mesh_crypto::AEAD_TAG_LEN);
    memcpy(complete.nonce, nonce, mesh_crypto::AEAD_NONCE_LEN);
    /* Initiator is done — wipe sensitive state and arm the success
     * notification. The integration layer's caller sequence:
     *   1. send the SEND_COMPLETE payload over mesh_transport
     *   2. next tick() → NOTIFY_PAIRED (gated on pending_notify_paired) */
    ctx.state = State::PAIRED;
    secure_zero(ctx.ephem_privkey, sizeof(ctx.ephem_privkey));
    secure_zero(ctx.session_key,   sizeof(ctx.session_key));
    secure_zero(ctx.opera_secret,  sizeof(ctx.opera_secret));
    ctx.opera_secret_present = false;
    ctx.pending_notify_paired = true;
    Action a = make_send_action(ActionType::SEND_COMPLETE, ctx.peer_mac,
                                &complete, sizeof(complete));
    a.confirmation_code = ctx.confirmation_code;
    (void)now_ms;
    return a;
  } else {
    /* Joiner: hash matched, now just wait for COMPLETE. */
    ctx.state = State::AWAITING_COMPLETE;
    (void)now_ms;
    return make_action(ActionType::NONE);
  }
}

Action joiner_handle_complete(PairingContext& ctx,
                              const uint8_t from_mac[6],
                              const uint8_t* payload, size_t payload_len) {
  if (ctx.state != State::AWAITING_COMPLETE) return make_action(ActionType::NONE);
  if (payload_len != sizeof(PairCompletePayload)) return make_action(ActionType::NONE);
  if (memcmp(from_mac, ctx.peer_mac, 6) != 0) return make_action(ActionType::NONE);
  const PairCompletePayload* complete = (const PairCompletePayload*)payload;

  const uint8_t* ct  = complete->encrypted_secret;
  const uint8_t* tag = complete->encrypted_secret + mesh_crypto::OPERA_SECRET_LEN;
  if (!mesh_crypto::aead_decrypt(ctx.session_key, complete->nonce, nullptr, 0,
                                 ct, mesh_crypto::OPERA_SECRET_LEN, tag,
                                 ctx.opera_secret)) {
    fail(ctx);
    return make_action(ActionType::NOTIFY_FAILED);
  }
  ctx.opera_secret_present = true;
  ctx.state = State::PAIRED;
  /* Wipe the ephemeral private and the session_key — the AEAD has
   * already decrypted the opera_secret into ctx.opera_secret, which
   * the integration layer reads via consume_opera_secret() then we
   * wipe THAT too. session_key has no further use after this point. */
  secure_zero(ctx.ephem_privkey, sizeof(ctx.ephem_privkey));
  secure_zero(ctx.session_key,   sizeof(ctx.session_key));
  Action a = make_action(ActionType::NOTIFY_PAIRED);
  a.confirmation_code = ctx.confirmation_code;
  return a;
}

}  /* namespace */

Action receive(PairingContext& ctx,
               const uint8_t  from_mac[6],
               MsgType        msg_type,
               const uint8_t* payload, size_t payload_len,
               uint32_t       now_ms) {
  if (from_mac == nullptr || payload == nullptr) return make_action(ActionType::NONE);
  if (ctx.state == State::IDLE || ctx.state == State::FAILED || ctx.state == State::PAIRED) {
    return make_action(ActionType::NONE);
  }
  switch (msg_type) {
    case MsgType::DISCOVER: return initiator_handle_discover(ctx, from_mac, payload, payload_len);
    case MsgType::OFFER:    return joiner_handle_offer   (ctx, from_mac, payload, payload_len);
    case MsgType::ACCEPT:   return initiator_handle_accept(ctx, from_mac, payload, payload_len);
    case MsgType::CONFIRM:  return either_handle_confirm  (ctx, from_mac, payload, payload_len, now_ms);
    case MsgType::COMPLETE: return joiner_handle_complete (ctx, from_mac, payload, payload_len);
  }
  return make_action(ActionType::NONE);
}

Action tick(PairingContext& ctx, uint32_t now_ms) {
  if (ctx.state == State::IDLE || ctx.state == State::FAILED) {
    return make_action(ActionType::NONE);
  }
  if (ctx.state == State::PAIRED) {
    /* Initiator's deferred success signal — fires exactly once after
     * SEND_COMPLETE was returned. Cleared so subsequent ticks return
     * NONE. */
    if (ctx.pending_notify_paired) {
      ctx.pending_notify_paired = false;
      Action a = make_action(ActionType::NOTIFY_PAIRED);
      a.confirmation_code = ctx.confirmation_code;
      return a;
    }
    return make_action(ActionType::NONE);
  }
  if ((now_ms - ctx.started_ms) >= PAIRING_TIMEOUT_MS) {
    fail(ctx);
    return make_action(ActionType::NOTIFY_FAILED);
  }
  return make_action(ActionType::NONE);
}

Action confirm_code(PairingContext& ctx, uint32_t now_ms) {
  if (ctx.state != State::AWAITING_CONFIRM) return make_action(ActionType::NONE);
  ctx.user_confirmed = true;
  (void)now_ms;

  PairConfirmPayload confirm{};
  compute_confirmation_hash(ctx.session_key, ctx.confirmation_code, confirm.confirmation_hash);

  ctx.state = State::AWAITING_CONFIRM_PEER;
  return make_send_action(ActionType::SEND_CONFIRM, ctx.peer_mac, &confirm, sizeof(confirm));
}

Action cancel(PairingContext& ctx) {
  if (ctx.state == State::IDLE) return make_action(ActionType::NONE);
  fail(ctx);
  return make_action(ActionType::NOTIFY_FAILED);
}

bool consume_opera_secret(PairingContext& ctx,
                          uint8_t out[mesh_crypto::OPERA_SECRET_LEN]) {
  if (!ctx.opera_secret_present || out == nullptr) return false;
  if (ctx.role != ROLE_JOINER) return false;
  memcpy(out, ctx.opera_secret, mesh_crypto::OPERA_SECRET_LEN);
  secure_zero(ctx.opera_secret, sizeof(ctx.opera_secret));
  ctx.opera_secret_present = false;
  return true;
}

}  /* namespace mesh_pairing */
