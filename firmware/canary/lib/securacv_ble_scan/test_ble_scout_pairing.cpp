/**
 * @file test_ble_scout_pairing.cpp
 * @brief Host-build test for the BLE Scout proximity pairing window (F27).
 *
 * Two halves:
 *   A. The pure window FSM (ble_scout_pairing.h): arm / offer / finish /
 *      expire / cancel, the clamps, the label rule, wrap-safe timing, the
 *      status copy and the hashed_id hex codec the HTTP API speaks.
 *   B. The integration in ble_scout.cpp: an armed window pairs the FIRST
 *      unpaired advert at/above the threshold from inside
 *      ble_scout_on_advert(), weaker adverts and already-paired beacons do
 *      not consume it, the window is single-shot, a canceled or expired
 *      window pairs nothing, the registry snapshot carries hashed_id +
 *      label and never the MAC, and every change marks the registry dirty
 *      until the loop-task tick writes it.
 *   C. The fleet-format filter (repo sweep F36): ble_scout_advert_kind.h
 *      refuses another Canary's presence beacon and Chirp and nothing else,
 *      its constants are held to fleet_beacon.h (included below by a path
 *      relative to this file) and to the Chirp shape fleet_roster_feed.cpp
 *      tables and the canary-wap's ble_config.h sends (both read from
 *      disk), and through ble_scout_on_advert() a strong beacon or Chirp
 *      from an unpaired advertiser never pairs while every near miss, and
 *      an advert with no manufacturer data, still does.
 *
 * Every advert in B and C goes through scan_advert(), which does what
 * ble_scout_nimble.cpp does with one: classify its manufacturer data and
 * pass the verdict as ble_scout_on_advert()'s `offerable`. That callback
 * needs NimBLE and is compiled out here, so C also reads both trees' copies
 * of it and holds them to that shape (one read, the whole blob classified,
 * the verdict passed by the one call).
 *
 * Run it from the repository root (the C pins open source files by the path
 * this file was compiled as), as firmware.yml's run_test does.
 *
 * Build (the firmware.yml "Mesh + Scout Host Tests" link set):
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD -Wall -Wextra -Wno-unused-parameter \
 *       firmware/canary/lib/securacv_ble_scan/test_ble_scout_pairing.cpp \
 *       firmware/canary/lib/securacv_ble_scan/src/ble_scan.cpp \
 *       firmware/canary/lib/securacv_ble_scan/src/ble_scout.cpp \
 *       firmware/canary/lib/securacv_ble_scan/src/ble_scout_state.cpp \
 *       firmware/canary/lib/securacv_ble_scan/src/ble_scout_key.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_crypto.cpp \
 *       firmware/common/csi/src/csi_event.cpp \
 *       firmware/common/csi/src/csi_module.cpp \
 *       firmware/common/csi/src/csi_bundler.cpp \
 *       firmware/common/csi/src/csi_witness_payload.cpp \
 *       -I firmware/canary/lib/securacv_ble_scan/src \
 *       -I firmware/canary/lib/securacv_mesh/src \
 *       -I firmware/common/csi/src \
 *       -o /tmp/test_scout_pairing && /tmp/test_scout_pairing
 */

#include "ble_scout.h"
#include "ble_scout_pairing.h"
#include "ble_scout_advert_kind.h"
#include "ble_scan.h"

/* The presence beacon's own wire contract, by a path relative to this file
 * (firmware/common/fleet_link/) — no -I, so the CI link set is unchanged. */
#include "../../../common/fleet_link/fleet_beacon.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_ble_scout_pairing_run() { return 0; }
#else

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                        \
    }                                                                      \
  } while (0)

