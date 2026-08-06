// Host-side test for fleet_beacon_udp.h — the LAN-multicast transport for the
// fleet-link presence beacon. Mirrors test_fleet_beacon.cpp in shape: pure
// header, no sockets, so the receive-side decision is exercised without a
// network stack.
//
// The property under test is the one that matters when three transports share
// one wire format: a datagram body carries the manufacturer blob VERBATIM, so
// bytes built for the BLE advert must parse identically when they arrive over
// UDP — and everything that is not one of ours must be rejected before it can
// reach the fleet model.

#include "fleet_beacon_udp.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond, msg)                                            \
  do {                                                             \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; }   \
    else         { printf("ok:   %s\n", (msg)); }                 \
  } while (0)

// Build a full on-air blob (company id + payload) the way both the BLE
// advertiser and the UDP sender do, so the test starts from real sender bytes.
static size_t build_blob_v2(uint8_t* out, uint8_t flags, int battery, int health,
                            uint32_t chain, uint8_t fp0, uint8_t fp1,
                            uint8_t cls, int score) {
  uint8_t payload[FLEET_BEACON_PAYLOAD_V2_LEN];
  fleet_beacon_build_v2(payload, flags, battery, health, chain, fp0, fp1, cls,
                        score);
  out[0] = (uint8_t)(FLEET_BEACON_COMPANY_ID & 0xFF);
  out[1] = (uint8_t)((FLEET_BEACON_COMPANY_ID >> 8) & 0xFF);
  memcpy(&out[2], payload, FLEET_BEACON_PAYLOAD_V2_LEN);
  return FLEET_BEACON_MFG_V2_LEN;
}

static size_t build_blob_v1(uint8_t* out, uint8_t flags, int battery, int health,
                            uint32_t chain, uint8_t fp0, uint8_t fp1) {
  uint8_t payload[FLEET_BEACON_PAYLOAD_LEN];
  fleet_beacon_build(payload, flags, battery, health, chain, fp0, fp1);
  out[0] = (uint8_t)(FLEET_BEACON_COMPANY_ID & 0xFF);
  out[1] = (uint8_t)((FLEET_BEACON_COMPANY_ID >> 8) & 0xFF);
  memcpy(&out[2], payload, FLEET_BEACON_PAYLOAD_LEN);
  return FLEET_BEACON_MFG_LEN;
}

