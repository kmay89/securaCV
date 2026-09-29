/*
 * SecuraCV Canary — Opera mesh wire registry (spec §4.5)
 * Version 0.1.0
 *
 * CRYPTO-ADJACENT (v0.4): implemented, awaiting maintainer crypto review;
 * not bench-verified (U1 Track C2). Nothing here has run on two radios.
 *
 * ONE byte, ONE meaning, BOTH trees. Until v0.4 the PlatformIO mesh
 * (firmware/canary/lib/securacv_mesh) and canary-wap
 * (firmware/projects/canary-wap/arduino/canary_wap/mesh_network.h) each
 * numbered the type byte of their outer frame for themselves: TAMPER_ALERT
 * was 18 on one and 4 on the other, and canary-wap's CHANNEL_LOCK (20) and
 * HUB_ELECTION (21) sat on the PIO values of OFFLINE_IMMINENT and
 * WITNESS_RECORD — a frame with a valid signature could have been read as
 * a different message on the other side. The version byte differed too
 * (PIO 1, canary-wap 0), and PIO put an unsigned copy of the type ahead of
 * the envelope that canary-wap does not have. Spec §4.5 now states one
 * registry and one outer frame, and this header IS that registry: both
 * trees' enums take their values from it, and firmware/scripts/
 * check_mesh_sync.sh holds the canary-wap copy byte-identical to this file.
 *
 * The outer frame (spec §4.5), identical in both trees:
 *
 *   opera-authenticated (signed):
 *     [version = OPERA_VERSION][type 16..255][opera_id 16][sender_fp 8]
 *     [counter u64 LE][timestamp u32 LE][payload][Ed25519 sig 64]
 *     — mesh_envelope.h's layout, canary-wap's send_to_peer() layout. The
 *     signature is over everything before it, hashed under
 *     mesh_crypto::DOMAIN_MESSAGE ("securacv:mesh:message:v0") in both.
 *   pre-membership pairing (unsigned — there is no peer key yet):
 *     [type 8..12][the raw pairing payload struct]
 *
 * Why the first byte tells the two apart, and the rest of the radio too:
 *   • a signed frame starts with OPERA_VERSION (1);
 *   • a pairing frame starts with 8..12 — never 0 or 1, the two version
 *     bytes the trees have used as a first byte, so a receiver that keys on
 *     the first byte (both do) cannot take one for the other whatever the
 *     length. This is why the pairing block is 8..12 (canary-wap's) and not
 *     the PIO tree's old 0..4: PAIR_DISCOVER = 0 was canary-wap's version
 *     byte and PAIR_OFFER = 1 is the canonical one;
 *   • Chirp frames start with CHIRP_MAGIC (0xC4) and Beacon frames with
 *     BEACON_MAGIC (0xB1) — both kept out of the registry by the
 *     static_asserts at the end of this file.
 *
 * What the registry does NOT claim: the two trees still encode several
 * payloads differently (TAMPER_ALERT, HEARTBEAT, the pairing structs, the
 * pairing key derivation, spec §4.5 table), and they cannot pair with each
 * other yet, so no cross-tree frame can be verified today. The registry is
 * the precondition — a type byte now means the same thing on both sides —
 * not the whole of interoperability.
 *
 * Pure header: no Arduino, no ESP-IDF, no other mesh header, so a host
 * test can include it alone (test_mesh_wire.cpp, and canary-wap's
 * test_mesh_wire_wap.cpp against the staged copy).
 */

#ifndef SECURACV_MESH_WIRE_H
#define SECURACV_MESH_WIRE_H

#include <stdint.h>
#include <stdbool.h>

