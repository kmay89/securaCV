/*
 * SecuraCV Canary — the device prefix a nearby Canary's name carries
 * (sweep F405)
 *
 * ble_nearby.h counts an advert that carries the SecuraCV service UUID as a
 * Canary. When that advert carries neither a chirp nor a fleet-link beacon,
 * the scan takes the peer's four-character device prefix from its advertised
 * name, "SCV-XXXX" (ble_manager.h builds it from BLE_DEVICE_NAME_PREFIX and
 * the last four hex digits of the key fingerprint). The name is whatever the
 * advertiser chose: any radio in range can put the service UUID beside a name
 * such as `SCV-12"4` or `SCV-<!--`. The scan used to copy those four bytes
 * unchecked into NearbyCanary::deviceIdPrefix, and buildNearbyJson() writes
 * that field into GET /api/nearby's hand-built JSON between quotes, so a
 * quote or a backslash broke the answer and a control byte made it a text
 * JSON.parse refuses.
 *
 * nearby_prefix_from_name() takes the four bytes only when all four are
 * ASCII hex digits (0-9, a-f, A-F, as advertised: a genuine name is
 * uppercase, a chirp's or a beacon's prefix lowercase); any other name
 * yields nothing, and the scan counts the advert as a non-Canary as it
 * always did for a name without the prefix. So every byte that reaches
 * deviceIdPrefix is a hex digit, whichever path wrote it: the chirp and the
 * beacon parsers print theirs with "%02x".
 *
 * Header-only and pure (no Arduino, no NimBLE), so the host suite drives it
 * directly (tests_host/test_bluetooth_commands_wap.cpp), and
 * firmware/scripts/check_wap_json_answers.py (J11) records it as the one
 * writer of a nearby prefix that is not a hex printf.
 */

#ifndef BLE_NEARBY_PREFIX_H
#define BLE_NEARBY_PREFIX_H

#include <stddef.h>
#include <string.h>

namespace ble_nearby {

// The characters the prefix holds, and the bytes `out` needs for them and
// the NUL (NearbyCanary::deviceIdPrefix is char[5]).
static const size_t kNamePrefixDigits = 4;

// An ASCII hex digit. Not isxdigit(): that one takes an int that must be an
// unsigned char's value or EOF, and a name's bytes are plain chars here.
inline bool prefix_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// The device prefix of an advertised name of `len` bytes (embedded NULs
// counted: NimBLE hands the name over as a std::string): the four bytes
// right after `want` (BLE_DEVICE_NAME_PREFIX), into out[0..3] with out[4] =
// '\0', when the name starts with `want` and those four bytes are all ASCII
// hex digits. Bytes after them are not read, as the scan never read them.
// Otherwise returns false with out[0] = '\0'. `out` holds at least
// kNamePrefixDigits + 1 bytes.
inline bool nearby_prefix_from_name(const char* name, size_t len, const char* want, char* out) {
  out[0] = '\0';
  if (name == nullptr || want == nullptr) return false;
  const size_t w = strlen(want);
  if (len < w + kNamePrefixDigits || memcmp(name, want, w) != 0) return false;
  for (size_t k = 0; k < kNamePrefixDigits; ++k) {
    if (!prefix_hex_digit(name[w + k])) return false;
  }
  memcpy(out, name + w, kNamePrefixDigits);
  out[kNamePrefixDigits] = '\0';
  return true;
}

}  // namespace ble_nearby

#endif  // BLE_NEARBY_PREFIX_H
