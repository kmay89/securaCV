/* A fake SD card in RAM: the slice of Arduino-ESP32's SD / File API that
 * csi_event_log.cpp uses (cardType, exists, mkdir, open, remove, rename;
 * File's bool, size, seek, available, read, write, close), over a map of
 * path -> bytes. A test inserts, pulls and fills the card through
 * SD.files / SD.dirs / SD.present, and counts writes through SD.writes. */
#ifndef STUB_SD_FAKE_SD_H
#define STUB_SD_FAKE_SD_H

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
  int available() const { return data_ ? (int)(data_->size() - pos_) : 0; }
  int read() {
    if (!data_ || pos_ >= data_->size()) return -1;
    return (unsigned char)(*data_)[pos_++];
  }
  int read(uint8_t* buf, size_t n) {
    if (!data_) return -1;
    size_t left = data_->size() - pos_;
    if (n > left) n = left;
    memcpy(buf, data_->data() + pos_, n);
    pos_ += n;
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
    if (!present || files.count(a) == 0 || files.count(b) != 0) return false;
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
  if (!data_ || !writable_) return 0;
  fake_sd_instance().writes++;
  data_->append((const char*)buf, n);
  pos_ = data_->size();
  return n;
}

#endif
