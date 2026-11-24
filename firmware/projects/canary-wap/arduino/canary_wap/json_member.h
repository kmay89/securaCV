/**
 * @file json_member.h
 * @brief The canary-wap's one reader of a small JSON request body's keys
 *        (sweeps F462, F469): a member of the body's own object, never a
 *        key found with strstr() anywhere in the text.
 *
 * The CSI routes parse their small JSON bodies by hand (ArduinoJson is kept
 * out of csi_integration.cpp, csi_mqtt.cpp and csi_tune_lab.cpp). Each key
 * used to be found with strstr() of the key anywhere in the body, the value
 * read after the next ':', so a value spelled like a key was read as the
 * key: {"label":"privacy_ceiling","x":"p2"} stored the privacy ceiling P2
 * (POST /api/settings, F462), {"note":"event_id 7","event_id":9} dismissed
 * event 7 (POST /api/events/dismiss), {"note":"host","x":"evil.example"}
 * saved the broker host evil.example and {"x":{"host":"inner"},"host":
 * "outer"} saved inner (POST /api/mqtt/config), and a Tuning Lab POST read a
 * nested object's knob and the first of two (F469). Every key those routes
 * read is found here instead, where a JSON parser finds it.
 *
 * Pure and header-only: no NVS, no Arduino, no link line. csi_settings_nvs.h
 * includes it, so the settings stores and the Tuning Lab read through it as
 * they did; csi_mqtt.cpp includes it itself, and the three host suites that
 * link csi_mqtt.cpp alone over stubs/mqtt read it from there.
 * test_wap_tune_lab.cpp runs the reader and dismiss_event_id();
 * test_mqtt_reinit.cpp runs the MQTT config POST over it.
 */

#ifndef SECURACV_WAP_JSON_MEMBER_H
#define SECURACV_WAP_JSON_MEMBER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/** One member of a JSON object, as json_object_member() found it. */
struct JsonMember {
  const char* key;     /* the key's opening quote */
  const char* value;   /* the value's first byte */
  size_t value_len;    /* the value's bytes: a string with both quotes, an
                          object or array to its closing bracket, a literal
                          or number whole */
};

/** Deepest nesting json_object_member() reads; a deeper body is refused. */
constexpr size_t kJsonMaxDepth = 16;

/** What json_object_find() found. */
enum class JsonFind : uint8_t {
  kFound,        /* one own member has the key: *out is it */
  kAbsent,       /* the text is one object the reader takes, and no own member has the key */
  kTwice,        /* two or more own members have it (parsers disagree on which one wins) */
  kUnreadable,   /* the text is not one object the reader takes */
};

