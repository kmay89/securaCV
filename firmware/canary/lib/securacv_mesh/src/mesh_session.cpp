/*
 * SecuraCV Canary — Mesh session bridge — Implementation
 *
 * Singleton bridge wiring mesh_transport ↔ mesh_pairing.
 *
 * Recv path:
 *   mesh_transport::process() invokes our recv_cb on the main loop →
 *   decode the 1-byte MsgType envelope → mesh_pairing::receive(),
 *   which returns an Action → dispatch_action() forwards it back
 *   through mesh_transport, firing the integration-layer's callbacks
 *   on terminal states.
 *
 * Send path:
 *   start_pairing_*, confirm_pairing_code, cancel_pairing all return
 *   an Action from mesh_pairing, which dispatch_action() forwards.
 *
 * Tick path:
 *   process() drives mesh_pairing::tick() once per call. The initiator
 *   NOTIFY_PAIRED arrives via this path; joiner NOTIFY_PAIRED arrives
 *   inline from the COMPLETE recv handler.
 */

#include "mesh_session.h"
#include "mesh_envelope.h"

#include <string.h>

#ifndef CSI_TEST_HOST_BUILD
  #include <Arduino.h>
#endif

namespace mesh_session {

/* ──────────────────────────────────────────────────────────────────────────
 * STATE
 * ────────────────────────────────────────────────────────────────────────── */

static bool                       s_initialized = false;
static bool                       s_running     = false;
/* User on/off switch (F10). "Disabled" implies "not running": start()
 * refuses while this is false. Reset to true by deinit(). */
static bool                       s_enabled     = true;
/* now_ms of the latest process() call — the receive path's clock (the
 * transport recv callback carries no timestamp). Used to stamp received
 * alerts; off by at most one main-loop pass. */
static uint32_t                   s_last_process_ms = 0;
static mesh_pairing::PairingContext s_ctx;
static uint8_t                    s_device_pub [mesh_crypto::PUBKEY_LEN];
static uint8_t                    s_device_priv[mesh_crypto::PRIVKEY_LEN];

static PairedCallback     s_paired_cb     = nullptr;
static FailedCallback     s_failed_cb     = nullptr;
static CodeReadyCallback  s_code_ready_cb = nullptr;

/* Opera-authenticated broadcast state (PR 5c-3). Declared here at file
 * scope alongside the other lifecycle-managed state so deinit() can
 * wipe it in one place. Without that wipe a deinit()/init() cycle
 * leaves has_opera_secret() returning true from the prior run and the
 * next send_beacon_event() would sign frames with a stale opera_id /
 * sender_fp / continuing counter (codex P1 catch on #472 — missed at
 * merge time, addressed in this PR). */
static bool     s_opera_id_set       = false;
static uint8_t  s_opera_id [mesh_crypto::OPERA_ID_LEN];
static uint8_t  s_sender_fp[mesh_crypto::FINGERPRINT_LEN];
static uint64_t s_outbound_counter   = 0;

/* Opera display name (PR-8; persisted since F10). Surfaced by GET
 * /api/mesh so the UI can label the opera. This module keeps only the
 * RAM copy; the integration layer persists it (mesh_state
 * save_/load_opera_name, NVS key "opera_name", FE-gated) and seeds it
 * at boot. Wiped on deinit() and leave_opera() alongside the other
 * opera-auth state so neither path leaves a stale name. */
static char     s_opera_name[mesh_pairing::MAX_OPERA_NAME_LEN + 1] = {0};

/* Receive-side state (PR 5c-4). Trusted-peer table — small fixed
 * array indexed by sender_fp at recv time, with a per-peer monotonic
 * last_counter for replay defense. Entries are populated by the
 * integration layer via register_trusted_peer() after pairing
 * succeeds. Wiped on deinit() and on clear_trusted_peers(). */
struct TrustedPeer {
  uint8_t  sender_fp [mesh_crypto::FINGERPRINT_LEN];
  uint8_t  pubkey    [mesh_crypto::PUBKEY_LEN];
  uint64_t last_counter;
  bool     in_use;
  /* MAC↔fingerprint join (PR-8 follow-up): the MAC this peer last spoke
   * from, recorded only after a frame fully verifies (signature +
   * opera_id + replay), so the binding is as trustworthy as the frame.
   * Lets /api/mesh/peers join the durable membership set against the
   * live transport table's liveness/RSSI. */
  uint8_t  mac[mesh_transport::MESH_TRANSPORT_MAC_LEN];
  bool     mac_known;
  /* Verified TAMPER_ALERT frames from this peer (F11 residual). Counted
   * in dispatch_verified, i.e. only after signature + opera_id + replay
   * checks — the same trust argument as the MAC binding above. */
  uint32_t alerts_received;
};
static TrustedPeer s_trusted_peers[MAX_TRUSTED_PEERS];

/* Replay tombstones (review fix). A peer that leaves or is removed used to
 * take its last_counter with it; re-registering the same device into the
 * same, un-rotated opera restarted the counter at 0, so every frame it had
 * signed before it left — its LEAVE, alerts, beacon events, REKEY_OFFER —
 * verified once more as fresh. Dropping a trusted peer now parks
 * (fingerprint, last_counter) here and register_trusted_peer() re-applies
 * it. Bounded: when full, the oldest tombstone is evicted. Reported by
 * get_replay_counters() and rebuilt by restore_replay_counter(), so they
 * persist in NVS replay_ctrs beside the live counters. Wiped by deinit()
 * only — leave_opera() keeps them, for the leaver's own re-pair. */
struct CounterTombstone {
  uint8_t  sender_fp[mesh_crypto::FINGERPRINT_LEN];
  uint64_t last_counter;
  uint32_t stamp;    /* insertion order, for oldest-first eviction */
  bool     in_use;
};
static CounterTombstone s_tombstones[MAX_COUNTER_TOMBSTONES];
static uint32_t         s_tombstone_stamp = 0;

static beacon_event_received_fn s_beacon_event_cb = nullptr;
static channel_lock_received_fn s_channel_lock_cb = nullptr;
static hub_election_received_fn s_hub_election_cb = nullptr;
static peer_left_fn             s_peer_left_cb    = nullptr;
static tamper_alert_received_fn s_tamper_alert_cb = nullptr;
static rekey_commit_fn          s_rekey_commit_cb = nullptr;

/* opera_secret rotation (F10-rekey). One transaction at a time, as
 * initiator or survivor. Wiped by deinit(), leave_opera() and disable. */
static mesh_rekey::Context      s_rekey;
/* Initiator only: the removed peer's pubkey. It leaves the trusted table at
 * remove_peer(), but its NVS entry must still be dropped BEFORE the new
 * secret is persisted (mesh_state::persist_rotation fails closed), so the
 * commit hands it to the rekey-commit handler along with the dropped
 * survivors. Wiped with s_rekey (reset_rekey). */
static uint8_t                  s_rekey_removed_pub[mesh_crypto::PUBKEY_LEN];
static bool                     s_rekey_removed_pub_set = false;

static_assert(static_cast<uint8_t>(mesh_rekey::MsgType::OFFER)  ==
              static_cast<uint8_t>(mesh_envelope::MsgType::REKEY_OFFER),  "rekey msg_type drift");
static_assert(static_cast<uint8_t>(mesh_rekey::MsgType::ACCEPT) ==
              static_cast<uint8_t>(mesh_envelope::MsgType::REKEY_ACCEPT), "rekey msg_type drift");
static_assert(static_cast<uint8_t>(mesh_rekey::MsgType::SECRET) ==
              static_cast<uint8_t>(mesh_envelope::MsgType::REKEY_SECRET), "rekey msg_type drift");
static_assert(static_cast<uint8_t>(mesh_rekey::MsgType::ACK)    ==
              static_cast<uint8_t>(mesh_envelope::MsgType::REKEY_ACK),    "rekey msg_type drift");
static_assert(mesh_rekey::MAX_SURVIVORS >= MAX_TRUSTED_PEERS,
              "every other trusted peer must fit in a rotation");

/* Alert channel state (F10). Opera-wide lifetime counter for the boot,
 * plus a ring of the most recent MAX_ALERT_HISTORY records (s_alert_head
 * is the next write slot). clear_alerts() empties the ring and keeps the
 * counters; deinit() and leave_opera() wipe both. */
static uint32_t           s_alerts_received = 0;
static mesh_alert::Record s_alert_ring[MAX_ALERT_HISTORY];
static size_t             s_alert_head  = 0;
static size_t             s_alert_count = 0;

/* REST request slot (review fix; see mesh_session.h). s_slot_state is the
 * only field both tasks race on, and it moves by __atomic builtins; the
 * request and result bodies belong to whoever owns the current state:
 *   IDLE      nobody          → CLAIMED by submit_request (httpd)
 *   CLAIMED   one writer      (submit filling the request, or a withdraw /
 *                             take / abandon wiping a body) → PENDING / IDLE
 *   PENDING   nobody writes   → RUNNING by process() (main loop), or back
 *                             through CLAIMED to IDLE by a withdraw
 *   RUNNING   main loop       → DONE when it publishes the result, or
 *                             ABANDONED by the httpd side giving up
 *   DONE      httpd reader    → through CLAIMED to IDLE (take / abandon)
 *   ABANDONED main loop       → IDLE once it has wiped the result. */
enum SlotState : uint8_t {
  SLOT_IDLE = 0,
  SLOT_CLAIMED,
  SLOT_PENDING,
  SLOT_RUNNING,
  SLOT_DONE,
  SLOT_ABANDONED,
};
static uint8_t       s_slot_state = SLOT_IDLE;
static Request       s_slot_req;
static RequestResult s_slot_result;

/* ──────────────────────────────────────────────────────────────────────────
 * INTERNAL HELPERS
 * ────────────────────────────────────────────────────────────────────────── */

/* secure_zero: volatile-loop + asm memory barrier so the compiler can't
 * dead-store-eliminate the wipe. Same pattern as mesh_crypto.cpp and
 * mesh_pairing.cpp; file-local to keep this module standalone. */
static inline void secure_zero(void* p, size_t n) {
  volatile uint8_t* b = static_cast<volatile uint8_t*>(p);
  while (n--) *b++ = 0;
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" ::: "memory");
#endif
}

/* Convert mesh_pairing::MsgType (used internally by the state machine)
 * to mesh_session::MsgType (used on the wire). Today these are 1:1 by
 * design; the helper exists so a future divergence is a single edit. */
static inline MsgType pairing_msg_to_session(mesh_pairing::MsgType m) {
  return static_cast<MsgType>(static_cast<uint8_t>(m));
}

/* Map mesh_pairing::Action to a session-frame outgoing MsgType. Returns
 * (out_msg_type, dest_mac, payload, len) by reference; caller decides
 * unicast vs broadcast based on the Action::type. */
static bool action_to_wire(const mesh_pairing::Action& a,
                           MsgType*  out_msg_type) {
  switch (a.type) {
    case mesh_pairing::ActionType::BROADCAST_DISCOVER:
      *out_msg_type = MsgType::PAIR_DISCOVER; return true;
    case mesh_pairing::ActionType::SEND_OFFER:
      *out_msg_type = MsgType::PAIR_OFFER;    return true;
    case mesh_pairing::ActionType::SEND_ACCEPT:
      *out_msg_type = MsgType::PAIR_ACCEPT;   return true;
    case mesh_pairing::ActionType::SEND_CONFIRM:
      *out_msg_type = MsgType::PAIR_CONFIRM;  return true;
    case mesh_pairing::ActionType::SEND_COMPLETE:
      *out_msg_type = MsgType::PAIR_COMPLETE; return true;
    default:
      return false;   /* non-wire action (NOTIFY_*, NONE) */
  }
}

/* Forward an Action returned by the pairing state machine. Sends any
 * outbound payload via mesh_transport (broadcast for DISCOVER, unicast
 * otherwise). Fires the integration-layer callback on terminal actions
 * (NOTIFY_*). */
static void dispatch_action(const mesh_pairing::Action& a) {
  MsgType wire_type;
  if (action_to_wire(a, &wire_type)) {
    /* Build the on-wire frame: 1-byte MsgType + payload bytes. */
    uint8_t frame[MAX_SESSION_FRAME];
    frame[0] = static_cast<uint8_t>(wire_type);
    if (a.payload_len > 0) {
      memcpy(frame + MSGTYPE_HEADER_LEN, a.payload, a.payload_len);
    }
    const size_t frame_len = MSGTYPE_HEADER_LEN + a.payload_len;

    if (a.type == mesh_pairing::ActionType::BROADCAST_DISCOVER) {
      /* Pre-membership: the joiner is not yet a peer of the initiator
       * (and vice versa), so mesh_transport::send_to_peer would reject
       * the FF MAC. send_raw bypasses the peer-table check and routes
       * straight to esp_now_send (which has the FF MAC pre-registered
       * by mesh_transport::init). */
      mesh_transport::send_raw(a.peer_mac, frame, frame_len);
    } else {
      mesh_transport::send_to_peer(a.peer_mac, frame, frame_len);
    }
  }

  /* Integration-layer callbacks for non-wire actions. */
  switch (a.type) {
    case mesh_pairing::ActionType::NOTIFY_CODE_READY:
      if (s_code_ready_cb) s_code_ready_cb(a.confirmation_code);
      break;
    case mesh_pairing::ActionType::NOTIFY_PAIRED: {
      /* Cache the opera name the joiner learned from the OFFER (the
       * initiator already cached its own at start_pairing_initiator;
       * re-caching here is harmless and is the only place the joiner
       * sees it). This module never writes NVS; the PairedCallback
       * persists it (mesh_state::save_opera_name, FE-gated). */
      set_opera_name(s_ctx.opera_name);
      uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN];
      const bool have_secret =
          mesh_pairing::consume_opera_secret(s_ctx, opera_secret);
      if (s_paired_cb) {
        s_paired_cb(have_secret ? opera_secret : nullptr, a.confirmation_code);
      }
      /* Wipe the local copy after the callback returns — the integration
       * layer was responsible for persisting it. secure_zero (volatile +
       * asm barrier) so the compiler can't elide this. */
      secure_zero(opera_secret, sizeof(opera_secret));
      break;
    }
    case mesh_pairing::ActionType::NOTIFY_FAILED:
      if (s_failed_cb) s_failed_cb();
      break;
    default:
      break;
  }
}

