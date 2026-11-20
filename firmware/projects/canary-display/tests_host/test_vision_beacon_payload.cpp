// Host test for the Canary Vision's fleet-beacon payload builder
// (firmware/projects/canary-vision/src/net/fleet_beacon_payload.cpp), compiled
// verbatim over stand-ins for the five things it reads: the witness identity
// and chain, the diagnostics level, the STA link, the Bluetooth setup door
// and the stored detection settings. It lives in the display's suite, as
// test_ota_mgr_cut.cpp does for the Vision's OTA glue, because this is the
// suite CI runs; the display's beacon_parse.h side of the same bytes is in
// test_beacon_parse.cpp.
//
// Audit A2-07: the beacon is cleartext on every band it rides (BLE advert,
// ESP-NOW, LAN multicast), and it carried the live detection (the ALERT flag,
// the class token and the confidence) by default, so anyone in radio range or
// on the Wi-Fi could log when the room held a person. The detection is an
// owner opt-in now (canary/detect_config.h beacon_detect, default off). This
// pins both halves: opted out, no frame carries a detection AND the
// generation never moves on a detection edge (an off-schedule send would mark
// the edge by its timing alone); opted in, the frame carries exactly what the
// presence FSM fed in.
//
// Run: make -C firmware/projects/canary-display/tests_host run-vision-beacon-payload

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "canary/detect_config.h"
#include "canary/diagnostics.h"
#include "canary/net/fleet_beacon_payload.h"

static int g_fail = 0;
#define CHECK(cond, msg)                                               \
  do {                                                                 \
    if (!(cond)) { std::printf("FAIL: %s\n", (msg)); ++g_fail; }       \
    else         { std::printf("ok:   %s\n", (msg)); }                 \
  } while (0)

// ── Stand-ins for what the builder reads ───────────────────────────────────
static canary::cfg::DetectConfig g_cfg{};   // beacon_detect defaults to false

namespace canary::cfg {
const DetectConfig& detect() { return g_cfg; }
}  // namespace canary::cfg

namespace canary::witness {
bool ready() { return true; }
uint32_t chain_length() { return 0x1234; }
}  // namespace canary::witness

namespace canary::diag {
const Snapshot& get() {
  static Snapshot s{};
  return s;
}
}  // namespace canary::diag

namespace canary::net {
bool wifi_connected() { return true; }
namespace improv_ble {
bool setup_open() { return false; }
}  // namespace improv_ble
}  // namespace canary::net

namespace device_signature {
const char* fingerprint_hex() { return "0011223344556677"; }
}  // namespace device_signature

using canary::net::fleet_beacon_note_detection;
using canary::net::fleet_beacon_payload_build;
using canary::net::fleet_beacon_payload_generation;

static FleetBeaconFields frame() {
  uint8_t mfg[FLEET_BEACON_MFG_V2_LEN];
  const size_t n = fleet_beacon_payload_build(mfg);
  FleetBeaconFields f{};
  if (!fleet_beacon_parse(mfg, n, &f)) {
    std::printf("FAIL: the builder's frame does not parse\n");
    ++g_fail;
  }
  return f;
}

static bool carries_detection(const FleetBeaconFields& f) {
  return (f.flags & FLEET_BEACON_FLAG_ALERT) != 0 ||
         f.detect_class != FLEET_BEACON_DETECT_NONE || f.detect_score != -1;
}

int main() {
  // ── Default: opted out ──────────────────────────────────────────────────
  CHECK(!canary::cfg::DetectConfig{}.beacon_detect,
        "the beacon detection opt-in defaults to off");
  {
    const uint32_t gen0 = fleet_beacon_payload_generation();
    fleet_beacon_note_detection(true, FLEET_BEACON_DETECT_PERSON, 87, 1000);
    const FleetBeaconFields f = frame();
    CHECK(!(f.flags & FLEET_BEACON_FLAG_ALERT),
          "opted out: a live person sets no ALERT flag");
    CHECK(f.detect_class == FLEET_BEACON_DETECT_NONE,
          "opted out: no class token on the wire");
    CHECK(f.detect_score == -1, "opted out: no confidence on the wire");
    CHECK(fleet_beacon_payload_generation() == gen0,
          "opted out: a detection edge does not move the generation");
    fleet_beacon_note_detection(true, FLEET_BEACON_DETECT_ANIMAL, 60, 2000);
    fleet_beacon_note_detection(false, FLEET_BEACON_DETECT_PERSON, -1, 3000);
    CHECK(fleet_beacon_payload_generation() == gen0,
          "opted out: class changes and presence ending move nothing either");
    // The rest of the beacon is unchanged by the opt-in.
    CHECK(f.fp_b0 == 0x66 && f.fp_b1 == 0x77, "the fingerprint suffix still rides");
    CHECK(f.chain_lo16 == 0x1234, "the chain height still rides");
    CHECK((f.flags & FLEET_BEACON_FLAG_ON_WIFI_STA) != 0, "the STA flag still rides");
  }

  // ── Opted in: the frame carries what the FSM fed ────────────────────────
  {
    g_cfg.beacon_detect = true;
    const uint32_t gen0 = fleet_beacon_payload_generation();
    fleet_beacon_note_detection(true, FLEET_BEACON_DETECT_PERSON, 87, 4000);
    const FleetBeaconFields f = frame();
    CHECK((f.flags & FLEET_BEACON_FLAG_ALERT) != 0, "opted in: ALERT is set");
    CHECK(f.detect_class == FLEET_BEACON_DETECT_PERSON, "opted in: the class rides");
    CHECK(f.detect_score == 87, "opted in: the confidence rides");
    CHECK(fleet_beacon_payload_generation() == gen0 + 1,
          "opted in: the presence edge bumps the generation once");

    // Turning the opt-in off clears the very next frame, before the next
    // vision tick has fed anything...
    g_cfg.beacon_detect = false;
    CHECK(!carries_detection(frame()),
          "opting out clears the next frame at once");
    // ...and the next tick records "nothing seen", an edge the carriers send
    // at once, so a display stops hearing the detection on every band.
    const uint32_t gen1 = fleet_beacon_payload_generation();
    fleet_beacon_note_detection(true, FLEET_BEACON_DETECT_PERSON, 90, 5000);
    CHECK(fleet_beacon_payload_generation() == gen1 + 1,
          "the opt-out lands as one clearing edge");
    CHECK(!carries_detection(frame()), "and the frame stays clear");
    fleet_beacon_note_detection(true, FLEET_BEACON_DETECT_PERSON, 91, 6000);
    CHECK(fleet_beacon_payload_generation() == gen1 + 1,
          "after which detections move nothing");
  }

  std::printf(g_fail == 0 ? "\nALL vision beacon payload checks PASSED\n"
                          : "\n%d FAILURE(S)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
