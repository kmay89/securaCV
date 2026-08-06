// Host-side test for the CANONICAL fleet_beacon.h (firmware/common/fleet_link)
// — the direct BLE "fleet link" presence beacon builder/parser shared byte-
// identically by the canary and canary-wap firmwares. Pure header, no
// NimBLE/Arduino glue: the test includes it directly and asserts the exact
// 9-byte payload put on air (bytes [2..10] of the on-air manufacturer blob;
// NimBLE prepends the 2 company bytes) plus the full-blob parser round-trip.
//
// This mirrors firmware/projects/canary-wap/tests_host/test_fleet_beacon.cpp,
// but includes the canonical copy so the shared wire format is exercised from
// its single source of truth. The canary-wap copy is kept byte-identical by
// firmware/scripts/check_fleet_beacon_sync.sh.

#include "fleet_beacon.h"

#include <cstdio>
#include <cstdint>

static int g_failures = 0;
#define CHECK(cond, msg)                                            \
  do {                                                             \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; }   \
    else         { printf("ok:   %s\n", (msg)); }                 \
  } while (0)

int main() {
  // ── Nominal build: known values, LE chain, fp bytes ──────────────────
  {
    uint8_t p[FLEET_BEACON_PAYLOAD_LEN];
    const uint8_t flags = FLEET_BEACON_FLAG_TAMPER | FLEET_BEACON_FLAG_ON_WIFI_STA;
    size_t n = fleet_beacon_build(p, flags, /*battery=*/76, /*health=*/88,
                                  /*chain=*/0x1234, /*fp0=*/0xAB, /*fp1=*/0xCD);
    CHECK(n == 9, "builder returns 9 payload bytes");
    CHECK(p[0] == 0x10, "type byte is 0x10 (BEACON_PRESENCE)");
    CHECK(p[1] == 0x01, "schema version is 0x01");
    CHECK(p[2] == flags, "flags byte round-trips (tamper|on_wifi_sta)");
    CHECK(p[3] == 76,    "battery byte");
    CHECK(p[4] == 88,    "health byte");
    CHECK(p[5] == 0x34,  "chain_lo low byte (LE)");
    CHECK(p[6] == 0x12,  "chain_lo high byte (LE)");
    CHECK(p[7] == 0xAB,  "fp byte 0");
    CHECK(p[8] == 0xCD,  "fp byte 1");
  }

  // ── Unknown battery/health map to 0xFF ───────────────────────────────
  {
    uint8_t p[FLEET_BEACON_PAYLOAD_LEN];
    fleet_beacon_build(p, 0, /*battery=*/-1, /*health=*/-1, 0, 0, 0);
    CHECK(p[3] == 0xFF, "battery <0 -> 0xFF unknown");
    CHECK(p[4] == 0xFF, "health <0 -> 0xFF unknown");

    fleet_beacon_build(p, 0, /*battery=*/101, /*health=*/200, 0, 0, 0);
    CHECK(p[3] == 0xFF, "battery >100 -> 0xFF unknown");
    CHECK(p[4] == 0xFF, "health >100 -> 0xFF unknown");

    // Boundaries stay in-range.
    fleet_beacon_build(p, 0, /*battery=*/0, /*health=*/100, 0, 0, 0);
    CHECK(p[3] == 0,   "battery 0 is valid");
    CHECK(p[4] == 100, "health 100 is valid");
  }

  // ── Only the low 16 bits of chain_height ride the wire ───────────────
  {
    uint8_t p[FLEET_BEACON_PAYLOAD_LEN];
    fleet_beacon_build(p, 0, 50, 50, /*chain=*/0xDEAD0042u, 0, 0);
    CHECK(p[5] == 0x42, "chain low byte from a >16-bit height");
    CHECK(p[6] == 0x00, "chain high byte truncated to low 16 bits");
  }

  // ── Round-trip build -> full-blob parse ──────────────────────────────
  {
    uint8_t p[FLEET_BEACON_PAYLOAD_LEN];
    const uint8_t flags = FLEET_BEACON_FLAG_MIC_MUTED | FLEET_BEACON_FLAG_DEGRADED |
                          FLEET_BEACON_FLAG_ALERT;
    fleet_beacon_build(p, flags, 42, 7, 0xBEEF, 0x12, 0x34);

    // Assemble the full 11-byte on-air blob (company id + payload).
    uint8_t mfg[FLEET_BEACON_MFG_LEN];
    mfg[0] = FLEET_BEACON_COMPANY_ID & 0xFF;
    mfg[1] = (FLEET_BEACON_COMPANY_ID >> 8) & 0xFF;
    for (size_t i = 0; i < FLEET_BEACON_PAYLOAD_LEN; i++) mfg[2 + i] = p[i];

    FleetBeaconFields f{};
    CHECK(fleet_beacon_parse(mfg, sizeof(mfg), &f), "parse accepts a well-formed blob");
    CHECK(f.flags == flags,        "parsed flags match");
    CHECK(f.battery_pct == 42,     "parsed battery matches");
    CHECK(f.health_pct == 7,       "parsed health matches");
    CHECK(f.chain_lo16 == 0xBEEF,  "parsed chain_lo16 matches (LE)");
    CHECK(f.fp_b0 == 0x12 && f.fp_b1 == 0x34, "parsed fp bytes match");

    // Unknown sentinels decode to -1.
    fleet_beacon_build(p, 0, -1, -1, 0, 0, 0);
    for (size_t i = 0; i < FLEET_BEACON_PAYLOAD_LEN; i++) mfg[2 + i] = p[i];
    CHECK(fleet_beacon_parse(mfg, sizeof(mfg), &f), "parse accepts unknown-pct blob");
    CHECK(f.battery_pct == -1 && f.health_pct == -1, "0xFF decodes to -1 unknown");
  }

  // ── Parser rejects malformed blobs ───────────────────────────────────
  {
    uint8_t mfg[FLEET_BEACON_MFG_LEN] = {0xFF, 0xFF, 0x10, 0x01, 0, 50, 50, 0, 0, 0x11, 0x22};
    FleetBeaconFields f{};
    CHECK(!fleet_beacon_parse(mfg, 10, &f), "reject short blob (10 bytes)");
    CHECK(!fleet_beacon_parse(mfg, 17, &f), "reject chirp-sized blob (17 bytes)");
    uint8_t bad = mfg[0]; mfg[0] = 0x00;
    CHECK(!fleet_beacon_parse(mfg, FLEET_BEACON_MFG_LEN, &f), "reject wrong company id");
    mfg[0] = bad;
    mfg[2] = 0x01;  // a chirp type, not the beacon type
    CHECK(!fleet_beacon_parse(mfg, FLEET_BEACON_MFG_LEN, &f), "reject wrong type byte");
    mfg[2] = 0x10;
    mfg[3] = 0x02;  // v2 version on a v1-length blob — length/version must agree
    CHECK(!fleet_beacon_parse(mfg, FLEET_BEACON_MFG_LEN, &f), "reject v2 version at v1 length");
  }

  // ── v2: detection class + confidence ride two trailing bytes ─────────
  {
    uint8_t p[FLEET_BEACON_PAYLOAD_V2_LEN];
    const uint8_t flags = FLEET_BEACON_FLAG_ALERT | FLEET_BEACON_FLAG_ON_WIFI_STA;
    size_t n = fleet_beacon_build_v2(p, flags, /*battery=*/-1, /*health=*/90,
                                     /*chain=*/0x0101, /*fp0=*/0xAA, /*fp1=*/0xBB,
                                     FLEET_BEACON_DETECT_PERSON, /*score=*/87);
    CHECK(n == 11,   "v2 builder returns 11 payload bytes");
    CHECK(p[0] == 0x10, "v2 keeps the 0x10 type byte");
    CHECK(p[1] == 0x02, "v2 schema version is 0x02");
    CHECK(p[9] == FLEET_BEACON_DETECT_PERSON, "v2 detect class byte");
    CHECK(p[10] == 87, "v2 detect score byte");

    uint8_t mfg[FLEET_BEACON_MFG_V2_LEN];
    mfg[0] = FLEET_BEACON_COMPANY_ID & 0xFF;
    mfg[1] = (FLEET_BEACON_COMPANY_ID >> 8) & 0xFF;
    for (size_t i = 0; i < FLEET_BEACON_PAYLOAD_V2_LEN; i++) mfg[2 + i] = p[i];

    FleetBeaconFields f{};
    CHECK(fleet_beacon_parse(mfg, sizeof(mfg), &f), "parse accepts a v2 blob");
    CHECK(f.flags == flags,           "v2 parsed flags match (alert set)");
    CHECK(f.battery_pct == -1,        "v2 parsed battery unknown");
    CHECK(f.health_pct == 90,         "v2 parsed health matches");
    CHECK(f.chain_lo16 == 0x0101,     "v2 parsed chain matches");
    CHECK(f.fp_b0 == 0xAA && f.fp_b1 == 0xBB, "v2 parsed fp bytes match");
    CHECK(f.detect_class == FLEET_BEACON_DETECT_PERSON, "v2 parsed detect class");
    CHECK(f.detect_score == 87,       "v2 parsed detect score");

    // Out-of-range score maps to the unknown sentinel -> -1.
    fleet_beacon_build_v2(p, 0, -1, -1, 0, 0, 0, FLEET_BEACON_DETECT_NONE, 150);
    CHECK(p[10] == FLEET_BEACON_SCORE_UNKNOWN, "score >100 -> 0xFF unknown");
    for (size_t i = 0; i < FLEET_BEACON_PAYLOAD_V2_LEN; i++) mfg[2 + i] = p[i];
    CHECK(fleet_beacon_parse(mfg, sizeof(mfg), &f), "parse accepts idle v2 blob");
    CHECK(f.detect_class == FLEET_BEACON_DETECT_NONE, "idle v2 class is NONE");
    CHECK(f.detect_score == -1,       "0xFF score decodes to -1 unknown");

    // A v1 blob decodes with the detect fields at their absent values.
    uint8_t p1[FLEET_BEACON_PAYLOAD_LEN];
    fleet_beacon_build(p1, 0, 50, 50, 0, 0x12, 0x34);
    uint8_t mfg1[FLEET_BEACON_MFG_LEN];
    mfg1[0] = 0xFF; mfg1[1] = 0xFF;
    for (size_t i = 0; i < FLEET_BEACON_PAYLOAD_LEN; i++) mfg1[2 + i] = p1[i];
    CHECK(fleet_beacon_parse(mfg1, sizeof(mfg1), &f), "v1 blob still parses");
    CHECK(f.detect_class == FLEET_BEACON_DETECT_NONE && f.detect_score == -1,
          "v1 blob yields NONE/-1 detect fields");

    // Length/version agreement holds in the other direction too.
    mfg[3] = FLEET_BEACON_VERSION;  // v1 version on a v2-length blob
    CHECK(!fleet_beacon_parse(mfg, sizeof(mfg), &f), "reject v1 version at v2 length");
  }

  printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
