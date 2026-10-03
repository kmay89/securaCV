// A strict JSON reader for host tests that must show an answer PARSES, the
// way a browser's JSON.parse() reads it (RFC 8259): no raw byte below 0x20
// inside a string, only the escapes JSON defines, no trailing text, no
// trailing commas. Strings come back decoded (\uXXXX as UTF-8), so a test can
// compare what a reader gets with what went in, byte for byte.
//
// Test-only; deliberately small. It is not the firmware's parser (that is
// ArduinoJson, which is laxer: it takes raw control bytes), so passing here is
// the stricter claim.

#ifndef CANARY_WAP_TESTS_JSON_STRICT_H
#define CANARY_WAP_TESTS_JSON_STRICT_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace json_strict {

struct Value {
  enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
  bool b = false;
  std::string text;                                  // a number's spelling, or a string's bytes
  std::vector<Value> items;                          // an array's
  std::vector<std::pair<std::string, Value>> keys;   // an object's, in order

  const Value* get(const std::string& k) const {
    for (const auto& kv : keys)
      if (kv.first == k) return &kv.second;
    return nullptr;
  }
};

class Reader {
 public:
  explicit Reader(const std::string& s) : s_(s) {}

  // True when the whole text is one JSON value (whitespace around it only).
  bool parse(Value* out) {
    pos_ = 0;
    ws();
    if (!value(out, 0)) return false;
    ws();
    return pos_ == s_.size();
  }

  size_t where() const { return pos_; }

 private:
  const std::string& s_;
  size_t pos_ = 0;

  bool eof() const { return pos_ >= s_.size(); }
  char peek() const { return eof() ? '\0' : s_[pos_]; }
  void ws() {
    while (!eof() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
  }
  bool lit(const char* w) {
    size_t n = 0;
    while (w[n]) ++n;
    if (s_.compare(pos_, n, w) != 0) return false;
    pos_ += n;
    return true;
  }

  static void utf8(uint32_t cp, std::string* out) {
    if (cp < 0x80) {
      out->push_back((char)cp);
    } else if (cp < 0x800) {
      out->push_back((char)(0xC0 | (cp >> 6)));
      out->push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back((char)(0xE0 | (cp >> 12)));
      out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back((char)(0x80 | (cp & 0x3F)));
    } else {
      out->push_back((char)(0xF0 | (cp >> 18)));
      out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back((char)(0x80 | (cp & 0x3F)));
    }
  }

  bool hex4(uint32_t* v) {
    if (pos_ + 4 > s_.size()) return false;
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[pos_++];
      r <<= 4;
      if (c >= '0' && c <= '9') r |= (uint32_t)(c - '0');
      else if (c >= 'a' && c <= 'f') r |= (uint32_t)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') r |= (uint32_t)(c - 'A' + 10);
      else return false;
    }
    *v = r;
    return true;
  }

  bool string(std::string* out) {
    if (peek() != '"') return false;
    ++pos_;
    for (;;) {
      if (eof()) return false;
      const unsigned char c = (unsigned char)s_[pos_++];
      if (c == '"') return true;
      if (c < 0x20) return false;                    // JSON.parse refuses a raw control byte
      if (c != '\\') {
        out->push_back((char)c);
        continue;
      }
      if (eof()) return false;
      const char e = s_[pos_++];
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(&cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00) {
            uint32_t lo = 0;
            if (!lit("\\u") || !hex4(&lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(cp, out);
          break;
        }
        default: return false;
      }
    }
  }

  bool number(std::string* out) {
    const size_t start = pos_;
    if (peek() == '-') ++pos_;
    if (peek() == '0') {
      ++pos_;
    } else if (peek() >= '1' && peek() <= '9') {
      while (peek() >= '0' && peek() <= '9') ++pos_;
    } else {
      return false;
    }
    if (peek() == '.') {
      ++pos_;
      if (!(peek() >= '0' && peek() <= '9')) return false;
      while (peek() >= '0' && peek() <= '9') ++pos_;
    }
    if (peek() == 'e' || peek() == 'E') {
      ++pos_;
      if (peek() == '+' || peek() == '-') ++pos_;
      if (!(peek() >= '0' && peek() <= '9')) return false;
      while (peek() >= '0' && peek() <= '9') ++pos_;
    }
    *out = s_.substr(start, pos_ - start);
    return true;
  }

  bool value(Value* v, int depth) {
    if (depth > 32) return false;
    ws();
    const char c = peek();
    if (c == '{') {
      v->kind = Value::Object;
      ++pos_;
      ws();
      if (peek() == '}') { ++pos_; return true; }
      for (;;) {
        ws();
        std::string k;
        if (!string(&k)) return false;
        ws();
        if (peek() != ':') return false;
        ++pos_;
        Value item;
        if (!value(&item, depth + 1)) return false;
        v->keys.emplace_back(k, item);
        ws();
        if (peek() == ',') { ++pos_; continue; }
        if (peek() == '}') { ++pos_; return true; }
        return false;
      }
    }
    if (c == '[') {
      v->kind = Value::Array;
      ++pos_;
      ws();
      if (peek() == ']') { ++pos_; return true; }
      for (;;) {
        Value item;
        if (!value(&item, depth + 1)) return false;
        v->items.push_back(item);
        ws();
        if (peek() == ',') { ++pos_; continue; }
        if (peek() == ']') { ++pos_; return true; }
        return false;
      }
    }
    if (c == '"') { v->kind = Value::String; return string(&v->text); }
    if (lit("true")) { v->kind = Value::Bool; v->b = true; return true; }
    if (lit("false")) { v->kind = Value::Bool; v->b = false; return true; }
    if (lit("null")) { v->kind = Value::Null; return true; }
    v->kind = Value::Number;
    return number(&v->text);
  }
};

inline bool parse(const std::string& s, Value* out) {
  Reader r(s);
  *out = Value();
  return r.parse(out);
}

}  // namespace json_strict

#endif  // CANARY_WAP_TESTS_JSON_STRICT_H
