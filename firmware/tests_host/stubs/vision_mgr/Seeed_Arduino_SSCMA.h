/* Seeed_Arduino_SSCMA.h for test_vision_module_link.cpp: a scripted Grove
 * Vision AI module behind the same API the firmware calls on Seeed's
 * library (v1.0.3, the version firmware/envs/platformio/canary-vision.ini
 * pins). Only what vision_mgr.cpp needs, with the library's own types and
 * return conventions:
 *   - begin() returns a bool; the real one is `ID(false) && name(false)`
 *     (Seeed_Arduino_SSCMA.cpp:117), the library's own business, so it is
 *     scripted here and counted apart from the firmware's calls;
 *   - name()/ID() return a char* (nullptr when the module did not answer);
 *   - invoke() returns an int status code, CMD_OK == 0 (the inversion
 *     F384 names was a bool reading this int);
 *   - boxes() keeps the previous frame's boxes when an invoke fails, as the
 *     library does (its INVOKE event parser is what replaces them).
 * The script is one global the suite sets before each case. */
#ifndef STUB_SEEED_ARDUINO_SSCMA_H
#define STUB_SEEED_ARDUINO_SSCMA_H

#include <stdint.h>

#include <vector>

#define CMD_OK 0
#define CMD_AGAIN 1
#define CMD_ELOG 2
#define CMD_ETIMEDOUT 3
#define CMD_EIO 4

typedef struct {
  uint16_t x;
  uint16_t y;
  uint16_t w;
  uint16_t h;
  uint8_t score;
  uint8_t target;
} boxes_t;

struct SscmaScript {
  // What the module does.
  bool begin_ok = true;            // the library's own id and name queries at begin()
  bool name_answers = true;        // the firmware's own NAME? round trip
  int invoke_rc = CMD_OK;          // what the next invoke() returns
  std::vector<boxes_t> frame;      // the boxes a successful invoke reports
  // What the firmware did.
  int begin_calls = 0;
  int name_calls = 0;
  int name_cached_calls = 0;       // name(true): the cache, not the module
  int id_calls = 0;                // any ID(), cached or not
  int invoke_calls = 0;
};

inline SscmaScript& sscma_script() {
  static SscmaScript s;
  return s;
}

class SSCMA {
 public:
  bool begin() {
    ++sscma_script().begin_calls;
    return sscma_script().begin_ok;
  }

  char* name(bool cache = true) {
    SscmaScript& s = sscma_script();
    ++s.name_calls;
    if (cache) ++s.name_cached_calls;
    return s.name_answers ? name_ : nullptr;
  }

  char* ID(bool cache = true) {
    (void)cache;
    ++sscma_script().id_calls;
    return id_;
  }

  int invoke(int times = 1, bool filter = 0, bool show = 0) {
    (void)times;
    (void)filter;
    (void)show;
    SscmaScript& s = sscma_script();
    ++s.invoke_calls;
    if (s.invoke_rc == CMD_OK) boxes_ = s.frame;
    return s.invoke_rc;
  }

  std::vector<boxes_t>& boxes() { return boxes_; }

 private:
  char name_[32] = "Grove Vision AI V2";
  char id_[32] = "a1b2c3d4";
  std::vector<boxes_t> boxes_;
};

#endif
