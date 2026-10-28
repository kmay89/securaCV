#include "canary/vision/vision_mgr.h"
#include "canary/config.h"
#include "canary/detect_config.h"
#include "canary/vision/detection_pipeline.h"
#include "canary/log.h"

#include <Wire.h>
#include <Seeed_Arduino_SSCMA.h>

#include "pins.h"

namespace canary::vision {

static SSCMA AI;
static bool g_inited = false;
static bool g_module_ok = false;

// The module link (sweep F384). What the boot line and the `j` proof record
// say about the module is one verdict, and it is never the module's own id:
//
//   * module_id() was the library's id accessor, cast to int. SSCMA 1.0.3's
//     ID() returns its cached `_ID` buffer whenever `cache && _ID`, always
//     true for a char array, so that read asked the module nothing and the
//     value was the buffer's RAM address: non-zero whether or not the module
//     had answered, which is what the desktop Flasher's boot-line proof and
//     every "non-zero ID" doc read.
//   * The obvious fix, the same accessor with the cache off, would ask the
//     module for its own id, which the device-details design treats as a
//     per-unit identifier. Nothing here calls that accessor in any form. The
//     library's own I2C begin() is `ID(false) && name(false)`
//     (Seeed_Arduino_SSCMA.cpp:117), so the library itself asks the module
//     for its id once and keeps the answer in its private buffer; this file
//     never reads, prints or stores it.
//   * The verdict is begin() and then a fresh NAME? round trip (name(false),
//     never the cache). The name is not echoed either: only whether it came.
//     A begin() that failed skips the round trip, so a dead module does not
//     also cost NAME?'s 3 s timeout at boot.
void init() {
  // Explicit pins from the board's pins.h: the Arduino variant defaults
  // differ per board (ESP32-C3 DevKit: 8/9, XIAO C3: 6/7, XIAO S3: 5/6)
  // and the DevKit default does NOT match our documented Grove wiring.
  Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL);
  const bool began = AI.begin();
  delay(250);
  g_module_ok = began && AI.name(false) != nullptr;

  log_header("I2C");
  if (!g_module_ok) {
    canary::dbg_serial().println("ERROR: Grove Vision AI V2 not responding — check wiring/model (see docs/hardware/grove_vision_ai_v2_guide.md)");
  }
  // The format is a contract: gen_vision.py stages this line and the
  // desktop Flasher's monitor parses it. Only its value changed: 1 or 0.
  canary::dbg_serial().printf("Grove Vision AI ID=%d\n", g_module_ok ? 1 : 0);

  g_inited = true;
}

bool ready() { return g_inited && g_module_ok; }

int module_id() { return ready() ? 1 : 0; }

bool module_ok() { return ready(); }

bool sample(VisionSample& out) {
  if (!ready()) return false;

  // invoke() returns an SSCMA status code, CMD_OK (0) on success, not a
  // bool. Stored in a bool it read backwards: a successful invoke that found
  // no box returned no sample, so with a working module and an empty room the
  // presence FSM was never ticked and could not time a person out, and a
  // failed one returned a sample built from the boxes the library still held
  // from the last good frame. A failed invoke now yields no sample at all,
  // and a successful one always yields one, empty or not.
  if (AI.invoke(1, false, false) != CMD_OK) return false;

  // Runtime settings (NVS-backed, adjustable from HA) — the loaded SSCMA
  // model decides the class index and score calibration.
  const auto& det = canary::cfg::detect();

  auto& boxes = AI.boxes();
  out = detection::sample_from_boxes(boxes, det);
  return true;
}

} // namespace canary::vision
