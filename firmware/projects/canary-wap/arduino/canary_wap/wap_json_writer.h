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
 *  - replaced_text() writes the same replacement into a plain string, for a
 *    value ArduinoJson writes itself or a printf's %s (sweep F262: the SSIDs
 *    the Wi-Fi answers, the self-test and the health log carry): each of
 *    those control bytes becomes U+FFFD's three UTF-8 bytes, which
 *    ArduinoJson writes as they are, and a text that does not fit is cut on
 *    a character boundary, never inside one. raw_control() is the one test
 *    of which bytes, for str(), the replace_controls writer and it.
 *  - shows_as() and listed_by_shown() read that text backwards, the way a
 *    page shows it: a page posts back the SSID a list showed it, U+FFFD and
 *    all, and the device joins the listed network that text names by the
 *    network's own bytes (the F262 review: a network whose name held a
 *    control byte was listed but could not be joined from the list).
 *  - Every write is whole or not at all: a write that does not fit before the
 *    reserve sets `overflow` and writes nothing, and every write after it is
 *    refused too, so the text is always a run of whole writes followed by a
 *    NUL, never a cut one.
 *  - A builder that must not answer a partial text measures it first (a
 *    writer begun with begin_measure() counts the bytes and writes none) and
 *    writes it into a buffer of that length (identity_json.h); one that keeps
 *    what fits rolls a refused piece back with rollback() (fleet_scan_cache.h
 *    keeps the shortest adverts that fit, each whole).
 *  - A write never passes the bytes it checked (sweep F230). escaped()
 *    measures its string and checks the room for that length, and the copy
 *    behind it is bounded by that same length: a string another task grows
 *    between the measure and the copy (the identity answers read live
 *    globals, and the loop task writes g_wifi_status.ap_auth) stops the copy
 *    at the bound and refuses the write, whole, instead of writing on past
 *    an exactly sized buffer. room() compares without a subtraction that can
 *    wrap, so a length past the cap could never read as room again.
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

/* True when `c` is a byte ArduinoJson 7.4.1 writes raw though JSON forbids it
 * raw: a control byte other than \b \f \n \r \t. str() writes it as \u00XX,
 * a replace_controls writer as \ufffd (sweep F211), and replaced_text() as
 * the replacement character itself (sweep F262): the one test all three
 * read. */
inline bool raw_control(unsigned char c) { return escaped_byte_len(c) == kEscapeMax; }

/* U+FFFD, the replacement character, as UTF-8: what replaced_text() writes
 * for a raw_control() byte. */
static constexpr char kReplacement[] = "\xEF\xBF\xBD";
static constexpr size_t kReplacementLen = sizeof(kReplacement) - 1;

/* The buffer replaced_text() needs to keep a string of `n` bytes whole, its
 * NUL included: n bytes that are all raw_control() spend 3 each (an SSID of
 * 32 bytes of 0x01 is 96 bytes of text). */
constexpr size_t replaced_cap(size_t n) { return n * kReplacementLen + 1; }

/* The bytes the character at `p` spans: a UTF-8 lead byte (0xC0 to 0xF7) and
 * the continuation bytes (0x80 to 0xBF) that follow it, up to its sequence's
 * length; any other byte, a stray continuation byte included, alone. */
inline size_t char_span(const unsigned char* p) {
  const unsigned char c = p[0];
  const size_t want = c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : c < 0xF8 ? 4 : 1;
  size_t n = 1;
  while (n < want && (p[n] & 0xC0) == 0x80) ++n;
  return n;
}

/* Copies `s` (NULL as "") into out[cap] as the text an ArduinoJson value or a
 * printf's %s takes, with each raw_control() byte written as U+FFFD and every
 * other byte as it is (sweep F262: a string the device did not choose, such
 * as an SSID an access point broadcasts or a person typed, written raw by
 * ArduinoJson, made the answer unparseable; ArduinoJson writes the
 * replacement as the valid UTF-8 it is and still escapes `"` and `\`
 * itself). The same replacement as a replace_controls writer's \ufffd, for
 * a text that is not JSON yet.
 *
 * `out` is always terminated (cap > 0) and never written past `cap`. The
 * text is whole, or cut before the first character that does not fit whole:
 * never part of a U+FFFD, never a UTF-8 lead byte without the continuation
 * bytes that follow it in `s`. Returns true when all of `s` fit; false when
 * it cut (or `out` is NULL or `cap` 0, which write nothing). */
