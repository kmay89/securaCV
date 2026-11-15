// A host stand-in for the ArduinoJson 7.4.1 calls the canary-wap handlers a
// suite cuts out of canary_wap.ino make, for a suite whose Arduino.h is not
// stubs/bt's (sweep F303, F426: the Opera name routes and GET /api/logs over
// the real mesh_network.cpp on stubs/mesh_net, POST /api/vault/key over the
// vault's cut functions). stubs/bt/ArduinoJson.h is the same model for the
// Bluetooth harness; it includes stubs/bt's Arduino.h, which cannot sit
// beside stubs/mesh_net's. This file includes none: the suite includes its
// own Arduino.h first, whose String must take a `const char*` and give it
// back through c_str().
//
// It is not ArduinoJson. It keeps a value tree (null, bool, integers,
// floats, strings, objects in insertion order, arrays), reads a request body
// into it and writes it back out, the two ways the tests need to be exact:
//   - written, a string escapes `"` `\` \b \f \n \r \t, the seven
//     TextFormatter::writeChar() escapes, and writes every other byte as it
//     is, a control byte or a byte that is not UTF-8 included: what the
//     handlers' answers carry is what ArduinoJson would send;
//   - read, a string decodes the JSON escapes (\uXXXX, a surrogate pair
//     too, as UTF-8) and keeps every other byte as it is, a raw control
//     byte included, as ArduinoJson's parser does; deserializeJson() reads
//     one value and ignores what follows it, and answers EmptyInput for
//     nothing and InvalidInput for what is not JSON (a body cut short is
//     not JSON either; ArduinoJson names it IncompleteInput, also not Ok).
// A test reads an answer with json_strict.h, which is stricter than a
// browser's JSON.parse, never with this.
#ifndef CANARY_WAP_TESTS_ARDUINOJSON_TREE_H
#define CANARY_WAP_TESTS_ARDUINOJSON_TREE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace aj_tree {

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
  Node* find(const std::string& key) const {
    if (kind != kObject) return nullptr;
    for (const auto& m : members) {
      if (m.first == key) return m.second.get();
    }
    return nullptr;
  }
  // The member `key`, made when missing; a null value becomes an object.
  Node* member(const std::string& key) {
    if (kind == kNull) reset(kObject);
    if (kind != kObject) return nullptr;
    if (Node* n = find(key)) return n;
    members.emplace_back(key, std::unique_ptr<Node>(new Node));
    return members.back().second.get();
  }
};

inline void set(Node* n, bool v) {
  n->reset(Node::kBool);
  n->b = v;
}
inline void set(Node* n, const char* v) {
  if (v == nullptr) {
    n->reset(Node::kNull);
    return;
  }
  n->reset(Node::kString);
  n->s = v;
}
template <typename T>
typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value>::type
set(Node* n, T v) {
  if (std::is_signed<T>::value && (int64_t)v < 0) {
    n->reset(Node::kInt);
    n->i = (int64_t)v;
  } else {
    n->reset(Node::kUInt);
    n->u = (uint64_t)v;
  }
}
template <typename T>
typename std::enable_if<std::is_floating_point<T>::value>::type set(Node* n, T v) {
  n->reset(Node::kFloat);
  n->f = (double)v;
}
// A String (anything with c_str()).
template <typename S>
auto set(Node* n, const S& v) -> decltype((void)v.c_str()) {
  set(n, v.c_str());
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
      default: out->push_back(c); break;   // a control byte too, as ArduinoJson writes it
    }
  }
  out->push_back('"');
}

inline void write(const Node& n, std::string* out) {
  char num[40];
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
      for (size_t k = 0; k < n.items.size(); ++k) {
        if (k) out->push_back(',');
        write(*n.items[k], out);
      }
      out->push_back(']');
      break;
    }
  }
}

inline void skip_space(const char*& p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
}

inline void put_utf8(uint32_t cp, std::string* out) {
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

inline bool hex4(const char*& p, uint32_t* v) {
  uint32_t r = 0;
  for (int k = 0; k < 4; ++k) {
    const char c = *p++;
    r <<= 4;
    if (c >= '0' && c <= '9') r |= (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') r |= (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') r |= (uint32_t)(c - 'A' + 10);
    else return false;
  }
  *v = r;
  return true;
}

inline bool parse_string(const char*& p, std::string* out) {
  if (*p != '"') return false;
  ++p;
  for (;;) {
    const char c = *p++;
    if (c == '\0') return false;   // cut short
    if (c == '"') return true;
    if (c != '\\') {
      out->push_back(c);
      continue;
    }
    const char e = *p++;
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
        if (!hex4(p, &cp)) return false;
        if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u') {
          const char* q = p + 2;
          uint32_t lo = 0;
          if (hex4(q, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p = q;
          }
        }
        put_utf8(cp, out);
        break;
      }
      default: return false;
    }
  }
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
      if (!parse_value(p, n->member(key), depth + 1)) return false;
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
    set(n, true);
    return true;
  }
  if (strncmp(p, "false", 5) == 0) {
    p += 5;
    set(n, false);
    return true;
  }
  if (strncmp(p, "null", 4) == 0) {
    p += 4;
    n->reset(Node::kNull);
    return true;
  }
  if (*p == '-' || (*p >= '0' && *p <= '9')) {
    char* end = nullptr;
    const double v = strtod(p, &end);
    if (end == p) return false;
    n->reset(Node::kFloat);
    n->f = v;
    p = end;
    return true;
  }
  return false;
}

}  // namespace aj_tree

