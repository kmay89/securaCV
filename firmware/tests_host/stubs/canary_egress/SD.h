/* A fake SD card in RAM: the slice of Arduino-ESP32's SD / File API that
 * the canary's csi_event_log.cpp uses (exists, mkdir, open, remove, rename;
 * File's bool, size, seek, position, available, read, write, close), over a
 * map of path -> bytes (the canary-wap's tests_host has the same fake,
 * without position()). A test inserts, pulls and fills the card through
 * SD.files / SD.dirs / SD.present, counts writes through SD.writes, makes
 * every write fail through SD.fail_writes, cuts the next write short through
 * SD.short_write_next (a power cut mid-line), cuts the next line's last
 * bytes through SD.short_by_next (a line that lands without its newline),
 * and makes rename() fail
 * through SD.fail_renames (a rewrite whose commit step fails). */
#ifndef STUB_CANARY_EGRESS_SD_H
#define STUB_CANARY_EGRESS_SD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <map>
#include <set>
#include <string>

enum sdcard_type_t { CARD_NONE = 0, CARD_MMC, CARD_SD, CARD_SDHC, CARD_UNKNOWN };

#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"

/* Bytes read through any File, so a test can see a path read nothing. */
inline size_t& fake_sd_bytes_read() {
  static size_t n = 0;
  return n;
}

class File {
 public:
  File() = default;
  File(std::string* data, bool writable) : data_(data), writable_(writable) {}
  explicit operator bool() const { return data_ != nullptr; }
  size_t size() const { return data_ ? data_->size() : 0; }
  bool seek(size_t pos) {
    if (!data_ || pos > data_->size()) return false;
    pos_ = pos;
    return true;
  }
  size_t position() const { return pos_; }
  int available() const { return data_ ? (int)(data_->size() - pos_) : 0; }
  int read() {
    if (!data_ || pos_ >= data_->size()) return -1;
    fake_sd_bytes_read()++;
    return (unsigned char)(*data_)[pos_++];
  }
  int read(uint8_t* buf, size_t n) {
    if (!data_) return -1;
    size_t left = data_->size() - pos_;
    if (n > left) n = left;
    memcpy(buf, data_->data() + pos_, n);
    pos_ += n;
    fake_sd_bytes_read() += n;
    return (int)n;
  }
  size_t write(const uint8_t* buf, size_t n);
  void close() { data_ = nullptr; }

 private:
  std::string* data_ = nullptr;
  bool writable_ = false;
  size_t pos_ = 0;
};

class FakeSD {
 public:
  bool present = false;
  std::map<std::string, std::string> files;
  std::set<std::string> dirs;
  size_t writes = 0;   // every byte-changing call: write, mkdir, remove, rename, open-for-write
  bool fail_writes = false;   // File::write writes nothing (a card that refuses writes)
  size_t short_write_next = 0;  // >0: the next File::write writes only this many bytes, once
  size_t short_by_next = 0;     // >0: the next File::write longer than this writes that many bytes fewer, once
  bool fail_renames = false;  // rename() fails, changing nothing

  sdcard_type_t cardType() const { return present ? CARD_SDHC : CARD_NONE; }
  bool exists(const char* p) const {
    return present && (files.count(p) != 0 || dirs.count(p) != 0);
  }
  bool mkdir(const char* p) {
    if (!present) return false;
    writes++;
    dirs.insert(p);
    return true;
  }
  File open(const char* p, const char* mode) {
    if (!present) return File();
    if (strcmp(mode, FILE_READ) == 0) {
      auto it = files.find(p);
      if (it == files.end()) return File();
      return File(&it->second, false);
    }
    writes++;
    std::string& d = files[p];
    if (strcmp(mode, FILE_WRITE) == 0) d.clear();
    File f(&d, true);
    f.seek(d.size());
    return f;
  }
  bool remove(const char* p) {
    if (!present) return false;
    writes++;
    return files.erase(p) != 0;
  }
  bool rename(const char* a, const char* b) {
    if (!present || fail_renames || files.count(a) == 0 || files.count(b) != 0) return false;
    writes++;
    files[b] = files[a];
    files.erase(a);
    return true;
  }
};

inline FakeSD& fake_sd_instance() {
  static FakeSD sd;
  return sd;
}
#define SD fake_sd_instance()

inline size_t File::write(const uint8_t* buf, size_t n) {
  if (!data_ || !writable_ || fake_sd_instance().fail_writes) return 0;
  if (fake_sd_instance().short_write_next > 0) {
    if (n > fake_sd_instance().short_write_next) n = fake_sd_instance().short_write_next;
    fake_sd_instance().short_write_next = 0;
  }
  if (fake_sd_instance().short_by_next > 0 && n > fake_sd_instance().short_by_next) {
    n -= fake_sd_instance().short_by_next;
    fake_sd_instance().short_by_next = 0;
  }
  fake_sd_instance().writes++;
  data_->append((const char*)buf, n);
  pos_ = data_->size();
  return n;
}

#endif