inline bool replaced_text(char* out, size_t cap, const char* s) {
  if (out == nullptr || cap == 0) return false;
  size_t len = 0;
  for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p;) {
    const size_t span = char_span(p);
    const bool replace = raw_control(*p);   /* below 0x20: a span of one */
    const size_t need = replace ? kReplacementLen : span;
    if (need > cap - 1 - len) {
      out[len] = '\0';
      return false;
    }
    memcpy(out + len, replace ? (const unsigned char*)kReplacement : p, need);
    len += need;
    p += span;
  }
  out[len] = '\0';
  return true;
}

/* The next character of a string at `p` (not its NUL) as a page shows the
 * text replaced_text() wrote for it, and posts it back: a browser reads an
 * answer through fetch().json() or a TextDecoder, which decode UTF-8 the
 * WHATWG way, each ill-formed sequence's maximal subpart (a byte no
 * sequence starts with, or a lead byte and the continuation bytes it took
 * before one it could not) becoming one U+FFFD, and JSON.stringify() and
 * the request encode what it read as UTF-8 again. `*text` and the return
 * value are the bytes posted back for it: U+FFFD's three for a
 * raw_control() byte (replaced_text() wrote U+FFFD) or an ill-formed
 * subpart (the browser read U+FFFD), the character's own bytes otherwise.
 * `*span` is the bytes of the string it takes. */
inline size_t shown_piece(const unsigned char* p, const unsigned char** text, size_t* span) {
  const unsigned char c = p[0];
  size_t need = 0;                          /* continuation bytes the lead byte wants */
  unsigned char lo = 0x80, hi = 0xBF;       /* the first one's range (WHATWG's bounds) */
  if (c >= 0xC2 && c <= 0xDF) {
    need = 1;
  } else if (c >= 0xE0 && c <= 0xEF) {
    need = 2;
    if (c == 0xE0) lo = 0xA0;               /* no overlong form */
    if (c == 0xED) hi = 0x9F;               /* no surrogate */
  } else if (c >= 0xF0 && c <= 0xF4) {
    need = 3;
    if (c == 0xF0) lo = 0x90;               /* no overlong form */
    if (c == 0xF4) hi = 0x8F;               /* nothing past U+10FFFF */
  }
  bool whole = c < 0x80 ? !raw_control(c) : need > 0;
  size_t n = 1;
  while (whole && n <= need) {
    if (p[n] < lo || p[n] > hi) {           /* the string's NUL ends it too */
      whole = false;
      break;
    }
    lo = 0x80;
    hi = 0xBF;
    ++n;
  }
  *span = n;
  *text = whole ? p : (const unsigned char*)kReplacement;
  return whole ? n : kReplacementLen;
}

/* True when a page that read replaced_text()'s whole text of `raw` posts
 * back exactly `shown` (NULL is ""): the inverse a post of a listed name is
 * read through (the F262 review: GET /api/wifi/scan, GET /api/wifi and the
 * BLE provisioning list show a network's control bytes as U+FFFD, and a
 * browser shows any byte that is not UTF-8 the same way; the wizard, the
 * dashboard and the phone post back the text they were shown, which no
 * access point broadcasts, and the join needs the network's own bytes).
 * Reads at most strlen(shown) + 1 bytes of `shown`. */
inline bool shows_as(const char* raw, const char* shown) {
  const unsigned char* s = (const unsigned char*)(shown ? shown : "");
  for (const unsigned char* p = (const unsigned char*)(raw ? raw : ""); *p;) {
    const unsigned char* text;
    size_t span;
    const size_t len = shown_piece(p, &text, &span);
    for (size_t k = 0; k < len; ++k) {
      if (s[k] != text[k]) return false;   /* a NUL in `shown` differs here */
    }
    s += len;
    p += span;
  }
  return *s == '\0';
}

