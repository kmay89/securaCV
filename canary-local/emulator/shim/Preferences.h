// canary-local/emulator/shim/Preferences.h — NVS as a browser-side store.
//
// The display keeps small truths in NVS: the splash's "we've met" flag,
// the bird's earned trust days, the glass settings, per-witness mutes,
// TOFU pins, runtime identity. The emulator backs the same API with an
// in-memory map mirrored to the page (localStorage), so the emulated
// device *remembers you* across visits exactly like the real one — and
// the scenario API can wipe it to stage a true first boot.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "WString.h"

class Preferences {
 public:
  Preferences() { ns_[0] = 0; }
  bool begin(const char* ns, bool readOnly = false);
  void end();

  // The exact getter/putter shapes the display tree uses.
  uint8_t getUChar(const char* key, uint8_t def = 0);
  size_t putUChar(const char* key, uint8_t v);
  // ESP32 stores a bool as a one-byte NVS value, so these are the UChar pair
  // with the cast folded in — not a separate storage shape. Inline here (not
  // out-of-line beside getUChar) because two translation units implement this
  // class for two link targets, and an alias belongs in neither of them twice.
  bool getBool(const char* key, bool def = false) {
    return getUChar(key, def ? 1 : 0) != 0;
  }
  size_t putBool(const char* key, bool v) { return putUChar(key, v ? 1 : 0); }
  uint16_t getUShort(const char* key, uint16_t def = 0);
  size_t putUShort(const char* key, uint16_t v);
  int16_t getShort(const char* key, int16_t def = 0);
  size_t putShort(const char* key, int16_t v);
  uint32_t getUInt(const char* key, uint32_t def = 0);
  size_t putUInt(const char* key, uint32_t v);
  // ESP32 Preferences exposes both spellings. Canary Vision uses ULong for
  // millisecond tuning values; keep it an alias of the same 32-bit NVS type.
  uint32_t getULong(const char* key, uint32_t def = 0) { return getUInt(key, def); }
  size_t putULong(const char* key, uint32_t v) { return putUInt(key, v); }
  // ESP32 stores a bool as a one-byte NVS value, so this is an alias rather
  // than a separate store — a value written as a bool reads back as a UChar
  // and vice versa, exactly as the real API behaves. The display tree's
  // Hallway switch (care/hallway.cpp) uses this spelling.
  bool getBool(const char* key, bool def = false) {
    return getUChar(key, def ? 1 : 0) != 0;
  }
  size_t putBool(const char* key, bool v) { return putUChar(key, v ? 1 : 0); }
  int32_t getInt(const char* key, int32_t def = 0);
  size_t putInt(const char* key, int32_t v);
  int64_t getLong64(const char* key, int64_t def = 0);
  size_t putLong64(const char* key, int64_t v);
  size_t getString(const char* key, char* out, size_t cap);
  String getString(const char* key, const String& def = String());
  size_t putString(const char* key, const char* v);
  size_t getBytes(const char* key, void* out, size_t cap);
  size_t putBytes(const char* key, const void* data, size_t len);
  size_t getBytesLength(const char* key);
  bool isKey(const char* key);
  bool remove(const char* key);
  bool clear();

 private:
  char ns_[32];
};
