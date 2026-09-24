// Source-level pins for canary-wap's beacon_channel.cpp (host tests only).
//
// beacon_channel.cpp needs Arduino, ESP-NOW and the rweather crypto library,
// so no host test links it. Its decisions are host-tested through the
// Arduino-free headers (beacon_cancel_policy.h, beacon_cosign_aad.h), but a
// test of a helper cannot see whether the firmware calls that helper where it
// must: rewrite a call site and every helper test stays green. These
// functions read the real .cpp, strip its comments, and hand back one
// function's body, so a test can assert what that body calls and in what
// order. The pins are exact on purpose — a call site that changes shape has to
// change its pin in the same commit, where a reviewer sees both.
//
// Used by test_beacon_origination.cpp and test_beacon_cancel_origination.cpp;
// the Makefile passes the .cpp's absolute path as BEACON_CHANNEL_CPP, and every
// pin fails closed when the file cannot be read.

#ifndef SECURACV_TESTS_HOST_BEACON_SOURCE_SCAN_H
#define SECURACV_TESTS_HOST_BEACON_SOURCE_SCAN_H

#include <cctype>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>

namespace beacon_source_scan {

// The whole file, or "" (and *ok = false) when it cannot be read.
inline std::string read_source(const char* path, bool* ok) {
  std::ifstream f(path);
  *ok = f.good();
  if (!*ok) return std::string();
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Strip // and /* */ comments (string and char literals kept intact), so a pin
// reads code, not prose about the code. Newlines inside comments are kept, so
// line-oriented scans still see one line per source line.
inline std::string strip_comments(const std::string& src) {
  std::string out;
  enum { CODE, LINE, BLOCK, STR, CHR } st = CODE;
  for (size_t i = 0; i < src.size(); i++) {
    const char c = src[i];
    const char n = (i + 1 < src.size()) ? src[i + 1] : '\0';
    switch (st) {
      case CODE:
        if (c == '/' && n == '/') { st = LINE; i++; }
        else if (c == '/' && n == '*') { st = BLOCK; i++; }
        else { if (c == '"') st = STR; else if (c == '\'') st = CHR; out += c; }
        break;
      case LINE:
        if (c == '\n') { st = CODE; out += c; }
        break;
      case BLOCK:
        if (c == '*' && n == '/') { st = CODE; i++; }
        else if (c == '\n') out += c;
        break;
      case STR:
      case CHR:
        out += c;
        if (c == '\\' && n) { out += n; i++; }
        else if ((st == STR && c == '"') || (st == CHR && c == '\'')) st = CODE;
        break;
    }
  }
  return out;
}

// Every whitespace character removed, so a pin does not depend on formatting:
// "hdr->msg_type = x(c);" and "hdr->msg_type=x(c);" squeeze to the same text.
// Pins are written squeezed too.
inline std::string squeeze(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (!std::isspace(static_cast<unsigned char>(c))) out += c;
  }
  return out;
}

// Index just past the bracket that closes the one at `open` (which must hold
// `open_ch`), skipping string and char literals; npos when unbalanced.
inline size_t match_close(const std::string& code, size_t open, char open_ch, char close_ch) {
  int depth = 0;
  for (size_t i = open; i < code.size(); i++) {
    const char c = code[i];
    if (c == '"' || c == '\'') {
      const char q = c;
      for (i++; i < code.size() && code[i] != q; i++) {
        if (code[i] == '\\') i++;
      }
      continue;
    }
    if (c == open_ch) depth++;
    else if (c == close_ch && --depth == 0) return i + 1;
  }
  return std::string::npos;
}

inline bool ident_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The body ("{ ... }") of the first DEFINITION of the free function `name` in
// comment-stripped `code`, or "" when there is none. A definition is `name(`
// not preceded by an identifier character, '.', '>' or ':' (so neither a
// longer name, a member call nor a qualified call matches), whose parameter
// list is followed by '{'. A forward declaration (';') or a call (anything
// else after the ')') is skipped.
inline std::string function_body(const std::string& code, const std::string& name) {
  const std::string needle = name + "(";
  for (size_t at = code.find(needle); at != std::string::npos;
       at = code.find(needle, at + 1)) {
    if (at > 0) {
      const char p = code[at - 1];
      if (ident_char(p) || p == '.' || p == '>' || p == ':') continue;
    }
    const size_t params_end = match_close(code, at + name.size(), '(', ')');
    if (params_end == std::string::npos) return std::string();
    size_t j = params_end;
    while (j < code.size() && std::isspace(static_cast<unsigned char>(code[j]))) j++;
    if (j >= code.size() || code[j] != '{') continue;
    const size_t body_end = match_close(code, j, '{', '}');
    if (body_end == std::string::npos) return std::string();
    return code.substr(j, body_end - j);
  }
  return std::string();
}

inline size_t count(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos;
       at = hay.find(needle, at + needle.size())) {
    n++;
  }
  return n;
}

// True when both occur in `hay` and the first occurrence of `first` ends
// before the first occurrence of `second` begins — so an extra `second`
// inserted ahead of `first` fails, not only a missing one.
inline bool before(const std::string& hay, const std::string& first,
                   const std::string& second) {
  const size_t a = hay.find(first);
  const size_t b = hay.find(second);
  if (a == std::string::npos || b == std::string::npos) return false;
  return a + first.size() <= b;
}

// The block ("{ ... }") that opens right after the first occurrence of
// `head` (which must end just before that '{'), or "" when there is none.
inline std::string block_after(const std::string& hay, const std::string& head) {
  const size_t at = hay.find(head);
  if (at == std::string::npos) return std::string();
  const size_t open = at + head.size();
  if (open >= hay.size() || hay[open] != '{') return std::string();
  const size_t end = match_close(hay, open, '{', '}');
  if (end == std::string::npos) return std::string();
  return hay.substr(open, end - open);
}

}  // namespace beacon_source_scan

#endif  // SECURACV_TESTS_HOST_BEACON_SOURCE_SCAN_H
