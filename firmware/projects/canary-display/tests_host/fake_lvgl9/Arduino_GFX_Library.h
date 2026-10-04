// tests_host/fake_lvgl9/Arduino_GFX_Library.h — the one graphics call the
// display tree makes (lvgl_port.cpp's flush: draw16bitRGBBitmap, packed
// RGB565 rows), recorded onto a framebuffer of the panel's NATIVE size, the
// way the dash's Arduino_RGB_Display copies a flush into the RGB
// peripheral's scanned framebuffer. A blit reaching past the panel is
// counted (the real display clips it: those pixels are lost).
#pragma once
#include <stdint.h>

#include <vector>

class Arduino_GFX {
 public:
  struct Blit { int x, y, w, h; };
  Arduino_GFX(int16_t w, int16_t h) : W(w), H(h), fb((size_t)w * h, 0xFFFF) {}
  void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t* bitmap, int16_t w,
                          int16_t h) {
    blits.push_back({x, y, w, h});
    for (int r = 0; r < h; r++) {
      for (int c = 0; c < w; c++) {
        const int px = x + c, py = y + r;
        if (px < 0 || py < 0 || px >= W || py >= H) { off_panel++; continue; }
        fb[(size_t)py * W + px] = bitmap[(size_t)r * w + c];
      }
    }
  }
  void clear(uint16_t v) {
    for (auto& p : fb) p = v;
    blits.clear();
    off_panel = 0;
  }
  int W, H;
  std::vector<uint16_t> fb;
  std::vector<Blit> blits;
  long off_panel = 0;
};