namespace {

namespace P = ble_scout::pairing;
namespace AK = ble_scout::advert_kind;
using ble_scan::HASHED_ID_LEN;
using ble_scan::MAC_LEN;

/* ────────────────────────────────────────────────────────────────────────
 * A. Pure window FSM
 * ──────────────────────────────────────────────────────────────────────── */

void test_init_is_idle() {
  P::Window w;
  P::init(&w);
  CHECK(w.state == P::State::IDLE);
  const P::Status st = P::status(&w, 1234);
  CHECK(st.state == P::State::IDLE);
  CHECK(st.remaining_ms == 0);
  CHECK(std::strcmp(P::state_name(st.state), "idle") == 0);
  std::printf("PASS test_init_is_idle\n");
}

void test_clamps() {
  CHECK(P::clamp_window_ms(0) == P::DEFAULT_WINDOW_MS);
  CHECK(P::clamp_window_ms(1000) == P::MIN_WINDOW_MS);
  CHECK(P::clamp_window_ms(120000) == P::MAX_WINDOW_MS);
  CHECK(P::clamp_window_ms(30000) == 30000);
  CHECK(P::clamp_rssi_min(-100) == P::RSSI_MIN_FLOOR);
  CHECK(P::clamp_rssi_min(0) == P::RSSI_MIN_CEIL);
  CHECK(P::clamp_rssi_min(-45) == -45);
  CHECK(P::MAX_WINDOW_MS == 60000);          /* the API's window_s <= 60 */
  CHECK(P::DEFAULT_RSSI_MIN == -45);

  P::Window w;
  P::init(&w);
  CHECK(P::arm(&w, "keys", 999999, -120, 0) == P::ArmResult::OK);
  CHECK(w.window_ms == 60000);
  CHECK(w.rssi_min == -70);
  std::printf("PASS test_clamps\n");
}

void test_label_rule() {
  CHECK(!P::label_ok(nullptr));
  CHECK(!P::label_ok(""));
  CHECK(!P::label_ok("   "));
  CHECK(P::label_ok("Keys"));
  CHECK(P::label_ok("Front door tag #2"));
  CHECK(P::label_ok("abcdefghijklmnopqrstuvw"));    /* 23 = MAX_LABEL_LEN */
  CHECK(!P::label_ok("abcdefghijklmnopqrstuvwx"));  /* 24 */
  CHECK(!P::label_ok("tab\there"));
  CHECK(!P::label_ok("K\xc3\xbc" "che"));           /* UTF-8: refused, not rewritten */

  P::Window w;
  P::init(&w);
  CHECK(P::arm(&w, "", 0, -45, 0) == P::ArmResult::BAD_LABEL);
  CHECK(w.state == P::State::IDLE);
  std::printf("PASS test_label_rule\n");
}

void test_offer_threshold_and_single_shot() {
  P::Window w;
  P::init(&w);
  CHECK(P::arm(&w, "keys", 30000, -45, 1000) == P::ArmResult::OK);
  CHECK(P::arm(&w, "other", 30000, -45, 2000) == P::ArmResult::BUSY);

  CHECK(!P::offer(&w, -46, 2000));        /* one dB short */
  CHECK(w.state == P::State::ARMED);
  CHECK(P::offer(&w, -45, 3000));         /* at threshold: claims */
  CHECK(w.state == P::State::CLAIMED);
  CHECK(std::strcmp(P::state_name(w.state), "pairing") == 0);
  CHECK(!P::offer(&w, -30, 3001));        /* single-shot */
  CHECK(P::arm(&w, "other", 30000, -45, 3002) == P::ArmResult::BUSY);
  CHECK(!P::cancel(&w, 3002));            /* claimed: finishes, not canceled */

  uint8_t id[HASHED_ID_LEN];
  for (size_t i = 0; i < HASHED_ID_LEN; ++i) id[i] = (uint8_t)(0xA0 + i);
  P::finish(&w, true, id);
  CHECK(w.state == P::State::PAIRED);
  const P::Status st = P::status(&w, 4000);
  CHECK(st.state == P::State::PAIRED);
  CHECK(std::memcmp(st.paired_id, id, HASHED_ID_LEN) == 0);
  CHECK(std::strcmp(st.label, "keys") == 0);

  P::finish(&w, false, nullptr);          /* not CLAIMED: ignored */
  CHECK(w.state == P::State::PAIRED);

  /* A finished window re-arms. */
  CHECK(P::arm(&w, "wallet", 30000, -50, 5000) == P::ArmResult::OK);
  CHECK(P::status(&w, 5000).state == P::State::ARMED);
  const P::Status fresh = P::status(&w, 5000);
  for (size_t i = 0; i < HASHED_ID_LEN; ++i) CHECK(fresh.paired_id[i] == 0);
  std::printf("PASS test_offer_threshold_and_single_shot\n");
}

void test_failed_pair() {
  P::Window w;
  P::init(&w);
  CHECK(P::arm(&w, "keys", 30000, -45, 0) == P::ArmResult::OK);
  CHECK(P::offer(&w, -40, 10));
  P::finish(&w, false, nullptr);
  CHECK(w.state == P::State::FAILED);
  CHECK(std::strcmp(P::state_name(w.state), "failed") == 0);
  std::printf("PASS test_failed_pair\n");
}

void test_expiry() {
  P::Window w;
  P::init(&w);
  CHECK(P::arm(&w, "keys", 10000, -45, 1000) == P::ArmResult::OK);
  CHECK(!P::expire(&w, 10999));
  CHECK(P::status(&w, 6000).remaining_ms == 5000);
  /* status() reports an elapsed window as expired before the tick does. */
  CHECK(P::status(&w, 11000).state == P::State::EXPIRED);
  CHECK(w.state == P::State::ARMED);
  CHECK(P::expire(&w, 11000));
  CHECK(w.state == P::State::EXPIRED);
  CHECK(!P::expire(&w, 12000));
  CHECK(!P::offer(&w, -30, 12000));

  /* An offer past the window expires it even if no tick ran. */
  CHECK(P::arm(&w, "keys", 10000, -45, 20000) == P::ArmResult::OK);
  CHECK(!P::offer(&w, -30, 30000));
  CHECK(w.state == P::State::EXPIRED);

  /* An armed-but-elapsed window no longer blocks a new arm. */
  CHECK(P::arm(&w, "keys", 10000, -45, 40000) == P::ArmResult::OK);
  CHECK(P::arm(&w, "keys2", 10000, -45, 50000) == P::ArmResult::OK);
  std::printf("PASS test_expiry\n");
}

void test_cancel() {
  P::Window w;
  P::init(&w);
  CHECK(!P::cancel(&w, 0));               /* idle */
  CHECK(P::arm(&w, "keys", 10000, -45, 0) == P::ArmResult::OK);
  CHECK(P::cancel(&w, 100));
  CHECK(w.state == P::State::CANCELED);
  CHECK(std::strcmp(P::state_name(w.state), "canceled") == 0);
  CHECK(!P::offer(&w, -30, 200));
  CHECK(P::arm(&w, "keys", 10000, -45, 300) == P::ArmResult::OK);
  CHECK(!P::cancel(&w, 10300));           /* elapsed: expires instead */
  CHECK(w.state == P::State::EXPIRED);
  std::printf("PASS test_cancel\n");
}

void test_millis_wrap() {
  P::Window w;
  P::init(&w);
  const uint32_t t0 = 0xFFFFF000u;        /* 4096 ms before the wrap */
  CHECK(P::arm(&w, "keys", 10000, -45, t0) == P::ArmResult::OK);
  CHECK(!P::expire(&w, 0x00000100u));     /* 4352 ms in: still armed */
  CHECK(P::status(&w, 0x00000100u).remaining_ms == 10000 - 4352);
  CHECK(P::offer(&w, -40, 0x00000100u));
  P::Window w2;
  P::init(&w2);
  CHECK(P::arm(&w2, "keys", 10000, -45, t0) == P::ArmResult::OK);
  CHECK(P::expire(&w2, t0 + 10000u));     /* wraps past zero, expires */
  std::printf("PASS test_millis_wrap\n");
}

void test_hex_codec() {
  uint8_t id[HASHED_ID_LEN];
  for (size_t i = 0; i < HASHED_ID_LEN; ++i) id[i] = (uint8_t)(i * 17);
  char hex[2 * HASHED_ID_LEN + 1];
  P::id_to_hex(id, hex);
  CHECK(std::strlen(hex) == 32);
  CHECK(std::strcmp(hex, "00112233445566778899aabbccddeeff") == 0);
  uint8_t back[HASHED_ID_LEN];
  CHECK(P::id_from_hex(hex, back));
  CHECK(std::memcmp(back, id, HASHED_ID_LEN) == 0);
  CHECK(P::id_from_hex("00112233445566778899AABBCCDDEEFF", back));
  CHECK(std::memcmp(back, id, HASHED_ID_LEN) == 0);

  uint8_t untouched[HASHED_ID_LEN];
  std::memset(untouched, 0x5A, sizeof(untouched));
  CHECK(!P::id_from_hex(nullptr, untouched));
  CHECK(!P::id_from_hex("", untouched));
  CHECK(!P::id_from_hex("00112233445566778899aabbccddeef", untouched));    /* 31 */
  CHECK(!P::id_from_hex("00112233445566778899aabbccddeeff0", untouched));  /* 33 */
  CHECK(!P::id_from_hex("00112233445566778899aabbccddeefg", untouched));
  CHECK(!P::id_from_hex("0011223344556677 899aabbccddeeff", untouched));
  for (size_t i = 0; i < HASHED_ID_LEN; ++i) CHECK(untouched[i] == 0x5A);
  std::printf("PASS test_hex_codec\n");
}

/* ────────────────────────────────────────────────────────────────────────
 * C1. The fleet-format classifier, pure (ble_scout_advert_kind.h, F36)
 * ──────────────────────────────────────────────────────────────────────── */

/* A presence beacon exactly as the scanner sees it: the company id
 * (fleet_beacon.h's) followed by fleet_beacon.h's own builder output. */
size_t beacon_blob(uint8_t* out, bool v2, uint8_t flags) {
  out[0] = (uint8_t)(FLEET_BEACON_COMPANY_ID & 0xFF);
  out[1] = (uint8_t)((FLEET_BEACON_COMPANY_ID >> 8) & 0xFF);
  if (v2) {
    fleet_beacon_build_v2(out + 2, flags, 87, 93, 0x12345u, 0xAB, 0xCD,
                          FLEET_BEACON_DETECT_PERSON, 77);
    return FLEET_BEACON_MFG_V2_LEN;
  }
  fleet_beacon_build(out + 2, flags, 87, 93, 0x12345u, 0xAB, 0xCD);
  return FLEET_BEACON_MFG_LEN;
}

/* A Chirp in the layout the canary-wap's ble_config.h documents (17 bytes:
 * company 0xFFFF, type, 4-byte hour bucket, 8-byte chain-hash prefix,
 * 2-byte device id prefix). The length here is that comment's, not the
 * classifier's; test_chirp_shape_held_to_roster_and_sender holds the two
 * together. */
constexpr size_t kChirpWireLen = 17;
size_t chirp_blob(uint8_t out[kChirpWireLen], uint8_t type) {
  out[0] = 0xFF;
  out[1] = 0xFF;
  out[2] = type;
  for (size_t i = 3; i < 15; ++i) out[i] = (uint8_t)(0x30 + i);
  out[15] = 0xAB;
  out[16] = 0xCD;
  return kChirpWireLen;
}

void test_advert_kind_held_to_fleet_beacon_h() {
  CHECK(AK::BEACON_TYPE == FLEET_BEACON_TYPE);
  CHECK(AK::BEACON_LEN_V1 == FLEET_BEACON_MFG_LEN);
  CHECK(AK::BEACON_LEN_V2 == FLEET_BEACON_MFG_V2_LEN);
  CHECK(AK::BEACON_LEN_V1 == FLEET_BEACON_PAYLOAD_LEN + 2);
  CHECK(AK::BEACON_LEN_V2 == FLEET_BEACON_PAYLOAD_V2_LEN + 2);
  CHECK(AK::COMPANY_ID_LO == (FLEET_BEACON_COMPANY_ID & 0xFF));
  CHECK(AK::COMPANY_ID_HI == ((FLEET_BEACON_COMPANY_ID >> 8) & 0xFF));

  /* Every beacon fleet_beacon.h builds is refused, whatever its flags. */
  const uint8_t flag_sets[] = {0x00, FLEET_BEACON_FLAG_TAMPER | FLEET_BEACON_FLAG_ALERT,
                               FLEET_BEACON_FLAG_SETUP_OPEN, 0xFF};
  for (uint8_t flags : flag_sets) {
    for (int v2 = 0; v2 <= 1; ++v2) {
      uint8_t blob[FLEET_BEACON_MFG_V2_LEN];
      const size_t n = beacon_blob(blob, v2 != 0, flags);
      FleetBeaconFields f;
      CHECK(fleet_beacon_parse(blob, n, &f));
      CHECK(AK::classify(blob, n) == AK::Kind::PRESENCE_BEACON);
      CHECK(!AK::offerable(blob, n));
    }
  }
  CHECK(std::strcmp(AK::kind_name(AK::Kind::PRESENCE_BEACON), "presence_beacon") == 0);
  std::printf("PASS test_advert_kind_held_to_fleet_beacon_h\n");
}

/* fleet_beacon_parse() is the roster's beacon reader. Every blob it accepts,
 * over every length 0..32 and every type and version byte, is refused. */
void test_every_parsed_beacon_is_refused() {
  int parsed = 0;
  uint8_t blob[32];
  for (size_t len = 0; len <= sizeof(blob); ++len) {
    for (int type = 0; type <= 0xFF; ++type) {
      for (int version = 0; version <= 3; ++version) {
        std::memset(blob, 0x5A, sizeof(blob));
        blob[0] = 0xFF;
        blob[1] = 0xFF;
        blob[2] = (uint8_t)type;
        blob[3] = (uint8_t)version;
        FleetBeaconFields f;
        if (!fleet_beacon_parse(blob, len, &f)) continue;
        ++parsed;
        CHECK(AK::classify(blob, len) == AK::Kind::PRESENCE_BEACON);
      }
    }
  }
  CHECK(parsed == 2);   /* v1 at 11 bytes, v2 at 13 */

  /* The version byte is not read (a choice): a beacon-typed advert at a
   * beacon length from a schema this parser predates is still refused. */
  uint8_t odd[FLEET_BEACON_MFG_LEN];
  beacon_blob(odd, false, 0);
  odd[3] = 0x07;
  FleetBeaconFields f;
  CHECK(!fleet_beacon_parse(odd, sizeof(odd), &f));
  CHECK(AK::classify(odd, sizeof(odd)) == AK::Kind::PRESENCE_BEACON);
  std::printf("PASS test_every_parsed_beacon_is_refused\n");
}

void test_chirps_are_refused() {
  for (int type = 0x01; type <= 0x05; ++type) {
    uint8_t blob[kChirpWireLen];
    const size_t n = chirp_blob(blob, (uint8_t)type);
    CHECK(AK::classify(blob, n) == AK::Kind::CHIRP_ADVERT);
    CHECK(!AK::offerable(blob, n));
  }
  CHECK(std::strcmp(AK::kind_name(AK::Kind::CHIRP_ADVERT), "chirp") == 0);
  std::printf("PASS test_chirps_are_refused\n");
}

/* Over every length 0..40 and every type byte: with company 0xFFFF exactly
 * one type is refused at 11 and at 13 bytes, five at 17 and none anywhere
 * else; with any other company nothing is refused at all. */
void test_refusals_counted_exhaustively() {
  const uint8_t companies[][2] = {{0xFF, 0xFF}, {0x4C, 0x00}, {0xFE, 0xFF},
                                  {0xFF, 0xFE}, {0x00, 0x00}, {0x59, 0x00}};
  uint8_t blob[40];
  for (const auto& c : companies) {
    const bool fleet_company = c[0] == 0xFF && c[1] == 0xFF;
    for (size_t len = 0; len <= sizeof(blob); ++len) {
      int refused = 0;
      for (int type = 0; type <= 0xFF; ++type) {
        std::memset(blob, 0x00, sizeof(blob));
        blob[0] = c[0];
        blob[1] = c[1];
        blob[2] = (uint8_t)type;
        if (!AK::offerable(blob, len)) ++refused;
      }
      int want = 0;
      if (fleet_company && (len == 11 || len == 13)) want = 1;
      if (fleet_company && len == 17) want = 5;
      if (refused != want) {
        std::fprintf(stderr, "company %02X%02X len %zu: %d refused, want %d\n",
                     c[1], c[0], len, refused, want);
      }
      CHECK(refused == want);
    }
  }
  std::printf("PASS test_refusals_counted_exhaustively\n");
}

/* The near misses the window must still be offered. */
struct NearMiss {
  const char* name;
  uint8_t     bytes[24];
  size_t      len;
};

std::size_t near_misses(NearMiss* out) {
  std::size_t n = 0;
  auto add = [&](const char* name, const uint8_t* b, size_t len) {
    out[n].name = name;
    std::memset(out[n].bytes, 0, sizeof(out[n].bytes));
    std::memcpy(out[n].bytes, b, len);
    out[n].len = len;
    ++n;
  };
  uint8_t c[kChirpWireLen];
  uint8_t b1[FLEET_BEACON_MFG_LEN];
  uint8_t b2[FLEET_BEACON_MFG_V2_LEN];
  uint8_t t[24];

  chirp_blob(c, 0x01); c[0] = 0x4C; c[1] = 0x00;      add("chirp, company 0x004C", c, sizeof(c));
  chirp_blob(c, 0x01); c[0] = 0xFE;                   add("chirp, company 0xFFFE", c, sizeof(c));
  chirp_blob(c, 0x01); c[1] = 0xFE;                   add("chirp, company 0xFEFF", c, sizeof(c));
  chirp_blob(c, 0x00);                                add("chirp, type 0x00", c, sizeof(c));
  chirp_blob(c, 0x06);                                add("chirp, type 0x06", c, sizeof(c));
  chirp_blob(c, 0x10);                                add("chirp length, beacon type", c, sizeof(c));
  chirp_blob(c, 0x01);                                add("chirp, 16 bytes", c, 16);
  std::memset(t, 0x11, sizeof(t)); chirp_blob(t, 0x01);
                                                      add("chirp, 18 bytes", t, 18);
  beacon_blob(b1, false, 0); b1[0] = 0x4C; b1[1] = 0x00; add("beacon v1, company 0x004C", b1, sizeof(b1));
  beacon_blob(b2, true, 0);  b2[1] = 0xFE;            add("beacon v2, company 0xFEFF", b2, sizeof(b2));
  beacon_blob(b1, false, 0); b1[2] = 0x0F;            add("beacon v1, type 0x0F", b1, sizeof(b1));
  beacon_blob(b1, false, 0); b1[2] = 0x11;            add("beacon v1, type 0x11", b1, sizeof(b1));
  beacon_blob(b2, true, 0);  b2[2] = 0x02;            add("beacon v2, type 0x02", b2, sizeof(b2));
  beacon_blob(b1, false, 0);                          add("beacon v1, truncated to 10", b1, 10);
  std::memset(t, 0, sizeof(t)); beacon_blob(t, false, 0);
                                                      add("beacon v1 bytes, 12 long", t, 12);
  std::memset(t, 0, sizeof(t)); beacon_blob(t, true, 0);
                                                      add("beacon v2 bytes, 14 long", t, 14);
  chirp_blob(c, 0x01);                                add("truncated to the type byte", c, 3);
  chirp_blob(c, 0x01);                                add("truncated to the company id", c, 2);
  chirp_blob(c, 0x01);                                add("one byte", c, 1);
  return n;
}

void test_near_misses_are_offerable() {
  NearMiss table[32];
  const std::size_t n = near_misses(table);
  CHECK(n == 19);
  for (std::size_t i = 0; i < n; ++i) {
    /* An exact-size heap copy, so a sanitizer build sees any read past the
     * advert's last byte (the truncated shapes are 1, 2 and 3 bytes long). */
    const std::vector<uint8_t> exact(table[i].bytes, table[i].bytes + table[i].len);
    if (!AK::offerable(exact.data(), exact.size())) {
      std::fprintf(stderr, "near miss refused: %s\n", table[i].name);
    }
    CHECK(AK::offerable(exact.data(), exact.size()));
    CHECK(AK::classify(exact.data(), exact.size()) == AK::Kind::NOT_FLEET);
  }
  /* No manufacturer data: NimBLE's empty string, or no buffer at all. */
  const uint8_t none[1] = {0};
  CHECK(AK::offerable(none, 0));
  CHECK(AK::offerable(nullptr, 0));
  CHECK(AK::offerable(nullptr, 17));   /* a length without bytes is nothing */
  CHECK(std::strcmp(AK::kind_name(AK::Kind::NOT_FLEET), "not_fleet") == 0);
  std::printf("PASS test_near_misses_are_offerable\n");
}

/* ── Source pins: the Chirp shape lives in two files this test cannot
 *    compile (fleet_roster_feed.cpp needs the fleet_link include path, and
 *    ble_config.h needs Arduino), so it reads them, whitespace collapsed. */

std::string dir_of_this_file() {
  const std::string f = __FILE__;
  const std::size_t slash = f.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : f.substr(0, slash);
}

std::string read_raw(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "cannot open %s (run from the repository root)\n", path.c_str());
    std::exit(1);
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string collapse(const std::string& raw) {
  std::string out;
  bool space = false;
  for (char ch : raw) {
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += ch;
  }
  return out;
}

std::string read_collapsed(const std::string& path) { return collapse(read_raw(path)); }

/* The code alone: every comment becomes one space, string and character
 * literals are kept whole (a comment opener inside one is not a comment),
 * then whitespace is collapsed. For counting calls and declarations, where a
 * comment that names a function must not count as a call to it. */
std::string read_code(const std::string& path) {
  const std::string raw = read_raw(path);
  std::string out;
  std::size_t i = 0;
  while (i < raw.size()) {
    const char c = raw[i];
    const char next = i + 1 < raw.size() ? raw[i + 1] : '\0';
    if (c == '/' && next == '/') {
      while (i < raw.size() && raw[i] != '\n') ++i;
      out += ' ';
    } else if (c == '/' && next == '*') {
      const std::size_t end = raw.find("*/", i + 2);
      i = end == std::string::npos ? raw.size() : end + 2;
      out += ' ';
    } else if (c == '"' || c == '\'') {
      out += c;
      ++i;
      while (i < raw.size() && raw[i] != c && raw[i] != '\n') {
        if (raw[i] == '\\' && i + 1 < raw.size()) out += raw[i++];
        out += raw[i++];
      }
      if (i < raw.size()) out += raw[i++];
    } else {
      out += c;
      ++i;
    }
  }
  return collapse(out);
}

bool has(const std::string& text, const std::string& needle) {
  if (text.find(needle) != std::string::npos) return true;
  std::fprintf(stderr, "missing from source: \"%s\"\n", needle.c_str());
  return false;
}

std::size_t count_of(const std::string& text, const std::string& needle) {
  std::size_t n = 0;
  for (std::size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + needle.size())) {
    ++n;
  }
  return n;
}

