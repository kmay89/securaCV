/* Wire.h for test_vision_module_link.cpp: records the pins vision_mgr.cpp
 * opens the bus on (the board's pins.h, never the Arduino variant's). */
#ifndef STUB_VISION_MGR_WIRE_H
#define STUB_VISION_MGR_WIRE_H

class TwoWire {
 public:
  int sda = -1;
  int scl = -1;
  int begins = 0;
  void begin(int sda_pin, int scl_pin) {
    sda = sda_pin;
    scl = scl_pin;
    ++begins;
  }
};

inline TwoWire Wire;

#endif
