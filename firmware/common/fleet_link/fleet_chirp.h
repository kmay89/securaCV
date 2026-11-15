/*
 * SecuraCV Canary — Fleet Chirp advert (wire format)
 *
 * The Chirp is the connectionless BLE alert: a manufacturer-specific
 * advertisement (AD type 0xFF) the canary-wap puts on air for ~2 s at a time
 * (ble_chirp.h) and every receiver in the fleet reads:
 *
 *   - the canary's Scout roster feed      (lib/securacv_ble_scan/src/fleet_roster_feed.cpp)
 *   - canary-sense and canary-vision      (src/net/fleet_roster_scan.cpp)
 *   - the display                         (src/net/chirp_scan.cpp)
 *   - the Scout pairing filter, both trees (ble_scout_advert_kind.h)
 *
 * all through fleet_chirp_parse() / fleet_chirp_matches() below. Two more
 * read the same bytes without this parser: the canary-wap's own nearby scan
 * (ble_nearby.h, on a wider rule of its own, its offsets static_asserted to
 * the ones here) and the iPhone app's decoder (ios/.../Wire/ChirpAdvert.swift,
 * held to this file by ChirpAdvertTests' source belt).
 *
 * It rides the SAME company id (0xFFFF) as the presence beacon
 * (fleet_beacon.h) and is told apart from it by BOTH length (17 vs the
 * beacon's 11 / 13) AND type byte (0x01..0x05 vs the beacon's 0x10).
 *
 * AUTHORITATIVE WIRE CONTRACT (docs/ble_protocol.md §5 says the same in
 * prose). Full manufacturer-data blob as the scanner sees it, 17 bytes:
 *
 *   [0-1]   0xFF 0xFF     company id (little-endian)
 *   [2]     type          0x01 alert, 0x02 heartbeat, 0x03 tamper,
 *                         0x04 witness, 0x05 boot
 *   [3-6]   hour_bucket   uint32, BIG-endian (most significant byte first,
 *                         unlike the beacon's little-endian chain height):
 *                         hours since the SENDER booted (millis()/3600000).
 *                         It carries no wall-clock meaning; compare buckets
 *                         from one sender over a short window only.
 *   [7-14]  chain8        the first 8 bytes of the sender's witness chain
 *                         head (all zero before it has one)
 *   [15-16] fp2           the last 2 bytes of the sender's pubkey
 *                         fingerprint: the same 2 bytes the beacon carries
 *                         at [9-10] and the "SCV-XXXX" name derives from, so
 *                         a Chirp and a beacon from one device land on one
 *                         roster entry
 *
 * fleet_chirp_build() writes the 15 bytes after the company id; the sender
 * puts the company id in front of them (ble_chirp.h's sendChirp()).
 * fleet_chirp_parse() reads the whole 17-byte blob, company id included,
 * which is what NimBLE's getManufacturerData() hands a receiver.
 *
 * A Chirp advert is UNSIGNED: it proves nothing about who sent it. Receivers
 * use it for presence and attention only, never to mark anything verified.
 * It shares a name, and nothing else, with the Chirp community witness
 * channel (spec/chirp_channel_v0.md, the canary-wap's chirp_channel.cpp),
 * signed template alerts over ESP-NOW; nothing here touches that channel.
 *
 * This header is PURE: <stdint.h> and <stddef.h> only, no NimBLE, no
 * Arduino, so a plain g++ host test round-trips it byte for byte
 * (test_fleet_chirp.cpp beside it). One spelling of the layout for every
 * tree:
 *   - canonical here; the canary, canary-sense and canary-vision PlatformIO
 *     builds reach it through -I firmware/common/fleet_link;
 *   - the canary-display PlatformIO build reaches it as
 *     "fleet_link/fleet_chirp.h" through -I firmware/common, and its
 *     Arduino sketch gets a flat copy from setup.sh regen
 *     (check_display_arduino_sync.sh);
 *   - the canary-wap Arduino sketch, which compiles only its own folder,
 *     carries a byte-identical copy (check_fleet_beacon_sync.sh).
 */

#ifndef FLEET_CHIRP_H
#define FLEET_CHIRP_H

#include <stdint.h>
#include <stddef.h>

// ════════════════════════════════════════════════════════════════
// WIRE CONSTANTS
// ════════════════════════════════════════════════════════════════

// Company id the Chirp shares with the presence beacon ("testing" id).
#define FLEET_CHIRP_COMPANY_ID   0xFFFF

// Payload after the company id (bytes [2..16] above) = 15 bytes.
#define FLEET_CHIRP_PAYLOAD_LEN  15
// Full blob as the scanner sees it, including the 2 company bytes.
#define FLEET_CHIRP_MFG_LEN      17

// Type byte [2]. Contiguous: a receiver accepts FIRST..LAST and nothing else.
#define FLEET_CHIRP_TYPE_ALERT      0x01  // manual or sensor-triggered alert
#define FLEET_CHIRP_TYPE_HEARTBEAT  0x02  // periodic "I'm alive" (every 5 min)
#define FLEET_CHIRP_TYPE_TAMPER     0x03  // tamper event detected
#define FLEET_CHIRP_TYPE_WITNESS    0x04  // new witness record created
#define FLEET_CHIRP_TYPE_BOOT       0x05  // device just booted
#define FLEET_CHIRP_TYPE_FIRST      FLEET_CHIRP_TYPE_ALERT
#define FLEET_CHIRP_TYPE_LAST       FLEET_CHIRP_TYPE_BOOT

