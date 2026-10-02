// tests_host/lv_txt_wrap.h — LVGL 8.4's label wrap, for host tests.
//
// lv_txt.c's lv_txt_get_next_word, _lv_txt_get_next_line and
// lv_txt_get_size, at letter_space 0 and line_space 0, no recolor, with
// LV_TXT_LINE_BREAK_LONG_LEN 0 and LV_TXT_BREAK_CHARS " ,.;:-_" (the
// display's lv_conf leaves both at LVGL's defaults). The text is decoded
// from UTF-8 the way lv_txt_utf8_next does, and each letter's width is the
// caller's lv_font_get_glyph_width(font, letter, next).
//
// One port, two users: test_splash_layout.cpp measures the speech bubble's
// tallest form with it, and fake_lvgl/lvgl.h sizes a fixed-width
// LV_LABEL_LONG_WRAP label with it (test_splash_scenes.cpp reads the bubble
// splash.cpp built). test_splash_layout holds it to the bubble heights the
// real LVGL 8.4 drew in a native harness, so both users are held to LVGL.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace lvwrap {

inline std::vector<uint32_t> utf8(const char* t) {
  std::vector<uint32_t> cps;
  for (size_t i = 0; t[i] != '\0';) {
    const unsigned char c = (unsigned char)t[i];
    int more = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
    uint32_t cp = more == 0 ? c : more == 1 ? (c & 0x1F) : more == 2 ? (c & 0x0F) : (c & 0x07);
    ++i;
    for (; more > 0 && t[i] != '\0'; --more, ++i) cp = (cp << 6) | ((unsigned char)t[i] & 0x3F);
    cps.push_back(cp);
  }
  return cps;
}

inline bool is_break(uint32_t c) {
  const char* b = " ,.;:-_";
  for (int i = 0; b[i]; ++i)
    if (c == (uint32_t)b[i]) return true;
  return false;
}

// lv_txt_get_next_word: how many letters of t[0..n) the next word takes.
template <class GW>
uint32_t next_word(const uint32_t* t, size_t n, GW gw, int max_w, int* word_w,
                   bool force) {
  if (n == 0) return 0;
  const uint32_t kNone = 0xFFFFFFFFu;
  uint32_t i = 0, word_len = 0, break_index = kNone;
  int cur_w = 0;
  uint32_t letter = t[0];
  while (i < n) {
    const uint32_t letter_next = i + 1 < n ? t[i + 1] : 0;
    word_len++;
    cur_w += gw(letter, letter_next);
    if (break_index == kNone && cur_w > max_w) break_index = i;
    if (letter == '\n' || letter == '\r' || is_break(letter)) {
      if (i == 0 && break_index == kNone) *word_w = cur_w;
      word_len--;
      break;
    }
    if (break_index == kNone) *word_w = cur_w;
    i++;
    letter = letter_next;
  }
  if (break_index == kNone) {
    if (word_len == 0) i = i + 1;
    return i;
  }
  if (force) return break_index;
  *word_w = 0;
  return 0;
}

// _lv_txt_get_next_line: how many letters the next line takes.
template <class GW>
uint32_t next_line(const uint32_t* t, size_t n, GW gw, int max_w) {
  uint32_t i = 0;
  while (i < n && max_w > 0) {
    int word_w = 0;
    const uint32_t adv = next_word(t + i, n - i, gw, max_w, &word_w, i == 0);
    max_w -= word_w;
    if (adv == 0) break;
    i += adv;
    if (i < n && (t[i] == '\n' || t[i] == '\r')) {
      i++;
      break;
    }
  }
  if (i == 0) i = 1;
  return i;
}

// lv_txt_get_size's height, in lines: a line per _lv_txt_get_next_line, one
// more after a trailing newline, and one for the empty text.
template <class GW>
int line_count(const char* text, GW gw, int max_w) {
  const std::vector<uint32_t> t = utf8(text);
  int lines = 0;
  size_t at = 0;
  while (at < t.size()) {
    at += next_line(t.data() + at, t.size() - at, gw, max_w);
    lines++;
  }
  if (at != 0 && (t[at - 1] == '\n' || t[at - 1] == '\r')) lines++;
  return lines == 0 ? 1 : lines;
}

}  // namespace lvwrap