bool count_is(const std::string& text, const std::string& needle, std::size_t want,
              const std::string& where) {
  const std::size_t n = count_of(text, needle);
  if (n == want) return true;
  std::fprintf(stderr, "%s: \"%s\" occurs %zu times, want %zu\n", where.c_str(),
               needle.c_str(), n, want);
  return false;
}

void test_chirp_shape_held_to_roster_and_sender() {
  char buf[160];

  /* The receiver: fleet_roster_feed.cpp's on_advert() tables exactly two
   * shapes, the beacon through fleet_beacon.h and the Chirp by literals. */
  const std::string roster = read_collapsed(dir_of_this_file() + "/src/fleet_roster_feed.cpp");
  const std::size_t body = roster.find("void on_advert(");
  const std::size_t body_end = roster.find("void tick(", body);
  CHECK(body != std::string::npos && body_end != std::string::npos);
  const std::string on_advert = roster.substr(body, body_end - body);
  CHECK(count_of(on_advert, "if (len ==") == 2);
  CHECK(has(on_advert, "if (len == FLEET_BEACON_MFG_LEN || len == FLEET_BEACON_MFG_V2_LEN) {"));
  CHECK(has(on_advert, "if (!fleet_beacon_parse(mfg, len, &f)) return;"));
  std::snprintf(buf, sizeof(buf), "if (len == %zu) {", AK::CHIRP_LEN);
  CHECK(has(on_advert, buf));
  std::snprintf(buf, sizeof(buf), "if (mfg[0] != 0x%02X || mfg[1] != 0x%02X) return;",
                AK::COMPANY_ID_LO, AK::COMPANY_ID_HI);
  CHECK(has(on_advert, buf));
  CHECK(has(on_advert, "const uint8_t type = mfg[2];"));
  std::snprintf(buf, sizeof(buf), "if (type < 0x%02X || type > 0x%02X) return;",
                AK::CHIRP_TYPE_FIRST, AK::CHIRP_TYPE_LAST);
  CHECK(has(on_advert, buf));

  /* The sender: the canary-wap's ble_config.h — the payload after the
   * company id, the company id, and the ChirpType values, contiguous from
   * CHIRP_TYPE_FIRST to CHIRP_TYPE_LAST. */
  const std::string cfg = read_collapsed(
      dir_of_this_file() + "/../../../projects/canary-wap/arduino/canary_wap/ble_config.h");
  std::snprintf(buf, sizeof(buf), "#define CHIRP_PAYLOAD_SIZE %zu ", AK::CHIRP_LEN - 2);
  CHECK(has(cfg, buf));
  std::snprintf(buf, sizeof(buf), "#define CHIRP_COMPANY_ID 0x%02X%02X ",
                AK::COMPANY_ID_HI, AK::COMPANY_ID_LO);
  CHECK(has(cfg, buf));
  const std::size_t e0 = cfg.find("enum ChirpType : uint8_t {");
  CHECK(e0 != std::string::npos);
  const std::size_t e1 = cfg.find("};", e0);
  CHECK(e1 != std::string::npos);
  const std::string chirp_enum = cfg.substr(e0, e1 - e0);
  CHECK(count_of(chirp_enum, "= 0x") ==
        (std::size_t)(AK::CHIRP_TYPE_LAST - AK::CHIRP_TYPE_FIRST + 1));
  for (int t = AK::CHIRP_TYPE_FIRST; t <= AK::CHIRP_TYPE_LAST; ++t) {
    std::snprintf(buf, sizeof(buf), "= 0x%02X,", t);
    CHECK(has(chirp_enum, buf));
  }
  CHECK(kChirpWireLen == AK::CHIRP_LEN);
  std::printf("PASS test_chirp_shape_held_to_roster_and_sender\n");
}

