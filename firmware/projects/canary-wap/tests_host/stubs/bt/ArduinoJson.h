/* Host stand-in for <ArduinoJson.h> (the canary-wap Bluetooth channel host
 * harness, test_bluetooth_commands_wap.cpp, sweep F235): the calls
 * bluetooth_api.h's handlers make, so the harness runs the REAL handlers
 * over the real bluetooth_channel.cpp. ArduinoJson itself is not on the host.
 *
 * It is not ArduinoJson. It keeps a value tree (null, bool, signed and
 * unsigned integers, floats, strings, objects in insertion order, arrays),
 * reads a request body into it and writes it back out:
 *   - doc["k"] = v creates the member on assignment; a read of a missing
 *     member creates nothing, and is<JsonVariant>() is false for it;
 *   - as<bool>() reads null false, a number true when it is not zero, and a
 *     string, object or array true; as<integer>() reads a number that fits
 *     the type, else 0, and aborts on a string (ArduinoJson 7.4.1 parses
 *     one, "5" reading 5; that parse is not modeled, so a test that posts a
 *     quoted number fails loudly instead of pinning a 0); as<const char*>()
 *     reads a string, else nullptr;
 *   - a string is written with the seven escapes check_wap_json_answers.py
 *     counts (\" \\ \b \f \n \r \t), any other byte as it is;
 *   - deserializeJson() reads one value and stops after it, and answers
 *     EmptyInput for nothing, InvalidInput for what is not JSON.
 * What the tests here hold is the handlers' choice of answer (its status
 * line, its success, error and message), not ArduinoJson's edge cases;
 * check_wap_json_answers.py and check_wap_loop_commands.py hold the rest. */
#ifndef STUB_BT_ARDUINOJSON_H
#define STUB_BT_ARDUINOJSON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Arduino.h"   // String

namespace host_json {

struct Node {
  enum Kind { kNull, kBool, kInt, kUInt, kFloat, kString, kObject, kArray };
  Kind kind = kNull;
  bool b = false;
  int64_t i = 0;
  uint64_t u = 0;
  double f = 0;
  std::string s;
  std::vector<std::pair<std::string, std::unique_ptr<Node>>> members;
  std::vector<std::unique_ptr<Node>> items;