namespace json_member_detail {

inline bool ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
inline bool digit(char c) { return c >= '0' && c <= '9'; }
inline bool hex(char c) {
  return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* The string that opens at t[*i] == '"': on true *i is one past its closing
 * quote and *escaped says whether it holds an escape. False for one that
 * does not close inside t[0, len), holds a NUL, holds a raw control byte
 * (unless `raw_controls`: 0x01 to 0x1f taken as they are), or an escape
 * JSON does not have. */
inline bool skip_string(const char* t, size_t len, size_t* i, bool* escaped, bool raw_controls) {
  *escaped = false;
  size_t j = *i + 1;
  while (j < len) {
    const unsigned char c = (unsigned char)t[j];
    if (c == '"') {
      *i = j + 1;
      return true;
    }
    if (c == 0 || (c < 0x20 && !raw_controls)) return false;
    if (c == '\\') {
      *escaped = true;
      if (++j >= len) return false;
      switch (t[j]) {
        case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
          break;
        case 'u':
          for (int k = 0; k < 4; ++k) {
            if (++j >= len || !hex(t[j])) return false;
          }
          break;
        default:
          return false;
      }
    }
    ++j;
  }
  return false;
}

/* The literal or number at t[*i]: on true *i is one past it. */
inline bool skip_scalar(const char* t, size_t len, size_t* i) {
  static const char* const kLiterals[] = {"true", "false", "null"};
  for (const char* lit : kLiterals) {
    const size_t n = strlen(lit);
    if (len - *i >= n && memcmp(t + *i, lit, n) == 0) {
      *i += n;
      return true;
    }
  }
  size_t j = *i;
  if (j < len && t[j] == '-') ++j;
  if (j >= len || !digit(t[j])) return false;
  if (t[j] == '0') {
    ++j;
  } else {
    while (j < len && digit(t[j])) ++j;
  }
  if (j < len && t[j] == '.') {
    ++j;
    if (j >= len || !digit(t[j])) return false;
    while (j < len && digit(t[j])) ++j;
  }
  if (j < len && (t[j] == 'e' || t[j] == 'E')) {
    ++j;
    if (j < len && (t[j] == '+' || t[j] == '-')) ++j;
    if (j >= len || !digit(t[j])) return false;
    while (j < len && digit(t[j])) ++j;
  }
  *i = j;
  return true;
}

/* A value's bare bytes: a string's contents, anything else as it is. False
 * for a string that holds an escape: none of the values read here needs
 * one. */
inline bool member_text(const JsonMember& m, const char** p, size_t* n) {
  if (m.value == nullptr || m.value_len == 0) return false;
  if (m.value[0] == '"') {
    if (m.value_len < 2) return false;
    *p = m.value + 1;
    *n = m.value_len - 2;
    return memchr(*p, '\\', *n) == nullptr;
  }
  *p = m.value;
  *n = m.value_len;
  return true;
}

}  // namespace json_member_detail

/** Find the member `key` (spelled bare: "pet_mode") of the one JSON object
 *  text[0, len) holds, whitespace around it and nothing else. Only the
 *  object's own members count: a key spelled inside a string (escaped
 *  quotes and all) or a member of an object or array nested in it is not
 *  one. The whole text is read, never a byte past `len`, and it must be
 *  JSON: strings closed, no NUL and no raw control byte in one (with
 *  `raw_controls`, a raw 0x01 to 0x1f in a string is taken as it is: POST
 *  /api/mqtt/config has always stored one, and no quote or backslash moves
 *  for it), each escape whole (\" \\ \/ \b \f \n \r \t \uXXXX), brackets
 *  matched at most kJsonMaxDepth deep, every value a string, an object, an
 *  array, true, false, null or a number, commas and colons where they
 *  belong. A byte 0x80 and above in a string is taken as it is (UTF-8 is
 *  not validated; no word or bool read here can match one). An own
 *  member's key that holds an escape makes the text unreadable: none of the
 *  keys read here needs one, and a parser that decodes it could read
 *  {"host":...} as "host". *out is set only on kFound. */
inline JsonFind json_object_find(const char* text, size_t len, const char* key, JsonMember* out,
                                 bool raw_controls = false) {
  using namespace json_member_detail;
  if (text == nullptr || key == nullptr || out == nullptr) return JsonFind::kUnreadable;
  const size_t key_len = strlen(key);
  enum Expect { kValue, kKeyOrClose, kKey, kColon, kValueOrClose, kAfterValue };
  char open[kJsonMaxDepth];   /* '{' or '[' at each depth */
  size_t depth = 0;
  size_t i = 0;
  while (i < len && ws(text[i])) ++i;
  if (i >= len || text[i] != '{') return JsonFind::kUnreadable;
  open[depth++] = '{';
  ++i;
  Expect expect = kKeyOrClose;
  bool this_member = false;   /* the own member being read has the key */
  size_t key_at = 0;
  size_t value_at = 0;
  size_t found = 0;
  JsonMember hit = {nullptr, nullptr, 0};
  for (;;) {
    while (i < len && ws(text[i])) ++i;
    if (i >= len) return JsonFind::kUnreadable;   /* the text ends inside the object */
    const char c = text[i];
    bool value_done = false;
    if (expect == kKeyOrClose || expect == kKey) {
      if (c == '}' && expect == kKeyOrClose) {
        ++i;
        --depth;
        if (depth == 0) break;
        value_done = true;
      } else if (c == '"') {
        const size_t at = i;
        bool escaped = false;
        if (!skip_string(text, len, &i, &escaped, raw_controls)) return JsonFind::kUnreadable;
        if (depth == 1) {
          if (escaped) return JsonFind::kUnreadable;   /* an own key with an escape */
          this_member = (i - at - 2 == key_len) && memcmp(text + at + 1, key, key_len) == 0;
          key_at = at;
        }
        expect = kColon;
        continue;
      } else {
        return JsonFind::kUnreadable;
      }
    } else if (expect == kColon) {
      if (c != ':') return JsonFind::kUnreadable;
      ++i;
      expect = kValue;
      continue;
    } else if (expect == kValue || expect == kValueOrClose) {
      if (c == ']' && expect == kValueOrClose) {
        ++i;
        --depth;
        value_done = true;
      } else {
        if (depth == 1) value_at = i;
        if (c == '"') {
          bool escaped = false;
          if (!skip_string(text, len, &i, &escaped, raw_controls)) return JsonFind::kUnreadable;
          value_done = true;
        } else if (c == '{' || c == '[') {
          if (depth == kJsonMaxDepth) return JsonFind::kUnreadable;
          open[depth++] = c;
          ++i;
          expect = (c == '{') ? kKeyOrClose : kValueOrClose;
          continue;
        } else {
          if (!skip_scalar(text, len, &i)) return JsonFind::kUnreadable;
          value_done = true;
        }
      }
    } else {   /* kAfterValue */
      if (c == ',') {
        ++i;
        expect = (open[depth - 1] == '{') ? kKey : kValue;
        continue;
      }
      if ((c == '}' && open[depth - 1] == '{') || (c == ']' && open[depth - 1] == '[')) {
        ++i;
        --depth;
        if (depth == 0) break;
        value_done = true;
      } else {
        return JsonFind::kUnreadable;
      }
    }
    if (value_done) {
      if (depth == 1 && this_member) {
        ++found;
        hit.key = text + key_at;
        hit.value = text + value_at;
        hit.value_len = i - value_at;
        this_member = false;
      }
      expect = kAfterValue;
    }
  }
  /* The object closed: nothing but whitespace may follow it. */
  while (i < len && ws(text[i])) ++i;
  if (i != len) return JsonFind::kUnreadable;
  if (found == 0) return JsonFind::kAbsent;
  if (found > 1) return JsonFind::kTwice;
  *out = hit;
  return JsonFind::kFound;
}

/** json_object_find()'s kFound: true with *out the member, false (with *out
 *  untouched) when the text is not one object the reader takes, when no own
 *  member has the key, and when two do (parsers disagree on which one wins,
 *  so neither does). */
inline bool json_object_member(const char* text, size_t len, const char* key, JsonMember* out) {
  return json_object_find(text, len, key, out) == JsonFind::kFound;
}

/** The member's value as a bool: true or false, or the same in quotes (a
 *  value sent as a string reads as the bare one, as it always has). */
inline bool json_member_bool(const JsonMember& m, bool* out) {
  const char* p = nullptr;
  size_t n = 0;
  if (!json_member_detail::member_text(m, &p, &n)) return false;
  if (n == 4 && memcmp(p, "true", 4) == 0) {
    *out = true;
    return true;
  }
  if (n == 5 && memcmp(p, "false", 5) == 0) {
    *out = false;
    return true;
  }
  return false;
}

/** Where json_member_int() saturates, either side. */
constexpr int32_t kJsonIntMax = 999999999;

/** The member's value as an integer: a number with no fraction or
 *  exponent, or the same digits in quotes, saturated at kJsonIntMax either
 *  side (a longer number reads as kJsonIntMax, never as its first digits;
 *  every reader here clamps far inside that). */
inline bool json_member_int(const JsonMember& m, int32_t* out) {
  const char* p = nullptr;
  size_t n = 0;
  if (!json_member_detail::member_text(m, &p, &n)) return false;
  size_t i = 0;
  const bool negative = (n > 0 && p[0] == '-');
  if (negative) i = 1;
  if (i >= n) return false;
  /* Saturated at kJsonIntMax: once another digit would carry the value
   * past it, it is kJsonIntMax, and the digits after are still read, to
   * refuse one that is not a digit. It kept the first nine digits before
   * (1234567890 read as 123456789, a different number; the F462 review). */
  int32_t v = 0;
  for (; i < n; ++i) {
    if (!json_member_detail::digit(p[i])) return false;
    const int32_t d = (int32_t)(p[i] - '0');
    v = (v > (kJsonIntMax - d) / 10) ? kJsonIntMax : v * 10 + d;
  }
  *out = negative ? -v : v;
  return true;
}

/** The member's value as an unsigned 32-bit integer (F469): whole digits, 0
 *  to UINT32_MAX, or the same digits in quotes. A sign, a fraction, an
 *  exponent or a number past UINT32_MAX reads as none: never as its first
 *  digits, and never saturated, since the value names one thing (an event
 *  id: the space starts at 0xC0000000, past json_member_int()'s
 *  kJsonIntMax). */
inline bool json_member_u32(const JsonMember& m, uint32_t* out) {
  const char* p = nullptr;
  size_t n = 0;
  if (!json_member_detail::member_text(m, &p, &n) || n == 0) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) {
    if (!json_member_detail::digit(p[i])) return false;
    v = v * 10u + (uint64_t)(p[i] - '0');
    if (v > UINT32_MAX) return false;
  }
  *out = (uint32_t)v;
  return true;
}