namespace mesh_wire {

/* Byte 0 of every signed opera frame. Receivers drop any other value.
 * v0.4 raised canary-wap from 0 to this; the PIO tree already used 1. */
constexpr uint8_t OPERA_VERSION = 1;

/* ── 0..15: pre-membership pairing, unsigned. 0..7 and 13..15 are reserved
 * and MUST stay unassigned (see the first-byte argument above). ── */
constexpr uint8_t PAIR_FIRST    = 8;
constexpr uint8_t PAIR_DISCOVER = 8;
constexpr uint8_t PAIR_OFFER    = 9;
constexpr uint8_t PAIR_ACCEPT   = 10;
constexpr uint8_t PAIR_CONFIRM  = 11;
constexpr uint8_t PAIR_COMPLETE = 12;
constexpr uint8_t PAIR_LAST     = 12;

/* ── 16..29: opera-authenticated traffic both trees carry (the PIO tree's
 * numbering since v0.1 of mesh_envelope.h; spec §4.2–§4.4). ── */
constexpr uint8_t OPERA_FIRST      = 16;
constexpr uint8_t HEARTBEAT        = 16;
constexpr uint8_t CSI_FEATURES     = 17;
constexpr uint8_t TAMPER_ALERT     = 18;
constexpr uint8_t POWER_ALERT      = 19;
constexpr uint8_t OFFLINE_IMMINENT = 20;
constexpr uint8_t WITNESS_RECORD   = 21;
constexpr uint8_t BEACON_EVENT     = 22;
constexpr uint8_t CHANNEL_LOCK     = 23;
constexpr uint8_t HUB_ELECTION     = 24;
constexpr uint8_t LEAVE_OPERA      = 25;
constexpr uint8_t REKEY_OFFER      = 26;   /* PIO §5.6 ephemeral rotation */
constexpr uint8_t REKEY_ACCEPT     = 27;
constexpr uint8_t REKEY_SECRET     = 28;
constexpr uint8_t REKEY_ACK        = 29;

/* ── 30..39: canary-wap's per-peer session layer (its AUTH exchange and its
 * session-key rotation, spec §3.1, §5.6 v0.2). The PIO tree keeps no
 * session keys and drops these as unknown; they are assigned here so no
 * PIO type can ever be given one of their values. ── */
constexpr uint8_t AUTH_CHALLENGE   = 30;
constexpr uint8_t AUTH_RESPONSE    = 31;
constexpr uint8_t AUTH_COMPLETE    = 32;
constexpr uint8_t PEER_LIST        = 33;
constexpr uint8_t ENCRYPTED        = 34;
constexpr uint8_t OPERA_REKEY      = 35;
constexpr uint8_t OPERA_REKEY_ACK  = 36;

/* 37..255 reserved. */
constexpr uint8_t ASSIGNED_LAST    = 36;

/* First bytes of the other two protocols that share the ESP-NOW radio on a
 * canary-wap (chirp_channel::CHIRP_MAGIC, beacon_wire's magic). A registry
 * value equal to one of these would let a receiver that keys on the first
 * byte confuse the protocols; the static_asserts below refuse it. */
constexpr uint8_t CHIRP_MAGIC  = 0xC4;
constexpr uint8_t BEACON_MAGIC = 0xB1;

inline bool is_pairing_type(uint8_t b) { return b >= PAIR_FIRST && b <= PAIR_LAST; }
inline bool is_opera_type(uint8_t b)   { return b >= OPERA_FIRST; }

/* The registry's own invariants, checked wherever it is compiled. */
static_assert(PAIR_FIRST > OPERA_VERSION,
              "a pairing type must never equal the version byte");
static_assert(PAIR_FIRST > 0, "0 was canary-wap's version byte; keep it unassigned");
static_assert(PAIR_LAST < OPERA_FIRST, "pairing and opera blocks must not overlap");
static_assert(HEARTBEAT == OPERA_FIRST && REKEY_ACK == 29 && AUTH_CHALLENGE == 30,
              "the two opera blocks must be contiguous and disjoint");
static_assert(ASSIGNED_LAST < CHIRP_MAGIC && ASSIGNED_LAST < BEACON_MAGIC,
              "no registry value may equal another protocol's magic byte");
static_assert(OPERA_VERSION != CHIRP_MAGIC && OPERA_VERSION != BEACON_MAGIC,
              "the version byte must not equal another protocol's magic byte");

}  /* namespace mesh_wire */

#endif  /* SECURACV_MESH_WIRE_H */
