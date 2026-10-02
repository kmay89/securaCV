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

const char* fail_reason_name(FailReason r) {
  switch (r) {
    case FailReason::NONE:            return "none";
    case FailReason::CANCELED:        return "canceled";
    case FailReason::TIMEOUT:         return "timeout";
    case FailReason::BAD_CONFIRM:     return "bad_confirm";
    case FailReason::BAD_COMPLETE:    return "bad_complete";
    case FailReason::CRYPTO:          return "crypto";
    case FailReason::PARTNER_REFUSED: return "partner_refused";
  }
  return "unknown";
}

Outcome outcome_of(const PairingContext& ctx) {
  switch (ctx.state) {
    case State::IDLE:   return Outcome::NONE;
    case State::FAILED: return Outcome::FAILED;
    case State::PAIRED: return ctx.pending_notify_paired ? Outcome::RUNNING : Outcome::PAIRED;
    default:            return Outcome::RUNNING;
  }
}

const char* outcome_name(Outcome o) {
  switch (o) {
    case Outcome::NONE:    return "none";
    case Outcome::RUNNING: return "running";
    case Outcome::PAIRED:  return "paired";
    case Outcome::FAILED:  return "failed";
  }
  return "none";
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
 *
 * CRYPTO (F33 part 2 — crypto review, maintainer to confirm): this used to
 * call ed25519_generate_keypair(), whose public key is an Edwards point
 * derived from SHA-512(seed) — not the X25519 public key of that seed —
 * so on a device the two sides' x25519_derive() gave different session
 * keys and the 6-digit codes could never match (the host shim hid it by
 * agreeing whatever the keys were). x25519_generate_keypair() clamps a
 * random scalar per RFC 7748 §5 and derives pub = scalar * basepoint. */
inline bool generate_ephemeral(PairingContext& ctx) {
  return mesh_crypto::x25519_generate_keypair(ctx.ephem_pubkey, ctx.ephem_privkey);
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

/* The initiator's kept COMPLETE and its leading CONFIRM (F134), wiped. */
inline void wipe_kept_complete(PairingContext& ctx) {
  ctx.complete_resend_armed = false;
  secure_zero(&ctx.kept_complete, sizeof(ctx.kept_complete));
  secure_zero(&ctx.kept_confirm,  sizeof(ctx.kept_confirm));
}

/* End the pairing: FAILED, every secret wiped, and the NOTIFY_FAILED that
 * tells the integration layer why. */
inline Action fail(PairingContext& ctx, FailReason why) {
  ctx.state = State::FAILED;
  ctx.fail_reason = why;
  ctx.peer_confirmed = false;
  wipe_kept_complete(ctx);
  secure_zero(ctx.ephem_privkey, sizeof(ctx.ephem_privkey));
  secure_zero(ctx.session_key, sizeof(ctx.session_key));
  secure_zero(ctx.opera_secret, sizeof(ctx.opera_secret));
  ctx.opera_secret_present = false;
  Action a = make_action(ActionType::NOTIFY_FAILED);
  a.fail_reason = why;
  return a;
}

/* F118: may this device hold the partner the exchange named? Asked only
 * once the partner's key and address are both known (AWAITING_CONFIRM or
 * later). */
inline bool partner_admitted(const PairingContext& ctx) {
  return ctx.partner_gate == nullptr || ctx.partner_gate(ctx.peer_pubkey, ctx.peer_mac);
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
                       uint32_t       now_ms,
                       PartnerGate    gate) {
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
  ctx.partner_gate = gate;
  if (!generate_ephemeral(ctx)) return fail(ctx, FailReason::CRYPTO);
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
                    uint32_t       now_ms,
                    PartnerGate    gate) {
  if (ctx.state != State::IDLE) return make_action(ActionType::NONE);
  if (device_pub == nullptr || device_priv == nullptr) {
    return make_action(ActionType::NONE);
  }
  context_init(ctx);
  ctx.role = ROLE_JOINER;
  memcpy(ctx.device_pubkey,  device_pub,  mesh_crypto::PUBKEY_LEN);
  memcpy(ctx.device_privkey, device_priv, mesh_crypto::PRIVKEY_LEN);
  ctx.partner_gate = gate;
  if (!generate_ephemeral(ctx)) return fail(ctx, FailReason::CRYPTO);
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
 *   • Both      handle CONFIRM                   → SEND_COMPLETE, or kept until the owner's
 *                                                 confirm (initiator); checked (joiner)
 *   • Joiner    handles COMPLETE (after its own owner confirmed) → NOTIFY_PAIRED + opera_secret
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

  if (!derive_session_state(ctx)) return fail(ctx, FailReason::CRYPTO);

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

  if (!derive_session_state(ctx)) return fail(ctx, FailReason::CRYPTO);

  /* Initiator now has the session_key + code — surface to UI. */
  ctx.state = State::AWAITING_CONFIRM;
  Action a = make_action(ActionType::NOTIFY_CODE_READY);
  a.confirmation_code = ctx.confirmation_code;
  return a;
}

/* The initiator's last step, whichever owner confirmed first: AEAD-seal
 * the opera_secret to the joiner under the session key, go PAIRED and arm
 * the NOTIFY_PAIRED the next tick() returns. Layout: ct || tag, then the
 * nonce. Its callers: either_handle_confirm (the joiner's CONFIRM arrives
 * after this side's owner confirmed) and confirm_code (it arrived before).
 *
 * The COMPLETE carries this side's own CONFIRM to go in front of it
 * (Action::leading_confirm), in both orders. An updated joiner does not
 * need it. A joiner on firmware before F97 does: it reads a CONFIRM only
 * after its own owner confirmed, and takes a COMPLETE only after that, so
 * it never read the one confirm_code sent before its owner confirmed, and
 * in the joiner-first order none was sent. Without this CONFIRM such a
 * joiner dropped the COMPLETE while this side reported PAIRED (host-probed
 * against the pre-F97 code, both orders). Built here, before the session
 * key is wiped.
 *
 * Both frames are kept, as sent, for the copies tick() sends (F134):
 * nothing acknowledges a COMPLETE, so one lost on the air would otherwise
 * leave this side PAIRED with a joiner that never got the secret. `now_ms`
 * is the first send. */
Action initiator_complete(PairingContext& ctx, uint32_t now_ms) {
  /* F118: nothing is sealed to a partner this device cannot hold. Asked
   * here as well as at the owner's confirm (confirm_code), because in the
   * initiator-first order this runs when the joiner's CONFIRM arrives, a
   * while after that confirm. */
  if (!partner_admitted(ctx)) return fail(ctx, FailReason::PARTNER_REFUSED);
  PairConfirmPayload lead{};
  compute_confirmation_hash(ctx.session_key, ctx.confirmation_code, lead.confirmation_hash);
  PairCompletePayload complete{};
  uint8_t nonce[mesh_crypto::AEAD_NONCE_LEN];
  mesh_crypto::aead_generate_nonce(nonce);
  uint8_t ct[mesh_crypto::OPERA_SECRET_LEN];
  uint8_t tag[mesh_crypto::AEAD_TAG_LEN];
  if (!mesh_crypto::aead_encrypt(ctx.session_key, nonce, nullptr, 0,
                                 ctx.opera_secret, mesh_crypto::OPERA_SECRET_LEN,
                                 ct, tag)) {
    return fail(ctx, FailReason::CRYPTO);
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
  ctx.peer_confirmed = false;
  ctx.pending_notify_paired = true;
  ctx.kept_complete         = complete;
  ctx.kept_confirm          = lead;
  ctx.complete_first_ms     = now_ms;
  ctx.complete_last_ms      = now_ms;
  ctx.complete_copies       = 0;
  ctx.complete_resend_armed = true;
  Action a = make_send_action(ActionType::SEND_COMPLETE, ctx.peer_mac,
                              &complete, sizeof(complete));
  a.confirmation_code = ctx.confirmation_code;
  a.leading_confirm = lead;
  a.leading_confirm_present = true;
  secure_zero(&complete, sizeof(complete));
  return a;
}

/* F134: the next copy of the kept COMPLETE, if one is due; the copies end
 * at the window. Signed elapsed time, as for the joiner's re-send: a
 * receive() stamped a little after this tick's clock reads as not yet
 * due. */
Action complete_copy_if_due(PairingContext& ctx, uint32_t now_ms) {
  if (!ctx.complete_resend_armed) return make_action(ActionType::NONE);
  if ((int32_t)(now_ms - ctx.complete_first_ms) >= (int32_t)COMPLETE_RESEND_WINDOW_MS) {
    wipe_kept_complete(ctx);
    return make_action(ActionType::NONE);
  }
  if ((int32_t)(now_ms - ctx.complete_last_ms) < (int32_t)COMPLETE_RESEND_INTERVAL_MS) {
    return make_action(ActionType::NONE);
  }
  ctx.complete_last_ms = now_ms;
  ++ctx.complete_copies;
  Action a = make_send_action(ActionType::SEND_COMPLETE, ctx.peer_mac,
                              &ctx.kept_complete, sizeof(ctx.kept_complete));
  a.confirmation_code = ctx.confirmation_code;
  a.leading_confirm = ctx.kept_confirm;
  a.leading_confirm_present = true;
  return a;
}

/* A partner's CONFIRM, in either order (spec §5.2; sweep F97, canary-wap's
 * F75 carried over). Until F97 this acted only in AWAITING_CONFIRM_PEER,
 * i.e. after this device's own owner had confirmed, and dropped a CONFIRM
 * that arrived earlier, while neither side ever sends its CONFIRM twice and
 * tick() re-sends nothing. So:
 *   • the joiner's owner confirming first left the initiator without the
 *     joiner's CONFIRM, and both sides failed at the 5-minute timeout;
 *   • the initiator's owner confirming first got the initiator's CONFIRM
 *     dropped by the joiner, which then waited in AWAITING_CONFIRM_PEER for
 *     it and dropped the COMPLETE: the initiator reported PAIRED and held a
 *     member that never joined, and the joiner failed at the timeout.
 * A pairing completed only when both owners confirmed before either CONFIRM
 * crossed the air (host-probed, both orders).
 *
 * Now a CONFIRM counts only from the partner's address, and only once the
 * code is shown (AWAITING_CONFIRM or later): before the ACCEPT the
 * initiator's session key is all zero, so anyone can compute that hash.
 *   • The initiator keeps a verified early CONFIRM (peer_confirmed) and
 *     completes at its own owner's confirm (confirm_code). Every COMPLETE
 *     goes out with this side's CONFIRM in front of it (initiator_complete),
 *     for a joiner on firmware before F97; an updated joiner does not need
 *     it.
 *   • The joiner checks the initiator's CONFIRM in either order and needs
 *     nothing else from it: it takes the COMPLETE once its own owner has
 *     confirmed (joiner_handle_complete), and the COMPLETE decrypting under
 *     the session key is the proof the CONFIRM was. The CONFIRM in front of
 *     the COMPLETE is checked like any other; one arriving after the
 *     joiner already moved to AWAITING_COMPLETE is dropped.
 * A wrong hash from the partner's address ends the pairing in either order,
 * as it did after the owner's confirm. Not closed (F94): the hash is the
 * same in both directions and the address is not authenticated, so the
 * initiator's own CONFIRM reflected to it from the joiner's address counts
 * as the joiner's; the same before F97. */
Action either_handle_confirm(PairingContext& ctx,
                             const uint8_t from_mac[6],
                             const uint8_t* payload, size_t payload_len,
                             uint32_t now_ms) {
  if (ctx.state != State::AWAITING_CONFIRM &&
      ctx.state != State::AWAITING_CONFIRM_PEER) {
    return make_action(ActionType::NONE);
  }
  if (payload_len != sizeof(PairConfirmPayload)) return make_action(ActionType::NONE);
  if (memcmp(from_mac, ctx.peer_mac, 6) != 0) return make_action(ActionType::NONE);
  const PairConfirmPayload* cf = (const PairConfirmPayload*)payload;

  uint8_t expected[mesh_crypto::SHA256_OUT_LEN];
  compute_confirmation_hash(ctx.session_key, ctx.confirmation_code, expected);
  if (!mesh_crypto::ct_equal(cf->confirmation_hash, expected, mesh_crypto::SHA256_OUT_LEN)) {
    return fail(ctx, FailReason::BAD_CONFIRM);
  }

  if (ctx.role == ROLE_INITIATOR) {
    if (ctx.state == State::AWAITING_CONFIRM) {
      /* The joiner's owner confirmed first: keep it for confirm_code. */
      ctx.peer_confirmed = true;
      return make_action(ActionType::NONE);
    }
    return initiator_complete(ctx, now_ms);
  }
  /* Joiner: the hash matched. After its own owner's confirm it now waits
   * for the COMPLETE only; before it, nothing changes (the COMPLETE is
   * still taken only after this owner confirms). Reading the initiator's
   * CONFIRM here also says the initiator's owner has confirmed, so a
   * pre-F97 initiator now waits for a CONFIRM it reads after that, and
   * dropped this side's earlier one if it came first: arm the re-send
   * (F117; tick() sends it, unless the COMPLETE comes first). */
  if (ctx.state == State::AWAITING_CONFIRM_PEER) {
    ctx.state = State::AWAITING_COMPLETE;
    ctx.confirm_resend_armed = true;
    ctx.confirm_resends      = 0;
    ctx.confirm_resend_ms    = now_ms;
  }
  return make_action(ActionType::NONE);
}

/* The initiator's COMPLETE, taken once this device's own owner has
 * confirmed (AWAITING_CONFIRM_PEER or AWAITING_COMPLETE), and only by a
 * joiner. Until F97 only AWAITING_COMPLETE, which the joiner reached only by
 * receiving the initiator's CONFIRM after its own owner confirmed; an
 * initiator whose owner confirmed first had that CONFIRM dropped, and its
 * COMPLETE then was too (either_handle_confirm above). Never before the
 * owner's confirm: a COMPLETE in AWAITING_CONFIRM would finish the pairing
 * with no owner on this side. */
Action joiner_handle_complete(PairingContext& ctx,
                              const uint8_t from_mac[6],
                              const uint8_t* payload, size_t payload_len) {
  if (ctx.role != ROLE_JOINER) return make_action(ActionType::NONE);
  if (ctx.state != State::AWAITING_CONFIRM_PEER &&
      ctx.state != State::AWAITING_COMPLETE) {
    return make_action(ActionType::NONE);
  }
  if (payload_len != sizeof(PairCompletePayload)) return make_action(ActionType::NONE);
  if (memcmp(from_mac, ctx.peer_mac, 6) != 0) return make_action(ActionType::NONE);
  /* F118: a joiner that cannot hold its initiator does not open the
   * secret, let alone install it. Asked here as well as at the owner's
   * confirm, in case the tables changed since. */
  if (!partner_admitted(ctx)) return fail(ctx, FailReason::PARTNER_REFUSED);
  const PairCompletePayload* complete = (const PairCompletePayload*)payload;

  const uint8_t* ct  = complete->encrypted_secret;
  const uint8_t* tag = complete->encrypted_secret + mesh_crypto::OPERA_SECRET_LEN;
  if (!mesh_crypto::aead_decrypt(ctx.session_key, complete->nonce, nullptr, 0,
                                 ct, mesh_crypto::OPERA_SECRET_LEN, tag,
                                 ctx.opera_secret)) {
    return fail(ctx, FailReason::BAD_COMPLETE);
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
     * NONE, or a due copy of the COMPLETE (F134). */
    if (ctx.pending_notify_paired) {
      ctx.pending_notify_paired = false;
      Action a = make_action(ActionType::NOTIFY_PAIRED);
      a.confirmation_code = ctx.confirmation_code;
      return a;
    }
    return complete_copy_if_due(ctx, now_ms);
  }
  if ((now_ms - ctx.started_ms) >= PAIRING_TIMEOUT_MS) {
    return fail(ctx, FailReason::TIMEOUT);
  }
  /* F117: the joiner's CONFIRM again, for an initiator on firmware before
   * F97 (mesh_pairing.h, STATE MACHINE). Only in AWAITING_COMPLETE — its
   * owner confirmed and it read the initiator's CONFIRM since — only to the
   * partner, and bounded. Signed elapsed time: a receive() stamped a little
   * after this tick's clock reads as not yet due. */
  if (ctx.role == ROLE_JOINER && ctx.state == State::AWAITING_COMPLETE &&
      ctx.confirm_resend_armed && ctx.confirm_resends < CONFIRM_RESEND_MAX) {
    const uint32_t gap = ctx.confirm_resends == 0 ? CONFIRM_RESEND_FIRST_MS
                                                  : CONFIRM_RESEND_INTERVAL_MS;
    if ((int32_t)(now_ms - ctx.confirm_resend_ms) >= (int32_t)gap) {
      ++ctx.confirm_resends;
      ctx.confirm_resend_ms = now_ms;
      PairConfirmPayload confirm{};
      compute_confirmation_hash(ctx.session_key, ctx.confirmation_code,
                                confirm.confirmation_hash);
      return make_send_action(ActionType::SEND_CONFIRM, ctx.peer_mac,
                              &confirm, sizeof(confirm));
    }
  }
  return make_action(ActionType::NONE);
}

Action confirm_code(PairingContext& ctx, uint32_t now_ms) {
  if (ctx.state != State::AWAITING_CONFIRM) return make_action(ActionType::NONE);
  ctx.user_confirmed = true;

  /* F118 (spec §5.2): a device that cannot hold its partner fails the
   * pairing, and says so at its owner's confirm, before its CONFIRM goes
   * out. On a joiner that matters most: with no CONFIRM from it, its
   * initiator never seals the opera_secret to it and never takes it as a
   * member (canary-wap's joiner refuses only at the COMPLETE, F73, after
   * its initiator has already added it). */
  if (!partner_admitted(ctx)) return fail(ctx, FailReason::PARTNER_REFUSED);

  /* F97: the joiner's owner confirmed first and its CONFIRM is kept. The
   * joiner is owed the COMPLETE now. It goes with this side's CONFIRM in
   * front of it (initiator_complete): an updated joiner takes the COMPLETE
   * without it, but a joiner on firmware before F97 waits in
   * AWAITING_CONFIRM_PEER for exactly that CONFIRM and drops a COMPLETE
   * that comes alone. Two frames back to back are safe here: the
   * transport's receive ring holds eight. canary-wap sends the COMPLETE
   * alone (F75), because its receive buffer holds one frame and the second
   * frame would be the one dropped. */
  if (ctx.role == ROLE_INITIATOR && ctx.peer_confirmed) return initiator_complete(ctx, now_ms);

  PairConfirmPayload confirm{};
  compute_confirmation_hash(ctx.session_key, ctx.confirmation_code, confirm.confirmation_hash);

  ctx.state = State::AWAITING_CONFIRM_PEER;
  return make_send_action(ActionType::SEND_CONFIRM, ctx.peer_mac, &confirm, sizeof(confirm));
}

/* F135: only a running pairing is canceled. One that has ended is left as
 * it ended: PAIRED stays PAIRED (an initiator's NOTIFY_PAIRED still fires
 * at the next tick(), and the joiner keeps the secret it opened), FAILED
 * keeps its reason and reports nothing again, and IDLE has nothing to end.
 * Until F135 cancel() failed every state but IDLE. The session runs a REST
 * pair/cancel at the start of process(), before the tick, so one that
 * landed after the initiator's COMPLETE went out (the joiner's CONFIRM
 * arrived in the transport pass just before) turned the PAIRED context
 * FAILED: the joiner held the secret, and the initiator never registered,
 * bound or stored it. On FAILED it fired NOTIFY_FAILED, and the session's
 * FailedCallback, a second time. */
Action cancel(PairingContext& ctx) {
  switch (ctx.state) {
    case State::IDLE:
    case State::PAIRED:
    case State::FAILED:
      return make_action(ActionType::NONE);
    default:
      return fail(ctx, FailReason::CANCELED);
  }
}

bool stop_complete_resend(PairingContext& ctx) {
  const bool was = ctx.complete_resend_armed;
  wipe_kept_complete(ctx);
  return was;
}

bool complete_resend_running(const PairingContext& ctx) {
  return ctx.complete_resend_armed;
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
