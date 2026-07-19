// canary-local/emulator/shim/Arduino_GFX_Library.h — the one graphics type.
//
// The UI never touches panel specifics (canary/hal/display.h contract);
// the only Arduino_GFX calls in the whole display tree are lvgl_port.cpp's
// flush (fillScreen + draw16bitRGBBitmap). This shim receives those exact
// dirty-region blits and converts RGB565 → RGBA into the shared
// framebuffer JS textures from — so the emulator preserves the firmware's
// real partial-redraw behavior, visible in the page's flush counter.
#pragma once

#include <stdint.h>

class Arduino_GFX {
 public:
  Arduino_GFX(int16_t w, int16_t h) : width_(w), height_(h) {}
  virtual ~Arduino_GFX() {}

  int16_t width() const { return width_; }
  int16_t height() const { return height_; }

  void fillScreen(uint16_t color565);
  void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t* bitmap, int16_t w,
                          int16_t h);

 private:
  int16_t width_, height_;
};

// Color helpers some sketches use; harmless to provide.
#ifndef BLACK
#define BLACK 0x0000
#endif