/* PR 5c-4 helper: look up a trusted peer by sender_fp. Returns nullptr
 * if no match. O(N) with N=MAX_TRUSTED_PEERS=8 — sub-microsecond. */
static TrustedPeer* find_trusted_peer(
    const uint8_t sender_fp[mesh_crypto::FINGERPRINT_LEN]) {
  for (size_t i = 0; i < MAX_TRUSTED_PEERS; ++i) {
    if (!s_trusted_peers[i].in_use) continue;
    if (mesh_crypto::ct_equal(s_trusted_peers[i].sender_fp, sender_fp,
                              mesh_crypto::FINGERPRINT_LEN)) {
      return &s_trusted_peers[i];
    }
  }
  return nullptr;
}

static CounterTombstone* find_tombstone(
    const uint8_t sender_fp[mesh_crypto::FINGERPRINT_LEN]) {
  for (size_t i = 0; i < MAX_COUNTER_TOMBSTONES; ++i) {
    if (!s_tombstones[i].in_use) continue;
    if (mesh_crypto::ct_equal(s_tombstones[i].sender_fp, sender_fp,
                              mesh_crypto::FINGERPRINT_LEN)) {
      return &s_tombstones[i];
    }
  }
  return nullptr;
}

/* Park a counter for a fingerprint that is no longer trusted. Keeps the
 * higher of an existing tombstone and `counter`; a zero counter (the peer
 * never got a frame through) needs no tombstone. Returns true iff a
 * tombstone was created or raised. */
static bool tombstone_put(const uint8_t sender_fp[mesh_crypto::FINGERPRINT_LEN],
                          uint64_t      counter) {
  if (counter == 0) return false;
  CounterTombstone* t = find_tombstone(sender_fp);
  if (t != nullptr) {
    if (counter <= t->last_counter) return false;
    t->last_counter = counter;
    t->stamp = ++s_tombstone_stamp;
    return true;
  }
  for (size_t i = 0; i < MAX_COUNTER_TOMBSTONES; ++i) {
    if (!s_tombstones[i].in_use) { t = &s_tombstones[i]; break; }
  }
  if (t == nullptr) {   /* full: evict the oldest */
    t = &s_tombstones[0];
    for (size_t i = 1; i < MAX_COUNTER_TOMBSTONES; ++i) {
      if (s_tombstones[i].stamp < t->stamp) t = &s_tombstones[i];
    }
  }
  memcpy(t->sender_fp, sender_fp, mesh_crypto::FINGERPRINT_LEN);
  t->last_counter = counter;
  t->stamp        = ++s_tombstone_stamp;
  t->in_use       = true;
  return true;
}