class JsonObject;
class JsonArray;

class JsonVariant {
 public:
  JsonVariant(aj_tree::Node* parent, const char* key) : parent_(parent), key_(key ? key : "") {}
  explicit JsonVariant(aj_tree::Node* node) : node_(node) {}

  template <typename T>
  JsonVariant& operator=(const T& v) {
    if (aj_tree::Node* n = made()) aj_tree::set(n, v);
    return *this;
  }
  // A char array, as a string (not a pointer to its first byte's value).
  JsonVariant& operator=(char* v) { return assign_text(v); }
  JsonVariant& operator=(const char* v) { return assign_text(v); }

  // const char* p = doc["k"]: the string, or nullptr for any other value.
  operator const char*() const { return text(); }
  const char* operator|(const char* fallback) const {
    const char* t = text();
    return t ? t : fallback;
  }
  bool operator|(bool fallback) const {
    const aj_tree::Node* n = found();
    return n && n->kind == aj_tree::Node::kBool ? n->b : fallback;
  }

  template <typename T>
  T to();

 private:
  JsonVariant& assign_text(const char* v) {
    if (aj_tree::Node* n = made()) aj_tree::set(n, v);
    return *this;
  }
  const char* text() const {
    const aj_tree::Node* n = found();
    return n && n->kind == aj_tree::Node::kString ? n->s.c_str() : nullptr;
  }
  const aj_tree::Node* found() const {
    if (node_) return node_;
    return parent_ ? parent_->find(key_) : nullptr;
  }
  aj_tree::Node* made() {
    if (!node_ && parent_) node_ = parent_->member(key_);
    return node_;
  }

  aj_tree::Node* parent_ = nullptr;
  std::string key_;
  aj_tree::Node* node_ = nullptr;
};

class JsonObject {
 public:
  JsonObject() = default;
  explicit JsonObject(aj_tree::Node* n) : n_(n) {}
  JsonVariant operator[](const char* key) const { return JsonVariant(n_, key); }

 private:
  aj_tree::Node* n_ = nullptr;
};

class JsonArray {
 public:
  JsonArray() = default;
  explicit JsonArray(aj_tree::Node* n) : n_(n) {}
  template <typename T>
  T add();

 private:
  aj_tree::Node* n_ = nullptr;
};

template <>
inline JsonArray JsonVariant::to<JsonArray>() {
  aj_tree::Node* n = made();
  if (n) n->reset(aj_tree::Node::kArray);
  return JsonArray(n);
}
template <>
inline JsonObject JsonVariant::to<JsonObject>() {
  aj_tree::Node* n = made();
  if (n) n->reset(aj_tree::Node::kObject);
  return JsonObject(n);
}
template <>
inline JsonObject JsonArray::add<JsonObject>() {
  if (!n_ || n_->kind != aj_tree::Node::kArray) return JsonObject();
  n_->items.emplace_back(new aj_tree::Node);
  aj_tree::Node* n = n_->items.back().get();
  n->reset(aj_tree::Node::kObject);
  return JsonObject(n);
}

class JsonDocument {
 public:
  JsonDocument() : root_(new aj_tree::Node) {}
  JsonDocument(const JsonDocument&) = delete;
  JsonDocument& operator=(const JsonDocument&) = delete;
  JsonVariant operator[](const char* key) { return JsonVariant(root_.get(), key); }
  const aj_tree::Node& root() const { return *root_; }
  aj_tree::Node& root() { return *root_; }

 private:
  std::unique_ptr<aj_tree::Node> root_;
};

class DeserializationError {
 public:
  enum Code { Ok, EmptyInput, IncompleteInput, InvalidInput, NoMemory, TooDeep };
  DeserializationError(Code c) : code_(c) {}  // NOLINT: implicit, as ArduinoJson's
  friend bool operator==(const DeserializationError& e, Code c) { return e.code_ == c; }
  friend bool operator!=(const DeserializationError& e, Code c) { return e.code_ != c; }

 private:
  Code code_;
};

inline DeserializationError deserializeJson(JsonDocument& doc, const char* input) {
  doc.root().reset(aj_tree::Node::kNull);
  if (input == nullptr) return DeserializationError::EmptyInput;
  const char* p = input;
  aj_tree::skip_space(p);
  if (*p == '\0') return DeserializationError::EmptyInput;
  if (!aj_tree::parse_value(p, &doc.root(), 0)) {
    doc.root().reset(aj_tree::Node::kNull);
    return DeserializationError::InvalidInput;
  }
  return DeserializationError::Ok;
}

inline std::string aj_tree_text(const JsonDocument& doc) {
  std::string text;
  aj_tree::write(doc.root(), &text);
  return text;
}
inline size_t measureJson(const JsonDocument& doc) { return aj_tree_text(doc).size(); }
template <typename S>
inline size_t serializeJson(const JsonDocument& doc, S& out) {
  const std::string text = aj_tree_text(doc);
  out = S(text.c_str());
  return text.size();
}

#endif  // CANARY_WAP_TESTS_ARDUINOJSON_TREE_H
