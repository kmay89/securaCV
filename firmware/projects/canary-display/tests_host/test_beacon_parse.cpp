// Host-side test for canary/net/beacon_parse.h — the fleet-link presence
// beacon wire parser. Pure header (only <stdint.h>/<stddef.h> + the
// dependency-free BeaconStatus), so the test includes it directly and asserts
// the exact 11-byte manufacturer-blob decode the WAP puts on air
// (canary-wap/fleet_beacon.h builds the identical layout).

#include "canary/net/beacon_parse.h"

#include <cstdio>
#include <cstring>

using canary::net::beacon_fp4_from_mfg;
using canary::net::beacon_parse_status;
using canary::fleet::BeaconStatus;

static int g_failures = 0;
#define CHECK(cond, msg)                                            \
  do {                                                             \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; }   \
    else         { printf("ok:   %s\n", (msg)); }                 \
  } while (0)

// A well-formed 11-byte blob: company FFFF, type 0x10, ver 0x01, flags,
// batt=64, health=77, chain=0x1234 (LE), fp bytes AB CD.
static void make_good(uint8_t out[11], uint8_t flags, uint8_t batt,
                      uint8_t health, uint8_t chain_lo, uint8_t chain_hi,
                      uint8_t fp0, uint8_t fp1) {
  out[0] = 0xFF; out[1] = 0xFF; out[2] = 0x10; out[3] = 0x01;
  out[4] = flags; out[5] = batt; out[6] = health;
  out[7] = chain_lo; out[8] = chain_hi; out[9] = fp0; out[10] = fp1;
}

int main() {
  // ── fp4 hex-encode from the fingerprint bytes ────────────────────────
  {
    uint8_t m[11];
    make_good(m, 0, 64, 77, 0x34, 0x12, 0xAB, 0xCD);
    char fp4[5] = {0};
    CHECK(beacon_fp4_from_mfg(m, 11, fp4), "fp4: accepts a well-formed blob");
    CHECK(strcmp(fp4, "abcd") == 0, "fp4: lowercase hex of fp bytes AB CD");
  }

  // ── status decode: flags, battery, health, LE chain ──────────────────
  {
    uint8_t m[11];
    const uint8_t flags = canary::net::BEACON_FLAG_TAMPER |
                          canary::net::BEACON_FLAG_MIC_MUTED |
                          canary::net::BEACON_FLAG_DEGRADED;
    make_good(m, flags, 64, 77, 0x34, 0x12, 0xAB, 0xCD);
    BeaconStatus s;
    CHECK(beacon_parse_status(m, 11, s), "status: accepts a well-formed blob");
    CHECK(s.tamper && s.mic_muted && s.degraded, "status: flag bits decode");
    CHECK(s.battery_present && s.battery_pct == 64, "status: battery 64");
    CHECK(s.health_present && s.health == 77, "status: health 77");
    CHECK(s.chain_present && s.chain_seq == 0x1234, "status: LE chain 0x1234");
  }

  // ── on_wifi_sta / alert flags don't set tamper/mic/degraded ──────────
  {
    uint8_t m[11];
    make_good(m, canary::net::BEACON_FLAG_ON_WIFI_STA | canary::net::BEACON_FLAG_ALERT,
              50, 50, 0, 0, 0, 0);
    BeaconStatus s;
    beacon_parse_status(m, 11, s);
    CHECK(!s.tamper && !s.mic_muted && !s.degraded,
          "status: on_wifi/alert flags leave tamper/mic/degraded clear");
  }

  // ── 0xFF battery/health decode to not-present ────────────────────────
  {
    uint8_t m[11];
    make_good(m, 0, 0xFF, 0xFF, 0x00, 0x00, 0x11, 0x22);
    BeaconStatus s;
    beacon_parse_status(m, 11, s);
    CHECK(!s.battery_present && s.battery_pct == -1, "status: 0xFF battery unknown");
    CHECK(!s.health_present && s.health == -1, "status: 0xFF health unknown");
    CHECK(s.chain_present && s.chain_seq == 0, "status: chain always present");
  }

  // ── malformed blobs are rejected ─────────────────────────────────────
  {
    uint8_t m[11];
    make_good(m, 0, 50, 50, 0, 0, 0x11, 0x22);
    char fp4[5];
    BeaconStatus s;

    CHECK(!beacon_fp4_from_mfg(m, 10, fp4), "reject short (10 bytes)");
    CHECK(!beacon_fp4_from_mfg(m, 17, fp4), "reject oversize/chirp-sized (17 bytes)");
    CHECK(!beacon_parse_status(m, 12, s), "reject oversize status (12 bytes)");

    uint8_t bad = m[0]; m[0] = 0x4C;
    CHECK(!beacon_fp4_from_mfg(m, 11, fp4), "reject wrong company id");
    m[0] = bad;

    m[2] = 0x01;  // a chirp type, not the beacon type
    CHECK(!beacon_fp4_from_mfg(m, 11, fp4), "reject wrong type byte");
    m[2] = 0x10;

    m[3] = 0x02;  // wrong schema version
    CHECK(!beacon_fp4_from_mfg(m, 11, fp4), "reject wrong schema version");
    CHECK(!beacon_parse_status(m, 11, s), "status: reject wrong schema version");
  }

  printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