/* Drop a trusted-peer slot, keeping its replay counter as a tombstone. The
 * one place a slot is emptied outside deinit(). */
static void drop_trusted_slot(TrustedPeer* p) {
  tombstone_put(p->sender_fp, p->last_counter);
  memset(p, 0, sizeof(*p));
}

/* True while a pairing exchange is between start_* and a terminal state.
 * Used so disable/leave only cancel a pairing that is actually running
 * (cancel() from IDLE would fire a spurious FailedCallback). */
static bool pairing_in_progress() {
  switch (s_ctx.state) {
    case mesh_pairing::State::IDLE:
    case mesh_pairing::State::PAIRED:
    case mesh_pairing::State::FAILED:
      return false;
    default:
      return true;
  }
}

static void reset_alerts() {
  s_alerts_received = 0;
  memset(s_alert_ring, 0, sizeof(s_alert_ring));
  s_alert_head  = 0;
  s_alert_count = 0;
}

/* End any rotation in flight, without committing it. */
static void reset_rekey() {
  mesh_rekey::context_init(s_rekey);
  secure_zero(s_rekey_removed_pub, sizeof(s_rekey_removed_pub));
  s_rekey_removed_pub_set = false;
}

/* Build [1-byte session msg type][signed envelope] for an
 * opera-authenticated send. Bumps the outbound counter. Returns the total
 * frame length, or 0 when there is no opera or signing/serialization
 * fails. Used by the F10 senders; the three pre-F10 senders keep their
 * own inline copies of the same sequence. */
static size_t build_signed_frame(mesh_envelope::MsgType type,
                                 const uint8_t*         payload,
                                 size_t                 payload_len,
                                 uint32_t               now_ms,
                                 uint8_t*               out,
                                 size_t                 out_cap) {
  if (!s_opera_id_set || out == nullptr || out_cap < 1) return 0;
  mesh_envelope::Header header;
  header.version   = mesh_envelope::PROTOCOL_VERSION;
  header.msg_type  = static_cast<uint8_t>(type);
  memcpy(header.opera_id,  s_opera_id,  sizeof(header.opera_id));
  memcpy(header.sender_fp, s_sender_fp, sizeof(header.sender_fp));
  header.counter   = ++s_outbound_counter;
  header.timestamp = now_ms;
  out[0] = static_cast<uint8_t>(type);
  const size_t env_len = mesh_envelope::serialize_signed(
      header, payload, payload_len,
      s_device_priv, s_device_pub,
      out + 1, out_cap - 1);
  return env_len == 0 ? 0 : 1 + env_len;
}

/* Forget a trusted peer entirely: copy out its pubkey, drop its transport
 * MAC when one is bound (so later broadcasts stop reaching it), then
 * unregister it. Returns false when fp is not trusted. */
static bool forget_peer(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN],
                        uint8_t       pubkey_out[mesh_crypto::PUBKEY_LEN]) {
  TrustedPeer* p = find_trusted_peer(fp);
  if (p == nullptr) return false;
  memcpy(pubkey_out, p->pubkey, mesh_crypto::PUBKEY_LEN);
  if (p->mac_known) mesh_transport::remove_peer(p->mac);
  drop_trusted_slot(p);
  return true;
}

/* Sign and send one rekey payload: broadcast, or unicast to dest_fp's
 * verified MAC (broadcast when none is bound yet). Signed under the
 * CURRENT opera_id — the ACK ordering depends on it. */
static void send_rekey_frame(const mesh_rekey::Action& a, bool broadcast,
                             uint32_t now_ms) {
  uint8_t frame[1 + mesh_envelope::MAX_FRAME_LEN];
  const size_t n = build_signed_frame(
      static_cast<mesh_envelope::MsgType>(static_cast<uint8_t>(a.msg_type)),
      a.payload, a.payload_len, now_ms, frame, sizeof(frame));
  if (n == 0) return;
  const TrustedPeer* dest = broadcast ? nullptr : find_trusted_peer(a.dest_fp);
  if (dest != nullptr && dest->mac_known) {
    mesh_transport::send_to_peer(dest->mac, frame, n);
  } else {
    mesh_transport::broadcast(frame, n);
  }
  secure_zero(frame, sizeof(frame));
}

/* Switch to a rotated secret: rebind opera_id (the outbound counter is
 * kept — receivers track it per fingerprint), forget the listed peers,
 * and hand the secret + the pubkeys of every peer this rotation dropped to
 * the integration layer — `already_forgotten_pub` (the initiator's removed
 * peer, dropped from the table at start) first, when given. */
static void install_rotated_secret(const uint8_t new_secret[mesh_crypto::OPERA_SECRET_LEN],
                                   const uint8_t (*fps)[mesh_crypto::FINGERPRINT_LEN],
                                   size_t        n_fps,
                                   const uint8_t* already_forgotten_pub) {
  set_opera_secret(new_secret);
  uint8_t forgotten[MAX_TRUSTED_PEERS][mesh_crypto::PUBKEY_LEN];
  size_t  n_forgotten = 0;
  if (already_forgotten_pub != nullptr) {
    memcpy(forgotten[n_forgotten++], already_forgotten_pub, mesh_crypto::PUBKEY_LEN);
  }
  for (size_t i = 0; i < n_fps && n_forgotten < MAX_TRUSTED_PEERS; ++i) {
    if (forget_peer(fps[i], forgotten[n_forgotten])) ++n_forgotten;
  }
  if (s_rekey_commit_cb) s_rekey_commit_cb(new_secret, forgotten, n_forgotten);
}

/* Apply one mesh_rekey::Action, then wipe it (it may carry the secret). */
static void apply_rekey_action(mesh_rekey::Action& a, uint32_t now_ms) {
  switch (a.type) {
    case mesh_rekey::ActionType::BROADCAST_OFFER:
      send_rekey_frame(a, /*broadcast=*/true, now_ms);
      break;
    case mesh_rekey::ActionType::SEND_ACCEPT:
    case mesh_rekey::ActionType::SEND_SECRET:
      send_rekey_frame(a, /*broadcast=*/false, now_ms);
      break;
    case mesh_rekey::ActionType::ACK_AND_INSTALL:
      /* ORDER MATTERS: the ACK must verify under the opera_id the
       * initiator still holds, so it goes out before the switch. */
      send_rekey_frame(a, /*broadcast=*/false, now_ms);
      install_rotated_secret(a.new_secret, &a.removed_fp, 1, nullptr);
      break;
    case mesh_rekey::ActionType::COMMIT: {
      /* Consume the removed peer's stashed pubkey before the handler runs. */
      uint8_t removed_pub[mesh_crypto::PUBKEY_LEN];
      const bool have_removed = s_rekey_removed_pub_set;
      memcpy(removed_pub, s_rekey_removed_pub, sizeof(removed_pub));
      secure_zero(s_rekey_removed_pub, sizeof(s_rekey_removed_pub));
      s_rekey_removed_pub_set = false;
      install_rotated_secret(a.new_secret, a.dropped, a.dropped_count,
                             have_removed ? removed_pub : nullptr);
      break;
    }
    case mesh_rekey::ActionType::ABORT:
    case mesh_rekey::ActionType::NONE:
    default:
      break;
  }
  mesh_rekey::wipe(a);
}

/* Dispatch a verified opera-authenticated frame by envelope msg_type.
 * Called from on_opera_frame after parse_and_verify + counter check
 * have both passed. */