/* Which of `count` listed strings a posted text names: the first whose bytes
 * are the posted text's own (a name that holds U+FFFD itself is its own
 * network), else the first a page shows as the posted text (shows_as(): two
 * names that differ only in bytes a page shows as U+FFFD show as one, and
 * the first listed, the strongest in a scan's order, is the one meant),
 * else -1. `at(i)` gives the i-th listed string (NULL is skipped). A posted
 * text no listed string shows as names nothing, and the caller keeps it as
 * posted (a name typed by hand, a hidden network). */
template <typename At>
inline int listed_by_shown(const char* posted, int count, At at) {
  if (posted == nullptr) return -1;
  for (int i = 0; i < count; ++i) {
    const char* raw = at(i);
    if (raw != nullptr && strcmp(raw, posted) == 0) return i;
  }
  for (int i = 0; i < count; ++i) {
    const char* raw = at(i);
    if (raw != nullptr && shows_as(raw, posted)) return i;
  }
  return -1;
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

/* True when `n` more bytes fit before the reserve and the NUL. Every
 * subtraction here is of a smaller number from a larger one: `avail` is at
 * least 0 once cap > reserve, and `len` is tested against it before it is
 * taken from it, so a length past the cap is no room, never a wrapped huge
 * one (sweep F230: `cap - 1 - reserve - len` wrapped once len passed it). */
inline bool room(const Writer& w, size_t n) {
  if (w.overflow || w.cap <= w.reserve) return false;
  const size_t avail = w.cap - 1 - w.reserve;
  return w.len <= avail && n <= avail - w.len;
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
 * not at all: the inside of a JSON string the caller opens and closes.
 *
 * The copy is bounded by the length measured and checked (n), not by the
 * string's NUL: a string that grew after it was measured (another task
 * writing it) would otherwise be copied past the room checked for it. Each
 * byte's escape is written only when it fits in what is left of n; one that
 * does not stops the copy, the text is put back as it was, and the write is
 * refused (sweep F230). The copy also reads at most n bytes of `s`, since
 * every byte spends at least one. */
inline bool escaped(Writer& w, const char* s) {
  const size_t n = string_len(s) - 2;
  if (!room(w, n)) return refuse(w);
  if (!w.out) {
    w.len += n;
    return true;
  }
#ifdef WAP_JSON_TEST_HOOK_AFTER_MEASURE
  WAP_JSON_TEST_HOOK_AFTER_MEASURE(s);   /* host tests only: grow `s` here */
#endif
  static const char kHex[] = "0123456789abcdef";
  char* const start = w.out + w.len;
  char* o = start;
  for (const char* p = s ? s : ""; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (escaped_byte_len(c) > n - (size_t)(o - start)) {
      *start = '\0';                     /* grew since measured: none of it */
      return refuse(w);
    }
    switch (c) {
      case '"':  *o++ = '\\'; *o++ = '"';  break;
      case '\\': *o++ = '\\'; *o++ = '\\'; break;
      case '\b': *o++ = '\\'; *o++ = 'b';  break;
      case '\f': *o++ = '\\'; *o++ = 'f';  break;
      case '\n': *o++ = '\\'; *o++ = 'n';  break;
      case '\r': *o++ = '\\'; *o++ = 'r';  break;
      case '\t': *o++ = '\\'; *o++ = 't';  break;
      default:
        if (raw_control(c) && w.replace_controls) {
          *o++ = '\\'; *o++ = 'u'; *o++ = 'f'; *o++ = 'f'; *o++ = 'f'; *o++ = 'd';
        } else if (raw_control(c)) {
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

/* Writes `s` as a quoted JSON string (NULL as ""), whole or not at all: a
 * string that grew between this measure and escaped()'s own leaves neither
 * quote behind it. */
inline bool str(Writer& w, const char* s) {
  if (!room(w, string_len(s))) return refuse(w);
#ifdef WAP_JSON_TEST_HOOK_AFTER_MEASURE
  if (w.out) WAP_JSON_TEST_HOOK_AFTER_MEASURE(s);   /* host tests only */
#endif
  const size_t at = w.len;
  if (raw(w, "\"", 1) && escaped(w, s) && raw(w, "\"", 1)) return true;
  w.len = at;
  if (w.out) w.out[at] = '\0';
  return refuse(w);
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
