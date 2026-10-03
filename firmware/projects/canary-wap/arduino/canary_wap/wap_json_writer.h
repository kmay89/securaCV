/**
 * @file wap_json_writer.h
 * @brief A clamp-safe JSON text writer for the canary-wap's answers that carry
 *        bytes the device did not choose (sweep F211, F212).
 *
 * Some of the sketch's JSON texts carry strings the device did not choose:
 * the fleet scan's cache holds what other devices advertise over mDNS (up to
 * 255 bytes per TXT value, any bytes), and GET /api/device-info carries the
 * device name a person typed (the routes in front of
 * setup_wizard::set_device_name() bound its length, not its bytes).
 * ArduinoJson's sized serializeJson() cut the cache when it did not fit and
 * left it unterminated, and the device-info answer wrote the name with an
 * unescaped %s, so a `"` in a name broke it.
 *
 * This writer is the one place those texts are spelled:
 *
 *  - str() writes a quoted JSON string and escapes every byte JSON requires:
 *    `"` and `\` as `\"` and `\\`, backspace, form feed, newline, return and
 *    tab as their two-byte escapes, and every other byte below 0x20 as
 *    `\u00XX` (ArduinoJson 7.4.1 writes those last ones raw, which a strict
 *    parser such as a browser's JSON.parse refuses). Every other byte goes as
 *    it is. So one byte costs at most kEscapeMax (6) bytes.
 *  - A writer with `replace_controls` set writes those other control bytes as
 *    `\ufffd` instead (the replacement character, the same six bytes), for a
 *    text that ArduinoJson parses and serializes again before anyone reads
 *    it: ArduinoJson decodes `\u00XX` back to the raw byte and writes it raw
 *    (fleet_scan_cache.h's reader, handle_fleet_scan(), does exactly that),
 *    while `\ufffd` decodes to U+FFFD, which it writes as valid UTF-8.
 *  - Every write is whole or not at all: a write that does not fit before the
 *    reserve sets `overflow` and writes nothing, and every write after it is
 *    refused too, so the text is always a run of whole writes followed by a
 *    NUL, never a cut one.
 *  - A builder that must not answer a partial text measures it first (a
 *    writer begun with begin_measure() counts the bytes and writes none) and
 *    writes it into a buffer of that length (identity_json.h); one that keeps
 *    what fits rolls a refused piece back with rollback() (fleet_scan_cache.h
 *    keeps the adverts that fit).
 *
 * Callers spell every call `wap_json::name(...)`, never `using namespace
 * wap_json;`: the sketch's translation unit opens with Arduino.h, and a
 * using-directive puts these names next to its globals (`typedef bool
 * boolean;` made the first identity_json.h's unqualified `boolean(...)`
 * ambiguous, so neither ESP32 build compiled it). The host tests include
 * tests_host/arduino_globals.h, Arduino.h's global names, before the header
 * they test, so such a collision fails make.
 *
 * Pure hosted C++ (no Arduino, ESP-IDF or heap): any task, and host-tested
 * through its builders (tests_host/test_fleet_scan_cache.cpp,
 * test_identity_json.cpp).
 */

#ifndef CANARY_WAP_JSON_WRITER_H
#define CANARY_WAP_JSON_WRITER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace wap_json {

/* The most bytes one byte of a string spends in JSON (`\u00XX`). */
static constexpr size_t kEscapeMax = 6;

/* What str() spends on one byte. */
inline size_t escaped_byte_len(unsigned char c) {
  if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') return 2;
  if (c < 0x20) return kEscapeMax;
  return 1;
}

/* The bytes str() spends on `s`, its two quotes included (NULL is ""). */
inline size_t string_len(const char* s) {
  size_t n = 2;
  for (const char* p = s ? s : ""; *p; ++p) n += escaped_byte_len((unsigned char)*p);
  return n;
}

struct Writer {
  char*  out;        // the text, always followed by a NUL (NULL: measuring)
  size_t cap;        // out's size
  size_t len;        // bytes written, the NUL excluded
  size_t reserve;    // bytes held back past `len` for a closing written last
  bool   overflow;   // a write did not fit: it and every write after it were refused
  bool   replace_controls;  // a control byte str() would write as \u00XX goes as \ufffd
};

/* Starts an empty text in out[cap]. A NULL `out` or a `cap` of 0 leaves
 * nothing written and refuses every write. */
