// Host test for the canary-wap's BLE Chirp sender (ble_chirp.h), sweep F460.
//
// ble_chirp.h builds the 17-byte Chirp advert with fleet_chirp_build()
// (fleet_chirp.h, #1961) and sendChirp() puts the company id in front. The
// only off-device guard on that call's argument order, the fingerprint's
// byte order with it, was the iPhone app's ChirpAdvertTests, a text pin of
// the call compiled only on macOS (ios-selfheal.yml). This suite compiles
// the REAL ble_chirp.h (FEATURE_BLE and FEATURE_BLE_CHIRP on) over stubs/bt,
// whose advertiser keeps the data it was last given, and holds the blob
// sendChirp() puts on the air to the golden vector the receivers are held
// to (firmware/common/fleet_link/test_fleet_chirp.cpp: type 0x02, hour
// bucket 01 02 03 04 MSB first, chain prefix 11..88, fingerprint suffix
// AB CD) and to fleet_chirp_parse(), the one reader every receiver uses.
//
// The hour bucket is millis()/3600000, hours since boot, and a 32-bit
// millis() reaches at most hour 1193, so the golden's 0x01020304 is out of
// reach: those four bytes are each case's own, and the other eleven of a
// heartbeat are the golden's byte for byte. A null chain head (before the
// chain has one) puts eight zero bytes on air.
//
// ble_chirp.h's one call into Opera (ble_opera::refreshBeacon(), the
// restore after a chirp) is stood in below. ble_chirp.h includes
// "ble_opera.h" by a quoted name, which finds the sketch's own header
// beside it before any -I directory, so the stand-in takes that header's
// include guard first and the real one compiles to nothing here.

#ifndef FEATURE_BLE
#define FEATURE_BLE 1
#endif

#define BLE_OPERA_H   // the real ble_opera.h is skipped: the stand-in is its one function
namespace ble_opera {
inline unsigned g_refreshes = 0;
inline bool g_refresh_answer = true;
static inline bool refreshBeacon() {
  ++g_refreshes;
  return g_refresh_answer;
}
}  // namespace ble_opera

#include "ble_chirp.h"
#include "fleet_chirp.h"

#if !(FEATURE_BLE && FEATURE_BLE_CHIRP)
#error "the Chirp sender must compile with FEATURE_BLE and FEATURE_BLE_CHIRP on: the stub branch sends nothing"
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond, msg)                                               \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __func__, __LINE__, (msg));      \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

