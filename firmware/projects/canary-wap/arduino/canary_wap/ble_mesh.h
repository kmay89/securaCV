/*
 * SecuraCV Canary — BLE Mesh control-plane (design + scaffolding)
 *
 * See docs/BLE_MESH_OPERA_TANDEM.md for the full design.
 *
 * Role: short-range household control plane. Three message types — chain
 * head heartbeat, bond advertise (IRK sync), familiar Bloom delta — ride
 * a household-scoped encrypted channel so Canaries in the same home can
 * sync identity / state without WiFi association. Opera mesh stays the
 * data plane (witness records, evidence) — see firmware/canary/lib/
 * mesh_network and firmware/canary/lib/securacv_chain.
 *
 * Status: scaffolding only. init() refuses until a transport (Option A:
 * ESP-BLE-MESH stack swap, Option B: BLE 5 extended advertising, Option
 * C: Opera-as-transport) is wired in. The wire-format structs below
 * are the contract regardless of transport.
 */

#ifndef SECURACV_BLE_MESH_H
#define SECURACV_BLE_MESH_H

#include <stdint.h>
#include <stddef.h>

namespace ble_mesh {

// ─── Wire format ──────────────────────────────────────────────────────────
// Every BLE Mesh control message is AES-CCM-encrypted with the household
// AppKey. The header below sits IN the cleartext AAD; the type-specific
// payload follows in ciphertext. Sequence numbers are household-wide and
// per-sender, so a stale frame won't be re-applied.

enum MsgType : uint8_t {
  MSG_CHAIN_HEAD_HEARTBEAT = 0x01,
  MSG_BOND_ADVERTISE       = 0x02,
  MSG_FAMILIAR_BLOOM_DELTA = 0x03,
};

struct __attribute__((packed)) MsgHeader {
  uint16_t magic;      // 'SC' = 0x5343 (LE)
  uint8_t  version;    // 0x01
  uint8_t  msg_type;
  uint16_t household;  // hash16 of household netkey (lookup hint)
  uint32_t sender_id;  // last 4 bytes of sender's pubkey fingerprint
  uint32_t seq;        // monotonic, per-sender, household-wide
};
static_assert(sizeof(MsgHeader) == 14, "MsgHeader wire size must be 14 B");

struct __attribute__((packed)) ChainHeadHeartbeat {
  uint32_t chain_height;
  uint8_t  chain_head[8];   // truncated SHA-256 of latest record
};
static_assert(sizeof(ChainHeadHeartbeat) == 12, "ChainHeadHeartbeat wire size mismatch");

struct __attribute__((packed)) BondAdvertise {
  uint8_t irk[16];          // peer's BLE Identity Resolving Key
};
static_assert(sizeof(BondAdvertise) == 16, "BondAdvertise wire size mismatch");

struct __attribute__((packed)) FamiliarBloomDelta {
  uint8_t bloom_delta[12];
};
static_assert(sizeof(FamiliarBloomDelta) == 12, "FamiliarBloomDelta wire size mismatch");

// ─── Public API ───────────────────────────────────────────────────────────

// Bring up the mesh layer. Returns true on success. Currently always returns
// false — a transport hasn't been wired yet. See docs/BLE_MESH_OPERA_TANDEM.md.
//
// netkey:  16-byte household NetKey (encryption + integrity)
// appkey:  16-byte household AppKey (per-application key, scoped to control plane)
// my_sender_id:  last 4 bytes of this device's pubkey fingerprint
bool init(const uint8_t netkey[16], const uint8_t appkey[16], uint32_t my_sender_id);
void deinit();
bool is_running();

// Outbound publishes — return true if the message was queued for broadcast.
bool publish_chain_head(const ChainHeadHeartbeat& msg);
bool publish_bond(const BondAdvertise& msg);
bool publish_familiar_delta(const FamiliarBloomDelta& msg);

// Inbound handlers — wired by the call site (rf_presence::init, household::init,
// chain manager) so the mesh layer doesn't need to know about its consumers.
typedef void (*ChainHeadHandler)(uint32_t sender_id, const ChainHeadHeartbeat& msg);
typedef void (*BondHandler)(uint32_t sender_id, const BondAdvertise& msg);
typedef void (*BloomDeltaHandler)(uint32_t sender_id, const FamiliarBloomDelta& msg);
void set_handlers(ChainHeadHandler h_chain, BondHandler h_bond, BloomDeltaHandler h_bloom);

// Debug / introspection
uint32_t messages_sent();
uint32_t messages_received();
uint32_t auth_failures();
uint32_t replay_drops();

} // namespace ble_mesh

#endif // SECURACV_BLE_MESH_H