/* ── The scan callback itself: ScoutScanCallbacks::onResult() in
 *    ble_scout_nimble.cpp, both trees. It needs NimBLE, so the host build
 *    compiles it out and scan_advert() below stands in for it. This pin holds
 *    both copies to what scan_advert() does: one manufacturer-data read, the
 *    classifier asked about the whole blob (company id included), and its
 *    verdict passed unchanged by the one call into ble_scout_on_advert(),
 *    with no way out of onResult() before that call but a null device.
 *    check_ble_scan_sync.sh catches one copy drifting from the other; this
 *    catches the two drifting together. `call` is each tree's own spelling
 *    of the call (the canary names `now` for its fleet roster feed too). */
void check_scan_callback(const std::string& path, const std::string& call) {
  const std::string src = read_code(path);
  CHECK(has(src, "#include \"ble_scout_advert_kind.h\""));
  const std::size_t cb = src.find("void onResult(");
  CHECK(cb != std::string::npos);
  const std::size_t cb_end = src.find("void onScanEnd(", cb);
  CHECK(cb_end != std::string::npos);
  const std::string on_result = src.substr(cb, cb_end - cb);

  const std::string verdict =
      "const std::string mfg = device->haveManufacturerData() ? "
      "device->getManufacturerData() : std::string(); "
      "const uint8_t* mfg_bytes = reinterpret_cast<const uint8_t*>(mfg.data()); "
      "const bool offerable = advert_kind::offerable(mfg_bytes, mfg.size()); ";
  if (on_result.find(verdict + call) == std::string::npos) {
    std::fprintf(stderr, "scan callback drifted: %s\n", path.c_str());
  }
  CHECK(has(on_result, verdict + call));
  CHECK(has(on_result, "if (!device) return;"));
  CHECK(count_is(on_result, "return", 1, path));

  /* Once each in the whole file: no second read, no second call, no second
   * (shadowing) flag. */
  CHECK(count_is(src, "haveManufacturerData(", 1, path));
  CHECK(count_is(src, "getManufacturerData(", 1, path));
  CHECK(count_is(src, "advert_kind::", 1, path));
  CHECK(count_is(src, "bool offerable", 1, path));
  CHECK(count_is(src, "ble_scout_on_advert(", 1, path));
}

