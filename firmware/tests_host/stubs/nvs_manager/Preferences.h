/* Fake Preferences for test_nvs_manager_lock.cpp and test_nvs_store_result.cpp.
 * Like the real one, begin() on a started handle refuses (false) rather than
 * reopening it — the double begin a broken reopen would make is counted. The
 * test can make the next begin() fail, and reads each instance through
 * g_host_prefs (NvsManager's handle is private), in construction order.
 *
 * The two puts answer as the real ones do: the bytes written, or 0 on a
 * closed or read-only handle, a null key or value, or a zero length. The
 * test can make the next put report `put_reports` instead (0: NVS refused
 * it; fewer bytes than asked: a short write). Every put is counted, with the
 * mode of the handle it ran on. */
#ifndef STUB_NVS_MANAGER_PREFERENCES_H
#define STUB_NVS_MANAGER_PREFERENCES_H

#include <stddef.h>
#include <stdint.h>
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

  size_t putUInt(const char* key, uint32_t value) { return put(key, &value, sizeof(value)); }
  size_t putBytes(const char* key, const void* value, size_t len) { return put(key, value, len); }

  bool started = false;
  bool read_only = false;
  bool fail_next = false;     // the next begin() fails (a full or failed NVS)
  int double_begins = 0;      // begin() on a started handle
  int ends_while_closed = 0;  // end() on a handle that was not open
  int wrong_namespace = 0;    // begin() on any namespace but g_host_prefs_namespace

  long put_reports = -1;      // >= 0: what the next put reports, then back to -1
  int puts = 0;               // every put call
  int puts_open_rw = 0;       // ...made on a started, writable handle
  size_t last_put_len = 0;    // the length the last put was asked to write

 private:
  size_t put(const char* key, const void* value, size_t len) {
    puts++;
    last_put_len = len;
    const bool open_rw = started && !read_only;
    if (open_rw) puts_open_rw++;
    if (put_reports >= 0) {
      const size_t r = (size_t)put_reports;
      put_reports = -1;
      return r;
    }
    if (!open_rw || key == nullptr || value == nullptr || len == 0) return 0;
    return len;
  }
};

#endif