// Field offsets in the FULL blob (company id at [0-1]). The payload the
// builder writes is the same bytes, FLEET_CHIRP_MFG_LEN - FLEET_CHIRP_PAYLOAD_LEN
// (= 2) earlier.
#define FLEET_CHIRP_OFF_TYPE    2   // 1 byte
#define FLEET_CHIRP_OFF_HOUR    3   // 4 bytes, big-endian
#define FLEET_CHIRP_OFF_CHAIN   7   // FLEET_CHIRP_CHAIN_LEN bytes
#define FLEET_CHIRP_OFF_FP      15  // 2 bytes: fp_b0, fp_b1
#define FLEET_CHIRP_CHAIN_LEN   8

// ════════════════════════════════════════════════════════════════
// PURE BUILDER
// ════════════════════════════════════════════════════════════════

// Write the 15 payload bytes ([2..16] of the on-air blob) into out. Does NOT
// write the company id. chain8 may be nullptr (no chain head yet): the chain
// bytes are then zero. type is written as given; a receiver refuses one
// outside FLEET_CHIRP_TYPE_FIRST..LAST. Returns FLEET_CHIRP_PAYLOAD_LEN.
//
//   out[0]     = type
//   out[1..4]  = hour_bucket, most significant byte first
//   out[5..12] = chain8[0..7] (zeros when chain8 is nullptr)
//   out[13]    = fp_b0
//   out[14]    = fp_b1
static inline size_t fleet_chirp_build(uint8_t out[FLEET_CHIRP_PAYLOAD_LEN],
                                       uint8_t type,
                                       uint32_t hour_bucket,
                                       const uint8_t* chain8,
                                       uint8_t fp_b0,
                                       uint8_t fp_b1) {
    const size_t at = FLEET_CHIRP_MFG_LEN - FLEET_CHIRP_PAYLOAD_LEN;  // 2
    out[FLEET_CHIRP_OFF_TYPE - at]     = type;
    out[FLEET_CHIRP_OFF_HOUR - at]     = (uint8_t)((hour_bucket >> 24) & 0xFF);
    out[FLEET_CHIRP_OFF_HOUR - at + 1] = (uint8_t)((hour_bucket >> 16) & 0xFF);
    out[FLEET_CHIRP_OFF_HOUR - at + 2] = (uint8_t)((hour_bucket >> 8) & 0xFF);
    out[FLEET_CHIRP_OFF_HOUR - at + 3] = (uint8_t)(hour_bucket & 0xFF);
    for (size_t i = 0; i < FLEET_CHIRP_CHAIN_LEN; i++) {
        out[FLEET_CHIRP_OFF_CHAIN - at + i] = chain8 ? chain8[i] : 0;
    }
    out[FLEET_CHIRP_OFF_FP - at]     = fp_b0;
    out[FLEET_CHIRP_OFF_FP - at + 1] = fp_b1;
    return FLEET_CHIRP_PAYLOAD_LEN;
}

// ════════════════════════════════════════════════════════════════
// PURE PARSER
// ════════════════════════════════════════════════════════════════

// Parsed Chirp fields.
struct FleetChirpFields {
    uint8_t  type;                            // FLEET_CHIRP_TYPE_* (FIRST..LAST)
    uint32_t hour_bucket;                     // sender's hours since boot
    uint8_t  chain8[FLEET_CHIRP_CHAIN_LEN];   // chain head prefix
    uint8_t  fp_b0;                           // fingerprint suffix, [15]
    uint8_t  fp_b1;                           // fingerprint suffix, [16]
};

// True when a FULL manufacturer-data blob (company id included) has the
// Chirp's shape: exactly FLEET_CHIRP_MFG_LEN bytes, company 0xFFFF and a type
// byte in FLEET_CHIRP_TYPE_FIRST..LAST. The one rule for "this advert is a
// Chirp": fleet_chirp_parse() asks it, and so does the Scout pairing filter.
static inline bool fleet_chirp_matches(const uint8_t* mfg, size_t n) {
    if (!mfg || n != FLEET_CHIRP_MFG_LEN) return false;
    if (mfg[0] != (uint8_t)(FLEET_CHIRP_COMPANY_ID & 0xFF) ||
        mfg[1] != (uint8_t)((FLEET_CHIRP_COMPANY_ID >> 8) & 0xFF)) {
        return false;
    }
    const uint8_t type = mfg[FLEET_CHIRP_OFF_TYPE];
    return type >= FLEET_CHIRP_TYPE_FIRST && type <= FLEET_CHIRP_TYPE_LAST;
}

// Parse a FULL manufacturer-data blob as the scanner delivers it. Returns
// false, and leaves *out untouched, on anything fleet_chirp_matches()
// refuses (or a null out).
static inline bool fleet_chirp_parse(const uint8_t* mfg, size_t n,
                                     FleetChirpFields* out) {
    if (!out || !fleet_chirp_matches(mfg, n)) return false;
    out->type = mfg[FLEET_CHIRP_OFF_TYPE];
    out->hour_bucket = ((uint32_t)mfg[FLEET_CHIRP_OFF_HOUR] << 24) |
                       ((uint32_t)mfg[FLEET_CHIRP_OFF_HOUR + 1] << 16) |
                       ((uint32_t)mfg[FLEET_CHIRP_OFF_HOUR + 2] << 8) |
                       (uint32_t)mfg[FLEET_CHIRP_OFF_HOUR + 3];
    for (size_t i = 0; i < FLEET_CHIRP_CHAIN_LEN; i++) {
        out->chain8[i] = mfg[FLEET_CHIRP_OFF_CHAIN + i];
    }
    out->fp_b0 = mfg[FLEET_CHIRP_OFF_FP];
    out->fp_b1 = mfg[FLEET_CHIRP_OFF_FP + 1];
    return true;
}

#endif // FLEET_CHIRP_H
