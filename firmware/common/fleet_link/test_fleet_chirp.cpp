// Host-side test for the CANONICAL fleet_chirp.h (firmware/common/fleet_link)
// — the 17-byte BLE Chirp advert the canary-wap sends and every receiver in
// the fleet reads. Pure header, no NimBLE/Arduino glue: the test includes it
// directly and asserts the exact 15-byte payload put on air (bytes [2..16] of
// the manufacturer blob; the sender puts the company id in front), the
// full-blob parser's round trip, and every refusal.
//
// The golden bytes below are the canary-wap's buildChirpPayload() output as
// it was before fleet_chirp.h existed (ble_chirp.h at a2f5bb97), so a change
// to the layout that every receiver reads fails here first. The canary-wap
// copy is kept byte-identical by firmware/scripts/check_fleet_beacon_sync.sh;
// the canary-display sketch's copy by check_display_arduino_sync.sh.
//
// Build (firmware.yml fleet-link-host-tests), from the repository root:
//   g++ -std=c++17 -Wall -Wextra -Werror -I firmware/common/fleet_link
//       firmware/common/fleet_link/test_fleet_chirp.cpp -o /tmp/fleet_chirp

#include "fleet_chirp.h"
#include "fleet_beacon.h"   // the other advert on the same company id

#include <cstdio>
#include <cstdint>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond, msg)                                            \
  do {                                                             \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; }   \
    else         { printf("ok:   %s\n", (msg)); }                 \
  } while (0)

// Company id + builder output: the blob exactly as a scanner hands it over.
static size_t chirp_mfg(uint8_t out[FLEET_CHIRP_MFG_LEN], uint8_t type,
                        uint32_t hour, const uint8_t* chain8,
                        uint8_t fp0, uint8_t fp1) {
  out[0] = (uint8_t)(FLEET_CHIRP_COMPANY_ID & 0xFF);
  out[1] = (uint8_t)((FLEET_CHIRP_COMPANY_ID >> 8) & 0xFF);
  return 2 + fleet_chirp_build(out + 2, type, hour, chain8, fp0, fp1);
}

