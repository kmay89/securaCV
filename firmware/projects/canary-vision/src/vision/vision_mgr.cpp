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
static bool g_ready = false;
static int g_module_id = 0;

void init() {
  // Explicit pins from the board's pins.h: the Arduino variant defaults
  // differ per board (ESP32-C3 DevKit: 8/9, XIAO C3: 6/7, XIAO S3: 5/6)
  // and the DevKit default does NOT match our documented Grove wiring.
  Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL);
  const bool ok = AI.begin();
  delay(250);

  log_header("I2C");
  if (!ok) {
    canary::dbg_serial().println("ERROR: Grove Vision AI V2 not responding — check wiring/model (see docs/hardware/grove_vision_ai_v2_guide.md)");
  }
  g_module_id = (int)AI.ID();
  g_ready = ok && g_module_id > 0;
  canary::dbg_serial().printf("Grove Vision AI ID=%d\n", g_module_id);

  g_inited = true;
}

bool ready() { return g_inited && g_ready; }

int module_id() { return g_module_id; }

bool sample(VisionSample& out) {
  if (!ready()) return false;

  const bool invokeOk = AI.invoke(1, false, false);
  const bool hasBoxes = (AI.boxes().size() > 0);
  if (!(invokeOk || hasBoxes)) return false;

  // Runtime settings (NVS-backed, adjustable from HA) — the loaded SSCMA
  // model decides the class index and score calibration.
  const auto& det = canary::cfg::detect();

  auto& boxes = AI.boxes();
  out = detection::sample_from_boxes(boxes, det);
  return true;
}

} // namespace canary::vision
