// src/net/fleet_beacon_adv.cpp — fleet-link BLE presence beacon (advertise-only).
//
// A minimal NimBLE advertiser that broadcasts the canonical 11-byte fleet-link
// presence beacon (fleet_beacon.h) as manufacturer data. A canary-display
// resolves this witness directly over BLE — broker-free and WiFi-free. No GATT
// server and no scan response: these devices carry no service UUIDs to
// preserve, so the beacon IS the primary (and only) advert. Flash cost stays
// minimal — advertise-only, no services, no characteristics.
//
// Cross-core: NimBLE-Arduino 1.4.x (arduino-esp32 core 2.x, this project) and
// 2.x (core 3.x, canary-sense) differ on init()/setAdvertisementData() return
// types and isInitialized(); the ESP_ARDUINO_VERSION_MAJOR split mirrors
// firmware/projects/canary-display/src/net/chirp_scan.cpp.

#include "canary/net/fleet_beacon_adv.h"

#if defined(FEATURE_FLEET_BEACON) && FEATURE_FLEET_BEACON

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <fleet_beacon.h>              // canonical wire format (common/fleet_link)
#include <string>
#include <cstring>

#include "canary/config.h"
#include "canary/runtime_config.h"     // canary::cfg::get().device_id (GAP name)
#include "canary/witness.h"            // chain_length(), ready()
#include "canary/diagnostics.h"        // diag::get().level -> degraded flag
#include "canary/net/wifi_mgr.h"       // wifi_connected() -> on_wifi_sta flag
#include "canary/log.h"
#include "identity/device_signature.h" // fingerprint_hex() -> fp2

namespace canary::net {

namespace {

constexpr uint32_t REFRESH_MS = 5000;   // rebuild the manufacturer data cadence

bool     s_inited   = false;   // NimBLE up + advert started
bool     s_failed   = false;   // bring-up failed — no-op for the rest of this boot
bool     s_fp_valid = false;   // fp2 captured from a ready witness identity
uint8_t  s_fp0      = 0;
uint8_t  s_fp1      = 0;
uint32_t s_next_ms  = 0;

// Live detection state (fleet_beacon_note_detection). While active, the
// advert carries the ALERT flag plus class + confidence — the v2 beacon.
bool     s_det_active = false;
uint8_t  s_det_class  = FLEET_BEACON_DETECT_NONE;
int      s_det_score  = -1;

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// fp2 = last 2 bytes of the pubkey fingerprint — the SAME 2 bytes the chirp
// carries at [15-16] and the "SCV-XXXX" name derives from — so the display
// correlates this BLE beacon with the same witness it sees on the broker. The
// witness exposes the fingerprint as the 16-char device_signature::
// fingerprint_hex(); bytes 6..7 are hex chars [12..15]. Valid only once the
// witness identity is ready; retried each refresh until then (never fabricated).
void capture_fp() {
  if (s_fp_valid) return;
  if (!canary::witness::ready()) return;
  const char* fp = device_signature::fingerprint_hex();
  if (!fp || strlen(fp) < 16) return;
  const int h12 = hex_nibble(fp[12]), h13 = hex_nibble(fp[13]);
  const int h14 = hex_nibble(fp[14]), h15 = hex_nibble(fp[15]);
  if (h12 < 0 || h13 < 0 || h14 < 0 || h15 < 0) return;
  s_fp0 = (uint8_t)((h12 << 4) | h13);
  s_fp1 = (uint8_t)((h14 << 4) | h15);
  s_fp_valid = true;
}

// Build the 13-byte v2 manufacturer blob from live state and (re)install it as
// the primary advert. Honest sourcing — no fabricated fields:
//   flags        degraded (diag level != Normal), on_wifi_sta (STA link up),
//                alert (presence FSM holds a live detection — the one witness
//                type with a clean signal for it). tamper / mic_muted: no
//                clean signal at this layer — left clear rather than guessed.
//   battery_pct  0xFF unknown — mains-powered witness, no battery gauge.
//   health_pct   0xFF unknown — no 0..100 self-test score at this layer.
//   chain_height witness chain length (low 16 bits ride the wire).
//   fp2          real fingerprint suffix once captured, else 0x00 sentinel.
//   detect       class token + confidence percent while the alert flag is up
//                (NONE / unknown otherwise) — never anything identifying.
void publish_adv(uint32_t now) {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!adv) return;

  capture_fp();

