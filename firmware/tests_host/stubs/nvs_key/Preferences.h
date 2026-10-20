/* Preferences, and the NVS behind it, for test_nvs_load_key.cpp (sweep F360).
 *
 * One namespace (host_key_nvs().ns, which the test sets to NVS_MAIN_NS) and
 * its blobs, kept across handles as NVS keeps them across a reboot. nvs.h
 * beside this file answers IDF's nvs_open() / nvs_get_blob() from the same
 * store. As Arduino-ESP32 2.0.17's Preferences does: a read-only begin() of
 * a namespace never created fails (IDF answers ESP_ERR_NVS_NOT_FOUND), a
 * read-write one creates it, and begin() on a begun handle refuses;
 * getBytesLength() answers 0 for a missing key and for a failed lookup
 * alike; getBytes() asks the length first, answers 0 when that is 0 or more
 * than the buffer holds, and answers 0 when the read fails, leaving the
 * buffer as it was; putBytes() stores and answers the length, or 0 on a
 * closed or read-only handle, a null key or value, or a zero length.
 *
 * The faults, each in force until the test clears it:
 *   ro_open_fault  a read-only open fails with this error (IDF's answers it,
 *                  Preferences' begin() fails); read-write opens still work,
 *                  as after a transient fault a later write can survive.
 *   length_fault   the blob length lookup fails with this error (IDF's
 *                  nvs_get_blob() size query answers it; getBytesLength()
 *                  and so getBytes() answer 0). ESP_ERR_NVS_TYPE_MISMATCH
 *                  models a "privkey" stored as another type.
 *   begin_fails    Preferences' begin() refuses every open (IDF unaffected).
 *   read           what getBytes() does once its length lookup has
 *                  succeeded: Ok; Fails (answers 0, the buffer untouched:
 *                  the F360 case); Garbles (fills the buffer with 0xA5,
 *                  then answers 0, as IDF fills it before its data CRC check
 *                  refuses); Short (copies 31 bytes, answers 31).
 * Every call counts. failed_begins is the error line Arduino's begin() logs
 * for each open it refuses; prefs_open is the handles begun and not ended. */
#ifndef STUB_NVS_KEY_PREFERENCES_H
#define STUB_NVS_KEY_PREFERENCES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "esp_err.h"

struct HostKeyNvs {
  enum class Read { Ok, Fails, Garbles, Short };

  std::string ns;                                      // the one namespace modeled
  bool ns_exists = false;
  std::map<std::string, std::vector<uint8_t>> blobs;   // that namespace's blobs

  esp_err_t ro_open_fault = ESP_OK;
  esp_err_t length_fault = ESP_OK;
  bool begin_fails = false;
  Read read = Read::Ok;

  int ro_begins = 0, rw_begins = 0, failed_begins = 0, double_begins = 0;
  int wrong_namespace = 0;
  int reads = 0, puts = 0, puts_stored = 0;
  int prefs_open = 0;   // Preferences handles begun and not yet ended
  int idf_opens = 0, idf_rw_opens = 0, idf_closes = 0, idf_open_handles = 0;

  // The store alone (no faults, no counts): what a reboot would find.
  bool same_store(const HostKeyNvs& o) const {
    return ns_exists == o.ns_exists && blobs == o.blobs;
  }
  void clear_faults() {
    ro_open_fault = ESP_OK;
    length_fault = ESP_OK;
    begin_fails = false;
    read = Read::Ok;
  }
};
inline HostKeyNvs& host_key_nvs() {
  static HostKeyNvs nvs;
  return nvs;
}

class Preferences {
 public:
  bool begin(const char* name, bool readOnly = false) {
    HostKeyNvs& n = host_key_nvs();
    if (started_) { n.double_begins++; return false; }
    if (readOnly) n.ro_begins++; else n.rw_begins++;
    if (name == nullptr || n.ns != name) { n.wrong_namespace++; n.failed_begins++; return false; }
    if (n.begin_fails || (readOnly && n.ro_open_fault != ESP_OK) ||
        (readOnly && !n.ns_exists)) {
      n.failed_begins++;               /* log_e("nvs_open failed: ...") */
      return false;
    }
    if (!readOnly) n.ns_exists = true;
    n.prefs_open++;
    started_ = true;
    read_only_ = readOnly;
    return true;
  }
  void end() {
    if (started_) host_key_nvs().prefs_open--;
    started_ = false;
    read_only_ = false;
  }

  size_t getBytesLength(const char* key) {
    HostKeyNvs& n = host_key_nvs();
    if (!started_ || key == nullptr || n.length_fault != ESP_OK) return 0;
    auto it = n.blobs.find(key);
    return it == n.blobs.end() ? 0 : it->second.size();
  }
  size_t getBytes(const char* key, void* buf, size_t maxLen) {
    HostKeyNvs& n = host_key_nvs();
    n.reads++;
    const size_t len = getBytesLength(key);
    if (len == 0 || buf == nullptr || maxLen == 0) return len;
    if (len > maxLen) return 0;
    switch (n.read) {
      case HostKeyNvs::Read::Ok:
        memcpy(buf, n.blobs[key].data(), len);
        return len;
      case HostKeyNvs::Read::Fails:
        return 0;
      case HostKeyNvs::Read::Garbles:
        memset(buf, 0xA5, len);
        return 0;
      case HostKeyNvs::Read::Short:
        memcpy(buf, n.blobs[key].data(), len - 1);
        return len - 1;
    }
    return 0;
  }
  size_t putBytes(const char* key, const void* value, size_t len) {
    HostKeyNvs& n = host_key_nvs();
    n.puts++;
    if (!started_ || read_only_ || key == nullptr || value == nullptr || len == 0) return 0;
    const uint8_t* p = static_cast<const uint8_t*>(value);
    n.blobs[key].assign(p, p + len);
    n.puts_stored++;
    return len;
  }

 private:
  bool started_ = false;
  bool read_only_ = false;
};

#endif