void test_scan_callbacks_pass_the_verdict() {
  check_scan_callback(dir_of_this_file() + "/src/ble_scout_nimble.cpp",
                      "const uint32_t now = millis(); "
                      "ble_scout_on_advert(mac, rssi, now, offerable);");
  check_scan_callback(
      dir_of_this_file() +
          "/../../../projects/canary-wap/arduino/canary_wap/ble_scout_nimble.cpp",
      "ble_scout_on_advert(mac, rssi, millis(), offerable);");
  std::printf("PASS test_scan_callbacks_pass_the_verdict\n");
}

/* ────────────────────────────────────────────────────────────────────────
 * B. Integration through ble_scout.cpp (one process, shared state — each
 *    test builds on the registry the previous ones left).
 * ──────────────────────────────────────────────────────────────────────── */

/* The host build's deterministic per-device key (ble_scout_key.cpp). */
void expected_id(const uint8_t mac[MAC_LEN], uint8_t out[HASHED_ID_LEN]) {
  uint8_t key[ble_scan::PER_DEVICE_KEY_LEN];
  for (size_t i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)i;
  CHECK(ble_scan::hash_beacon_id(key, mac, out));
}

bool snapshot_has(const uint8_t id[HASHED_ID_LEN], const char* label) {
  ble_scan::PairedBeacon snap[ble_scan::MAX_PAIRED_BEACONS];
  const size_t n = ble_scout::ble_scout_registry_snapshot(snap, ble_scan::MAX_PAIRED_BEACONS);
  for (size_t i = 0; i < n; ++i) {
    if (std::memcmp(snap[i].hashed_id, id, HASHED_ID_LEN) == 0) {
      return label == nullptr || std::strcmp(snap[i].label, label) == 0;
    }
  }
  return false;
}