static void dispatch_verified(TrustedPeer&               peer,
                              const mesh_envelope::Header& hdr,
                              const uint8_t*             payload,
                              size_t                     payload_len) {
  switch (static_cast<mesh_envelope::MsgType>(hdr.msg_type)) {
    case mesh_envelope::MsgType::BEACON_EVENT: {
      if (s_beacon_event_cb == nullptr) return;
      mesh_beacon::BeaconState state;
      char                     label[mesh_beacon::MAX_LABEL_BYTES + 1];
      if (!mesh_beacon::decode(payload, payload_len,
                               &state, label, sizeof(label))) {
        return;   /* malformed payload — drop silently */
      }
      s_beacon_event_cb(peer.sender_fp, state, label);
      break;
    }
    case mesh_envelope::MsgType::CHANNEL_LOCK: {
      if (s_channel_lock_cb == nullptr) return;
      uint8_t                    channel;
      mesh_channel_hop::Reason   reason;
      if (!mesh_channel_hop::decode(payload, payload_len,
                                    &channel, &reason)) {
        return;
      }
      s_channel_lock_cb(peer.sender_fp, channel, reason);
      break;
    }
    case mesh_envelope::MsgType::HUB_ELECTION: {
      if (s_hub_election_cb == nullptr) return;
      mesh_hub_election::Event event;
      uint8_t                  elected_fp[mesh_hub_election::FINGERPRINT_LEN];
      if (!mesh_hub_election::decode(payload, payload_len,
                                     &event, elected_fp)) {
        return;
      }
      s_hub_election_cb(peer.sender_fp, event, elected_fp);
      break;
    }
    case mesh_envelope::MsgType::TAMPER_ALERT: {
      mesh_alert::Kind kind;
      uint8_t          severity    = 0;
      uint32_t         witness_seq = 0;
      if (!mesh_alert::decode(payload, payload_len,
                              &kind, &severity, &witness_seq)) {
        return;   /* malformed payload — drop silently, count nothing */
      }
      peer.alerts_received++;
      s_alerts_received++;
      mesh_alert::Record& r = s_alert_ring[s_alert_head];
      r.timestamp_ms = s_last_process_ms;
      memcpy(r.sender_fp, peer.sender_fp, sizeof(r.sender_fp));
      r.kind        = kind;
      r.severity    = severity;
      r.witness_seq = witness_seq;
      s_alert_head = (s_alert_head + 1) % MAX_ALERT_HISTORY;
      if (s_alert_count < MAX_ALERT_HISTORY) ++s_alert_count;
      if (s_tamper_alert_cb) {
        s_tamper_alert_cb(peer.sender_fp, kind, severity, witness_seq);
      }
      break;
    }
    case mesh_envelope::MsgType::REKEY_OFFER:
    case mesh_envelope::MsgType::REKEY_ACCEPT:
    case mesh_envelope::MsgType::REKEY_SECRET:
    case mesh_envelope::MsgType::REKEY_ACK: {
      if (payload == nullptr) return;
      /* Copy the sender fp: a COMMIT below may forget `peer`'s slot. */
      uint8_t sender_fp[mesh_crypto::FINGERPRINT_LEN];
      memcpy(sender_fp, peer.sender_fp, sizeof(sender_fp));
      mesh_rekey::Action a = mesh_rekey::receive(
          s_rekey, s_sender_fp,
          static_cast<mesh_rekey::MsgType>(hdr.msg_type),
          sender_fp, payload, payload_len, s_last_process_ms);
      apply_rekey_action(a, s_last_process_ms);
      break;
    }
    case mesh_envelope::MsgType::LEAVE_OPERA: {
      if (payload_len != 0) return;   /* LEAVE carries no payload */
      /* Copy out before unregistering: `peer` is the slot being zeroed. */
      uint8_t fp    [mesh_crypto::FINGERPRINT_LEN];
      uint8_t pubkey[mesh_crypto::PUBKEY_LEN];
      memcpy(fp,     peer.sender_fp, sizeof(fp));
      memcpy(pubkey, peer.pubkey,    sizeof(pubkey));
      /* The frame verified under the SIGNER's key, so this can only ever
       * remove the signer's own entry. */
      unregister_trusted_peer(fp);
      if (s_peer_left_cb) s_peer_left_cb(fp, pubkey);
      break;
    }
    default:
      break;
  }
}

/* PR 5c-4: handle an opera-authenticated frame (type_byte >= 16). The
 * full signed envelope (38B header + payload + 64B signature) starts
 * at data + 1. We must:
 *   1. Validate frame_len is at least HEADER_LEN + SIG_LEN.
 *   2. Peek the sender_fp from the header without verifying yet.
 *   3. Look up the trusted peer by sender_fp.
 *   4. parse_and_verify with that peer's pubkey.
 *   5. Reject if opera_id doesn't match our own (cross-opera leak).
 *   6. Reject if counter <= peer.last_counter (replay).
 *   7. Update peer.last_counter and dispatch by msg_type.
 *
 * Steps 1-7 ALL drop silently on failure — there's no error feedback
 * to the (possibly malicious) sender. */
static void on_opera_frame(const uint8_t mac[mesh_transport::MESH_TRANSPORT_MAC_LEN],
                           const uint8_t* data, size_t len) {
  /* data[0] is the session msg-type byte; the envelope starts at +1. */
  const uint8_t* env       = data + MSGTYPE_HEADER_LEN;
  const size_t   env_len   = len   - MSGTYPE_HEADER_LEN;
  if (env_len < mesh_envelope::MIN_FRAME_LEN) return;

  /* Step 2: peek sender_fp via the canonical offset constant rather
   * than hand-rolled byte arithmetic — keeps the header layout pinned
   * in mesh_envelope.h. */
  const uint8_t* sender_fp_in_frame = env + mesh_envelope::OFFSET_SENDER_FP;

  TrustedPeer* peer = find_trusted_peer(sender_fp_in_frame);
  if (peer == nullptr) return;            /* unknown sender */

  /* Step 4: parse + signature verify. */
  mesh_envelope::Header  hdr;
  const uint8_t*         payload     = nullptr;
  size_t                 payload_len = 0;
  if (!mesh_envelope::parse_and_verify(env, env_len, peer->pubkey,
                                       &hdr, &payload, &payload_len)) {
    return;                                /* forged or corrupt */
  }

  /* Step 5: cross-opera leak. parse_and_verify already checked version
   * and signature; we additionally check the opera_id matches ours so
   * a different opera that happened to pair with this same sender
   * pubkey can't deliver events into our world. */
  if (!s_opera_id_set) return;
  if (!mesh_crypto::ct_equal(hdr.opera_id, s_opera_id,
                             mesh_crypto::OPERA_ID_LEN)) {
    return;
  }

  /* Step 6: replay defense — strict monotonic counter per-peer. The
   * sender's outbound counter increments per send (PR 5c-3); the
   * receiver tracks last_counter per peer. counter==last_counter is
   * a replay; only counter>last_counter advances. */
  if (hdr.counter <= peer->last_counter) return;
  peer->last_counter = hdr.counter;

  /* Every check passed: at this instant the source MAC provably spoke
   * for this fingerprint. Record it for the /api/mesh/peers liveness
   * join; refreshed on every verified frame so an address change heals
   * on the peer's next transmission. */
  memcpy(peer->mac, mac, mesh_transport::MESH_TRANSPORT_MAC_LEN);
  peer->mac_known = true;

  /* Step 7: dispatch by envelope msg_type. */
  dispatch_verified(*peer, hdr, payload, payload_len);
}

/* mesh_transport recv callback. Decodes the 1-byte MsgType envelope
 * and routes to either the pairing state machine (type_byte <= 4) or
 * the opera-authenticated dispatch (type_byte >= 16). */
static void on_transport_recv(const uint8_t mac[6],
                              const uint8_t* data, size_t len,
                              int8_t /*rssi*/) {
  if (!s_running || data == nullptr || len < MSGTYPE_HEADER_LEN) return;
  const uint8_t type_byte = data[0];

  /* PAIR_* (0..4) — pre-membership pairing traffic, no envelope. */
  if (type_byte <= static_cast<uint8_t>(MsgType::PAIR_COMPLETE)) {
    const mesh_pairing::MsgType pair_type =
        static_cast<mesh_pairing::MsgType>(type_byte);
    const uint8_t* payload     = data + MSGTYPE_HEADER_LEN;
    const size_t   payload_len = len  - MSGTYPE_HEADER_LEN;
    /* now_ms isn't readily available in this callback context, but
     * mesh_pairing::receive uses it only for the tamper-path nothing-
     * else, so 0 is acceptable. The tick() path supplies a real now_ms
     * for timeout enforcement. */
    mesh_pairing::Action a = mesh_pairing::receive(s_ctx, mac, pair_type,
                                                    payload, payload_len, 0);
    dispatch_action(a);
    return;
  }

  /* 5..15 — reserved for future pairing extensions. Drop silently. */
  if (type_byte < static_cast<uint8_t>(mesh_envelope::MsgType::HEARTBEAT)) return;

  /* >=16 — opera-authenticated traffic. PR 5c-4 routes it here; the
   * peer table + signature verify + replay check happen inside. */
  on_opera_frame(mac, data, len);
}

