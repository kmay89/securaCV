/**
 * @file device_name_rule.h
 * @brief What the canary-wap's friendly name may hold: no control byte
 *        (sweep F226).
 *
 * The name a person gives the device (POST /api/wifi/connect's
 * `device_name`, POST /api/device-name) is kept in setup_wizard.h
 * (s_device_name, NVS "dev_name") and read back by every surface that shows
 * it: GET /api/device-info (identity_json.h, escaped), POST
 * /api/device-name's own answer and GET /api/fleet/scan's self row (both
 * ArduinoJson), GET /api/fleet (fleet_selfreport.h, escaped), the mDNS TXT
 * record other Canaries list, and the serial banner. The routes bounded its
 * length (1 to 32 bytes) and wanted one byte to survive the mDNS-label
 * sanitizer, and took every other byte, NUL aside. ArduinoJson 7.4.1 writes
 * a control byte other than \b \f \n \r \t raw, which a browser's JSON.parse
 * refuses, so a name holding 0x01 made both ArduinoJson answers unparseable
 * (the Fleet sheet then said it could not reach the device).
 *
 * So no control byte (below 0x20) is ever the device's name, by one rule at
 * the two places s_device_name is written, rather than by escaping each
 * answer that reads it (the next reader would need it too):
 *
 *  - storable(): setup_wizard::set_device_name(), the one setter both
 *    rename routes call, refuses a name holding one (and an empty one, or
 *    one past kMaxLen bytes, as before). The BLE "device name" is a
 *    different name (bluetooth_channel's own setting), and no serial or BLE
 *    command sets this one.
 *  - usable(): the routes' own test, storable() and a letter or digit for
 *    the mDNS label (canary-<label>.local), so POST /api/device-name answers
 *    a refused name as invalid rather than as an NVS failure.
 *  - scrub_stored(): setup_wizard::init() turns each control byte of a name
 *    stored before this rule into a space, in RAM, as it reads it. The mDNS
 *    sanitizer maps a control byte and a space alike to '-', so the device's
 *    hostname is the one it already had; the NVS row keeps its old bytes
 *    until the next rename (init() opens the namespace read-only).
 *
 * Pure hosted C++ (no Arduino): host-tested in
 * tests_host/test_device_name.cpp, beside setup_wizard.h's init() and
 * set_device_name() cut verbatim; check_wap_json_answers.py rule J8 holds the
 * setter, the loader and both routes to it.
 */

#ifndef CANARY_WAP_DEVICE_NAME_RULE_H
#define CANARY_WAP_DEVICE_NAME_RULE_H

#include <stddef.h>

namespace device_name_rule {

/* The longest name kept, in bytes (setup_wizard::DEVICE_NAME_MAX). */
static constexpr size_t kMaxLen = 32;

/* True when `c` is a byte JSON may not carry raw (below 0x20). */
inline bool is_control(char c) { return (unsigned char)c < 0x20; }

/* True when `name` may be stored as the device's name: 1 to kMaxLen bytes,
 * none of them a control byte. */
inline bool storable(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;
  size_t n = 0;
  for (const char* p = name; *p; ++p) {
    if (++n > kMaxLen || is_control(*p)) return false;
  }
  return true;
}

/* True when `name` is storable and holds a letter or a digit: the bytes the
 * mDNS-label sanitizer keeps (it lowercases A-Z, keeps a-z, 0-9 and '-',
 * writes every other byte as '-' and trims the hyphens at both ends), so its
 * label is not empty. */
inline bool usable(const char* name) {
  if (!storable(name)) return false;
  for (const char* p = name; *p; ++p) {
    const char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
  }
  return false;
}

/* A name read back from NVS: each control byte becomes a space, in place.
 * Returns how many did. */
inline size_t scrub_stored(char* name) {
  size_t changed = 0;
  for (char* p = name; p != nullptr && *p; ++p) {
    if (is_control(*p)) {
      *p = ' ';
      ++changed;
    }
  }
  return changed;
}

}  // namespace device_name_rule

#endif  // CANARY_WAP_DEVICE_NAME_RULE_H