/* No byte sequence equal to the MAC anywhere in the snapshot. */
bool snapshot_free_of(const uint8_t mac[MAC_LEN]) {
  ble_scan::PairedBeacon snap[ble_scan::MAX_PAIRED_BEACONS];
  std::memset(snap, 0, sizeof(snap));
  const size_t n = ble_scout::ble_scout_registry_snapshot(snap, ble_scan::MAX_PAIRED_BEACONS);
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(snap);
  const size_t len = n * sizeof(snap[0]);
  for (size_t i = 0; i + MAC_LEN <= len; ++i) {
    if (std::memcmp(bytes + i, mac, MAC_LEN) == 0) return false;
  }
  return true;
}

/* What ble_scout_nimble.cpp does with one advert: classify its manufacturer
 * data (none by default, as for a tag) and pass the verdict along.
 * test_scan_callbacks_pass_the_verdict holds both trees' callbacks to it. */
void scan_advert(const uint8_t mac[MAC_LEN], int8_t rssi, uint32_t now,
                 const uint8_t* mfg = nullptr, size_t mfg_len = 0) {
  ble_scout::ble_scout_on_advert(mac, rssi, now, AK::offerable(mfg, mfg_len));
}

void test_window_before_init_is_not_ready() {
  CHECK(ble_scout::ble_scout_pair_window_start("keys", 0, -45, 0) ==
        P::ArmResult::NOT_READY);
  std::printf("PASS test_window_before_init_is_not_ready\n");
}