/** True when the member's value is the string `word`, exactly (a string
 *  that holds an escape is none of the words read here). */
inline bool json_member_is(const JsonMember& m, const char* word) {
  if (word == nullptr || m.value == nullptr || m.value_len < 2 || m.value[0] != '"') return false;
  const char* p = nullptr;
  size_t n = 0;
  if (!json_member_detail::member_text(m, &p, &n)) return false;
  return n == strlen(word) && memcmp(p, word, n) == 0;
}

/* ── POST /api/events/dismiss (F469) ────────────────────────────────────
 * Its body is {"event_id": <id>}. The id was the first digits after the
 * first "event_id" anywhere in the body, quoted or not, so
 * {"note":"event_id 7","event_id":9} dismissed event 7, and an id past
 * 4294967295 read as strtoul()'s ULONG_MAX (4294967295 on the ESP32). */

/** What dismiss_event_id() read. */
enum class DismissRead : uint8_t {
  kId,        /* *id is the body's own "event_id" */
  kMissing,   /* the body has no "event_id" of its own */
  kTwice,     /* the body names "event_id" more than once */
  kNotJson,   /* the body is not one JSON object the reader takes */
  kNotId,     /* its "event_id" is not a whole number from 0 to 4294967295 */
};

/** The "event_id" member of the dismiss body text[0, len), read by
 *  json_member_u32(). *id is set only on kId. */
inline DismissRead dismiss_event_id(const char* text, size_t len, uint32_t* id) {
  JsonMember m = {nullptr, nullptr, 0};
  switch (json_object_find(text, len, "event_id", &m)) {
    case JsonFind::kFound:
      return json_member_u32(m, id) ? DismissRead::kId : DismissRead::kNotId;
    case JsonFind::kAbsent:
      return DismissRead::kMissing;
    case JsonFind::kTwice:
      return DismissRead::kTwice;
    case JsonFind::kUnreadable:
      break;
  }
  return DismissRead::kNotJson;
}

#endif /* SECURACV_WAP_JSON_MEMBER_H */
