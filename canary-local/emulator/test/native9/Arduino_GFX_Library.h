// canary-local/emulator/test/native9/Arduino_GFX_Library.h — the one
// graphics call the display tree makes (lvgl_port.cpp's flush:
// draw16bitRGBBitmap, packed RGB565 rows), recorded onto a framebuffer of
// the panel's NATIVE size, the way the dash's Arduino_RGB_Display copies a
// flush into the RGB peripheral's scanned framebuffer (glass_turn_lvgl9.sh,
// F225). A blit reaching past the panel is counted, not drawn: the real
// display clips it and those pixels are lost. Which native pixels a refresh
// painted is kept beside the pixels, so "every pixel painted" does not
// depend on a sentinel color the scene might also draw.
#pragma once
#include <stdint.h>

#include <vector>

class Arduino_GFX {
 public:
  Arduino_GFX(int16_t w, int16_t h)
      : W(w), H(h), fb((size_t)w * h, 0), painted((size_t)w * h, 0) {}
  void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t* bitmap, int16_t w,
                          int16_t h) {
    blits++;
    for (int r = 0; r < h; r++) {
      for (int c = 0; c < w; c++) {
        const int px = x + c, py = y + r;
        if (px < 0 || py < 0 || px >= W || py >= H) {
          off_panel++;
          continue;
        }
        fb[(size_t)py * W + px] = bitmap[(size_t)r * w + c];
        painted[(size_t)py * W + px] = 1;
      }
    }
  }
  // Forget what was painted (the pixels stay, as on the glass).
  void clear_marks() {
    for (auto& p : painted) p = 0;
    off_panel = 0;
    blits = 0;
  }
  int W, H;
  std::vector<uint16_t> fb;
  std::vector<uint8_t> painted;
  long off_panel = 0;
  long blits = 0;
};