/* ──────────────────────────────────────────────────────────────────────────
 * LIFECYCLE
 * ────────────────────────────────────────────────────────────────────────── */

bool init(const uint8_t device_pubkey [mesh_crypto::PUBKEY_LEN],
          const uint8_t device_privkey[mesh_crypto::PRIVKEY_LEN]) {
  if (s_initialized) return true;
  if (device_pubkey == nullptr || device_privkey == nullptr) return false;

  memcpy(s_device_pub,  device_pubkey,  mesh_crypto::PUBKEY_LEN);
  memcpy(s_device_priv, device_privkey, mesh_crypto::PRIVKEY_LEN);
  mesh_pairing::context_init(s_ctx);
  mesh_transport::set_recv_callback(&on_transport_recv);
  s_initialized = true;
  return true;
}

void deinit() {
  if (!s_initialized) return;
  mesh_transport::set_recv_callback(nullptr);
  mesh_pairing::context_init(s_ctx);   /* wipes ephem/session/secret */
  secure_zero(s_device_priv, sizeof(s_device_priv));
  s_paired_cb = nullptr;
  s_failed_cb = nullptr;
  s_code_ready_cb = nullptr;
  /* PR 5c-3 follow-up: clear opera-auth state so a deinit()/init()
   * cycle starts clean — without this, has_opera_secret() would lie
   * about a stale opera_id from the prior run. The opera_id and
   * sender_fp aren't secret (they're advertised in every signed
   * broadcast) but the staleness alone causes incorrect identity
   * binding on the next send. */
  s_opera_id_set     = false;
  secure_zero(s_opera_id,  sizeof(s_opera_id));
  secure_zero(s_sender_fp, sizeof(s_sender_fp));
  s_outbound_counter = 0;
  s_opera_name[0]    = '\0';
  s_enabled          = true;
  s_last_process_ms  = 0;
  reset_alerts();
  reset_rekey();
  secure_zero(&s_slot_req,    sizeof(s_slot_req));
  secure_zero(&s_slot_result, sizeof(s_slot_result));
  __atomic_store_n(&s_slot_state, (uint8_t)SLOT_IDLE, __ATOMIC_RELEASE);
  /* PR 5c-4: wipe the trusted-peer table + handler so a deinit()/init()
   * cycle doesn't carry stale peers or replay counters into the next
   * session. The pubkeys aren't secret but the staleness alone would
   * let an attacker that scraped a paired peer's pubkey replay any
   * recorded frame whose counter is <= the cached last_counter — a
   * real (if narrow) freshness violation. */
  memset(s_trusted_peers, 0, sizeof(s_trusted_peers));
  memset(s_tombstones, 0, sizeof(s_tombstones));
  s_tombstone_stamp = 0;
  s_beacon_event_cb = nullptr;
  s_channel_lock_cb = nullptr;
  s_hub_election_cb = nullptr;
  s_peer_left_cb    = nullptr;
  s_tamper_alert_cb = nullptr;
  s_rekey_commit_cb = nullptr;
  s_running = false;
  s_initialized = false;
}

bool start() {
  if (!s_initialized) return false;
  if (!s_enabled) return false;   /* disabled ⇒ not running */
  s_running = true;
  return true;
}

void stop() {
  s_running = false;
}

bool is_running() { return s_running; }

void set_enabled(bool enabled) {
  if (!enabled) {
    /* Tear down a pairing in flight while we can still dispatch its
     * NOTIFY_FAILED (cancel_pairing is a no-op once stopped). */
    if (s_running && pairing_in_progress()) cancel_pairing();
    /* A rotation cannot finish while stopped; drop it (the REST handler
     * refuses to disable mid-rotation, so this is the last resort). */
    reset_rekey();
    s_enabled = false;
    stop();
    return;
  }
  s_enabled = true;
  if (s_initialized) start();
}

bool is_enabled() { return s_enabled; }

void set_paired_callback    (PairedCallback     cb) { s_paired_cb     = cb; }
void set_failed_callback    (FailedCallback     cb) { s_failed_cb     = cb; }
void set_code_ready_callback(CodeReadyCallback  cb) { s_code_ready_cb = cb; }

/* ──────────────────────────────────────────────────────────────────────────
 * PAIRING ENTRY POINTS
 * ────────────────────────────────────────────────────────────────────────── */

bool start_pairing_initiator(const uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN],
                             const char*   opera_name,
                             uint32_t      now_ms) {
  if (!s_running) return false;
  mesh_pairing::Action a =
      mesh_pairing::start_initiator(s_ctx, s_device_pub, s_device_priv,
                                    opera_secret, opera_name, now_ms);
  if (a.type == mesh_pairing::ActionType::NONE) return false;
  /* Cache the opera display name for GET /api/mesh. The initiator knows
   * it up front (it's the existing opera's name); the joiner learns it
   * from the OFFER and caches it on NOTIFY_PAIRED. RAM copy only — see
   * s_opera_name for where it is persisted. */
  set_opera_name(opera_name);
  dispatch_action(a);
  return true;
}

bool start_pairing_joiner(uint32_t now_ms) {
  if (!s_running) return false;
  mesh_pairing::Action a =
      mesh_pairing::start_joiner(s_ctx, s_device_pub, s_device_priv, now_ms);
  if (a.type == mesh_pairing::ActionType::NONE) return false;
  dispatch_action(a);
  return true;
}

bool confirm_pairing_code(uint32_t now_ms) {
  if (!s_running) return false;
  mesh_pairing::Action a = mesh_pairing::confirm_code(s_ctx, now_ms);
  if (a.type == mesh_pairing::ActionType::NONE) return false;
  dispatch_action(a);
  return true;
}

void cancel_pairing() {
  if (!s_running) return;
  mesh_pairing::Action a = mesh_pairing::cancel(s_ctx);
  dispatch_action(a);
}

mesh_pairing::State pairing_state()        { return s_ctx.state; }
uint32_t            pairing_confirmation_code() { return s_ctx.confirmation_code; }

bool get_paired_peer_pubkey(uint8_t out[mesh_crypto::PUBKEY_LEN]) {
  if (out == nullptr) return false;
  /* peer_pubkey is captured at OFFER (initiator side) / ACCEPT
   * (joiner side); both happen well before PAIRED. We gate on
   * AWAITING_CONFIRM-or-later so the accessor doesn't expose a
   * stale buffer from a prior pairing attempt before the new one
   * has populated it. */
  switch (s_ctx.state) {
    case mesh_pairing::State::AWAITING_CONFIRM:
    case mesh_pairing::State::AWAITING_CONFIRM_PEER:
    case mesh_pairing::State::PAIRED:
      memcpy(out, s_ctx.peer_pubkey, mesh_crypto::PUBKEY_LEN);
      return true;
    default:
      return false;
  }
}

/* ──────────────────────────────────────────────────────────────────────────
 * MAIN LOOP
 * ────────────────────────────────────────────────────────────────────────── */

static void drain_request(uint32_t now_ms);   /* REST request slot, below */

void process(uint32_t now_ms) {
  s_last_process_ms = now_ms;
  /* A queued REST request runs first, and even while stopped: enabling
   * and leaving must work on a disabled mesh. */
  drain_request(now_ms);
  if (!s_running) return;
  mesh_pairing::Action a = mesh_pairing::tick(s_ctx, now_ms);
  dispatch_action(a);
  /* Rotation timeout: initiator commits (dropping the non-ACKed),
   * survivor aborts and keeps the old secret. */
  mesh_rekey::Action r = mesh_rekey::tick(s_rekey, now_ms);
  apply_rekey_action(r, now_ms);
}