void test_window_pairs_first_strong_unpaired_advert() {
  CHECK(ble_scout::ble_scout_init());
  CHECK(ble_scout::ble_scout_count() == 0);
  CHECK(!ble_scout::ble_scout_registry_dirty());

  CHECK(ble_scout::ble_scout_pair_window_start("keys", 30000, -45, 1000) ==
        P::ArmResult::OK);
  CHECK(ble_scout::ble_scout_pair_window_start("x", 30000, -45, 1001) ==
        P::ArmResult::BUSY);

  const uint8_t far_mac[MAC_LEN]  = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  const uint8_t near_mac[MAC_LEN] = {0xC0, 0xFF, 0xEE, 0x00, 0x11, 0x22};
  scan_advert(far_mac, -60, 2000);   /* across the room */
  CHECK(ble_scout::ble_scout_count() == 0);
  CHECK(ble_scout::ble_scout_pair_window_status(2000).state == P::State::ARMED);

  scan_advert(near_mac, -38, 2500);  /* held against it */
  CHECK(ble_scout::ble_scout_count() == 1);

  uint8_t want[HASHED_ID_LEN];
  expected_id(near_mac, want);
  const P::Status st = ble_scout::ble_scout_pair_window_status(2600);
  CHECK(st.state == P::State::PAIRED);
  CHECK(std::memcmp(st.paired_id, want, HASHED_ID_LEN) == 0);
  CHECK(snapshot_has(want, "keys"));
  CHECK(snapshot_free_of(near_mac));

  /* Single-shot: a second strong stranger does not pair. */
  const uint8_t other_mac[MAC_LEN] = {0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
  scan_advert(other_mac, -30, 2700);
  CHECK(ble_scout::ble_scout_count() == 1);

  /* The loop-task tick owes, then writes, the NVS blob. */
  CHECK(ble_scout::ble_scout_registry_dirty());
  ble_scout::ble_scout_tick(3000);
  CHECK(!ble_scout::ble_scout_registry_dirty());
  std::printf("PASS test_window_pairs_first_strong_unpaired_advert\n");
}

void test_paired_beacon_does_not_consume_window() {
  CHECK(ble_scout::ble_scout_pair_window_start("wallet", 30000, -45, 10000) ==
        P::ArmResult::OK);
  const uint8_t keys_mac[MAC_LEN]   = {0xC0, 0xFF, 0xEE, 0x00, 0x11, 0x22};
  const uint8_t wallet_mac[MAC_LEN] = {0x77, 0x66, 0x55, 0x44, 0x33, 0x22};
  scan_advert(keys_mac, -30, 10100);  /* already paired */
  CHECK(ble_scout::ble_scout_pair_window_status(10100).state == P::State::ARMED);
  CHECK(ble_scout::ble_scout_count() == 1);
  uint8_t keys_id[HASHED_ID_LEN];
  expected_id(keys_mac, keys_id);
  CHECK(snapshot_has(keys_id, "keys"));                   /* not relabeled */

  scan_advert(wallet_mac, -44, 10200);
  CHECK(ble_scout::ble_scout_count() == 2);
  uint8_t wallet_id[HASHED_ID_LEN];
  expected_id(wallet_mac, wallet_id);
  CHECK(snapshot_has(wallet_id, "wallet"));
  CHECK(snapshot_free_of(wallet_mac));
  ble_scout::ble_scout_tick(10300);
  std::printf("PASS test_paired_beacon_does_not_consume_window\n");
}

void test_canceled_and_expired_windows_pair_nothing() {
  const uint8_t mac[MAC_LEN] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

  CHECK(ble_scout::ble_scout_pair_window_start("bag", 30000, -45, 20000) ==
        P::ArmResult::OK);
  CHECK(ble_scout::ble_scout_pair_window_cancel(20100));
  CHECK(!ble_scout::ble_scout_pair_window_cancel(20150));
  CHECK(ble_scout::ble_scout_pair_window_status(20100).state == P::State::CANCELED);
  scan_advert(mac, -30, 20200);
  CHECK(ble_scout::ble_scout_count() == 2);

  CHECK(ble_scout::ble_scout_pair_window_start("bag", 10000, -45, 30000) ==
        P::ArmResult::OK);
  ble_scout::ble_scout_tick(40000);                       /* loop-task expiry */
  CHECK(ble_scout::ble_scout_pair_window_status(40000).state == P::State::EXPIRED);
  scan_advert(mac, -30, 40100);
  CHECK(ble_scout::ble_scout_count() == 2);
  CHECK(!ble_scout::ble_scout_registry_dirty());
  std::printf("PASS test_canceled_and_expired_windows_pair_nothing\n");
}

void test_bad_label_refused_at_start() {
  CHECK(ble_scout::ble_scout_pair_window_start("", 0, -45, 50000) ==
        P::ArmResult::BAD_LABEL);
  CHECK(ble_scout::ble_scout_pair_window_start("K\xc3\xbc" "che", 0, -45, 50000) ==
        P::ArmResult::BAD_LABEL);
  std::printf("PASS test_bad_label_refused_at_start\n");
}

void test_unpair_marks_dirty() {
  const uint8_t wallet_mac[MAC_LEN] = {0x77, 0x66, 0x55, 0x44, 0x33, 0x22};
  uint8_t wallet_id[HASHED_ID_LEN];
  expected_id(wallet_mac, wallet_id);
  CHECK(ble_scout::ble_scout_unpair(wallet_id));
  CHECK(ble_scout::ble_scout_count() == 1);
  CHECK(!snapshot_has(wallet_id, nullptr));
  CHECK(ble_scout::ble_scout_registry_dirty());
  ble_scout::ble_scout_tick(60000);
  CHECK(!ble_scout::ble_scout_registry_dirty());
  CHECK(!ble_scout::ble_scout_unpair(wallet_id));         /* already gone */
  CHECK(!ble_scout::ble_scout_registry_dirty());          /* nothing changed */
  std::printf("PASS test_unpair_marks_dirty\n");
}

/* ────────────────────────────────────────────────────────────────────────
 * C2. The fleet-format filter through ble_scout_on_advert() (F36)
 * ──────────────────────────────────────────────────────────────────────── */

void test_fleet_adverts_never_pair() {
  const size_t base = ble_scout::ble_scout_count();
  CHECK(!ble_scout::ble_scout_registry_dirty());
  CHECK(ble_scout::ble_scout_pair_window_start("tag", 30000, -45, 61000) ==
        P::ArmResult::OK);

  /* Another Canary's presence beacon (v1 and v2) and each Chirp type, from
   * unpaired advertisers held right against this one, several times each. */
  uint32_t t = 61100;
  uint8_t mac[MAC_LEN] = {0xCA, 0xFE, 0x00, 0x00, 0x00, 0x00};
  for (int v2 = 0; v2 <= 1; ++v2) {
    uint8_t blob[FLEET_BEACON_MFG_V2_LEN];
    const size_t n = beacon_blob(blob, v2 != 0, FLEET_BEACON_FLAG_ALERT);
    mac[5] = (uint8_t)(0x10 + v2);
    for (int k = 0; k < 5; ++k) scan_advert(mac, -20, t += 10, blob, n);
  }
  for (int type = 0x01; type <= 0x05; ++type) {
    uint8_t blob[kChirpWireLen];
    const size_t n = chirp_blob(blob, (uint8_t)type);
    mac[5] = (uint8_t)(0x20 + type);
    for (int k = 0; k < 5; ++k) scan_advert(mac, -25, t += 10, blob, n);
  }
  CHECK(ble_scout::ble_scout_count() == base);
  CHECK(ble_scout::ble_scout_pair_window_status(t).state == P::State::ARMED);
  CHECK(!ble_scout::ble_scout_registry_dirty());

  /* The window is still the tag's: the owner's tag (no manufacturer data)
   * pairs once it is held close. */
  const uint8_t tag_mac[MAC_LEN] = {0x7A, 0x60, 0x00, 0x00, 0x00, 0x01};
  scan_advert(tag_mac, -40, t += 10);
  CHECK(ble_scout::ble_scout_count() == base + 1);
  CHECK(ble_scout::ble_scout_pair_window_status(t).state == P::State::PAIRED);
  uint8_t tag_id[HASHED_ID_LEN];
  expected_id(tag_mac, tag_id);
  CHECK(snapshot_has(tag_id, "tag"));

  CHECK(ble_scout::ble_scout_unpair(tag_id));
  ble_scout::ble_scout_tick(t + 10);
  CHECK(ble_scout::ble_scout_count() == base);
  CHECK(!ble_scout::ble_scout_registry_dirty());
  std::printf("PASS test_fleet_adverts_never_pair\n");
}

/* A paired device whose advert is in a fleet format still does not consume
 * the window (paired never does) and keeps its label: the filter sits on
 * the unpaired branch only. */
void test_paired_fleet_format_advert_leaves_window_alone() {
  const uint8_t keys_mac[MAC_LEN] = {0xC0, 0xFF, 0xEE, 0x00, 0x11, 0x22};
  uint8_t keys_id[HASHED_ID_LEN];
  expected_id(keys_mac, keys_id);
  CHECK(snapshot_has(keys_id, "keys"));
  CHECK(ble_scout::ble_scout_pair_window_start("wallet2", 10000, -45, 63000) ==
        P::ArmResult::OK);
  uint8_t blob[FLEET_BEACON_MFG_LEN];
  const size_t n = beacon_blob(blob, false, 0);
  scan_advert(keys_mac, -30, 63010, blob, n);
  CHECK(ble_scout::ble_scout_pair_window_status(63010).state == P::State::ARMED);
  CHECK(snapshot_has(keys_id, "keys"));
  CHECK(ble_scout::ble_scout_pair_window_cancel(63020));
  std::printf("PASS test_paired_fleet_format_advert_leaves_window_alone\n");
}

void test_near_miss_adverts_still_pair() {
  const size_t base = ble_scout::ble_scout_count();
  NearMiss table[32];
  const std::size_t n = near_misses(table);
  uint32_t t = 64000;
  for (std::size_t i = 0; i <= n + 1; ++i) {
    /* i == n: NimBLE's empty manufacturer data; i == n + 1: none at all. */
    const uint8_t empty[1] = {0};
    const uint8_t* bytes = i < n ? table[i].bytes : (i == n ? empty : nullptr);
    const size_t   len   = i < n ? table[i].len : 0;
    const char*    name  = i < n ? table[i].name : (i == n ? "empty data" : "no data");

    CHECK(ble_scout::ble_scout_pair_window_start("near", 10000, -45, t) ==
          P::ArmResult::OK);
    const uint8_t mac[MAC_LEN] = {0x5A, 0x5A, 0x00, 0x00, (uint8_t)i, 0x01};
    scan_advert(mac, -40, t + 10, bytes, len);
    if (ble_scout::ble_scout_pair_window_status(t + 20).state != P::State::PAIRED) {
      std::fprintf(stderr, "near miss did not pair: %s\n", name);
    }
    CHECK(ble_scout::ble_scout_pair_window_status(t + 20).state == P::State::PAIRED);
    CHECK(ble_scout::ble_scout_count() == base + 1);
    uint8_t id[HASHED_ID_LEN];
    expected_id(mac, id);
    CHECK(snapshot_has(id, "near"));
    CHECK(ble_scout::ble_scout_unpair(id));
    ble_scout::ble_scout_tick(t + 30);
    CHECK(ble_scout::ble_scout_count() == base);
    t += 100;
  }
  CHECK(!ble_scout::ble_scout_registry_dirty());
  std::printf("PASS test_near_miss_adverts_still_pair (%zu shapes)\n", n + 2);
}

void test_full_registry_refuses_to_arm() {
  /* Fill the remaining slots through the window itself. */
  uint32_t t = 70000;
  while (ble_scout::ble_scout_count() < ble_scan::MAX_PAIRED_BEACONS) {
    const uint8_t n = (uint8_t)ble_scout::ble_scout_count();
    const uint8_t mac[MAC_LEN] = {0xEE, 0xEE, 0xEE, 0x00, 0x00, n};
    CHECK(ble_scout::ble_scout_pair_window_start("tag", 10000, -45, t) ==
          P::ArmResult::OK);
    scan_advert(mac, -40, t + 10);
    CHECK(ble_scout::ble_scout_pair_window_status(t + 20).state == P::State::PAIRED);
    t += 100;
  }
  CHECK(ble_scout::ble_scout_pair_window_start("one more", 10000, -45, t) ==
        P::ArmResult::REGISTRY_FULL);
  ble_scan::PairedBeacon snap[ble_scan::MAX_PAIRED_BEACONS + 4];
  CHECK(ble_scout::ble_scout_registry_snapshot(snap, 3) == 3);
  CHECK(ble_scout::ble_scout_registry_snapshot(snap, ble_scan::MAX_PAIRED_BEACONS + 4) ==
        ble_scan::MAX_PAIRED_BEACONS);
  CHECK(ble_scout::ble_scout_registry_snapshot(nullptr, 4) == 0);
  std::printf("PASS test_full_registry_refuses_to_arm\n");
}

}  /* namespace */

int main() {
  test_init_is_idle();
  test_clamps();
  test_label_rule();
  test_offer_threshold_and_single_shot();
  test_failed_pair();
  test_expiry();
  test_cancel();
  test_millis_wrap();
  test_hex_codec();

  test_advert_kind_held_to_fleet_beacon_h();
  test_every_parsed_beacon_is_refused();
  test_chirps_are_refused();
  test_refusals_counted_exhaustively();
  test_near_misses_are_offerable();
  test_chirp_shape_held_to_roster_and_sender();
  test_scan_callbacks_pass_the_verdict();

  test_window_before_init_is_not_ready();
  test_window_pairs_first_strong_unpaired_advert();
  test_paired_beacon_does_not_consume_window();
  test_canceled_and_expired_windows_pair_nothing();
  test_bad_label_refused_at_start();
  test_unpair_marks_dirty();
  test_fleet_adverts_never_pair();
  test_paired_fleet_format_advert_leaves_window_alone();
  test_near_miss_adverts_still_pair();
  test_full_registry_refuses_to_arm();
  std::printf("\nALL BLE_SCOUT_PAIRING TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
