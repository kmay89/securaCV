/*
 * SecuraCV Canary — BLE Scout advert classifier (repo sweep F36)
 *
 * The proximity pairing window (ble_scout_pairing.h, F27) pairs the first
 * strong advert from an unpaired device. A neighboring Canary advertises all
 * the time, so without a filter its advert could win the window instead of
 * the tag the owner is holding against this one. This header names the
 * fleet's own advert formats so the scan callback can keep them out of the
 * window: ble_scout_nimble.cpp reads an advert's manufacturer data once,
 * asks offerable(), and passes the answer to ble_scout_on_advert(), which
 * offers only offerable adverts to pairing::offer(). Tracking an already
 * paired device does not look at it.
 *
 * Two shapes are the fleet's. Both are the manufacturer data exactly as
 * NimBLE's getManufacturerData() returns it: the 2 company-id bytes first,
 * company 0xFFFF (little-endian on the wire).
 *
 *   presence beacon — 11 bytes (v1) or 13 bytes (v2), type byte [2] 0x10
 *                     (firmware/common/fleet_link/fleet_beacon.h). The
 *                     version byte [3] is NOT read: a beacon from a newer
 *                     schema at either length is still another Canary.
 *   Chirp           — 17 bytes, type byte [2] 0x01..0x05 (the canary-wap's
 *                     ble_chirp.h / ble_config.h sender, and the shape
 *                     fleet_roster_feed.cpp tables).
 *
 * Everything else is offerable: a different company, a different length, a
 * type outside those values, an advert with no manufacturer data at all.
 * That is the documented limit: a phone or a stranger's tag held close can
 * still win the window. Only a fleet-format advert is refused here.
 *
 * Not excluded (a choice, repo sweep F36): an advert that carries a SecuraCV
 * GATT service UUID but no fleet-format manufacturer data — the canary-wap
 * Bluetooth channel's SERVICE_UUID advert, or the legacy UUID-and-name
 * fallback the beacon code drops to when its advert data is refused. This
 * header reads manufacturer data only, so it stays a pure function of bytes
 * both trees share; a UUID rule would copy UUID strings from canary-wap-only
 * headers into this one with nothing to hold the copies together.
 *
 * test_ble_scout_pairing.cpp holds these constants to fleet_beacon.h (by
 * include) and to fleet_roster_feed.cpp's Chirp branch (by reading it).
 *
 * PURE and self-contained: <stdint.h> and <stddef.h> only, no NimBLE, no
 * Arduino, no fleet_link include path. Committed byte-identically in both
 * trees (firmware/canary/lib/securacv_ble_scan/src/ and the canary-wap
 * sketch folder); firmware/scripts/check_ble_scan_sync.sh fails on drift.
 */

#ifndef SECURACV_BLE_SCOUT_ADVERT_KIND_H
#define SECURACV_BLE_SCOUT_ADVERT_KIND_H

#include <stddef.h>
#include <stdint.h>

namespace ble_scout {
namespace advert_kind {

/* Company id 0xFFFF, as its two little-endian bytes [0] and [1]. */
constexpr uint8_t COMPANY_ID_LO = 0xFF;
constexpr uint8_t COMPANY_ID_HI = 0xFF;

/* Presence beacon (fleet_beacon.h): type byte and the two lengths. */
constexpr uint8_t BEACON_TYPE   = 0x10;
constexpr size_t  BEACON_LEN_V1 = 11;
constexpr size_t  BEACON_LEN_V2 = 13;

/* Chirp: one length and a range of type bytes. */
constexpr size_t  CHIRP_LEN        = 17;
constexpr uint8_t CHIRP_TYPE_FIRST = 0x01;
constexpr uint8_t CHIRP_TYPE_LAST  = 0x05;

enum class Kind : uint8_t {
  NOT_FLEET       = 0,   /* offerable to the pairing window */
  PRESENCE_BEACON = 1,
  CHIRP_ADVERT    = 2,
};

/* Classify one advert's manufacturer data. A null pointer or a length too
 * short to hold the type byte is NOT_FLEET (an advert with no manufacturer
 * data is passed as nullptr/0 or as an empty buffer). */
inline Kind classify(const uint8_t* mfg, size_t len) {
  if (mfg == nullptr || len < 3) return Kind::NOT_FLEET;
  if (mfg[0] != COMPANY_ID_LO || mfg[1] != COMPANY_ID_HI) return Kind::NOT_FLEET;
  const uint8_t type = mfg[2];
  if ((len == BEACON_LEN_V1 || len == BEACON_LEN_V2) && type == BEACON_TYPE) {
    return Kind::PRESENCE_BEACON;
  }
  if (len == CHIRP_LEN && type >= CHIRP_TYPE_FIRST && type <= CHIRP_TYPE_LAST) {
    return Kind::CHIRP_ADVERT;
  }
  return Kind::NOT_FLEET;
}

/* True when the pairing window may be offered this advert: anything that is
 * not in one of the fleet's own formats. */
inline bool offerable(const uint8_t* mfg, size_t len) {
  return classify(mfg, len) == Kind::NOT_FLEET;
}

inline const char* kind_name(Kind k) {
  switch (k) {
    case Kind::PRESENCE_BEACON: return "presence_beacon";
    case Kind::CHIRP_ADVERT:    return "chirp";
    case Kind::NOT_FLEET:       break;
  }
  return "not_fleet";
}

}  /* namespace advert_kind */
}  /* namespace ble_scout */

#endif /* SECURACV_BLE_SCOUT_ADVERT_KIND_H */