  void reset(Kind k) {
    kind = k;
    b = false;
    i = 0;
    u = 0;
    f = 0;
    s.clear();
    members.clear();
    items.clear();
  }
  Node* find(const char* key) const {
    if (kind != kObject || key == nullptr) return nullptr;
    for (const auto& m : members) {
      if (m.first == key) return m.second.get();
    }
    return nullptr;
  }
  // The member `key`, made when missing; a null value becomes an object.
  Node* member(const char* key) {
    if (kind == kNull) reset(kObject);
    if (kind != kObject || key == nullptr) return nullptr;
    if (Node* n = find(key)) return n;
    members.emplace_back(key, std::unique_ptr<Node>(new Node));
    return members.back().second.get();
  }
};

inline void assign(Node* n, bool v) {
  if (!n) return;
  n->reset(Node::kBool);
  n->b = v;
}
inline void assign(Node* n, const char* v) {
  if (!n) return;
  if (v == nullptr) {
    n->reset(Node::kNull);
    return;
  }
  n->reset(Node::kString);
  n->s = v;
}
inline void assign(Node* n, const String& v) { assign(n, v.c_str()); }
template <typename T>
typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value>::type
assign(Node* n, T v) {
  if (!n) return;
  if (std::is_signed<T>::value && (int64_t)v < 0) {
    n->reset(Node::kInt);
    n->i = (int64_t)v;
  } else {
    n->reset(Node::kUInt);
    n->u = (uint64_t)v;
  }
}
template <typename T>
typename std::enable_if<std::is_floating_point<T>::value>::type assign(Node* n, T v) {
  if (!n) return;
  n->reset(Node::kFloat);
  n->f = (double)v;
}

inline void write_string(const std::string& s, std::string* out) {
  out->push_back('"');
  for (char c : s) {
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\b': out->append("\\b"); break;
      case '\f': out->append("\\f"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default: out->push_back(c); break;
    }
  }
  out->push_back('"');
}
inline void write(const Node& n, std::string* out) {
  char num[32];
  switch (n.kind) {
    case Node::kNull: out->append("null"); break;
    case Node::kBool: out->append(n.b ? "true" : "false"); break;
    case Node::kInt:
      snprintf(num, sizeof num, "%lld", (long long)n.i);
      out->append(num);
      break;
    case Node::kUInt:
      snprintf(num, sizeof num, "%llu", (unsigned long long)n.u);
      out->append(num);
      break;
    case Node::kFloat:
      snprintf(num, sizeof num, "%.9g", n.f);
      out->append(num);
      break;
    case Node::kString: write_string(n.s, out); break;
    case Node::kObject: {
      out->push_back('{');
      bool first = true;
      for (const auto& m : n.members) {
        if (!first) out->push_back(',');
        first = false;
        write_string(m.first, out);
        out->push_back(':');
        write(*m.second, out);
      }
      out->push_back('}');
      break;
    }
    case Node::kArray: {
      out->push_back('[');
      bool first = true;
      for (const auto& it : n.items) {
        if (!first) out->push_back(',');
        first = false;
        write(*it, out);
      }
      out->push_back(']');
      break;
    }
  }
}

inline void skip_space(const char*& p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
}
inline bool parse_string(const char*& p, std::string* out) {
  if (*p != '"') return false;
  ++p;
  out->clear();
  while (*p && *p != '"') {
    char c = *p++;
    if ((unsigned char)c < 0x20) return false;
    if (c != '\\') {
      out->push_back(c);
      continue;
    }
    c = *p++;
    switch (c) {
      case '"': out->push_back('"'); break;
      case '\\': out->push_back('\\'); break;
      case '/': out->push_back('/'); break;
      case 'b': out->push_back('\b'); break;
      case 'f': out->push_back('\f'); break;
      case 'n': out->push_back('\n'); break;
      case 'r': out->push_back('\r'); break;
      case 't': out->push_back('\t'); break;
      case 'u': {
        unsigned cp = 0;
        for (int k = 0; k < 4; ++k) {
          const char h = *p++;
          cp <<= 4;
          if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
          else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
          else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
          else return false;
        }
        if (cp < 0x80) {
          out->push_back((char)cp);
        } else if (cp < 0x800) {
          out->push_back((char)(0xC0 | (cp >> 6)));
          out->push_back((char)(0x80 | (cp & 0x3F)));
        } else {
          out->push_back((char)(0xE0 | (cp >> 12)));
          out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
          out->push_back((char)(0x80 | (cp & 0x3F)));
        }
        break;
      }
      default: return false;
    }
  }
  if (*p != '"') return false;
  ++p;
  return true;
}
inline bool parse_value(const char*& p, Node* n, int depth) {
  if (depth > 10) return false;
  skip_space(p);
  if (*p == '{') {
    ++p;
    n->reset(Node::kObject);
    skip_space(p);
    if (*p == '}') {
      ++p;
      return true;
    }
    for (;;) {
      skip_space(p);
      std::string key;
      if (!parse_string(p, &key)) return false;
      skip_space(p);
      if (*p++ != ':') return false;
      Node* m = n->member(key.c_str());
      if (!parse_value(p, m, depth + 1)) return false;
      skip_space(p);
      if (*p == ',') {
        ++p;
        continue;
      }
      if (*p == '}') {
        ++p;
        return true;
      }
      return false;
    }
  }
  if (*p == '[') {
    ++p;
    n->reset(Node::kArray);
    skip_space(p);
    if (*p == ']') {
      ++p;
      return true;
    }
    for (;;) {
      n->items.emplace_back(new Node);
      if (!parse_value(p, n->items.back().get(), depth + 1)) return false;
      skip_space(p);
      if (*p == ',') {
        ++p;
        continue;
      }
      if (*p == ']') {
        ++p;
        return true;
      }
      return false;
    }
  }
  if (*p == '"') {
    n->reset(Node::kString);
    return parse_string(p, &n->s);
  }
  if (strncmp(p, "true", 4) == 0) {
    p += 4;
    assign(n, true);
    return true;
  }
  if (strncmp(p, "false", 5) == 0) {
    p += 5;
    assign(n, false);
    return true;
  }
  if (strncmp(p, "null", 4) == 0) {
    p += 4;
    n->reset(Node::kNull);
    return true;
  }
  if (*p == '-' || (*p >= '0' && *p <= '9')) {
    const char* start = p;
    bool real = false;
    if (*p == '-') ++p;
    if (!(*p >= '0' && *p <= '9')) return false;
    while ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' ||
           (*p == '-' && (p[-1] == 'e' || p[-1] == 'E'))) {
      if (*p == '.' || *p == 'e' || *p == 'E') real = true;
      ++p;
    }
    const std::string text(start, p);
    if (real) {
      n->reset(Node::kFloat);
      n->f = strtod(text.c_str(), nullptr);
    } else if (text[0] == '-') {
      n->reset(Node::kInt);
      n->i = strtoll(text.c_str(), nullptr, 10);
    } else {
      n->reset(Node::kUInt);
      n->u = strtoull(text.c_str(), nullptr, 10);
    }
    return true;
  }
  return false;
}

}  // namespace host_json

