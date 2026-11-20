/* Host fake of the Arduino core surface arduino-esp32 2.0.17's WebServer
 * library compiles against (firmware/third_party/WebServer-2.0.17-securacv),
 * for test_webserver_boundary.cpp. String is std::string underneath and keeps
 * only the members the library calls, with Arduino's semantics for each
 * (indexOf returns -1, substring clamps, toInt reads a leading integer).
 * `a + b` returns a StringSumHelper& as Arduino's does, because WebServer.cpp
 * passes such sums to a function taking String&. */
#ifndef STUB_WEBSERVER_2017_ARDUINO_H
#define STUB_WEBSERVER_2017_ARDUINO_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cctype>
#include <string>

typedef bool boolean;
typedef const char* PGM_P;
typedef const void* PGM_VOID_P;
#define PROGMEM
#define PSTR(s) (s)
#define strcpy_P strcpy
#define strlen_P strlen
#define memccpy_P memccpy

class __FlashStringHelper;
#define FPSTR(p) (reinterpret_cast<const __FlashStringHelper*>(p))
#define F(s) FPSTR(s)

class String {
 public:
  String() = default;
  String(const char* c) : s_(c ? c : "") {}  // NOLINT: implicit, as Arduino's
  String(const __FlashStringHelper* f) : s_(f ? reinterpret_cast<const char*>(f) : "") {}  // NOLINT
  String(const std::string& s) : s_(s) {}  // NOLINT
  explicit String(char c) : s_(1, c) {}
  explicit String(int v) : s_(std::to_string(v)) {}
  explicit String(unsigned int v) : s_(std::to_string(v)) {}
  explicit String(long v) : s_(std::to_string(v)) {}
  explicit String(unsigned long v) : s_(std::to_string(v)) {}

  const char* c_str() const { return s_.c_str(); }
  unsigned int length() const { return static_cast<unsigned int>(s_.size()); }
  char charAt(unsigned int i) const { return i < s_.size() ? s_[i] : '\0'; }
  char operator[](unsigned int i) const { return charAt(i); }
  const char* begin() const { return s_.c_str(); }
  const char* end() const { return s_.c_str() + s_.size(); }

  int indexOf(char c, unsigned int from = 0) const { return pos(s_.find(c, from)); }
  int indexOf(const String& t, unsigned int from = 0) const { return pos(s_.find(t.s_, from)); }
  int indexOf(const char* t, unsigned int from = 0) const { return pos(s_.find(t, from)); }
  String substring(unsigned int from) const { return substring(from, length()); }
  String substring(unsigned int from, unsigned int to) const {
    if (from > to) std::swap(from, to);
    if (from >= s_.size()) return String();
    to = std::min<unsigned int>(to, length());
    return String(s_.substr(from, to - from));
  }
  bool startsWith(const String& p) const { return s_.compare(0, p.s_.size(), p.s_) == 0 && s_.size() >= p.s_.size(); }
  bool endsWith(const String& p) const {
    return s_.size() >= p.s_.size() && s_.compare(s_.size() - p.s_.size(), p.s_.size(), p.s_) == 0;
  }
  bool equalsIgnoreCase(const String& o) const {
    if (s_.size() != o.s_.size()) return false;
    for (size_t i = 0; i < s_.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(s_[i])) != std::tolower(static_cast<unsigned char>(o.s_[i]))) return false;
    }
    return true;
  }
  void replace(const String& from, const String& to) {
    if (from.s_.empty()) return;
    size_t at = 0;
    while ((at = s_.find(from.s_, at)) != std::string::npos) {
      s_.replace(at, from.s_.size(), to.s_);
      at += to.s_.size();
    }
  }
  void trim() {
    size_t a = 0, b = s_.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s_[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s_[b - 1]))) --b;
    s_ = s_.substr(a, b - a);
  }
  long toInt() const { return strtol(s_.c_str(), nullptr, 10); }
  bool equalsConstantTime(const String& o) const { return s_ == o.s_; }

  String& operator+=(const String& o) { s_ += o.s_; return *this; }
  String& operator+=(const char* o) { s_ += (o ? o : ""); return *this; }
  String& operator+=(char c) { s_ += c; return *this; }
  String& operator+=(const __FlashStringHelper* f) { return *this += reinterpret_cast<const char*>(f); }
  bool operator==(const String& o) const { return s_ == o.s_; }
  bool operator==(const char* o) const { return s_ == (o ? o : ""); }
  bool operator!=(const String& o) const { return s_ != o.s_; }
  bool operator!=(const char* o) const { return !(*this == o); }

  const std::string& str() const { return s_; }

 private:
  static int pos(size_t p) { return p == std::string::npos ? -1 : static_cast<int>(p); }
  std::string s_;
};

// Arduino's concatenation: the left operand becomes a temporary
// StringSumHelper that each `+` appends to and returns by reference.
class StringSumHelper : public String {
 public:
  StringSumHelper(const String& s) : String(s) {}  // NOLINT
  StringSumHelper(const char* p) : String(p) {}  // NOLINT
};
inline StringSumHelper& operator+(const StringSumHelper& lhs, const String& rhs) {
  StringSumHelper& a = const_cast<StringSumHelper&>(lhs);
  a += rhs;
  return a;
}
inline StringSumHelper& operator+(const StringSumHelper& lhs, const char* rhs) {
  StringSumHelper& a = const_cast<StringSumHelper&>(lhs);
  a += rhs;
  return a;
}
inline StringSumHelper& operator+(const StringSumHelper& lhs, char rhs) {
  StringSumHelper& a = const_cast<StringSumHelper&>(lhs);
  a += rhs;
  return a;
}
inline StringSumHelper& operator+(const StringSumHelper& lhs, const __FlashStringHelper* rhs) {
  StringSumHelper& a = const_cast<StringSumHelper&>(lhs);
  a += rhs;
  return a;
}

inline unsigned long millis() { return 0; }
inline void delay(unsigned long) {}
inline void yield() {}

#endif
