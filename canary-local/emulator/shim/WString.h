// canary-local/emulator/shim/WString.h — just enough Arduino String.
// The display tree uses String only for NVS credential round-trips
// (runtime_config.cpp); std::string wears the costume.
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

 private:
  std::string s_;
};
#endif  // __cplusplus