/* ──────────────────────────────────────────────────────────────────────────
 * OPERA-AUTHENTICATED BROADCAST (PR 5c-3)
 *
 * The supporting state lives at the top of this TU alongside the
 * other lifecycle-managed state so deinit() can wipe it in one place.
 *
 *   s_opera_id_set   — false until set_opera_secret() succeeds.
 *   s_opera_id       — 16 bytes; sha256_domain(DOMAIN_OPERA_ID, secret)
 *                      truncated. Cached so we don't re-hash on every send.
 *   s_sender_fp      — 8 bytes; sha256_domain(DOMAIN_FINGERPRINT,
 *                      s_device_pub) truncated. Cached at first
 *                      set_opera_secret() since device_pub doesn't
 *                      change post-init().
 *   s_outbound_counter — monotonic per-process, RAM only — NOT
 *                      persisted. Receivers DO persist their per-peer
 *                      last_counter (mesh_state replay_ctrs), so after
 *                      this device reboots, a peer that restored a
 *                      higher last_counter drops this device's frames as
 *                      replays until the counter climbs past it. Open
 *                      item (F14 report): persist or epoch the outbound
 *                      counter.
 * ────────────────────────────────────────────────────────────────────────── */


bool set_opera_secret(const uint8_t opera_secret[mesh_crypto::OPERA_SECRET_LEN]) {
  if (opera_secret == nullptr) return false;
  if (!s_initialized) return false;   /* device keypair must be loaded first */

  mesh_crypto::compute_opera_id(opera_secret, s_opera_id);
  mesh_crypto::compute_fingerprint(s_device_pub, s_sender_fp);
  s_opera_id_set = true;
  return true;
}

bool has_opera_secret() {
  return s_opera_id_set;
}

bool get_opera_id(uint8_t out[mesh_crypto::OPERA_ID_LEN]) {
  if (out == nullptr || !s_opera_id_set) return false;
  memcpy(out, s_opera_id, mesh_crypto::OPERA_ID_LEN);
  return true;
}

bool has_opera() {
  return has_opera_secret();
}

void set_opera_name(const char* name) {
  if (name == nullptr) { s_opera_name[0] = '\0'; return; }
  strncpy(s_opera_name, name, sizeof(s_opera_name) - 1);
  s_opera_name[sizeof(s_opera_name) - 1] = '\0';
}

void get_opera_name(char* out, size_t cap) {
  if (out == nullptr || cap == 0) return;
  strncpy(out, s_opera_name, cap - 1);
  out[cap - 1] = '\0';
}

bool send_beacon_event(mesh_beacon::BeaconState state,
                       const char*              label,
                       uint32_t                 now_ms) {
  if (!s_initialized || !s_running) return false;
  if (!s_opera_id_set)             return false;

  /* 1. Encode the 25-byte BEACON_EVENT payload. */
  uint8_t payload[mesh_beacon::PAYLOAD_LEN];
  if (!mesh_beacon::encode(state, label, payload, sizeof(payload))) {
    return false;
  }

  /* 2. Build the envelope header. The outbound counter is bumped FIRST
   * so two simultaneous calls (which shouldn't happen — single-task
   * discipline — but the increment is local anyway) produce distinct
   * counters. */
  mesh_envelope::Header header;
  header.version   = mesh_envelope::PROTOCOL_VERSION;
  header.msg_type  = static_cast<uint8_t>(mesh_envelope::MsgType::BEACON_EVENT);
  memcpy(header.opera_id,  s_opera_id,  sizeof(header.opera_id));
  memcpy(header.sender_fp, s_sender_fp, sizeof(header.sender_fp));
  header.counter   = ++s_outbound_counter;
  header.timestamp = now_ms;

  /* 3. Serialize + sign. The signed frame is HEADER_LEN(38) +
   * PAYLOAD_LEN(25) + SIGNATURE_LEN(64) = 127 bytes. We then prepend
   * a 1-byte session msg type so the same wire dispatch that handles
   * PAIR_* frames can route this too: receivers see frame[0]=22 and
   * forward frame[1..] into mesh_envelope::parse_and_verify (peer-
   * table lookup added in PR 5c-4 / PR 4b). */
  uint8_t session_frame[1 + mesh_envelope::MAX_FRAME_LEN];
  session_frame[0] = static_cast<uint8_t>(mesh_envelope::MsgType::BEACON_EVENT);
  const size_t env_len = mesh_envelope::serialize_signed(
      header, payload, sizeof(payload),
      s_device_priv, s_device_pub,
      session_frame + 1, sizeof(session_frame) - 1);
  if (env_len == 0) return false;

  /* 4. Broadcast to every paired peer. mesh_transport::broadcast
   * returns the number of peers that accepted; 0 means no peers
   * known yet (legitimate during early boot before pairing). We
   * still consider that a failure for the send_beacon_event return
   * so the caller can choose to retry / queue. */
  const size_t n = mesh_transport::broadcast(session_frame, 1 + env_len);
  return n > 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * PR 5c-4 — TRUSTED PEER TABLE + BEACON_EVENT RECEIVER
 * ────────────────────────────────────────────────────────────────────────── */

bool register_trusted_peer(const uint8_t pubkey[mesh_crypto::PUBKEY_LEN]) {
  if (pubkey == nullptr) return false;

  /* Compute fingerprint once so we can both dedup and use it as the
   * lookup key. */
  uint8_t fp[mesh_crypto::FINGERPRINT_LEN];
  mesh_crypto::compute_fingerprint(pubkey, fp);

  /* Dedup: refuse re-registration of the same pubkey. Otherwise a
   * naive re-register call would zero last_counter and re-open the
   * replay window between (old last_counter, 0]. Callers that NEED
   * to rotate a peer's pubkey should clear_trusted_peers() first
   * and re-add the entire set (the counters survive that as
   * tombstones). */
  for (size_t i = 0; i < MAX_TRUSTED_PEERS; ++i) {
    if (s_trusted_peers[i].in_use &&
        mesh_crypto::ct_equal(s_trusted_peers[i].sender_fp, fp,
                              mesh_crypto::FINGERPRINT_LEN)) {
      return false;
    }
  }

  /* Find a free slot. */
  for (size_t i = 0; i < MAX_TRUSTED_PEERS; ++i) {
    if (!s_trusted_peers[i].in_use) {
      memcpy(s_trusted_peers[i].pubkey,    pubkey, mesh_crypto::PUBKEY_LEN);
      memcpy(s_trusted_peers[i].sender_fp, fp,     sizeof(fp));
      /* A device that was trusted before resumes from its tombstone —
       * everything it signed before it was dropped stays a replay. */
      s_trusted_peers[i].last_counter = 0;
      CounterTombstone* t = find_tombstone(fp);
      if (t != nullptr) {
        s_trusted_peers[i].last_counter = t->last_counter;
        memset(t, 0, sizeof(*t));
      }
      /* No verified frame yet this registration — the liveness join
       * reports the peer OFFLINE until one arrives. */
      memset(s_trusted_peers[i].mac, 0, sizeof(s_trusted_peers[i].mac));
      s_trusted_peers[i].mac_known    = false;
      s_trusted_peers[i].in_use       = true;
      return true;
    }
  }
  return false;   /* table full */
}

void clear_trusted_peers() {
  for (size_t i = 0; i < MAX_TRUSTED_PEERS; ++i) {
    if (s_trusted_peers[i].in_use) drop_trusted_slot(&s_trusted_peers[i]);
  }
  memset(s_trusted_peers, 0, sizeof(s_trusted_peers));
}

bool unregister_trusted_peer(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN]) {
  if (fp == nullptr) return false;
  TrustedPeer* p = find_trusted_peer(fp);
  if (p == nullptr) return false;
  drop_trusted_slot(p);
  return true;
}

size_t trusted_peer_count() {
  size_t n = 0;
  for (size_t i = 0; i < MAX_TRUSTED_PEERS; ++i) {
    if (s_trusted_peers[i].in_use) ++n;
  }
  return n;
}

