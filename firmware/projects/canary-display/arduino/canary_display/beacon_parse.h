#pragma once
#include <stdint.h>
#include <stddef.h>
#include "canary/fleet/fleet_model.h"  // BeaconStatus

// Fleet-link presence beacon parser (pure — only <stdint.h>/<stddef.h> and the
// dependency-free BeaconStatus). The WAP emits this as a CONTINUOUS BLE
// manufacturer-specific advertisement so a display discovers nearby WAPs with
// NO MQTT broker and NO home WiFi. It shares the chirp's company id (0xFFFF)
// but is disambiguated by BOTH size (11 vs the chirp's 17) AND type byte.
//
// AUTHORITATIVE WIRE CONTRACT (documented identically on the WAP side in
// canary-wap/arduino/canary_wap/fleet_beacon.h). Full manufacturer-data blob
// as the scanner sees it via getManufacturerData(), 11 bytes:
//
//   [0-1]  0xFF 0xFF     company id (little-endian), added by NimBLE
//   [2]    0x10          BEACON_PRESENCE type (distinct from chirp 0x01..0x05)
//   [3]    0x01          schema version
//   [4]    flags         bit0=tamper, bit1=mic_muted, bit2=degraded,
//                        bit3=on_wifi_sta, bit4=alert_active
//   [5]    battery_pct   0..100, 0xFF = unknown/none
//   [6]    health_pct    0..100, 0xFF = unknown
//   [7-8]  chain_height  low 16 bits, uint16 little-endian
//   [9-10] fp2           last 2 bytes of the pubkey fingerprint (the same 2
//                        bytes the chirp carries at [15-16] and the "SCV-XXXX"
//                        name derives from)
//
// VERSION 2 (13 bytes) is version 1 plus two trailing bytes, emitted by
// witnesses with an optical pipeline (canary-vision): a detection class token
// and a confidence percentage — and deliberately nothing more (no identity, no
// timestamp; the advert IS the "now"). flags bit4 (alert_active) is set while
// the sender holds a live detection; [11..12] then say what, how confidently:
//
//   [3]    0x02          schema version 2
//   [11]   detect_class  0x00 none, 0x01 person, 0x02 vehicle, 0x03 animal,
//                        0x04 package
//   [12]   detect_score  detection confidence 0..100 (%), 0xFF = unknown
//
// Both parsers below accept BOTH versions — length and version must agree.

namespace canary::net {

// Wire constants (mirror fleet_beacon.h on the WAP).
static constexpr size_t  BEACON_MFG_LEN     = 11;
static constexpr size_t  BEACON_MFG_V2_LEN  = 13;
static constexpr uint8_t BEACON_TYPE        = 0x10;
static constexpr uint8_t BEACON_VERSION     = 0x01;
static constexpr uint8_t BEACON_VERSION_2   = 0x02;
static constexpr uint8_t BEACON_UNKNOWN_PCT = 0xFF;
static constexpr uint8_t BEACON_SCORE_UNKNOWN = 0xFF;

// Detection class tokens (v2 byte [11]) — the ObjectClass vocabulary and
// nothing beyond it.
static constexpr uint8_t BEACON_DETECT_NONE    = 0x00;
static constexpr uint8_t BEACON_DETECT_PERSON  = 0x01;
static constexpr uint8_t BEACON_DETECT_VEHICLE = 0x02;
static constexpr uint8_t BEACON_DETECT_ANIMAL  = 0x03;
static constexpr uint8_t BEACON_DETECT_PACKAGE = 0x04;

static constexpr uint8_t BEACON_FLAG_TAMPER      = 0x01;  // bit0
static constexpr uint8_t BEACON_FLAG_MIC_MUTED   = 0x02;  // bit1
static constexpr uint8_t BEACON_FLAG_DEGRADED    = 0x04;  // bit2
static constexpr uint8_t BEACON_FLAG_ON_WIFI_STA = 0x08;  // bit3
static constexpr uint8_t BEACON_FLAG_ALERT       = 0x10;  // bit4

// Shared header validation: size/company/type, and the version byte that
// AGREES with the size (11 -> v1, 13 -> v2). Both entry points below use it.
static inline bool beacon_header_ok(const uint8_t* mfg, size_t n) {
  if (!mfg) return false;
  if (n != BEACON_MFG_LEN && n != BEACON_MFG_V2_LEN) return false;
  if (mfg[0] != 0xFF || mfg[1] != 0xFF) return false;   // company id 0xFFFF (LE)
  if (mfg[2] != BEACON_TYPE) return false;
  const uint8_t want = (n == BEACON_MFG_V2_LEN) ? BEACON_VERSION_2
                                                : BEACON_VERSION;
  return mfg[3] == want;
}

// Validate a full manufacturer-data blob as a fleet-link beacon and hex-encode
// its 2 fingerprint bytes ([9],[10]) into fp4_out ("abcd\0", 4 lowercase hex +
// NUL) — the SAME encoding chirp_scan.cpp uses, so a beacon and a chirp from
// the same canary suffix-match the same witness. Returns false (and leaves
// fp4_out untouched) on any size / company-id / type / version mismatch.
static inline bool beacon_fp4_from_mfg(const uint8_t* mfg, size_t n,
                                       char fp4_out[5]) {
  if (!fp4_out) return false;
  if (!beacon_header_ok(mfg, n)) return false;
  static const char H[] = "0123456789abcdef";
  fp4_out[0] = H[(mfg[9] >> 4) & 0xF];
  fp4_out[1] = H[mfg[9] & 0xF];
  fp4_out[2] = H[(mfg[10] >> 4) & 0xF];
  fp4_out[3] = H[mfg[10] & 0xF];
  fp4_out[4] = '\0';
  return true;
}

// Decode the status fields ([4..8], plus [11..12] on a v2 blob) into a
// BeaconStatus. Same validation as beacon_fp4_from_mfg. 0xFF battery/health
// decode to not-present; the low-16 chain height is always considered present.
// Returns false on a malformed blob (out is left unmodified).
static inline bool beacon_parse_status(const uint8_t* mfg, size_t n,
                                       canary::fleet::BeaconStatus& out) {
  if (!beacon_header_ok(mfg, n)) return false;

  const uint8_t flags = mfg[4];
  out.tamper    = (flags & BEACON_FLAG_TAMPER) != 0;
  out.mic_muted = (flags & BEACON_FLAG_MIC_MUTED) != 0;
  out.degraded  = (flags & BEACON_FLAG_DEGRADED) != 0;
  out.alert     = (flags & BEACON_FLAG_ALERT) != 0;

  if (n == BEACON_MFG_V2_LEN) {
    out.detect_class = mfg[11];
    out.detect_score = (mfg[12] == BEACON_SCORE_UNKNOWN) ? -1
                                                         : (int16_t)mfg[12];
  } else {
    out.detect_class = BEACON_DETECT_NONE;
    out.detect_score = -1;
  }

  if (mfg[5] == BEACON_UNKNOWN_PCT) {
    out.battery_pct = -1;
    out.battery_present = false;
  } else {
    out.battery_pct = (int16_t)mfg[5];
    out.battery_present = true;
  }

  if (mfg[6] == BEACON_UNKNOWN_PCT) {
    out.health = -1;
    out.health_present = false;
  } else {
    out.health = (int16_t)mfg[6];
    out.health_present = true;
  }

  out.chain_seq = (uint32_t)(mfg[7] | ((uint16_t)mfg[8] << 8));  // LE low 16
  out.chain_present = true;
  return true;
}

}  // namespace canary::net