int main() {
  // ── The layout: lengths and offsets add up, fp2 at [15-16] ──────────
  {
    CHECK(FLEET_CHIRP_MFG_LEN == 17, "full blob is 17 bytes");
    CHECK(FLEET_CHIRP_PAYLOAD_LEN == 15, "payload after the company id is 15 bytes");
    CHECK(FLEET_CHIRP_MFG_LEN == FLEET_CHIRP_PAYLOAD_LEN + 2, "company id is the 2 bytes in front");
    CHECK(FLEET_CHIRP_COMPANY_ID == 0xFFFF, "company id 0xFFFF");
    CHECK(FLEET_CHIRP_OFF_TYPE == 2, "type at [2]");
    CHECK(FLEET_CHIRP_OFF_HOUR == FLEET_CHIRP_OFF_TYPE + 1 && FLEET_CHIRP_OFF_HOUR == 3,
          "hour bucket at [3-6]");
    CHECK(FLEET_CHIRP_OFF_CHAIN == FLEET_CHIRP_OFF_HOUR + 4 && FLEET_CHIRP_OFF_CHAIN == 7,
          "chain prefix at [7-14]");
    CHECK(FLEET_CHIRP_CHAIN_LEN == 8, "chain prefix is 8 bytes");
    CHECK(FLEET_CHIRP_OFF_FP == FLEET_CHIRP_OFF_CHAIN + FLEET_CHIRP_CHAIN_LEN &&
          FLEET_CHIRP_OFF_FP == 15, "fp2 at [15-16]");
    CHECK(FLEET_CHIRP_OFF_FP + 2 == FLEET_CHIRP_MFG_LEN, "fp2 is the last 2 bytes");
    CHECK(FLEET_CHIRP_TYPE_FIRST == 0x01 && FLEET_CHIRP_TYPE_LAST == 0x05,
          "types 0x01..0x05");
    CHECK(FLEET_CHIRP_TYPE_ALERT == 0x01 && FLEET_CHIRP_TYPE_HEARTBEAT == 0x02 &&
          FLEET_CHIRP_TYPE_TAMPER == 0x03 && FLEET_CHIRP_TYPE_WITNESS == 0x04 &&
          FLEET_CHIRP_TYPE_BOOT == 0x05, "the five named types, contiguous");
  }

  // ── Told apart from the presence beacon on the same company id ──────
  {
    CHECK(FLEET_CHIRP_COMPANY_ID == FLEET_BEACON_COMPANY_ID, "beacon and Chirp share 0xFFFF");
    CHECK(FLEET_CHIRP_MFG_LEN != FLEET_BEACON_MFG_LEN &&
          FLEET_CHIRP_MFG_LEN != FLEET_BEACON_MFG_V2_LEN, "lengths differ (17 vs 11/13)");
    CHECK(FLEET_BEACON_TYPE < FLEET_CHIRP_TYPE_FIRST || FLEET_BEACON_TYPE > FLEET_CHIRP_TYPE_LAST,
          "beacon type 0x10 is outside the Chirp range");
    CHECK(FLEET_CHIRP_OFF_FP == 15, "the Chirp's fp2 is the [15-16] the beacon header names");

    uint8_t b[FLEET_BEACON_MFG_V2_LEN] = {0xFF, 0xFF};
    fleet_beacon_build(b + 2, 0, 50, 50, 7, 0xAB, 0xCD);
    FleetChirpFields c;
    CHECK(!fleet_chirp_parse(b, FLEET_BEACON_MFG_LEN, &c), "a v1 beacon is not a Chirp");
    fleet_beacon_build_v2(b + 2, 0, 50, 50, 7, 0xAB, 0xCD, FLEET_BEACON_DETECT_PERSON, 90);
    CHECK(!fleet_chirp_parse(b, FLEET_BEACON_MFG_V2_LEN, &c), "a v2 beacon is not a Chirp");

    uint8_t m[FLEET_CHIRP_MFG_LEN];
    for (int t = FLEET_CHIRP_TYPE_FIRST; t <= FLEET_CHIRP_TYPE_LAST; t++) {
      chirp_mfg(m, (uint8_t)t, 1, nullptr, 0xAB, 0xCD);
      FleetBeaconFields f;
      CHECK(!fleet_beacon_parse(m, sizeof(m), &f), "a Chirp is not a beacon");
    }
  }

  // ── Golden bytes: what the canary-wap has always put on air ─────────
  {
    const uint8_t chain[FLEET_CHIRP_CHAIN_LEN] = {0x11, 0x22, 0x33, 0x44,
                                                  0x55, 0x66, 0x77, 0x88};
    uint8_t p[FLEET_CHIRP_PAYLOAD_LEN + 2];
    memset(p, 0x5A, sizeof(p));
    const size_t n = fleet_chirp_build(p, FLEET_CHIRP_TYPE_HEARTBEAT, 0x01020304u,
                                       chain, 0xAB, 0xCD);
    const uint8_t want[FLEET_CHIRP_PAYLOAD_LEN] = {
        0x02,                                            // type
        0x01, 0x02, 0x03, 0x04,                          // hour bucket, MSB first
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,  // chain prefix
        0xAB, 0xCD};                                     // fp2
    CHECK(n == FLEET_CHIRP_PAYLOAD_LEN, "builder returns 15 payload bytes");
    CHECK(memcmp(p, want, sizeof(want)) == 0, "payload bytes are the golden vector");
    CHECK(p[15] == 0x5A && p[16] == 0x5A, "builder writes no byte past the payload");
  }

  // ── The hour bucket is big-endian, all 32 bits ──────────────────────
  {
    uint8_t p[FLEET_CHIRP_PAYLOAD_LEN];
    fleet_chirp_build(p, FLEET_CHIRP_TYPE_ALERT, 0xDEADBEEFu, nullptr, 0, 0);
    CHECK(p[1] == 0xDE && p[2] == 0xAD && p[3] == 0xBE && p[4] == 0xEF,
          "0xDEADBEEF goes on air DE AD BE EF");
    fleet_chirp_build(p, FLEET_CHIRP_TYPE_ALERT, 1u, nullptr, 0, 0);
    CHECK(p[1] == 0 && p[2] == 0 && p[3] == 0 && p[4] == 1, "hour 1 is 00 00 00 01");
    fleet_chirp_build(p, FLEET_CHIRP_TYPE_ALERT, 0xFFFFFFFFu, nullptr, 0, 0);
    CHECK(p[1] == 0xFF && p[2] == 0xFF && p[3] == 0xFF && p[4] == 0xFF,
          "the largest bucket survives whole");
  }

  // ── No chain head yet: the chain bytes are zero, every one written ──
  {
    uint8_t p[FLEET_CHIRP_PAYLOAD_LEN];
    memset(p, 0x5A, sizeof(p));
    fleet_chirp_build(p, FLEET_CHIRP_TYPE_BOOT, 0, nullptr, 0x7E, 0x01);
    bool zero = true;
    for (size_t i = 5; i < 13; i++) zero = zero && p[i] == 0;
    CHECK(zero, "a null chain writes 8 zero bytes");
    bool written = true;
    for (size_t i = 0; i < sizeof(p); i++) written = written && p[i] != 0x5A;
    CHECK(written, "the builder writes every payload byte");
  }

  // ── The type is written as given; the receiver is the one that refuses
  {
    uint8_t m[FLEET_CHIRP_MFG_LEN];
    chirp_mfg(m, 0x06, 0, nullptr, 0, 0);
    CHECK(m[FLEET_CHIRP_OFF_TYPE] == 0x06, "builder writes an out-of-range type unchanged");
    FleetChirpFields c;
    CHECK(!fleet_chirp_parse(m, sizeof(m), &c), "and the parser refuses it");
  }

  // ── Round trip over every type and the field edges ──────────────────
  {
    const uint32_t hours[] = {0u, 1u, 0x7Fu, 0x80u, 0xFFu, 0x0100u, 0x01020304u,
                              0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu};
    const uint8_t chains[][FLEET_CHIRP_CHAIN_LEN] = {
        {0, 0, 0, 0, 0, 0, 0, 0},
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08},
        {0x80, 0x00, 0x7F, 0xFF, 0x00, 0x01, 0xFE, 0x5A}};
    const uint8_t fps[][2] = {{0x00, 0x00}, {0xFF, 0xFF}, {0xAB, 0xCD}, {0x00, 0xFF}, {0xFF, 0x00}};
    int trips = 0, good = 0;
    for (int t = FLEET_CHIRP_TYPE_FIRST; t <= FLEET_CHIRP_TYPE_LAST; t++) {
      for (uint32_t h : hours) {
        for (const auto& ch : chains) {
          for (const auto& fp : fps) {
            uint8_t m[FLEET_CHIRP_MFG_LEN];
            const size_t n = chirp_mfg(m, (uint8_t)t, h, ch, fp[0], fp[1]);
            FleetChirpFields c;
            memset(&c, 0xA5, sizeof(c));
            ++trips;
            if (n == FLEET_CHIRP_MFG_LEN && fleet_chirp_matches(m, n) &&
                fleet_chirp_parse(m, n, &c) && c.type == t && c.hour_bucket == h &&
                memcmp(c.chain8, ch, FLEET_CHIRP_CHAIN_LEN) == 0 &&
                c.fp_b0 == fp[0] && c.fp_b1 == fp[1] &&
                m[FLEET_CHIRP_OFF_FP] == fp[0] && m[FLEET_CHIRP_OFF_FP + 1] == fp[1]) {
              ++good;
            }
          }
        }
      }
    }
    CHECK(trips == 5 * 10 * 4 * 5, "round trip covers 5 types x 10 hours x 4 chains x 5 fp2");
    CHECK(good == trips, "every built Chirp parses back to the fields it was built from");
  }

  // ── Refusals: length, type, company, null — and *out stays untouched
  {
    uint8_t big[24];
    memset(big, 0, sizeof(big));
    chirp_mfg(big, FLEET_CHIRP_TYPE_TAMPER, 9, nullptr, 0x12, 0x34);
    FleetChirpFields c;
    memset(&c, 0xA5, sizeof(c));
    FleetChirpFields untouched;
    memset(&untouched, 0xA5, sizeof(untouched));

    CHECK(fleet_chirp_parse(big, 17, &c), "17 bytes parse");
    memset(&c, 0xA5, sizeof(c));
    CHECK(!fleet_chirp_parse(big, 16, &c), "16 bytes refused");
    CHECK(!fleet_chirp_parse(big, 18, &c), "18 bytes refused");
    CHECK(!fleet_chirp_parse(big, 0, &c), "0 bytes refused");
    CHECK(!fleet_chirp_parse(big, FLEET_BEACON_MFG_LEN, &c), "11 bytes refused");
    CHECK(!fleet_chirp_parse(big, FLEET_BEACON_MFG_V2_LEN, &c), "13 bytes refused");
    const uint8_t bad_types[] = {0x00, 0x06, 0x10, 0x7F, 0x80, 0xFF};
    for (uint8_t t : bad_types) {
      big[FLEET_CHIRP_OFF_TYPE] = t;
      CHECK(!fleet_chirp_parse(big, 17, &c), "type outside 0x01..0x05 refused");
    }
    big[FLEET_CHIRP_OFF_TYPE] = FLEET_CHIRP_TYPE_TAMPER;
    const uint8_t companies[][2] = {{0x4C, 0x00}, {0xFE, 0xFF}, {0xFF, 0xFE}, {0x00, 0x00}};
    for (const auto& co : companies) {
      big[0] = co[0];
      big[1] = co[1];
      CHECK(!fleet_chirp_parse(big, 17, &c), "a company other than 0xFFFF refused");
    }
    big[0] = 0xFF;
    big[1] = 0xFF;
    CHECK(!fleet_chirp_parse(nullptr, 17, &c), "a null blob refused");
    CHECK(!fleet_chirp_matches(nullptr, 17), "a null blob does not match");
    CHECK(memcmp(&c, &untouched, sizeof(c)) == 0, "a refused parse leaves *out untouched");
    CHECK(!fleet_chirp_parse(big, 17, nullptr), "a null out refused");
    CHECK(fleet_chirp_parse(big, 17, &c), "and the same blob still parses");
  }

  // ── Exhaustively: exactly five shapes at 17 bytes on 0xFFFF, none else
  {
    const uint8_t companies[][2] = {{0xFF, 0xFF}, {0x4C, 0x00}, {0xFE, 0xFF},
                                    {0xFF, 0xFE}, {0x00, 0x00}, {0x59, 0x00}};
    uint8_t blob[40];
    bool exact = true, agree = true;
    for (const auto& co : companies) {
      const bool fleet = co[0] == 0xFF && co[1] == 0xFF;
      for (size_t len = 0; len <= sizeof(blob); len++) {
        int accepted = 0;
        for (int type = 0; type <= 0xFF; type++) {
          memset(blob, 0x5A, sizeof(blob));
          blob[0] = co[0];
          blob[1] = co[1];
          blob[2] = (uint8_t)type;
          FleetChirpFields c;
          const bool parsed = fleet_chirp_parse(blob, len, &c);
          if (parsed != fleet_chirp_matches(blob, len)) agree = false;
          if (parsed) accepted++;
        }
        const int want = (fleet && len == FLEET_CHIRP_MFG_LEN) ? 5 : 0;
        if (accepted != want) {
          printf("  company %02X%02X len %zu: %d accepted, want %d\n", co[1], co[0], len,
                 accepted, want);
          exact = false;
        }
      }
    }
    CHECK(exact, "only the five Chirp types at 17 bytes on 0xFFFF are accepted");
    CHECK(agree, "fleet_chirp_matches() and fleet_chirp_parse() agree on every blob");
  }

  printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