size_t get_replay_counters(uint8_t (*out_fps)[mesh_crypto::FINGERPRINT_LEN],
                           uint64_t* out_counters,
                           size_t    out_cap) {
  if (out_fps == nullptr || out_counters == nullptr) return 0;
  size_t n = 0;
  /* Tombstones persist too, so a reboot does not re-open their window.
   * Oldest first, then the live counters: restoring the blob in order
   * (main.cpp) re-creates tombstones in the same age order, and a live
   * entry that comes back as a tombstone (the device left the opera before
   * the next save) comes back as the newest — so if the table overflows,
   * eviction still takes the oldest. */
  uint32_t after = 0;   /* emit in stamp order: each pass takes the next-oldest */
  for (size_t k = 0; k < MAX_COUNTER_TOMBSTONES && n < out_cap; ++k) {
    const CounterTombstone* next = nullptr;
    for (size_t i = 0; i < MAX_COUNTER_TOMBSTONES; ++i) {
      const CounterTombstone& t = s_tombstones[i];
      if (!t.in_use || t.stamp <= after) continue;
      if (next == nullptr || t.stamp < next->stamp) next = &t;
    }
    if (next == nullptr) break;
    memcpy(out_fps[n], next->sender_fp, mesh_crypto::FINGERPRINT_LEN);
    out_counters[n] = next->last_counter;
    after = next->stamp;
    ++n;
  }
  for (size_t i = 0; i < MAX_TRUSTED_PEERS && n < out_cap; ++i) {
    if (!s_trusted_peers[i].in_use) continue;
    memcpy(out_fps[n], s_trusted_peers[i].sender_fp, mesh_crypto::FINGERPRINT_LEN);
    out_counters[n] = s_trusted_peers[i].last_counter;
    ++n;
  }
  return n;
}

size_t get_peer_links(PeerLink* out, size_t cap) {
  if (out == nullptr) return 0;
  size_t n = 0;
  for (size_t i = 0; i < MAX_TRUSTED_PEERS && n < cap; ++i) {
    if (!s_trusted_peers[i].in_use) continue;
    memcpy(out[n].fp,  s_trusted_peers[i].sender_fp, mesh_crypto::FINGERPRINT_LEN);
    memcpy(out[n].mac, s_trusted_peers[i].mac,       mesh_transport::MESH_TRANSPORT_MAC_LEN);
    out[n].mac_known = s_trusted_peers[i].mac_known;
    out[n].alerts_received = s_trusted_peers[i].alerts_received;
    ++n;
  }
  return n;
}

bool restore_replay_counter(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN],
                            uint64_t counter) {
  if (fp == nullptr) return false;
  for (size_t i = 0; i < MAX_TRUSTED_PEERS; ++i) {
    if (!s_trusted_peers[i].in_use) continue;
    if (memcmp(s_trusted_peers[i].sender_fp, fp, mesh_crypto::FINGERPRINT_LEN) == 0) {
      if (counter > s_trusted_peers[i].last_counter) {
        s_trusted_peers[i].last_counter = counter;
        return true;
      }
      return false;
    }
  }
  /* Not a trusted peer (any more): the persisted entry was a tombstone. */
  return tombstone_put(fp, counter);
}

void set_beacon_event_handler(beacon_event_received_fn fn) {
  s_beacon_event_cb = fn;
}

bool send_channel_lock(uint8_t channel,
                       mesh_channel_hop::Reason reason,
                       uint32_t now_ms) {
  if (!s_initialized || !s_running) return false;
  if (!s_opera_id_set)             return false;

  uint8_t payload[mesh_channel_hop::PAYLOAD_LEN];
  if (!mesh_channel_hop::encode(channel, reason, payload, sizeof(payload))) {
    return false;
  }

  mesh_envelope::Header header;
  header.version   = mesh_envelope::PROTOCOL_VERSION;
  header.msg_type  = static_cast<uint8_t>(mesh_envelope::MsgType::CHANNEL_LOCK);
  memcpy(header.opera_id,  s_opera_id,  sizeof(header.opera_id));
  memcpy(header.sender_fp, s_sender_fp, sizeof(header.sender_fp));
  header.counter   = ++s_outbound_counter;
  header.timestamp = now_ms;

  uint8_t session_frame[1 + mesh_envelope::MAX_FRAME_LEN];
  session_frame[0] = static_cast<uint8_t>(mesh_envelope::MsgType::CHANNEL_LOCK);
  const size_t env_len = mesh_envelope::serialize_signed(
      header, payload, sizeof(payload),
      s_device_priv, s_device_pub,
      session_frame + 1, sizeof(session_frame) - 1);
  if (env_len == 0) return false;

  const size_t n = mesh_transport::broadcast(session_frame, 1 + env_len);
  return n > 0;
}

void set_channel_lock_handler(channel_lock_received_fn fn) {
  s_channel_lock_cb = fn;
}

bool send_hub_election(mesh_hub_election::Event event,
                       const uint8_t fingerprint[mesh_crypto::FINGERPRINT_LEN],
                       uint32_t now_ms) {
  if (!s_initialized || !s_running) return false;
  if (!s_opera_id_set)             return false;

  uint8_t payload[mesh_hub_election::PAYLOAD_LEN];
  if (!mesh_hub_election::encode(event, fingerprint, payload, sizeof(payload))) {
    return false;
  }

  mesh_envelope::Header header;
  header.version   = mesh_envelope::PROTOCOL_VERSION;
  header.msg_type  = static_cast<uint8_t>(mesh_envelope::MsgType::HUB_ELECTION);
  memcpy(header.opera_id,  s_opera_id,  sizeof(header.opera_id));
  memcpy(header.sender_fp, s_sender_fp, sizeof(header.sender_fp));
  header.counter   = ++s_outbound_counter;
  header.timestamp = now_ms;

  uint8_t session_frame[1 + mesh_envelope::MAX_FRAME_LEN];
  session_frame[0] = static_cast<uint8_t>(mesh_envelope::MsgType::HUB_ELECTION);
  const size_t env_len = mesh_envelope::serialize_signed(
      header, payload, sizeof(payload),
      s_device_priv, s_device_pub,
      session_frame + 1, sizeof(session_frame) - 1);
  if (env_len == 0) return false;

  const size_t n = mesh_transport::broadcast(session_frame, 1 + env_len);
  return n > 0;
}

void set_hub_election_handler(hub_election_received_fn fn) {
  s_hub_election_cb = fn;
}

/* ──────────────────────────────────────────────────────────────────────────
 * F10 — LEAVE + TAMPER ALERTS
 * ────────────────────────────────────────────────────────────────────────── */

bool leave_opera(uint32_t now_ms) {
  bool notified = false;
  if (s_initialized && s_running && s_opera_id_set) {
    /* Signed under the CURRENT opera_id before we forget it, so the
     * survivors can verify it. Best effort: nobody listening is fine. */
    uint8_t frame[1 + mesh_envelope::MAX_FRAME_LEN];
    const size_t n = build_signed_frame(mesh_envelope::MsgType::LEAVE_OPERA,
                                        nullptr, 0, now_ms,
                                        frame, sizeof(frame));
    if (n > 0) notified = mesh_transport::broadcast(frame, n) > 0;
  }
  if (s_running && pairing_in_progress()) cancel_pairing();
  reset_rekey();                       /* leaving ends any rotation */
  clear_trusted_peers();               /* their counters stay as tombstones */
  s_opera_id_set     = false;
  secure_zero(s_opera_id,  sizeof(s_opera_id));
  secure_zero(s_sender_fp, sizeof(s_sender_fp));
  /* s_outbound_counter is deliberately KEPT: receivers keep this device's
   * last counter as a tombstone across the leave, so after a re-pair into
   * the same opera its frames must keep counting up from where they were,
   * not restart at 1 and be dropped as replays. */
  s_opera_name[0]    = '\0';
  reset_alerts();
  return notified;
}

void set_peer_left_handler(peer_left_fn fn) {
  s_peer_left_cb = fn;
}

bool send_tamper_alert(mesh_alert::Kind kind,
                       uint8_t          severity,
                       uint32_t         witness_seq,
                       uint32_t         now_ms) {
  if (!s_initialized || !s_running) return false;
  if (!s_opera_id_set)             return false;

  uint8_t payload[mesh_alert::PAYLOAD_LEN];
  if (!mesh_alert::encode(kind, severity, witness_seq,
                          payload, sizeof(payload))) {
    return false;
  }
  uint8_t frame[1 + mesh_envelope::MAX_FRAME_LEN];
  const size_t n = build_signed_frame(mesh_envelope::MsgType::TAMPER_ALERT,
                                      payload, sizeof(payload), now_ms,
                                      frame, sizeof(frame));
  if (n == 0) return false;
  return mesh_transport::broadcast(frame, n) > 0;
}