namespace {

constexpr uint32_t kHourMs = 3600000u;

// test_fleet_chirp.cpp's golden payload: the 15 bytes after the company id.
constexpr uint8_t kGolden[FLEET_CHIRP_PAYLOAD_LEN] = {
    0x02,                                            // type (heartbeat)
    0x01, 0x02, 0x03, 0x04,                          // hour bucket, MSB first
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,  // chain prefix
    0xAB, 0xCD};                                     // fingerprint suffix

// The device id the sketch hands ble_manager (the pubkey fingerprint as
// hex): its last four characters are the suffix the Chirp carries.
const char kDeviceIdHex[] = "a3f7b2c1d4e5abcd";

// A whole chain head, the golden prefix first; the rest must never go on air.
uint8_t g_chain_head[32];

void fill_chain_head() {
  memcpy(g_chain_head, kGolden + 5, FLEET_CHIRP_CHAIN_LEN);
  memset(g_chain_head + FLEET_CHIRP_CHAIN_LEN, 0xEE, sizeof(g_chain_head) - FLEET_CHIRP_CHAIN_LEN);
}

// The manufacturer blob the advertiser was last given.
std::string on_air() { return host_sim::advertising.advertisement_data().mfg; }

const uint8_t* bytes(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

// One chirp at `hour` hours (plus `extra_ms` into it): sent, put on the
// air, then the 2 s broadcast ends and update() hands the air back to
// Opera's beacon. Returns the blob.
std::string chirp_at(ChirpType type, uint32_t hour, uint32_t extra_ms) {
  host_sim::now_ms = hour * kHourMs + extra_ms;
  const unsigned sets = host_sim::advertising.advertisement_data_sets();
  CHECK(ble_chirp::sendChirp(type), "the chirp starts");
  CHECK(host_sim::advertising.advertisement_data_sets() == sets + 1, "its advert goes on the air once");
  CHECK(host_sim::advertising.isAdvertising() && ble_chirp::isBroadcasting(), "broadcasting");
  const std::string blob = on_air();
  const unsigned refreshes = ble_opera::g_refreshes;
  host_sim::now_ms = host_sim::now_ms.load() + CHIRP_BROADCAST_DURATION_MS;
  ble_chirp::update();
  CHECK(!ble_chirp::isBroadcasting() && ble_opera::g_refreshes == refreshes + 1,
        "after 2 s the beacon is restored through Opera");
  CHECK(host_sim::advertising.isAdvertising(), "and the advertiser is running again");
  return blob;
}

void hour_is(const std::string& blob, uint32_t hour, const char* msg) {
  const uint8_t* b = bytes(blob);
  CHECK(b[3] == (uint8_t)(hour >> 24) && b[4] == (uint8_t)(hour >> 16) &&
            b[5] == (uint8_t)(hour >> 8) && b[6] == (uint8_t)hour,
        msg);
}

// ── The sender's blob is the golden layout, byte for byte ───────────────

void a_heartbeat_is_the_golden_vector_but_its_own_hour() {
  fill_chain_head();
  CHECK(ble_chirp::init(kDeviceIdHex, g_chain_head), "init");
  // Hour 0x0102 (258 h: the golden's top two bytes as this bucket's low
  // two, both nonzero; millis() never reaches the golden's 0x01020304),
  // 1234 ms into it.
  const uint32_t hour = 0x0102;
  const std::string blob = chirp_at(CHIRP_HEARTBEAT, hour, 1234);
  CHECK(blob.size() == FLEET_CHIRP_MFG_LEN, "17 bytes: company id + 15");
  if (blob.size() != FLEET_CHIRP_MFG_LEN) return;
  const uint8_t* b = bytes(blob);
  CHECK(b[0] == 0xFF && b[1] == 0xFF, "company id 0xFFFF in front");
  CHECK(b[FLEET_CHIRP_OFF_TYPE] == kGolden[0], "the golden's type byte (heartbeat)");
  hour_is(blob, hour, "the hour bucket, MSB first: 00 00 01 02");
  CHECK(memcmp(b + FLEET_CHIRP_OFF_CHAIN, kGolden + 5, FLEET_CHIRP_CHAIN_LEN) == 0,
        "the golden's chain prefix 11..88, and none of the head past it");
  CHECK(b[FLEET_CHIRP_OFF_FP] == kGolden[13] && b[FLEET_CHIRP_OFF_FP + 1] == kGolden[14],
        "the golden's fingerprint suffix AB CD, in that order");
  // The same bytes the receivers' builder writes for these fields.
  uint8_t want[FLEET_CHIRP_MFG_LEN] = {0xFF, 0xFF};
  fleet_chirp_build(want + 2, FLEET_CHIRP_TYPE_HEARTBEAT, hour, kGolden + 5, 0xAB, 0xCD);
  CHECK(memcmp(b, want, sizeof(want)) == 0, "all 17 bytes are fleet_chirp_build()'s");
  // And the one reader every receiver uses takes it back field by field.
  FleetChirpFields f;
  CHECK(fleet_chirp_parse(b, blob.size(), &f), "fleet_chirp_parse() takes it");
  CHECK(f.type == FLEET_CHIRP_TYPE_HEARTBEAT && f.hour_bucket == hour, "type and hour read back");
  CHECK(memcmp(f.chain8, kGolden + 5, FLEET_CHIRP_CHAIN_LEN) == 0, "chain prefix reads back");
  CHECK(f.fp_b0 == 0xAB && f.fp_b1 == 0xCD, "fingerprint suffix reads back AB CD");
  CHECK(host_sim::advertising.advertisement_data().name == "SCV-abcd", "named SCV-<the same four>");
}

// Every type the sketch sends, each at its own hour: the type byte is
// fleet_chirp.h's, and the rest is the same layout.
void every_type_goes_on_air_in_the_same_layout() {
  struct Case { ChirpType type; uint8_t wire; uint32_t hour; uint32_t extra_ms; };
  const Case cases[] = {
      {CHIRP_ALERT, FLEET_CHIRP_TYPE_ALERT, 0x0103, 0},
      {CHIRP_TAMPER, FLEET_CHIRP_TYPE_TAMPER, 0x0304, kHourMs - 1},   // the last ms of the hour
      {CHIRP_WITNESS, FLEET_CHIRP_TYPE_WITNESS, 1000, 77},
      {CHIRP_BOOT, FLEET_CHIRP_TYPE_BOOT, 1193, 0},                   // the last hour millis() reaches
  };
  for (const Case& c : cases) {
    const std::string blob = chirp_at(c.type, c.hour, c.extra_ms);
    CHECK(blob.size() == FLEET_CHIRP_MFG_LEN, "17 bytes");
    if (blob.size() != FLEET_CHIRP_MFG_LEN) continue;
    const uint8_t* b = bytes(blob);
    CHECK(b[FLEET_CHIRP_OFF_TYPE] == c.wire, "the type byte is fleet_chirp.h's");
    hour_is(blob, c.hour, "the hour bucket, MSB first");
    uint8_t want[FLEET_CHIRP_MFG_LEN] = {0xFF, 0xFF};
    fleet_chirp_build(want + 2, c.wire, c.hour, kGolden + 5, 0xAB, 0xCD);
    CHECK(memcmp(b, want, sizeof(want)) == 0, "all 17 bytes are fleet_chirp_build()'s");
    FleetChirpFields f;
    CHECK(fleet_chirp_parse(b, blob.size(), &f) && f.type == c.wire && f.hour_bucket == c.hour &&
              f.fp_b0 == 0xAB && f.fp_b1 == 0xCD &&
              memcmp(f.chain8, kGolden + 5, FLEET_CHIRP_CHAIN_LEN) == 0,
          "and parses back whole");
  }
}

// Before the chain has a head (a null pointer) the chain bytes are zero;
// the rest of the blob is unchanged.
void a_null_chain_head_goes_on_air_as_zeros() {
  CHECK(ble_chirp::init(kDeviceIdHex, nullptr), "init with no chain head");
  // The rate limit runs from the last chirp (hour 1193, above): twenty
  // seconds on, inside the same hour.
  const uint32_t at = 1193u * kHourMs + 20000u;
  host_sim::now_ms = at;
  CHECK(ble_chirp::sendChirp(CHIRP_HEARTBEAT), "the chirp starts");
  const std::string blob = on_air();
  CHECK(blob.size() == FLEET_CHIRP_MFG_LEN, "17 bytes");
  if (blob.size() != FLEET_CHIRP_MFG_LEN) return;
  const uint8_t* b = bytes(blob);
  bool zero = true;
  for (size_t i = 0; i < FLEET_CHIRP_CHAIN_LEN; i++) zero = zero && b[FLEET_CHIRP_OFF_CHAIN + i] == 0;
  CHECK(zero, "eight zero chain bytes");
  uint8_t want[FLEET_CHIRP_MFG_LEN] = {0xFF, 0xFF};
  fleet_chirp_build(want + 2, FLEET_CHIRP_TYPE_HEARTBEAT, 1193, nullptr, 0xAB, 0xCD);
  CHECK(memcmp(b, want, sizeof(want)) == 0, "all 17 bytes are fleet_chirp_build()'s with no chain");
  FleetChirpFields f;
  CHECK(fleet_chirp_parse(b, blob.size(), &f) && f.fp_b0 == 0xAB && f.fp_b1 == 0xCD, "parses back");
  host_sim::now_ms = at + CHIRP_BROADCAST_DURATION_MS;
  ble_chirp::update();
}

// A suffix whose two bytes differ the other way round: the first two hex
// characters are the byte at [15], the last two the byte at [16].
void the_suffix_bytes_keep_their_order() {
  fill_chain_head();
  CHECK(ble_chirp::init("00000000000012fe", g_chain_head), "init");
  host_sim::now_ms = 1193u * kHourMs + 60000u;
  CHECK(ble_chirp::sendChirp(CHIRP_ALERT), "the chirp starts");
  const std::string blob = on_air();
  CHECK(blob.size() == FLEET_CHIRP_MFG_LEN, "17 bytes");
  if (blob.size() != FLEET_CHIRP_MFG_LEN) return;
  const uint8_t* b = bytes(blob);
  CHECK(b[FLEET_CHIRP_OFF_FP] == 0x12 && b[FLEET_CHIRP_OFF_FP + 1] == 0xFE, "12 FE, as the hex reads");
  host_sim::now_ms = host_sim::now_ms.load() + CHIRP_BROADCAST_DURATION_MS;
  ble_chirp::update();
}

// The sender's own bounds around the blob: one chirp per ten seconds (a
// refused one puts nothing on the air), the count and the last type, and
// the five-minute heartbeat, whose blob is the same layout.
void the_rate_limit_and_the_heartbeat_send_the_same_blob() {
  fill_chain_head();
  const uint32_t t_init = 2u * kHourMs;
  host_sim::now_ms = t_init;
  CHECK(ble_chirp::init(kDeviceIdHex, g_chain_head), "init");
  const uint32_t t0 = t_init + 1000u;
  host_sim::now_ms = t0;
  const uint32_t sent = ble_chirp::getChirpsSent();
  CHECK(ble_chirp::sendChirp(CHIRP_WITNESS), "a chirp");
  CHECK(ble_chirp::getChirpsSent() == sent + 1 && ble_chirp::getLastChirpType() == CHIRP_WITNESS,
        "counted, and its type kept");
  host_sim::now_ms = t0 + CHIRP_BROADCAST_DURATION_MS;
  ble_chirp::update();
  CHECK(ble_chirp::isRateLimited(), "inside ten seconds: rate limited");
  const unsigned sets = host_sim::advertising.advertisement_data_sets();
  host_sim::now_ms = t0 + CHIRP_MIN_INTERVAL_MS - 1;
  CHECK(!ble_chirp::sendChirp(CHIRP_ALERT), "a second chirp inside ten seconds: refused");
  CHECK(host_sim::advertising.advertisement_data_sets() == sets && ble_chirp::getChirpsSent() == sent + 1,
        "and nothing went on the air");
  host_sim::now_ms = t0 + CHIRP_MIN_INTERVAL_MS;
  CHECK(!ble_chirp::isRateLimited(), "at ten seconds: free");
  // The heartbeat is due five minutes after init()'s stamp, not before.
  host_sim::now_ms = t_init + CHIRP_HEARTBEAT_INTERVAL_MS - 1;
  ble_chirp::heartbeatCheck();
  CHECK(ble_chirp::getChirpsSent() == sent + 1, "a millisecond early: no heartbeat");
  host_sim::now_ms = t_init + CHIRP_HEARTBEAT_INTERVAL_MS;
  ble_chirp::heartbeatCheck();
  CHECK(ble_chirp::getChirpsSent() == sent + 2 && ble_chirp::getLastChirpType() == CHIRP_HEARTBEAT,
        "five minutes on: the heartbeat chirped");
  const std::string blob = on_air();
  CHECK(blob.size() == FLEET_CHIRP_MFG_LEN, "17 bytes");
  if (blob.size() != FLEET_CHIRP_MFG_LEN) return;
  uint8_t want[FLEET_CHIRP_MFG_LEN] = {0xFF, 0xFF};
  fleet_chirp_build(want + 2, FLEET_CHIRP_TYPE_HEARTBEAT, 2, kGolden + 5, 0xAB, 0xCD);
  CHECK(memcmp(bytes(blob), want, sizeof(want)) == 0, "the heartbeat's 17 bytes are fleet_chirp_build()'s");
  host_sim::now_ms = host_sim::now_ms.load() + CHIRP_BROADCAST_DURATION_MS;
  ble_chirp::update();
}

}  // namespace

// The clock only moves forward, as millis() does on a device: each case
// starts after the last one's chirps.
int main() {
  the_rate_limit_and_the_heartbeat_send_the_same_blob();
  a_heartbeat_is_the_golden_vector_but_its_own_hour();
  every_type_goes_on_air_in_the_same_layout();
  a_null_chain_head_goes_on_air_as_zeros();
  the_suffix_bytes_keep_their_order();
  if (g_failures) {
    std::printf("%d FAILURE(S) of %d checks\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d ble_chirp sender checks PASSED\n", g_checks);
  return 0;
}