  uint8_t flags = 0;
  if (canary::diag::get().level != canary::diag::Level::Normal) {
    flags |= FLEET_BEACON_FLAG_DEGRADED;
  }
  if (canary::net::wifi_connected()) {
    flags |= FLEET_BEACON_FLAG_ON_WIFI_STA;
  }
  if (s_det_active) {
    flags |= FLEET_BEACON_FLAG_ALERT;
  }

  const uint32_t chain_height = canary::witness::chain_length();

  // Payload = bytes [2..12]; battery/health unknown (-1 -> 0xFF via the builder).
  uint8_t payload[FLEET_BEACON_PAYLOAD_V2_LEN];
  fleet_beacon_build_v2(payload, flags, /*battery_pct=*/-1, /*health_pct=*/-1,
                        chain_height, s_fp0, s_fp1,
                        s_det_active ? s_det_class : FLEET_BEACON_DETECT_NONE,
                        s_det_active ? s_det_score : -1);

  // Full manufacturer blob a scanner sees = company id (LE) + 11-byte payload.
  // NimBLE's setManufacturerData takes the bytes INCLUDING the company id;
  // fleet_beacon_parse / beacon_parse.h validate all 13 on the receiving side
  // (and still accept the 11-byte v1 blob other witness types emit).
  uint8_t mfg[FLEET_BEACON_MFG_V2_LEN];
  mfg[0] = (uint8_t)(FLEET_BEACON_COMPANY_ID & 0xFF);
  mfg[1] = (uint8_t)((FLEET_BEACON_COMPANY_ID >> 8) & 0xFF);
  memcpy(&mfg[2], payload, FLEET_BEACON_PAYLOAD_V2_LEN);

  NimBLEAdvertisementData advData;
  advData.setManufacturerData(std::string((const char*)mfg, sizeof(mfg)));

  // stop -> set -> start refreshes the on-air payload deterministically on BOTH
  // NimBLE majors: 1.4.x's setAdvertisementData only latches for the next
  // start(), while 2.x updates live. setAdvertisementData is called as a bare
  // statement so the return-type drift (void on 1.4.x, bool on 2.x) is
  // irrelevant. Advertise-only, so nothing else to preserve across the restart.
  adv->stop();
  adv->setAdvertisementData(advData);
  adv->start();

  s_next_ms = now + REFRESH_MS;
}

}  // namespace

void fleet_beacon_begin(uint32_t now) {
  if (s_inited || s_failed) return;

  // Bring NimBLE up exactly once, under the device id as the GAP name (matches
  // the mDNS advert). init() returns void on 1.4.x and bool on 2.x — call it as
  // a statement and confirm via isInitialized() on 2.x (1.4.x lacks it, so
  // there a non-null getAdvertising() is the signal) — same guard style as
  // chirp_scan.cpp's ble_up().
  NimBLEDevice::init(canary::cfg::get().device_id);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!NimBLEDevice::isInitialized()) {
    s_failed = true;
    canary::log_line("BEACON", "NimBLE init failed — presence beacon off this boot.");
    return;
  }
#endif
  if (!NimBLEDevice::getAdvertising()) {
    s_failed = true;
    canary::log_line("BEACON", "No advertiser handle — presence beacon off this boot.");
    return;
  }

  s_inited = true;
  publish_adv(now);
  canary::log_line("BEACON",
                   "Fleet-link presence beacon advertising (BLE, broker-free).");
}

void fleet_beacon_tick(uint32_t now) {
  if (!s_inited) return;
  if ((int32_t)(now - s_next_ms) < 0) return;
  publish_adv(now);
}

void fleet_beacon_note_detection(bool active, uint8_t detect_class,
                                 int score_pct, uint32_t now) {
  // An edge is what a display must hear NOW: presence flipping, or the class
  // changing mid-presence. A mere confidence wobble rides the next refresh.
  const bool edge = (active != s_det_active) ||
                    (active && detect_class != s_det_class);
  s_det_active = active;
  s_det_class  = detect_class;
  s_det_score  = score_pct;
  if (edge && s_inited) publish_adv(now);
}

}  // namespace canary::net

#else  // FEATURE_FLEET_BEACON off — no-op stubs (per-board size-guard off switch)

#include <stdint.h>
namespace canary::net {
void fleet_beacon_begin(uint32_t) {}
void fleet_beacon_tick(uint32_t) {}
void fleet_beacon_note_detection(bool, uint8_t, int, uint32_t) {}
}  // namespace canary::net

#endif  // FEATURE_FLEET_BEACON
