// Host-test stub for ESP32 Preferences (NVS). Persistence is a no-op —
// the audio tests cover detection logic, not NVS.
#ifndef HOST_STUB_PREFERENCES_H
#define HOST_STUB_PREFERENCES_H
class Preferences {
 public:
  bool begin(const char*, bool = false) { return false; }
  void end() {}
  bool putBool(const char*, bool) { return true; }
  bool getBool(const char*, bool def = false) { return def; }
  unsigned short getUShort(const char*, unsigned short def = 0) { return def; }
  size_t putUShort(const char*, unsigned short) { return 2; }
};
#endif