class JsonObject;
class JsonArray;

class JsonVariant {
 public:
  JsonVariant() = default;
  explicit JsonVariant(host_json::Node* node) : node_(node) {}
  JsonVariant(host_json::Node* parent, const char* key) : parent_(parent), key_(key ? key : "") {}

  template <typename T>
  JsonVariant& operator=(const T& v) {
    host_json::assign(made(), v);
    return *this;
  }
  template <typename T>
  T to();
  // is<JsonVariant>(): whether the value is there (the handlers' "the
  // request names this key").
  template <typename T>
  bool is() const {
    static_assert(std::is_same<T, JsonVariant>::value, "the stand-in answers is<JsonVariant>() only");
    return found() != nullptr;
  }
  template <typename T>
  T as() const {
    return as_impl(static_cast<T*>(nullptr));
  }
  // `doc["k"] | "default"`: the string when the value is one, else the
  // default (the BLE provisioning harness reads its CREDS so).
  const char* operator|(const char* def) const {
    const char* v = as<const char*>();
    return v ? v : def;
  }

 private:
  host_json::Node* found() const {
    if (node_) return node_;
    return parent_ ? parent_->find(key_.c_str()) : nullptr;
  }
  host_json::Node* made() {
    if (!node_ && parent_) node_ = parent_->member(key_.c_str());
    return node_;
  }
  bool as_impl(bool*) const {
    const host_json::Node* n = found();
    if (!n) return false;
    switch (n->kind) {
      case host_json::Node::kNull: return false;
      case host_json::Node::kBool: return n->b;
      case host_json::Node::kInt: return n->i != 0;
      case host_json::Node::kUInt: return n->u != 0;
      case host_json::Node::kFloat: return n->f != 0;
      default: return true;
    }
  }
  const char* as_impl(const char**) const {
    const host_json::Node* n = found();
    return n && n->kind == host_json::Node::kString ? n->s.c_str() : nullptr;
  }
  template <typename T>
  typename std::enable_if<std::is_integral<T>::value, T>::type as_impl(T*) const {
    const host_json::Node* n = found();
    if (!n) return 0;
    long double v = 0;
    switch (n->kind) {
      case host_json::Node::kBool: return n->b ? 1 : 0;
      case host_json::Node::kInt: v = (long double)n->i; break;
      case host_json::Node::kUInt: v = (long double)n->u; break;
      case host_json::Node::kFloat: v = (long double)n->f; break;
      case host_json::Node::kString:
        // ArduinoJson 7.4.1 parses a string here (VariantData::asIntegral()
        // calls parseNumber()): {"tx_power":"5"} reads 5. The stand-in does
        // not model that parse, so a test that reaches it stops here rather
        // than pin a 0 the device would not answer.
        fprintf(stderr, "ArduinoJson stand-in: as<integer>() of the string \"%s\" is not modeled "
                        "(ArduinoJson 7.4.1 parses it)\n", n->s.c_str());
        abort();
      default: return 0;
    }
    if (v < (long double)std::numeric_limits<T>::min() || v > (long double)std::numeric_limits<T>::max()) {
      return 0;
    }
    return (T)v;
  }

  host_json::Node* node_ = nullptr;
  host_json::Node* parent_ = nullptr;
  std::string key_;
};

class JsonObject {
 public:
  JsonObject() = default;
  explicit JsonObject(host_json::Node* n) : n_(n) {}
  JsonVariant operator[](const char* key) const { return JsonVariant(n_, key); }

