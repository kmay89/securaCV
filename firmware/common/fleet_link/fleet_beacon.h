/*
 * SecuraCV Canary — Fleet-Link Presence Beacon (wire format)
 *
 * A continuous BLE manufacturer-specific advertisement (AD type 0xFF) that a
 * SecuraCV display can discover directly — NO MQTT broker and NO home WiFi.
 * It rides the SAME company id (0xFFFF) as the Chirp, but is disambiguated by
 * BOTH length (11 vs the chirp's 17) AND its type byte (0x10, distinct from
 * the chirp types 0x01..0x05).
 *
 * AUTHORITATIVE WIRE CONTRACT (documented identically on the display side in
 * include/canary/net/beacon_parse.h). Full manufacturer-data blob as the
 * scanner sees it (NimBLE prepends the 2 company bytes), 11 bytes:
 *
 *   [0-1]  0xFF 0xFF     company id (little-endian), added by NimBLE
 *   [2]    0x10          BEACON_PRESENCE type (distinct from chirp 0x01..0x05)
 *   [3]    0x01          schema version
 *   [4]    flags         bit0=tamper, bit1=mic_muted, bit2=degraded,
 *                        bit3=on_wifi_sta, bit4=alert_active
 *   [5]    battery_pct   0..100, 0xFF = unknown/none
 *   [6]    health_pct    0..100, 0xFF = unknown
 *   [7-8]  chain_height  low 16 bits, uint16 little-endian
 *   [9-10] fp2           last 2 bytes of the pubkey fingerprint (the same 2
 *                        bytes the chirp carries at [15-16] and the "SCV-XXXX"
 *                        name derives from)
 *
 * This header is PURE: it pulls in no NimBLE/Arduino symbols, so a plain g++
 * host test can round-trip the builder byte-for-byte (tests_host/
 * test_fleet_beacon.cpp). Any Arduino-only convenience is guarded out below.
 */

#ifndef FLEET_BEACON_H
#define FLEET_BEACON_H

#include <stdint.h>
#include <stddef.h>

// ════════════════════════════════════════════════════════════════
// WIRE CONSTANTS
// ════════════════════════════════════════════════════════════════

// Company id the beacon shares with the chirp (NimBLE prepends it LE).
#define FLEET_BEACON_COMPANY_ID   0xFFFF

// Manufacturer-data type byte ([2]) — distinct from chirp types 0x01..0x05.
#define FLEET_BEACON_TYPE         0x10
#define FLEET_BEACON_VERSION      0x01

// Payload written AFTER the company id (bytes [2..10] above) = 9 bytes.
#define FLEET_BEACON_PAYLOAD_LEN  9
// Full blob as the scanner sees it, including the 2 company bytes.
#define FLEET_BEACON_MFG_LEN      11

// Flag bits for byte [4].
#define FLEET_BEACON_FLAG_TAMPER      0x01  // bit0
#define FLEET_BEACON_FLAG_MIC_MUTED   0x02  // bit1
#define FLEET_BEACON_FLAG_DEGRADED    0x04  // bit2
#define FLEET_BEACON_FLAG_ON_WIFI_STA 0x08  // bit3
#define FLEET_BEACON_FLAG_ALERT       0x10  // bit4

// Sentinel for an unknown/absent battery or health percentage.
#define FLEET_BEACON_UNKNOWN_PCT      0xFF

// ════════════════════════════════════════════════════════════════
// PURE BUILDER
// ════════════════════════════════════════════════════════════════

// Clamp a signed percentage to a 0..100 byte, mapping out-of-range (<0 or
// >100) to 0xFF (unknown). Pure — host-testable.
static inline uint8_t fleet_beacon_pct(int pct) {
    if (pct < 0 || pct > 100) return FLEET_BEACON_UNKNOWN_PCT;
    return (uint8_t)pct;
}

// Write the 9 payload bytes ([2..10] of the on-air blob) into out. Does NOT
// write the company id — NimBLE prepends that. Returns FLEET_BEACON_PAYLOAD_LEN.
//
//   out[0] = type (0x10)          out[5] = chain_height low byte  (LE)
//   out[1] = version (0x01)       out[6] = chain_height high byte (LE)
//   out[2] = flags                out[7] = fp_b0
//   out[3] = battery_pct          out[8] = fp_b1
//   out[4] = health_pct
static inline size_t fleet_beacon_build(uint8_t out[FLEET_BEACON_PAYLOAD_LEN],
                                        uint8_t flags,
                                        int battery_pct,
                                        int health_pct,
                                        uint32_t chain_height,
                                        uint8_t fp_b0,
                                        uint8_t fp_b1) {
    const uint16_t chain_lo16 = (uint16_t)(chain_height & 0xFFFF);
    out[0] = FLEET_BEACON_TYPE;
    out[1] = FLEET_BEACON_VERSION;
    out[2] = flags;
    out[3] = fleet_beacon_pct(battery_pct);
    out[4] = fleet_beacon_pct(health_pct);
    out[5] = (uint8_t)(chain_lo16 & 0xFF);          // low 8 bits (LE)
    out[6] = (uint8_t)((chain_lo16 >> 8) & 0xFF);   // high 8 bits (LE)
    out[7] = fp_b0;
    out[8] = fp_b1;
    return FLEET_BEACON_PAYLOAD_LEN;
}

// ════════════════════════════════════════════════════════════════
// PURE PARSER (symmetry with the display side; host round-trips)
// ════════════════════════════════════════════════════════════════

// Parsed beacon fields. battery_pct/health_pct are -1 when the wire carried
// the 0xFF unknown sentinel.
struct FleetBeaconFields {
    uint8_t  flags;
    int      battery_pct;   // -1 = unknown
    int      health_pct;    // -1 = unknown
    uint16_t chain_lo16;
    uint8_t  fp_b0;
    uint8_t  fp_b1;
};

// Parse the FULL manufacturer-data blob (including the 2 company bytes) as the
// scanner delivers it. Validates length (11), company id (0xFFFF LE), type
// (0x10) and version (0x01). Returns false on any mismatch.
static inline bool fleet_beacon_parse(const uint8_t* mfg, size_t n,
                                      FleetBeaconFields* out) {
    if (!mfg || !out) return false;
    if (n != FLEET_BEACON_MFG_LEN) return false;
    if (mfg[0] != 0xFF || mfg[1] != 0xFF) return false;   // company id
    if (mfg[2] != FLEET_BEACON_TYPE) return false;
    if (mfg[3] != FLEET_BEACON_VERSION) return false;
    out->flags       = mfg[4];
    out->battery_pct = (mfg[5] == FLEET_BEACON_UNKNOWN_PCT) ? -1 : (int)mfg[5];
    out->health_pct  = (mfg[6] == FLEET_BEACON_UNKNOWN_PCT) ? -1 : (int)mfg[6];
    out->chain_lo16  = (uint16_t)(mfg[7] | ((uint16_t)mfg[8] << 8));
    out->fp_b0       = mfg[9];
    out->fp_b1       = mfg[10];
    return true;
}

#endif // FLEET_BEACON_H
