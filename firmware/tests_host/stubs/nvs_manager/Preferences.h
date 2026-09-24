/* Fake Preferences for test_nvs_manager_lock.cpp. Like the real one, begin()
 * on a started handle refuses (false) rather than reopening it — the double
 * begin a broken reopen would make is counted. The test can make the next
 * begin() fail, and reads each instance through g_host_prefs (NvsManager's
 * handle is private), in construction order. */
#ifndef STUB_NVS_MANAGER_PREFERENCES_H
#define STUB_NVS_MANAGER_PREFERENCES_H

#include <string.h>

class Preferences;
inline Preferences* g_host_prefs[4] = {nullptr, nullptr, nullptr, nullptr};
inline int g_host_prefs_count = 0;
inline const char* g_host_prefs_namespace = nullptr;  // the test sets NVS_MAIN_NS

class Preferences {
 public:
  Preferences() {
    if (g_host_prefs_count < 4) g_host_prefs[g_host_prefs_count++] = this;
  }

  bool begin(const char* name, bool readOnly = false) {
    if (started) { double_begins++; return false; }
    if (fail_next) { fail_next = false; return false; }
    if (name == nullptr || g_host_prefs_namespace == nullptr ||
        strcmp(name, g_host_prefs_namespace) != 0) {
      wrong_namespace++;
    }
    started = true;
    read_only = readOnly;
    return true;
  }

  void end() {
    if (!started) ends_while_closed++;
    started = false;
    read_only = false;
  }

  bool started = false;
  bool read_only = false;
  bool fail_next = false;     // the next begin() fails (a full or failed NVS)
  int double_begins = 0;      // begin() on a started handle
  int ends_while_closed = 0;  // end() on a handle that was not open
  int wrong_namespace = 0;    // begin() on any namespace but g_host_prefs_namespace
};

#endif
