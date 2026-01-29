#pragma once
#include <stdint.h>

struct BBox {
  int x=0, y=0, w=0, h=0;
  int score=0; // 0-100
};

struct Voxel {
  uint8_t x = 0;
  uint8_t y = 0;
  uint8_t w = 0;
  uint8_t h = 0;

  constexpr Voxel() = default;
  constexpr Voxel(uint8_t x_, uint8_t y_, uint8_t w_, uint8_t h_)
    : x(x_), y(y_), w(w_), h(h_) {}
};


struct VisionSample {
  bool person_now=false;
  BBox bbox;
  Voxel voxel;
};

struct StateSnapshot {
  bool presence=false;
  bool dwelling=false;

  uint32_t presence_ms=0;
  uint32_t dwell_ms=0;

  int confidence=0; // percent
  Voxel voxel;
  BBox bbox;

  const char* last_event="boot";
  uint32_t uptime_s=0;
  uint32_t ts_ms=0;
};

struct EventMsg {
  const char* event_name=nullptr;
  const char* reason=nullptr;
};