inline void begin(Writer& w, char* out, size_t cap, size_t reserve = 0) {
  if (out == nullptr) cap = 0;
  w.out = out;
  w.cap = cap;
  w.len = 0;
  w.reserve = reserve;
  w.overflow = (cap == 0 || reserve >= cap);
  w.replace_controls = false;
  if (cap > 0) out[0] = '\0';
}

/* Counts what the writes would spend and writes nothing: len is then the
 * text's length, the NUL excluded. */
inline void begin_measure(Writer& w) {
  w.out = nullptr;
  w.cap = SIZE_MAX;
  w.len = 0;
  w.reserve = 0;
  w.overflow = false;
  w.replace_controls = false;   // the same length either way
}

/* True when `n` more bytes fit before the reserve and the NUL. */
inline bool room(const Writer& w, size_t n) {
  return !w.overflow && w.cap > w.reserve && n <= w.cap - 1 - w.reserve - w.len;
}

inline bool refuse(Writer& w) {
  w.overflow = true;
  return false;
}

/* Writes `n` bytes of JSON the caller spelled (keys, punctuation, a number's
 * digits), whole or not at all. */
inline bool raw(Writer& w, const char* text, size_t n) {
  if (!room(w, n)) return refuse(w);
  if (w.out) {
    memcpy(w.out + w.len, text, n);
    w.out[w.len + n] = '\0';
  }
  w.len += n;
  return true;
}

inline bool raw(Writer& w, const char* text) { return raw(w, text, strlen(text)); }

/* Writes the bytes of `s` escaped, without quotes (NULL as nothing), whole or
 * not at all: the inside of a JSON string the caller opens and closes. */
inline bool escaped(Writer& w, const char* s) {
  const size_t n = string_len(s) - 2;
  if (!room(w, n)) return refuse(w);
  if (!w.out) {
    w.len += n;
    return true;
  }
  static const char kHex[] = "0123456789abcdef";
  char* o = w.out + w.len;
  for (const char* p = s ? s : ""; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    switch (c) {
      case '"':  *o++ = '\\'; *o++ = '"';  break;
      case '\\': *o++ = '\\'; *o++ = '\\'; break;
      case '\b': *o++ = '\\'; *o++ = 'b';  break;
      case '\f': *o++ = '\\'; *o++ = 'f';  break;
      case '\n': *o++ = '\\'; *o++ = 'n';  break;
      case '\r': *o++ = '\\'; *o++ = 'r';  break;
      case '\t': *o++ = '\\'; *o++ = 't';  break;
      default:
        if (c < 0x20 && w.replace_controls) {
          *o++ = '\\'; *o++ = 'u'; *o++ = 'f'; *o++ = 'f'; *o++ = 'f'; *o++ = 'd';
        } else if (c < 0x20) {
          *o++ = '\\'; *o++ = 'u'; *o++ = '0'; *o++ = '0';
          *o++ = kHex[c >> 4]; *o++ = kHex[c & 0x0F];
        } else {
          *o++ = (char)c;
        }
    }
  }
  w.len = (size_t)(o - w.out);
  w.out[w.len] = '\0';
  return true;
}

/* Writes `s` as a quoted JSON string (NULL as ""), whole or not at all. */
inline bool str(Writer& w, const char* s) {
  if (!room(w, string_len(s))) return refuse(w);
  raw(w, "\"", 1);
  escaped(w, s);
  return raw(w, "\"", 1);
}

/* Writes `v` in decimal, whole or not at all (no printf: the same digits on
 * every libc). */
inline bool number(Writer& w, uint64_t v) {
  char tmp[20];
  size_t n = 0;
  do {
    tmp[n++] = (char)('0' + (int)(v % 10));
    v /= 10;
  } while (v != 0);
  char digits[20];
  for (size_t i = 0; i < n; ++i) digits[i] = tmp[n - 1 - i];
  return raw(w, digits, n);
}

inline bool boolean_value(Writer& w, bool v) { return v ? raw(w, "true", 4) : raw(w, "false", 5); }

inline bool ok(const Writer& w) { return !w.overflow; }

/* Where the text ends now, to roll a refused piece back to. */
inline size_t mark(const Writer& w) { return w.len; }

/* Drops everything written since `at` (a mark()) and the refusal with it. */
inline void rollback(Writer& w, size_t at) {
  if (w.cap == 0 || at > w.len) return;
  w.len = at;
  if (w.out) w.out[w.len] = '\0';
  w.overflow = (w.reserve >= w.cap);
}

}  // namespace wap_json

#endif  // CANARY_WAP_JSON_WRITER_H