 private:
  host_json::Node* n_ = nullptr;
};

class JsonArray {
 public:
  JsonArray() = default;
  explicit JsonArray(host_json::Node* n) : n_(n) {}
  template <typename T>
  T add();
  // ArduinoJson 7.4.1's add(value): the value appended, a string or a number
  // (GET /api/bluetooth's services_missing, sweep F260).
  template <typename T>
  bool add(const T& v) {
    if (!n_ || n_->kind != host_json::Node::kArray) return false;
    n_->items.emplace_back(new host_json::Node);
    host_json::assign(n_->items.back().get(), v);
    return true;
  }

 private:
  host_json::Node* n_ = nullptr;
};

template <>
inline JsonObject JsonVariant::to<JsonObject>() {
  host_json::Node* n = made();
  if (n) n->reset(host_json::Node::kObject);
  return JsonObject(n);
}
template <>
inline JsonArray JsonVariant::to<JsonArray>() {
  host_json::Node* n = made();
  if (n) n->reset(host_json::Node::kArray);
  return JsonArray(n);
}
template <>
inline JsonObject JsonArray::add<JsonObject>() {
  if (!n_ || n_->kind != host_json::Node::kArray) return JsonObject();
  n_->items.emplace_back(new host_json::Node);
  host_json::Node* n = n_->items.back().get();
  n->reset(host_json::Node::kObject);
  return JsonObject(n);
}

class JsonDocument {
 public:
  JsonDocument() : root_(new host_json::Node) {}
  JsonDocument(const JsonDocument&) = delete;
  JsonDocument& operator=(const JsonDocument&) = delete;
  JsonVariant operator[](const char* key) { return JsonVariant(root_.get(), key); }
  const host_json::Node& root() const { return *root_; }
  host_json::Node& root() { return *root_; }

 private:
  std::unique_ptr<host_json::Node> root_;
};

class DeserializationError {
 public:
  enum Code { Ok, EmptyInput, IncompleteInput, InvalidInput, NoMemory, TooDeep };
  DeserializationError(Code c) : code_(c) {}  // NOLINT: implicit, as ArduinoJson's
  Code code() const { return code_; }
  explicit operator bool() const { return code_ != Ok; }
  friend bool operator==(const DeserializationError& e, Code c) { return e.code_ == c; }
  friend bool operator!=(const DeserializationError& e, Code c) { return e.code_ != c; }

 private:
  Code code_;
};

inline DeserializationError deserializeJson(JsonDocument& doc, const char* input) {
  doc.root().reset(host_json::Node::kNull);
  if (input == nullptr) return DeserializationError::EmptyInput;
  const char* p = input;
  host_json::skip_space(p);
  if (*p == '\0') return DeserializationError::EmptyInput;
  if (!host_json::parse_value(p, &doc.root(), 0)) {
    doc.root().reset(host_json::Node::kNull);
    return DeserializationError::InvalidInput;
  }
  return DeserializationError::Ok;
}

// The sized read (ArduinoJson reads at most `size` bytes; the stand-in reads
// a copy of them, so a NUL among them ends the text as it ends the copy).
inline DeserializationError deserializeJson(JsonDocument& doc, const char* input, size_t size) {
  if (input == nullptr) return deserializeJson(doc, nullptr);
  const std::string copy(input, size);
  return deserializeJson(doc, copy.c_str());
}

inline size_t measureJson(const JsonDocument& doc) {
  std::string text;
  host_json::write(doc.root(), &text);
  return text.size();
}
// Into a buffer, as ArduinoJson 7.4.1's serialize() into a char array: at
// most `size` bytes, a NUL after them only when one fits (not counted), and
// the bytes written returned.
inline size_t serializeJson(const JsonDocument& doc, char* out, size_t size) {
  std::string text;
  host_json::write(doc.root(), &text);
  const size_t n = text.size() < size ? text.size() : size;
  if (n) memcpy(out, text.data(), n);
  if (n < size) out[n] = '\0';
  return n;
}
inline size_t serializeJson(const JsonDocument& doc, String& out) {
  std::string text;
  host_json::write(doc.root(), &text);
  out = String(text);
  return text.size();
}

#endif
