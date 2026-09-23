// canary-local/emulator/shim/WString.h — just enough Arduino String.
// The display tree uses String for NVS credential round-trips
// (runtime_config.cpp) and, since the first-boot portal compiles here too,
// for net/provision.cpp's request args and its /scan JSON (+=); std::string
// wears the costume.
#pragma once

#ifdef __cplusplus
#include <string>
#include <string.h>

class String {
 public:
  String() {}
  String(const char* s) : s_(s ? s : "") {}
  String(const std::string& s) : s_(s) {}

  unsigned int length() const { return (unsigned int)s_.size(); }
  const char* c_str() const { return s_.c_str(); }
  bool isEmpty() const { return s_.empty(); }

  char operator[](unsigned int i) const {
    return i < s_.size() ? s_[i] : '\0';
  }

  bool equals(const char* o) const { return s_ == (o ? o : ""); }
  bool operator==(const char* o) const { return equals(o); }
  bool operator!=(const char* o) const { return !equals(o); }
  bool operator==(const String& o) const { return s_ == o.s_; }
  bool operator!=(const String& o) const { return s_ != o.s_; }

  String& operator=(const char* s) {
    s_ = s ? s : "";
    return *this;
  }
  String operator+(const char* o) const { return String(s_ + (o ? o : "")); }
  String& operator+=(const char* o) {
    s_ += o ? o : "";
    return *this;
  }
  String& operator+=(const String& o) {
    s_ += o.s_;
    return *this;
  }
  String& operator+=(char c) {
    s_ += c;
    return *this;
  }

 private:
  std::string s_;
};
#endif  // __cplusplus
