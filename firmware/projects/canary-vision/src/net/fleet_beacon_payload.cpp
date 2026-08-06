// src/net/fleet_beacon_payload.cpp — what the fleet-link beacon says.
//
// See canary/net/fleet_beacon_payload.h for why this is separate from the
// carriers that transmit it. Short version: the bytes are built once here, so
// BLE and LAN multicast cannot drift apart, and disabling the BLE radio for
// flash budget does not take the payload down with it.
//
// This file touches no radio API at all — it reads live state and writes
// bytes. That is what keeps it compiled on every board regardless of which
// FEATURE_* carrier flags are set.

#include "canary/net/fleet_beacon_payload.h"

#include <string.h>

#include "canary/config.h"
#include "canary/witness.h"             // chain_length(), ready()
#include "canary/diagnostics.h"         // diag::get().level -> degraded flag
#include "canary/net/wifi_mgr.h"        // wifi_connected() -> on_wifi_sta flag
#include "identity/device_signature.h"  // fingerprint_hex() -> fp2

namespace canary::net {

namespace {

bool    s_fp_valid = false;   // fp2 captured from a ready witness identity
uint8_t s_fp0 = 0;
uint8_t s_fp1 = 0;

// Live detection state (fleet_beacon_note_detection).
bool     s_det_active = false;
uint8_t  s_det_class  = FLEET_BEACON_DETECT_NONE;
int      s_det_score  = -1;
uint32_t s_generation = 0;

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// fp2 = last 2 bytes of the pubkey fingerprint — the SAME 2 bytes the chirp
// carries at [15-16] and the "SCV-XXXX" name derives from — so a display
// correlates this beacon with the same witness it sees on the broker, and so
// every band resolves to one witness rather than one per radio. The witness
// exposes the fingerprint as the 16-char device_signature::fingerprint_hex();
// bytes 6..7 are hex chars [12..15]. Valid only once the witness identity is
// ready; retried on each build until then (never fabricated).
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

}  // namespace

size_t fleet_beacon_payload_build(uint8_t out[FLEET_BEACON_MFG_V2_LEN]) {
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

  uint8_t payload[FLEET_BEACON_PAYLOAD_V2_LEN];
  fleet_beacon_build_v2(payload, flags, /*battery_pct=*/-1, /*health_pct=*/-1,
                        chain_height, s_fp0, s_fp1,
                        s_det_active ? s_det_class : FLEET_BEACON_DETECT_NONE,
                        s_det_active ? s_det_score : -1);

  // The company id belongs in the buffer even though NimBLE prepends its own
  // on air: carrying the FULL blob is what lets a UDP datagram and a BLE
  // advert be the same bytes, parsed by the same function on the far side.
  out[0] = (uint8_t)(FLEET_BEACON_COMPANY_ID & 0xFF);
  out[1] = (uint8_t)((FLEET_BEACON_COMPANY_ID >> 8) & 0xFF);
  memcpy(&out[2], payload, FLEET_BEACON_PAYLOAD_V2_LEN);
  return FLEET_BEACON_MFG_V2_LEN;
}

uint32_t fleet_beacon_payload_generation() { return s_generation; }

void fleet_beacon_note_detection(bool active, uint8_t detect_class,
                                 int score_pct, uint32_t /*now*/) {
  // An edge is what a display must hear NOW: presence flipping, or the class
  // changing mid-presence. A mere confidence wobble rides the next refresh.
  const bool edge = (active != s_det_active) ||
                    (active && detect_class != s_det_class);
  s_det_active = active;
  s_det_class  = detect_class;
  s_det_score  = score_pct;
  if (edge) s_generation++;
}

}  // namespace canary::net