int main() {
  printf("=== fleet_beacon_udp.h (LAN multicast transport) ===\n");

  // ── The transport constants are a contract, not a preference ────────────
  // Both firmwares compile against THIS header, so these values can only
  // change in one place — but they still get asserted, because a sender and a
  // receiver that disagree fail silently on the wire with no compile error.
  {
    CHECK(FLEET_BEACON_UDP_PORT == 5867, "port is 5867");
    CHECK(FLEET_BEACON_UDP_GROUP_B0 == 239, "group is in 239.0.0.0/8 (org-local scope)");
    CHECK(strcmp(FLEET_BEACON_UDP_GROUP, "239.83.67.86") == 0,
          "group string matches its octets");
    // TTL 1 is a privacy property: a presence beacon must not be routable off
    // the local subnet. If this ever reads > 1, that is a leak, not a tune.
    CHECK(FLEET_BEACON_UDP_TTL == 1, "TTL 1 confines the beacon to the subnet");
    CHECK(FLEET_BEACON_UDP_MAX_LEN == FLEET_BEACON_MFG_V2_LEN,
          "max read length is exactly a v2 blob");
  }

  // ── A v2 blob survives the trip byte-for-byte ───────────────────────────
  {
    uint8_t blob[FLEET_BEACON_MFG_V2_LEN];
    const size_t n = build_blob_v2(blob,
                                   FLEET_BEACON_FLAG_ALERT |
                                       FLEET_BEACON_FLAG_ON_WIFI_STA,
                                   /*battery=*/-1, /*health=*/-1,
                                   /*chain=*/0x1234, /*fp0=*/0xAB, /*fp1=*/0xCD,
                                   FLEET_BEACON_DETECT_PERSON, /*score=*/87);
    FleetBeaconFields f{};
    CHECK(fleet_beacon_udp_accept(blob, n, &f), "accepts a v2 datagram");
    CHECK((f.flags & FLEET_BEACON_FLAG_ALERT) != 0, "alert flag survives");
    CHECK((f.flags & FLEET_BEACON_FLAG_ON_WIFI_STA) != 0, "on-wifi flag survives");
    CHECK(f.detect_class == FLEET_BEACON_DETECT_PERSON, "class survives (person)");
    CHECK(f.detect_score == 87, "confidence survives (87%)");
    CHECK(f.chain_lo16 == 0x1234, "chain height survives");
    CHECK(f.fp_b0 == 0xAB && f.fp_b1 == 0xCD, "fingerprint suffix survives");
    CHECK(f.battery_pct == -1 && f.health_pct == -1, "unknown sentinels survive");
  }

  // ── A v1 sender stays understood over the new transport ─────────────────
  // canary-sense and the WAP emit v1. They must not have to change to be heard
  // on the LAN, and a v1 blob must never fabricate a detection.
  {
    uint8_t blob[FLEET_BEACON_MFG_LEN];
    const size_t n = build_blob_v1(blob, FLEET_BEACON_FLAG_TAMPER,
                                   /*battery=*/55, /*health=*/90,
                                   /*chain=*/7, /*fp0=*/0x01, /*fp1=*/0x02);
    FleetBeaconFields f{};
    CHECK(fleet_beacon_udp_accept(blob, n, &f), "accepts a v1 datagram");
    CHECK((f.flags & FLEET_BEACON_FLAG_TAMPER) != 0, "v1 tamper flag survives");
    CHECK(f.battery_pct == 55 && f.health_pct == 90, "v1 percentages survive");
    CHECK(f.detect_class == FLEET_BEACON_DETECT_NONE,
          "v1 reports no detection (never fabricated)");
    CHECK(f.detect_score == -1, "v1 confidence is unknown, not zero");
  }

  // ── Everything else on the group is rejected before the fleet model ─────
  {
    uint8_t good[FLEET_BEACON_MFG_V2_LEN];
    build_blob_v2(good, 0, -1, -1, 1, 0, 0, FLEET_BEACON_DETECT_NONE, -1);
    FleetBeaconFields f{};

    CHECK(!fleet_beacon_udp_accept(nullptr, FLEET_BEACON_MFG_V2_LEN, &f),
          "rejects a null body");
    CHECK(!fleet_beacon_udp_accept(good, 0, &f), "rejects an empty datagram");

    // Oversize: the length guard must fire, and it must fire on the LENGTH
    // rather than on whatever happens to sit in the first 13 bytes.
    uint8_t big[64];
    memcpy(big, good, sizeof(good));
    memset(big + sizeof(good), 0, sizeof(big) - sizeof(good));
    CHECK(!fleet_beacon_udp_accept(big, sizeof(big), &f),
          "rejects an oversize datagram even with a valid prefix");

    // Truncated mid-blob — a short read must not parse as v1.
    CHECK(!fleet_beacon_udp_accept(good, FLEET_BEACON_MFG_V2_LEN - 1, &f),
          "rejects a truncated v2 datagram");

    // Foreign traffic that guessed the port.
    uint8_t foreign[FLEET_BEACON_MFG_V2_LEN];
    memcpy(foreign, good, sizeof(foreign));
    foreign[0] = 0x4C; foreign[1] = 0x00;            // someone else's company id
    CHECK(!fleet_beacon_udp_accept(foreign, sizeof(foreign), &f),
          "rejects a foreign company id");

    uint8_t wrong_type[FLEET_BEACON_MFG_V2_LEN];
    memcpy(wrong_type, good, sizeof(wrong_type));
    wrong_type[2] = 0x03;                            // a chirp type, not a beacon
    CHECK(!fleet_beacon_udp_accept(wrong_type, sizeof(wrong_type), &f),
          "rejects a non-beacon type byte");

    // Length and version must AGREE: a 13-byte body claiming v1 (or an 11-byte
    // body claiming v2) is malformed, not leniently parsed.
    uint8_t v2_len_v1_ver[FLEET_BEACON_MFG_V2_LEN];
    memcpy(v2_len_v1_ver, good, sizeof(v2_len_v1_ver));
    v2_len_v1_ver[3] = FLEET_BEACON_VERSION;
    CHECK(!fleet_beacon_udp_accept(v2_len_v1_ver, sizeof(v2_len_v1_ver), &f),
          "rejects a v2-length body claiming version 1");

    uint8_t v1_len_v2_ver[FLEET_BEACON_MFG_LEN];
    build_blob_v1(v1_len_v2_ver, 0, -1, -1, 1, 0, 0);
    v1_len_v2_ver[3] = FLEET_BEACON_VERSION_2;
    CHECK(!fleet_beacon_udp_accept(v1_len_v2_ver, sizeof(v1_len_v2_ver), &f),
          "rejects a v1-length body claiming version 2");
  }

  // ── One blob, three transports ──────────────────────────────────────────
  // The point of carrying the manufacturer blob verbatim: bytes built once are
  // understood identically whether they arrive by BLE, ESP-NOW, or UDP. If
  // this ever diverges, a display would show a different sighting depending on
  // which radio heard it.
  {
    uint8_t blob[FLEET_BEACON_MFG_V2_LEN];
    const size_t n = build_blob_v2(blob, FLEET_BEACON_FLAG_ALERT, -1, -1, 42,
                                   0xDE, 0xAD, FLEET_BEACON_DETECT_VEHICLE, 61);
    FleetBeaconFields via_udp{}, via_radio{};
    const bool a = fleet_beacon_udp_accept(blob, n, &via_udp);
    const bool b = fleet_beacon_parse(blob, n, &via_radio);   // BLE / ESP-NOW path
    CHECK(a && b, "the same blob parses on both paths");
    CHECK(memcmp(&via_udp, &via_radio, sizeof(FleetBeaconFields)) == 0,
          "UDP and radio paths yield byte-identical fields");
  }

  if (g_failures) {
    printf("\n%d CHECK(s) FAILED\n", g_failures);
    return 1;
  }
  printf("\nAll fleet_beacon_udp checks passed.\n");
  return 0;
}
