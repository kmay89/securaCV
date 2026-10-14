// What a browser reads from bytes it decodes as UTF-8, for host tests that
// follow an answer through a page and back (the F262 review: the wizard, the
// dashboard and the phone post back the SSID a list showed them). The WHATWG
// Encoding Standard's "UTF-8 decoder", which fetch().json() and TextDecoder
// run, transcribed from the standard (bytes needed and seen, the lower and
// upper boundary, a byte out of range read again), with its output written
// back out as UTF-8: each ill-formed sequence's maximal subpart becomes one
// U+FFFD (EF BF BD), and every well-formed character is its own bytes.
//
// Test-only, and independent of the firmware's own reading of it
// (wap_json::shows_as()): checked against Node 22's TextDecoder on 200 000
// seeded byte strings drawn mostly from the bytes UTF-8 turns on, every one
// equal.
#ifndef CANARY_WAP_TESTS_BROWSER_UTF8_H
#define CANARY_WAP_TESTS_BROWSER_UTF8_H

#include <cstdint>
#include <string>

inline std::string browser_utf8(const std::string& in) {
  std::string out;
  uint32_t cp = 0;
  int needed = 0, seen = 0;
  unsigned lower = 0x80, upper = 0xBF;
  auto put = [&](uint32_t c) {
    if (c < 0x80) {
      out.push_back((char)c);
    } else if (c < 0x800) {
      out.push_back((char)(0xC0 | (c >> 6)));
      out.push_back((char)(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
      out.push_back((char)(0xE0 | (c >> 12)));
      out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (c & 0x3F)));
    } else {
      out.push_back((char)(0xF0 | (c >> 18)));
      out.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
      out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (c & 0x3F)));
    }
  };
  for (size_t i = 0; i < in.size();) {
    const unsigned b = (unsigned char)in[i];
    if (needed == 0) {
      ++i;
      if (b <= 0x7F) { put(b); }
      else if (b >= 0xC2 && b <= 0xDF) { needed = 1; cp = b & 0x1F; }
      else if (b >= 0xE0 && b <= 0xEF) {
        if (b == 0xE0) lower = 0xA0;
        if (b == 0xED) upper = 0x9F;
        needed = 2; cp = b & 0xF;
      } else if (b >= 0xF0 && b <= 0xF4) {
        if (b == 0xF0) lower = 0x90;
        if (b == 0xF4) upper = 0x8F;
        needed = 3; cp = b & 0x7;
      } else { put(0xFFFD); }
      continue;
    }
    if (b < lower || b > upper) {        // the byte is read again (not consumed)
      cp = 0; needed = 0; seen = 0; lower = 0x80; upper = 0xBF;
      put(0xFFFD);
      continue;
    }
    ++i;
    lower = 0x80; upper = 0xBF;
    cp = (cp << 6) | (b & 0x3F);
    if (++seen == needed) {
      put(cp);
      cp = 0; needed = 0; seen = 0;
    }
  }
  if (needed != 0) put(0xFFFD);          // the stream ended inside a sequence
  return out;
}

#endif