void set_tamper_alert_handler(tamper_alert_received_fn fn) {
  s_tamper_alert_cb = fn;
}

uint32_t alerts_received() { return s_alerts_received; }

size_t get_alerts(mesh_alert::Record* out, size_t cap) {
  if (out == nullptr) return 0;
  size_t n = 0;
  /* Newest first: walk back from the slot before the write head. */
  for (size_t k = 0; k < s_alert_count && n < cap; ++k) {
    const size_t idx = (s_alert_head + MAX_ALERT_HISTORY - 1 - k) % MAX_ALERT_HISTORY;
    out[n++] = s_alert_ring[idx];
  }
  return n;
}

void clear_alerts() {
  /* History only — the lifetime counters keep counting (WAP parity). */
  memset(s_alert_ring, 0, sizeof(s_alert_ring));
  s_alert_head  = 0;
  s_alert_count = 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * F10-rekey — PEER REMOVAL WITH opera_secret ROTATION
 * CRYPTO: maintainer review required before merge; bench-gated.
 * ────────────────────────────────────────────────────────────────────────── */

RemoveResult remove_peer(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN],
                         uint32_t      now_ms,
                         uint8_t       removed_pubkey_out[mesh_crypto::PUBKEY_LEN]) {
  if (fp == nullptr || removed_pubkey_out == nullptr) return RemoveResult::NOT_FOUND;
  if (!s_initialized || !s_running) return RemoveResult::DISABLED;
  if (!s_opera_id_set)              return RemoveResult::NO_OPERA;
  if (mesh_rekey::in_progress(s_rekey)) return RemoveResult::IN_FLIGHT;
  /* A pairing in flight would hand its joiner the secret this rotation is
   * about to retire — the one the removed device still holds. */
  if (pairing_in_progress())            return RemoveResult::PAIRING;
  if (find_trusted_peer(fp) == nullptr) return RemoveResult::NOT_FOUND;

  /* Everyone else who stays. */
  uint8_t survivors[MAX_TRUSTED_PEERS][mesh_crypto::FINGERPRINT_LEN];
  size_t  n = 0;
  for (size_t i = 0; i < MAX_TRUSTED_PEERS; ++i) {
    if (!s_trusted_peers[i].in_use) continue;
    if (mesh_crypto::ct_equal(s_trusted_peers[i].sender_fp, fp,
                              mesh_crypto::FINGERPRINT_LEN)) continue;
    memcpy(survivors[n++], s_trusted_peers[i].sender_fp, mesh_crypto::FINGERPRINT_LEN);
  }

  uint8_t id_bytes[4];
  mesh_crypto::fill_random(id_bytes, sizeof(id_bytes));
  const uint32_t rekey_id = (uint32_t)id_bytes[0] | ((uint32_t)id_bytes[1] << 8) |
                            ((uint32_t)id_bytes[2] << 16) | ((uint32_t)id_bytes[3] << 24);

  /* Start FIRST: a refused start must not leave the peer half-removed. */
  mesh_rekey::Action a = mesh_rekey::start(s_rekey, s_sender_fp, fp,
                                           survivors, n, rekey_id, now_ms);
  if (a.type == mesh_rekey::ActionType::NONE) return RemoveResult::FAILED;

  forget_peer(fp, removed_pubkey_out);
  memcpy(s_rekey_removed_pub, removed_pubkey_out, sizeof(s_rekey_removed_pub));
  s_rekey_removed_pub_set = true;
  const RemoveResult r = (a.type == mesh_rekey::ActionType::COMMIT)
                             ? RemoveResult::COMMITTED
                             : RemoveResult::STARTED;
  apply_rekey_action(a, now_ms);
  return r;
}

bool rekey_in_progress() { return mesh_rekey::in_progress(s_rekey); }

void set_rekey_commit_handler(rekey_commit_fn fn) { s_rekey_commit_cb = fn; }

/* ──────────────────────────────────────────────────────────────────────────
 * REST REQUEST SLOT (review fix) — see mesh_session.h and the state table
 * at s_slot_state.
 * ────────────────────────────────────────────────────────────────────────── */

static inline uint8_t slot_load() {
  return __atomic_load_n(&s_slot_state, __ATOMIC_ACQUIRE);
}
static inline void slot_store(uint8_t v) {
  __atomic_store_n(&s_slot_state, v, __ATOMIC_RELEASE);
}
static inline bool slot_cas(uint8_t from, uint8_t to) {
  return __atomic_compare_exchange_n(&s_slot_state, &from, to, /*weak=*/false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

bool submit_request(const Request& req) {
  if (!slot_cas(SLOT_IDLE, SLOT_CLAIMED)) return false;
  s_slot_req = req;
  s_slot_req.name[sizeof(s_slot_req.name) - 1] = '\0';
  slot_store(SLOT_PENDING);
  return true;
}

bool take_request_result(RequestResult* out) {
  if (out == nullptr) return false;
  if (!slot_cas(SLOT_DONE, SLOT_CLAIMED)) return false;
  *out = s_slot_result;
  secure_zero(&s_slot_result, sizeof(s_slot_result));
  slot_store(SLOT_IDLE);
  return true;
}

bool withdraw_request() {
  if (!slot_cas(SLOT_PENDING, SLOT_CLAIMED)) return false;
  secure_zero(&s_slot_req, sizeof(s_slot_req));
  slot_store(SLOT_IDLE);
  return true;
}

void abandon_request() {
  if (withdraw_request()) return;                      /* never ran */
  if (slot_cas(SLOT_RUNNING, SLOT_ABANDONED)) return;  /* its result is dropped */
  if (slot_cas(SLOT_DONE, SLOT_CLAIMED)) {             /* finished meanwhile */
    secure_zero(&s_slot_result, sizeof(s_slot_result));
    slot_store(SLOT_IDLE);
  }
}

/* Main loop: run one request against the session. */
static void execute_request(const Request& req, uint32_t now_ms, RequestResult* res) {
  res->type   = req.type;
  res->status = RequestStatus::OK;
  switch (req.type) {
    case RequestType::LEAVE:
      /* Leaving mid-rotation would split the survivors: the ones that
       * already installed keep the new secret, the rest abort. */
      if (mesh_rekey::in_progress(s_rekey)) {
        res->status = RequestStatus::REKEY_IN_FLIGHT;
        break;
      }
      res->notified = leave_opera(now_ms);
      /* The radio peer table goes too: nothing to talk to without an opera. */
      mesh_transport::clear_peers();
      break;
    case RequestType::SET_NAME:
      if (!s_opera_id_set) {
        res->status = RequestStatus::NO_OPERA;
        break;
      }
      set_opera_name(req.name);
      break;
    case RequestType::SET_ENABLED:
      /* A rotation cannot finish while the mesh is off. */
      if (!req.enabled && mesh_rekey::in_progress(s_rekey)) {
        res->status  = RequestStatus::REKEY_IN_FLIGHT;
        res->enabled = s_enabled;
        break;
      }
      set_enabled(req.enabled);
      res->enabled = s_enabled;
      break;
    case RequestType::CLEAR_ALERTS:
      clear_alerts();
      break;
    case RequestType::REMOVE:
      res->remove = remove_peer(req.fp, now_ms, res->removed_pubkey);
      break;
    case RequestType::NONE:
    default:
      res->status = RequestStatus::BAD_REQUEST;
      break;
  }
}

static void drain_request(uint32_t now_ms) {
  if (!slot_cas(SLOT_PENDING, SLOT_RUNNING)) return;
  Request req = s_slot_req;
  secure_zero(&s_slot_req, sizeof(s_slot_req));
  RequestResult res;
  memset(&res, 0, sizeof(res));
  execute_request(req, now_ms, &res);
  secure_zero(&req, sizeof(req));
  s_slot_result = res;
  secure_zero(&res, sizeof(res));
  if (!slot_cas(SLOT_RUNNING, SLOT_DONE)) {
    /* ABANDONED: the handler gave up; nobody will read this result. */
    secure_zero(&s_slot_result, sizeof(s_slot_result));
    slot_store(SLOT_IDLE);
  }
}

}  /* namespace mesh_session */
